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
#include "gti/lifetime_types.h"

#include <algorithm>
#include <map>
#include <unordered_set>
#include <utility>

namespace lang {

namespace {

// Projections reduce through block types, which may themselves name
// projections; a cyclic block must not recurse without bound.
constexpr std::size_t maximumProjectionDepth = 64;
constexpr std::size_t maximumProjectionWork = 16384;
thread_local std::vector<SemanticType> activeProjections;
thread_local std::size_t projectionWork = 0;

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
    if (index != 0)
      text += ", ";
    text += spell(arguments[index]);
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

auto SemanticVisitor::Impl::makeAssociatedTypeParameters(
    const std::vector<GenericParameter> &parameters, const Token &name,
    const std::vector<std::string> &fromScope,
    const std::vector<std::string> &enclosingNames)
    -> std::vector<GenericParameterInfo> {
  for (const GenericParameter &parameter : parameters) {
    if (parameter.pack || parameter.valueType || parameter.equal) {
      reportConformanceError(
          parameter.name,
          "Associated type families accept type parameters without packs, "
          "value parameters or defaults.");
    }
    if (std::find(enclosingNames.begin(), enclosingNames.end(),
                  parameter.name.lexeme) != enclosingNames.end()) {
      reportConformanceError(
          parameter.name,
          "Associated type parameter '" + parameter.name.lexeme +
              "' cannot shadow an enclosing parameter or concept member.");
    }
  }
  return makeGenericParameters(parameters, name, fromScope, false);
}

// The same lexical bindings serve concept signatures and block alias targets.
// Keep unapplied families symbolic; only an application substitutes binders.
auto SemanticVisitor::Impl::bindAssociatedTypeNames(
    ConceptId id, const std::vector<SemanticType> &arguments,
    const std::vector<Lifetime> &lifetimes) -> void {
  const auto &names = concepts[id - 1].associatedTypes;
  for (std::size_t member = 0; member < names.size(); ++member) {
    SemanticType projection(SemanticType::TypeParameter, arguments);
    projection.lifetimeArguments = lifetimes;
    projection.projectionConcept = id;
    projection.projectionMember = static_cast<std::uint32_t>(member);
    typeParameterScopes.back().emplace(names[member], std::move(projection));
  }
}

auto SemanticVisitor::Impl::associatedTypeSymbol(ConceptId id,
                                                 std::uint32_t member) const
    -> SymbolId {
  const auto &info = concepts[id - 1];
  if (member >= info.associatedTypes.size())
    return 0;
  for (const auto &declaration :
       info.declaration->protocolBody()->associatedTypes)
    if (declaration.name.lexeme == info.associatedTypes[member])
      return symbolForDeclaration(declaration.name);
  return 0;
}

// Called by resolveConcept with the concept's parameter scope active.
auto SemanticVisitor::Impl::resolveProtocolConcept(
    RegisteredConcept &conceptInfo, bool &valid) -> void {
  const ConceptDecl &declaration = *conceptInfo.declaration;
  const ProtocolConceptBody &body = *declaration.protocolBody();
  conceptInfo.protocol = true;
  conceptInfo.associatedTypes.clear();
  conceptInfo.associatedTypeParameters.clear();
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
  std::vector<Lifetime> lifetimes;
  for (const GenericParameterId id : conceptInfo.parameterIds) {
    parameters.push_back(SemanticType::typeParameter(id));
  }
  for (const GenericParameterId id : conceptInfo.lifetimeParameterIds)
    lifetimes.push_back(Lifetime::parameter(id));
  std::unordered_set<std::string> names;
  for (const auto &parameter : declaration.genericParameters()) {
    names.insert(parameter.name.lexeme);
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

  std::vector<std::string> enclosingNames(names.begin(), names.end());
  for (const auto &associated : body.associatedTypes)
    enclosingNames.push_back(associated.name.lexeme);
  for (const auto &statement : body.operations)
    if (const auto *operation = stmtAs<FunctionDecl>(statement.get()))
      enclosingNames.push_back(operation->name().lexeme);
  const std::size_t diagnosticsBefore = diagnostics.size();
  for (const ProtocolAssociatedType &associated : body.associatedTypes) {
    if (!claim(associated.name)) {
      continue;
    }
    conceptInfo.associatedTypes.push_back(associated.name.lexeme);
    conceptInfo.associatedTypeParameters.push_back(makeAssociatedTypeParameters(
        associated.genericParameters, associated.name,
        conceptInfo.namespaceScope, enclosingNames));
  }
  valid = valid && diagnostics.size() == diagnosticsBefore;
  bindAssociatedTypeNames(conceptInfo.id, parameters, lifetimes);
  for (const auto &associated : body.associatedTypes) {
    const auto member =
        memberIndex(conceptInfo.associatedTypes, associated.name.lexeme);
    if (!member)
      continue;
    std::vector<SemanticType> inputs;
    std::vector<Lifetime> inputLifetimes;
    for (const auto &parameter : conceptInfo.associatedTypeParameters[*member])
      if (parameter.lifetime)
        inputLifetimes.push_back(Lifetime::parameter(parameter.id));
      else
        inputs.push_back(SemanticType::typeParameter(parameter.id));
    const auto type =
        associatedTypeProjection(conceptInfo.id, *member, parameters, inputs,
                                 lifetimes, inputLifetimes);
    const auto symbol = recordToolingSymbol(
        associated.name, SymbolKind::TypeAlias,
        conceptInfo.qualifiedName + "::" + associated.name.lexeme, type);
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(associated.name),
         .kind = SemanticOccurrenceKind::Symbol,
         .symbol = symbol,
         .roles = OccurrenceRole::Declaration | OccurrenceRole::TypeUse,
         .name = associated.name.lexeme,
         .type = type});
  }

  semanticModel.recordProtocolConcept(
      conceptInfo.id, {.qualifiedName = conceptInfo.qualifiedName,
                       .associatedTypes = conceptInfo.associatedTypes,
                       .genericParameters = conceptInfo.genericParameters,
                       .associatedTypeParameters =
                           conceptInfo.associatedTypeParameters,
                       .parameterCount = conceptInfo.parameterIds.size()});

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
    CallableDeclaration declarationRecord{
        .sourceUnit = conceptInfo.sourceUnit,
        .syntax = operation,
        .genericParameters = conceptInfo.genericParameters};
    CallableCandidate candidate{.declarationRecord = &declarationRecord,
                                .declaration = operation,
                                .returnType = resolved.returnType,
                                .parameterTypes = resolved.parameterTypes};
    normalizeFunctionLifetimeContract(candidate);
    resolved.returnType = std::move(candidate.returnType);
    resolved.parameterTypes = std::move(candidate.parameterTypes);
    resolved.lifetimeContract = std::move(candidate.lifetimeContract);
    valid = valid && resolved.lifetimeContract.complete;
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
    // The header owns these parameters and their diagnostics. Operations share
    // their resolved identities, rather than registering each written copy.
    std::vector<GenericParameterInfo> parameters = makeGenericParameters(
        declaration->genericParameters(), declaration->forKeyword(), enclosing);
    for (const StmtPtr &member : declaration->members()) {
      if (const auto *function = stmtAs<FunctionDecl>(member.get())) {
        functionGenericParameters.emplace(function, parameters);
      }
    }
    const ConceptApplication &target = declaration->target();
    const Token &site = target.name.last();
    if (reportCompilerPrivatePath(target.name)) {
      continue;
    }
    const std::optional<ConceptId> conceptId =
        resolveConceptPath(target.name, enclosing);
    if (!conceptId || *conceptId == 0 || *conceptId > concepts.size()) {
      const auto global = resolveConceptPathGlobally(target.name, enclosing);
      if (!(global && *global != 0 && *global <= concepts.size() &&
            reportInvisibleDeclaration(
                site, pathSpelling(target.name),
                concepts[*global - 1].declaration->name(),
                concepts[*global - 1].sourceUnit))) {
        reportConformanceError(site, "Unknown or invisible concept '" +
                                         pathSpelling(target.name) + "'.");
        diagnostics.back().missingIncludeName = site.lexeme;
      }
      continue;
    }
    recordQualifiedPathUses(target.name);
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(site),
         .kind = SemanticOccurrenceKind::Symbol,
         .symbol = concepts[*conceptId - 1].symbol,
         .roles = OccurrenceRole::Reference | OccurrenceRole::TypeUse,
         .name = site.lexeme});
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
    const std::size_t arity = conceptInfo.genericParameters.size();
    if (target.arguments.size() != arity) {
      reportConformanceError(site, "A conformance block applies concept '" +
                                       conceptInfo.qualifiedName + "' to " +
                                       std::to_string(arity) + " generic argument" +
                                       (arity == 1 ? "" : "s") + ".");
      continue;
    }

    bool valid = parameters.size() == declaration->genericParameters().size();
    for (const GenericParameterInfo &parameter : parameters) {
      if (parameter.pack || parameter.value) {
        reportConformanceError(parameter.name,
                               "Conformance block parameters are type or "
                               "lifetime parameters; packs and value parameters are "
                               "not supported.");
        valid = false;
      }
    }

    // Each argument is a closed type, or one class or enum template, pointer
    // or fixed-length array type applied to block parameters; every block
    // parameter appears in some argument. A repeated parameter must bind the
    // same type at each of its occurrences.
    beginTypeParameterScope(parameters);
    registeringConformanceParameters = &parameters;
    // Registration needs only the types' identities; validateConformanceTypes
    // validates the written types once what they depend on is known.
    ConformanceTypeCheck typeCheck{.declaration = declaration,
                                   .sourceUnit = currentSourceUnit,
                                   .namespaceScope = enclosing,
                                   .genericParameters = parameters};
    std::vector<SemanticType> patterns;
    std::vector<Lifetime> lifetimePatterns;
    std::unordered_set<GenericParameterId> used;
    const auto ownParameter = [&](const SemanticType &type) {
      return type.kind == SemanticType::TypeParameter &&
             !type.isAssociatedTypeProjection() &&
             std::any_of(parameters.begin(), parameters.end(),
                         [&](const GenericParameterInfo &parameter) {
                           return parameter.id == type.genericParameterId;
                         });
    };
    for (std::size_t argumentIndex = 0;
         argumentIndex < target.arguments.size(); ++argumentIndex) {
      const TypeRef &argument = target.arguments[argumentIndex];
      if (conceptInfo.genericParameters[argumentIndex].lifetime) {
        const bool bare = argument.name.segments.size() == 1 &&
                          argument.arguments.empty() &&
                          argument.pointers.empty() && !argument.reference &&
                          argument.arrayExtents.empty() &&
                          !argument.genericValueExpression;
        const Lifetime lifetime = bare ? resolveLifetimeArgument(argument.name.last())
                                       : Lifetime{};
        if (!bare)
          reportConformanceError(argument.name.last(),
                                 "A concept lifetime argument must name a lifetime.");
        lifetimePatterns.push_back(lifetime);
        valid = valid && lifetime.specified();
        if (lifetime.kind == Lifetime::Kind::Parameter)
          used.insert(lifetime.id);
        continue;
      }
      typeCheck.types.push_back(&argument);
      SemanticType pattern = typeOf(argument, enclosing);
      for (const Lifetime lifetime : freeTypeLifetimes(pattern))
        if (lifetime.kind == Lifetime::Kind::Parameter)
          used.insert(lifetime.id);
      if (pattern == SemanticType::Unknown) {
        valid = false;
      } else if (!openConformancePattern(pattern)) {
        pattern = canonicalExactSpecializationType(std::move(pattern));
      } else {
        const bool nominalTemplate =
            (pattern.kind == SemanticType::Class && pattern.classId != 0 &&
             pattern.classId <= classes.size() &&
             classInfo(pattern.classId).exactSpecializationPrimary == 0) ||
            (pattern.kind == SemanticType::Enum && pattern.enumId != 0 &&
             pattern.enumId <= enums.size());
        const bool shape =
            (nominalTemplate || builtinConformanceConstructor(pattern)) &&
            pattern.valueArguments.empty() &&
            (!pattern.arguments.empty() || !pattern.lifetimeArguments.empty()) &&
            std::all_of(pattern.arguments.begin(), pattern.arguments.end(),
                        ownParameter);
        if (!shape) {
          reportConformanceError(
              argument.name.last(),
              "Each argument of a conformance block is a concrete type, or "
              "one class or enum template, pointer or array type applied to "
              "the block's parameters, such as 'Box<T>', 'T*' or 'T[3]'.");
          valid = false;
        } else {
          for (const SemanticType &parameter : pattern.arguments) {
            used.insert(parameter.genericParameterId);
          }
        }
      }
      patterns.push_back(std::move(pattern));
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
      // A nominal template/type or the concept owns its module's blocks.
      // Registration closes that module's implementation sources, preserving
      // one canonical conformance regardless of the requester's imports.
      bool owns = declarationOwner(currentSourceUnit) ==
                  declarationOwner(conceptInfo.sourceUnit);
      for (std::size_t position = 0; !owns && position < patterns.size();
           ++position) {
        owns = ownsNominalSpecializationArgument(currentSourceUnit,
                                                 patterns[position]);
      }
      if (!owns) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2123", DiagnosticPhase::Semantics, site,
            "This module cannot declare a conformance block for '" +
                application + "'; it owns neither the concept nor " +
                (arity == 1 ? "the type." : "any of the argument types."));
        diagnostic.related.push_back(
            {tokenSpan(conceptInfo.declaration->name()),
             "Concept declared here."});
        diagnostic.hints.emplace_back(
            "Declare the block in the concept's module or in the module of "
            "one of its argument types, or wrap a type in one this module "
            "owns.");
        diagnostics.emplace_back(std::move(diagnostic));
        valid = false;
      }
    }

    if (valid) {
      for (const RegisteredConformance &other : conformances) {
        if (other.conceptId == *conceptId &&
            conformancePatternsOverlap(other, patterns)) {
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
    std::vector<std::vector<GenericParameterInfo>> associatedTypeParameters(
        conceptInfo.associatedTypes.size());
    typeCheck.conceptId = *conceptId;
    typeCheck.patterns = patterns;
    bindAssociatedTypeNames(*conceptId, patterns);
    std::vector<std::string> enclosingNames = conceptInfo.associatedTypes;
    enclosingNames.push_back(conceptInfo.declaration->name().lexeme);
    for (const auto &parameter : parameters)
      enclosingNames.push_back(parameter.name.lexeme);
    for (const auto &operation : conceptInfo.operations)
      enclosingNames.push_back(operation.name);
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
      auto familyParameters = makeAssociatedTypeParameters(
          alias->genericParameters(), alias->name(), enclosing, enclosingNames);
      const auto &required = conceptInfo.associatedTypeParameters[*index];
      if (familyParameters.size() != required.size()) {
        reportConformanceError(
            alias->name(), "Associated type '" + alias->name().lexeme +
                               "' requires " + std::to_string(required.size()) +
                               " type parameters.");
      } else {
        for (std::size_t parameter = 0; parameter < required.size();
             ++parameter)
          if (familyParameters[parameter].conceptId !=
                  required[parameter].conceptId ||
              familyParameters[parameter].lifetime !=
                  required[parameter].lifetime ||
              familyParameters[parameter].constraints !=
                  required[parameter].constraints)
            reportConformanceError(familyParameters[parameter].name,
                                   "Associated type parameter constraints must "
                                   "match the concept declaration.",
                                   &required[parameter].name,
                                   "Declared parameter contract.");
      }
      beginTypeParameterScope(familyParameters);
      associatedTypes[*index] = typeOf(alias->target(), enclosing);
      semanticModel.recordOccurrence(
          {.sourceUnit = currentSourceUnit,
           .span = tokenSpan(alias->name()),
           .kind = SemanticOccurrenceKind::Symbol,
           .symbol = associatedTypeSymbol(*conceptId, *index),
           .roles = OccurrenceRole::Definition | OccurrenceRole::TypeUse,
           .name = alias->name().lexeme,
           .type = associatedTypes[*index]});
      endTypeParameterScope();
      typeCheck.associatedTypes.push_back({alias, familyParameters});
      associatedTypeParameters[*index] = std::move(familyParameters);
    }
    for (std::size_t index = 0; index < defined.size(); ++index) {
      if (!defined[index]) {
        reportConformanceError(
            site, "The conformance block does not define associated type '" +
                      conceptInfo.associatedTypes[index] + "' of concept '" +
                      conceptInfo.qualifiedName + "'.");
      }
    }
    // All conformance heads are registered before their written type contracts
    // are validated. Resolve only identities here; the lexical validation pass
    // checks the source clause after class members and other blocks are known.
    auto requirements = resolveRequirements(
        declaration->requiresClause() ? &*declaration->requiresClause()
                                      : nullptr,
        enclosing, false);
    endTypeParameterScope();
    registeringConformanceParameters = nullptr;
    conformanceTypeChecks.push_back(std::move(typeCheck));
    if (!valid) {
      continue;
    }
    conformances.push_back(
        {.conceptId = *conceptId,
         .sourceUnit = currentSourceUnit,
         .declaration = declaration,
         .namespaceScope = enclosing,
         .functionScope = scope,
         .genericParameters = std::move(parameters),
         .patterns = std::move(patterns),
         .lifetimePatterns = std::move(lifetimePatterns),
         .requirements = std::move(requirements),
         .associatedTypes = std::move(associatedTypes),
         .associatedTypeParameters = std::move(associatedTypeParameters)});
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
          value.projectionMember < conformance.associatedTypes.size() &&
          value.arguments.size() ==
              conformance.patterns.size() +
                  genericTypeParameterCount(conformance.associatedTypeParameters[
                      value.projectionMember]) &&
          value.lifetimeArguments.size() ==
              conformance.lifetimePatterns.size() +
                  genericLifetimeParameterCount(conformance.associatedTypeParameters[
                      value.projectionMember]) &&
          std::equal(conformance.patterns.begin(), conformance.patterns.end(),
                     value.arguments.begin()) &&
          std::equal(conformance.lifetimePatterns.begin(), conformance.lifetimePatterns.end(),
                     value.lifetimeArguments.begin())) {
        GenericSubstitution family;
        const auto &parameters =
            conformance.associatedTypeParameters[value.projectionMember];
        std::size_t typeIndex = conformance.patterns.size();
        std::size_t lifetimeIndex = conformance.lifetimePatterns.size();
        for (const auto &parameter : parameters)
          if (parameter.lifetime)
            family.lifetimes.emplace(parameter.id,
                                      value.lifetimeArguments[lifetimeIndex++]);
          else
            family.types.emplace(parameter.id, value.arguments[typeIndex++]);
        return substituteType(
            conformance.associatedTypes[value.projectionMember], family);
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
    for (std::size_t position = 0;
         position < conceptInfo.lifetimeParameterIds.size(); ++position)
      toBlock.lifetimes.emplace(conceptInfo.lifetimeParameterIds[position],
                                conformance.lifetimePatterns[position]);
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
      // A concept operation declares no defaults, so its block function may
      // not offer arguments the concept's interface lacks: a concrete call
      // and a call through the requirement accept the same arguments. The
      // function stays bound, so its uses report nothing further.
      for (const Parameter &parameter : function->parameters()) {
        if (parameter.hasDefault()) {
          reportConformanceError(
              parameter.name,
              "Conformance block operation '" + function->name().lexeme +
                  "' cannot declare a default argument; concept '" +
                  conceptInfo.qualifiedName + "' declares none.",
              &conceptInfo.operations[*index].declaration->name(),
              "Operation declared here.");
          break;
        }
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
        if (functionParameters[parameter].lifetime)
          toBlockParameters.lifetimes.emplace(
              functionParameters[parameter].id,
              Lifetime::parameter(conformance.genericParameters[parameter].id));
        else
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
      std::vector<SemanticType> expectedParameters, actualParameters;
      for (const auto &parameter : candidate.parameterTypes)
        actualParameters.push_back(actual(parameter));
      std::string expectedSignature =
          typeSpelling(expectedReturn) + " " + operation.name + "(";
      for (std::size_t parameter = 0;
           parameter < operation.parameterTypes.size(); ++parameter) {
        const SemanticType expectedParameter =
            expected(operation.parameterTypes[parameter]);
        expectedSignature +=
            (parameter == 0 ? "" : ", ") + typeSpelling(expectedParameter);
        expectedParameters.push_back(expectedParameter);
      }
      expectedSignature += ")";
      SemanticType expectedCallable = SemanticType::functionSignature(
          expectedReturn, std::move(expectedParameters));
      SemanticType actualCallable = SemanticType::functionSignature(
          actual(candidate.returnType), std::move(actualParameters));
      const auto quantifyImplicit = [](SemanticType &signature,
                                       const LifetimeSignatureContract &contract,
                                       const std::vector<GenericParameterInfo> &parameters) {
        for (Lifetime lifetime : contract.parameters)
          if (lifetime.kind == Lifetime::Kind::Parameter &&
              std::none_of(parameters.begin(), parameters.end(),
                           [&](const auto &parameter) {
                return parameter.lifetime && parameter.id == lifetime.id;
              }))
            signature.quantifiedLifetimes.push_back(lifetime.id);
      };
      quantifyImplicit(expectedCallable, operation.lifetimeContract,
                       conceptInfo.genericParameters);
      quantifyImplicit(actualCallable, candidate.lifetimeContract,
                       candidate.record().genericParameters);
      bool exact = equivalentLifetimeTypes(actualCallable, expectedCallable);
      // A protocol's result-origin contract is part of its signature. A
      // concrete owned result makes it inert; a borrowed result must publish
      // the same parameter index, independently of parameter spelling. Defer
      // dependent returns until concrete conformance selection: an open type
      // can instantiate to an owned result with no origin contract. Bodies are
      // checked against the declaration in the ordinary return-origin checker.
      if (operation.returnBorrowParameter &&
          expectedReturn.kind == SemanticType::Reference) {
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
// alias's constraints are: at its lexical position, after registration made
// every block and class signature available and preceding constexpr bodies
// have been analyzed. This also preserves definition-before-use for source
// predicates while allowing projections through blocks declared later.
// The requirement each operation restates is not validated again.
auto SemanticVisitor::Impl::validateConformanceTypes(
    const ConformanceDecl &declaration) -> void {
  const SourceUnitId enclosingUnit = currentSourceUnit;
  const auto enclosingNamespace = currentNamespace;
  const auto enclosingClass = currentClass;
  currentClass.reset();
  for (const ConformanceTypeCheck &check : conformanceTypeChecks) {
    if (check.declaration != &declaration)
      continue;
    currentSourceUnit = check.sourceUnit;
    currentNamespace = check.namespaceScope;
    beginTypeParameterScope(check.genericParameters);
    registeringConformanceParameters = &check.genericParameters;
    auto requirements = resolveRequirements(
        declaration.requiresClause() ? &*declaration.requiresClause() : nullptr,
        check.namespaceScope);
    requirementScopes.push_back(std::move(requirements));
    for (const TypeRef *type : check.types) {
      validateType(*type);
    }
    bindAssociatedTypeNames(check.conceptId, check.patterns);
    for (const auto &associated : check.associatedTypes) {
      beginTypeParameterScope(associated.parameters);
      const auto before = diagnostics.size();
      validateType(associated.declaration->target());
      if (diagnostics.size() == before &&
          typeOf(associated.declaration->target()) == SemanticType::Unknown)
        reportConformanceError(associated.declaration->name(),
                               "Associated type definition is unresolved, "
                               "cyclic, or exceeds the projection limit.");
      endTypeParameterScope();
    }
    registeringConformanceParameters = nullptr;
    requirementScopes.pop_back();
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
  // A definition family selects representation and members, not a different
  // public template. Protocol deduction binds the public arguments just like
  // ordinary generic call deduction; object compatibility remains exact.
  const auto family = [&](ClassId id) {
    const ClassId primary = classInfo(id).definitionFamilyPrimary;
    return primary == 0 ? id : primary;
  };
  if (owner != 0 && owner <= classes.size() &&
      family(type.classId) == family(owner)) {
    return type.arguments;
  }
  if (info.exactSpecializationPrimary != 0 &&
      info.exactSpecializationPrimary == owner) {
    return info.exactTypeArguments;
  }
  return std::nullopt;
}

// A block's pattern is open when it applies a template to block parameters;
// a closed pattern is one concrete type.
auto SemanticVisitor::Impl::openConformancePattern(const SemanticType &pattern)
    -> bool {
  if (containsOpenTypeParameter(pattern))
    return true;
  const auto lifetimes = freeTypeLifetimes(pattern);
  return std::any_of(lifetimes.begin(), lifetimes.end(), [](Lifetime lifetime) {
    return lifetime.kind == Lifetime::Kind::Parameter;
  });
}

// A built-in type that a pattern may apply to block parameters, as it applies
// a class or enum template: a raw pointer `T*` or `const T*`, or a
// fixed-length array `T[3]`. Its type arguments are the pattern's arguments.
auto SemanticVisitor::Impl::builtinConformanceConstructor(
    const SemanticType &type) -> bool {
  switch (type.kind) {
  case SemanticType::RawPointer:
  case SemanticType::Reference:
    return type.arguments.size() == 1;
  case SemanticType::StringView:
    return type.lifetimeArguments.size() == 1;
  case SemanticType::Array:
    // A fixed length; a length parameter is not a type argument.
    return type.arguments.size() == 1 && type.arrayLength != 0 &&
           type.arrayLengthParameterId == 0;
  default:
    return false;
  }
}

// Whether two types apply the same constructor, ignoring its type arguments.
auto SemanticVisitor::Impl::sameConformanceConstructor(
    const SemanticType &left, const SemanticType &right) -> bool {
  return left.kind == right.kind && left.classId == right.classId &&
         left.enumId == right.enumId &&
         (left.kind != SemanticType::RawPointer ||
          left.pointerAccess == right.pointerAccess) &&
         (left.kind != SemanticType::Reference ||
          left.referenceAccess == right.referenceAccess) &&
         (left.kind != SemanticType::Array ||
          (left.arrayLength == right.arrayLength &&
           left.arrayLengthParameterId == right.arrayLengthParameterId));
}

// The arguments that make `type` an instance of an open pattern's class or
// enum template or built-in type, or nothing when it is not one.
auto SemanticVisitor::Impl::patternInstanceArguments(
    const SemanticType &pattern, const SemanticType &type) const
    -> std::optional<std::vector<SemanticType>> {
  if (pattern.kind == SemanticType::Class) {
    return classTemplateArguments(type, pattern.classId);
  }
  if (pattern.kind == SemanticType::Enum && type.kind == SemanticType::Enum &&
      type.enumId == pattern.enumId && type.valueArguments.empty()) {
    return type.arguments;
  }
  if (builtinConformanceConstructor(pattern) &&
      sameConformanceConstructor(pattern, type) &&
      type.arguments.size() == pattern.arguments.size()) {
    return type.arguments;
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::matchConformance(
    ConceptId id, const std::vector<SemanticType> &arguments,
    const std::vector<Lifetime> &lifetimes) const
    -> std::optional<ConformanceMatch> {
  for (std::size_t index = 0; index < conformances.size(); ++index) {
    const RegisteredConformance &conformance = conformances[index];
    if (conformance.conceptId != id ||
        conformance.patterns.size() != arguments.size() ||
        conformance.lifetimePatterns.size() != lifetimes.size())
      continue;
    ConformanceMatch match{.conformance = index + 1};
    match.binding.assign(genericTypeParameterCount(conformance.genericParameters),
                         SemanticType::Unknown);
    const auto bindType = [&](GenericParameterId parameter, SemanticType type) {
      type = canonicalExactSpecializationType(std::move(type));
      std::size_t slot = 0;
      for (const auto &generic : conformance.genericParameters) {
        if (generic.lifetime || generic.value)
          continue;
        if (generic.id != parameter) {
          ++slot;
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
    const auto bindLifetime = [&](Lifetime pattern, Lifetime actual) {
      if (pattern.kind != Lifetime::Kind::Parameter ||
          std::none_of(conformance.genericParameters.begin(),
                       conformance.genericParameters.end(),
                       [&](const auto &parameter) {
        return parameter.lifetime && parameter.id == pattern.id;
      }))
        return pattern == actual;
      if (!actual.specified())
        return false;
      const auto [bound, inserted] =
          match.lifetimeBinding.emplace(pattern.id, actual);
      return inserted || bound->second == actual;
    };
    const auto matchType = [&](auto &&self, const SemanticType &pattern,
                               const SemanticType &actual) -> bool {
      if (pattern.kind == SemanticType::TypeParameter &&
          !pattern.isAssociatedTypeProjection())
        return bindType(pattern.genericParameterId, actual);
      auto actualArguments = pattern.kind == SemanticType::Class
                                 ? classTemplateArguments(actual, pattern.classId)
                                 : std::optional(actual.arguments);
      if (!actualArguments || actualArguments->size() != pattern.arguments.size())
        return false;
      SemanticType patternHead = pattern, actualHead = actual;
      if (pattern.kind == SemanticType::Class)
        actualHead.classId = pattern.classId;
      patternHead.arguments.clear();
      actualHead.arguments.clear();
      patternHead.referenceLifetime = {};
      actualHead.referenceLifetime = {};
      patternHead.lifetimeArguments.clear();
      actualHead.lifetimeArguments.clear();
      if (patternHead != actualHead ||
          !bindLifetime(pattern.referenceLifetime, actual.referenceLifetime) ||
          pattern.lifetimeArguments.size() != actual.lifetimeArguments.size())
        return false;
      for (std::size_t region = 0; region < pattern.lifetimeArguments.size(); ++region)
        if (!bindLifetime(pattern.lifetimeArguments[region],
                          actual.lifetimeArguments[region]))
          return false;
      for (std::size_t child = 0; child < pattern.arguments.size(); ++child)
        if (!self(self, pattern.arguments[child], (*actualArguments)[child]))
          return false;
      return true;
    };
    bool matches = true;
    for (std::size_t position = 0; matches && position < lifetimes.size(); ++position)
      matches = bindLifetime(conformance.lifetimePatterns[position], lifetimes[position]);
    for (std::size_t position = 0; matches && position < arguments.size(); ++position)
      matches = matchType(matchType, conformance.patterns[position], arguments[position]);
    if (matches)
      return match;
  }
  return std::nullopt;
}

// Whether some concrete arguments match both blocks. Every pattern is a
// closed type or a template or built-in type applied to block parameters, so
// unifying two blocks binds parameters only to closed types or to each
// other: a union-find over both blocks' parameters, each class carrying at
// most one closed type, decides it.
auto SemanticVisitor::Impl::conformancePatternsOverlap(
    const RegisteredConformance &existing,
    const std::vector<SemanticType> &patterns) const -> bool {
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
    const bool leftOpen = openConformancePattern(left);
    const bool rightOpen = openConformancePattern(right);
    if (!leftOpen && !rightOpen) {
      if (left != right) {
        return false;
      }
      continue;
    }
    if (leftOpen && rightOpen) {
      if (!sameConformanceConstructor(left, right) ||
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
    const SemanticType &open = leftOpen ? left : right;
    const std::optional<std::vector<SemanticType>> instance =
        patternInstanceArguments(open, leftOpen ? right : left);
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
    ConceptId id, std::uint32_t member, std::vector<SemanticType> arguments,
    std::vector<SemanticType> familyArguments, std::vector<Lifetime> lifetimes,
    std::vector<Lifetime> familyLifetimes) const -> SemanticType {
  arguments.insert(arguments.end(), familyArguments.begin(),
                   familyArguments.end());
  SemanticType projection(SemanticType::TypeParameter, std::move(arguments));
  lifetimes.insert(lifetimes.end(), familyLifetimes.begin(), familyLifetimes.end());
  projection.lifetimeArguments = std::move(lifetimes);
  projection.projectionConcept = id;
  projection.projectionMember = member;
  return reduceAssociatedTypeProjection(std::move(projection));
}

auto SemanticVisitor::Impl::reduceAssociatedTypeProjection(
    SemanticType projection) const -> SemanticType {
  if (!projection.isAssociatedTypeProjection() ||
      projection.arguments.empty()) {
    return projection;
  }
  if (activeProjections.empty())
    projectionWork = 0;
  const auto charge = [&](const auto &self, const SemanticType &type) -> bool {
    if (++projectionWork > maximumProjectionWork)
      return false;
    for (const auto &argument : type.arguments)
      if (!self(self, argument))
        return false;
    return true;
  };
  if (activeProjections.size() >= maximumProjectionDepth ||
      !charge(charge, projection) ||
      std::find(activeProjections.begin(), activeProjections.end(),
                projection) != activeProjections.end())
    return SemanticType::Unknown;
  struct ProjectionScope {
    explicit ProjectionScope(const SemanticType &projection) {
      activeProjections.push_back(projection);
    }
    ~ProjectionScope() { activeProjections.pop_back(); }
    ProjectionScope(const ProjectionScope &) = delete;
    ProjectionScope &operator=(const ProjectionScope &) = delete;
  } scope(projection);
  const auto unknown = [&](const auto &self, const SemanticType &type) -> bool {
    return type == SemanticType::Unknown ||
           std::any_of(
               type.arguments.begin(), type.arguments.end(),
               [&](const auto &argument) { return self(self, argument); });
  };
  if (unknown(unknown, projection))
    return SemanticType::Unknown;
  const auto &conceptInfo = concepts[projection.projectionConcept - 1];
  const std::size_t ownerArity = conceptInfo.parameterIds.size();
  const std::size_t ownerLifetimeArity = conceptInfo.lifetimeParameterIds.size();
  if (projection.projectionMember >=
          conceptInfo.associatedTypeParameters.size() ||
      projection.arguments.size() !=
          ownerArity +
              genericTypeParameterCount(conceptInfo.associatedTypeParameters[
                  projection.projectionMember]) ||
      projection.lifetimeArguments.size() != ownerLifetimeArity +
          genericLifetimeParameterCount(conceptInfo.associatedTypeParameters[
              projection.projectionMember]))
    return projection;
  const std::vector<SemanticType> ownerArguments(
      projection.arguments.begin(), projection.arguments.begin() + ownerArity);
  const std::vector<Lifetime> ownerLifetimes(
      projection.lifetimeArguments.begin(),
      projection.lifetimeArguments.begin() + ownerLifetimeArity);
  const std::optional<ConformanceMatch> match =
      matchConformance(projection.projectionConcept, ownerArguments, ownerLifetimes);
  if (!match) {
    return projection;
  }
  const RegisteredConformance &conformance =
      conformances[match->conformance - 1];
  if (projection.projectionMember >= conformance.associatedTypes.size()) {
    return SemanticType::Unknown;
  }
  GenericSubstitution substitution = nominalSubstitution(
      conformance.genericParameters, {.types = match->binding});
  substitution.lifetimes = match->lifetimeBinding;
  const auto &familyParameters =
      conformance.associatedTypeParameters[projection.projectionMember];
  if (projection.arguments.size() != ownerArity +
          genericTypeParameterCount(familyParameters) ||
      projection.lifetimeArguments.size() != ownerLifetimeArity +
          genericLifetimeParameterCount(familyParameters))
    return SemanticType::Unknown;
  std::size_t typeIndex = ownerArity, lifetimeIndex = ownerLifetimeArity;
  for (const auto &parameter : familyParameters)
    if (parameter.lifetime)
      substitution.lifetimes.emplace(parameter.id,
          projection.lifetimeArguments[lifetimeIndex++]);
    else
      substitution.types.emplace(parameter.id, projection.arguments[typeIndex++]);
  SemanticType result = substituteType(
      conformance.associatedTypes[projection.projectionMember], substitution);
  return unknown(unknown, result) ? SemanticType::Unknown : result;
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
    if (parameter.lifetime) {
      const Lifetime own = lifetimeOf(parameter.name);
      if (own.specified())
        substitution.lifetimes.emplace(parameter.id, own);
      continue;
    }
    if (const std::optional<SemanticType> own =
            resolveTypeParameter(NamePath(parameter.name))) {
      substitution.types.emplace(parameter.id, *own);
    }
  }
  return substituteType(
      substituteType(type, substitution),
      GenericSubstitution{.types = instanceTypeSubstitution,
                          .values = instanceValueSubstitution,
                          .lifetimes = instanceLifetimeSubstitution});
}

// Inside a block's function its own associated types are transparent: an
// associated name, or a projection of the block's own pattern, is the
// block's `using` type.
auto SemanticVisitor::Impl::blockAssociatedType(
    std::string_view name, const std::vector<std::string> &fromScope,
    const std::vector<SemanticType> &familyArguments,
    const std::vector<Lifetime> &familyLifetimes) const
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
  const auto &parameters = conformance->associatedTypeParameters[*member];
  if (genericTypeParameterCount(parameters) != familyArguments.size() ||
      genericLifetimeParameterCount(parameters) != familyLifetimes.size())
    return SemanticType::Unknown;
  GenericSubstitution substitution = nominalSubstitution(
      parameters, {.types = familyArguments, .lifetimes = familyLifetimes});
  return substituteType(
      blockScopeType(*conformance, conformance->associatedTypes[*member]),
      substitution);
}

auto SemanticVisitor::Impl::ownBlockProjection(
    ConceptId id, std::uint32_t member,
    const std::vector<SemanticType> &arguments,
    const std::vector<std::string> &fromScope,
    const std::vector<SemanticType> &familyArguments,
    const std::vector<Lifetime> &lifetimes,
    const std::vector<Lifetime> &familyLifetimes) const
    -> std::optional<SemanticType> {
  const RegisteredConformance *conformance = enclosingConformance(fromScope);
  if (conformance == nullptr || conformance->conceptId != id ||
      member >= conformance->associatedTypes.size()) {
    return std::nullopt;
  }
  const auto &target = conformance->declaration->target();
  const auto own = nominalArguments(concepts[id - 1].genericParameters,
                                    target.arguments, fromScope,
                                    target.name.last());
  if (!own || own->types != arguments || own->lifetimes != lifetimes)
    return std::nullopt;
  return blockAssociatedType(concepts[id - 1].associatedTypes[member],
                             fromScope, familyArguments, familyLifetimes);
}

auto SemanticVisitor::Impl::protocolOperationType(
    ConceptId id, const SemanticType &type,
    const std::vector<SemanticType> &arguments,
    const std::vector<Lifetime> &lifetimes) const -> SemanticType {
  const RegisteredConcept &conceptInfo = concepts[id - 1];
  GenericSubstitution substitution;
  for (std::size_t position = 0; position < conceptInfo.parameterIds.size() &&
                                 position < arguments.size();
       ++position) {
    substitution.types.emplace(conceptInfo.parameterIds[position],
                               arguments[position]);
  }
  for (std::size_t position = 0; position < conceptInfo.lifetimeParameterIds.size() &&
                                 position < lifetimes.size(); ++position)
    substitution.lifetimes.emplace(conceptInfo.lifetimeParameterIds[position],
                                   lifetimes[position]);
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
      .conformanceArguments = requirement.arguments,
      .conceptExplanation = conceptInfo.explanation()};
  if (requirement.arguments.size() != conceptInfo.parameterIds.size() ||
      requirement.lifetimeArguments.size() != conceptInfo.lifetimeParameterIds.size()) {
    return failure;
  }
  if (activeConformanceRequirements.empty())
    conformanceRequirementWork = 0;
  const auto charge = [&](const auto &self, const SemanticType &type) -> bool {
    if (++conformanceRequirementWork > maximumProjectionWork)
      return false;
    for (const auto &argument : type.arguments)
      if (!self(self, argument))
        return false;
    return true;
  };
  const bool withinBudget =
      std::all_of(requirement.arguments.begin(), requirement.arguments.end(),
                  [&](const auto &type) { return charge(charge, type); });
  const auto key = std::tuple{requirement.conceptId, requirement.arguments,
                              requirement.lifetimeArguments};
  if (!withinBudget ||
      activeConformanceRequirements.size() >= maximumProjectionDepth ||
      std::find(activeConformanceRequirements.begin(),
                activeConformanceRequirements.end(),
                key) != activeConformanceRequirements.end()) {
    failure.missingConformance = false;
    failure.expressionFailure =
        "Conformance requirements are cyclic or exceed the nesting limit or "
        "type-expansion budget.";
    return failure;
  }
  activeConformanceRequirements.push_back(key);
  struct RequirementScope {
    decltype(activeConformanceRequirements) &active;
    ~RequirementScope() { active.pop_back(); }
  } activeScope{activeConformanceRequirements};
  const std::optional<ConformanceMatch> match =
      matchConformance(requirement.conceptId, requirement.arguments,
                       requirement.lifetimeArguments);
  if (!match) {
    return failure;
  }
  const RegisteredConformance &conformance =
      conformances[match->conformance - 1];
  if (auto constraint = firstConstraintFailure(conformance.genericParameters,
                                               match->binding)) {
    if (failure.conceptExplanation)
      constraint->conceptExplanation = failure.conceptExplanation;
    return constraint;
  }
  GenericSubstitution substitution = nominalSubstitution(
      conformance.genericParameters, {.types = match->binding});
  substitution.lifetimes = match->lifetimeBinding;
  const auto replaceLifetime = [&](Lifetime region) {
    const auto found = substitution.lifetimes.find(region.id);
    return region.kind == Lifetime::Kind::Parameter &&
                   found != substitution.lifetimes.end()
               ? found->second : region;
  };
  auto requirements = conformance.requirements;
  for (auto &nested : requirements) {
    for (auto &argument : nested.arguments)
      argument = substituteType(argument, substitution);
    for (auto &lifetime : nested.lifetimeArguments)
      lifetime = replaceLifetime(lifetime);
    if (nested.lifetimeBound) {
      nested.lifetimeBound->longer = replaceLifetime(nested.lifetimeBound->longer);
      nested.lifetimeBound->shorter = replaceLifetime(nested.lifetimeBound->shorter);
    }
    if (nested.outlivesType)
      nested.outlivesType = substituteType(*nested.outlivesType, substitution);
  }
  if (auto constraint = firstRequirementFailure(requirements))
    return constraint;
  if (const auto operation =
          protocolBorrowContractFailure(conformance, match->binding,
                                         match->lifetimeBinding)) {
    failure.missingConformance = false;
    failure.conformanceBorrowOperation =
        conceptInfo.operations[*operation].declaration->name();
    return failure;
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::protocolBorrowContractFailure(
    const RegisteredConformance &conformance,
    const std::vector<SemanticType> &binding,
    const LifetimeSubstitution &lifetimes) const
    -> std::optional<std::size_t> {
  const auto &operations = concepts[conformance.conceptId - 1].operations;
  for (std::size_t index = 0; index < operations.size(); ++index) {
    const auto &operation = operations[index];
    if (!operation.returnBorrowParameter)
      continue;
    if (index >= conformance.operations.size() ||
        conformance.operations[index].overloads.size() != 1)
      return index;
    const auto &candidate = conformance.operations[index].overloads.front();
    const auto &parameters = candidate.record().genericParameters;
    if (genericTypeParameterCount(parameters) != binding.size() ||
        parameters.size() != conformance.genericParameters.size())
      return index;
    GenericSubstitution substitution = nominalSubstitution(parameters, {.types = binding});
    for (std::size_t parameter = 0; parameter < parameters.size(); ++parameter)
      if (parameters[parameter].lifetime) {
        const auto found = lifetimes.find(conformance.genericParameters[parameter].id);
        if (found != lifetimes.end())
          substitution.lifetimes.emplace(parameters[parameter].id, found->second);
      }
    const auto returnType = substituteType(candidate.returnType, substitution);
    if (returnType == SemanticType::Unknown)
      return index;
    // An open result may become a reference. Require the declared dependency
    // already; concrete analysis still verifies its exact origins and access.
    if (returnType.kind != SemanticType::Reference &&
        !containsOpenTypeParameter(returnType))
      continue;
    const auto *function = candidate.declaration;
    const auto *origin =
        function ? function->findFunctionAttribute("borrowed_from") : nullptr;
    const auto parameter = *operation.returnBorrowParameter;
    if (!origin || !origin->argument ||
        parameter >= function->parameters().size() ||
        origin->argument->lexeme !=
            function->parameters()[parameter].name.lexeme)
      return index;
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::validateAssociatedTypeArguments(
    const TypeRef &type, ConceptId id, std::uint32_t member) -> bool {
  const auto before = diagnostics.size();
  const auto &parameters = concepts[id - 1].associatedTypeParameters[member];
  const auto arguments = resolveNominalArguments(
      parameters, type.name.last(), type.arguments, type.name.last(),
      "GTI-S2123");
  return arguments.valid && diagnostics.size() == before;
}

auto SemanticVisitor::Impl::validateAssociatedTypePath(const TypeRef &type,
                                                       ConceptId id) -> void {
  const TypeRef &owner = *type.qualifiedOwner;
  const RegisteredConcept &conceptInfo = concepts[id - 1];
  const Token &site = type.name.last();
  if (site.completion) {
    captureQualifiedCompletion(type.name, type.qualifiedOwner.get(), true);
    return;
  }
  const auto resolved = resolveNominalArguments(
      conceptInfo.genericParameters, conceptInfo.declaration->name(),
      owner.arguments, owner.name.last(), "GTI-S2123");
  if (!resolved.valid)
    return;
  const auto member = memberIndex(conceptInfo.associatedTypes, site.lexeme);
  if (!member) {
    reportConformanceError(site, "Concept '" + conceptInfo.qualifiedName +
                                     "' has no associated type '" +
                                     site.lexeme + "'.");
    return;
  }
  if (!validateAssociatedTypeArguments(type, id, *member))
    return;
  if (deferringTypeAliasConstraints)
    return;
  const auto &arguments = resolved.types;
  const std::string application = applicationText(
      conceptInfo.qualifiedName, arguments,
      [&](const SemanticType &argument) { return typeSpelling(argument); });
  if (std::any_of(arguments.begin(), arguments.end(),
                  [&](const SemanticType &argument) {
                    return containsOpenTypeParameter(argument);
                  })) {
    if (firstConceptApplicationFailure(
            {.conceptId = id, .arguments = arguments,
             .lifetimeArguments = resolved.lifetimes})) {
      reportConformanceError(site, "'" + application + "::" + site.lexeme +
                                       "' requires 'requires " + application +
                                       "' on the enclosing declaration.");
      return;
    }
    if (typeOf(type) == SemanticType::Unknown)
      reportConformanceError(site, "Associated type application is unresolved, "
                                   "cyclic, or exceeds the projection limit.");
    return;
  }
  // The selected block must also accept the arguments, exactly as
  // `requires` checks it, so a type cannot name an associated type through
  // a block whose constraints it fails.
  const std::optional<ConstraintFailure> failure =
      protocolConformanceFailure({.conceptId = id, .arguments = arguments,
             .lifetimeArguments = resolved.lifetimes});
  if (failure && failure->missingConformance) {
    reportConformanceError(site, "No conformance block 'for concept " +
                                     application + "' is declared.");
  } else if (failure) {
    reportConstraintFailure(site, *failure);
  } else if (typeOf(type) == SemanticType::Unknown) {
    reportConformanceError(site, "Associated type application is unresolved, "
                                 "cyclic, or exceeds the projection limit.");
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
  if (!id)
    return std::nullopt;
  const auto resolved = nominalArguments(concepts[*id - 1].genericParameters,
                                         owner.arguments, currentNamespace,
                                         owner.name.last());
  if (!resolved)
    return std::nullopt;
  std::vector<SemanticType> arguments;
  const GenericSubstitution instance{.types = instanceTypeSubstitution,
                                     .values = instanceValueSubstitution,
                                     .lifetimes = instanceLifetimeSubstitution};
  for (const auto &argument : resolved->types)
    arguments.push_back(substituteType(argument, instance));
  const std::optional<ConformanceMatch> match =
      matchConformance(*id, arguments, resolved->lifetimes);
  if (!match) {
    return std::nullopt;
  }
  const RegisteredConformance &conformance =
      conformances[match->conformance - 1];
  if (protocolBorrowContractFailure(conformance, match->binding,
                                    match->lifetimeBinding))
    return std::nullopt;
  const std::optional<std::size_t> operation =
      operationIndex(concepts[*id - 1].operations, callee.name().last().lexeme);
  if (!operation || *operation >= conformance.operations.size() ||
      conformance.operations[*operation].overloads.empty()) {
    return std::nullopt;
  }
  std::vector<Lifetime> lifetimeArguments;
  for (const auto &parameter : conformance.genericParameters) {
    if (!parameter.lifetime)
      continue;
    const auto found = match->lifetimeBinding.find(parameter.id);
    if (found == match->lifetimeBinding.end())
      return std::nullopt;
    lifetimeArguments.push_back(found->second);
  }
  return ProtocolOperationTarget{.symbol = conformance.operations[*operation],
                                 .typeArguments = match->binding,
                                 .lifetimeArguments = std::move(lifetimeArguments)};
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
  const Token &site = callee.name().last();
  const auto resolved = resolveNominalArguments(
      conceptInfo.genericParameters, conceptInfo.declaration->name(),
      owner.arguments, owner.name.last(), "GTI-S2123");
  if (!resolved.valid)
    return fail();
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
  const GenericSubstitution instance{.types = instanceTypeSubstitution,
                                     .values = instanceValueSubstitution,
                                     .lifetimes = instanceLifetimeSubstitution};
  for (const auto &argument : resolved.types) {
    arguments.push_back(substituteType(argument, instance));
    if (arguments.back() == SemanticType::Unknown)
      return fail();
  }
  const std::string application = applicationText(
      conceptInfo.qualifiedName, arguments,
      [&](const SemanticType &argument) { return typeSpelling(argument); });
  if (std::none_of(arguments.begin(), arguments.end(),
                   [&](const SemanticType &argument) {
                     return containsOpenTypeParameter(argument);
                   })) {
    if (const auto match = matchConformance(id, arguments, resolved.lifetimes)) {
      const auto &conformance = conformances[match->conformance - 1];
      if (const auto failed =
              protocolBorrowContractFailure(conformance, match->binding,
                                             match->lifetimeBinding)) {
        const auto &operation = conceptInfo.operations[*failed];
        reportConformanceError(
            site,
            "Operation '" + operation.name +
                "' must preserve the concept's borrowed_from parameter "
                "contract for its concrete reference result.",
            &operation.declaration->name(), "Operation declared here.");
        return fail();
      }
    }
    // Preserve the direct operation's precise signature diagnostic above.
    // An invalid contract reached through a nested requirement instead makes
    // that requirement unsatisfied and keeps its constraint provenance here.
    if (const auto failure = protocolConformanceFailure(
            {.conceptId = id, .arguments = arguments,
             .lifetimeArguments = resolved.lifetimes});
        failure && !failure->missingConformance) {
      reportConstraintFailure(site, *failure);
      return fail();
    }
    if (!protocolOperationTarget(callee)) {
      reportConformanceError(site, "No conformance block 'for concept " +
                                       application + "' is declared.");
      return fail();
    }
    return false;
  }
  if (firstConceptApplicationFailure(
          {.conceptId = id, .arguments = arguments,
             .lifetimeArguments = resolved.lifetimes})) {
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
    parameterTypes.push_back(protocolOperationType(id, parameter, arguments, resolved.lifetimes));
  }
  const SemanticType returnType =
      protocolOperationType(id, operation.returnType, arguments, resolved.lifetimes);
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
  const DeclarationOrderScope declarationOrder{*this, stmt};
  validateConformanceTypes(stmt);
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
