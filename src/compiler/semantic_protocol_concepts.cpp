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
#include <map>
#include <unordered_set>
#include <utility>

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

// `C<A, B>` for diagnostics.
template <class Spell>
[[nodiscard]] std::string
applicationText(const std::string &name,
                const std::vector<SemanticType> &arguments, Spell spell) {
  std::string text = name + "<";
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    text += (index == 0 ? "" : ", ") + spell(arguments[index]);
  }
  return text + ">";
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
  if (declaration.typeParameters().empty() ||
      conceptInfo.parameterIds.size() != declaration.typeParameters().size()) {
    reportConformanceError(
        declaration.name(),
        "A concept with a body declares at least one type parameter.");
    valid = false;
    return;
  }
  std::vector<SemanticType> parameters;
  for (const GenericParameterId id : conceptInfo.parameterIds) {
    parameters.push_back(SemanticType::typeParameter(id));
  }
  std::unordered_set<std::string> names;
  for (const Token &parameter : declaration.typeParameters()) {
    names.insert(parameter.lexeme);
  }
  names.insert(declaration.name().lexeme);
  const auto claim = [&](const Token &name) {
    if (!names.insert(name.lexeme).second) {
      reportConformanceError(name, "Duplicate member name '" + name.lexeme +
                                       "' in concept '" +
                                       conceptInfo.qualifiedName + "'.");
      valid = false;
      return false;
    }
    return true;
  };

  // Associated names type as projections of the parameters, so an operation
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
        associatedTypeProjection(conceptInfo.id, member, parameters));
  }

  semanticModel.recordProtocolConcept(
      conceptInfo.id, {.qualifiedName = conceptInfo.qualifiedName,
                       .associatedTypes = conceptInfo.associatedTypes});

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
        operation->runtimeBinding() || operation->hasCLinkage()) {
      reject(operation->name(),
             "is a free function signature and takes no modifiers, "
             "runtime binding or receiver qualifier");
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
    for (const FunctionAttribute &attribute : operation->functionAttributes()) {
      if (attribute.name.lexeme != "borrowed_from") {
        reject(attribute.name, "only accepts a borrowed_from origin attribute");
        continue;
      }
      if (resolved.returnBorrowParameter) {
        reject(attribute.name, "cannot repeat its borrowed_from contract");
        continue;
      }
      const auto &formals = operation->parameters();
      const auto selected = std::find_if(
          formals.begin(), formals.end(), [&](const Parameter &parameter) {
            return attribute.argument &&
                   attribute.argument->kind != TokenKind::THIS &&
                   parameter.name.lexeme == attribute.argument->lexeme;
          });
      if (selected == formals.end()) {
        reject(attribute.name,
               "must name one of its parameters in borrowed_from");
        continue;
      }
      resolved.returnBorrowParameter = std::size_t(selected - formals.begin());
      if (!containsOpenTypeParameter(resolved.returnType) &&
          resolved.returnType.kind != SemanticType::Reference) {
        reject(attribute.name,
               "requires a checked reference or dependent result for "
               "borrowed_from");
      }
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
    const std::size_t arity = conceptInfo.parameterIds.size();
    if (target.arguments.size() != arity) {
      reportConformanceError(site, "A conformance block applies concept '" +
                                       conceptInfo.qualifiedName + "' to " +
                                       std::to_string(arity) + " type" +
                                       (arity == 1 ? "" : "s") + ".");
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

    // Each argument is a closed type, or one class template applied to block
    // parameters; every block parameter appears in some argument. A repeated
    // parameter must bind the same type at each of its occurrences.
    beginTypeParameterScope(parameters);
    registeringConformanceParameters = &parameters;
    // Registration needs only the types' identities; validateConformanceTypes
    // validates the written types once what they depend on is known.
    ConformanceTypeCheck typeCheck{.sourceUnit = currentSourceUnit,
                                   .namespaceScope = enclosing,
                                   .genericParameters = parameters};
    std::vector<SemanticType> patterns;
    std::vector<ClassId> patternTemplates;
    std::unordered_set<GenericParameterId> used;
    const auto ownParameter = [&](const SemanticType &type) {
      return type.kind == SemanticType::TypeParameter &&
             !type.isAssociatedTypeProjection() &&
             std::any_of(parameters.begin(), parameters.end(),
                         [&](const GenericParameterInfo &parameter) {
                           return parameter.id == type.genericParameterId;
                         });
    };
    for (const TypeRef &argument : target.arguments) {
      typeCheck.types.push_back(&argument);
      SemanticType pattern = typeOf(argument, enclosing);
      ClassId patternTemplate = 0;
      if (pattern == SemanticType::Unknown) {
        valid = false;
      } else if (!containsOpenTypeParameter(pattern)) {
        pattern = canonicalExactSpecializationType(std::move(pattern));
      } else {
        bool shape =
            pattern.kind == SemanticType::Class && pattern.classId != 0 &&
            pattern.classId <= classes.size() &&
            classInfo(pattern.classId).exactSpecializationPrimary == 0 &&
            pattern.valueArguments.empty() && !pattern.arguments.empty() &&
            std::all_of(pattern.arguments.begin(), pattern.arguments.end(),
                        ownParameter);
        if (!shape) {
          reportConformanceError(
              argument.name.last(),
              "Each argument of a conformance block is a concrete type or "
              "one class template applied to the block's parameters, such "
              "as 'Box<T>'.");
          valid = false;
        } else {
          patternTemplate = pattern.classId;
          for (const SemanticType &parameter : pattern.arguments) {
            used.insert(parameter.genericParameterId);
          }
        }
      }
      patterns.push_back(std::move(pattern));
      patternTemplates.push_back(patternTemplate);
    }
    if (valid) {
      for (const GenericParameterInfo &parameter : parameters) {
        if (!used.contains(parameter.id)) {
          reportConformanceError(parameter.name,
                                 "Block parameter '" + parameter.name.lexeme +
                                     "' does not appear in the concept's "
                                     "arguments.");
          valid = false;
        }
      }
    }
    std::string application = conceptInfo.qualifiedName + "<";
    for (std::size_t position = 0; position < patterns.size(); ++position) {
      application +=
          (position == 0 ? "" : ", ") + typeSpelling(patterns[position]);
    }
    application += ">";

    if (valid) {
      bool owns =
          samePackageOwnershipDomain(currentSourceUnit, conceptInfo.sourceUnit);
      for (std::size_t position = 0; !owns && position < patterns.size();
           ++position) {
        owns = patternTemplates[position] != 0
                   ? samePackageOwnershipDomain(
                         currentSourceUnit,
                         classInfo(patternTemplates[position]).sourceUnit)
                   : ownsNominalSpecializationArgument(currentSourceUnit,
                                                       patterns[position]);
      }
      if (!owns) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2123", DiagnosticPhase::Semantics, site,
            "This package cannot declare a conformance block for '" +
                application + "'; it owns neither the concept nor " +
                (arity == 1 ? "the type." : "any of the argument types."));
        diagnostic.related.push_back(
            {tokenSpan(conceptInfo.declaration->name()),
             "Concept declared here."});
        diagnostic.hints.emplace_back(
            "Declare the block in the concept's package or in the package of "
            "one of its argument types, or wrap a type in one this package "
            "owns.");
        diagnostics.emplace_back(std::move(diagnostic));
        valid = false;
      }
    }

    if (valid) {
      for (const RegisteredConformance &other : conformances) {
        if (other.conceptId == *conceptId &&
            conformancePatternsOverlap(other, patterns, patternTemplates)) {
          reportConformanceError(
              site,
              "This conformance block for concept '" +
                  conceptInfo.qualifiedName +
                  "' overlaps another; the same types match at most one "
                  "block.",
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
      typeCheck.types.push_back(&alias->target());
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
    registeringConformanceParameters = nullptr;
    conformanceTypeChecks.push_back(std::move(typeCheck));
    if (!valid) {
      continue;
    }
    conformances.push_back({.conceptId = *conceptId,
                            .sourceUnit = currentSourceUnit,
                            .declaration = declaration,
                            .namespaceScope = enclosing,
                            .functionScope = scope,
                            .genericParameters = std::move(parameters),
                            .patterns = std::move(patterns),
                            .patternTemplates = std::move(patternTemplates),
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
    // the block's own patterns resolved to its types.
    const auto resolveOwn = [&](const auto &self,
                                SemanticType value) -> SemanticType {
      if (value.isAssociatedTypeProjection() &&
          value.projectionConcept == conformance.conceptId &&
          value.arguments == conformance.patterns &&
          value.projectionMember < conformance.associatedTypes.size()) {
        return conformance.associatedTypes[value.projectionMember];
      }
      for (SemanticType &argument : value.arguments) {
        argument = self(self, std::move(argument));
      }
      return value;
    };
    GenericSubstitution toBlock;
    for (std::size_t position = 0; position < conceptInfo.parameterIds.size();
         ++position) {
      toBlock.types.emplace(conceptInfo.parameterIds[position],
                            conformance.patterns[position]);
    }
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
          function->isVirtual() || function->unsafeKeyword() ||
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
      // A protocol's result-origin contract is part of its signature. A
      // concrete owned result makes it inert; a borrowed or dependent result
      // must publish the same parameter index, independently of parameter
      // spelling. Bodies are checked against that declaration in the ordinary
      // return-origin checker.
      if (operation.returnBorrowParameter &&
          (expectedReturn.kind == SemanticType::Reference ||
           containsOpenTypeParameter(expectedReturn))) {
        const auto *origin = function->findFunctionAttribute("borrowed_from");
        const auto parameter = *operation.returnBorrowParameter;
        if (origin == nullptr || !origin->argument ||
            parameter >= function->parameters().size() ||
            origin->argument->lexeme !=
                function->parameters()[parameter].name.lexeme) {
          reportConformanceError(
              function->name(),
              "Operation '" + function->name().lexeme +
                  "' must preserve the concept's borrowed_from parameter "
                  "contract.",
              &operation.declaration->name(), "Operation declared here.");
          continue;
        }
      }
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

// A block's patterns and `using` types are validated here, once, as a type
// alias's constraints are: after every block is registered and each class's
// members and lifecycle are known. Only then can a concrete type's
// constraints, or a projection through a block declared later, be decided.
// The requirement each operation restates is not validated again.
auto SemanticVisitor::Impl::validateConformanceTypes() -> void {
  const SourceUnitId enclosingUnit = currentSourceUnit;
  const auto enclosingNamespace = currentNamespace;
  const auto enclosingClass = currentClass;
  currentClass.reset();
  for (const ConformanceTypeCheck &check :
       std::exchange(conformanceTypeChecks, {})) {
    currentSourceUnit = check.sourceUnit;
    currentNamespace = check.namespaceScope;
    beginTypeParameterScope(check.genericParameters);
    registeringConformanceParameters = &check.genericParameters;
    for (const TypeRef *type : check.types) {
      validateType(*type);
    }
    registeringConformanceParameters = nullptr;
    endTypeParameterScope();
  }
  currentSourceUnit = enclosingUnit;
  currentNamespace = enclosingNamespace;
  currentClass = enclosingClass;
}

auto SemanticVisitor::Impl::classTemplateArguments(const SemanticType &type,
                                                   ClassId owner) const
    -> std::optional<std::vector<SemanticType>> {
  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size()) {
    return std::nullopt;
  }
  if (type.classId == owner) {
    return type.arguments;
  }
  const ClassInfo &info = classInfo(type.classId);
  if (info.exactSpecializationPrimary != 0 &&
      info.exactSpecializationPrimary == owner) {
    return info.exactTypeArguments;
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::matchConformance(
    ConceptId id, const std::vector<SemanticType> &arguments) const
    -> std::optional<ConformanceMatch> {
  if (std::any_of(arguments.begin(), arguments.end(),
                  [&](const SemanticType &argument) {
                    return containsOpenTypeParameter(argument);
                  })) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < conformances.size(); ++index) {
    const RegisteredConformance &conformance = conformances[index];
    if (conformance.conceptId != id ||
        conformance.patterns.size() != arguments.size()) {
      continue;
    }
    ConformanceMatch match{.conformance = index + 1};
    match.binding.assign(conformance.genericParameters.size(),
                         SemanticType::Unknown);
    // A repeated parameter binds once and must agree at later occurrences.
    const auto bind = [&](GenericParameterId parameter, SemanticType type) {
      type = canonicalExactSpecializationType(std::move(type));
      for (std::size_t slot = 0; slot < conformance.genericParameters.size();
           ++slot) {
        if (conformance.genericParameters[slot].id != parameter) {
          continue;
        }
        if (match.binding[slot] == SemanticType::Unknown) {
          match.binding[slot] = std::move(type);
          return true;
        }
        return match.binding[slot] == type;
      }
      return false;
    };
    bool matches = true;
    for (std::size_t position = 0; matches && position < arguments.size();
         ++position) {
      const SemanticType &pattern = conformance.patterns[position];
      if (conformance.patternTemplates[position] == 0) {
        matches =
            pattern == canonicalExactSpecializationType(arguments[position]);
        continue;
      }
      const std::optional<std::vector<SemanticType>> instance =
          classTemplateArguments(arguments[position],
                                 conformance.patternTemplates[position]);
      matches = instance && instance->size() == pattern.arguments.size();
      for (std::size_t inner = 0; matches && inner < pattern.arguments.size();
           ++inner) {
        matches = bind(pattern.arguments[inner].genericParameterId,
                       (*instance)[inner]);
      }
    }
    if (matches) {
      return match;
    }
  }
  return std::nullopt;
}

// Whether some concrete arguments match both blocks. Every pattern is a
// closed type or a class template applied to block parameters, so unifying
// two blocks binds parameters only to closed types or to each other: a
// union-find over both blocks' parameters, each class carrying at most one
// closed type, decides it.
auto SemanticVisitor::Impl::conformancePatternsOverlap(
    const RegisteredConformance &existing,
    const std::vector<SemanticType> &patterns,
    const std::vector<ClassId> &templates) const -> bool {
  if (existing.patterns.size() != patterns.size()) {
    return false;
  }
  // Parameters are keyed by block (0: existing, 1: candidate) and identity.
  using Variable = std::pair<int, GenericParameterId>;
  std::map<Variable, Variable> parent;
  std::map<Variable, SemanticType> closed;
  const auto find = [&](Variable variable) {
    while (true) {
      const auto found = parent.find(variable);
      if (found == parent.end() || found->second == variable) {
        return variable;
      }
      variable = found->second;
    }
  };
  const auto assign = [&](Variable variable, SemanticType type) {
    type = canonicalExactSpecializationType(std::move(type));
    const Variable root = find(variable);
    const auto found = closed.find(root);
    if (found == closed.end()) {
      closed.emplace(root, std::move(type));
      return true;
    }
    return found->second == type;
  };
  const auto unite = [&](Variable left, Variable right) {
    const Variable leftRoot = find(left);
    const Variable rightRoot = find(right);
    if (leftRoot == rightRoot) {
      return true;
    }
    parent[leftRoot] = rightRoot;
    const auto leftClosed = closed.find(leftRoot);
    if (leftClosed == closed.end()) {
      return true;
    }
    SemanticType type = leftClosed->second;
    closed.erase(leftClosed);
    return assign(rightRoot, std::move(type));
  };
  for (std::size_t position = 0; position < patterns.size(); ++position) {
    const SemanticType &left = existing.patterns[position];
    const SemanticType &right = patterns[position];
    const ClassId leftTemplate = existing.patternTemplates[position];
    const ClassId rightTemplate = templates[position];
    if (leftTemplate == 0 && rightTemplate == 0) {
      if (left != right) {
        return false;
      }
      continue;
    }
    if (leftTemplate != 0 && rightTemplate != 0) {
      if (leftTemplate != rightTemplate ||
          left.arguments.size() != right.arguments.size()) {
        return false;
      }
      for (std::size_t inner = 0; inner < left.arguments.size(); ++inner) {
        if (!unite({0, left.arguments[inner].genericParameterId},
                   {1, right.arguments[inner].genericParameterId})) {
          return false;
        }
      }
      continue;
    }
    // One side is closed: it must be an instance of the other's template.
    const bool leftOpen = leftTemplate != 0;
    const SemanticType &open = leftOpen ? left : right;
    const std::optional<std::vector<SemanticType>> instance =
        classTemplateArguments(leftOpen ? right : left,
                               leftOpen ? leftTemplate : rightTemplate);
    if (!instance || instance->size() != open.arguments.size()) {
      return false;
    }
    for (std::size_t inner = 0; inner < open.arguments.size(); ++inner) {
      if (!assign({leftOpen ? 0 : 1, open.arguments[inner].genericParameterId},
                  (*instance)[inner])) {
        return false;
      }
    }
  }
  return true;
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
    ConceptId id, std::uint32_t member,
    std::vector<SemanticType> arguments) const -> SemanticType {
  SemanticType projection(SemanticType::TypeParameter, std::move(arguments));
  projection.projectionConcept = id;
  projection.projectionMember = member;
  return reduceAssociatedTypeProjection(std::move(projection));
}

auto SemanticVisitor::Impl::reduceAssociatedTypeProjection(
    SemanticType projection) const -> SemanticType {
  if (!projection.isAssociatedTypeProjection() ||
      projection.arguments.empty() ||
      std::any_of(projection.arguments.begin(), projection.arguments.end(),
                  [&](const SemanticType &argument) {
                    return containsOpenTypeParameter(argument);
                  })) {
    return projection;
  }
  const std::optional<ConformanceMatch> match =
      matchConformance(projection.projectionConcept, projection.arguments);
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
    ConceptId id, std::uint32_t member,
    const std::vector<SemanticType> &arguments,
    const std::vector<std::string> &fromScope) const
    -> std::optional<SemanticType> {
  const RegisteredConformance *conformance = enclosingConformance(fromScope);
  if (conformance == nullptr || conformance->conceptId != id ||
      member >= conformance->associatedTypes.size() ||
      conformance->declaration->target().arguments.size() != arguments.size()) {
    return std::nullopt;
  }
  for (std::size_t position = 0; position < arguments.size(); ++position) {
    if (typeOf(conformance->declaration->target().arguments[position],
               fromScope) != arguments[position]) {
      return std::nullopt;
    }
  }
  return blockScopeType(*conformance, conformance->associatedTypes[member]);
}

auto SemanticVisitor::Impl::protocolOperationType(
    ConceptId id, const SemanticType &type,
    const std::vector<SemanticType> &arguments) const -> SemanticType {
  const RegisteredConcept &conceptInfo = concepts[id - 1];
  GenericSubstitution substitution;
  for (std::size_t position = 0; position < conceptInfo.parameterIds.size() &&
                                 position < arguments.size();
       ++position) {
    substitution.types.emplace(conceptInfo.parameterIds[position],
                               arguments[position]);
  }
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
      .missingConformance = true,
      .conformanceArguments = requirement.arguments};
  if (requirement.arguments.size() != conceptInfo.parameterIds.size() ||
      std::any_of(requirement.arguments.begin(), requirement.arguments.end(),
                  [&](const SemanticType &type) {
                    return containsOpenTypeParameter(type);
                  })) {
    return failure;
  }
  const std::optional<ConformanceMatch> match =
      matchConformance(requirement.conceptId, requirement.arguments);
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
  const std::size_t arity = conceptInfo.parameterIds.size();
  if (owner.arguments.size() != arity) {
    reportConformanceError(site, "Concept '" + conceptInfo.qualifiedName +
                                     "' takes " + std::to_string(arity) +
                                     " type argument" +
                                     (arity == 1 ? "" : "s") + ".");
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
  std::vector<SemanticType> arguments;
  for (const TypeRef &argument : owner.arguments) {
    arguments.push_back(typeOf(argument));
    if (arguments.back() == SemanticType::Unknown) {
      return;
    }
  }
  const std::string application = applicationText(
      conceptInfo.qualifiedName, arguments,
      [&](const SemanticType &argument) { return typeSpelling(argument); });
  if (std::any_of(arguments.begin(), arguments.end(),
                  [&](const SemanticType &argument) {
                    return containsOpenTypeParameter(argument);
                  })) {
    if (firstConceptApplicationFailure(
            {.conceptId = id, .arguments = arguments})) {
      reportConformanceError(site, "'" + application + "::" + site.lexeme +
                                       "' requires 'requires " + application +
                                       "' on the enclosing declaration.");
    }
    return;
  }
  if (!matchConformance(id, arguments)) {
    reportConformanceError(site, "No conformance block 'for concept " +
                                     application + "' is declared.");
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
  if (!id || owner.arguments.size() != concepts[*id - 1].parameterIds.size()) {
    return std::nullopt;
  }
  std::vector<SemanticType> arguments;
  for (const TypeRef &argument : owner.arguments) {
    arguments.push_back(substituteType(
        typeOf(argument),
        GenericSubstitution{.types = instanceTypeSubstitution,
                            .values = instanceValueSubstitution}));
  }
  const std::optional<ConformanceMatch> match =
      matchConformance(*id, arguments);
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
  const std::size_t arity = conceptInfo.parameterIds.size();
  if (owner.arguments.size() != arity) {
    reportConformanceError(site, "Concept '" + conceptInfo.qualifiedName +
                                     "' takes " + std::to_string(arity) +
                                     " type argument" +
                                     (arity == 1 ? "" : "s") + ".");
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
  std::vector<SemanticType> arguments;
  for (const TypeRef &argument : owner.arguments) {
    arguments.push_back(substituteType(
        typeOf(argument),
        GenericSubstitution{.types = instanceTypeSubstitution,
                            .values = instanceValueSubstitution}));
    if (arguments.back() == SemanticType::Unknown) {
      return fail();
    }
  }
  const std::string application = applicationText(
      conceptInfo.qualifiedName, arguments,
      [&](const SemanticType &argument) { return typeSpelling(argument); });
  if (std::none_of(arguments.begin(), arguments.end(),
                   [&](const SemanticType &argument) {
                     return containsOpenTypeParameter(argument);
                   })) {
    if (!protocolOperationTarget(callee)) {
      reportConformanceError(site, "No conformance block 'for concept " +
                                       application + "' is declared.");
      return fail();
    }
    return false;
  }
  if (firstConceptApplicationFailure(
          {.conceptId = id, .arguments = arguments})) {
    reportConformanceError(site, "Calling '" + application +
                                     "::" + site.lexeme +
                                     "' requires 'requires " + application +
                                     "' on the enclosing declaration.");
    return fail();
  }

  // Through the requirement: check the arguments against the declared
  // signature. Each concrete instance resolves the block function itself.
  const RegisteredConcept::ProtocolOperation &operation =
      conceptInfo.operations[*index];
  std::vector<SemanticType> parameterTypes;
  parameterTypes.reserve(operation.parameterTypes.size());
  for (const SemanticType &parameter : operation.parameterTypes) {
    parameterTypes.push_back(protocolOperationType(id, parameter, arguments));
  }
  const SemanticType returnType =
      protocolOperationType(id, operation.returnType, arguments);
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
    ResolvedCallInfo selected{.returnType = returnType,
                              .parameterTypes = parameterTypes};
    const auto inferred = functionReturnBorrowSummary(
        returnType, parameterTypes, 0, false, IntrinsicKind::None);
    selected.borrowOrigin = inferred.origin;
    selected.borrowArgument = inferred.parameter;
    selected.borrowAccess = inferred.access;
    selected.borrowArguments = inferred.parameters;
    if (returnType.kind == SemanticType::Reference &&
        operation.returnBorrowParameter) {
      selected.borrowOrigin = BorrowOriginKind::Argument;
      selected.borrowArgument = *operation.returnBorrowParameter;
      selected.borrowAccess = returnType.referenceAccess;
    }
    semanticModel.record(expr, std::move(selected));
  }
  currentType = !valid ? SemanticType::Unknown
                : returnType.kind == SemanticType::Reference &&
                        returnType.arguments.size() == 1
                    ? returnType.arguments.front()
                    : returnType;
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
