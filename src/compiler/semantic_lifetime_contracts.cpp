// RFC 0004 signature lifetime contracts.
//
// Lifetimes never reach SemanticType: the parser moves every lifetime
// argument out of a type's argument list and `outlives<...>` out of the
// type-level requirement expression. A declaration's lifetime contract is
// therefore read from its written signature here, and translated into the
// same core borrow facts that `[[borrowed_from]]` produces. Body-local
// lifetime validity is not decided here (see docs/architecture/mir.md).

#include "semantic_analyzer_impl.h"

#include <algorithm>

namespace lang {

namespace {

// Every region name written in a type: its reference region and the lifetime
// arguments of it and its nested type arguments.
void collectRegionNames(const TypeRef &type, std::vector<std::string> &names) {
  if (type.referenceLifetime &&
      type.referenceLifetime->name.kind == TokenKind::IDENTIFIER)
    names.push_back(type.referenceLifetime->name.lexeme);
  for (const auto &argument : type.lifetimeArguments)
    if (argument.name.segments.size() == 1)
      names.push_back(argument.name.last().lexeme);
  for (const auto &argument : type.arguments)
    collectRegionNames(argument, names);
}

// The single region a result names, from the reference or its first lifetime
// argument; empty when the result elides every region.
std::optional<Token> resultRegion(const TypeRef &result) {
  if (result.referenceLifetime)
    return result.referenceLifetime->name;
  if (!result.lifetimeArguments.empty() &&
      result.lifetimeArguments.front().name.segments.size() == 1)
    return result.lifetimeArguments.front().name.last();
  // A region written inside a type argument, as in `Result<int&<a>, E>`,
  // is the region of the reference the result carries.
  for (const TypeRef &argument : result.arguments)
    if (auto nested = resultRegion(argument))
      return nested;
  return std::nullopt;
}

bool contains(const std::vector<std::string> &names, const std::string &name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

auto SemanticVisitor::Impl::explicitLifetimeResult(
    const FunctionDecl &syntax, std::span<const SemanticType> parameterTypes,
    ClassId ownerClass) const -> ExplicitLifetimeResult {
  ExplicitLifetimeResult result;
  const auto region = resultRegion(syntax.returnType());
  if (!region)
    return result;
  const std::string &name = region->lexeme;
  if (region->kind == TokenKind::THIS || name == "this") {
    result.kind = ExplicitLifetimeResult::Kind::This;
    return result;
  }
  if (region->kind == TokenKind::STATIC || name == "static") {
    result.kind = ExplicitLifetimeResult::Kind::Static;
    return result;
  }
  if (name == "_")
    return result; // An explicit inference request is still elision.
  result.kind = ExplicitLifetimeResult::Kind::Region;
  result.region = name;

  // A region bound by the enclosing class is a stored region: the receiver's
  // fields that name it supply the result, never the receiver borrow itself.
  if (ownerClass != 0) {
    const auto *declaration = classInfo(ownerClass).declaration;
    if (declaration != nullptr) {
      for (const auto &parameter : declaration->lifetimeParameters())
        if (parameter.name.lexeme == name)
          result.classBinder = true;
      // A class argument pattern such as `int&<source>` binds `source`.
      std::vector<std::string> patternRegions;
      for (const auto &pattern : declaration->genericArgumentPatterns())
        collectRegionNames(pattern, patternRegions);
      if (contains(patternRegions, name))
        result.classBinder = true;
    }
  }

  // Regions that outlive the result's region also supply it.
  std::vector<std::string> suppliers{name};
  if (const auto &clause = syntax.requiresClause())
    for (const auto &requirement : clause->expression.lifetimeRequirements) {
      std::vector<std::string> pair;
      for (const auto &argument : requirement.arguments)
        if (argument.name.segments.size() == 1)
          pair.push_back(argument.name.last().lexeme);
      for (const auto &argument : requirement.arguments)
        collectRegionNames(argument, pair);
      if (pair.size() >= 2 && pair[1] == name && !contains(suppliers, pair[0]))
        suppliers.push_back(pair[0]);
    }

  // Expanded parameter positions, so the index agrees with MIR's bindings.
  std::vector<SemanticType> expanded;
  for (const SemanticType &type : parameterTypes)
    SemanticType::appendExpandedArgument(expanded, type);
  const auto &parameters = syntax.parameters();
  std::size_t position = 0;
  for (std::size_t index = 0;
       index < parameters.size() && index < parameterTypes.size(); ++index) {
    const std::size_t width =
        parameters[index].pack ? expanded.size() - (parameterTypes.size() - 1)
                               : 1;
    std::vector<std::string> names;
    collectRegionNames(parameters[index].type, names);
    const bool supplies = std::any_of(names.begin(), names.end(),
                                      [&](const std::string &candidate) {
                                        return contains(suppliers, candidate);
                                      });
    if (supplies)
      for (std::size_t element = position;
           element < position + width && element < expanded.size(); ++element)
        result.parameters.push_back(element);
    position += width;
  }
  return result;
}

auto SemanticVisitor::Impl::constructorLifetimeSuppliers(
    const ClassDecl &owner, const ConstructorDecl &constructor) const
    -> std::vector<std::size_t> {
  std::vector<std::string> classRegions;
  for (const auto &parameter : owner.lifetimeParameters())
    classRegions.push_back(parameter.name.lexeme);
  for (const auto &pattern : owner.genericArgumentPatterns())
    collectRegionNames(pattern, classRegions);
  std::vector<std::size_t> suppliers;
  if (classRegions.empty())
    return suppliers;
  const auto &parameters = constructor.parameters();
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (parameters[index].pack)
      return {};
    std::vector<std::string> names;
    collectRegionNames(parameters[index].type, names);
    if (std::any_of(names.begin(), names.end(), [&](const std::string &name) {
          return contains(classRegions, name);
        }))
      suppliers.push_back(index);
  }
  return suppliers;
}

auto SemanticVisitor::Impl::hasExplicitBorrowContract(
    const FunctionDecl &syntax) -> bool {
  if (syntax.hasFunctionAttribute("borrowed_from"))
    return true;
  // `static` keeps the inferred static-storage origin; the declared region is
  // checked against it, not substituted for it.
  // Only `this` and a region the function itself binds state a contract:
  // class-bound (stored) regions are what the existing inference derives
  // from the receiver's fields.
  const auto region = resultRegion(syntax.returnType());
  if (!region)
    return false;
  // `this` states the receiver contract `[[borrowed_from(this)]]` does.
  if (region->kind == TokenKind::THIS || region->lexeme == "this")
    return true;
  const auto parameters = syntax.lifetimeParameters();
  return std::any_of(parameters.begin(), parameters.end(),
                     [&](const GenericParameter &parameter) {
                       return parameter.name.lexeme == region->lexeme;
                     });
}

// The one parameter a declaration's explicit result contract names, however
// it is spelled: the borrowed_from attribute's argument, or the parameter its
// result region binds. Empty for an inferred, receiver or multi-supplier one.
auto SemanticVisitor::Impl::declaredContractParameter(
    const FunctionDecl &syntax, const SemanticType &returnType,
    std::span<const SemanticType> parameterTypes) const
    -> std::optional<std::size_t> {
  if (const auto *origin = syntax.findFunctionAttribute("borrowed_from")) {
    if (!origin->argument)
      return std::nullopt;
    for (std::size_t index = 0; index < syntax.parameters().size(); ++index)
      if (syntax.parameters()[index].name.lexeme == origin->argument->lexeme)
        return index;
    return std::nullopt;
  }
  if (!hasExplicitBorrowContract(syntax))
    return std::nullopt;
  (void)returnType;
  const auto lifetime = explicitLifetimeResult(syntax, parameterTypes, 0);
  if (lifetime.kind != ExplicitLifetimeResult::Kind::Region ||
      lifetime.parameters.size() != 1)
    return std::nullopt;
  return lifetime.parameters.front();
}

// RFC 0004 elision as a complete declaration contract, from types alone. Every
// reference-bearing result path may be supplied by every eligible input: a
// free function's reference and borrowed-state parameters, or a method's
// receiver (its own storage unless consumed, and the regions it stores). A
// mutable result path accepts only mutable sources. The body is checked
// against this upper bound in MIR; nothing is inferred from it.
auto SemanticVisitor::Impl::signatureCompleteReturnBorrow(
    const FunctionInfo &function, ReceiverMutability receiverMutability,
    const SemanticType &concreteReceiver)
    -> std::optional<FunctionBorrowDependencySummary> {
  const SemanticType &result = function.returnType;
  const bool directReference =
      result.kind == SemanticType::Reference && result.arguments.size() == 1;
  std::optional<std::vector<StoredReferencePath>> inventory;
  if (directReference)
    inventory = std::vector<StoredReferencePath>{{.reference = result}};
  else
    inventory = storedReferenceInventory(result);
  if (!inventory || inventory->empty())
    return std::nullopt;
  // Complete contracts cannot describe an escaping mutable dependency graph:
  // a carrier result with a mutable stored path keeps its base origin.
  if (!directReference &&
      std::any_of(inventory->begin(), inventory->end(), [](const auto &path) {
        return path.reference.referenceAccess == AccessMode::Mutable;
      }))
    return std::nullopt;
  const auto alternatives =
      directReference ? std::optional{std::vector<BorrowAlternativePath>{}}
                      : storedBorrowAlternativeInventory(result);
  if (!alternatives)
    return std::nullopt;

  struct Source {
    BorrowOriginKind origin;
    std::size_t parameter;
    std::vector<SymbolId> fields;
    std::vector<BorrowDependencyCondition> conditions;
    AccessMode access;
    // The stored reference this source reaches; Unknown for caller storage,
    // which may hold a referent of any type.
    SemanticType reference = SemanticType::Unknown;
  };
  std::vector<Source> sources;
  // A value's stored regions, each reached through its own field path.
  const auto addStored = [&](BorrowOriginKind origin, std::size_t parameter,
                             const SemanticType &type) -> bool {
    const auto paths = storedReferenceInventory(type);
    if (!paths)
      return false;
    for (const auto &path : *paths) {
      auto fields = storedFieldSymbols(type, path.fields);
      auto conditions = storedConditionSymbols(type, path.conditions);
      if (fields.size() != path.fields.size() || !conditions)
        return false;
      sources.push_back({origin, parameter, std::move(fields),
                         std::move(*conditions), path.reference.referenceAccess,
                         path.reference});
    }
    return true;
  };
  // An explicit contract names its suppliers: the receiver or the listed
  // parameters. Otherwise a method elides to its receiver and a free
  // function to every borrow-carrying input.
  const bool method = function.ownerClass != 0 && !function.staticMember;
  std::vector<std::size_t> namedParameters;
  bool receiverSupplies = method;
  bool parametersSupply = !method;
  if (function.explicitReturnBorrowOrigin) {
    switch (function.returnBorrowOrigin) {
    case BorrowOriginKind::Receiver:
      if (!method)
        return std::nullopt;
      receiverSupplies = true;
      parametersSupply = false;
      break;
    case BorrowOriginKind::Argument:
      receiverSupplies = false;
      parametersSupply = true;
      namedParameters = function.returnBorrowParameters.empty()
                            ? std::vector{function.returnBorrowParameter}
                            : function.returnBorrowParameters;
      break;
    default:
      return std::nullopt;
    }
  }
  const auto named = [&](std::size_t index) {
    return namedParameters.empty() ||
           std::find(namedParameters.begin(), namedParameters.end(), index) !=
               namedParameters.end();
  };
  if (receiverSupplies) {
    const SemanticType receiver = concreteReceiver.kind == SemanticType::Class
                                      ? concreteReceiver
                                      : openClassType(function.ownerClass);
    if (receiverMutability != ReceiverMutability::Consuming)
      sources.push_back({BorrowOriginKind::Receiver,
                         0,
                         {},
                         {},
                         receiverAllowsMutation(receiverMutability)
                             ? AccessMode::Mutable
                             : AccessMode::ReadOnly});
    if (!addStored(BorrowOriginKind::Receiver, 0, receiver))
      return std::nullopt;
  }
  if (parametersSupply) {
    for (std::size_t index = 0; index < function.parameterTypes.size();
         ++index) {
      if (!named(index))
        continue;
      const SemanticType &parameter = function.parameterTypes[index];
      if (parameter.kind == SemanticType::Reference &&
          parameter.arguments.size() == 1) {
        sources.push_back({BorrowOriginKind::Argument,
                           index,
                           {},
                           {},
                           parameter.referenceAccess});
        if (typeTraits(parameter.arguments.front()).containsBorrowedState &&
            !addStored(BorrowOriginKind::Argument, index,
                       parameter.arguments.front()))
          return std::nullopt;
      } else if (typeTraits(parameter).containsBorrowedState &&
                 !addStored(BorrowOriginKind::Argument, index, parameter)) {
        return std::nullopt;
      }
    }
  }

  FunctionBorrowDependencySummary summary{.possibleAlternatives =
                                              *alternatives};
  for (const auto &path : *inventory) {
    auto fields = storedFieldSymbols(result, path.fields);
    auto conditions = storedConditionSymbols(result, path.conditions);
    if (fields.size() != path.fields.size() || !conditions)
      return std::nullopt;
    const AccessMode access = path.reference.referenceAccess;
    bool supplied = false;
    for (const Source &source : sources) {
      if (access == AccessMode::Mutable && source.access != AccessMode::Mutable)
        continue;
      // A stored reference supplies only a referent of its own type.
      if (source.reference.kind == SemanticType::Reference &&
          (source.reference.arguments.size() != 1 ||
           path.reference.arguments.size() != 1 ||
           source.reference.arguments.front() !=
               path.reference.arguments.front()))
        continue;
      supplied = true;
      BorrowedFieldOrigin dependency{.resultFields = fields,
                                     .origin = source.origin,
                                     .parameter = source.parameter,
                                     .sourceFields = source.fields,
                                     .access = access,
                                     .resultConditions = *conditions,
                                     .sourceConditions = source.conditions};
      if (std::find(summary.dependencies.begin(), summary.dependencies.end(),
                    dependency) == summary.dependencies.end())
        summary.dependencies.push_back(std::move(dependency));
    }
    if (!supplied)
      return std::nullopt;
  }
  // A direct reference borrowed whole from one supplier is exactly its base
  // origin; a complete contract would only restate it.
  if (directReference && summary.dependencies.size() == 1 &&
      summary.dependencies.front().sourceFields.empty() &&
      summary.dependencies.front().sourceConditions.empty())
    return std::nullopt;
  return summary;
}

} // namespace lang
