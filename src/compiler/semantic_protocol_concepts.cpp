// Protocol concepts and conformance blocks.
//
// A concept with a body declares associated types and operation signatures.
// A type satisfies it only through a `for <Parameters> concept C<Pattern>`
// block, which supplies a `using` type for each associated name and a
// function for each operation. The parser places the block's functions in a
// generated namespace as ordinary generic functions over the block's
// parameters, so calls to them are ordinary calls through HIR and MIR. This
// file owns the registry: block validation, selection for a concrete type,
// associated type projection, and resolution of `C<X>::operation(...)`.
#include "semantic_analyzer_impl.h"

#include <algorithm>
#include <unordered_set>

namespace lang {

namespace {

// Projections reduce through block types, which may themselves name
// projections; a cyclic block must not recurse without bound.
constexpr std::size_t maximumProjectionDepth = 64;
thread_local std::size_t projectionDepth = 0;

template <class Names>
[[nodiscard]] std::optional<std::uint32_t> memberIndex(const Names &names,
                                                       std::string_view name) {
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (names[index] == name) {
      return static_cast<std::uint32_t>(index);
    }
  }
  return std::nullopt;
}

template <class Operations>
[[nodiscard]] std::optional<std::size_t>
operationIndex(const Operations &operations, std::string_view name) {
  for (std::size_t index = 0; index < operations.size(); ++index) {
    if (operations[index].name == name) {
      return index;
    }
  }
  return std::nullopt;
}

} // namespace

auto SemanticVisitor::Impl::reportConformanceError(const Token &site,
                                                   std::string message,
                                                   const Token *related,
                                                   std::string relatedMessage)
    -> void {
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2123", DiagnosticPhase::Semantics, site, std::move(message));
  if (related != nullptr) {
    diagnostic.related.push_back(
        {tokenSpan(*related), std::move(relatedMessage)});
  }
  diagnostics.emplace_back(std::move(diagnostic));
}

// Called by resolveConcept with the concept's parameter scope active.
auto SemanticVisitor::Impl::resolveProtocolConcept(
    RegisteredConcept &conceptInfo, bool &valid) -> void {
  const ConceptDecl &declaration = *conceptInfo.declaration;
  const ProtocolConceptBody &body = *declaration.protocolBody();
  conceptInfo.protocol = true;
  conceptInfo.associatedTypes.clear();
  conceptInfo.operations.clear();
  if (declaration.typeParameters().size() != 1 ||
      conceptInfo.parameterIds.size() != 1) {
    reportConformanceError(
        declaration.name(),
        "A concept with a body declares exactly one type parameter.");
    valid = false;
    return;
  }
  const Token &parameterName = declaration.typeParameters().front();
  const SemanticType parameter =
      SemanticType::typeParameter(conceptInfo.parameterIds.front());
  std::unordered_set<std::string> names;
  const auto claim = [&](const Token &name) {
    if (name.lexeme == parameterName.lexeme ||
        name.lexeme == declaration.name().lexeme ||
        !names.insert(name.lexeme).second) {
      reportConformanceError(name, "Duplicate member name '" + name.lexeme +
                                       "' in concept '" +
                                       conceptInfo.qualifiedName + "'.");
      valid = false;
      return false;
    }
    return true;
  };

  // Associated names type as projections of the parameter, so an operation
  // signature can name them directly.
  for (const ProtocolAssociatedType &associated : body.associatedTypes) {
    if (!claim(associated.name)) {
      continue;
    }
    const auto member =
        static_cast<std::uint32_t>(conceptInfo.associatedTypes.size());
    conceptInfo.associatedTypes.push_back(associated.name.lexeme);
    typeParameterScopes.back().emplace(
        associated.name.lexeme,
        associatedTypeProjection(conceptInfo.id, member, parameter));
  }

  for (const StmtPtr &statement : body.operations) {
    const auto *operation = stmtAs<FunctionDecl>(statement.get());
    if (operation == nullptr || !claim(operation->name())) {
      continue;
    }
    const auto reject = [&](const Token &site, std::string_view what) {
      reportConformanceError(site, "Concept operation '" +
                                       operation->name().lexeme + "' " +
                                       std::string(what) + ".");
      valid = false;
    };
    if (operation->operatorName()) {
      reject(operation->name(), "cannot be an operator");
      continue;
    }
    if (!operation->genericParameters().empty()) {
      reject(operation->name(), "cannot declare generic parameters");
    }
    if (operation->requiresClause()) {
      reject(operation->requiresClause()->keyword,
             "cannot declare a requires clause");
    }
    if (operation->isStatic() || operation->isVirtual() ||
        operation->isConstexpr() || operation->unsafeKeyword() ||
        operation->receiverMutability() != ReceiverMutability::ReadOnly ||
        !operation->functionAttributes().empty() ||
        operation->runtimeBinding() || operation->hasCLinkage()) {
      reject(operation->name(),
             "is a free function signature and takes no modifiers, "
             "attributes or receiver qualifier");
    }
    validateType(operation->returnType());
    RegisteredConcept::ProtocolOperation resolved{
        .name = operation->name().lexeme,
        .declaration = operation,
        .returnType =
            typeOf(operation->returnType(), operation->returnMutability(),
                   conceptInfo.namespaceScope)};
    for (const Parameter &formal : operation->parameters()) {
      if (formal.pack || formal.hasDefault()) {
        reject(formal.name, "cannot declare a parameter pack or default");
      }
      validateType(formal.type);
      resolved.parameterTypes.push_back(
          typeOf(formal, conceptInfo.namespaceScope));
    }
    conceptInfo.operations.push_back(std::move(resolved));
  }
}

auto SemanticVisitor::Impl::registerConformances(const StmtList &statements,
                                                 std::vector<std::string> scope)
    -> void {
  const SourceUnitId enclosingSourceUnit = currentSourceUnit;
  const std::vector<std::string> enclosingNamespace = currentNamespace;
  for (const StmtPtr &statement : statements) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        registerConformances(*branch, scope);
      }
      continue;
    }
    if (const auto *namespaceDecl = stmtAs<NamespaceDecl>(statement.get())) {
      scope.emplace_back(namespaceDecl->name().lexeme);
      registerConformances(namespaceDecl->declarations(), scope);
      scope.pop_back();
      continue;
    }
    const auto *declaration = stmtAs<ConformanceDecl>(statement.get());
    if (declaration == nullptr) {
      continue;
    }

    // The block is declared in the namespace enclosing its generated one.
    std::vector<std::string> enclosing = scope;
    if (!enclosing.empty()) {
      enclosing.pop_back();
    }
    currentSourceUnit = sourceUnitFor(declaration->forKeyword());
    currentNamespace = enclosing;
    const ConceptApplication &target = declaration->target();
    const Token &site = target.name.last();
    const std::optional<ConceptId> conceptId =
        resolveConceptPath(target.name, enclosing);
    if (!conceptId || *conceptId == 0 || *conceptId > concepts.size()) {
      reportConformanceError(site, "Unknown or invisible concept '" +
                                       pathSpelling(target.name) + "'.");
      continue;
    }
    (void)resolveConcept(*conceptId, &site);
    const RegisteredConcept &conceptInfo = concepts[*conceptId - 1];
    if (!conceptInfo.protocol) {
      reportConformanceError(
          site,
          "Concept '" + conceptInfo.qualifiedName +
              "' has no body; a conformance block supplies only a concept "
              "declared with '{ ... }'.",
          &conceptInfo.declaration->name(), "Concept declared here.");
      continue;
    }
    if (target.arguments.size() != 1) {
      reportConformanceError(site, "A conformance block applies concept '" +
                                       conceptInfo.qualifiedName +
                                       "' to exactly one type.");
      continue;
    }

    const bool hasFunctions =
        std::any_of(declaration->members().begin(),
                    declaration->members().end(), [](const StmtPtr &member) {
                      return stmtAs<FunctionDecl>(member.get()) != nullptr;
                    });
    // Every block function carries these parameters and reports their
    // problems itself; the block reports them only when it has no function.
    const std::size_t parameterDiagnostics = diagnostics.size();
    std::vector<GenericParameterInfo> parameters = makeGenericParameters(
        declaration->genericParameters(), declaration->forKeyword(), enclosing);
    if (hasFunctions) {
      diagnostics.erase(diagnostics.begin() +
                            static_cast<std::ptrdiff_t>(parameterDiagnostics),
                        diagnostics.end());
    }
    bool valid = parameters.size() == declaration->genericParameters().size();
    for (const GenericParameterInfo &parameter : parameters) {
      if (parameter.pack || parameter.value) {
        reportConformanceError(parameter.name,
                               "Conformance block parameters are type "
                               "parameters; packs and value parameters are "
                               "not supported.");
        valid = false;
      }
    }

    beginTypeParameterScope(parameters);
    validateType(target.arguments.front());
    SemanticType pattern = typeOf(target.arguments.front(), enclosing);
    ClassId patternTemplate = 0;
    if (pattern == SemanticType::Unknown) {
      valid = false;
    } else if (parameters.empty()) {
      if (containsOpenTypeParameter(pattern)) {
        reportConformanceError(site, "A conformance block without parameters "
                                     "needs a concrete type.");
        valid = false;
      }
      pattern = canonicalExactSpecializationType(std::move(pattern));
    } else {
      // A class template applied to each block parameter exactly once.
      bool shape = pattern.kind == SemanticType::Class &&
                   pattern.classId != 0 && pattern.classId <= classes.size() &&
                   classInfo(pattern.classId).exactSpecializationPrimary == 0 &&
                   pattern.valueArguments.empty() &&
                   pattern.arguments.size() == parameters.size();
      std::unordered_set<GenericParameterId> used;
      if (shape) {
        for (const SemanticType &argument : pattern.arguments) {
          shape = shape && argument.kind == SemanticType::TypeParameter &&
                  !argument.isAssociatedTypeProjection() &&
                  used.insert(argument.genericParameterId).second;
        }
        for (const GenericParameterInfo &parameter : parameters) {
          shape = shape && used.contains(parameter.id);
        }
      }
      if (!shape) {
        reportConformanceError(
            site, "A generic conformance block's pattern applies one class "
                  "template to each of the block's parameters exactly once, "
                  "such as 'Box<T>'.");
        valid = false;
      } else {
        patternTemplate = pattern.classId;
      }
    }

    if (valid) {
      const bool ownsConcept =
          samePackageOwnershipDomain(currentSourceUnit, conceptInfo.sourceUnit);
      const bool ownsType =
          patternTemplate != 0
              ? samePackageOwnershipDomain(
                    currentSourceUnit, classInfo(patternTemplate).sourceUnit)
              : ownsNominalSpecializationArgument(currentSourceUnit, pattern);
      if (!ownsConcept && !ownsType) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2123", DiagnosticPhase::Semantics, site,
            "This package cannot declare a conformance block for '" +
                conceptInfo.qualifiedName + "<" + typeSpelling(pattern) +
                ">'; it owns neither the concept nor the type.");
        diagnostic.related.push_back(
            {tokenSpan(conceptInfo.declaration->name()),
             "Concept declared here."});
        diagnostic.hints.emplace_back(
            "Declare the block in the concept's or the type's package, or "
            "wrap the type in one this package owns.");
        diagnostics.emplace_back(std::move(diagnostic));
        valid = false;
      }
    }

    if (valid) {
      const auto instanceOf = [&](const SemanticType &type, ClassId owner) {
        return type.kind == SemanticType::Class && type.classId != 0 &&
               type.classId <= classes.size() &&
               (type.classId == owner ||
                classInfo(type.classId).exactSpecializationPrimary == owner);
      };
      for (const RegisteredConformance &other : conformances) {
        if (other.conceptId != *conceptId) {
          continue;
        }
        const bool overlap = other.patternTemplate != 0 && patternTemplate != 0
                                 ? other.patternTemplate == patternTemplate
                             : other.patternTemplate != 0
                                 ? instanceOf(pattern, other.patternTemplate)
                             : patternTemplate != 0
                                 ? instanceOf(other.pattern, patternTemplate)
                                 : other.pattern == pattern;
        if (overlap) {
          reportConformanceError(
              site,
              "This conformance block for concept '" +
                  conceptInfo.qualifiedName +
                  "' overlaps another; a type matches at most one block.",
              &other.declaration->target().name.last(),
              "Overlapping conformance block is here.");
          valid = false;
          break;
        }
      }
    }

    std::vector<SemanticType> associatedTypes(
        conceptInfo.associatedTypes.size(), SemanticType::Unknown);
    std::vector<bool> defined(conceptInfo.associatedTypes.size(), false);
    // Problems below leave the block registered, so its operations are still
    // checked and uses of it do not report a second, missing-block error.
    for (const StmtPtr &member : declaration->members()) {
      const auto *alias = stmtAs<TypeAliasDecl>(member.get());
      if (alias == nullptr) {
        continue;
      }
      const std::optional<std::uint32_t> index =
          memberIndex(conceptInfo.associatedTypes, alias->name().lexeme);
      if (!index) {
        reportConformanceError(alias->name(), "'" + alias->name().lexeme +
                                                  "' is not an associated "
                                                  "type of concept '" +
                                                  conceptInfo.qualifiedName +
                                                  "'.");
        continue;
      }
      if (defined[*index]) {
        reportConformanceError(alias->name(),
                               "Duplicate definition of associated type '" +
                                   alias->name().lexeme + "'.");
        continue;
      }
      defined[*index] = true;
      validateType(alias->target());
      associatedTypes[*index] = typeOf(alias->target(), enclosing);
    }
    for (std::size_t index = 0; index < defined.size(); ++index) {
      if (!defined[index]) {
        reportConformanceError(
            site, "The conformance block does not define associated type '" +
                      conceptInfo.associatedTypes[index] + "' of concept '" +
                      conceptInfo.qualifiedName + "'.");
      }
    }
    endTypeParameterScope();
    if (!valid) {
      continue;
    }
    conformances.push_back({.conceptId = *conceptId,
                            .sourceUnit = currentSourceUnit,
                            .declaration = declaration,
                            .namespaceScope = enclosing,
                            .functionScope = scope,
                            .genericParameters = std::move(parameters),
                            .pattern = std::move(pattern),
                            .patternTemplate = patternTemplate,
                            .associatedTypes = std::move(associatedTypes)});
  }
  currentSourceUnit = enclosingSourceUnit;
  currentNamespace = enclosingNamespace;
}

// After namespace symbols exist: select each block's function for every
// operation and require the concept's exact signature.
auto SemanticVisitor::Impl::bindConformanceOperations() -> void {
  for (RegisteredConformance &conformance : conformances) {
    if (conformance.operationsBound) {
      continue;
    }
    conformance.operationsBound = true;
    const RegisteredConcept &conceptInfo = concepts[conformance.conceptId - 1];
    conformance.operations.assign(conceptInfo.operations.size(), Symbol{});
    const ConformanceDecl &declaration = *conformance.declaration;
    const Token &site = declaration.target().name.last();
    currentSourceUnit = conformance.sourceUnit;
    currentNamespace = conformance.namespaceScope;

    // Both signatures in the block's parameter space, with projections of
    // the block's own pattern resolved to its types.
    const auto resolveOwn = [&](const auto &self,
                                SemanticType value) -> SemanticType {
      if (value.isAssociatedTypeProjection() &&
          value.projectionConcept == conformance.conceptId &&
          value.arguments.size() == 1 &&
          value.arguments.front() == conformance.pattern &&
          value.projectionMember < conformance.associatedTypes.size()) {
        return conformance.associatedTypes[value.projectionMember];
      }
      for (SemanticType &argument : value.arguments) {
        argument = self(self, std::move(argument));
      }
      return value;
    };
    GenericSubstitution toBlock;
    toBlock.types.emplace(conceptInfo.parameterIds.front(),
                          conformance.pattern);
    const auto expected = [&](const SemanticType &type) {
      return resolveOwn(resolveOwn, substituteType(type, toBlock));
    };

    std::vector<bool> defined(conceptInfo.operations.size(), false);
    for (const StmtPtr &member : declaration.members()) {
      const auto *function = stmtAs<FunctionDecl>(member.get());
      if (function == nullptr) {
        continue;
      }
      const std::optional<std::size_t> index =
          operationIndex(conceptInfo.operations, function->name().lexeme);
      if (!index) {
        reportConformanceError(function->name(),
                               "'" + function->name().lexeme +
                                   "' is not an operation of concept '" +
                                   conceptInfo.qualifiedName + "'.");
        continue;
      }
      if (defined[*index]) {
        reportConformanceError(function->name(),
                               "Duplicate definition of operation '" +
                                   function->name().lexeme + "'.");
        continue;
      }
      defined[*index] = true;
      if (function->operatorName() || function->isStatic() ||
          function->isVirtual() ||
          function->receiverMutability() != ReceiverMutability::ReadOnly ||
          function->runtimeBinding() || function->hasCLinkage()) {
        reportConformanceError(function->name(),
                               "Conformance block operation '" +
                                   function->name().lexeme +
                                   "' is an ordinary function definition "
                                   "without modifiers.");
        continue;
      }
      const auto found = catalog.namespaceSymbols.find(
          qualifiedName(conformance.functionScope, function->name().lexeme));
      if (found == catalog.namespaceSymbols.end() ||
          found->second.overloads.size() != 1) {
        continue;
      }
      const CallableCandidate &candidate = found->second.overloads.front();
      const std::vector<GenericParameterInfo> &functionParameters =
          candidate.record().genericParameters;
      if (functionParameters.size() != conformance.genericParameters.size()) {
        continue;
      }
      GenericSubstitution toBlockParameters;
      for (std::size_t parameter = 0; parameter < functionParameters.size();
           ++parameter) {
        toBlockParameters.types.emplace(
            functionParameters[parameter].id,
            SemanticType::typeParameter(
                conformance.genericParameters[parameter].id));
      }
      const RegisteredConcept::ProtocolOperation &operation =
          conceptInfo.operations[*index];
      if (std::find(conformance.associatedTypes.begin(),
                    conformance.associatedTypes.end(), SemanticType::Unknown) !=
          conformance.associatedTypes.end()) {
        // Already reported; an incomplete block's signatures cannot be
        // compared.
        conformance.operations[*index] = found->second;
        continue;
      }
      const auto actual = [&](const SemanticType &type) {
        return resolveOwn(resolveOwn, substituteType(type, toBlockParameters));
      };
      const SemanticType expectedReturn = expected(operation.returnType);
      bool exact =
          actual(candidate.returnType) == expectedReturn &&
          candidate.parameterTypes.size() == operation.parameterTypes.size();
      std::string expectedSignature =
          typeSpelling(expectedReturn) + " " + operation.name + "(";
      for (std::size_t parameter = 0;
           parameter < operation.parameterTypes.size(); ++parameter) {
        const SemanticType expectedParameter =
            expected(operation.parameterTypes[parameter]);
        expectedSignature +=
            (parameter == 0 ? "" : ", ") + typeSpelling(expectedParameter);
        exact =
            exact && parameter < candidate.parameterTypes.size() &&
            actual(candidate.parameterTypes[parameter]) == expectedParameter;
      }
      expectedSignature += ")";
      if (!exact) {
        reportConformanceError(
            function->name(),
            "Operation '" + function->name().lexeme +
                "' must have the signature '" + expectedSignature +
                "' that concept '" + conceptInfo.qualifiedName +
                "' declares for this block.",
            &operation.declaration->name(), "Operation declared here.");
        continue;
      }
      conformance.operations[*index] = found->second;
    }
    for (std::size_t index = 0; index < defined.size(); ++index) {
      if (!defined[index]) {
        reportConformanceError(
            site,
            "The conformance block does not define operation '" +
                conceptInfo.operations[index].name + "' of concept '" +
                conceptInfo.qualifiedName + "'.",
            &conceptInfo.operations[index].declaration->name(),
            "Operation declared here.");
      }
    }
  }
}

auto SemanticVisitor::Impl::matchConformance(ConceptId id,
                                             const SemanticType &argument) const
    -> std::optional<ConformanceMatch> {
  if (containsOpenTypeParameter(argument)) {
    return std::nullopt;
  }
  const SemanticType key = canonicalExactSpecializationType(argument);
  for (std::size_t index = 0; index < conformances.size(); ++index) {
    const RegisteredConformance &conformance = conformances[index];
    if (conformance.conceptId != id) {
      continue;
    }
    if (conformance.patternTemplate == 0) {
      if (conformance.pattern == key) {
        return ConformanceMatch{.conformance = index + 1};
      }
      continue;
    }
    if (argument.kind != SemanticType::Class || argument.classId == 0 ||
        argument.classId > classes.size()) {
      continue;
    }
    ClassId owner = argument.classId;
    std::vector<SemanticType> arguments = argument.arguments;
    if (const ClassInfo &info = classInfo(owner);
        info.exactSpecializationPrimary != 0) {
      owner = info.exactSpecializationPrimary;
      arguments = info.exactTypeArguments;
    }
    if (owner != conformance.patternTemplate ||
        arguments.size() != conformance.pattern.arguments.size()) {
      continue;
    }
    ConformanceMatch match{.conformance = index + 1};
    match.binding.assign(conformance.genericParameters.size(),
                         SemanticType::Unknown);
    for (std::size_t position = 0; position < arguments.size(); ++position) {
      for (std::size_t parameter = 0;
           parameter < conformance.genericParameters.size(); ++parameter) {
        if (conformance.genericParameters[parameter].id ==
            conformance.pattern.arguments[position].genericParameterId) {
          match.binding[parameter] = arguments[position];
        }
      }
    }
    return match;
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::protocolConceptOwner(
    const TypeRef &owner, const std::vector<std::string> &fromScope) const
    -> std::optional<ConceptId> {
  if (owner.qualifiedOwner) {
    return std::nullopt;
  }
  const std::optional<ConceptId> id = resolveConceptPath(owner.name, fromScope);
  if (!id || *id == 0 || *id > concepts.size() || !concepts[*id - 1].protocol) {
    return std::nullopt;
  }
  return id;
}

auto SemanticVisitor::Impl::associatedTypeProjection(
    ConceptId id, std::uint32_t member, SemanticType argument) const
    -> SemanticType {
  SemanticType projection(SemanticType::TypeParameter, {std::move(argument)});
  projection.projectionConcept = id;
  projection.projectionMember = member;
  return reduceAssociatedTypeProjection(std::move(projection));
}

auto SemanticVisitor::Impl::reduceAssociatedTypeProjection(
    SemanticType projection) const -> SemanticType {
  if (!projection.isAssociatedTypeProjection() ||
      projection.arguments.size() != 1 ||
      containsOpenTypeParameter(projection.arguments.front())) {
    return projection;
  }
  const std::optional<ConformanceMatch> match = matchConformance(
      projection.projectionConcept, projection.arguments.front());
  if (!match) {
    return projection;
  }
  const RegisteredConformance &conformance =
      conformances[match->conformance - 1];
  if (projection.projectionMember >= conformance.associatedTypes.size() ||
      projectionDepth >= maximumProjectionDepth) {
    return SemanticType::Unknown;
  }
  GenericSubstitution substitution;
  for (std::size_t parameter = 0;
       parameter < conformance.genericParameters.size(); ++parameter) {
    substitution.types.emplace(conformance.genericParameters[parameter].id,
                               match->binding[parameter]);
  }
  struct DepthScope {
    DepthScope() { ++projectionDepth; }
    ~DepthScope() { --projectionDepth; }
    DepthScope(const DepthScope &) = delete;
    DepthScope &operator=(const DepthScope &) = delete;
  } depth;
  return substituteType(
      conformance.associatedTypes[projection.projectionMember], substitution);
}

// The block whose functions are declared in fromScope, if any.
auto SemanticVisitor::Impl::enclosingConformance(
    const std::vector<std::string> &fromScope) const
    -> const RegisteredConformance * {
  if (fromScope.empty() ||
      !fromScope.back().starts_with("__gti_conformance_")) {
    return nullptr;
  }
  const auto found =
      std::find_if(conformances.begin(), conformances.end(),
                   [&](const RegisteredConformance &conformance) {
                     return conformance.functionScope == fromScope;
                   });
  return found == conformances.end() ? nullptr : &*found;
}

// Block types are in the block's parameter space. A block function names the
// same parameters through its own copies, found by name in its scope.
auto SemanticVisitor::Impl::blockScopeType(
    const RegisteredConformance &conformance, const SemanticType &type) const
    -> SemanticType {
  GenericSubstitution substitution;
  for (const GenericParameterInfo &parameter : conformance.genericParameters) {
    if (const std::optional<SemanticType> own =
            resolveTypeParameter(NamePath(parameter.name))) {
      substitution.types.emplace(parameter.id, *own);
    }
  }
  return substituteType(
      substituteType(type, substitution),
      GenericSubstitution{.types = instanceTypeSubstitution,
                          .values = instanceValueSubstitution});
}

// Inside a block's function its own associated types are transparent: an
// associated name, or a projection of the block's own pattern, is the
// block's `using` type.
auto SemanticVisitor::Impl::blockAssociatedType(
    std::string_view name, const std::vector<std::string> &fromScope) const
    -> std::optional<SemanticType> {
  const RegisteredConformance *conformance = enclosingConformance(fromScope);
  if (conformance == nullptr) {
    return std::nullopt;
  }
  const std::optional<std::uint32_t> member =
      memberIndex(concepts[conformance->conceptId - 1].associatedTypes, name);
  if (!member || *member >= conformance->associatedTypes.size()) {
    return std::nullopt;
  }
  return blockScopeType(*conformance, conformance->associatedTypes[*member]);
}

auto SemanticVisitor::Impl::ownBlockProjection(
    ConceptId id, std::uint32_t member, const SemanticType &argument,
    const std::vector<std::string> &fromScope) const
    -> std::optional<SemanticType> {
  const RegisteredConformance *conformance = enclosingConformance(fromScope);
  if (conformance == nullptr || conformance->conceptId != id ||
      member >= conformance->associatedTypes.size() ||
      typeOf(conformance->declaration->target().arguments.front(), fromScope) !=
          argument) {
    return std::nullopt;
  }
  return blockScopeType(*conformance, conformance->associatedTypes[member]);
}

auto SemanticVisitor::Impl::protocolOperationType(
    ConceptId id, const SemanticType &type, const SemanticType &argument) const
    -> SemanticType {
  const RegisteredConcept &conceptInfo = concepts[id - 1];
  GenericSubstitution substitution;
  substitution.types.emplace(conceptInfo.parameterIds.front(), argument);
  return substituteType(type, substitution);
}

auto SemanticVisitor::Impl::protocolConformanceFailure(
    const AppliedConceptRequirement &requirement) const
    -> std::optional<ConstraintFailure> {
  const RegisteredConcept &conceptInfo = concepts[requirement.conceptId - 1];
  const SemanticType argument = requirement.arguments.empty()
                                    ? SemanticType::Unknown
                                    : requirement.arguments.front();
  ConstraintFailure failure{
      .parameter = requirement.syntax != nullptr &&
                           !requirement.syntax->arguments.empty()
                       ? requirement.syntax->arguments.front().name.last()
                   : requirement.syntax != nullptr
                       ? requirement.syntax->name.last()
                       : conceptInfo.declaration->name(),
      .argument = argument,
      .constraintName = requirement.syntax != nullptr
                            ? std::optional<NamePath>(requirement.syntax->name)
                            : std::nullopt,
      .application = requirement.syntax,
      .leafApplication = requirement.syntax,
      .applicationArgumentIndex = 0,
      .missingConformance = true};
  if (requirement.arguments.size() != 1 ||
      containsOpenTypeParameter(argument)) {
    return failure;
  }
  const std::optional<ConformanceMatch> match =
      matchConformance(requirement.conceptId, argument);
  if (!match) {
    return failure;
  }
  const RegisteredConformance &conformance =
      conformances[match->conformance - 1];
  return firstConstraintFailure(conformance.genericParameters, match->binding);
}

auto SemanticVisitor::Impl::validateAssociatedTypePath(const TypeRef &type,
                                                       ConceptId id) -> void {
  const TypeRef &owner = *type.qualifiedOwner;
  const RegisteredConcept &conceptInfo = concepts[id - 1];
  for (const TypeRef &argument : owner.arguments) {
    validateType(argument);
  }
  const Token &site = type.name.last();
  if (owner.arguments.size() != 1) {
    reportConformanceError(site, "Concept '" + conceptInfo.qualifiedName +
                                     "' takes exactly one type argument.");
    return;
  }
  if (!type.arguments.empty()) {
    reportConformanceError(site,
                           "An associated type takes no generic arguments.");
    return;
  }
  if (!memberIndex(conceptInfo.associatedTypes, site.lexeme)) {
    reportConformanceError(site, "Concept '" + conceptInfo.qualifiedName +
                                     "' has no associated type '" +
                                     site.lexeme + "'.");
    return;
  }
  const SemanticType argument = typeOf(owner.arguments.front());
  if (argument == SemanticType::Unknown) {
    return;
  }
  if (containsOpenTypeParameter(argument)) {
    if (firstConceptApplicationFailure(
            {.conceptId = id, .arguments = {argument}})) {
      reportConformanceError(
          site, "'" + conceptInfo.qualifiedName + "<" + typeSpelling(argument) +
                    ">::" + site.lexeme + "' requires 'requires " +
                    conceptInfo.qualifiedName + "<" + typeSpelling(argument) +
                    ">' on the enclosing declaration.");
    }
    return;
  }
  if (!matchConformance(id, argument)) {
    reportConformanceError(
        site, "No conformance block 'for concept " + conceptInfo.qualifiedName +
                  "<" + typeSpelling(argument) + ">' is declared.");
  }
}

auto SemanticVisitor::Impl::protocolOperationTarget(const QualifiedName &callee)
    const -> std::optional<ProtocolOperationTarget> {
  if (!callee.ownerType()) {
    return std::nullopt;
  }
  const TypeRef &owner = *callee.ownerType();
  const std::optional<ConceptId> id =
      protocolConceptOwner(owner, currentNamespace);
  if (!id || owner.arguments.size() != 1) {
    return std::nullopt;
  }
  const SemanticType argument =
      substituteType(typeOf(owner.arguments.front()),
                     GenericSubstitution{.types = instanceTypeSubstitution,
                                         .values = instanceValueSubstitution});
  const std::optional<ConformanceMatch> match = matchConformance(*id, argument);
  if (!match) {
    return std::nullopt;
  }
  const RegisteredConformance &conformance =
      conformances[match->conformance - 1];
  const std::optional<std::size_t> operation =
      operationIndex(concepts[*id - 1].operations, callee.name().last().lexeme);
  if (!operation || *operation >= conformance.operations.size() ||
      conformance.operations[*operation].overloads.empty()) {
    return std::nullopt;
  }
  return ProtocolOperationTarget{.symbol = conformance.operations[*operation],
                                 .typeArguments = match->binding};
}

// Returns true when the call is fully analyzed here: an error, or a call
// through `requires C<T>` in a generic body. A concrete operand continues on
// the ordinary call path with the selected block function.
auto SemanticVisitor::Impl::analyzeProtocolOperationCall(
    const Call &expr, const QualifiedName &callee, ConceptId id) -> bool {
  const TypeRef &owner = *callee.ownerType();
  const RegisteredConcept &conceptInfo = concepts[id - 1];
  const auto fail = [&]() {
    for (const ExprPtr &argument : expr.arguments()) {
      analyze(argument);
    }
    currentType = SemanticType::Unknown;
    return true;
  };
  for (const TypeRef &argument : owner.arguments) {
    validateType(argument);
  }
  const Token &site = callee.name().last();
  if (owner.arguments.size() != 1) {
    reportConformanceError(site, "Concept '" + conceptInfo.qualifiedName +
                                     "' takes exactly one type argument.");
    return fail();
  }
  if (!expr.typeArguments().empty()) {
    reportConformanceError(
        site, "A concept operation takes no explicit generic arguments.");
    return fail();
  }
  const std::optional<std::size_t> index =
      operationIndex(conceptInfo.operations, site.lexeme);
  if (!index) {
    reportConformanceError(site, "Concept '" + conceptInfo.qualifiedName +
                                     "' has no operation '" + site.lexeme +
                                     "'.");
    return fail();
  }
  const SemanticType argument =
      substituteType(typeOf(owner.arguments.front()),
                     GenericSubstitution{.types = instanceTypeSubstitution,
                                         .values = instanceValueSubstitution});
  if (argument == SemanticType::Unknown) {
    return fail();
  }
  if (!containsOpenTypeParameter(argument)) {
    if (!protocolOperationTarget(callee)) {
      reportConformanceError(site, "No conformance block 'for concept " +
                                       conceptInfo.qualifiedName + "<" +
                                       typeSpelling(argument) +
                                       ">' is declared.");
      return fail();
    }
    return false;
  }
  if (firstConceptApplicationFailure(
          {.conceptId = id, .arguments = {argument}})) {
    reportConformanceError(
        site, "Calling '" + conceptInfo.qualifiedName + "<" +
                  typeSpelling(argument) + ">::" + site.lexeme +
                  "' requires 'requires " + conceptInfo.qualifiedName + "<" +
                  typeSpelling(argument) + ">' on the enclosing declaration.");
    return fail();
  }

  // Through the requirement: check the arguments against the declared
  // signature. Each concrete instance resolves the block function itself.
  const RegisteredConcept::ProtocolOperation &operation =
      conceptInfo.operations[*index];
  std::vector<SemanticType> parameterTypes;
  parameterTypes.reserve(operation.parameterTypes.size());
  for (const SemanticType &parameter : operation.parameterTypes) {
    parameterTypes.push_back(protocolOperationType(id, parameter, argument));
  }
  const SemanticType returnType =
      protocolOperationType(id, operation.returnType, argument);
  if (expr.arguments().size() != parameterTypes.size()) {
    report(expr.paren(),
           "Function expects " + std::to_string(parameterTypes.size()) +
               " argument" + (parameterTypes.size() == 1 ? "" : "s") +
               " but received " + std::to_string(expr.arguments().size()) + ".",
           "GTI-S2005");
    return fail();
  }
  bool valid = true;
  for (std::size_t position = 0; position < parameterTypes.size(); ++position) {
    const SemanticType actual = analyze(expr.arguments()[position]);
    if (actual == SemanticType::Unknown) {
      valid = false;
      continue;
    }
    if (!callArgumentMatches(parameterTypes[position], actual,
                             expr.arguments()[position])) {
      report(expressionToken(expr.arguments()[position]),
             "Argument " + std::to_string(position + 1) + " of '" +
                 site.lexeme + "' has type '" + typeSpelling(actual) +
                 "' but the operation expects '" +
                 typeSpelling(parameterTypes[position]) + "'.",
             "GTI-S2005");
      valid = false;
    }
  }
  if (valid) {
    semanticModel.record(expr,
                         ResolvedCallInfo{.returnType = returnType,
                                          .parameterTypes = parameterTypes});
  }
  currentType = valid ? returnType : SemanticType::Unknown;
  return true;
}

auto SemanticVisitor::Impl::visitConformanceDecl(const ConformanceDecl &stmt)
    -> void {
  const auto found =
      std::find_if(conformances.begin(), conformances.end(),
                   [&](const RegisteredConformance &conformance) {
                     return conformance.declaration == &stmt;
                   });
  if (found == conformances.end()) {
    return;
  }
  const RegisteredConcept &conceptInfo = concepts[found->conceptId - 1];
  semanticModel.recordOccurrence(
      {.sourceUnit = currentSourceUnit,
       .span = tokenSpan(stmt.target().name.last()),
       .kind = SemanticOccurrenceKind::Symbol,
       .symbol = conceptInfo.symbol,
       .roles = OccurrenceRole::Reference | OccurrenceRole::TypeUse,
       .name = stmt.target().name.last().lexeme});
}

} // namespace lang
