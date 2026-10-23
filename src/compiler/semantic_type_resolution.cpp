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
      candidate.record().intrinsic != IntrinsicKind::None ||
      candidate.record().requiresUnsafe) {
    return false;
  }
  const FunctionDecl &declaration = *candidate.declaration;
  return declaration.body() != nullptr && !declaration.runtimeBinding() &&
         !declaration.hasCLinkage() && !declaration.operatorName();
}

auto SemanticVisitor::Impl::functionItemCandidateIsEligible(
    const CallableCandidate &candidate) -> bool {
  if (candidate.ownerClass != 0 || candidate.declaration == nullptr ||
      candidate.id == 0 || !candidate.record().genericParameters.empty() ||
      candidate.record().parameterPack || !candidate.requirements.empty() ||
      candidate.virtualMethod || candidate.pureVirtual ||
      candidate.overrideMethod || candidate.compilerPrivate ||
      candidate.record().intrinsic != IntrinsicKind::None ||
      candidate.record().requiresUnsafe) {
    return false;
  }
  const FunctionDecl &declaration = *candidate.declaration;
  return declaration.body() != nullptr && !declaration.runtimeBinding() &&
         !declaration.hasCLinkage() && !declaration.operatorName() &&
         !candidate.record().entryPoint;
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
  const bool concurrent =
      target.executionProfile == ExecutionProfile::Concurrent;
  const std::vector<const CallableCandidate *> matches =
      matchingNativeFunctionCandidates(symbol, expectedType);
  const bool confined =
      matches.size() == 1 && matches.front()->record().threadConfinedCallback;
  if (concurrent && !confined &&
      expectedType.cThreadCallbackKind() == CThreadCallbackKind::None) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2076", DiagnosticPhase::Semantics, location,
        "Concurrent GTI callbacks require a registered C-thread entry "
        "'(void*) -> int32_t', once initializer '() -> void', or an explicit "
        "[[thread_confined_callback]] target.");
    diagnostic.hints.emplace_back(
        "Use std::thrd_create for a worker or std::call_once for an "
        "initializer; "
        "other signatures require a thread-confined callback contract.");
    diagnostics.emplace_back(std::move(diagnostic));
    return SemanticType::Unknown;
  }
  if (concurrent &&
      !validateConcurrentSurfaceAvailability(location, "This C-thread entry")) {
    return SemanticType::Unknown;
  }
  if (concurrent &&
      (sourceGraph == nullptr ||
       std::none_of(sourceGraph->sourceUnits().begin(),
                    sourceGraph->sourceUnits().end(),
                    [](const SourceUnit &unit) {
                      return unit.role == SourceUnitRole::StandardLibrary &&
                             unit.standardLibraryName == "std/cthread";
                    }))) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2076", DiagnosticPhase::Semantics, location,
        "A concurrent GTI callback requires the C-thread runtime boundary.");
    diagnostic.hints.emplace_back(
        "Include <std/cthread> and invoke the callback only through "
        "std::thrd_create, std::call_once, or its explicit thread-confined "
        "boundary; general foreign-thread attachment "
        "is unavailable.");
    diagnostics.emplace_back(std::move(diagnostic));
    return SemanticType::Unknown;
  }

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
        "match. An unsafe function cannot become a safe callback.");
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
  if (selected.record().tracksCaller) {
    Diagnostic diagnostic = makeDiagnostic(
        DiagnosticId::InvalidFunctionContract, location,
        {"A [[track_caller]] function cannot be converted to a native "
         "callback; "
         "the callback signature cannot carry its caller location."});
    diagnostic.related.push_back({tokenSpan(selected.declaration->name()),
                                  "Caller-tracked function declared here."});
    diagnostic.hints.emplace_back(
        "Use an ordinary GTI wrapper that directly calls the tracked function; "
        "the wrapper call site becomes the reported location.");
    diagnostics.emplace_back(std::move(diagnostic));
    return SemanticType::Unknown;
  }
  semanticModel.recordNativeFunctionConversion(
      expression,
      NativeFunctionConversionInfo{
          .function = selected.id,
          .declaration = selected.declaration,
          .type = expectedType,
          .cThreadCallback =
              concurrent ? (confined ? CThreadCallbackKind::ThreadConfined
                                     : expectedType.cThreadCallbackKind())
                         : CThreadCallbackKind::None});
  requestSession.ownerlessTextCallableUses.push_back(
      {.target = selected.id, .location = location});
  return expectedType;
}

auto SemanticVisitor::Impl::analyzeFunctionItem(
    const Expr &expression, const Symbol &symbol,
    const SemanticType &expectedType, const Token &location) -> SemanticType {
  const CallableCandidate *selected = nullptr;
  for (const CallableCandidate &candidate : symbol.overloads) {
    const bool exactContext =
        candidate.id == expectedType.functionId &&
        expectedType.hasFunctionItemShape() &&
        expectedType.functionItemReturnType() != nullptr &&
        candidate.returnType == *expectedType.functionItemReturnType() &&
        candidate.parameterTypes.size() ==
            expectedType.functionItemParameterTypes().size() &&
        std::equal(candidate.parameterTypes.begin(),
                   candidate.parameterTypes.end(),
                   expectedType.functionItemParameterTypes().begin());
    if (functionItemCandidateIsEligible(candidate) &&
        (expectedType == SemanticType::Unknown || exactContext)) {
      if (selected != nullptr) {
        Diagnostic diagnostic =
            makeDiagnostic("GTI-S2090", DiagnosticPhase::Semantics, location,
                           "The function item is ambiguous without an exact "
                           "callable context.");
        diagnostic.hints.emplace_back(
            "Use a uniquely named namespace function or an explicitly typed "
            "lambda that calls the intended overload.");
        for (const CallableCandidate &overload : symbol.overloads) {
          if (functionItemCandidateIsEligible(overload))
            diagnostic.related.push_back(
                {tokenSpan(overload.declaration->name()),
                 "Candidate: " + functionSignatureSpelling(overload)});
        }
        diagnostics.emplace_back(std::move(diagnostic));
        return SemanticType::Unknown;
      }
      selected = &candidate;
    }
  }
  if (selected == nullptr) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2090", DiagnosticPhase::Semantics, location,
        expectedType == SemanticType::Unknown
            ? "This name does not identify an eligible namespace-scope GTI "
              "function item."
            : "The selected function-item context no longer names one exact "
              "eligible namespace-scope GTI function.");
    diagnostic.hints.emplace_back(
        "Function items require a non-generic namespace-scope GTI function "
        "with a body and an exact callable signature, without an unsafe "
        "caller contract.");
    for (const CallableCandidate &candidate : symbol.overloads) {
      if (candidate.declaration != nullptr)
        diagnostic.related.push_back(
            {tokenSpan(candidate.declaration->name()),
             "Candidate: " + functionSignatureSpelling(candidate)});
    }
    diagnostics.emplace_back(std::move(diagnostic));
    return SemanticType::Unknown;
  }
  const SemanticType itemType = SemanticType::functionItemType(
      selected->id, selected->returnType, selected->parameterTypes);
  semanticModel.recordFunctionItemSelection(
      expression, FunctionItemSelectionInfo{
                      .function = selected->id,
                      .declaration = selected->declaration,
                      .type = itemType,
                      .capability = CallableInvocationCapability::Read});
  requestSession.ownerlessTextCallableUses.push_back(
      {.target = selected->id, .location = location});
  return itemType;
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

auto SemanticVisitor::Impl::contextualizeFunctionItemArguments(
    const CallableCandidate &target,
    const std::vector<SemanticType> &explicitTypeArguments,
    const std::vector<CompileTimeValue> &explicitValueArguments,
    const ExprList &arguments, ContextualCallArguments &analysis, bool diagnose)
    -> bool {
  if (arguments.size() != analysis.types.size() ||
      arguments.size() != analysis.deferred.size()) {
    return false;
  }
  if (std::none_of(arguments.begin(), arguments.end(),
                   [&](const ExprPtr &argument) {
                     return isContextualNativeFunctionArgument(argument);
                   })) {
    return true;
  }

  GenericSubstitution baseSubstitution;
  std::size_t explicitType = 0;
  std::size_t explicitValue = 0;
  for (const GenericParameterInfo &parameter :
       target.record().genericParameters) {
    if (parameter.value) {
      if (explicitValue < explicitValueArguments.size()) {
        baseSubstitution.values.emplace(
            parameter.id, explicitValueArguments[explicitValue++]);
      }
    } else if (parameter.pack && explicitType < explicitTypeArguments.size()) {
      std::vector<SemanticType> elements(
          explicitTypeArguments.begin() +
              static_cast<std::ptrdiff_t>(explicitType),
          explicitTypeArguments.end());
      baseSubstitution.types.emplace(
          parameter.id,
          SemanticType::concreteTypePack(parameter.id, std::move(elements)));
      explicitType = explicitTypeArguments.size();
    } else if (explicitType < explicitTypeArguments.size()) {
      baseSubstitution.types.emplace(parameter.id,
                                     explicitTypeArguments[explicitType++]);
    }
  }
  if (target.ownerClass != 0 &&
      target.dispatchOwner.kind == SemanticType::Class) {
    const GenericSubstitution owner = classSubstitution(target.dispatchOwner);
    baseSubstitution.types.insert(owner.types.begin(), owner.types.end());
    baseSubstitution.values.insert(owner.values.begin(), owner.values.end());
  }

  const auto isTargetTypeParameter = [&](GenericParameterId id) {
    return std::any_of(target.record().genericParameters.begin(),
                       target.record().genericParameters.end(),
                       [&](const GenericParameterInfo &parameter) {
                         return !parameter.value && parameter.id == id;
                       });
  };
  const auto isTargetValueParameter = [&](GenericParameterId id) {
    return std::any_of(target.record().genericParameters.begin(),
                       target.record().genericParameters.end(),
                       [&](const GenericParameterInfo &parameter) {
                         return parameter.value && parameter.id == id;
                       });
  };

  const auto inferOuter = [&](const auto &self, const SemanticType &pattern,
                              const SemanticType &argument,
                              GenericSubstitution &substitution) -> bool {
    if (argument == SemanticType::Unknown) {
      return true;
    }
    if (pattern.kind == SemanticType::Reference &&
        pattern.arguments.size() == 1) {
      return self(self, pattern.arguments.front(), argument, substitution);
    }
    if (pattern.kind == SemanticType::TypeParameter &&
        isTargetTypeParameter(pattern.genericParameterId)) {
      const auto found = substitution.types.find(pattern.genericParameterId);
      if (found == substitution.types.end()) {
        substitution.types.emplace(pattern.genericParameterId, argument);
        return true;
      }
      return found->second == argument;
    }
    if (pattern.kind == SemanticType::Array &&
        pattern.arrayLengthParameterId != 0 &&
        isTargetValueParameter(pattern.arrayLengthParameterId) &&
        argument.kind == SemanticType::Array &&
        argument.arrayLengthParameterId == 0) {
      const CompileTimeValue length =
          CompileTimeValue::uint64(argument.arrayLength);
      const auto found =
          substitution.values.find(pattern.arrayLengthParameterId);
      if (found == substitution.values.end()) {
        substitution.values.emplace(pattern.arrayLengthParameterId, length);
      } else if (found->second != length) {
        return false;
      }
    }
    if (pattern.kind != argument.kind ||
        pattern.nativeCScalar != argument.nativeCScalar ||
        pattern.classId != argument.classId ||
        pattern.enumId != argument.enumId ||
        pattern.arguments.size() != argument.arguments.size()) {
      return true;
    }
    for (std::size_t index = 0; index < pattern.arguments.size(); ++index) {
      if (!self(self, pattern.arguments[index], argument.arguments[index],
                substitution)) {
        return false;
      }
    }
    return true;
  };

  for (std::size_t index = 0;
       index < analysis.types.size() && index < fixedParameterCount(target);
       ++index) {
    if (isContextualNativeFunctionArgument(arguments[index])) {
      continue;
    }
    if (!inferOuter(inferOuter, target.parameterTypes[index],
                    analysis.types[index], baseSubstitution)) {
      return false;
    }
  }
  if (target.parameterPack) {
    const GenericParameterInfo *pack = packGenericParameter(target);
    const std::size_t fixed = fixedParameterCount(target);
    if (pack != nullptr && !baseSubstitution.types.contains(pack->id)) {
      if (analysis.types.size() < fixed) {
        return false;
      }
      std::vector<SemanticType> elements;
      for (std::size_t index = fixed; index < analysis.types.size(); ++index) {
        SemanticType::appendExpandedArgument(elements, analysis.types[index]);
      }
      baseSubstitution.types.emplace(
          pack->id,
          elements.size() == 1 &&
                  elements.front().kind == SemanticType::TypePack &&
                  !elements.front().concretePack
              ? elements.front()
              : SemanticType::concreteTypePack(pack->id, std::move(elements)));
    }
  }

  const auto exactMatch = [&](const auto &self, const SemanticType &pattern,
                              const SemanticType &actual,
                              GenericSubstitution &substitution) -> bool {
    if (pattern.kind == SemanticType::TypeParameter &&
        isTargetTypeParameter(pattern.genericParameterId)) {
      const auto found = substitution.types.find(pattern.genericParameterId);
      if (found == substitution.types.end()) {
        substitution.types.emplace(pattern.genericParameterId, actual);
        return true;
      }
      return found->second == actual;
    }
    const bool targetArrayExtent =
        pattern.kind == SemanticType::Array &&
        pattern.arrayLengthParameterId != 0 &&
        isTargetValueParameter(pattern.arrayLengthParameterId);
    if (pattern.kind != actual.kind ||
        pattern.nativeCScalar != actual.nativeCScalar ||
        pattern.classId != actual.classId || pattern.enumId != actual.enumId ||
        pattern.genericParameterId != actual.genericParameterId ||
        pattern.functionId != actual.functionId ||
        pattern.lambdaId != actual.lambdaId ||
        pattern.lambdaParameterCount != actual.lambdaParameterCount ||
        pattern.lambdaCaptureCount != actual.lambdaCaptureCount ||
        pattern.lambdaEnclosingClassTypes != actual.lambdaEnclosingClassTypes ||
        pattern.lambdaEnclosingFunctionTypes !=
            actual.lambdaEnclosingFunctionTypes ||
        pattern.lambdaEnclosingClassValues !=
            actual.lambdaEnclosingClassValues ||
        pattern.lambdaEnclosingFunctionValues !=
            actual.lambdaEnclosingFunctionValues ||
        pattern.referenceAccess != actual.referenceAccess ||
        pattern.pointerAccess != actual.pointerAccess ||
        pattern.projectionConcept != actual.projectionConcept ||
        pattern.projectionMember != actual.projectionMember ||
        (!targetArrayExtent && pattern.arrayLength != actual.arrayLength) ||
        pattern.valueArguments != actual.valueArguments ||
        pattern.concretePack != actual.concretePack ||
        pattern.arguments.size() != actual.arguments.size()) {
      return false;
    }
    if (targetArrayExtent) {
      if (actual.arrayLengthParameterId != 0) {
        return false;
      }
      const CompileTimeValue length =
          CompileTimeValue::uint64(actual.arrayLength);
      const auto found =
          substitution.values.find(pattern.arrayLengthParameterId);
      if (found == substitution.values.end()) {
        substitution.values.emplace(pattern.arrayLengthParameterId, length);
      } else if (found->second != length) {
        return false;
      }
    } else if (pattern.arrayLengthParameterId !=
               actual.arrayLengthParameterId) {
      return false;
    }
    for (std::size_t index = 0; index < pattern.arguments.size(); ++index) {
      if (!self(self, pattern.arguments[index], actual.arguments[index],
                substitution)) {
        return false;
      }
    }
    return true;
  };

  // Disjoint definitions of one class name form a family; concrete
  // arguments select one definition, whose members are its own.
  const auto selectsOtherClassDefinition = [&](const SemanticType &owner,
                                               ClassId declared) {
    if (owner.kind != SemanticType::Class || owner.classId == 0 ||
        owner.classId > classes.size() || declared == 0 ||
        declared > classes.size() || owner.classId == declared)
      return false;
    const auto family = [&](ClassId id) {
      const ClassId primary = classInfo(id).definitionFamilyPrimary;
      return primary == 0 ? id : primary;
    };
    return family(owner.classId) == family(declared);
  };

  // A forwarding wrapper has the same invocation constraints as its checked
  // callee, expressed in the wrapper's substitution. Preserve the selected
  // call identities: looking up a helper by spelling would make overload
  // selection depend on a different declaration from the one body analysis
  // actually selected. This is contextual selection only; concrete callable
  // instances still undergo their ordinary boundary and body checks.
  const auto contextualRequirements = [&](FunctionId root,
                                          std::size_t parameter,
                                          const GenericSubstitution
                                              &rootSubstitution) {
    std::vector<CallableSignatureRequirement> signatures;
    std::vector<std::pair<FunctionId, std::size_t>> visiting;
    const auto collect = [&](const auto &self, FunctionId function,
                             std::size_t parameterIndex,
                             const GenericSubstitution &substitution) -> bool {
      const std::pair key{function, parameterIndex};
      if (std::find(visiting.begin(), visiting.end(), key) != visiting.end())
        return false;
      const FunctionInfo *info = semanticModel.findFunction(function);
      if (info == nullptr && instanceBaseModel != nullptr)
        info = instanceBaseModel->findFunction(function);
      if (info == nullptr)
        return false;
      const CallableParameterContract *current =
          callableParameterContract(function, parameterIndex);
      if (current != nullptr && current->boundary != CallableBoundary::Confined)
        return false;
      visiting.push_back(key);
      std::vector<CallableForwardingRequirement> edges;
      if (current != nullptr) {
        for (auto signature : current->signatures) {
          signature.returnType =
              substituteType(signature.returnType, substitution);
          for (SemanticType &type : signature.parameterTypes)
            type = substituteType(type, substitution);
          signatures.push_back(std::move(signature));
        }
        edges = current->forwardings;
      }
      // Primary body analysis can select a caller before the final
      // forwarding fixed point. Use those same exact pending edges, not
      // an inferred body shape or a library-name exception.
      const auto appendPending = [&](const SemanticModel &model) {
        for (const auto &pending : model.pendingCallableForwardings) {
          if ((pending.source != info->declaration &&
               pending.source != info->bodyDeclaration()) ||
              pending.sourceParameterIndex != parameterIndex)
            continue;
          if (std::none_of(edges.begin(), edges.end(), [&](const auto &edge) {
                return edge.source == pending.call &&
                       edge.function == pending.target &&
                       edge.parameterIndex == pending.targetParameterIndex;
              }))
            edges.push_back(
                {pending.call, pending.target, pending.targetParameterIndex});
        }
      };
      appendPending(semanticModel);
      if (instanceBaseModel != nullptr)
        appendPending(*instanceBaseModel);
      for (const auto &edge : edges) {
        const ResolvedCallInfo *call =
            edge.source == nullptr ? nullptr
                                   : semanticModel.findCall(*edge.source);
        if (call == nullptr && edge.source != nullptr &&
            instanceBaseModel != nullptr)
          call = instanceBaseModel->findCall(*edge.source);
        const FunctionInfo *callee = semanticModel.findFunction(edge.function);
        if (callee == nullptr && instanceBaseModel != nullptr)
          callee = instanceBaseModel->findFunction(edge.function);
        if (call == nullptr || call->function != edge.function ||
            callee == nullptr)
          return false;
        const SemanticType owner =
            substituteType(call->dispatchOwner, substitution);
        // A member selected in one class definition belongs to that
        // definition. When the substituted owner selects another definition
        // of the same name, the call reaches a different member whose
        // requirements this edge does not record.
        if (selectsOtherClassDefinition(owner, callee->ownerClass))
          return false;
        GenericSubstitution next = classSubstitution(owner);
        std::size_t typeIndex = 0;
        std::size_t valueIndex = 0;
        for (const auto &generic : callee->genericParameters) {
          if (generic.value) {
            if (valueIndex >= call->valueArguments.size())
              return false;
            next.values.emplace(
                generic.id,
                substituteCompileTimeValue(call->valueArguments[valueIndex++],
                                           substitution.values));
          } else if (generic.pack) {
            std::vector<SemanticType> elements;
            while (typeIndex < call->typeArguments.size())
              elements.push_back(substituteType(
                  call->typeArguments[typeIndex++], substitution));
            next.types.emplace(
                generic.id,
                elements.size() == 1 &&
                        elements.front().kind == SemanticType::TypePack &&
                        !elements.front().concretePack
                    ? elements.front()
                    : SemanticType::concreteTypePack(generic.id,
                                                     std::move(elements)));
          } else {
            if (typeIndex >= call->typeArguments.size())
              return false;
            next.types.emplace(
                generic.id,
                substituteType(call->typeArguments[typeIndex++], substitution));
          }
        }
        if (!self(self, edge.function, edge.parameterIndex, next))
          return false;
      }
      visiting.pop_back();
      return true;
    };
    // Incomplete or cyclic graphs cannot use a partial signature subset
    // to disambiguate. The ordinary unique-item fallback is still valid;
    // its concrete body and invocation are checked before HIR admission.
    if (!collect(collect, root, parameter, rootSubstitution))
      signatures.clear();
    return signatures;
  };

  bool valid = true;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (!isContextualNativeFunctionArgument(arguments[index])) {
      continue;
    }
    if (index < target.parameterTypes.size() &&
        target.parameterTypes[index].kind == SemanticType::NativeFunction) {
      continue;
    }
    const CallableParameterContract *contract =
        callableParameterContract(target.id, index);
    const bool threadTask = contract != nullptr &&
                            contract->boundary == CallableBoundary::Owned &&
                            contract->ownedTransport &&
                            contract->ownedTransport->kind ==
                                CallableOwnedTransportKind::ThreadTask;

    const std::optional<Symbol> source =
        resolveExpressionSymbol(arguments[index]);
    std::vector<std::pair<const CallableCandidate *, GenericSubstitution>>
        matches;
    if (source) {
      for (const CallableCandidate &candidate : source->overloads) {
        if (!functionItemCandidateIsEligible(candidate)) {
          continue;
        }
        GenericSubstitution substitution = baseSubstitution;
        // The candidate's exact item type is known before checking a result
        // projection which may itself depend on that callable parameter.
        // Bind it first, then compare the fully substituted contract. This
        // does not infer a callable from its desired return type.
        const SemanticType candidateItem = SemanticType::functionItemType(
            candidate.id, candidate.returnType, candidate.parameterTypes);
        if (!target.parameterPack && index < target.parameterTypes.size() &&
            !inferOuter(inferOuter, target.parameterTypes[index], candidateItem,
                        substitution)) {
          continue;
        }
        bool matchesContract = true;
        if (threadTask) {
          SemanticType result = contract->ownedTransport->destinationType;
          result = substituteType(result, substitution);
          matchesContract = candidate.parameterTypes.empty() &&
                            exactMatch(exactMatch, result, candidate.returnType,
                                       substitution);
        } else {
          for (const CallableSignatureRequirement &signature :
               contextualRequirements(target.id, index, substitution)) {
            if (!callableCapabilitySatisfies(CallableInvocationCapability::Read,
                                             signature.capability) ||
                (!signature.inferResultFromInstantiation &&
                 !exactMatch(exactMatch,
                             substituteType(signature.returnType, substitution),
                             candidate.returnType, substitution))) {
              matchesContract = false;
              break;
            }
            std::vector<SemanticType> substitutedArguments;
            for (const SemanticType &parameter : signature.parameterTypes) {
              substitutedArguments.push_back(
                  substituteType(parameter, substitution));
            }
            const auto concreteArguments =
                signature.source == nullptr
                    ? std::nullopt
                    : concreteCallArguments(substitutedArguments,
                                            signature.source->arguments());
            if (!concreteArguments && signature.source != nullptr &&
                std::any_of(substitutedArguments.begin(),
                            substitutedArguments.end(),
                            [](const SemanticType &argument) {
                              return argument.kind == SemanticType::TypePack &&
                                     !argument.concretePack;
                            })) {
              // An open forwarding pack cannot select among overloads by
              // arity. Only a unique eligible item can survive this primary
              // check; its concrete invocation is revalidated per instance,
              // exactly as when the item is first bound to an auto local.
              continue;
            }
            if (!concreteArguments ||
                concreteArguments->size() != candidate.parameterTypes.size()) {
              matchesContract = false;
              break;
            }
            for (std::size_t parameter = 0;
                 parameter < concreteArguments->size(); ++parameter) {
              const auto &argument = (*concreteArguments)[parameter];
              if (!(argument.forwardedPackElement
                        ? forwardedPackArgumentMatches(
                              candidate.parameterTypes[parameter],
                              argument.type)
                        : callArgumentMatches(
                              candidate.parameterTypes[parameter],
                              argument.type, *argument.expression))) {
                matchesContract = false;
                break;
              }
            }
            if (!matchesContract) {
              break;
            }
          }
        }
        if (!matchesContract) {
          continue;
        }
        const SemanticType item = SemanticType::functionItemType(
            candidate.id, candidate.returnType, candidate.parameterTypes);
        if (target.parameterPack && index >= fixedParameterCount(target)) {
          // A contextual name supplies one concrete pack element, not a new
          // formal parameter. Preserve the other independently deduced items
          // and account for preceding expanded source arguments.
          const GenericParameterInfo *pack = packGenericParameter(target);
          if (pack == nullptr)
            continue;
          auto found = substitution.types.find(pack->id);
          if (found == substitution.types.end() ||
              found->second.kind != SemanticType::TypePack ||
              !found->second.concretePack)
            continue;
          std::vector<SemanticType> preceding;
          for (std::size_t before = fixedParameterCount(target); before < index;
               ++before)
            SemanticType::appendExpandedArgument(preceding,
                                                 analysis.types[before]);
          if (preceding.size() >= found->second.arguments.size())
            continue;
          SemanticType &element = found->second.arguments[preceding.size()];
          if (element != SemanticType::Unknown && element != item)
            continue;
          element = item;
        } else {
          if (index >= target.parameterTypes.size() ||
              !inferOuter(inferOuter, target.parameterTypes[index], item,
                          substitution))
            continue;
        }
        matches.emplace_back(&candidate, std::move(substitution));
      }
    }
    if (matches.size() != 1) {
      if (diagnose) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2090", DiagnosticPhase::Semantics,
            expressionToken(arguments[index]),
            matches.empty()
                ? "No eligible namespace-scope GTI function overload "
                  "exactly matches the required callable signature."
                : "The function item is ambiguous; " +
                      std::to_string(matches.size()) +
                      " overloads exactly match the required callable "
                      "signature.");
        diagnostic.hints.emplace_back(
            "Use one non-generic namespace-scope GTI function whose exact "
            "parameters and result match the selected callable contract. "
            "An unsafe function cannot become a safe function item.");
        if (source) {
          for (const CallableCandidate &candidate : source->overloads) {
            if (candidate.declaration != nullptr) {
              diagnostic.related.push_back(
                  {tokenSpan(candidate.declaration->name()),
                   "Candidate: " + functionSignatureSpelling(candidate)});
            }
          }
        }
        diagnostics.emplace_back(std::move(diagnostic));
      }
      valid = false;
      continue;
    }
    const CallableCandidate &selected = *matches.front().first;
    baseSubstitution = std::move(matches.front().second);
    analysis.types[index] = SemanticType::functionItemType(
        selected.id, selected.returnType, selected.parameterTypes);
  }
  return valid;
}

auto SemanticVisitor::Impl::isContextualCallableResultArgument(
    const ExprPtr &expression) const -> bool {
  const Call *call = directCall(expression);
  if (call == nullptr)
    return false;

  // Inspect declaration types only. Evaluating the receiver here would move
  // ownership or create loans before the selected call analyzes its arguments.
  const auto receiverType = [&](const auto &self,
                                const ExprPtr &receiver) -> SemanticType {
    if (receiver == nullptr)
      return SemanticType::Unknown;
    if (const auto *group = exprAs<Grouping>(receiver.get()))
      return self(self, group->expression());
    if (const Call *move = directCall(receiver)) {
      const auto candidate = intrinsicCandidate(move->callee());
      if (candidate && candidate->record().intrinsic == IntrinsicKind::Move &&
          move->arguments().size() == 1)
        return self(self, move->arguments().front());
      return SemanticType::Unknown;
    }
    if (isThisObject(receiver) && currentClass)
      return SemanticType::classType(*currentClass);
    if (const auto *member = exprAs<Get>(receiver.get());
        member && member->access().kind == TokenKind::DOT) {
      SemanticType owner = self(self, member->object());
      if (owner.kind == SemanticType::Reference &&
          owner.arguments.size() == 1) {
        SemanticType referent = owner.arguments.front();
        owner = std::move(referent);
      }
      if (const MemberInfo *field = findMember(owner, member->name()))
        return substituteSymbol(field->symbol, owner).type;
      return SemanticType::Unknown;
    }
    const auto symbol = resolveExpressionSymbol(receiver);
    return symbol ? symbol->type : SemanticType::Unknown;
  };
  const SemanticType type = receiverType(receiverType, call->callee());
  return type.kind == SemanticType::TypeParameter ||
         (type.kind == SemanticType::Reference && type.arguments.size() == 1 &&
          type.arguments.front().kind == SemanticType::TypeParameter);
}

auto SemanticVisitor::Impl::analyzeContextualCallArguments(
    const ExprList &arguments,
    std::span<const std::optional<ResultUseStep>> uses)
    -> ContextualCallArguments {
  ContextualCallArguments result;
  result.types.reserve(arguments.size());
  result.deferred.reserve(arguments.size());
  bool deferred = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const ExprPtr &argument = arguments[index];
    deferred = deferred || isContextualArrayArgument(argument) ||
               isContextualCallableResultArgument(argument);
    // A namespace function name has no evaluation effects. Its selection may
    // wait for the exact signature without postponing later ordinary arguments
    // whose types are needed to deduce a pack. Effectful contextual producers
    // still defer their entire suffix; HIR/MIR retain source evaluation order.
    const bool argumentDeferred =
        deferred || isContextualNativeFunctionArgument(argument);
    result.deferred.emplace_back(argumentDeferred);
    if (argumentDeferred) {
      result.types.emplace_back(SemanticType::Unknown);
      continue;
    }
    std::optional<ResultUseScope> argumentUse;
    if (index < uses.size() && uses[index] && consultsResultUse(argument.get()))
      argumentUse.emplace(*this, argument.get(),
                          std::vector<ResultUseStep>{*uses[index]},
                          ResultUseAccess::Unknown);
    result.types.emplace_back(analyze(argument));
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
    if (parameter.kind != SemanticType::NativeFunction &&
        parameter.kind != SemanticType::FunctionItem) {
      return false;
    }
    if (parameter.kind == SemanticType::FunctionItem) {
      const std::optional<Symbol> symbol = resolveExpressionSymbol(expression);
      return symbol && parameter.hasFunctionItemShape() &&
             std::any_of(symbol->overloads.begin(), symbol->overloads.end(),
                         [&](const CallableCandidate &candidate) {
                           return candidate.id == parameter.functionId &&
                                  functionItemCandidateIsEligible(candidate);
                         });
    }
    const std::optional<Symbol> symbol = resolveExpressionSymbol(expression);
    return symbol &&
           matchingNativeFunctionCandidates(*symbol, parameter).size() == 1;
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
    const bool freshFunctionItem =
        parameter.kind == SemanticType::Reference &&
        parameter.referenceAccess == AccessMode::ReadOnly &&
        parameter.arguments.size() == 1 &&
        parameter.arguments.front().kind == SemanticType::FunctionItem &&
        parameter.arguments.front() == analysis.types[index] &&
        isContextualNativeFunctionArgument(arguments[index]);
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
      if (parameter.kind != SemanticType::NativeFunction &&
          parameter.kind != SemanticType::FunctionItem && !freshFunctionItem) {
        report(expressionToken(arguments[index]),
               "A function name argument requires one exact callable or "
               "named native-function context from the selected target.",
               "GTI-S2090");
        analysis.types[index] = SemanticType::Unknown;
        valid = false;
        continue;
      }
      analysis.types[index] = analyzeInitializer(
          arguments[index],
          freshFunctionItem ? parameter.arguments.front() : parameter);
    } else {
      analysis.types[index] =
          analyzeExpectedCallableResult(arguments[index], parameter);
    }
    if (analysis.types[index] != SemanticType::Unknown &&
        parameter != SemanticType::Unknown &&
        !callArgumentMatches(parameter, analysis.types[index],
                             arguments[index]) &&
        !(freshFunctionItem &&
          analysis.types[index] == parameter.arguments.front() &&
          freshTemporaryValue(arguments[index]).eligible)) {
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
      function.bodyDeclaration()->body() == nullptr ||
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
         target->bodyDeclaration()->body() != nullptr;
}

auto SemanticVisitor::Impl::ephemeralTemporaryArgumentMatches(
    const CallableCandidate &function, const SemanticType &parameter,
    const SemanticType &argument, const ExprPtr &expression) const -> bool {
  if (parameter.kind != SemanticType::Reference ||
      parameter.referenceAccess != AccessMode::ReadOnly ||
      parameter.arguments.size() != 1 ||
      parameter.arguments.front() != argument ||
      (argument.kind != SemanticType::Class && !payloadEnumType(argument) &&
       argument.kind != SemanticType::Lambda &&
       argument.kind != SemanticType::FunctionItem) ||
      !expression || !ephemeralBorrowTargetSupported(function)) {
    return false;
  }
  if (argument.kind == SemanticType::FunctionItem &&
      isContextualNativeFunctionArgument(expression)) {
    // Overload viability carries the selected item type locally. Do not publish
    // a selection into the shared model until the enclosing call is selected.
    // Only an actual namespace function name can supply this fresh item; a
    // stored item, explicit move, or factory result is not a fresh producer.
    const auto source = resolveExpressionSymbol(expression);
    if (!source || source->type != SemanticType::Function)
      return false;
    return std::any_of(source->overloads.begin(), source->overloads.end(),
                       [&](const CallableCandidate &candidate) {
                         return functionItemCandidateIsEligible(candidate) &&
                                SemanticType::functionItemType(
                                    candidate.id, candidate.returnType,
                                    candidate.parameterTypes) == argument;
                       });
  }
  // An explicit move transfers a whole local into a temporary owned by the
  // enclosing full expression, exactly as a moved receiver is; the call
  // borrows that temporary.
  if (const ExprPtr *moved = explicitMoveSource(expression)) {
    const Expr *source = moved->get();
    while (const auto *grouping = exprAs<Grouping>(source))
      source = grouping->expression().get();
    const ExpressionInfo *info = semanticModel.findExpression(*expression);
    const ClassInfo *record =
        argument.kind == SemanticType::Class ? classInfo(argument) : nullptr;
    return exprAs<Variable>(source) != nullptr && info != nullptr &&
           info->category == ValueCategory::Value && info->type == argument &&
           info->traits.movable && !info->traits.containsBorrowedState &&
           (argument.kind == SemanticType::Lambda ||
            payloadEnumType(argument) ||
            (record != nullptr && (record->kind == ClassKind::Class ||
                                   record->kind == ClassKind::Struct)));
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
  const auto *itemContract = argument.kind == SemanticType::FunctionItem
                                 ? callableParameterContract(function.id, index)
                                 : nullptr;
  // A fresh item binds for the call as a lambda does: a contract that is
  // not yet final is provisional, and finalization proves it confined.
  if ((!deferred || argument.kind == SemanticType::FunctionItem) &&
      (argument.kind != SemanticType::FunctionItem || itemContract == nullptr ||
       itemContract->boundary == CallableBoundary::Confined) &&
      ephemeralTemporaryArgumentMatches(function, parameter, argument,
                                        expression)) {
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
  if (parameter.kind == SemanticType::NativeFunction &&
      isContextualNativeFunctionArgument(expression)) {
    if (const auto symbol = resolveExpressionSymbol(expression);
        symbol && symbol->type == SemanticType::Function) {
      (void)analyzeNativeFunctionItem(*expression, *symbol, parameter,
                                      expressionToken(expression));
      return;
    }
  }
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
      } else if (argument.kind == SemanticType::Class ||
                 payloadEnumType(argument)) {
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
          case FreshTemporaryValueFailure::UnstableBorrowOwner:
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
                     " is not a supported fresh record producer or passive "
                     "payload-enum construction.";
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
      if (typeTraits(parameter).movable)
        diagnostics.back().hints.emplace_back(
            "Pass the argument with std::move(...) when this call should "
            "transfer ownership.");
      return;
    }
    std::string copyDescription =
        " would copy a move-only value; use std::move(owner) to ";
    if (typeTraits(parameter).ownership == OwnershipKind::Unique) {
      copyDescription = " would copy a unique owner; use std::move(owner) to ";
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
    bool hasView = false;
    if (const auto *owner = classInfo(argument)) {
      if (const auto member = owner->members.find("view");
          member != owner->members.end() &&
          member->second.access == AccessModifier::Public) {
        const Symbol concrete =
            substituteSymbol(member->second.symbol, argument);
        hasView =
            concrete.overloads.size() == 1 &&
            concrete.overloads.front().returnType == parameter &&
            concrete.overloads.front().parameterTypes.empty() &&
            concrete.overloads.front().record().genericParameters.empty() &&
            concrete.overloads.front().requirements.empty() &&
            !concrete.overloads.front().staticMember &&
            !concrete.overloads.front().record().requiresUnsafe &&
            concrete.overloads.front().receiverMutability ==
                ReceiverMutability::ReadOnly;
      }
    }
    diagnostic.hints.emplace_back(
        hasView
            ? "Pass 'value.view()' to obtain the exact parameter type; keep "
              "the "
              "source value alive while the view is used."
            : "Calls require exact argument types; pass the required type or "
              "use a supported explicit conversion when intentional.");
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
  if (!validateNestedClassAccess(classId, site)) {
    result.valid = false;
    return result;
  }

  if (arguments.size() < classInfo(classId).genericParameters.size())
    resolveClassGenericDefaults(classId);
  const ClassInfo &owner = classInfo(classId);
  if (!owner.declaration->genericArgumentPatterns().empty() ||
      !owner.definitionFamilyMembers.empty() ||
      owner.definitionFamilyPrimary != 0) {
    resolveClassArgumentPatterns(classId);
    for (const TypeRef &argument : arguments) {
      const std::size_t before = diagnostics.size();
      validateType(argument);
      if (argument.genericArgumentSyntax == GenericArgumentSyntax::Value) {
        report(argument.name.last(),
               "A class type pattern requires a type argument.",
               diagnosticCode);
      }
      result.types.push_back(typeOf(argument));
      result.valid = result.valid && diagnostics.size() == before;
    }
    const ClassId selected = selectClassDefinition(classId, result.types);
    resolveClassArgumentPatterns(selected);
    if (sourceGraph != nullptr && currentSourceUnit != 0 &&
        !sourceGraph->isDeclarationVisible(currentSourceUnit,
                                           classInfo(selected).sourceUnit,
                                           classInfo(selected).name.position)) {
      reportInvisibleDeclaration(site, owner.name.lexeme,
                                 classInfo(selected).name,
                                 classInfo(selected).sourceUnit);
      result.valid = false;
    }
    if (classInfo(selected).declaration->genericArgumentPatterns().empty()) {
      auto ordinary = resolveNominalArguments(
          classInfo(selected).genericParameters, classInfo(selected).name,
          arguments, site, std::move(diagnosticCode));
      ordinary.valid = ordinary.valid && result.valid;
      return ordinary;
    }
    if (!classArgumentPatternsMatch(classInfo(selected), result.types)) {
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2023", DiagnosticPhase::Semantics, site,
                         "Type arguments do not match the generic class "
                         "argument pattern for '" +
                             owner.name.lexeme + "'.");
      diagnostic.related.push_back(
          {tokenSpan(classInfo(selected).name),
           "Generic class argument pattern declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
      result.valid = false;
    }
    return result;
  }
  return resolveNominalArguments(owner.genericParameters, owner.name, arguments,
                                 site, std::move(diagnosticCode));
}

auto SemanticVisitor::Impl::resolveNominalArguments(
    const std::vector<GenericParameterInfo> &parameters, const Token &name,
    const std::vector<TypeRef> &arguments, const Token &site,
    std::string diagnosticCode) -> ResolvedClassArguments {
  ResolvedClassArguments result;
  if (!parameters.empty() && parameters.back().pack) {
    const std::size_t fixed = parameters.size() - 1;
    if (arguments.size() < fixed) {
      report(site,
             "Type '" + name.lexeme + "' requires at least " +
                 std::to_string(fixed) + " generic type arguments.",
             diagnosticCode);
      result.valid = false;
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const TypeRef &argument = arguments[index];
      const GenericParameterInfo &parameter =
          parameters[std::min(index, fixed)];
      const bool enclosingAllowPack = allowPackTypeReference;
      allowPackTypeReference = argument.packExpansion && index >= fixed &&
                               index + 1 == arguments.size();
      const std::size_t before = diagnostics.size();
      validateType(argument);
      allowPackTypeReference = enclosingAllowPack;
      if (typeOf(argument).kind == SemanticType::Reference) {
        validateReferencePlacement(argument, true, "generic type argument",
                                   false, true);
      }
      if (argument.genericArgumentSyntax == GenericArgumentSyntax::Value) {
        report(argument.name.last(),
               "A generic type pack requires type arguments.", diagnosticCode);
      }
      std::vector<SemanticType> expanded;
      SemanticType::appendExpandedArgument(expanded, typeOf(argument));
      for (const SemanticType &type : expanded) {
        const SemanticType constrained =
            type.kind == SemanticType::TypePack
                ? SemanticType::typeParameter(type.genericParameterId)
                : type;
        if (const auto failure =
                firstConstraintFailure({parameter}, {constrained})) {
          reportConstraintFailure(argument.name.last(), *failure);
        }
      }
      result.types.insert(result.types.end(), expanded.begin(), expanded.end());
      result.valid = result.valid && diagnostics.size() == before;
    }
    return result;
  }
  result.types.reserve(genericTypeParameterCount(parameters));
  result.values.reserve(genericValueParameterCount(parameters));
  const std::size_t requiredArguments = static_cast<std::size_t>(
      std::count_if(parameters.begin(), parameters.end(),
                    [](const GenericParameterInfo &parameter) {
                      return parameter.value ? !parameter.defaultValue
                                             : !parameter.defaultType;
                    }));
  if (arguments.size() < requiredArguments ||
      arguments.size() > parameters.size()) {
    const bool typeOnly = genericValueParameterCount(parameters) == 0;
    const std::string arity =
        requiredArguments == parameters.size()
            ? std::to_string(requiredArguments)
            : "between " + std::to_string(requiredArguments) + " and " +
                  std::to_string(parameters.size());
    report(site,
           "Type '" + name.lexeme + "' requires " + arity +
               (typeOnly ? " generic type argument" : " generic argument") +
               (parameters.size() == 1 ? "." : "s."),
           diagnosticCode);
    result.valid = false;
  }

  GenericSubstitution substitution;
  const std::size_t count = std::min(arguments.size(), parameters.size());
  for (std::size_t index = 0; index < count; ++index) {
    const GenericParameterInfo &parameter = parameters[index];
    const TypeRef &argument = arguments[index];
    if (parameter.value) {
      const std::optional<CompileTimeValue> value =
          genericValueArgument(argument);
      if (!value) {
        if (!reportGenericValueArgumentFailure(
                argument,
                "Generic value argument for '" + parameter.name.lexeme + "'")) {
          report(argument.name.last(),
                 "Generic parameter '" + parameter.name.lexeme +
                     "' requires a uint64_t compile-time value.",
                 diagnosticCode);
        }
        result.values.emplace_back();
        result.valid = false;
      } else {
        substitution.values.emplace(parameter.id, *value);
        if (argument.genericValueExpression) {
          recordArrayExtentUses(*argument.genericValueExpression);
        }
      }
      continue;
    }

    if (argument.genericArgumentSyntax == GenericArgumentSyntax::Value) {
      report(argument.name.last(),
             "Generic parameter '" + parameter.name.lexeme +
                 "' requires a type argument.",
             diagnosticCode);
      result.valid = false;
      continue;
    }
    const bool enclosingAllowPack = allowPackTypeReference;
    allowPackTypeReference = false;
    validateType(argument);
    allowPackTypeReference = enclosingAllowPack;
    const SemanticType type = typeOf(argument);
    if (type.kind == SemanticType::Reference) {
      validateReferencePlacement(argument, true, "generic type argument", false,
                                 true);
    }
    if (const auto failure = firstConstraintFailure({parameter}, {type})) {
      reportConstraintFailure(argument.name.last(), *failure);
      result.valid = false;
    }
    substitution.types.emplace(parameter.id, type);
  }
  if (!applyGenericDefaults(parameters, substitution, site, true)) {
    result.valid = false;
  }
  result.types.clear();
  result.values.clear();
  for (const GenericParameterInfo &parameter : parameters) {
    if (parameter.value) {
      const auto found = substitution.values.find(parameter.id);
      if (found == substitution.values.end()) {
        result.valid = false;
      } else {
        result.values.emplace_back(found->second);
      }
    } else {
      const auto found = substitution.types.find(parameter.id);
      if (found == substitution.types.end()) {
        result.valid = false;
      } else {
        result.types.emplace_back(found->second);
      }
    }
  }
  for (std::size_t index = count; index < arguments.size(); ++index) {
    if (arguments[index].genericArgumentSyntax !=
        GenericArgumentSyntax::Value) {
      validateType(arguments[index]);
    }
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
    deferred =
        deferred || (expression != nullptr &&
                     (isContextualArrayArgument(*expression) ||
                      (argumentTypes[index] == SemanticType::Unknown &&
                       isContextualCallableResultArgument(*expression))));
    if (expression != nullptr &&
        argumentTypes[index].kind == SemanticType::TypePack &&
        argumentTypes[index].concretePack) {
      if (!appendConcreteCallArgument(argumentTypes[index], *expression,
                                      analyzedArguments)) {
        currentType = SemanticType::Unknown;
        return;
      }
    } else {
      analyzedArguments.push_back({.type = argumentTypes[index],
                                   .expression = expression,
                                   .deferred = deferred});
    }
  }
  analyzeConstructorCallArguments(construction, classId, typeArguments,
                                  valueArguments, analyzedArguments, paren);
}

auto SemanticVisitor::Impl::forwardedPackArgumentMatches(
    const SemanticType &parameter, const SemanticType &argument) const -> bool {
  if (parameter == SemanticType::Unknown || argument == SemanticType::Unknown) {
    return true;
  }
  if (argument.kind == SemanticType::Reference) {
    return parameter == argument;
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
    bool requireExactNullParameter, bool contextualConstruction,
    std::vector<const ConstructorInfo *> *candidateProbe) -> void {
  selectConstructorCallArguments(&construction, nullptr, classId, typeArguments,
                                 valueArguments, std::move(arguments), paren,
                                 requireExactNullParameter,
                                 contextualConstruction, candidateProbe);
}

auto SemanticVisitor::Impl::selectConstructorCallArguments(
    const Expr *construction, const ReturnStmt *emptyReturn, ClassId classId,
    const std::vector<SemanticType> &typeArguments,
    const std::vector<CompileTimeValue> &valueArguments,
    std::vector<AnalyzedCallArgument> arguments, const Token &paren,
    bool requireExactNullParameter, bool contextualConstruction,
    std::vector<const ConstructorInfo *> *candidateProbe) -> void {
  if ((construction == nullptr) == (emptyReturn == nullptr) ||
      (emptyReturn != nullptr &&
       (emptyReturn->value() || !arguments.empty() || !contextualConstruction ||
        candidateProbe != nullptr))) {
    currentType = SemanticType::Unknown;
    return;
  }
  // A probe collects the constructors whose parameter types take these
  // arguments, whatever access or ownership the selection would then
  // require. It reports nothing and records nothing.
  if (candidateProbe != nullptr)
    candidateProbe->clear();
  const SemanticType selectedClass = selectExactClassSpecialization(
      SemanticType::classType(classId, typeArguments, valueArguments));
  classId = selectedClass.classId;
  if (classId == 0 || classId > classes.size()) {
    currentType = SemanticType::Unknown;
    return;
  }

  const ClassInfo &owner = classInfo(classId);
  const GenericSubstitution substitution = classSubstitution(selectedClass);
  const SemanticType constructedType = selectedClass;
  if (owner.compilerCapability == CompilerCapabilityTypeKind::ErasedObject ||
      (owner.compilerCapability ==
           CompilerCapabilityTypeKind::FunctionPointer &&
       (arguments.size() != 1 || arguments.front().type != constructedType))) {
    report(paren,
           "This opaque capability cannot be constructed as an object; "
           "use its checked reference or invocation factory.",
           "GTI-S2019");
    currentType = SemanticType::Unknown;
    return;
  }
  if (candidateProbe == nullptr &&
      typeTraits(constructedType).containsBorrowedState &&
      storedReferencePaths(constructedType).empty()) {
    const bool fieldAlreadyRejected = std::any_of(
        owner.fields.begin(), owner.fields.end(), [&](const FieldInfo &field) {
          return field.declaration != nullptr &&
                 std::any_of(diagnostics.begin(), diagnostics.end(),
                             [&](const Diagnostic &diagnostic) {
                               return diagnostic.code == "GTI-S2045" &&
                                      diagnostic.primary ==
                                          tokenSpan(field.declaration->name());
                             });
        });
    if (!fieldAlreadyRejected) {
      report(paren,
             "A borrowed aggregate must contain checked stored-reference "
             "paths through ordinary nominal fields and cannot declare a "
             "custom destructor.",
             "GTI-S2045");
    }
    currentType = SemanticType::Unknown;
    return;
  }
  if (candidateProbe == nullptr && !isCompilerTrustedRequester(paren) &&
      typeRequiresThreads(constructedType) &&
      !validateConcurrentSurfaceAvailability(paren, "This concurrent value")) {
    currentType = SemanticType::Unknown;
    return;
  }
  if (candidateProbe == nullptr && owner.abstract) {
    report(paren,
           "Cannot construct abstract " +
               std::string(classKindSpelling(owner.kind)) + " '" +
               owner.name.lexeme + "'.",
           "GTI-S2044");
  }

  // A same-type fresh result already denotes the destination object. Unlike
  // std::move(place), its Value category does not request a lifecycle move.
  // Resolve this before converting-constructor overload selection, including
  // deleted/private special members. The producer owns all actual execution.
  const ExprPtr *writtenSource = nullptr;
  if (const auto *call = exprAs<Call>(construction);
      call != nullptr && call->arguments().size() == 1)
    writtenSource = &call->arguments().front();
  else if (const auto *direct = exprAs<DirectInitializer>(construction);
           direct != nullptr && direct->arguments().size() == 1)
    writtenSource = &direct->arguments().front();
  if (candidateProbe == nullptr && !requireExactNullParameter &&
      !contextualConstruction && arguments.size() == 1 &&
      arguments.front().type == constructedType &&
      !arguments.front().forwardedPackElement && writtenSource != nullptr &&
      arguments.front().expression == writtenSource &&
      semanticModel.isFreshClassResult(**arguments.front().expression)) {
    semanticModel.record(
        *construction,
        ResolvedConstructionInfo{
            .constructedType = constructedType,
            .parameterTypes = {constructedType},
            .borrowOrigin = typeTraits(constructedType).containsBorrowedState
                                ? BorrowOriginKind::Argument
                                : BorrowOriginKind::None,
            .borrowAccess = borrowAccess(constructedType),
            .directInitialization = true});
    recordSelectedConstructionOccurrence(*construction);
    currentType = constructedType;
    return;
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
  // std::meta::construct selects the generated designated constructor, which
  // ordinary positional construction never considers.
  std::vector<const ConstructorInfo *> candidates;
  if (selectingDesignatedConstructor && owner.reversedDesignatedConstructor) {
    candidates.push_back(&*owner.reversedDesignatedConstructor);
  } else {
    for (const ConstructorInfo &constructor : owner.constructors) {
      candidates.push_back(&constructor);
    }
  }
  for (const ConstructorInfo *candidatePointer : candidates) {
    const ConstructorInfo &constructor = *candidatePointer;
    // A declaration constructs only through a constructor declared implicit.
    if (contextualConstruction && !constructor.implicitConstruction) {
      continue;
    }
    const std::size_t fixedCount =
        constructor.parameterPack && !constructor.parameterTypes.empty()
            ? constructor.parameterTypes.size() - 1
            : constructor.parameterTypes.size();
    if (arguments.size() < constructor.requiredParameterCount ||
        (!constructor.parameterPack &&
         arguments.size() > constructor.parameterTypes.size()) ||
        (!arguments.empty() &&
         arguments.back().type.kind == SemanticType::TypePack &&
         (!constructor.parameterPack || arguments.size() <= fixedCount))) {
      continue;
    }
    GenericSubstitution constructorSubstitution = substitution;
    bool inferred = true;
    GenericParameterId conflictingParameter = 0;
    bool conflictingValueParameter = false;
    Token conflictingSite = paren;
    for (std::size_t index = 0; index < std::min(fixedCount, arguments.size());
         ++index) {
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
      if (parameter.pack) {
        for (std::size_t index = fixedCount; index < arguments.size();
             ++index) {
          constructorTypeArguments.push_back(arguments[index].type);
        }
      } else if (parameter.value) {
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
          canAccessMember(constructor.access, classId, constructedType)) {
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
    parameterTypes.reserve(constructor.parameterPack ? arguments.size()
                                                     : fixedCount);
    for (std::size_t index = 0; index < fixedCount; ++index) {
      parameterTypes.push_back(substituteType(constructor.parameterTypes[index],
                                              constructorSubstitution));
    }
    if (constructor.parameterPack) {
      for (std::size_t index = fixedCount; index < arguments.size(); ++index) {
        parameterTypes.push_back(arguments[index].type);
      }
    }
    bool exact = true;
    std::optional<std::size_t> rejectedArgument;
    for (std::size_t index = 0; index < parameterTypes.size(); ++index) {
      const SemanticType &parameterType = parameterTypes[index];
      if (index >= arguments.size()) {
        continue;
      }
      const AnalyzedCallArgument &argument = arguments[index];
      const bool matches =
          requireExactNullParameter ? parameterType == SemanticType::NullPtr &&
                                          argument.type == SemanticType::NullPtr
          : argument.forwardedPackElement ||
                  parameterType.kind == SemanticType::TypePack
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
    if (candidateProbe != nullptr) {
      if (exact || rejectedArgument)
        candidateProbe->push_back(&constructor);
      continue;
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
               canAccessMember(constructor.access, classId, constructedType)) {
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
      if (exact && candidateProbe != nullptr) {
        candidateProbe->push_back(nullptr);
      } else if (exact) {
        viable.push_back(
            {nullptr, {firstType}, {}, {}, false, ConstructorKind::Ordinary});
      }
    }
  }
  if (candidateProbe != nullptr) {
    return;
  }

  if (!contextualConstruction && arguments.empty() &&
      defaultConstructor(owner) == nullptr &&
      classCanGenerateDefaultConstructor(constructedType)) {
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
  if (attemptedSpecial) {
    // A generic converting constructor instantiated with its own owner type
    // cannot hijack that owner's copy/move lifecycle operation. Preserve the
    // selected (including deleted/private) lifecycle policy before ordinary
    // by-value matching can outrank its reference-shaped formal.
    std::erase_if(viable, [&](const ViableConstructor &candidate) {
      return candidate.kind == ConstructorKind::Ordinary &&
             candidate.constructor != nullptr &&
             !candidate.constructor->genericParameters.empty() &&
             !candidate.parameterTypes.empty() &&
             candidate.parameterTypes.front() == constructedType;
    });
  }
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
        // A return that initializes its declared type from a named move-only
        // value copies that value as any other return would; no constructor
        // call is written, so no constructor argument is reported.
        if (contextualReturnValue != nullptr && arguments.size() == 1 &&
            arguments[index].expression != nullptr &&
            arguments[index].expression->get() == contextualReturnValue &&
            rejected.parameterTypes[index] == arguments[index].type &&
            isMoveOnlyOwnerType(arguments[index].type)) {
          reportMoveOnlyReturnCopy(*arguments[index].expression,
                                   arguments[index].type);
          currentType = constructedType;
          return;
        }
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
          const std::string transfer =
              "Pass the argument with std::move(...) when this call should "
              "transfer ownership.";
          if (diagnostics.back().code == "GTI-S2018" &&
              diagnostics.back().message.find("std::move") ==
                  std::string::npos &&
              typeTraits(rejected.parameterTypes[index]).movable &&
              std::find(diagnostics.back().hints.begin(),
                        diagnostics.back().hints.end(),
                        transfer) == diagnostics.back().hints.end()) {
            diagnostics.back().hints.emplace_back(transfer);
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
      if (viable.empty() && arguments.size() == 1 &&
          !conversionOperatorCandidates(arguments.front().type, constructedType,
                                        false)
               .empty()) {
        diagnostic.hints.emplace_back(
            "'" + typeSpelling(arguments.front().type) + "' converts to '" +
            typeSpelling(constructedType) +
            "' through its conversion operator, which brace construction "
            "never calls; write '" +
            typeSpelling(constructedType) + "(value)' to use it.");
      }
      for (const ConstructorInfo *candidatePointer : candidates) {
        const ConstructorInfo &candidate = *candidatePointer;
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
        argument.type =
            analyzeExpectedCallableResult(*argument.expression, parameter);
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

  if (selected.constructor != nullptr && selected.constructor->requiresUnsafe) {
    if (!contextualConstruction && construction != nullptr &&
        semanticModel.unsafeOperation(*construction) ==
            UnsafeOperationKind::ObjectLifetime) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2055", DiagnosticPhase::Semantics, paren,
          "Raw in-place construction cannot invoke an unsafe constructor; "
          "its lifetime contract does not establish constructor-specific "
          "caller obligations.");
      diagnostic.hints.emplace_back(
          "Construct the value explicitly inside an unsafe block, then move "
          "the completed value into storage.");
      diagnostic.related.push_back(
          {tokenSpan(*selected.constructor->declaration->unsafeKeyword()),
           "Unsafe constructor declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
      currentType = constructedType;
      return;
    }
    const bool missing = unsafeDepth == 0;
    if (construction != nullptr)
      requireUnsafe(*construction,
                    UnsafeOperationKind::DeclaredUnsafeConstruction, paren,
                    "Calling an unsafe constructor");
    else if (missing)
      report(paren, "Calling an unsafe constructor requires an unsafe block.",
             "GTI-S2055");
    if (missing && selected.constructor->declaration != nullptr) {
      diagnostics.back().related.push_back(
          {tokenSpan(*selected.constructor->declaration->unsafeKeyword()),
           "This constructor requires the caller to uphold its unsafe "
           "contract."});
    }
  }

  if (selected.constructor != nullptr &&
      !canAccessMember(selected.constructor->access, classId,
                       constructedType)) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2007", DiagnosticPhase::Semantics, paren,
        "Constructor of '" + owner.name.lexeme + "' is " +
            std::string(accessModifierSpelling(selected.constructor->access)) +
            ".");
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
    recordMutableBindingUse(place.root);
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
              selected.constructor->declaration == nullptr ||
              selected.constructor->parameterPack
          ? std::vector<const Expr *>{}
          : omittedDefaultArguments(
                selected.constructor->declaration->parameters(),
                arguments.size());
  recordDefaultArgumentDependencies(defaultArguments);
  std::vector<BorrowedFieldOrigin> textOrigins;
  if (selected.constructor != nullptr) {
    const auto textInputs = ownerlessTextConstructorInputs(
        constructedType, selected.constructor->declaration,
        selected.parameterTypes);
    for (const auto &input : textInputs) {
      const std::size_t index = input.parameter;
      const ConstructorInitializer *initializer = input.initializer;
      // A relay names the one text field its formal stores.
      const bool soleField = std::count_if(textInputs.begin(), textInputs.end(),
                                           [index](const auto &other) {
                                             return other.parameter == index;
                                           }) == 1;
      const ExprPtr *argument = nullptr;
      bool potentialPackView = false;
      if (index < arguments.size()) {
        // A concrete by-value pack element is a callee parameter, so its text
        // storage may be dynamic even though the pack expression has no single
        // text-owner trait. Open packs never reach constructor selection.
        potentialPackView = arguments[index].forwardedPackElement;
        argument = arguments[index].expression;
      } else if (index <
                 selected.constructor->declaration->parameters().size()) {
        const auto &parameter =
            selected.constructor->declaration->parameters()[index];
        if (parameter.defaultArgument)
          argument = &parameter.defaultArgument->expression;
      }
      if (argument == nullptr || !*argument)
        continue;
      // A class formal whose text field the constructor stores re-roots the
      // argument's owner dependency at that field to the stored path
      // (L-OWN-02).
      if (input.sourceField != 0) {
        if (!potentialPackView && index < arguments.size())
          relayConstructorTextField(
              constructedType, *selected.constructor->declaration, input,
              *argument, textOrigins, contextualConstruction);
        continue;
      }
      const bool conditionalText =
          index < selected.parameterTypes.size() &&
          selected.parameterTypes[index].kind == SemanticType::TypeParameter;
      if (!conditionalText && !potentialPackView &&
          !isOwnerDependentTextExpression(*argument))
        continue;
      // A fresh value this function returns relays a formal stored in its
      // text field: each call publishes its own argument's owner there.
      if (!potentialPackView && soleField && input.payload == 0 &&
          index < arguments.size() &&
          initializer->target.name.segments.size() == 1)
        if (const ClassInfo *owner = classInfo(constructedType)) {
          const auto member =
              owner->members.find(initializer->target.name.last().lexeme);
          if (member != owner->members.end() && construction != nullptr &&
              relayReturnedTextFormal(*construction, constructedType,
                                      selected.constructor->declaration,
                                      *argument,
                                      toolingSymbolFor(member->second.symbol)))
            continue;
        }
      if (!potentialPackView && requireOwnerlessTextParameter(*argument))
        continue;
      // An open non-forwarding expression is checked when its exact text
      // instantiation is analyzed; no reusable caller obligation was proved.
      if (conditionalText)
        continue;
      // The fresh value's text field carries this argument's read-only owner
      // dependency, as a reference field carries its referent's (L-OWN-02).
      // A class whose fields other code reads keeps the ownerless rule.
      bool restrictedOwner = false;
      bool readingConstructor = false;
      if (!potentialPackView && index < arguments.size()) {
        auto dependencies = textFieldInitializerDependencies(
            constructedType, *initializer, *argument, input.payload);
        restrictedOwner =
            !dependencies.empty() && !textPublicationOwner(constructedType);
        readingConstructor =
            !dependencies.empty() && !restrictedOwner &&
            !constructionKeepsPublishedText(constructedType,
                                            selected.constructor->declaration);
        if (restrictedOwner || readingConstructor)
          dependencies.clear();
        if (!dependencies.empty() && !textOrigins.empty()) {
          // Phase 1 of L-OWN-02 publishes one owner per value.
          report(expressionToken(*argument),
                 "A value can hold dynamic text from one owner; this "
                 "argument would add a second owner dependency.",
                 "GTI-S2045");
          continue;
        }
        if (!dependencies.empty()) {
          textOrigins.push_back(
              {.resultFields = dependencies.front().designatedFields,
               .origin = BorrowOriginKind::Argument,
               .parameter = index,
               .access = AccessMode::ReadOnly});
          continue;
        }
      }
      reportOwnerDependentTextStorage(
          *argument, "passed to a constructor that stores it in a field");
      diagnostics.back().related.push_back(
          {tokenSpan(initializer->target.name.last()),
           "This initializer stores the view by value and requires an "
           "ownerless argument."});
      diagnostics.back().hints.clear();
      if (restrictedOwner)
        diagnostics.back().hints.emplace_back(
            "A class with a destructor, a base class or virtual methods keeps "
            "its text fields ownerless, because that code reads them outside "
            "the body that created the value.");
      if (readingConstructor)
        diagnostics.back().hints.emplace_back(
            "This constructor's body or another of its initializers reads the "
            "object, so it keeps its text fields ownerless; a constructor with "
            "an empty body can store the view of a local owner.");
      diagnostics.back().hints.emplace_back(
          "Pass a literal-backed value, store owned text instead, or use a "
          "checked reference field to retain the view and its owner.");
    }
  }
  std::optional<std::size_t> storedParameter =
      selected.constructor == nullptr ? std::nullopt
                                      : selected.constructor->borrowParameter;
  AccessMode storedAccess = selected.constructor == nullptr
                                ? AccessMode::ReadOnly
                                : selected.constructor->borrowAccess;
  std::vector<std::size_t> storedParameters;
  const auto storedPaths = storedReferencePaths(constructedType);
  const bool conditionalFields =
      std::any_of(storedPaths.begin(), storedPaths.end(),
                  [](const auto &path) { return !path.conditions.empty(); });
  if (const auto paths = storedReferencePaths(constructedType);
      !paths.empty() && !conditionalFields &&
      selected.kind == ConstructorKind::Ordinary) {
    storedParameters = selected.constructor == nullptr
                           ? std::vector<std::size_t>{}
                           : storedCarrierConstructorParameters(
                                 constructedType, *selected.constructor,
                                 selected.parameterTypes);
    storedParameter =
        storedParameters.empty()
            ? std::nullopt
            : std::optional<std::size_t>(storedParameters.front());
    storedAccess = borrowAccess(constructedType);
    if (!storedParameter) {
      report(paren,
             "Construction of a stored-reference carrier must bind every "
             "borrowed field from an exact compatible constructor parameter.",
             "GTI-S2045");
      currentType = constructedType;
      return;
    }
  }
  if (storedReferencePaths(constructedType).size() <= 1) {
    storedParameters.clear();
  }
  std::vector<BorrowedFieldOrigin> storedFields;
  if (!storedParameters.empty() && selected.constructor != nullptr)
    for (const auto &binding : storedCarrierConstructorBindings(
             constructedType, selected.constructor->declaration,
             selected.parameterTypes))
      storedFields.push_back(
          {.resultFields =
               storedFieldSymbols(constructedType, binding.destination.fields),
           .origin = BorrowOriginKind::Argument,
           .parameter = binding.parameter,
           .sourceFields =
               storedFieldSymbols(selected.parameterTypes[binding.parameter],
                                  binding.sourceFields),
           .access = binding.destination.reference.referenceAccess});
  ResolvedConstructionInfo resolved{
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
          storedParameter
              ? BorrowOriginKind::Argument
              : (selected.kind != ConstructorKind::Ordinary &&
                         typeTraits(constructedType).containsBorrowedState
                     ? BorrowOriginKind::Argument
                     : BorrowOriginKind::None),
      .borrowArgument = storedParameter.value_or(0),
      .borrowAccess = storedAccess,
      .borrowArguments = std::move(storedParameters),
      .borrowFields = std::move(storedFields),
      .generatedDefault = selected.generatedDefault,
      .kind = selected.kind};
  if (conditionalFields && selected.kind == ConstructorKind::Ordinary &&
      !containsOpenTypeParameter(constructedType)) {
    const auto summary = completeConstructorBorrow(resolved, tokenSpan(paren));
    ResolvedCallInfo publication;
    publication.returnType = constructedType;
    publication.parameterTypes = selected.parameterTypes;
    std::vector<const ExprPtr *> inputs;
    for (const auto &argument : arguments)
      inputs.push_back(argument.expression);
    const auto dependencies =
        summary
            ? instantiateCompleteBorrowArguments(publication, *summary, inputs)
            : std::nullopt;
    if (!dependencies) {
      report(paren,
             "Construction requires a complete checked dependency contract "
             "for every active borrowed field and alternative.",
             "GTI-S2045");
    } else {
      resolved.borrowFields = summary->dependencies;
      if (!summary->dependencies.empty()) {
        resolved.borrowOrigin = BorrowOriginKind::Argument;
        resolved.borrowArgument = summary->dependencies.front().parameter;
      }
      if (contextualConstruction && construction != nullptr)
        resolved.contextualBorrowDependencies = *dependencies;
      else if (construction != nullptr)
        semanticModel.recordBorrowDependencySet(*construction, *dependencies);
      else if (!dependencies->dependencies.empty())
        report(paren,
               "An empty return cannot retain dependencies from constructor "
               "default arguments.",
               "GTI-S2045");
    }
  }
  if (!textOrigins.empty()) {
    // The construction publishes its argument's owner as a stored value
    // dependency at the text field, as a one-reference carrier does. The
    // retained loan, not a complete dependency set, carries it, exactly as
    // for a dynamic view binding.
    resolved.borrowOrigin = BorrowOriginKind::Argument;
    resolved.borrowArgument = textOrigins.front().parameter;
    resolved.borrowAccess = AccessMode::ReadOnly;
    resolved.borrowFields = std::move(textOrigins);
    resolved.dependentTextPublication = true;
  }
  if (emptyReturn != nullptr) {
    if (currentProgramEffectOwner.valid())
      static_cast<void>(
          recordProgramConstructionEffects(tokenSpan(paren), resolved, 0));
    semanticModel.recordEmptyReturnConstruction(
        *emptyReturn, std::move(resolved),
        selected.constructor != nullptr &&
            selected.constructor->requiresUnsafe);
  } else if (contextualConstruction) {
    // The initializer keeps its own construction, call and occurrence records.
    // A converted value that holds its argument's owner is a dependent value
    // that only its consumer may keep (L-OWN-02).
    const bool dependent = resolved.dependentTextPublication;
    semanticModel.recordContextualConstruction(*construction,
                                               std::move(resolved), paren);
    if (dependent)
      noteTextDependentValue(*construction);
  } else {
    semanticModel.record(*construction, std::move(resolved));
    recordSelectedConstructionOccurrence(*construction);
  }
  currentType = constructedType;
}

auto SemanticVisitor::Impl::recordSelectedConstructionOccurrence(
    const Expr &construction) -> void {
  if (const auto *call = exprAs<Call>(&construction)) {
    const ResolvedConstructionInfo *resolved =
        semanticModel.findConstruction(construction);
    const Token token = callableToken(call->callee());
    if (resolved != nullptr) {
      const SemanticType &constructedType = resolved->constructedType;
      const ClassInfo &owner = classInfo(constructedType.classId);
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

auto SemanticVisitor::Impl::textViewMemberFactory(std::string_view member) const
    -> std::optional<CallableCandidate> {
  // A builtin counted view has no source class body. Its cursor operations
  // are ordinary source functions selected only from this trusted unit.
  // The public alias spelling is deliberately absent from this binding.
  const auto &symbols = activeNamespaceSymbols();
  const auto found =
      symbols.find("gti_internal::text_view_" + std::string(member));
  if (found == symbols.end() || found->second.overloads.size() != 1 ||
      sourceGraph == nullptr)
    return std::nullopt;
  const auto &candidate = found->second.overloads.front();
  const auto *unit = sourceGraph->findUnit(candidate.record().sourceUnit);
  if (unit == nullptr || unit->role != SourceUnitRole::StandardLibrary ||
      unit->standardLibraryName != "std/string_view" ||
      candidate.ownerClass != 0 || candidate.declaration == nullptr ||
      candidate.bodyDeclaration()->body() == nullptr ||
      candidate.record().intrinsic != IntrinsicKind::None ||
      candidate.record().requiresUnsafe || candidate.parameterPack ||
      !candidate.record().genericParameters.empty() ||
      candidate.parameterTypes.empty() ||
      candidate.parameterTypes.front() !=
          SemanticType::referenceTo(SemanticType::StringView,
                                    AccessMode::ReadOnly))
    return std::nullopt;
  return candidate;
}

auto SemanticVisitor::Impl::analyzeStringViewMemberCall(
    const Call &call, const Get &member,
    const std::vector<SemanticType> &argumentTypes, const Token &paren)
    -> void {
  if (const auto factory = textViewMemberFactory(member.name().lexeme)) {
    std::vector<const ExprPtr *> inputs{&member.object()};
    for (const auto &argument : call.arguments())
      inputs.push_back(&argument);
    std::vector<SemanticType> types{semanticModel.typeOf(*member.object())};
    types.insert(types.end(), argumentTypes.begin(), argumentTypes.end());
    if (inputs.size() != factory->parameterTypes.size()) {
      report(paren,
             "Text-view '" + member.name().lexeme +
                 "' has the wrong number of arguments.",
             "GTI-S2035");
      currentType = SemanticType::Unknown;
      return;
    }
    for (std::size_t index = 0; index < inputs.size(); ++index) {
      if (!callArgumentMatches(factory->parameterTypes[index], types[index],
                               *inputs[index])) {
        reportCallArgumentMismatch(index, factory->parameterTypes[index],
                                   types[index], *inputs[index], "Text-view");
        currentType = SemanticType::Unknown;
        return;
      }
    }
    validateCallPlaceExclusivityImpl(factory->parameterTypes, inputs, paren);
    recordResolvedCall(call, *factory, {}, {}, {}, {}, CallReceiverMode::None,
                       &member.object());
    currentType = callExpressionType(factory->returnType);
    return;
  }
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
      return accessible && !constructor->requiresUnsafe &&
             resolveDefaultConstruction(type) &&
             (!requestsStructuralDefaultConstruction(*constructor) ||
              classCanUseStructuralDefaultConstruction(type));
    }
    return classCanGenerateDefaultConstructor(type);
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

auto SemanticVisitor::Impl::resolveDefaultConstruction(const SemanticType &type,
                                                       bool baseSubobject) const
    -> std::optional<ResolvedConstructionInfo> {
  const ClassInfo *owner = classInfo(type);
  if (owner == nullptr || (owner->abstract && !baseSubobject)) {
    return std::nullopt;
  }

  const ConstructorInfo *constructor = defaultConstructor(*owner);
  if (constructor == nullptr) {
    if (!classCanGenerateDefaultConstructor(type)) {
      return std::nullopt;
    }
    return ResolvedConstructionInfo{.constructedType = type,
                                    .generatedDefault = true};
  }
  if (constructor->requiresUnsafe || !constructor->genericParameters.empty() ||
      (!(baseSubobject && constructor->access == AccessModifier::Protected) &&
       !canAccessMember(constructor->access, owner->id, type))) {
    return std::nullopt;
  }

  const GenericSubstitution substitution = classSubstitution(type);
  std::vector<AppliedConceptRequirement> requirements =
      defaultConstructorRequirements(*constructor, substitution);
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

auto SemanticVisitor::Impl::defaultConstructorRequirements(
    const ConstructorInfo &constructor,
    const GenericSubstitution &substitution) const
    -> std::vector<AppliedConceptRequirement> {
  std::vector<AppliedConceptRequirement> requirements =
      constructor.requirements;
  for (AppliedConceptRequirement &requirement : requirements) {
    for (SemanticType &argument : requirement.arguments) {
      argument = substituteType(argument, substitution);
    }
  }
  return requirements;
}

auto SemanticVisitor::Impl::declaredDefaultConstructorApplies(
    const SemanticType &type) const -> bool {
  const ClassInfo *owner = classInfo(type);
  const ConstructorInfo *constructor =
      owner == nullptr ? nullptr : defaultConstructor(*owner);
  // A constructor template's own parameters are never deduced by default
  // construction; its status is not a property of the class arguments.
  if (constructor == nullptr || !constructor->genericParameters.empty()) {
    return true;
  }
  return !firstRequirementFailure(
      defaultConstructorRequirements(*constructor, classSubstitution(type)));
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
  if (argument.genericValueExpression) {
    const ArrayExtentEvaluation evaluation =
        evaluateResolvedArrayExtent(*argument.genericValueExpression);
    if (evaluation.value) {
      return CompileTimeValue::uint64(*evaluation.value);
    }
    struct DependencyCheck {
      bool valid = false;
      bool dependent = false;
    };
    const auto dependency =
        [&](const auto &self,
            const ArrayExtentExpr &expression) -> DependencyCheck {
      if (expression.isAtom()) {
        if (std::holds_alternative<std::uint64_t>(expression.token.literal)) {
          return {.valid = true};
        }
        if (expression.token.kind != TokenKind::IDENTIFIER) {
          return {};
        }
        if (const std::optional<CompileTimeValue> parameter =
                resolveValueParameter(expression.token)) {
          return {.valid = true,
                  .dependent = parameter->kind == CompileTimeValue::Parameter};
        }
        const Symbol *symbol = resolve(expression.token);
        const ConstantValue *constant =
            symbol == nullptr ? nullptr : constantFor(*symbol);
        return {.valid = constant != nullptr &&
                         constantUnsignedMagnitude(*constant).has_value()};
      }
      if (!expression.left || !expression.right) {
        return {};
      }
      const DependencyCheck left = self(self, *expression.left);
      const DependencyCheck right = self(self, *expression.right);
      return {.valid = left.valid && right.valid,
              .dependent = left.dependent || right.dependent};
    };
    const DependencyCheck check =
        dependency(dependency, *argument.genericValueExpression);
    if (!check.valid || !check.dependent)
      return std::nullopt;
    const auto bind = [&](const auto &self, const ArrayExtentExpr &source)
        -> std::shared_ptr<const GenericValueExpression> {
      if (!source.isAtom())
        return std::make_shared<GenericValueExpression>(
            GenericValueExpression{.kind = GenericValueExpression::Binary,
                                   .token = source.token,
                                   .left = self(self, *source.left),
                                   .right = self(self, *source.right)});
      if (const auto *literal =
              std::get_if<std::uint64_t>(&source.token.literal))
        return std::make_shared<GenericValueExpression>(
            GenericValueExpression{.value = *literal});
      if (const auto parameter = resolveValueParameter(source.token)) {
        if (parameter->expression)
          return parameter->expression;
        return std::make_shared<GenericValueExpression>(GenericValueExpression{
            .kind = parameter->kind == CompileTimeValue::Parameter
                        ? GenericValueExpression::Parameter
                        : GenericValueExpression::UInt64,
            .value = parameter->value,
            .parameterId = parameter->parameterId});
      }
      const Symbol *symbol = resolve(source.token);
      const ConstantValue *constant =
          symbol == nullptr ? nullptr : constantFor(*symbol);
      return std::make_shared<GenericValueExpression>(GenericValueExpression{
          .value = *constantUnsignedMagnitude(*constant)});
    };
    return CompileTimeValue{.expression =
                                bind(bind, *argument.genericValueExpression)};
  }
  if (argument.name.segments.size() != 1 || !argument.arguments.empty() ||
      !argument.pointers.empty() || !argument.arrayExtents.empty() ||
      argument.reference) {
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

auto SemanticVisitor::Impl::reportGenericValueArgumentFailure(
    const TypeRef &argument, std::string_view context) -> bool {
  if (!argument.genericValueExpression) {
    return false;
  }
  const ArrayExtentEvaluation evaluation =
      evaluateResolvedArrayExtent(*argument.genericValueExpression);
  const Token &location =
      evaluation.token == nullptr ? argument.name.last() : *evaluation.token;
  if (evaluation.error == ArrayExtentEvaluationError::Overflow ||
      evaluation.error == ArrayExtentEvaluationError::Underflow) {
    report(location,
           std::string(context) + " arithmetic " +
               (evaluation.error == ArrayExtentEvaluationError::Underflow
                    ? "underflows"
                    : "overflows") +
               " uint64_t.",
           "GTI-S2026");
    return true;
  }
  if (evaluation.error == ArrayExtentEvaluationError::ZeroDivisor) {
    report(location,
           std::string(context) +
               " arithmetic cannot divide or take modulo by zero.",
           "GTI-S2026");
    return true;
  }
  return false;
}

auto SemanticVisitor::Impl::evaluateResolvedArrayExtent(
    const ArrayExtentExpr &expression) const -> ArrayExtentEvaluation {
  if (expression.isAtom()) {
    if (const auto *value =
            std::get_if<std::uint64_t>(&expression.token.literal)) {
      return {.value = *value};
    }
    if (expression.token.kind == TokenKind::IDENTIFIER) {
      if (const std::optional<CompileTimeValue> parameter =
              resolveValueParameter(expression.token);
          parameter && parameter->kind == CompileTimeValue::UInt64) {
        return {.value = parameter->value};
      }
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

auto SemanticVisitor::Impl::isWithinClass(ClassId owner) const -> bool {
  for (ClassId current = currentClass.value_or(0); current != 0;
       current = classInfo(current).enclosingClass) {
    if (current == owner)
      return true;
  }
  return false;
}

auto SemanticVisitor::Impl::memberDeclaringType(const SemanticType &type,
                                                ClassId owner) const
    -> SemanticType {
  std::vector<SemanticType> pending{type};
  std::vector<SemanticType> visited;
  while (!pending.empty()) {
    SemanticType current = pending.back();
    pending.pop_back();
    if (current.kind != SemanticType::Class || current.classId == 0 ||
        current.classId > classes.size() ||
        std::find(visited.begin(), visited.end(), current) != visited.end())
      continue;
    if (current.classId == owner)
      return current;
    visited.push_back(current);
    // Invalid inheritance cycles are diagnosed independently. Bound recovery
    // even when such a cycle transforms its generic arguments at each step.
    if (visited.size() > classes.size())
      return SemanticType::Unknown;
    const auto substitution = classSubstitution(current);
    for (const auto &base : classInfo(current.classId).bases)
      pending.push_back(substituteType(base.type, substitution));
  }
  return SemanticType::Unknown;
}

auto SemanticVisitor::Impl::canAccessMember(
    AccessModifier access, ClassId owner, const SemanticType &receiver,
    const SemanticType &lookupType) const -> bool {
  if (owner == 0 || access == AccessModifier::Public)
    return true;
  if (access == AccessModifier::Private)
    return isWithinClass(owner);
  const SemanticType *unwrapped = &receiver;
  while ((unwrapped->kind == SemanticType::Reference ||
          unwrapped->kind == SemanticType::RawPointer) &&
         unwrapped->arguments.size() == 1)
    unwrapped = &unwrapped->arguments.front();
  const SemanticType &object = *unwrapped;
  const SemanticType declaringType = memberDeclaringType(
      lookupType == SemanticType::Unknown ? object : lookupType, owner);
  for (ClassId context = currentClass.value_or(0); context != 0;
       context = classInfo(context).enclosingClass) {
    const SemanticType contextType = openClassType(context);
    const SemanticType contextOwner = memberDeclaringType(contextType, owner);
    if (contextOwner != SemanticType::Unknown &&
        (declaringType == SemanticType::Unknown ||
         declaringType == contextOwner) &&
        (context == owner || object == SemanticType::Unknown ||
         object == contextType || isPublicBaseConversion(object, contextType)))
      return true;
  }
  return false;
}

auto SemanticVisitor::Impl::validateNestedClassAccess(ClassId id,
                                                      const Token &use)
    -> bool {
  for (ClassId current = id; current != 0;
       current = classInfo(current).enclosingClass) {
    const ClassInfo &nested = classInfo(current);
    if (nested.enclosingClass != 0 &&
        !canAccessMember(nested.enclosingAccess, nested.enclosingClass)) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2007", DiagnosticPhase::Semantics, use,
          "Nested type '" + nested.name.lexeme + "' of '" +
              classInfo(nested.enclosingClass).name.lexeme + "' is " +
              std::string(accessModifierSpelling(nested.enclosingAccess)) +
              ".");
      diagnostic.related.push_back(
          {tokenSpan(nested.name), "Nested type is declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
      return false;
    }
  }
  return true;
}

auto SemanticVisitor::Impl::validateClassPathAliases(const NamePath &path,
                                                     const Token &use) -> bool {
  for (std::size_t count = 1; count < path.segments.size(); ++count) {
    NamePath prefix(std::vector<Token>(path.segments.begin(),
                                       path.segments.begin() + count));
    if (const auto alias = resolveTypeAliasPath(prefix, currentNamespace);
        alias && !validateTypeAliasAccess(*alias, use)) {
      return false;
    }
  }
  return true;
}

auto SemanticVisitor::Impl::validateTypeAliasAccess(
    const ResolvedTypeAlias &alias, const Token &use) -> bool {
  for (const ResolvedTypeAlias &qualifier : alias.qualifiers) {
    if (!validateTypeAliasAccess(qualifier, use)) {
      return false;
    }
  }
  if (alias.compilerPrivate && !isCompilerTrustedRequester(use)) {
    reportCompilerPrivateAccess(use, alias.qualifiedName);
    return false;
  }
  if (alias.ownerClass != 0 &&
      !canAccessMember(alias.access, alias.ownerClass, SemanticType::Unknown,
                       alias.ownerType)) {
    const ClassInfo &owner = classInfo(alias.ownerClass);
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2007", DiagnosticPhase::Semantics, use,
        "Type alias '" + alias.declaration->name().lexeme + "' of '" +
            owner.name.lexeme + "' is " +
            std::string(accessModifierSpelling(alias.access)) + ".");
    diagnostic.related.push_back(
        {tokenSpan(alias.declaration->name()), "Type alias is declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
    return false;
  }
  if (alias.ownerClass != 0 && alias.ownerClass <= classes.size() &&
      !classInfo(alias.ownerClass).genericParameters.empty() &&
      !alias.specializedOwner && currentClass != alias.ownerClass) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2030", DiagnosticPhase::Semantics, use,
        "Type alias '" + alias.qualifiedName +
            "' belongs to a generic class; GTI cannot name a member through "
            "an unspecialized generic class path.");
    diagnostic.related.push_back({tokenSpan(classInfo(alias.ownerClass).name),
                                  "Generic owning type is declared here."});
    diagnostic.hints.emplace_back(
        "Write a specialized owning type before '::' (or a named alias to "
        "it), or use the alias inside its declaring generic class.");
    diagnostics.emplace_back(std::move(diagnostic));
    return false;
  }
  return true;
}

auto SemanticVisitor::Impl::validateType(const TypeRef &type,
                                         bool allowNativeFunctionType) -> void {
  const std::size_t diagnosticStart = diagnostics.size();
  if (type.referenceMutability && !type.reference) {
    report(*type.referenceMutability,
           "Mutable type access requires a checked reference; use 'mut T&'.",
           "GTI-S2030");
    return;
  }
  if (type.qualifiedOwner) {
    if (const std::optional<ConceptId> protocol =
            protocolConceptOwner(*type.qualifiedOwner, currentNamespace)) {
      validateAssociatedTypePath(type, *protocol);
      return;
    }
    validateType(*type.qualifiedOwner);
    if (diagnostics.size() != diagnosticStart) {
      return;
    }
    const SemanticType owner = typeOf(*type.qualifiedOwner);
    if (owner.kind != SemanticType::Class) {
      report(type.qualifiedOwner->name.last(),
             "A qualified type member requires a class-like owner, but '" +
                 typeRefSpelling(*type.qualifiedOwner) + "' is not one.",
             "GTI-S2030");
      return;
    }
    if (type.name.last().completion) {
      captureQualifiedCompletion(type.name, type.qualifiedOwner.get(), true);
      return;
    }
    if (!resolveTypeAliasPath(type, currentNamespace)) {
      report(type.name.last(),
             "Type '" + typeSpelling(owner) + "' has no direct type alias '" +
                 type.name.last().lexeme + "'.",
             "GTI-S2030");
      return;
    }
  }
  if (type.name.last().completion) {
    if (type.name.segments.size() == 1) {
      captureUnqualifiedCompletion(type.name.last());
    } else {
      captureQualifiedCompletion(type.name);
    }
    return;
  }
  if (type.packExpansion &&
      (!allowPackTypeReference || !isActiveTypePack(type) ||
       !type.arguments.empty() || !type.pointers.empty() || type.reference ||
       !type.arrayExtents.empty())) {
    report(*type.packExpansion,
           "A type-list expansion must name one bare type pack in the final "
           "signature parameter or class type-pack argument position.",
           "GTI-S2023");
    return;
  }
  if (type.isFunctionSignature()) {
    const std::size_t count = *type.functionSignatureParameterCount;
    if (type.arguments.size() != count + 1) {
      report(type.name.last(), "Malformed function signature type.",
             "GTI-S2093");
      return;
    }
    if (!type.pointers.empty() || type.reference ||
        !type.arrayExtents.empty()) {
      report(type.name.last(),
             "A function signature has no object storage; use it as an "
             "alias target or a generic type argument.",
             "GTI-S2093");
    }
    for (std::size_t index = 0; index <= count; ++index) {
      const TypeRef &component = type.arguments[index];
      if (component.referenceMutability && !component.reference) {
        report(
            *component.referenceMutability,
            "A mutable signature parameter must be a reference; use 'mut T&'.",
            "GTI-S2093");
      }
      const bool enclosingAllowPack = allowPackTypeReference;
      allowPackTypeReference = component.packExpansion && index + 1 == count;
      validateType(component);
      allowPackTypeReference = enclosingAllowPack;
      const SemanticType resolved = typeOf(component);
      if (component.packExpansion && resolved.kind == SemanticType::TypePack) {
        for (const SemanticType &element : resolved.arguments) {
          if (element == SemanticType::Void ||
              element.kind == SemanticType::FunctionSignature) {
            report(component.name.last(),
                   "Function signature parameter packs must expand to value "
                   "or reference types, not void or function signatures.",
                   "GTI-S2093");
          }
          validateReferenceReferent(element, component.name.last());
        }
        continue;
      }
      if (resolved.kind == SemanticType::FunctionSignature ||
          (index < count && resolved == SemanticType::Void) ||
          (index == count && resolved.kind == SemanticType::Array)) {
        report(component.name.last(),
               index < count
                   ? "Function signature parameters must be value or reference "
                     "types; use '()' for an empty parameter list."
                   : "A function signature result must be void, a value, or "
                     "a reference, not an array or another function signature.",
               "GTI-S2093");
      } else {
        validateReferencePlacement(component, true,
                                   "function signature component", false, true);
      }
    }
    return;
  }
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
    if (!type.pointers.empty() || !type.arrayExtents.empty() ||
        type.reference) {
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
             "recursive pointer to an admitted C ABI pointee.",
             "GTI-S2076");
    }
    const auto parameters = resolved.nativeFunctionParameterTypes();
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      if (!nativeFunctionBoundaryTypeAllowed(parameters[index], false)) {
        report(
            type.arguments[index].name.last(),
            "A native function parameter must be an immutable "
            "fixed-width C ABI value, c_string, a passive [[c_abi]] "
            "record, or a recursive raw pointer to an admitted C ABI pointee.",
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
  if (baseTypeOf(type, currentNamespace).kind ==
          SemanticType::FunctionSignature &&
      (!type.pointers.empty() || type.reference ||
       !type.arrayExtents.empty())) {
    report(type.name.last(),
           "A function signature has no object storage; pointers, references, "
           "and arrays of signatures are not supported.",
           "GTI-S2093");
  }
  if (!type.pointers.empty()) {
    const SemanticType pointee = baseTypeOf(type, currentNamespace);
    if (type.name.last().kind == TokenKind::AUTO ||
        pointee == SemanticType::NullPtr ||
        pointee.kind == SemanticType::TypeName ||
        pointee.kind == SemanticType::Function ||
        pointee.kind == SemanticType::TypePack ||
        pointee.kind == SemanticType::Array) {
      report(type.pointers.front().star,
             pointee.kind == SemanticType::Array
                 ? "Pointer-to-array types are not supported in the "
                   "initial raw-pointer model."
                 : "A raw pointer requires a concrete object type or void "
                   "pointee.",
             "GTI-S2056");
    }
  }
  if (type.pointers.empty()) {
    const SemanticType direct = baseTypeOf(type, currentNamespace);
    if (direct.kind == SemanticType::Class && direct.classId != 0 &&
        direct.classId <= classes.size() &&
        classInfo(direct.classId).cOpaqueHandle) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2065", DiagnosticPhase::Semantics, type.name.last(),
          "Opaque C handle type '" + typeSpelling(direct) +
              "' is incomplete and may be used only behind raw "
              "pointers.");
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
           "parameter-pack declaration or as an explicit final type-list "
           "expansion in a signature or class type pack.",
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
  if (compilerCapability == CompilerCapabilityTypeKind::SharedOwner) {
    if (type.arguments.size() != 1 ||
        type.arguments.front().genericArgumentSyntax ==
            GenericArgumentSyntax::Value) {
      report(type.name.last(),
             "Compiler-private shared ownership requires one object type.",
             "GTI-S2018");
      return;
    }
    validateType(type.arguments.front());
    const SemanticType element = typeOf(type.arguments.front());
    if (element.kind != SemanticType::TypeParameter &&
        ((!isAllocatedScalarType(element) &&
          element.kind != SemanticType::Class) ||
         typeTraits(element).containsBorrowedState)) {
      report(type.arguments.front().name.last(),
             "Shared ownership currently requires a class, struct, or scalar "
             "object type without borrowed state.",
             "GTI-S2018");
    }
    return;
  }
  if (compilerCapability == CompilerCapabilityTypeKind::UninitializedStorage) {
    if (type.arguments.size() != 1 ||
        type.arguments.front().genericArgumentSyntax ==
            GenericArgumentSyntax::Value) {
      report(type.name.last(),
             "Uninitialized storage requires exactly one complete object type.",
             "GTI-S2019");
      return;
    }
    validateType(type.arguments.front());
    const auto payload = typeOf(type.arguments.front());
    if (payload != SemanticType::Unknown &&
        !containsOpenTypeParameter(payload) &&
        (!runtimeObjectLayoutType(payload) ||
         typeTraits(payload).containsBorrowedState)) {
      report(type.arguments.front().name.last(),
             "Uninitialized storage requires a complete object type without "
             "checked borrowed state.",
             "GTI-S2019");
    }
  }
  if (compilerCapability == CompilerCapabilityTypeKind::PolymorphicStorage) {
    if (type.arguments.size() != 3 ||
        type.arguments.front().genericArgumentSyntax ==
            GenericArgumentSyntax::Value) {
      report(type.name.last(),
             "Compiler-private polymorphic storage requires one interface "
             "type, an inline byte capacity, and an inline alignment.",
             "GTI-S2019");
      return;
    }
    validateType(type.arguments.front());
    const SemanticType interface = typeOf(type.arguments.front());
    const ClassInfo *base =
        interface.kind == SemanticType::Class ? classInfo(interface) : nullptr;
    if (interface != SemanticType::Unknown &&
        interface.kind != SemanticType::TypeParameter &&
        (base == nullptr || !base->polymorphic)) {
      report(type.arguments.front().name.last(),
             "Polymorphic storage requires a polymorphic class or interface, "
             "not '" +
                 typeSpelling(interface) + "'.",
             "GTI-S2019");
    }
    const SemanticType storage = typeOf(type);
    if (storage.valueArguments.size() == 2) {
      const CompileTimeValue &bytes = storage.valueArguments[0];
      const CompileTimeValue &alignment = storage.valueArguments[1];
      if ((bytes.kind == CompileTimeValue::UInt64 &&
           (bytes.value == 0 || bytes.value > 4096)) ||
          (alignment.kind == CompileTimeValue::UInt64 &&
           (alignment.value == 0 || alignment.value > 4096 ||
            (alignment.value & (alignment.value - 1)) != 0))) {
        report(type.name.last(),
               "Polymorphic storage requires an inline capacity from 1 to "
               "4096 bytes and a power-of-two alignment no greater than "
               "4096.",
               "GTI-S2019");
      }
    }
    if (!type.arrayExtents.empty()) {
      report(type.name.last(),
             "Fixed arrays of compiler-private storage are not supported.",
             "GTI-S2019");
    }
    // Ordinary nominal argument validation below checks the value kinds.
  }
  if (compilerCapability == CompilerCapabilityTypeKind::OptionalStorage) {
    if (type.arguments.size() != 1 ||
        type.arguments.front().genericArgumentSyntax ==
            GenericArgumentSyntax::Value) {
      report(type.name.last(),
             "Compiler-private optional storage requires exactly one payload "
             "type.",
             "GTI-S2019");
      return;
    }
    validateType(type.arguments.front());
    const SemanticType payload = typeOf(type.arguments.front());
    if (payload != SemanticType::Unknown &&
        payload.kind != SemanticType::TypeParameter) {
      const auto traits = typeTraits(payload);
      if (payload == SemanticType::Void ||
          payload.kind == SemanticType::Reference ||
          payload.kind == SemanticType::RawPointer || !traits.movable ||
          traits.containsBorrowedState) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2019", DiagnosticPhase::Semantics,
            type.arguments.front().name.last(),
            "Optional storage payload '" + typeSpelling(payload) +
                "' must be a move-constructible value without borrowed state; "
                "void, reference, and raw-pointer payloads are not supported.");
        diagnostic.hints.push_back(
            "Use an owning payload. Optional storage does not retain "
            "dependencies on an external borrowed owner.");
        diagnostics.emplace_back(std::move(diagnostic));
      }
    }
    // Retain ordinary nominal argument/constraint validation below. Symbolic
    // payloads are checked again under concrete generic substitution.
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
  if (compilerCapability == CompilerCapabilityTypeKind::NativeCScalar) {
    if (!type.arguments.empty()) {
      report(type.name.last(),
             "A native C scalar type does not take generic arguments.",
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
  if (compilerCapability == CompilerCapabilityTypeKind::AtomicStorage) {
    if (type.arguments.size() != 1) {
      report(type.name.last(),
             "gti_internal::atomic_storage<T> requires exactly one scalar "
             "value type.",
             "GTI-S2086");
    } else {
      if (type.arguments.front().genericArgumentSyntax ==
          GenericArgumentSyntax::Value) {
        report(type.arguments.front().name.last(),
               "gti_internal::atomic_storage requires a type argument.",
               "GTI-S2086");
        return;
      }
      validateType(type.arguments.front());
      const SemanticType valueType = typeOf(type.arguments.front());
      if (!satisfiesConstraint(
              valueType, constraintBit(GenericConstraintKind::AtomicScalar))) {
        report(type.arguments.front().name.last(),
               "Compiler-private atomic storage supports bool and the "
               "fixed-width signed and unsigned integer types only.",
               "GTI-S2086");
      }
    }
    if (!type.arrayExtents.empty()) {
      report(type.name.last(),
             "Fixed arrays of compiler-private atomic storage are not "
             "supported.",
             "GTI-S2086");
    }
    return;
  }
  if (compilerCapability == CompilerCapabilityTypeKind::MutexStorage ||
      compilerCapability == CompilerCapabilityTypeKind::MutexLockToken) {
    const bool storage =
        compilerCapability == CompilerCapabilityTypeKind::MutexStorage;
    const std::string_view name = storage ? "gti_internal::mutex_storage"
                                          : "gti_internal::mutex_lock_token";
    if (type.arguments.size() != 1) {
      report(type.name.last(),
             std::string(name) + "<T> requires exactly one movable value "
                                 "type.",
             "GTI-S2088");
    } else {
      if (type.arguments.front().genericArgumentSyntax ==
          GenericArgumentSyntax::Value) {
        report(type.arguments.front().name.last(),
               std::string(name) + " requires a type argument.", "GTI-S2088");
        return;
      }
      validateType(type.arguments.front());
      const SemanticType valueType = typeOf(type.arguments.front());
      if (!satisfiesConstraint(valueType,
                               constraintBit(GenericConstraintKind::Movable)) ||
          valueType == SemanticType::Void ||
          valueType.kind == SemanticType::Reference ||
          valueType.kind == SemanticType::RawPointer) {
        report(type.arguments.front().name.last(),
               "Compiler-private mutex state requires an owned movable "
               "value type.",
               "GTI-S2088");
      }
    }
    if (!type.arrayExtents.empty()) {
      report(type.name.last(),
             "Fixed arrays of compiler-private mutex state are not "
             "supported.",
             "GTI-S2088");
    }
    return;
  }
  if (!type.arrayExtents.empty() && type.pointers.empty() &&
      baseTypeOf(type, currentNamespace) == SemanticType::Void) {
    // A pointer declarator makes the element `void*`, a sized pointer
    // scalar; only a bare `void` element has no object representation.
    report(type.name.last(), "Fixed array elements cannot have type void.",
           "GTI-S2015");
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
  if (type.name.segments.size() == 1 &&
      blockAssociatedType(type.name.last().lexeme, currentNamespace)) {
    if (!type.arguments.empty()) {
      report(type.name.last(), "An associated type takes no generic arguments.",
             "GTI-S2123");
    }
    return;
  }

  if (const std::optional<ResolvedTypeAlias> alias =
          resolveTypeAliasPath(type, currentNamespace)) {
    (void)validateTypeAliasAccess(*alias, type.name.last());
    validateConcreteEnumPayloads(typeOf(type), type.name.last());
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

  if (const auto id = resolveEnumPath(type.name, currentNamespace)) {
    if (*id == 0 || *id > enums.size())
      return;
    if (type.arguments.size() < enums[*id - 1].genericParameters.size())
      resolveEnumGenericDefaults(*id);
    const auto &owner = enums[*id - 1];
    const auto resolved = resolveNominalArguments(
        owner.genericParameters, owner.name, type.arguments, type.name.last(),
        owner.genericParameters.empty() ? "GTI-S2036" : "GTI-S2067");
    if (!resolved.valid)
      return;
    const auto concrete =
        SemanticType::enumType(*id, resolved.types, resolved.values);
    validateConcreteEnumPayloads(concrete, type.name.last());
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
    const std::string spelling = pathSpelling(type.name);
    report(type.name.last(), "Unknown type '" + spelling + "'.");
    diagnostics.back().missingIncludeName = type.name.last().lexeme;
    return;
  }
  if (type.arguments.size() < classInfo(*classId).genericParameters.size())
    resolveClassGenericDefaults(*classId);
  const ClassInfo &declaration = classInfo(*classId);
  if (declaration.compilerType == CompilerTypeKind::MetaCallableReturnType &&
      type.arguments.size() == 1) {
    if (callableReturnSubjectUnsupported(typeOf(type.arguments.front()))) {
      report(type.name.last(),
             "Callable return-type reflection requires a function signature, "
             "a selected function item, a native function pointer, or a lambda "
             "type; it does not select an overloaded operator().",
             "GTI-S2023");
      return;
    }
  }
  if (!validateClassPathAliases(type.name, type.name.last()))
    return;
  if (!validateNestedClassAccess(*classId, type.name.last()))
    return;
  if (!declaration.declaration->genericArgumentPatterns().empty() ||
      !declaration.definitionFamilyMembers.empty() ||
      declaration.definitionFamilyPrimary != 0 ||
      (!declaration.genericParameters.empty() &&
       declaration.genericParameters.back().pack)) {
    (void)resolveClassArguments(*classId, type.arguments, type.name.last(),
                                "GTI-S2023");
    return;
  }
  const std::size_t expectedArguments = declaration.genericParameters.size();
  const std::size_t requiredArguments = static_cast<std::size_t>(
      std::count_if(declaration.genericParameters.begin(),
                    declaration.genericParameters.end(),
                    [](const GenericParameterInfo &parameter) {
                      return parameter.value ? !parameter.defaultValue
                                             : !parameter.defaultType;
                    }));
  if (type.arguments.size() < requiredArguments ||
      type.arguments.size() > expectedArguments) {
    const bool typeOnly =
        genericValueParameterCount(declaration.genericParameters) == 0;
    const std::string arity =
        requiredArguments == expectedArguments
            ? std::to_string(expectedArguments)
            : "between " + std::to_string(requiredArguments) + " and " +
                  std::to_string(expectedArguments);
    report(type.name.last(),
           "Type '" + pathSpelling(type.name) + "' requires " + arity +
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
        if (!reportGenericValueArgumentFailure(
                argument,
                "Generic value argument for '" + parameter.name.lexeme + "'")) {
          report(argument.name.last(),
                 "Generic parameter '" + parameter.name.lexeme +
                     "' requires a uint64_t compile-time value.",
                 "GTI-S2026");
        }
      } else if (argument.genericValueExpression) {
        recordArrayExtentUses(*argument.genericValueExpression);
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
    const bool enclosingAllowPack = allowPackTypeReference;
    allowPackTypeReference = false;
    validateType(argument);
    allowPackTypeReference = enclosingAllowPack;
    const SemanticType argumentType = typeOf(argument);
    if (argumentType.kind == SemanticType::Reference) {
      validateReferencePlacement(argument, true, "generic type argument", false,
                                 true);
    }
    if (!(resolvingGenericTypeDefault &&
          containsOpenTypeParameter(argumentType))) {
      if (const auto failure =
              firstConstraintFailure({parameter}, {argumentType})) {
        reportConstraintFailure(argument.name.last(), *failure);
      }
    }
  }
  for (std::size_t index = count; index < type.arguments.size(); ++index) {
    const TypeRef &argument = type.arguments[index];
    if (argument.genericArgumentSyntax != GenericArgumentSyntax::Value) {
      validateType(argument);
    }
  }
  if (type.arguments.size() < expectedArguments &&
      diagnostics.size() == diagnosticStart) {
    if (const auto failure = firstNestedTypeConstraintFailure(typeOf(type))) {
      reportConstraintFailure(type.name.last(), *failure);
    }
  }
  if (catalog.sealed)
    validateOrdinaryClassLayout(typeOf(type));
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

auto SemanticVisitor::Impl::isGtiInternalTextView(const TypeRef &type) const
    -> bool {
  return compilerCapabilityType(type) == CompilerCapabilityTypeKind::TextView;
}

auto SemanticVisitor::Impl::containsReference(const TypeRef &type) const
    -> bool {
  const auto contains = [&](const auto &self,
                            const SemanticType &resolved) -> bool {
    if (resolved.kind == SemanticType::Reference) {
      return true;
    }
    if (resolved.kind == SemanticType::Class ||
        resolved.kind == SemanticType::Enum ||
        resolved.kind == SemanticType::FunctionSignature) {
      // Nominal arguments and signature components are metadata. Actual
      // nominal fields are checked under their concrete substitution.
      return false;
    }
    return std::any_of(
        resolved.arguments.begin(), resolved.arguments.end(),
        [&](const SemanticType &argument) { return self(self, argument); });
  };
  return contains(contains, typeOf(type));
}

auto SemanticVisitor::Impl::containsRawPointer(const TypeRef &type) -> bool {
  if (!type.pointers.empty()) {
    return true;
  }
  return std::any_of(
      type.arguments.begin(), type.arguments.end(),
      [](const TypeRef &argument) { return containsRawPointer(argument); });
}

auto SemanticVisitor::Impl::isActiveTypePack(const TypeRef &type) const
    -> bool {
  return activeTypePackIdentity(type).has_value();
}

auto SemanticVisitor::Impl::activeTypePackIdentity(const TypeRef &type) const
    -> std::optional<GenericParameterId> {
  const std::optional<SemanticType> parameter = resolveTypeParameter(type.name);
  if (!parameter || (parameter->kind != SemanticType::TypeParameter &&
                     parameter->kind != SemanticType::TypePack)) {
    return std::nullopt;
  }
  // The nearest lexical declaration owns whether a name is a pack; its
  // substituted value may retain another declaration's canonical pack ID.
  // Keep those identities separate, and do not see through a non-pack shadow.
  const std::string &name = type.name.last().lexeme;
  for (std::size_t index = typeParameterScopes.size(); index != 0; --index) {
    if (typeParameterScopes[index - 1].contains(name)) {
      const auto found = typePackScopes[index - 1].find(name);
      return found == typePackScopes[index - 1].end()
                 ? std::nullopt
                 : std::optional<GenericParameterId>(found->second);
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::validateReferencePlacement(
    const TypeRef &type, bool allowTopLevel, std::string_view context,
    bool allowRvalueReference, bool typeMetadata, bool genericFunctionParameter,
    bool contractedReferenceResult) -> void {
  const SemanticType resolved = typeOf(type);
  if (resolved.kind == SemanticType::FunctionSignature) {
    report(type.name.last(),
           "A function signature cannot be used as a " + std::string(context) +
               "; use it as an alias target or a generic type argument.",
           "GTI-S2093");
    return;
  }
  if (resolved.kind == SemanticType::Reference && !type.reference &&
      !typeMetadata && !contractedReferenceResult) {
    report(type.name.last(),
           "A reference type substituted into a " + std::string(context) +
               " requires an explicit reference declarator; keep the generic "
               "argument as the referent type and write '&' at the binding.",
           "GTI-S2017");
  }
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
  }
  // Range& parameters already support concrete fixed-array range instances.
  // This is a declaration-bound generic substitution, not permission to spell
  // array references as storage, aliases, returns, or signature metadata.
  const auto genericReferent =
      genericFunctionParameter ? resolveTypeParameter(type.name) : std::nullopt;
  const bool substitutedArrayParameter =
      !typeMetadata && allowTopLevel && type.reference &&
      type.arrayExtents.empty() && genericReferent &&
      genericReferent->kind == SemanticType::Array &&
      resolved.kind == SemanticType::Reference &&
      resolved.arguments.size() == 1 &&
      resolved.arguments.front() == *genericReferent;
  if (!substitutedArrayParameter)
    validateReferenceReferent(resolved, type.reference ? *type.reference
                                                       : type.name.last());
  const auto nominalKind = baseTypeOf(type, currentNamespace).kind;
  // A checked projected reference result is the resolved referent, not a
  // container for the syntactic projection operands. A callable operand may
  // itself mention reference parameters/results without storing any of them.
  // The ordinary referent validation above and exact borrowed_from proof
  // still apply; uncontracted bindings and parameters retain their rules.
  const bool projectedReferenceResult =
      contractedReferenceResult && resolved.kind == SemanticType::Reference &&
      !type.reference;
  if (!projectedReferenceResult && nominalKind != SemanticType::Class &&
      nominalKind != SemanticType::Enum) {
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      const TypeRef &argument = type.arguments[index];
      if (containsReference(argument)) {
        report(argument.reference ? *argument.reference : argument.name.last(),
               "References cannot be nested inside another type yet.",
               "GTI-S2017");
      }
    }
  }
}

auto SemanticVisitor::Impl::validateReferenceReferent(const SemanticType &type,
                                                      const Token &site)
    -> void {
  if (type.kind != SemanticType::Reference || type.arguments.size() != 1) {
    return;
  }
  const SemanticType &referencedType = type.arguments.front();
  if (referencedType == SemanticType::Void) {
    report(site, "References cannot refer to void.", "GTI-S2017");
  }
  if (referencedType.kind == SemanticType::Array) {
    report(site, "References to fixed arrays are not supported yet.",
           "GTI-S2017");
  }
  if (referencedType.kind == SemanticType::Reference) {
    report(site, "References cannot refer to another reference.", "GTI-S2017");
  }
  if (isDirectOwnerType(referencedType)) {
    report(site,
           "References to compiler-private unique owners are not "
           "supported; borrow the owned value instead.",
           "GTI-S2018");
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
  if (!validateForeignStorageBorrow(initializer) ||
      !validateNativePointerSlotBorrow(initializer, reference)) {
    return;
  }
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
  if (!expressionNeverReturns(initializer.get()) &&
      !hasStableBorrowStorage(initializer)) {
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
    const bool carrierShell =
        sourceLoan != nullptr &&
        sourceLoan->kind == SemanticLoanKind::StoredValue &&
        (typeTraits(referent).containsBorrowedState ||
         receiverStorageBorrow(initializer));
    const AccessMode sourceAccess =
        source && source->dependency ? source->dependency->access
        : sourceLoan != nullptr      ? sourceLoan->access
                                     : AccessMode::ReadOnly;
    if (sourceLoan != nullptr && !carrierShell &&
        sourceAccess == AccessMode::ReadOnly) {
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
  const auto *initializerInfo =
      initializer ? semanticModel.findExpression(*initializer) : nullptr;
  const bool textDependentValue =
      (type.kind == SemanticType::Class || type.kind == SemanticType::Enum) &&
      initializerInfo != nullptr &&
      (isTextDependentValue(*initializerInfo) ||
       contextualTextValue(*initializer, type));
  if (!isDirectStoredReferenceType(type) &&
      type.kind != SemanticType::StringView &&
      type.kind != SemanticType::CString && !textDependentValue) {
    report(expressionToken(initializer),
           "Borrowed state cannot be nested inside another stored value in "
           "the current lifetime model.",
           "GTI-S2045");
    return;
  }
  if (textDependentValue) {
    // A dependent text value is a by-value descriptor: its own storage needs
    // no stability, but each owner it designates must be a checked place.
    const auto dependencies = storedBorrowDependencies(initializer);
    if (dependencies.empty() ||
        std::any_of(dependencies.begin(), dependencies.end(),
                    [](const SemanticLoanDependency &dependency) {
                      return dependency.place.root == 0 &&
                             !dependency.place.receiver;
                    }))
      report(expressionToken(initializer),
             "A value holding dynamic text must name its text's owner.",
             "GTI-S2045");
    return;
  }
  // A complete producer fact can prove that this value has no external
  // owners even when other alternatives of its type store several borrows.
  // Absence of evidence is not an empty set: check the concrete alternative
  // inventory and every applicable reference path before accepting it.
  if (const auto complete = completeBorrowDependencySet(initializer);
      complete && complete->dependencies.empty() &&
      !complete->possibleAlternatives.empty()) {
    const auto inventory = storedReferenceInventory(type);
    const auto alternatives = storedBorrowAlternativeInventory(type);
    bool ownerless = inventory && alternatives;
    if (ownerless)
      for (const auto &alternative : complete->possibleAlternatives) {
        if (std::find(alternatives->begin(), alternatives->end(),
                      alternative) == alternatives->end()) {
          ownerless = false;
          break;
        }
        for (const auto &path : *inventory) {
          const auto conditions = storedConditionSymbols(type, path.conditions);
          if (!conditions ||
              std::all_of(conditions->begin(), conditions->end(),
                          [&](const auto &condition) {
                            return std::find(alternative.conditions.begin(),
                                             alternative.conditions.end(),
                                             condition) !=
                                   alternative.conditions.end();
                          })) {
            ownerless = false;
            break;
          }
        }
        if (!ownerless)
          break;
      }
    if (ownerless)
      return;
  }
  if (storedReferencePaths(type).size() > 1) {
    const auto dependencies = storedBorrowDependencies(initializer);
    if (dependencies.empty()) {
      report(expressionToken(initializer),
             "A composed borrowed value requires a checked owner for every "
             "stored dependency.",
             "GTI-S2045");
      return;
    }
    // A known entry carrier protects caller storage without treating its own
    // by-value shell as an owner; therefore protectsStorage is not itself a
    // validity test. Every dependency must still have a stable named root.
    if (std::any_of(dependencies.begin(), dependencies.end(),
                    [](const SemanticLoanDependency &dependency) {
                      return dependency.place.root == 0 &&
                             !dependency.place.receiver;
                    })) {
      report(expressionToken(initializer),
             "A composed borrowed value cannot retain temporary owner "
             "storage.",
             "GTI-S2045");
      return;
    }
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
  const auto *info = value ? semanticModel.findExpression(*value) : nullptr;
  const Expr *returned = value.get();
  while (const auto *group = exprAs<Grouping>(returned))
    returned = group->expression().get();
  const bool returnsCarrier = source && symbol != nullptr && loan != nullptr &&
                              loan->lifecycleBound && info != nullptr &&
                              info->type == symbol->type &&
                              currentReturnType == symbol->type &&
                              lifecycleBoundBorrowCarrierFor(info->type) &&
                              (source->transfer || returned == source->carrier);
  std::vector<SemanticLoanId> pending{loan == nullptr ? 0 : loan->id};
  std::unordered_set<SemanticLoanId> visited;
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    if (id == 0 || !visited.insert(id).second)
      continue;
    const auto *retained = semanticModel.findLoan(id);
    if (retained == nullptr)
      continue;
    // Publishing the complete value carries its own cleanup obligation (and
    // remains subject to ordinary copy/move checks). It does not carry the
    // obligations of separate local objects that the value merely references.
    if (retained->lifecycleBound &&
        !(returnsCarrier && retained->id == loan->id)) {
      const Symbol *cleanupCarrier = nullptr;
      for (const auto &scope : scopes)
        for (const auto &[_, candidate] : scope)
          if ((candidate.bindingKind == SemanticBindingKind::LocalVariable ||
               candidate.bindingKind == SemanticBindingKind::Parameter) &&
              candidate.type.kind != SemanticType::Reference &&
              std::find(retained->carriers.begin(), retained->carriers.end(),
                        toolingSymbolFor(candidate)) !=
                  retained->carriers.end())
            cleanupCarrier = &candidate;
      if (cleanupCarrier != nullptr) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2017", DiagnosticPhase::Semantics, expressionToken(value),
            "A borrow retained by a cleanup-bound carrier cannot escape "
            "through " +
                std::string(destination) + ".");
        diagnostic.hints.emplace_back(
            "Keep the borrow within the carrier's lifetime, or return an owned "
            "value. The carrier's cleanup must complete before its scope "
            "exits.");
        diagnostic.related.push_back(
            {tokenSpan(cleanupCarrier->declaration),
             "The cleanup-bound carrier is declared here."});
        diagnostics.emplace_back(std::move(diagnostic));
        return true;
      }
    }
    pending.push_back(retained->parent);
    for (const auto &dependency : retained->additionalDependencies)
      pending.push_back(dependency.parent);
    pending.push_back(retained->storedDependency);
  }
  if (returnsCarrier)
    return false;
  if (symbol == nullptr ||
      symbol->bindingKind != SemanticBindingKind::LocalVariable ||
      loan == nullptr || (loan->parent == 0 && !loan->lifecycleBound)) {
    return false;
  }
  // Returning a restored alias publishes the caller-owned referent, not the
  // local reference slot. Retain the exact incoming witness: neither unsafe
  // restoration nor the helper's signature permits another child to escape.
  const auto callerOwnedRestoration = [&]() {
    const auto *function = currentFunctionInfo();
    const auto *parent = semanticModel.findLoan(loan->parent);
    if (returned != source->carrier || source->transfer || source->dependency ||
        symbol->type.kind != SemanticType::Reference ||
        symbol->type != currentReturnType ||
        symbol->variableDeclaration == nullptr ||
        !symbol->variableDeclaration->initializer() ||
        loan->kind != SemanticLoanKind::Reference || loan->entry ||
        loan->lifecycleBound || !loan->additionalDependencies.empty() ||
        !loan->designatedFields.empty() || parent == nullptr ||
        parent->kind != SemanticLoanKind::Reference || !parent->entry ||
        parent->parent != 0 || parent->storedDependency != 0 ||
        parent->lifecycleBound || !parent->additionalDependencies.empty() ||
        !parent->designatedFields.empty() ||
        loan->storedDependency != parent->id || loan->place != parent->place ||
        loan->access != currentReturnType.referenceAccess ||
        loan->access != parent->access || function == nullptr ||
        function->bodyDeclaration() == nullptr ||
        function->returnBorrowOrigin != BorrowOriginKind::Argument ||
        !function->returnBorrowParameters.empty() ||
        !function->returnBorrowFields.empty() || function->returnBorrowPlace ||
        function->returnBorrowParameter >= function->parameterTypes.size() ||
        function->returnBorrowParameter >=
            function->bodyDeclaration()->parameters().size())
      return false;
    const auto index = function->returnBorrowParameter;
    const auto *parameter = semanticModel.findBinding(
        function->bodyDeclaration()->parameters()[index]);
    if (function->parameterTypes[index].kind != SemanticType::Reference ||
        parameter == nullptr || parent->place.receiver ||
        !parent->place.projections.empty() ||
        parent->place.root != parameter->symbol)
      return false;
    const Expr *initializer = symbol->variableDeclaration->initializer().get();
    while (const auto *group = exprAs<Grouping>(initializer))
      initializer = group->expression().get();
    const auto *call = exprAs<Call>(initializer);
    const auto *resolved =
        call == nullptr ? nullptr : semanticModel.findCall(*call);
    if (resolved == nullptr || !resolved->restoresReferenceResult ||
        resolved->returnType != currentReturnType ||
        resolved->borrowOrigin != BorrowOriginKind::Argument ||
        !resolved->borrowArguments.empty() || !resolved->borrowFields.empty() ||
        resolved->borrowPlace)
      return false;
    const auto *argument =
        resolvedCallArgument(*call, resolved, resolved->borrowArgument);
    const Expr *witness = argument == nullptr ? nullptr : argument->get();
    while (const auto *group = exprAs<Grouping>(witness))
      witness = group->expression().get();
    return exprAs<Variable>(witness) != nullptr &&
           semanticModel.findResolvedSymbol(*witness) == parameter->symbol;
  };
  // A loan whose ancestry runs only through carrier-less transients (an
  // accessor's call result, a constructed temporary) up to an entry loan is
  // caller-owned authority relayed through this value, not a reborrow of a
  // local object: the referent outlives the body. A local carrier or a
  // cleanup-bound ancestor anywhere on that chain still blocks the escape.
  const auto callerOwnedAncestry = [&]() {
    std::unordered_set<SemanticLoanId> seen;
    for (SemanticLoanId current = loan->parent;
         current != 0 && seen.insert(current).second;) {
      const auto *ancestor = semanticModel.findLoan(current);
      if (ancestor == nullptr || ancestor->lifecycleBound)
        return false;
      if (ancestor->entry)
        return true;
      // A local carrier's stored-value loan names the external referent its
      // stored reference designates, not the carrier's own storage. A
      // reborrow through it relays that authority; the chain must still
      // reach caller-owned entry authority above.
      for (const auto &scope : scopes)
        for (const auto &[_, candidate] : scope)
          if (candidate.bindingKind == SemanticBindingKind::LocalVariable &&
              std::find(ancestor->carriers.begin(), ancestor->carriers.end(),
                        toolingSymbolFor(candidate)) !=
                  ancestor->carriers.end() &&
              !(ancestor->kind == SemanticLoanKind::StoredValue &&
                ancestor->place.root != toolingSymbolFor(candidate)))
            return false;
      current = ancestor->parent;
    }
    return false;
  };
  if (callerOwnedRestoration() || callerOwnedAncestry())
    return false;
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
  const auto inventory = storedReferenceInventory(returnType);
  const bool conditionalResult =
      inventory &&
      std::any_of(inventory->begin(), inventory->end(),
                  [](const auto &path) { return !path.conditions.empty(); });
  if (conditionalResult) {
    if (valueType != returnType)
      return;
    if (rejectEscapingLocalChildLoan(value, "a conditional borrowed return"))
      return;
    const auto complete = completeBorrowDependencySet(value);
    const auto *function =
        currentLambdaReturn ? nullptr : currentFunctionInfo();
    if (complete && function &&
        normalizedCompleteReturnBorrow(*function, *complete))
      return;
    report(expressionToken(value),
           "A borrowed return requires a complete dependency set preserving "
           "every caller-owned origin, access mode and active alternative.",
           "GTI-S2045");
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

auto SemanticVisitor::Impl::validateOwnerDependentTextReturn(
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
  const bool cString = info->type.kind == SemanticType::CString;
  const std::string description =
      cString ? "owner-dependent c_string" : "dynamic std::string_view";
  const std::string subject =
      cString ? "An " + description : "A " + description;
  // A formal's or the receiver's stored text field (L-OWN-02) returns under
  // the function's field-sourced result contract, which carries the field's
  // dependency to each caller. Without one, the function requires ownerless
  // text in that field; beside a relay the read then stays a
  // lifetime-independent alternative, as a literal is (PR #170).
  if (function != nullptr)
    if (const auto field = ownerlessTextForwardingField(value)) {
      const BorrowedFieldOrigin sourced{
          .origin = field->receiver ? BorrowOriginKind::Receiver
                                    : BorrowOriginKind::Argument,
          .parameter = field->receiver ? 0 : field->parameter,
          .sourceFields = {field->field}};
      if (function->returnBorrowFields.size() != 1 ||
          function->returnBorrowFields.front() != sourced)
        (void)requireOwnerlessTextField(function->declaration, *field);
      // Within the body the returned read is ownerless either way: callers
      // supply ownerless text there, or the contract relays theirs.
      for (const Expr *read = value.get(); read != nullptr;) {
        if (const ExpressionInfo *readInfo =
                semanticModel.findExpression(*read))
          semanticModel.record(*read, expressionInfo(readInfo->type,
                                                     readInfo->category,
                                                     readInfo->access));
        const auto *group = exprAs<Grouping>(read);
        const auto *move = exprAs<Call>(read);
        read = group != nullptr ? group->expression().get()
               : move != nullptr && move->arguments().size() == 1 &&
                       resolvedIntrinsicKind(*move) == IntrinsicKind::Move
                   ? move->arguments().front().get()
                   : nullptr;
      }
      return;
    }
  if (function == nullptr ||
      function->returnBorrowOrigin == BorrowOriginKind::None) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2045", DiagnosticPhase::Semantics, expressionToken(value),
        subject + " cannot escape without one exact owner dependency.");
    diagnostic.hints.emplace_back(
        cString
            ? "Return an ownerless native C string, or relay a C string tied "
              "to this method's receiver or one exact read-only parameter."
            : "Return a literal-backed view, or relay a view tied to this "
              "method's receiver or one exact read-only parameter.");
    diagnostics.emplace_back(std::move(diagnostic));
    return;
  }
  if (rejectEscapingLocalChildLoan(value, (cString ? "an " : "a ") +
                                              description + " return")) {
    return;
  }
  validateCurrentReturnBorrowSource(value, description, "GTI-S2045");
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
  // A typed terminal expression publishes no reference. Keep ordinary type,
  // category and access checks above, but there is no escaping loan to prove.
  if (expressionNeverReturns(value.get()))
    return;
  if (analyzeRawReferenceReturn(value)) {
    return;
  }
  if (!validateForeignStorageBorrow(value) ||
      !validateNativePointerSlotBorrow(value, reference)) {
    return;
  }
  if (isBorrowedCarrierReference(reference)) {
    const std::optional<ReturnBorrowContract> contract =
        currentReturnBorrowContract();
    const SemanticType *originType =
        contract ? contract->originParameterType() : nullptr;
    if (originType != nullptr && isDirectStoredReferenceType(*originType)) {
      // A by-value carrier can expose its exact external referent, never its
      // own shell. Keep the existing field-path proof (and precise S2085
      // diagnostics) authoritative for that distinct return family.
      validateCurrentReturnBorrowSource(value, "reference", "GTI-S2017");
      return;
    }
    // An external dependency does not extend the containing object's storage.
    // A copied local view can be returned by value, but never by reference to
    // its local shell, even if its contents borrow from this or a parameter.
    const SemanticLoanPlace shell = loanPlace(value);
    bool validShell = contract && shell.receiver &&
                      contract->origin == BorrowOriginKind::Receiver;
    if (const Parameter *origin =
            contract ? contract->originParameter() : nullptr) {
      const auto *binding = semanticModel.findBinding(*origin);
      validShell = binding != nullptr && originType != nullptr &&
                   originType->kind == SemanticType::Reference &&
                   !shell.receiver && shell.root == binding->symbol;
    }
    if (!validShell) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2017", DiagnosticPhase::Semantics, expressionToken(value),
          "A reference to a borrowed-state object must return storage owned by "
          "the receiver or the selected reference parameter, not a local or "
          "by-value object.");
      diagnostic.hints.emplace_back(
          "Return the borrowed-state object by value, or return a reference to "
          "an object whose storage outlives this call.");
      diagnostics.emplace_back(std::move(diagnostic));
      return;
    }
  }
  enum class EscapingMutexGuardReceiver { None, Local, Temporary };
  const auto escapingMutexGuardReceiver = [&]() {
    const Expr *candidate = value.get();
    while (const auto *group = exprAs<Grouping>(candidate)) {
      candidate = group->expression().get();
    }
    const auto *call = exprAs<Call>(candidate);
    const auto *member =
        call == nullptr ? nullptr : exprAs<Get>(call->callee().get());
    if (member == nullptr) {
      return EscapingMutexGuardReceiver::None;
    }
    const Expr *receiver = member->object().get();
    while (const auto *group = exprAs<Grouping>(receiver)) {
      receiver = group->expression().get();
    }
    const auto *temporary = exprAs<Call>(receiver);
    const auto *variable = exprAs<Variable>(receiver);
    const ExpressionInfo *receiverInfo =
        semanticModel.findExpression(*member->object());
    const ClassInfo *owner =
        receiverInfo == nullptr ? nullptr : classInfo(receiverInfo->type);
    if (owner == nullptr || !isDefaultLibraryMutexGuardCarrier(*owner)) {
      return EscapingMutexGuardReceiver::None;
    }
    if (temporary != nullptr) {
      return EscapingMutexGuardReceiver::Temporary;
    }
    const Symbol *symbol =
        variable == nullptr ? nullptr : resolve(variable->name());
    return symbol != nullptr &&
                   symbol->bindingKind == SemanticBindingKind::LocalVariable
               ? EscapingMutexGuardReceiver::Local
               : EscapingMutexGuardReceiver::None;
  }();
  if (escapingMutexGuardReceiver != EscapingMutexGuardReceiver::None) {
    const bool temporary =
        escapingMutexGuardReceiver == EscapingMutexGuardReceiver::Temporary;
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2017", DiagnosticPhase::Semantics, expressionToken(value),
        temporary
            ? "A reference protected by a temporary mutex guard cannot "
              "escape the guard's full expression."
            : "A reference protected by a local mutex guard cannot escape "
              "the guard's scope.");
    diagnostic.hints.emplace_back(
        "Bind the guard in a lexical scope and copy or move the protected "
        "value before that scope exits.");
    diagnostics.emplace_back(std::move(diagnostic));
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
  const std::optional<ReturnBorrowContract> contract =
      currentReturnBorrowContract();
  if (!contract || contract->origin == BorrowOriginKind::None) {
    return;
  }
  // Field-path contracts belong to function declarations; a lambda's
  // contract is one parameter.
  const FunctionInfo *function =
      contract->lambda ? nullptr : currentFunctionInfo();
  const ExpressionInfo *valueInfo =
      value == nullptr ? nullptr : semanticModel.findExpression(*value);
  if (valueInfo == nullptr || valueInfo->type == SemanticType::Unknown) {
    return;
  }
  // A generic callable's reference result has no source until a concrete
  // instance selects the callable; that reanalysis proves the relay.
  {
    const Expr *candidate = value.get();
    while (const auto *group = exprAs<Grouping>(candidate))
      candidate = group->expression().get();
    if (const auto *call = exprAs<Call>(candidate);
        call != nullptr && semanticModel.findCall(*call) == nullptr &&
        semanticModel.findLambdaCall(*call) == nullptr &&
        semanticModel.findDeferredCallableCall(*call) != nullptr)
      return;
  }
  const ExprPtr *physicalOrigin = &value;
  std::unordered_set<const Expr *> visitedOrigins;
  while (physicalOrigin != nullptr && *physicalOrigin &&
         !receiverStorageBorrow(*physicalOrigin)) {
    if (!visitedOrigins.insert(physicalOrigin->get()).second) {
      physicalOrigin = nullptr;
      break;
    }
    if (const auto *group = exprAs<Grouping>(physicalOrigin->get())) {
      physicalOrigin = &group->expression();
    } else if (const auto *comma = exprAs<Binary>(physicalOrigin->get());
               comma != nullptr && comma->oper().kind == TokenKind::COMMA) {
      physicalOrigin = &comma->right();
    } else {
      physicalOrigin = storedBorrowSource(*physicalOrigin);
    }
  }
  if (physicalOrigin != nullptr && *physicalOrigin) {
    const bool stable = hasStableBorrowStorage(*physicalOrigin);
    const ExprPtr *receiver = storedBorrowSource(*physicalOrigin);
    const SemanticLoanPlace shell = receiver != nullptr
                                        ? loanPlace(*receiver)
                                        : SemanticLoanPlace{.receiver = true};
    bool validShell = stable && shell.receiver &&
                      contract->origin == BorrowOriginKind::Receiver;
    if (const Parameter *origin =
            stable ? contract->originParameter() : nullptr) {
      const auto *binding = semanticModel.findBinding(*origin);
      const SemanticType *originType = contract->originParameterType();
      validShell = binding != nullptr && originType != nullptr &&
                   originType->kind == SemanticType::Reference &&
                   !shell.receiver && shell.root == binding->symbol;
    } else if (stable && contract->origin == BorrowOriginKind::Global &&
               contract->place) {
      validShell = !shell.receiver && shell.root == contract->place->root;
    }
    if (!validShell) {
      // A named carrier is stable within this scope, but its long-lived
      // external dependency cannot authorize returning its own local shell.
      // Check physical storage before the ordinary external-owner relay.
      Diagnostic diagnostic = makeDiagnostic(
          std::move(code), DiagnosticPhase::Semantics, expressionToken(value),
          "Returned " + std::string(description) +
              (stable
                   ? " refers to local or by-value receiver storage that does "
                     "not outlive this call."
                   : " refers to temporary receiver storage that is destroyed "
                     "at the end of this full expression."));
      diagnostic.hints.emplace_back(
          "Return an owned value, or borrow from receiver storage that "
          "outlives "
          "the call. An external dependency does not extend the containing "
          "receiver's lifetime.");
      diagnostics.emplace_back(std::move(diagnostic));
      return;
    }
  }
  // A selected payload reference can carry an exact external dependency even
  // when the input carrier has several conditional paths. Prove the complete
  // scalar result against the declared parameter inventory before consulting
  // the older single-path carrier relay. This does not authorize the local
  // carrier shell: its storage check above remains independent.
  const auto completeResult = completeBorrowDependencySet(value);
  const bool conditionalSignature =
      function && conditionalReferenceReturn(
                      function->returnType, function->parameterTypes,
                      function->ownerClass ? openClassType(function->ownerClass)
                                           : SemanticType::Unknown);
  if (function != nullptr && (conditionalSignature || completeResult) &&
      valueInfo->category == ValueCategory::Place) {
    auto dependencies = completeResult ? completeResult->dependencies
                                       : storedBorrowDependencies(value);
    if (!dependencies.empty() &&
        std::all_of(dependencies.begin(), dependencies.end(),
                    [&](const auto &dependency) {
                      return dependency.place.valid() &&
                             dependency.designatedFields.empty() &&
                             dependency.conditions.empty() &&
                             (function->returnType.referenceAccess ==
                                  AccessMode::ReadOnly ||
                              dependency.access == AccessMode::Mutable);
                    })) {
      for (auto &dependency : dependencies)
        dependency.access = function->returnType.referenceAccess;
      auto constrained = *function;
      // An ordinary reference signature already selected its one owner.
      // A complete accessor result can prove that relay, but cannot silently
      // replace the enclosing function's inferred contract with another one.
      if (!conditionalSignature)
        constrained.explicitReturnBorrowOrigin = true;
      if (normalizedCompleteReturnBorrow(
              constrained, {.dependencies = std::move(dependencies)}))
        return;
    }
  }
  // A lambda's contract is one read-only reference parameter: beyond the
  // stable physical-origin check above, the returned reference must derive
  // from that parameter. Field paths, dependency lists, receivers and
  // globals belong to function declarations.
  if (function == nullptr) {
    const Parameter *source = contract->originParameter();
    if (source == nullptr || isParameterDerivedBorrow(value, *source)) {
      return;
    }
    Diagnostic diagnostic = makeDiagnostic(
        std::move(code), DiagnosticPhase::Semantics, expressionToken(value),
        "Returned " + std::string(description) +
            " must derive from the lambda's read-only parameter '" +
            source->name.lexeme + "'.");
    diagnostic.hints.emplace_back(
        "Return the parameter or a place reached through it; locals, copies, "
        "captures and temporaries cannot provide a lambda's borrowed "
        "result.");
    diagnostics.emplace_back(std::move(diagnostic));
    return;
  }
  if (!function->returnBorrowFields.empty()) {
    auto actual =
        function->returnType.kind == SemanticType::Reference
            ? std::vector<SemanticLoanDependency>{{.place =
                                                       retainedLoanPlace(value),
                                                   .access =
                                                       function->returnType
                                                           .referenceAccess}}
            : storedBorrowDependencies(value);
    const bool matches =
        actual.size() == function->returnBorrowFields.size() &&
        std::all_of(
            function->returnBorrowFields.begin(),
            function->returnBorrowFields.end(), [&](const auto &expected) {
              const auto found = std::find_if(
                  actual.begin(), actual.end(), [&](const auto &dependency) {
                    if (dependency.designatedFields != expected.resultFields ||
                        dependency.access != expected.access)
                      return false;
                    if (expected.origin == BorrowOriginKind::Receiver) {
                      if (!dependency.place.receiver)
                        return false;
                    } else if (expected.origin == BorrowOriginKind::Argument) {
                      if (function->declaration == nullptr ||
                          expected.parameter >= function->bodyDeclaration()
                                                    ->parameters()
                                                    .size() ||
                          dependency.place.receiver ||
                          dependency.place.root !=
                              symbolForDeclaration(
                                  function->bodyDeclaration()
                                      ->parameters()[expected.parameter]
                                      .name))
                        return false;
                    } else {
                      return false;
                    }
                    if (dependency.place.projections.size() !=
                        expected.sourceFields.size())
                      return false;
                    for (std::size_t index = 0;
                         index < expected.sourceFields.size(); ++index)
                      if (dependency.place.projections[index].kind !=
                              PlaceProjectionKind::Field ||
                          dependency.place.projections[index].field !=
                              expected.sourceFields[index])
                        return false;
                    return true;
                  });
              return found != actual.end();
            });
    if (!matches) {
      report(expressionToken(value),
             "Each returned borrowed field must preserve its exact declared "
             "source field, owner, and access permission.",
             std::move(code));
      return;
    }
  }
  if (storedReferencePaths(valueInfo->type).size() > 1) {
    const auto dependencies = storedBorrowDependencies(value);
    const SymbolId parameterSymbol =
        function->declaration != nullptr &&
                function->returnBorrowParameter <
                    function->bodyDeclaration()->parameters().size()
            ? symbolForDeclaration(
                  function->bodyDeclaration()
                      ->parameters()[function->returnBorrowParameter]
                      .name)
            : 0;
    std::vector<SymbolId> parameterSymbols{parameterSymbol};
    if (function->declaration != nullptr) {
      for (const auto parameter : function->returnBorrowParameters) {
        if (parameter < function->bodyDeclaration()->parameters().size()) {
          parameterSymbols.push_back(symbolForDeclaration(
              function->bodyDeclaration()->parameters()[parameter].name));
        }
      }
    }
    const bool validOrigins =
        !dependencies.empty() &&
        std::all_of(dependencies.begin(), dependencies.end(),
                    [&](const SemanticLoanDependency &dependency) {
                      return function->returnBorrowOrigin ==
                                     BorrowOriginKind::Receiver
                                 ? dependency.place.receiver
                                 : function->returnBorrowOrigin ==
                                           BorrowOriginKind::Argument &&
                                       std::find(parameterSymbols.begin(),
                                                 parameterSymbols.end(),
                                                 dependency.place.root) !=
                                           parameterSymbols.end();
                    });
    if (!validOrigins) {
      report(expressionToken(value),
             "Every returned borrowed field must derive from its declared "
             "owner dependency; local or unrelated storage cannot escape.",
             "GTI-S2045");
      return;
    }
    if (!function->returnBorrowParameters.empty()) {
      if (std::any_of(parameterSymbols.begin(), parameterSymbols.end(),
                      [&](SymbolId parameter) {
                        return std::none_of(
                            dependencies.begin(), dependencies.end(),
                            [&](const SemanticLoanDependency &dependency) {
                              return dependency.place.root == parameter;
                            });
                      })) {
        report(expressionToken(value),
               "A composed borrowed return must preserve every summarized "
               "parameter dependency.",
               "GTI-S2045");
      }
      return;
    }
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
          function->bodyDeclaration()->parameters().size()) {
    return;
  }
  const Parameter &source = function->bodyDeclaration()
                                ->parameters()[function->returnBorrowParameter];
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
      "Returned " + std::string(description) + " must derive from " +
          (source.name.lexeme.empty()
               ? "unnamed read-only parameter " +
                     std::to_string(function->returnBorrowParameter + 1)
               : "read-only parameter '" + source.name.lexeme + "'") +
          ".");
  diagnostic.related.push_back({tokenSpan(source.sourceAnchor()),
                                "Required owner dependency source is here."});
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
      const auto path = storedReferencePath(objectType);
      if (owner != nullptr && path && !path->fields.empty() &&
          selected != nullptr &&
          selected->symbol.variableDeclaration ==
              path->fields.front().declaration) {
        return path->fields.size() == 1
                   ? StoredCarrierReturnProvenance::ExternalReferent
                   : StoredCarrierReturnProvenance::CarrierValue;
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
    if (source != nullptr && construction->directInitialization)
      return storedCarrierReturnProvenance(*source, parameter, visiting);
    if (source != nullptr) {
      const auto provenance =
          storedCarrierReturnProvenance(*source, parameter, visiting);
      const auto bindings = storedCarrierConstructorBindings(
          construction->constructedType, construction->declaration,
          construction->parameterTypes);
      // A constructor can preserve a carrier's exact external dependencies
      // under another ordinary field. Its validated field map distinguishes
      // this from borrowing the temporary parameter shell itself.
      if (!bindings.empty() &&
          std::all_of(bindings.begin(), bindings.end(),
                      [&](const auto &binding) {
                        return binding.parameter ==
                                   construction->borrowArgument &&
                               !binding.sourceFields.empty();
                      }) &&
          provenance == StoredCarrierReturnProvenance::CarrierValue)
        return StoredCarrierReturnProvenance::CarrierValue;
      if (provenance == StoredCarrierReturnProvenance::ExternalReferent)
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
        resolved->borrowArgument >=
            resolvedCallArgumentCount(*call, resolved)) {
      return StoredCarrierReturnProvenance::None;
    }
    const StoredCarrierReturnProvenance source = storedCarrierReturnProvenance(
        *resolvedCallArgument(*call, resolved, resolved->borrowArgument),
        parameter, visiting);
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
  if (sourceType == function.returnType &&
      storedReferencePaths(sourceType).size() > 1 &&
      actual == StoredCarrierReturnProvenance::CarrierValue) {
    return;
  }
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
  // A forwarded pack element derives from the pack parameter it expands.
  if (const auto *expansion = exprAs<PackExpansion>(expression.get())) {
    const Symbol *symbol = resolve(expansion->name());
    return symbol != nullptr && symbol->parameterDeclaration == &parameter;
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
  // A complete scalar result already names its external referents, such as
  // the reference a carrier relays from that parameter's storage.
  const auto *binding = semanticModel.findBinding(parameter);
  const auto complete = completeBorrowDependencySet(expression);
  return binding != nullptr && binding->symbol != 0 && complete &&
         complete->possibleAlternatives.empty() &&
         !complete->dependencies.empty() &&
         std::all_of(complete->dependencies.begin(),
                     complete->dependencies.end(), [&](const auto &dependency) {
                       return dependency.designatedFields.empty() &&
                              dependency.conditions.empty() &&
                              !dependency.place.receiver &&
                              dependency.place.root == binding->symbol;
                     });
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
    if (symbol != nullptr && symbol->retainedLoan != 0) {
      const SemanticLoanInfo *loan =
          semanticModel.findLoan(symbol->retainedLoan);
      if (loan != nullptr && loan->receiverOrigin)
        return true;
    }
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
  if (const auto *construction =
          semanticModel.findContextualConstruction(*expression);
      construction && construction->contextualBorrowDependencies) {
    const auto &dependencies =
        construction->contextualBorrowDependencies->dependencies;
    return !dependencies.empty() &&
           std::all_of(
               dependencies.begin(), dependencies.end(),
               [](const auto &dependency) { return dependency.place.valid(); });
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
  // A moved value's text relays still view its owner's storage (L-OWN-02);
  // a consuming method cannot return a reference into the moved value.
  if (const ExprPtr *moved = explicitMoveSource(expression);
      moved != nullptr && *moved) {
    const ExpressionInfo *info = semanticModel.findExpression(**moved);
    if (info != nullptr && isTextDependentValue(*info))
      return hasStableBorrowStorage(*moved, visiting);
  }
  if (const auto dependencies =
          semanticModel.findBorrowDependencies(*expression);
      !dependencies.empty())
    return std::all_of(
        dependencies.begin(), dependencies.end(), [](const auto &dependency) {
          return dependency.place.root != 0 || dependency.place.receiver;
        });
  if (const auto *binary = exprAs<Binary>(expression.get());
      binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
    return hasStableBorrowStorage(binary->right(), visiting);
  }
  if (receiverStorageBorrow(expression)) {
    const ExprPtr *receiver = storedBorrowSource(expression);
    if (receiver == nullptr)
      return currentClass.has_value(); // An implicit receiver is addressable.
    const auto *info = semanticModel.findExpression(**receiver);
    // A temporary can carry a long-lived external borrow without its own
    // storage becoming long-lived. Compact ordinary receiver contracts
    // protect that physical storage, not the carried external referent.
    return info != nullptr && info->category == ValueCategory::Place &&
           hasStableBorrowStorage(*receiver, visiting);
  }
  if (const auto *operation = semanticModel.findOperator(*expression);
      operation != nullptr && !operation->borrowFields.empty()) {
    const ExprPtr *source = operatorBorrowSource(expression, *operation);
    // An exact stored-field mapping instead follows the external referent;
    // extracting it does not depend on retaining a temporary cursor shell.
    return source != nullptr && hasStableBorrowStorage(*source, visiting);
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

auto SemanticVisitor::Impl::genericDefaultConstant(
    const Token &name, const std::vector<std::string> &fromScope) const
    -> std::optional<GenericDefaultConstant> {
  for (std::size_t count = fromScope.size() + 1; count > 0; --count) {
    const std::string qualified =
        qualifiedName(fromScope, count - 1, name.lexeme);
    const auto found = genericDefaultConstants.find(qualified);
    if (found == genericDefaultConstants.end()) {
      continue;
    }
    for (auto candidate = found->second.rbegin();
         candidate != found->second.rend(); ++candidate) {
      if ((candidate->sourceUnit == currentSourceUnit &&
           candidate->declaration.position < name.position) ||
          (!candidate->internalLinkage &&
           candidate->sourceUnit != currentSourceUnit &&
           declarationVisible(candidate->sourceUnit, candidate->declaration))) {
        return *candidate;
      }
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::evaluateGenericDefaultConstantInitializer(
    const Expr &expression, const std::vector<std::string> &fromScope) const
    -> std::optional<std::uint64_t> {
  if (const auto *literal = exprAs<LiteralExpr>(&expression)) {
    if (const auto *value = std::get_if<std::uint64_t>(&literal->value())) {
      return *value;
    }
    return std::nullopt;
  }
  if (const auto *variable = exprAs<Variable>(&expression)) {
    const std::optional<GenericDefaultConstant> constant =
        genericDefaultConstant(variable->name(), fromScope);
    return constant ? std::optional{constant->value} : std::nullopt;
  }
  if (const auto *grouping = exprAs<Grouping>(&expression)) {
    return evaluateGenericDefaultConstantInitializer(*grouping->expression(),
                                                     fromScope);
  }
  if (const auto *conversion = exprAs<Conversion>(&expression)) {
    const TypeRef &target = conversion->targetType();
    if (target.name.last().kind != TokenKind::UINT64 ||
        !target.pointers.empty() || target.reference ||
        !target.arrayExtents.empty() || !target.arguments.empty()) {
      return std::nullopt;
    }
    return evaluateGenericDefaultConstantInitializer(*conversion->value(),
                                                     fromScope);
  }
  const auto *binary = exprAs<Binary>(&expression);
  if (binary == nullptr || (binary->oper().kind != TokenKind::PLUS &&
                            binary->oper().kind != TokenKind::MINUS &&
                            binary->oper().kind != TokenKind::STAR &&
                            binary->oper().kind != TokenKind::SLASH &&
                            binary->oper().kind != TokenKind::PERCENT)) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> left =
      evaluateGenericDefaultConstantInitializer(*binary->left(), fromScope);
  const std::optional<std::uint64_t> right =
      evaluateGenericDefaultConstantInitializer(*binary->right(), fromScope);
  if (!left || !right) {
    return std::nullopt;
  }
  const ConstantEvaluation evaluated = evaluateConstantBinary(
      binary->oper().kind,
      ConstantValue{makeConstantInteger({.magnitude = *left}, {.width = 64})},
      ConstantValue{makeConstantInteger({.magnitude = *right}, {.width = 64})},
      CheckedIntegerDomain{.width = 64});
  return evaluated ? constantUnsignedMagnitude(*evaluated.value) : std::nullopt;
}

auto SemanticVisitor::Impl::recordGenericDefaultConstant(
    const VariableDecl &declaration, const std::vector<std::string> &fromScope)
    -> void {
  const TypeRef &type = declaration.type();
  if (!declaration.isConstexpr() || declaration.isMutable() ||
      declaration.initializer() == nullptr ||
      type.name.last().kind != TokenKind::UINT64 || !type.pointers.empty() ||
      type.reference || !type.arrayExtents.empty() || !type.arguments.empty()) {
    return;
  }
  const std::optional<std::uint64_t> value =
      evaluateGenericDefaultConstantInitializer(*declaration.initializer(),
                                                fromScope);
  if (!value) {
    return;
  }
  const std::string qualified =
      qualifiedName(fromScope, declaration.name().lexeme);
  genericDefaultConstants[qualified].push_back(
      GenericDefaultConstant{.value = *value,
                             .sourceUnit = currentSourceUnit,
                             .internalLinkage = declaration.isStatic(),
                             .qualifiedName = qualified,
                             .declaration = declaration.name()});
}

auto SemanticVisitor::Impl::recordGenericDefaultConstantUses() -> void {
  for (std::size_t index = lateFinalization == nullptr
                               ? 0
                               : lateFinalization->genericDefaultConstantUses;
       index < genericDefaultConstantUses.size(); ++index) {
    const GenericDefaultConstantUse &use = genericDefaultConstantUses[index];
    const Symbol *symbol = nullptr;
    if (use.constant.internalLinkage) {
      const auto unit =
          valueFlow.internalNamespaceSymbols.find(use.constant.sourceUnit);
      if (unit != valueFlow.internalNamespaceSymbols.end()) {
        const auto found = unit->second.find(use.constant.qualifiedName);
        if (found != unit->second.end()) {
          symbol = &found->second;
        }
      }
    } else {
      const auto found =
          valueFlow.namespaceSymbols.find(use.constant.qualifiedName);
      if (found != valueFlow.namespaceSymbols.end()) {
        symbol = &found->second;
      }
    }
    if (symbol == nullptr || symbol->toolingSymbol == 0) {
      continue;
    }
    semanticModel.recordOccurrence(
        {.sourceUnit = sourceUnitFor(use.token),
         .span = tokenSpan(use.token),
         .kind = SemanticOccurrenceKind::Symbol,
         .symbol = symbol->toolingSymbol,
         .roles = OccurrenceRole::Reference | OccurrenceRole::Read,
         .name = use.token.lexeme,
         .type = SemanticType::UInt64});
  }
}

auto SemanticVisitor::Impl::normalizeGenericValueDefault(
    const ArrayExtentExpr &expression,
    const std::vector<std::string> &fromScope)
    -> std::shared_ptr<const GenericValueExpression> {
  if (expression.isAtom()) {
    if (const auto *value =
            std::get_if<std::uint64_t>(&expression.token.literal)) {
      return std::make_shared<GenericValueExpression>(
          GenericValueExpression{.kind = GenericValueExpression::UInt64,
                                 .token = expression.token,
                                 .value = *value});
    }
    if (expression.token.kind == TokenKind::IDENTIFIER) {
      if (const std::optional<CompileTimeValue> parameter =
              resolveValueParameter(expression.token)) {
        if (parameter->kind == CompileTimeValue::UInt64) {
          return std::make_shared<GenericValueExpression>(
              GenericValueExpression{.kind = GenericValueExpression::UInt64,
                                     .token = expression.token,
                                     .value = parameter->value});
        }
        if (parameter->kind == CompileTimeValue::Parameter) {
          recordGenericParameterUse(expression.token, *parameter,
                                    OccurrenceRole::Read);
          return std::make_shared<GenericValueExpression>(
              GenericValueExpression{.kind = GenericValueExpression::Parameter,
                                     .token = expression.token,
                                     .parameterId = parameter->parameterId});
        }
      }
      const Symbol *symbol = resolve(expression.token);
      const ConstantValue *constant =
          symbol == nullptr ? nullptr : constantFor(*symbol);
      if (constant != nullptr) {
        if (const std::optional<std::uint64_t> value =
                constantUnsignedMagnitude(*constant)) {
          return std::make_shared<GenericValueExpression>(
              GenericValueExpression{.kind = GenericValueExpression::UInt64,
                                     .token = expression.token,
                                     .value = *value});
        }
      }
      if (const std::optional<GenericDefaultConstant> constant =
              genericDefaultConstant(expression.token, fromScope)) {
        genericDefaultConstantUses.push_back(
            {.token = expression.token, .constant = *constant});
        return std::make_shared<GenericValueExpression>(
            GenericValueExpression{.kind = GenericValueExpression::UInt64,
                                   .token = expression.token,
                                   .value = constant->value});
      }
    }
    report(expression.token,
           "A generic value default must use uint64_t literals, visible "
           "uint64_t constexpr bindings, or earlier value parameters.",
           "GTI-S2026");
    return nullptr;
  }

  if (!expression.left || !expression.right) {
    report(expression.token, "Malformed generic value default expression.",
           "GTI-S2026");
    return nullptr;
  }
  const std::shared_ptr<const GenericValueExpression> left =
      normalizeGenericValueDefault(*expression.left, fromScope);
  const std::shared_ptr<const GenericValueExpression> right =
      normalizeGenericValueDefault(*expression.right, fromScope);
  if (!left || !right) {
    return nullptr;
  }
  if (left->kind == GenericValueExpression::UInt64 &&
      right->kind == GenericValueExpression::UInt64) {
    const ConstantEvaluation evaluated = evaluateConstantBinary(
        expression.token.kind,
        ConstantValue{
            makeConstantInteger({.magnitude = left->value}, {.width = 64})},
        ConstantValue{
            makeConstantInteger({.magnitude = right->value}, {.width = 64})},
        CheckedIntegerDomain{.width = 64});
    if (evaluated) {
      return std::make_shared<GenericValueExpression>(GenericValueExpression{
          .kind = GenericValueExpression::UInt64,
          .token = expression.token,
          .value = *constantUnsignedMagnitude(*evaluated.value)});
    }
    report(expression.token,
           evaluated.failure == ConstantEvaluationFailure::IntegerOverflow
               ? (expression.token.kind == TokenKind::MINUS
                      ? "Generic value default arithmetic underflows uint64_t."
                      : "Generic value default arithmetic overflows uint64_t.")
               : "Generic value default arithmetic cannot divide or take "
                 "modulo by zero.",
           "GTI-S2026");
    return nullptr;
  }
  return std::make_shared<GenericValueExpression>(
      GenericValueExpression{.kind = GenericValueExpression::Binary,
                             .token = expression.token,
                             .left = std::move(left),
                             .right = std::move(right)});
}

auto SemanticVisitor::Impl::evaluateGenericValueExpression(
    const GenericValueExpression &expression,
    const GenericSubstitution &substitution) const -> GenericValueEvaluation {
  if (expression.kind == GenericValueExpression::UInt64) {
    return {.value = CompileTimeValue::uint64(expression.value)};
  }
  if (expression.kind == GenericValueExpression::Parameter) {
    if (const auto found = substitution.values.find(expression.parameterId);
        found != substitution.values.end()) {
      return {.value = found->second};
    }
    return {.value = CompileTimeValue::parameter(expression.parameterId)};
  }
  if (!expression.left || !expression.right) {
    return {.failure = ConstantEvaluationFailure::UnsupportedExpression,
            .token = &expression.token};
  }
  const GenericValueEvaluation left =
      evaluateGenericValueExpression(*expression.left, substitution);
  if (!left.value) {
    return left;
  }
  const GenericValueEvaluation right =
      evaluateGenericValueExpression(*expression.right, substitution);
  if (!right.value) {
    return right;
  }
  if (left.value->kind != CompileTimeValue::UInt64 ||
      right.value->kind != CompileTimeValue::UInt64) {
    return {.value = CompileTimeValue{}};
  }
  const ConstantEvaluation evaluated = evaluateConstantBinary(
      expression.token.kind,
      ConstantValue{
          makeConstantInteger({.magnitude = left.value->value}, {.width = 64})},
      ConstantValue{makeConstantInteger({.magnitude = right.value->value},
                                        {.width = 64})},
      CheckedIntegerDomain{.width = 64});
  if (!evaluated) {
    return {.failure = evaluated.failure, .token = &expression.token};
  }
  return {.value = CompileTimeValue::uint64(
              *constantUnsignedMagnitude(*evaluated.value))};
}

auto SemanticVisitor::Impl::firstNestedTypeConstraintFailure(
    const SemanticType &type) const -> std::optional<ConstraintFailure> {
  // Defaults can mention an earlier parameter without proving its eventual
  // constraints. Validate every closed component now, and the remaining
  // components after substitution when the default is actually selected.
  for (const SemanticType &argument : type.arguments) {
    if (auto failure = firstNestedTypeConstraintFailure(argument)) {
      return failure;
    }
  }
  const std::vector<GenericParameterInfo> *parameters = nullptr;
  if (type.kind == SemanticType::Class && type.classId != 0 &&
      type.classId <= classes.size())
    parameters = &classInfo(type.classId).genericParameters;
  else if (type.kind == SemanticType::Enum && type.enumId != 0 &&
           type.enumId <= enums.size())
    parameters = &enums[type.enumId - 1].genericParameters;
  if (parameters == nullptr) {
    return std::nullopt;
  }
  std::size_t index = 0;
  for (const GenericParameterInfo &parameter : *parameters) {
    if (parameter.value) {
      continue;
    }
    const std::size_t end = parameter.pack ? type.arguments.size() : index + 1;
    for (; index < end && index < type.arguments.size(); ++index) {
      if (containsOpenTypeParameter(type.arguments[index])) {
        continue;
      }
      if (auto failure =
              firstConstraintFailure({parameter}, {type.arguments[index]})) {
        return failure;
      }
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::applyGenericDefaults(
    const std::vector<GenericParameterInfo> &parameters,
    GenericSubstitution &substitution, const Token &site, bool diagnoseFailure)
    -> bool {
  bool valid = true;
  for (const GenericParameterInfo &parameter : parameters) {
    if (parameter.value) {
      if (substitution.values.contains(parameter.id) ||
          !parameter.defaultValue) {
        continue;
      }
      const GenericValueEvaluation evaluation =
          evaluateGenericValueExpression(*parameter.defaultValue, substitution);
      if (!evaluation.value) {
        if (diagnoseFailure) {
          const Token &location =
              evaluation.token == nullptr ? site : *evaluation.token;
          if (evaluation.failure ==
              ConstantEvaluationFailure::IntegerOverflow) {
            report(location,
                   location.kind == TokenKind::MINUS
                       ? "Generic value default arithmetic underflows "
                         "uint64_t for the selected arguments."
                       : "Generic value default arithmetic overflows "
                         "uint64_t for the selected arguments.",
                   "GTI-S2026");
          } else if (evaluation.failure ==
                         ConstantEvaluationFailure::DivisionByZero ||
                     evaluation.failure ==
                         ConstantEvaluationFailure::ModuloByZero) {
            report(location,
                   "Generic value default arithmetic cannot divide or take "
                   "modulo by zero for the selected arguments.",
                   "GTI-S2026");
          } else {
            report(location,
                   "Default for generic value parameter '" +
                       parameter.name.lexeme +
                       "' is invalid for the selected arguments.",
                   "GTI-S2026");
          }
        }
        valid = false;
      } else {
        substitution.values.emplace(parameter.id, *evaluation.value);
      }
      continue;
    }
    if (substitution.types.contains(parameter.id) || !parameter.defaultType) {
      continue;
    }
    const SemanticType type =
        substituteType(*parameter.defaultType, substitution);
    if (type == SemanticType::Unknown) {
      if (diagnoseFailure) {
        report(site,
               "Default for generic type parameter '" + parameter.name.lexeme +
                   "' does not resolve to a type.",
               "GTI-S2023");
      }
      valid = false;
    } else {
      if (const auto failure = firstNestedTypeConstraintFailure(type)) {
        if (diagnoseFailure) {
          reportConstraintFailure(site, *failure);
        }
        valid = false;
      }
      if (!containsOpenTypeParameter(type)) {
        if (const auto failure = firstConstraintFailure({parameter}, {type})) {
          if (diagnoseFailure) {
            reportConstraintFailure(site, *failure);
          }
          valid = false;
        }
      }
      substitution.types.emplace(parameter.id, type);
    }
  }
  return valid;
}

auto SemanticVisitor::Impl::makeGenericParameters(
    const std::vector<GenericParameter> &parameters,
    const Token &declarationName, const std::vector<std::string> &fromScope,
    bool allowStructural, bool resolveDefaults)
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
    ConceptId constraintConcept = 0;
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
        constraintConcept = *resolved;
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
            currentSourceUnit != conceptInfo.sourceUnit) {
          report(parameter.constraint->last(),
                 "Compiler capability concept '" + conceptInfo.qualifiedName +
                     "' is private to its standard-library unit; compose a "
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
          } else if (conceptInfo.protocol) {
            // Satisfaction needs a conformance block, which an inline
            // constraint's atomic facts cannot express.
            report(parameter.constraint->last(),
                   "A concept with a body must be written in a trailing "
                   "requires clause.",
                   "GTI-S2029");
            constraints = constraintBit(GenericConstraintKind::Invalid);
          } else if (conceptInfo.hasStructuralRequirements &&
                     !allowStructural) {
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
          diagnostics.back().missingIncludeName =
              parameter.constraint->last().lexeme;
        }
        constraints = constraintBit(GenericConstraintKind::Invalid);
      }
    }
    const GenericParameterId id = nextGenericParameterId++;
    genericConstraints.insert_or_assign(id, constraints);
    if (constraintConcept != 0) {
      genericConstraintConcepts.insert_or_assign(id, constraintConcept);
    }
    result.push_back(
        GenericParameterInfo{.id = id,
                             .name = parameter.name,
                             .pack = parameter.pack.has_value(),
                             .value = valueParameter,
                             .constraints = constraints,
                             .constraintName = parameter.constraint,
                             .conceptId = constraintConcept});
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
  if (resolveDefaults)
    resolveGenericParameterDefaults(parameters, result, fromScope);
  return result;
}

auto SemanticVisitor::Impl::resolveGenericParameterDefaults(
    const std::vector<GenericParameter> &parameters,
    std::vector<GenericParameterInfo> &result,
    const std::vector<std::string> &fromScope) -> void {
  bool sawDefault = false;
  const bool hasPack = std::any_of(parameters.begin(), parameters.end(),
                                   [](const GenericParameter &parameter) {
                                     return parameter.pack.has_value();
                                   });
  std::vector<GenericParameterInfo> available;
  available.reserve(result.size());
  for (const GenericParameter &parameter : parameters) {
    const auto found = std::find_if(
        result.begin(), result.end(), [&](const GenericParameterInfo &info) {
          return info.name.lexeme == parameter.name.lexeme &&
                 info.name.position == parameter.name.position &&
                 info.name.source == parameter.name.source;
        });
    if (found == result.end()) {
      continue;
    }
    const bool hasDefault = parameter.equal.has_value();
    if (hasDefault) {
      sawDefault = true;
    } else if (sawDefault && !parameter.pack) {
      report(parameter.name,
             "A generic parameter without a default cannot follow a "
             "defaulted parameter.",
             "GTI-S2026");
    }
    if (parameter.pack && hasDefault) {
      report(*parameter.equal,
             "A generic type pack cannot declare a default argument.",
             "GTI-S2023");
    } else if (hasPack && hasDefault) {
      report(*parameter.equal,
             "Generic defaults are not supported in a parameter list that "
             "contains a type pack.",
             "GTI-S2023");
    }

    beginTypeParameterScope(available);
    if (parameter.defaultType) {
      const auto referencesUnavailableParameter =
          [&](const auto &self, const TypeRef &type) -> bool {
        if (type.qualifiedOwner && self(self, *type.qualifiedOwner)) {
          return true;
        }
        if (type.name.segments.size() == 1 &&
            std::any_of(parameters.begin(), parameters.end(),
                        [&](const GenericParameter &candidate) {
                          return candidate.name.lexeme ==
                                 type.name.last().lexeme;
                        }) &&
            std::none_of(available.begin(), available.end(),
                         [&](const GenericParameterInfo &candidate) {
                           return candidate.name.lexeme ==
                                  type.name.last().lexeme;
                         })) {
          return true;
        }
        return std::any_of(
            type.arguments.begin(), type.arguments.end(),
            [&](const TypeRef &argument) { return self(self, argument); });
      };
      if (referencesUnavailableParameter(referencesUnavailableParameter,
                                         *parameter.defaultType)) {
        report(parameter.defaultType->name.last(),
               "A generic type default may reference only visible types and "
               "earlier generic parameters.",
               "GTI-S2023");
      } else {
        const std::size_t diagnosticStart = diagnostics.size();
        const bool enclosingDefault = resolvingGenericTypeDefault;
        resolvingGenericTypeDefault = true;
        validateType(*parameter.defaultType);
        const SemanticType type = typeOf(*parameter.defaultType);
        if (type == SemanticType::Unknown &&
            diagnostics.size() == diagnosticStart) {
          report(parameter.defaultType->name.last(),
                 "A generic type default may reference only visible types "
                 "and earlier generic parameters.",
                 "GTI-S2023");
        } else if (type != SemanticType::Unknown) {
          // Structural member signatures are not complete while generic
          // parameters are registered. Their exact concept application is
          // checked by applyGenericDefaults when this default is selected.
          if (const auto failed =
                  firstUnsatisfiedConstraint(type, found->constraints)) {
            reportConstraintFailure(
                parameter.defaultType->name.last(),
                ConstraintFailure{.parameter = found->name,
                                  .argument = type,
                                  .constraints = found->constraints,
                                  .failed = *failed,
                                  .constraintName = found->constraintName});
          } else {
            found->defaultType = std::make_shared<SemanticType>(type);
          }
        }
        resolvingGenericTypeDefault = enclosingDefault;
      }
    } else if (parameter.defaultValue) {
      const auto unavailableValueParameter =
          [&](const auto &self,
              const ArrayExtentExpr &expression) -> const Token * {
        if (expression.isAtom()) {
          if (expression.token.kind != TokenKind::IDENTIFIER) {
            return nullptr;
          }
          const bool declaredGeneric = std::any_of(
              parameters.begin(), parameters.end(),
              [&](const GenericParameter &candidate) {
                return candidate.name.lexeme == expression.token.lexeme;
              });
          const bool availableValue = std::any_of(
              available.begin(), available.end(),
              [&](const GenericParameterInfo &candidate) {
                return candidate.value &&
                       candidate.name.lexeme == expression.token.lexeme;
              });
          return declaredGeneric && !availableValue ? &expression.token
                                                    : nullptr;
        }
        if (expression.left) {
          if (const Token *token = self(self, *expression.left)) {
            return token;
          }
        }
        return expression.right ? self(self, *expression.right) : nullptr;
      };
      if (const Token *token = unavailableValueParameter(
              unavailableValueParameter, *parameter.defaultValue)) {
        report(*token,
               "A generic value default may reference only earlier value "
               "parameters.",
               "GTI-S2026");
      } else {
        found->defaultValue =
            normalizeGenericValueDefault(*parameter.defaultValue, fromScope);
      }
    }
    endTypeParameterScope();
    available.emplace_back(*found);
  }
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
  auto &requirements = requirementScopes.emplace_back();
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
    // An inline class annotation carries the same concept application as a
    // trailing requires clause. Make its guaranteed operations available to
    // all class bodies, including constructors and destructors, without
    // manufacturing proof for concrete arguments before their admission check.
    if (concrete == instanceTypeSubstitution.end() &&
        parameter.conceptId != 0 &&
        !invalidConstraintSet(parameter.constraints)) {
      requirements.push_back(
          {.conceptId = parameter.conceptId,
           .arguments = {SemanticType::typeParameter(parameter.id)}});
    }
    if (parameter.pack) {
      packs.emplace(parameter.name.lexeme, parameter.id);
    }
  }
}

auto SemanticVisitor::Impl::endTypeParameterScope() -> void {
  typeParameterScopes.pop_back();
  valueParameterScopes.pop_back();
  typePackScopes.pop_back();
  requirementScopes.pop_back();
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
  if (type.isAssociatedTypeProjection()) {
    SemanticType result = type;
    for (SemanticType &argument : result.arguments) {
      argument = substituteType(argument, substitution);
    }
    return reduceAssociatedTypeProjection(std::move(result));
  }
  if (type.kind == SemanticType::TypeParameter ||
      (type.kind == SemanticType::TypePack && !type.concretePack)) {
    const auto found = substitution.types.find(type.genericParameterId);
    return found == substitution.types.end()
               ? type
               : selectExactClassSpecialization(found->second);
  }

  SemanticType result = type;
  for (SemanticType &argument : result.arguments) {
    argument = substituteType(argument, substitution);
  }
  if (result.kind == SemanticType::Class || result.kind == SemanticType::Enum ||
      result.kind == SemanticType::FunctionSignature) {
    std::vector<SemanticType> expanded;
    for (SemanticType &argument : result.arguments) {
      SemanticType::appendExpandedArgument(expanded, std::move(argument));
    }
    result.arguments = std::move(expanded);
  }
  for (SemanticType &argument : result.lambdaEnclosingClassTypes) {
    argument = substituteType(argument, substitution);
  }
  for (SemanticType &argument : result.lambdaEnclosingFunctionTypes) {
    argument = substituteType(argument, substitution);
  }
  for (CompileTimeValue &argument : result.valueArguments) {
    argument = substituteCompileTimeValue(argument, substitution.values);
  }
  const auto substituteValues = [&](std::vector<CompileTimeValue> &values) {
    for (CompileTimeValue &argument : values) {
      argument = substituteCompileTimeValue(argument, substitution.values);
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
    if (!parameter.typeArgumentPath.empty()) {
      if (auto type = projectGenericTypeArgument(
              objectType.arguments, parameter.typeArgumentPath, parameter.id,
              parameter.pack)) {
        result.types.emplace(parameter.id, std::move(*type));
      }
    } else if (parameter.pack) {
      std::vector<SemanticType> elements(
          objectType.arguments.begin() + static_cast<std::ptrdiff_t>(typeIndex),
          objectType.arguments.end());
      // A forwarded symbolic pack retains its identity, rather than becoming
      // a concrete one-element list containing another list.
      if (elements.size() == 1 &&
          elements.front().kind == SemanticType::TypePack &&
          !elements.front().concretePack) {
        result.types.emplace(parameter.id, elements.front());
      } else {
        result.types.emplace(
            parameter.id,
            SemanticType::concreteTypePack(parameter.id, std::move(elements)));
      }
      typeIndex = objectType.arguments.size();
    } else if (parameter.value) {
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

auto SemanticVisitor::Impl::enumSubstitution(const SemanticType &type) const
    -> GenericSubstitution {
  GenericSubstitution result;
  if (type.kind != SemanticType::Enum || type.enumId == 0 ||
      type.enumId > enums.size()) {
    return result;
  }
  std::size_t typeIndex = 0;
  std::size_t valueIndex = 0;
  for (const auto &parameter : enums[type.enumId - 1].genericParameters) {
    if (parameter.value) {
      if (valueIndex < type.valueArguments.size())
        result.values.emplace(parameter.id, type.valueArguments[valueIndex++]);
    } else if (parameter.pack) {
      if (typeIndex > type.arguments.size())
        return {};
      std::vector<SemanticType> elements(type.arguments.begin() + typeIndex,
                                         type.arguments.end());
      result.types.emplace(parameter.id,
                           elements.size() == 1 && elements.front().kind ==
                                                       SemanticType::TypePack
                               ? elements.front()
                               : SemanticType::concreteTypePack(
                                     parameter.id, std::move(elements)));
      typeIndex = type.arguments.size();
    } else if (typeIndex < type.arguments.size()) {
      result.types.emplace(parameter.id, type.arguments[typeIndex++]);
    }
  }
  return result;
}

auto SemanticVisitor::Impl::enumTypeFor(const SemanticType &type) const
    -> std::optional<EnumTypeInfo> {
  if (type.kind != SemanticType::Enum || type.enumId == 0 ||
      type.enumId > enums.size())
    return std::nullopt;
  const auto *primary = semanticModel.findEnumType(type.enumId);
  if (primary == nullptr)
    return std::nullopt;
  const auto count = genericTypeParameterCount(primary->genericParameters);
  const bool pack = !primary->genericParameters.empty() &&
                    primary->genericParameters.back().pack;
  if ((pack ? type.arguments.size() + 1 < count
            : type.arguments.size() != count) ||
      type.valueArguments.size() !=
          genericValueParameterCount(primary->genericParameters))
    return std::nullopt;
  const auto substitution = enumSubstitution(type);
  for (const auto &parameter : primary->genericParameters) {
    if (parameter.value ? !substitution.values.contains(parameter.id)
                        : !substitution.types.contains(parameter.id))
      return std::nullopt;
  }
  EnumTypeInfo result = *primary;
  for (auto &variant : result.enumerators)
    for (auto &payload : variant.payloadTypes)
      payload = substituteType(payload, substitution);
  semanticModel.recordEnumInstanceType(type, result);
  return result;
}

auto SemanticVisitor::Impl::nominalArguments(
    const std::vector<GenericParameterInfo> &parameters,
    const std::vector<TypeRef> &arguments,
    const std::vector<std::string> &fromScope) const
    -> std::optional<ResolvedClassArguments> {
  ResolvedClassArguments result;
  const bool pack = !parameters.empty() && parameters.back().pack;
  const auto required = static_cast<std::size_t>(std::count_if(
      parameters.begin(), parameters.end(), [](const auto &parameter) {
        return !parameter.pack && (parameter.value ? !parameter.defaultValue
                                                   : !parameter.defaultType);
      }));
  if (arguments.size() < required ||
      (!pack && arguments.size() > parameters.size()))
    return std::nullopt;
  GenericSubstitution substitution;
  std::vector<SemanticType> packArguments;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto &parameter = parameters[std::min(index, parameters.size() - 1)];
    if (parameter.value) {
      const auto value = genericValueArgument(arguments[index]);
      if (!value)
        return std::nullopt;
      substitution.values.emplace(parameter.id, *value);
    } else {
      auto resolved = typeOf(arguments[index], fromScope);
      if (resolved == SemanticType::Unknown)
        return std::nullopt;
      if (parameter.pack) {
        SemanticType::appendExpandedArgument(packArguments,
                                             std::move(resolved));
      } else {
        substitution.types.emplace(parameter.id, std::move(resolved));
      }
    }
  }
  for (const auto &parameter : parameters) {
    if (parameter.pack)
      continue;
    if (parameter.value) {
      auto found = substitution.values.find(parameter.id);
      if (found == substitution.values.end() && parameter.defaultValue) {
        const auto evaluated = evaluateGenericValueExpression(
            *parameter.defaultValue, substitution);
        if (evaluated.value)
          found =
              substitution.values.emplace(parameter.id, *evaluated.value).first;
      }
      if (found == substitution.values.end())
        return std::nullopt;
      result.values.push_back(found->second);
    } else {
      auto found = substitution.types.find(parameter.id);
      if (found == substitution.types.end() && parameter.defaultType)
        found =
            substitution.types
                .emplace(parameter.id,
                         substituteType(*parameter.defaultType, substitution))
                .first;
      if (found == substitution.types.end())
        return std::nullopt;
      result.types.push_back(found->second);
    }
  }
  result.types.insert(result.types.end(), packArguments.begin(),
                      packArguments.end());
  return result;
}

auto SemanticVisitor::Impl::openClassType(ClassId id) const -> SemanticType {
  const ClassInfo &owner = classInfo(id);
  if (!owner.declaration->genericArgumentPatterns().empty()) {
    std::vector<SemanticType> patterns;
    const GenericSubstitution substitution{.types = instanceTypeSubstitution,
                                           .values = instanceValueSubstitution};
    for (const SemanticType &pattern : owner.genericArgumentPatterns) {
      patterns.push_back(substituteType(pattern, substitution));
    }
    return selectExactClassSpecialization(
        SemanticType::classType(id, std::move(patterns)));
  }
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
    SemanticType::appendExpandedArgument(
        arguments,
        concrete == instanceTypeSubstitution.end()
            ? (parameter.pack ? SemanticType::typePack(parameter.id)
                              : SemanticType::typeParameter(parameter.id))
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

  // A conformance operation inherits its block's checked header. Resolve its
  // requirement facts here without diagnosing the same written header again.
  const bool restated = clause->keyword.generated;

  const std::vector<std::string> enclosingNamespace = currentNamespace;
  currentNamespace = scope;
  std::vector<const TypeRef *> argumentsToValidate;
  for (std::size_t applicationIndex = 0;
       applicationIndex < clause->requirements().size(); ++applicationIndex) {
    const ConceptApplication &application =
        clause->requirements()[applicationIndex];
    if (application.memberRequirement) {
      recordMemberRequirementReceiver(
          *application.memberRequirement,
          typeOf(application.memberRequirement->receiver(), scope));
      AppliedConceptRequirement requirement{
          .syntax = &application,
          .expression = &clause->expression,
          .expressionApplication = applicationIndex,
          .trustedMemberAccess = isCompilerTrustedRequester(
              application.memberRequirement->member())};
      for (const auto &argument : application.arguments) {
        argumentsToValidate.push_back(&argument);
        requirement.arguments.push_back(typeOf(argument, scope));
      }
      result.push_back(std::move(requirement));
      continue;
    }
    if (reportCompilerPrivatePath(application.name)) {
      continue;
    }
    if (!restated)
      recordQualifiedPathUses(application.name);
    const std::optional<ConceptId> resolved =
        resolveConceptPath(application.name, scope);
    if (!resolved) {
      if (restated) {
        continue;
      }
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
        diagnostics.back().missingIncludeName = application.name.last().lexeme;
      }
      continue;
    }

    RegisteredConcept &conceptInfo = concepts[*resolved - 1];
    if (!restated)
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
      if (!restated)
        report(application.name.last(),
               "Concept '" + conceptInfo.qualifiedName + "' requires " +
                   std::to_string(
                       conceptInfo.declaration->typeParameters().size()) +
                   " type argument" +
                   (conceptInfo.declaration->typeParameters().size() == 1
                        ? "."
                        : "s."),
               "GTI-S2049");
      continue;
    }

    AppliedConceptRequirement requirement{.conceptId = *resolved,
                                          .syntax = &application,
                                          .expression = &clause->expression,
                                          .expressionApplication =
                                              applicationIndex};
    bool valid = true;
    for (const TypeRef &argument : application.arguments) {
      const std::optional<SemanticType> parameter =
          resolveTypeParameter(argument.name);
      const bool plainParameter = parameter && argument.arguments.empty() &&
                                  argument.pointers.empty() &&
                                  !argument.reference &&
                                  argument.arrayExtents.empty();
      if (plainParameter && isActiveTypePack(argument)) {
        if (!restated)
          report(argument.name.last(),
                 "Requires-clause arguments cannot use an unexpanded type "
                 "pack.",
                 "GTI-S2049");
        requirement.arguments.emplace_back(SemanticType::Unknown);
        valid = false;
        continue;
      }
      argumentsToValidate.push_back(&argument);
      const SemanticType type = typeOf(argument, scope);
      if (type == SemanticType::Unknown) {
        requirement.arguments.emplace_back(type);
        valid = false;
        continue;
      }
      requirement.arguments.emplace_back(type);
    }
    if (valid) {
      result.emplace_back(std::move(requirement));
    }
  }
  // Type formation above establishes the complete clause's identities without
  // demanding a compound argument's constraints before their proof exists.
  // Validation consumes the ordinary guaranteed-fact scope: conjunction adds
  // facts, while disjunction retains only facts common to every alternative.
  requirementScopes.push_back(result);
  // Only a dependent application is an assumption for admission. A closed
  // application may make the function nonviable, but cannot assert a false
  // fact into existence while forming another closed argument's type.
  std::erase_if(requirementScopes.back(), [](const auto &requirement) {
    return std::none_of(requirement.arguments.begin(),
                        requirement.arguments.end(), containsOpenTypeParameter);
  });
  // A generated clause is a conformance block's application, restated on
  // each of its operations. validateConformanceTypes validates the block's
  // patterns once, so a restated copy does not report them again.
  for (const TypeRef *argument : argumentsToValidate) {
    if (!restated) {
      validateType(*argument);
    }
    recordTypeUse(*argument);
  }
  requirementScopes.pop_back();
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

auto SemanticVisitor::Impl::declaresConcreteClassStaticAssertions(
    const StmtList &members) const -> bool {
  return std::any_of(
      members.begin(), members.end(), [&](const StmtPtr &member) {
        if (const auto *conditional = stmtAs<ConditionalStmt>(member.get())) {
          const StmtList *branch = conditional->activeBranch(target);
          return branch != nullptr &&
                 declaresConcreteClassStaticAssertions(*branch);
        }
        return stmtAs<StaticAssertDecl>(member.get()) != nullptr;
      });
}

auto SemanticVisitor::Impl::concreteClassAssertionsFail(
    ClassId classId, const std::vector<SemanticType> &typeArguments,
    const std::vector<CompileTimeValue> &values) -> bool {
  const ClassInfo &owner = classInfo(classId);
  if (owner.genericParameters.empty() || owner.declaration == nullptr ||
      !declaresConcreteClassStaticAssertions(owner.declaration->members())) {
    return false;
  }
  if (const std::optional<bool> known =
          knownConcreteClassAssertionOutcome(classId, typeArguments, values))
    return *known;
  // The instance's field analysis reports these assertions; this analysis
  // only learns whether one fails.
  const auto start = static_cast<std::ptrdiff_t>(diagnostics.size());
  analyzeConcreteClassStaticAssertions(owner.declaration->members());
  const bool failed =
      std::any_of(diagnostics.begin() + start, diagnostics.end(),
                  [](const SemanticDiagnostic &diagnostic) {
                    return diagnostic.severity == DiagnosticSeverity::Error;
                  });
  diagnostics.erase(diagnostics.begin() + start, diagnostics.end());
  recordConcreteClassAssertionOutcome(classId, typeArguments, values, failed);
  return failed;
}

auto SemanticVisitor::Impl::knownConcreteClassAssertionOutcome(
    ClassId classId, const std::vector<SemanticType> &typeArguments,
    const std::vector<CompileTimeValue> &values) const -> std::optional<bool> {
  if (requestSession.classAssertionOutcomes.empty())
    return std::nullopt;
  const auto found = requestSession.classAssertionOutcomes.find(
      InstanceAnalysisKey{.kind = InstanceAnalysisKind::ClassFieldInitializers,
                          .declaration = classId,
                          .classTypes = typeArguments,
                          .classValues = values});
  if (found == requestSession.classAssertionOutcomes.end())
    return std::nullopt;
  return found->second;
}

auto SemanticVisitor::Impl::recordConcreteClassAssertionOutcome(
    ClassId classId, const std::vector<SemanticType> &typeArguments,
    const std::vector<CompileTimeValue> &values, bool failed) -> void {
  // An analysis nested in an open reanalysis of the same instance skips the
  // assertions its enclosing reanalysis owns, so it learns nothing.
  if (requestSession.assertionsOwnedUpChain)
    return;
  requestSession.classAssertionOutcomes.insert_or_assign(
      InstanceAnalysisKey{.kind = InstanceAnalysisKind::ClassFieldInitializers,
                          .declaration = classId,
                          .classTypes = typeArguments,
                          .classValues = values},
      failed);
}

auto SemanticVisitor::Impl::beginAssertionTracking() -> void {
  requestSession.assertionDiagnostics.clear();
  requestSession.trackedAssertionFailed = false;
  requestSession.assertionTrackingStart = diagnostics.size();
  requestSession.trackAssertionDiagnostics = true;
}

auto SemanticVisitor::Impl::endAssertionTracking(bool ownerRejected) -> void {
  requestSession.trackAssertionDiagnostics = false;
  if (ownerRejected || requestSession.trackedAssertionFailed) {
    diagnostics.erase(
        diagnostics.begin() +
            static_cast<std::ptrdiff_t>(std::min(
                requestSession.assertionTrackingStart, diagnostics.size())),
        diagnostics.end());
    diagnostics.insert(
        diagnostics.end(),
        std::make_move_iterator(requestSession.assertionDiagnostics.begin()),
        std::make_move_iterator(requestSession.assertionDiagnostics.end()));
  }
  requestSession.assertionDiagnostics.clear();
  requestSession.trackedAssertionFailed = false;
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
  analyzingDependentConstantCondition = false;
  constantConditionGenericDependency = false;
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
  contextualReturnValue = nullptr;
  contextualOperatorIntegerType.reset();
  contextualCallableResult.reset();
  contextualResultUse.reset();
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
  const bool pack =
      !owner.genericParameters.empty() && owner.genericParameters.back().pack;
  const std::size_t fixed =
      genericTypeParameterCount(owner.genericParameters) - (pack ? 1 : 0);
  if (!owner.declaration->genericArgumentPatterns().empty()) {
    if (!valueArguments.empty() ||
        !classArgumentPatternsMatch(owner, typeArguments)) {
      report(owner.name,
             "Concrete type arguments do not match the generic class argument "
             "pattern.",
             "GTI-S2023");
      return false;
    }
  } else if (typeArguments.size() < fixed ||
             (!pack && typeArguments.size() != fixed) ||
             valueArguments.size() !=
                 genericValueParameterCount(owner.genericParameters)) {
    return false;
  }
  const GenericSubstitution substitution = classSubstitution(
      SemanticType::classType(classId, typeArguments, valueArguments));
  for (const auto &[id, type] : substitution.types) {
    instanceTypeSubstitution.insert_or_assign(id, type);
  }
  for (const auto &[id, value] : substitution.values) {
    instanceValueSubstitution.insert_or_assign(id, value);
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
  addEnclosingClassStaticMembers(classId);
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
  if (function.definition != nullptr)
    currentSourceUnit = sourceUnitFor(function.definition->name());

  const bool functionTypePack = !function.genericParameters.empty() &&
                                function.genericParameters.back().pack;
  const std::size_t fixedTypeCount =
      functionTypePack
          ? genericTypeParameterCount(function.genericParameters) - 1
          : genericTypeParameterCount(function.genericParameters);
  if (functionTypeArguments.size() < fixedTypeCount ||
      (!functionTypePack && functionTypeArguments.size() != fixedTypeCount) ||
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
  if (functionTypePack) {
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
    const SemanticType type = typeOf(parameter, scope);
    candidate.parameterTypes.emplace_back(
        parameter.pack && type.kind == SemanticType::TypeParameter
            ? SemanticType::typePack(type.genericParameterId)
            : type);
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
  validateParameterPacks(function.parameters(), genericParameters,
                         &function.returnType());
}

auto SemanticVisitor::Impl::validateParameterPacks(
    std::span<const Parameter> parameters,
    const std::vector<GenericParameterInfo> &genericParameters,
    const TypeRef *returnType) -> void {
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

  const bool functionOwnsPack = pack != nullptr;
  if (pack == nullptr && returnType != nullptr && currentClass &&
      !parameters.empty() && parameters.back().pack) {
    const auto parameterIdentity =
        activeTypePackIdentity(parameters.back().type);
    for (const GenericParameterInfo &ownerParameter :
         classInfo(*currentClass).genericParameters) {
      if (ownerParameter.pack && parameterIdentity == ownerParameter.id) {
        pack = &ownerParameter;
        break;
      }
    }
  }

  const auto isBarePack = [&](const TypeRef &syntax) {
    const SemanticType type = typeOf(syntax);
    return pack != nullptr &&
           (type.kind == SemanticType::TypeParameter ||
            type.kind == SemanticType::TypePack) &&
           activeTypePackIdentity(syntax) == pack->id;
  };
  const auto expandsPack = [&](const auto &self, const TypeRef &type) -> bool {
    if (type.qualifiedOwner && self(self, *type.qualifiedOwner)) {
      return true;
    }
    const auto parameter = resolveTypeParameter(type.name);
    if (type.packExpansion && parameter && pack != nullptr &&
        activeTypePackIdentity(type) == pack->id) {
      return true;
    }
    return std::any_of(
        type.arguments.begin(), type.arguments.end(),
        [&](const TypeRef &argument) { return self(self, argument); });
  };

  if (returnType != nullptr && isBarePack(*returnType)) {
    report(returnType->name.last(),
           "A generic type pack cannot be used as a function return type.",
           "GTI-S2023");
  }

  const Parameter *declaredPack = nullptr;
  bool metadataPack = false;
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const Parameter &parameter = parameters[index];
    const SemanticType parameterType = typeOf(parameter);
    if (!parameter.pack) {
      metadataPack = metadataPack || expandsPack(expandsPack, parameter.type);
      if (isBarePack(parameter.type)) {
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
    if (index + 1 != parameters.size()) {
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
    if (parameterType.kind == SemanticType::TypePack &&
        parameterType.concretePack) {
      for (const SemanticType &element : parameterType.arguments) {
        if (element == SemanticType::Void ||
            element.kind == SemanticType::FunctionSignature) {
          report(*parameter.pack,
                 "A parameter pack requires non-void object types or "
                 "references; function signature elements are not supported.",
                 "GTI-S2023");
          break;
        }
      }
    }
    if (pack == nullptr ||
        (parameterType.kind != SemanticType::TypeParameter &&
         parameterType.kind != SemanticType::TypePack) ||
        activeTypePackIdentity(parameter.type) != pack->id) {
      report(*parameter.pack,
             "A parameter pack type must name the function's or enclosing "
             "class's generic type pack.",
             "GTI-S2023");
    }
  }

  if (functionOwnsPack && declaredPack == nullptr &&
      (returnType == nullptr || !metadataPack)) {
    report(pack->name,
           returnType == nullptr
               ? "A generic type pack must be bound by a final parameter pack."
               : "A generic type pack must be bound by a final parameter pack "
                 "or a type-list expansion in a function parameter type.",
           "GTI-S2023");
  } else if (pack == nullptr && declaredPack != nullptr) {
    report(*declaredPack->pack,
           "A parameter pack requires a matching final generic type pack.",
           "GTI-S2023");
  }
}

auto SemanticVisitor::Impl::readableIteratorValue(
    const SemanticType &type) const -> bool {
  if (type == SemanticType::Void || type == SemanticType::Unknown ||
      type.kind == SemanticType::Reference ||
      // These legacy builtin text handles carry expression-specific owner
      // dependencies that their ordinary type traits alone cannot exclude.
      type.kind == SemanticType::StringView ||
      type.kind == SemanticType::CString)
    return false;
  const auto traits = typeTraits(type);
  return traits.copyable && !traits.containsBorrowedState;
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
  case OverloadedOperator::Conversion:
    break;
  }
  if (name.kind == OverloadedOperator::Conversion &&
      !function.parameters().empty()) {
    fail("A conversion operator takes no parameters; its receiver is the "
         "converted value.");
  } else if (expectedArity && function.parameters().size() != *expectedArity) {
    fail(std::string(operatorSourceSpelling(name.kind)) + " expects " +
         std::to_string(*expectedArity) + " parameter" +
         (*expectedArity == 1 ? "." : "s."));
  }

  const SemanticType returnType =
      typeOf(function.returnType(), function.returnMutability());
  if (function.receiverMutability() == ReceiverMutability::Consuming) {
    if (name.kind != OverloadedOperator::Call && !valueOperator &&
        name.kind != OverloadedOperator::Conversion) {
      fail("A consuming receiver is supported only by value-producing "
           "operators, conversion operators and operator().");
    }
    if (returnType.kind == SemanticType::Reference ||
        typeTraits(returnType).containsBorrowedState) {
      fail(
          "A consuming operator cannot return borrowed state from its consumed "
          "receiver.");
    }
  }
  if ((comparisonOperator || name.kind == OverloadedOperator::ContextualBool) &&
      returnType != SemanticType::Bool) {
    fail(std::string(operatorSourceSpelling(name.kind)) + " must return bool.");
  }
  if (name.kind == OverloadedOperator::Conversion &&
      returnType != SemanticType::Unknown) {
    // One explicitly requested or implicit-declaration step to exactly this
    // value type. Reference and borrow-carrying results need the stored-borrow
    // provenance a conversion site does not yet record.
    if (returnType == SemanticType::Void) {
      fail("A conversion operator must produce a value, not void.");
    } else if (returnType == SemanticType::Bool) {
      fail("Declare the boolean conversion as 'operator bool()'.");
    } else if (returnType.kind == SemanticType::Reference) {
      fail("A conversion operator must produce a value; reference "
           "conversions are not supported yet.");
    } else if (conversionTargetCarriesBorrow(returnType)) {
      fail("A conversion operator cannot produce borrowed state yet; return "
           "an owned value, or provide a named method returning the view.");
    } else if (currentClass && returnType == openClassType(*currentClass)) {
      fail("A conversion operator cannot target its own class.");
    }
    // Every target shares one member name, so virtual dispatch would need a
    // slot per target that the C++ representation does not yet provide.
    const bool interfaceContract =
        currentClass && classInfo(*currentClass).kind == ClassKind::Interface;
    if (function.isVirtual() || function.isPure() || function.isOverride() ||
        interfaceContract) {
      fail("A conversion operator cannot be virtual yet; declare a virtual "
           "named method, and a conversion operator that calls it.");
    }
  }
  if (name.kind == OverloadedOperator::Dereference &&
      returnType.kind != SemanticType::Reference &&
      (function.receiverMutability() != ReceiverMutability::ReadOnly ||
       !readableIteratorValue(returnType))) {
    fail("operator* must return a checked reference, or use a read-only "
         "receiver to return an independent copyable value without borrowed "
         "state.");
  }
  if (name.kind == OverloadedOperator::Arrow &&
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
      function.receiverMutability() != ReceiverMutability::ReadOnly &&
      function.receiverMutability() != ReceiverMutability::Consuming) {
    fail(std::string(operatorSourceSpelling(name.kind)) +
         " must use a read-only or consuming receiver because it produces a "
         "value.");
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
  case SemanticType::Bool:
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
         cAbiPointerPointeeAllowed(type.arguments.front());
}

auto SemanticVisitor::Impl::nativeFunctionBoundaryTypeAllowed(
    const SemanticType &type, bool returnPosition) const -> bool {
  if (returnPosition && type == SemanticType::Void) {
    return true;
  }
  if (type == SemanticType::StringView) {
    return !returnPosition;
  }
  if (isCAbiScalar(type) || type == SemanticType::CString) {
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
    return cAbiPointerPointeeAllowed(type.arguments.front());
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
  if (!isCAbiRawPointer(returnType)) {
    fail(attribute.attribute,
         "[[c_array(count)]] requires a returned C ABI pointer or the "
         "admitted recursive raw-pointer type.");
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
      countType.kind == SemanticType::RawPointer &&
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

auto SemanticVisitor::Impl::validateNativeLinkNameAttribute(
    const FunctionDecl &function) -> void {
  if (!function.nativeLinkName()) {
    return;
  }
  const NativeLinkNameAttribute &attribute = *function.nativeLinkName();
  if (!function.hasCLinkage()) {
    report(attribute.attribute,
           "[[link_name(\"symbol\")]] is valid only on an extern \"C\" "
           "function declaration.",
           "GTI-S2054");
    return;
  }
  (void)validateNativeCIdentifierSpelling(attribute.name(), attribute.symbol,
                                          "GTI-S2054", "extern C link name",
                                          true, true, true);
}

auto SemanticVisitor::Impl::validateExternCVariable(
    const VariableDecl &variable, const SemanticType &type, SymbolId symbol)
    -> void {
  const auto fail = [&](const Token &where, std::string message) {
    report(where, std::move(message), "GTI-S2054");
  };
  const std::string name = variable.nativeLinkName()
                               ? std::string(variable.nativeLinkName()->name())
                               : variable.name().lexeme;
  const Token &location = variable.nativeLinkName()
                              ? variable.nativeLinkName()->symbol
                              : variable.name();
  (void)validateNativeCIdentifierSpelling(
      name, location, "GTI-S2054", "extern C variable", true, true, true);
  if (functionDepth != 0 || currentClass || variable.isStatic() ||
      variable.isConstexpr() || variable.initializer())
    fail(variable.name(),
         "An extern C variable must be a namespace-scope "
         "declaration without static, constexpr, or an initializer.");
  if (name == "main")
    fail(location,
         "The native symbol main is reserved for the GTI entry point.");
  if (!(isCAbiScalar(type) || isCAbiRawPointer(type) ||
        isCAbiRecordType(type) || isCAbiNativeFunction(type)))
    fail(variable.type().name.last(),
         "Foreign variables require a C ABI scalar, "
         "raw pointer, named native function pointer, or passive C ABI record "
         "type.");
  if (typeTraits(type).containsBorrowedState || requiresActiveCleanupFor(type))
    fail(variable.name(),
         "Foreign storage cannot carry GTI loans or cleanup obligations.");
  const auto [previous, inserted] =
      requestSession.externCVariables.emplace(name, &variable);
  if (!inserted && previous->second != &variable) {
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2054", DiagnosticPhase::Semantics, location,
                       "C symbol '" + name + "' is declared more than once.");
    diagnostic.related.push_back(
        {tokenSpan(previous->second->name()), "Previous declaration is here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }
  if (const auto function = externCSymbols.find(name);
      function != externCSymbols.end()) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2054", DiagnosticPhase::Semantics, location,
        "C symbol '" + name + "' conflicts with a C function declaration.");
    diagnostic.related.push_back(
        {tokenSpan(function->second->name()), "Conflicting function is here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }
  if (const auto local = rootNativeStorageSymbols.find(name);
      local != rootNativeStorageSymbols.end() && local->second != &variable) {
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2054", DiagnosticPhase::Semantics, location,
                       "C symbol '" + name +
                           "' conflicts with a root namespace GTI variable.");
    diagnostic.related.push_back(
        {tokenSpan(local->second->name()), "Conflicting storage is here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }
  semanticModel.recordForeignStorage({.symbol = symbol,
                                      .type = type,
                                      .access = variable.isMutable()
                                                    ? AccessMode::Mutable
                                                    : AccessMode::ReadOnly,
                                      .externalSymbol = name});
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
  const FunctionInfo *functionInfo = semanticModel.findFunction(function);
  const std::string_view externalSymbol =
      functionInfo == nullptr ? std::string_view(function.name().lexeme)
                              : std::string_view(functionInfo->externalSymbol);
  const Token &externalLocation = function.nativeLinkName()
                                      ? function.nativeLinkName()->symbol
                                      : function.name();
  if (!function.nativeLinkName()) {
    (void)validateNativeCIdentifier(function.name(), "GTI-S2054",
                                    "extern C symbol", true, true, true);
  }
  if (methodDeclaration || currentClass) {
    fail(function.name(),
         "extern \"C\" declarations must be namespace-scope free "
         "functions.");
  }
  if (externalSymbol == "main") {
    fail(externalLocation,
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
       !isCAbiRawPointer(returnType) && !isCAbiRecordType(returnType) &&
       !isCAbiNativeFunction(returnType))) {
    fail(function.returnType().name.last(),
         "extern \"C\" return types are limited to void, fixed-width "
         "integer or float scalars, c_string, valid [[c_abi]] records, and "
         "recursive raw pointers to those types, opaque C handles, void, "
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
         !isCAbiRawPointer(parameterType) && !isCAbiRecordType(parameterType) &&
         !isCAbiNativeFunction(parameterType))) {
      const Token &location = parameter.name.lexeme.empty()
                                  ? parameter.type.name.last()
                                  : parameter.name;
      fail(location,
           "extern \"C\" parameters are limited to immutable by-value "
           "fixed-width scalars, valid [[c_abi]] records, std::string_view "
           "counted buffers, c_string NUL-terminated pointers, and "
           "recursive raw pointers to those record or scalar types, opaque "
           "C handles, void, or exact "
           "named native function types.");
    }
  }

  if (const auto variable =
          requestSession.externCVariables.find(std::string(externalSymbol));
      variable != requestSession.externCVariables.end()) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2054", DiagnosticPhase::Semantics, externalLocation,
        "C symbol '" + std::string(externalSymbol) +
            "' conflicts with a C variable declaration.");
    diagnostic.related.push_back(
        {tokenSpan(variable->second->name()), "Conflicting storage is here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }
  const auto [previous, inserted] =
      externCSymbols.emplace(std::string(externalSymbol), &function);
  if (!inserted && previous->second != &function) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2054", DiagnosticPhase::Semantics, externalLocation,
        "C symbol '" + std::string(externalSymbol) +
            "' is declared more than once; extern \"C\" functions "
            "cannot be overloaded or redeclared.");
    const Token &previousLocation =
        previous->second->nativeLinkName()
            ? previous->second->nativeLinkName()->symbol
            : previous->second->name();
    diagnostic.related.push_back({tokenSpan(previousLocation),
                                  "Previous C-linkage declaration is here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }

  if (const auto storage =
          rootNativeStorageSymbols.find(std::string(externalSymbol));
      storage != rootNativeStorageSymbols.end()) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2054", DiagnosticPhase::Semantics, externalLocation,
        "C symbol '" + std::string(externalSymbol) +
            "' conflicts with a root namespace GTI variable.");
    diagnostic.related.push_back(
        {tokenSpan(storage->second->name()),
         "Conflicting GTI variable is declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }
}

} // namespace lang
