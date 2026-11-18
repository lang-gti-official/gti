// RFC 0004 signature lifetime contracts.
//
// Lifetimes never reach SemanticType: the parser moves every lifetime
// argument out of a type's argument list and `outlives<...>` out of the
// type-level requirement expression. A declaration's lifetime contract is
// therefore read from its written signature here, and translated into the
// core borrow facts semantics publishes. Body-local
// lifetime validity is not decided here (see docs/architecture/mir.md).

#include "semantic_analyzer_impl.h"

#include <algorithm>
#include <functional>

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

// The region one `outlives` argument names: a bare region, or a type's
// written reference region (`int&<a>`) or first lifetime argument.
std::optional<std::string> argumentRegion(const TypeRef &argument) {
  if (argument.referenceLifetime)
    return argument.referenceLifetime->name.lexeme;
  // A raw pointer carries no region: `outlives<T*, r>` holds for every T and
  // states nothing about its pointee.
  if (!argument.pointers.empty() && !argument.reference)
    return std::nullopt;
  if (!argument.lifetimeArguments.empty() &&
      argument.lifetimeArguments.front().name.segments.size() == 1)
    return argument.lifetimeArguments.front().name.last().lexeme;
  if (argument.name.segments.size() == 1 && !argument.reference &&
      argument.arguments.empty())
    return argument.name.last().lexeme;
  return std::nullopt;
}

bool contains(const std::vector<std::string> &names, const std::string &name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

using Relations = std::vector<std::pair<std::string, std::string>>;

// Whether `longer` outlives `shorter` under `relations`: by identity,
// through a chain of relations, or through `static`, which outlives all.
bool outlivesUnder(const Relations &relations, const std::string &longer,
                   const std::string &shorter) {
  if (longer == shorter || longer == "static")
    return true;
  std::vector<std::string> reached{longer};
  for (bool grew = true; grew;) {
    grew = false;
    for (const auto &[from, to] : relations)
      if (contains(reached, from) && !contains(reached, to)) {
        reached.push_back(to);
        grew = true;
      }
  }
  return contains(reached, shorter) || contains(reached, "static");
}

// The region one lifetime position names; empty when elided (`_` included).
std::optional<std::string> writtenRegion(const TypeRef &argument) {
  if (argument.name.segments.size() != 1)
    return std::nullopt;
  const std::string &name = argument.name.last().lexeme;
  if (name == "_")
    return std::nullopt;
  return name;
}

std::optional<std::string>
writtenReferenceRegion(const std::optional<LifetimeArgument> &region) {
  if (!region || region->name.lexeme == "_")
    return std::nullopt;
  return region->name.lexeme;
}

// The bounds a class's fields imply between its type and lifetime
// parameters, as (type position, lifetime position): a field written
// `X<..T..>&<a>` holds a reference valid for `a` to a value of T, so a
// well-formed application requires T's argument to outlive a's (Rust's
// implied bounds of a struct with `&'a T`).
std::vector<std::pair<std::size_t, std::size_t>>
classOutlivesBounds(const ClassDecl &declaration) {
  std::vector<std::pair<std::size_t, std::size_t>> bounds;
  if (!declaration.genericArgumentPatterns().empty())
    return bounds;
  const auto &types = declaration.genericParameters();
  const auto lifetimes = declaration.lifetimeParameters();
  const auto typeIndex =
      [&](const std::string &name) -> std::optional<std::size_t> {
    for (std::size_t index = 0; index < types.size(); ++index)
      if (!types[index].valueType && types[index].name.lexeme == name)
        return index;
    return std::nullopt;
  };
  const auto lifetimeIndex =
      [&](const std::string &name) -> std::optional<std::size_t> {
    for (std::size_t index = 0; index < lifetimes.size(); ++index)
      if (lifetimes[index].name.lexeme == name)
        return index;
    return std::nullopt;
  };
  const auto mentioned = [&](const auto &self, const TypeRef &type,
                             std::vector<std::size_t> &found) -> void {
    if (type.name.segments.size() == 1)
      if (const auto index = typeIndex(type.name.last().lexeme))
        found.push_back(*index);
    for (const auto &argument : type.arguments)
      self(self, argument, found);
  };
  const auto visit = [&](const auto &self, const TypeRef &type) -> void {
    if (const auto region = writtenReferenceRegion(type.referenceLifetime))
      if (const auto lifetime = lifetimeIndex(*region)) {
        std::vector<std::size_t> found;
        mentioned(mentioned, type, found);
        for (const auto index : found)
          bounds.emplace_back(index, *lifetime);
      }
    for (const auto &argument : type.arguments)
      self(self, argument);
  };
  for (const auto &member : declaration.members())
    if (const auto *field = stmtAs<VariableDecl>(member.get()))
      visit(visit, field->type());
  return bounds;
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
      if (result.classBinder) {
        result.classStoresRegion = !declaration->bases().empty();
        for (const auto &member : declaration->members())
          if (const auto *field = stmtAs<VariableDecl>(member.get())) {
            std::vector<std::string> names;
            collectRegionNames(field->type(), names);
            if (contains(names, name))
              result.classStoresRegion = true;
          }
      }
    }
  }

  // Regions that outlive the result's region also supply it.
  std::vector<std::string> suppliers{name};
  // `outlives<longer, shorter>` pairs, written directly or through a named
  // concept whose definition states them over its lifetime parameters.
  const auto regionPair = [](const ConceptApplication &requirement) {
    std::vector<std::string> pair;
    for (const auto &argument : requirement.arguments)
      if (argument.name.segments.size() == 1)
        pair.push_back(argument.name.last().lexeme);
    for (const auto &argument : requirement.arguments)
      collectRegionNames(argument, pair);
    return pair;
  };
  // Relations every alternative holds, plus each alternative's own.
  std::vector<std::pair<std::string, std::string>> outlives;
  std::vector<std::vector<std::pair<std::string, std::string>>> alternatives;
  if (const auto &clause = syntax.requiresClause()) {
    for (const auto &requirement : clause->expression.lifetimeRequirements)
      if (const auto pair = regionPair(requirement); pair.size() >= 2)
        outlives.emplace_back(pair[0], pair[1]);
    for (const auto &alternative : clause->expression.lifetimeAlternatives) {
      auto &relations = alternatives.emplace_back();
      for (const auto &requirement : alternative)
        if (const auto pair = regionPair(requirement); pair.size() >= 2)
          relations.emplace_back(pair[0], pair[1]);
    }
    for (const auto &application : clause->expression.applications) {
      auto id = resolveConceptPath(application.name, currentNamespace);
      if (!id)
        id = resolveConceptPathGlobally(application.name, currentNamespace);
      const ConceptDecl *named = id && *id <= concepts.size()
                                     ? concepts[*id - 1].declaration
                                     : nullptr;
      if (named == nullptr)
        continue;
      const auto parameters = named->lifetimeParameters();
      const auto arguments = regionPair(application);
      const auto rename = [&](const std::string &region) -> std::string {
        for (std::size_t index = 0;
             index < parameters.size() && index < arguments.size(); ++index)
          if (parameters[index].name.lexeme == region)
            return arguments[index];
        return region;
      };
      for (const auto &requirement :
           named->constraintExpression().lifetimeRequirements)
        if (const auto pair = regionPair(requirement); pair.size() >= 2)
          outlives.emplace_back(rename(pair[0]), rename(pair[1]));
    }
  }
  // A region supplies the result when, in every alternative, it outlives
  // the result's region through a chain of relations.
  if (alternatives.empty())
    alternatives.emplace_back();
  const auto outlivesIn = [&](const auto &relations, const std::string &from) {
    std::vector<std::string> reached{from};
    for (bool grew = true; grew;) {
      grew = false;
      for (const auto &[longer, shorter] : relations)
        if (contains(reached, longer) && !contains(reached, shorter)) {
          reached.push_back(shorter);
          grew = true;
        }
    }
    return contains(reached, name);
  };
  std::vector<std::string> candidates;
  for (const auto &[longer, shorter] : outlives)
    candidates.push_back(longer);
  for (const auto &relations : alternatives)
    for (const auto &[longer, shorter] : relations)
      candidates.push_back(longer);
  for (const auto &candidate : candidates) {
    if (contains(suppliers, candidate))
      continue;
    const bool always = std::all_of(
        alternatives.begin(), alternatives.end(), [&](const auto &relations) {
          auto all = outlives;
          all.insert(all.end(), relations.begin(), relations.end());
          return outlivesIn(all, candidate);
        });
    if (always)
      suppliers.push_back(candidate);
  }

  // Expanded parameter positions, so the index agrees with MIR's bindings.
  std::vector<SemanticType> expanded;
  for (const SemanticType &type : parameterTypes)
    SemanticType::appendExpandedArgument(expanded, type);
  const auto &parameters = syntax.parameters();
  std::size_t position = 0;
  for (std::size_t index = 0;
       index < parameters.size() && index < parameterTypes.size(); ++index) {
    // A pack's lanes are what the fixed formals leave, whether the types
    // arrive grouped or already expanded.
    const std::size_t width =
        parameters[index].pack ? expanded.size() - (parameters.size() - 1) : 1;
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

auto SemanticVisitor::Impl::resultBorrowsOnlyStoredRegions(
    const FunctionDecl &syntax, ClassId ownerClass, bool elidedPositions) const
    -> bool {
  if (ownerClass == 0)
    return false;
  const auto *declaration = classInfo(ownerClass).declaration;
  if (declaration == nullptr)
    return false;
  // The regions the receiver stores, and `static`, outlive any one receiver
  // borrow; `this`, `_` and a method's own binder do not.
  std::vector<std::string> external{"static"};
  for (const auto &parameter : declaration->lifetimeParameters())
    external.push_back(parameter.name.lexeme);
  for (const auto &pattern : declaration->genericArgumentPatterns())
    collectRegionNames(pattern, external);
  bool written = false;
  const auto externalRegion = [&](const std::string &name) {
    written = true;
    return contains(external, name);
  };
  const auto covered = [&](const auto &self, const TypeRef &type) -> bool {
    if (type.reference &&
        (!type.referenceLifetime ||
         !externalRegion(type.referenceLifetime->name.lexeme)))
      return false;
    for (const auto &argument : type.lifetimeArguments)
      if (argument.name.segments.size() != 1 ||
          !externalRegion(argument.name.last().lexeme))
        return false;
    if (elidedPositions && !type.reference && type.lifetimeArguments.empty()) {
      // A position written without regions elides them to the receiver
      // unless its borrowed state comes only through its type arguments.
      const SemanticType resolved = typeOf(type);
      if (resolved.kind == SemanticType::Class && resolved.classId != 0)
        if (const auto *owner = classInfo(resolved.classId).declaration;
            owner != nullptr && !owner->lifetimeParameters().empty())
          return false;
      if (typeTraits(resolved).containsBorrowedState &&
          std::none_of(
              type.arguments.begin(), type.arguments.end(),
              [&](const TypeRef &argument) {
                return typeTraits(typeOf(argument)).containsBorrowedState;
              }))
        return false;
    }
    for (const TypeRef &argument : type.arguments)
      if (!self(self, argument))
        return false;
    return true;
  };
  return covered(covered, syntax.returnType()) && written;
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

auto SemanticVisitor::Impl::validateSignatureRegions(
    const FunctionDecl &syntax, const FunctionInfo &function) -> void {
  for (const auto &parameter : syntax.parameters())
    validateWrittenPatternRegions(parameter.type, parameter.name);
  validateWrittenPatternRegions(syntax.returnType(), syntax.name());
  // RFC 0004 elision: an omitted result region uses the sole eligible input
  // (or a method's receiver). With several, the result is ambiguous and needs
  // a declared relationship (Rust E0106). The legacy body inference reports
  // the same programs from their bodies.
  // A by-value result carrying borrowed state elides its regions the same
  // way when none is written.
  const TypeRef &written = syntax.returnType();
  const bool elidedReference = written.reference && !written.referenceLifetime;
  const bool elidedCarrier =
      !written.reference && written.lifetimeArguments.empty() &&
      typeTraits(function.returnType).containsBorrowedState;
  // An out-of-class definition repeats a declaration whose signature, not
  // the definition's spelling, states the contract.
  const bool separateDefinition =
      function.declaration != nullptr && function.declaration != &syntax;
  if (!separateDefinition &&
      (function.ownerClass == 0 || function.staticMember) &&
      (elidedReference || elidedCarrier) &&
      std::none_of(syntax.parameters().begin(), syntax.parameters().end(),
                   [](const Parameter &parameter) { return parameter.pack; })) {
    std::size_t eligible = 0;
    for (const auto &type : function.parameterTypes)
      if (type.kind == SemanticType::Reference ||
          typeTraits(type).containsBorrowedState)
        ++eligible;
    if (eligible > 1) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2017", DiagnosticPhase::Semantics, syntax.name(),
          std::string(elidedReference ? "The returned reference's"
                                      : "The returned borrowing value's") +
              " elided region is ambiguous among " + std::to_string(eligible) +
              " borrowed parameters; name the suppliers with a lifetime "
              "parameter.");
      for (std::size_t index = 0; index < function.parameterTypes.size() &&
                                  index < syntax.parameters().size();
           ++index) {
        const auto &type = function.parameterTypes[index];
        if (type.kind != SemanticType::Reference &&
            !typeTraits(type).containsBorrowedState)
          continue;
        const Token &name = syntax.parameters()[index].name;
        diagnostic.related.push_back(
            {tokenSpan(name), "Parameter '" + name.lexeme +
                                  "' can supply a borrow to the result."});
      }
      diagnostic.hints.emplace_back(
          "Declare a lifetime parameter and use it on the result and its "
          "intended input supplier or suppliers. The signature must describe "
          "which inputs the returned borrow depends on.");
      diagnostics.emplace_back(std::move(diagnostic));
    }
  }
  std::vector<std::string> bound;
  for (const auto &parameter : syntax.lifetimeParameters())
    bound.push_back(parameter.name.lexeme);
  if (function.ownerClass != 0)
    if (const auto *owner = classInfo(function.ownerClass).declaration) {
      for (const auto &parameter : owner->lifetimeParameters())
        bound.push_back(parameter.name.lexeme);
      for (const auto &pattern : owner->genericArgumentPatterns())
        collectRegionNames(pattern, bound);
    }
  // Thread storage ends with its thread: it does not live for `static`
  // (Rust likewise refuses a 'static borrow of a thread_local).
  if (function.returnBorrowOrigin == BorrowOriginKind::Global &&
      function.returnBorrowPlace &&
      requestSession.threadLocalDeclarations.contains(
          function.returnBorrowPlace->root) &&
      explicitLifetimeResult(syntax, function.parameterTypes,
                             function.ownerClass)
              .kind == ExplicitLifetimeResult::Kind::Static)
    report(syntax.name(),
           "A 'static' result cannot borrow thread-local storage; it ends "
           "with its thread.",
           "GTI-S2127");
  // A result region the function binds must come from a parameter that
  // carries it, or one that carries a region declared to outlive it.
  if (std::any_of(syntax.parameters().begin(), syntax.parameters().end(),
                  [](const Parameter &parameter) { return parameter.pack; }))
    return;
  const auto lifetime =
      explicitLifetimeResult(syntax, function.parameterTypes,
                             function.staticMember ? 0 : function.ownerClass);
  if (lifetime.kind != ExplicitLifetimeResult::Kind::Region ||
      lifetime.classBinder || !lifetime.parameters.empty() ||
      !contains(bound, lifetime.region))
    return;
  if (const auto region = resultRegion(syntax.returnType()))
    report(*region,
           "No parameter supplies the result's region '" + lifetime.region +
               "': none carries it or a region declared to outlive it.",
           "GTI-S2127");
}

auto SemanticVisitor::Impl::validateReturnedFieldRegion(
    const FunctionDecl &syntax, ClassId owner, const ExprPtr &value) -> void {
  if (!value || owner == 0)
    return;
  const auto lifetime = explicitLifetimeResult(syntax, {}, owner);
  if (!lifetime.classBinder)
    return;
  const Expr *returned = value.get();
  while (const auto *group = exprAs<Grouping>(returned))
    returned = group->expression().get();
  // A receiver field: `this.f` or the implicit `f`.
  std::optional<Token> fieldName;
  if (const auto *get = exprAs<Get>(returned);
      get != nullptr && exprAs<This>(get->object().get()) != nullptr)
    fieldName = get->name();
  else if (const auto *variable = exprAs<Variable>(returned))
    fieldName = variable->name();
  if (!fieldName)
    return;
  const auto *declaration = classInfo(owner).declaration;
  if (declaration == nullptr)
    return;
  for (const auto &member : declaration->members()) {
    const auto *field = stmtAs<VariableDecl>(member.get());
    if (field == nullptr || field->name().lexeme != fieldName->lexeme)
      continue;
    // A local of the same name shadows the field.
    if (exprAs<Variable>(returned) != nullptr &&
        resolve(*fieldName) != nullptr &&
        resolve(*fieldName)->bindingKind != SemanticBindingKind::Field)
      return;
    std::vector<std::string> names;
    collectRegionNames(field->type(), names);
    if (!names.empty() && !contains(names, lifetime.region))
      report(*fieldName,
             "Field '" + fieldName->lexeme + "' holds region '" +
                 names.front() + "', but the result declares region '" +
                 lifetime.region + "'.",
             "GTI-S2127");
    return;
  }
}

auto SemanticVisitor::Impl::validateReturnedReceiverRegion(
    const FunctionDecl &syntax, ClassId owner, const ExprPtr &value) -> void {
  if (!value || owner == 0)
    return;
  const auto lifetime = explicitLifetimeResult(syntax, {}, owner);
  if (!lifetime.classBinder || !lifetime.classStoresRegion)
    return;
  const ExpressionInfo *info = semanticModel.findExpression(*value);
  if (info == nullptr)
    return;
  const auto fieldRecord = [&](SymbolId id) -> const SymbolRecord * {
    const SymbolRecord *field = semanticModel.database().findSymbol(id);
    if (field == nullptr && instanceBaseModel != nullptr)
      field = instanceBaseModel->database().findSymbol(id);
    return field;
  };
  // Only the receiver's references stored in the result's region supply it.
  // A place borrows what the first stored reference it crosses keeps alive,
  // in that reference's region as the receiver sees it: read through each
  // carrier field's lifetime arguments and each base class's, so a
  // `Pair<a, b>` field or a base's field supplies the region it binds there.
  // A region this walk cannot name (an elided slot, `_`, or a binder it does
  // not see) is unknown: the general place proof stands for it.
  using Regions =
      std::vector<std::pair<std::string, std::optional<std::string>>>;
  const auto mappedRegion =
      [](const Regions &map,
         const std::string &name) -> std::optional<std::string> {
    for (const auto &[from, to] : map)
      if (from == name)
        return to;
    return std::nullopt;
  };
  const auto bindRegions = [&](const ClassDecl &declaration,
                               const std::vector<TypeRef> &arguments,
                               const Regions &outer) {
    Regions bound;
    const auto binders = declaration.lifetimeParameters();
    for (std::size_t index = 0; index < binders.size(); ++index) {
      std::optional<std::string> region;
      if (index < arguments.size() &&
          arguments[index].name.segments.size() == 1 &&
          arguments[index].name.last().lexeme != "_")
        region = mappedRegion(outer, arguments[index].name.last().lexeme);
      bound.emplace_back(binders[index].name.lexeme, std::move(region));
    }
    return bound;
  };
  const auto knownClass = [&](ClassId id) {
    return id != 0 && id <= classes.size() &&
           classInfo(id).declaration != nullptr;
  };
  // The declaration of a field of a class or of one of its bases, with
  // `map` rebound to the class that declares it.
  std::function<const VariableDecl *(ClassId, const std::string &, Regions &)>
      declaredField;
  declaredField = [&](ClassId id, const std::string &name,
                      Regions &map) -> const VariableDecl * {
    if (!knownClass(id))
      return nullptr;
    const ClassInfo &info = classInfo(id);
    for (const auto &member : info.declaration->members())
      if (const auto *field = stmtAs<VariableDecl>(member.get());
          field != nullptr && field->name().lexeme == name)
        return field;
    for (const auto &base : info.bases) {
      if (base.syntax == nullptr || base.type.kind != SemanticType::Class ||
          !knownClass(base.type.classId))
        continue;
      Regions baseMap = bindRegions(*classInfo(base.type.classId).declaration,
                                    base.syntax->type.lifetimeArguments, map);
      if (const auto *field = declaredField(base.type.classId, name, baseMap)) {
        map = std::move(baseMap);
        return field;
      }
    }
    return nullptr;
  };
  struct Crossing {
    std::string field;
    std::optional<std::string> region;
    Token declared;
  };
  // Walks a receiver place to the first stored reference it crosses, or,
  // when it crosses none, to the class of the object it names with that
  // class's regions as the receiver binds them.
  struct Walked {
    bool resolved = false;
    std::optional<Crossing> crossing;
    ClassId object = 0;
    Regions regions;
  };
  const auto walk = [&](const SemanticLoanPlace &place) -> Walked {
    if (!place.receiver || place.root != 0 || !knownClass(owner))
      return {};
    ClassId current = owner;
    Regions map;
    for (const auto &binder :
         classInfo(owner).declaration->lifetimeParameters())
      map.emplace_back(binder.name.lexeme, binder.name.lexeme);
    // Regions a callable pattern binds name themselves too.
    std::vector<std::string> patternRegions;
    for (const auto &pattern :
         classInfo(owner).declaration->genericArgumentPatterns())
      collectRegionNames(pattern, patternRegions);
    for (const auto &region : patternRegions)
      map.emplace_back(region, region);
    for (const auto &projection : place.projections) {
      if (projection.kind == PlaceProjectionKind::ConstantIndex ||
          projection.kind == PlaceProjectionKind::DynamicIndex)
        continue;
      if (projection.kind != PlaceProjectionKind::Field)
        return {};
      const SymbolRecord *record = fieldRecord(projection.field);
      if (record == nullptr)
        return {};
      const VariableDecl *field = declaredField(current, record->name, map);
      if (field == nullptr)
        return {};
      const TypeRef &type = field->type();
      if (type.reference) {
        std::optional<std::string> region;
        if (type.referenceLifetime &&
            type.referenceLifetime->name.kind == TokenKind::IDENTIFIER)
          region = mappedRegion(map, type.referenceLifetime->name.lexeme);
        return {.resolved = true,
                .crossing =
                    Crossing{record->name, std::move(region), field->name()}};
      }
      if (record->type.kind != SemanticType::Class ||
          !knownClass(record->type.classId))
        return {};
      map = bindRegions(*classInfo(record->type.classId).declaration,
                        type.lifetimeArguments, map);
      current = record->type.classId;
    }
    return {.resolved = true, .object = current, .regions = std::move(map)};
  };
  // What an owned carrier of the receiver stores in one of its own regions,
  // relayed by a call: the region that carrier's field binds there.
  std::vector<Crossing> relayed;
  // The region `binder` of the object a receiver place names, when every
  // place is such an owned object of class `object`.
  const auto storedRegionOf = [&](const std::vector<SemanticLoanPlace> &objects,
                                  ClassId object, const std::string &binder,
                                  const Token &site) -> bool {
    if (objects.empty() || object == 0)
      return false;
    std::vector<Crossing> found;
    for (const auto &place : objects) {
      Walked walked = walk(place);
      if (!walked.resolved || walked.crossing || walked.object != object ||
          place.projections.empty())
        return false;
      const auto region = mappedRegion(walked.regions, binder);
      if (!region)
        return false;
      const SymbolRecord *field = fieldRecord(place.projections.front().field);
      found.push_back(
          {field == nullptr ? std::string("this") : field->name, region, site});
    }
    relayed.insert(relayed.end(), found.begin(), found.end());
    return true;
  };
  // The places a returned reference can borrow, followed through the shapes
  // that only relay a supplier: a field of a reference call result names a
  // part of that call's suppliers, a method's receiver supplies its result,
  // and the arguments bound to reference parameters supply a result written
  // in their region. Nullopt leaves a shape to the general place query.
  std::function<std::optional<std::vector<SemanticLoanPlace>>(const ExprPtr &)>
      suppliers;
  suppliers = [&](const ExprPtr &expression)
      -> std::optional<std::vector<SemanticLoanPlace>> {
    const ExprPtr *current = &expression;
    while (*current)
      if (const auto *group = exprAs<Grouping>(current->get()))
        current = &group->expression();
      else
        break;
    const Expr *node = current->get();
    if (node == nullptr)
      return std::nullopt;
    if (semanticPlace(node).root != nullptr)
      return loanPlaces(*current);
    if (const auto *member = exprAs<Get>(node)) {
      const MemberInfo *field =
          findMember(memberAccessObjectType(*member), member->name());
      if (field == nullptr || field->symbol.type == SemanticType::Function ||
          field->symbol.variableDeclaration == nullptr)
        return std::nullopt;
      auto places = suppliers(member->object());
      if (!places)
        return std::nullopt;
      for (auto &place : *places) {
        if (member->access().kind == TokenKind::ARROW)
          place.projections.push_back(
              {.kind = PlaceProjectionKind::Dereference});
        place.projections.push_back({.kind = PlaceProjectionKind::Field,
                                     .field = toolingSymbolFor(field->symbol)});
      }
      return places;
    }
    const auto *call = exprAs<Call>(node);
    const ResolvedCallInfo *resolved =
        call == nullptr ? nullptr : semanticModel.findCall(*call);
    if (resolved == nullptr ||
        resolved->returnType.kind != SemanticType::Reference)
      return std::nullopt;
    if (resolved->borrowOrigin == BorrowOriginKind::Receiver) {
      const FunctionInfo *callee =
          resolved->function == 0
              ? nullptr
              : semanticModel.findFunction(resolved->function);
      // Argument suppliers beside the receiver stay with the general query.
      if (callee == nullptr || callee->ownerClass == 0 ||
          callee->staticMember || callee->declaration == nullptr ||
          !resolved->borrowArguments.empty())
        return std::nullopt;
      // A result in the callee's own stored region is supplied by references
      // that receiver stores, so a call on this receiver in the same region
      // returns no storage the receiver owns.
      const auto *member = exprAs<Get>(call->callee().get());
      const bool thisReceiver =
          member == nullptr || exprAs<This>(member->object().get()) != nullptr;
      const auto calleeLifetime =
          explicitLifetimeResult(*callee->declaration, {}, callee->ownerClass);
      if (thisReceiver && callee->ownerClass == owner &&
          calleeLifetime.classBinder && calleeLifetime.classStoresRegion &&
          calleeLifetime.region == lifetime.region)
        return std::vector<SemanticLoanPlace>{};
      if (member == nullptr)
        return std::vector<SemanticLoanPlace>{{.receiver = true}};
      auto objects = suppliers(member->object());
      // A result in the callee class's stored region is what the receiver
      // object stores there: for an owned carrier of this receiver, the
      // region its field binds for that class region.
      if (objects && calleeLifetime.classBinder &&
          calleeLifetime.classStoresRegion &&
          storedRegionOf(*objects, callee->ownerClass, calleeLifetime.region,
                         expressionToken(*current)))
        return std::vector<SemanticLoanPlace>{};
      return objects;
    }
    if (resolved->borrowOrigin == BorrowOriginKind::Argument) {
      const std::size_t count = resolvedCallArgumentCount(*call, resolved);
      std::vector<std::size_t> positions = resolved->borrowArguments;
      if (positions.empty())
        positions.push_back(resolved->borrowArgument);
      std::vector<SemanticLoanPlace> places;
      for (const std::size_t position : positions) {
        if (position >= count || resolved->parameterTypes.size() != count)
          return std::nullopt;
        const ExprPtr &argument =
            *resolvedCallArgument(*call, resolved, position);
        // A reference parameter borrows the argument's place itself.
        if (resolved->parameterTypes[position].kind ==
            SemanticType::Reference) {
          auto borrowed = suppliers(argument);
          if (!borrowed)
            return std::nullopt;
          for (auto &place : *borrowed)
            appendUniquePlace(places, std::move(place));
          continue;
        }
        // A by-value carrier supplies what it stores in the region its
        // parameter type binds to the result's: for an owned carrier of
        // this receiver, the region its field binds there; otherwise what
        // the carrier's stored dependencies borrow.
        const SemanticType &carrier = resolved->parameterTypes[position];
        const FunctionDecl *declaration = resolved->declaration;
        const auto result = declaration
                                ? resultRegion(declaration->returnType())
                                : std::nullopt;
        if (carrier.kind != SemanticType::Class || !result ||
            declaration->parameters().size() != count ||
            std::any_of(
                declaration->parameters().begin(),
                declaration->parameters().end(),
                [](const Parameter &parameter) { return parameter.pack; }) ||
            !knownClass(carrier.classId))
          return std::nullopt;
        const TypeRef &written = declaration->parameters()[position].type;
        const auto binders =
            classInfo(carrier.classId).declaration->lifetimeParameters();
        std::optional<std::string> binder;
        for (std::size_t index = 0;
             index < written.lifetimeArguments.size() && index < binders.size();
             ++index)
          if (written.lifetimeArguments[index].name.segments.size() == 1 &&
              written.lifetimeArguments[index].name.last().lexeme ==
                  result->lexeme)
            binder = binders[index].name.lexeme;
        if (!binder)
          return std::nullopt;
        auto objects = suppliers(argument);
        if (objects && storedRegionOf(*objects, carrier.classId, *binder,
                                      expressionToken(argument)))
          continue;
        const auto dependencies = storedBorrowDependencies(argument);
        if (dependencies.empty())
          return std::nullopt;
        for (const auto &dependency : dependencies)
          appendUniquePlace(places, dependency.place);
      }
      return places;
    }
    return std::nullopt;
  };
  std::vector<SemanticLoanPlace> places;
  if (currentReturnType.kind == SemanticType::Reference) {
    if (auto resolved = suppliers(value))
      places = std::move(*resolved);
    else
      places = loanPlaces(value);
  } else
    for (const auto &dependency : storedBorrowDependencies(value))
      places.push_back(dependency.place);
  // A receiver place crosses a stored reference when it dereferences one or
  // projects a reference-typed field: it names that reference's referent.
  const auto crossesStoredReference = [&](const SemanticLoanPlace &place) {
    for (const auto &projection : place.projections) {
      if (projection.kind == PlaceProjectionKind::Dereference)
        return true;
      if (projection.kind != PlaceProjectionKind::Field)
        continue;
      const SymbolRecord *field = fieldRecord(projection.field);
      if (field == nullptr)
        return true; // Unknown structure: leave the proof to MIR.
      if (field->type.kind == SemanticType::Reference)
        return true;
    }
    return false;
  };
  if (std::any_of(places.begin(), places.end(),
                  [&](const SemanticLoanPlace &place) {
                    return place.receiver && place.root == 0 &&
                           !crossesStoredReference(place);
                  })) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2127", DiagnosticPhase::Semantics, expressionToken(value),
        "The result declares region '" + lifetime.region +
            "', which the receiver supplies only through the references it "
            "stores, but this return borrows storage the receiver owns.");
    diagnostic.hints.emplace_back(
        "Return a reference stored in region '" + lifetime.region +
        "', or declare a result borrowed from 'this'.");
    diagnostics.emplace_back(std::move(diagnostic));
    return;
  }
  const auto report = [&](const Crossing &crossed) {
    const std::string held = "region '" + *crossed.region + "'";
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2127", DiagnosticPhase::Semantics, expressionToken(value),
        "The result declares region '" + lifetime.region +
            "', but this return borrows through field '" + crossed.field +
            "', which holds " + held + ".");
    diagnostic.related.push_back(
        {tokenSpan(crossed.declared),
         "Field '" + crossed.field + "' holds " + held + " here."});
    diagnostic.hints.emplace_back(
        "Return a reference reached through a field stored in region '" +
        lifetime.region + "', or declare the result in the region it borrows.");
    diagnostics.emplace_back(std::move(diagnostic));
  };
  for (const auto &crossed : relayed)
    if (*crossed.region != lifetime.region) {
      report(crossed);
      return;
    }
  for (const auto &place : places) {
    const auto crossed = walk(place).crossing;
    if (!crossed || !crossed->region || *crossed->region == lifetime.region)
      continue;
    report(*crossed);
    return;
  }
}

auto SemanticVisitor::Impl::declaredOutlives(const FunctionDecl &syntax) const
    -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> relations;
  const auto add = [&](const ConceptApplication &requirement,
                       const auto &rename) {
    if (requirement.arguments.size() != 2)
      return;
    const auto longer = argumentRegion(requirement.arguments[0]);
    const auto shorter = argumentRegion(requirement.arguments[1]);
    if (longer && shorter)
      relations.emplace_back(rename(*longer), rename(*shorter));
  };
  const auto identity = [](const std::string &region) { return region; };
  const auto &clause = syntax.requiresClause();
  if (!clause)
    return relations;
  for (const auto &requirement : clause->expression.lifetimeRequirements)
    add(requirement, identity);
  const auto named = namedOutlives(*clause);
  relations.insert(relations.end(), named.begin(), named.end());
  return relations;
}

auto SemanticVisitor::Impl::namedOutlives(const RequiresClause &clause) const
    -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> relations;
  const auto add = [&](const ConceptApplication &requirement,
                       const auto &rename) {
    if (requirement.arguments.size() != 2)
      return;
    const auto longer = argumentRegion(requirement.arguments[0]);
    const auto shorter = argumentRegion(requirement.arguments[1]);
    if (longer && shorter)
      relations.emplace_back(rename(*longer), rename(*shorter));
  };
  for (const auto &application : clause.expression.applications) {
    auto id = resolveConceptPath(application.name, currentNamespace);
    if (!id)
      id = resolveConceptPathGlobally(application.name, currentNamespace);
    const ConceptDecl *named =
        id && *id <= concepts.size() ? concepts[*id - 1].declaration : nullptr;
    if (named == nullptr)
      continue;
    const auto parameters = named->lifetimeParameters();
    std::vector<std::string> arguments;
    for (const auto &argument : application.arguments)
      if (const auto region = argumentRegion(argument))
        arguments.push_back(*region);
    const auto rename = [&](const std::string &region) -> std::string {
      for (std::size_t index = 0;
           index < parameters.size() && index < arguments.size(); ++index)
        if (parameters[index].name.lexeme == region)
          return arguments[index];
      return region;
    };
    for (const auto &requirement :
         named->constraintExpression().lifetimeRequirements)
      add(requirement, rename);
  }
  return relations;
}

auto SemanticVisitor::Impl::declaredOutlivesAlternatives(
    const std::optional<RequiresClause> &clause) const
    -> std::vector<std::vector<std::pair<std::string, std::string>>> {
  using Relations = std::vector<std::pair<std::string, std::string>>;
  std::vector<Relations> alternatives;
  if (!clause)
    return {Relations{}};
  const auto relation = [](const ConceptApplication &requirement,
                           Relations &relations) {
    if (requirement.arguments.size() != 2)
      return;
    const auto longer = argumentRegion(requirement.arguments[0]);
    const auto shorter = argumentRegion(requirement.arguments[1]);
    if (longer && shorter)
      relations.emplace_back(*longer, *shorter);
  };
  const auto &expression = clause->expression;
  if (expression.lifetimeAlternatives.empty()) {
    auto &relations = alternatives.emplace_back();
    for (const auto &requirement : expression.lifetimeRequirements)
      relation(requirement, relations);
  } else {
    for (const auto &alternative : expression.lifetimeAlternatives) {
      auto &relations = alternatives.emplace_back();
      for (const auto &requirement : alternative)
        relation(requirement, relations);
    }
  }
  // Named lifetime concepts are conjoined with every alternative, as
  // declaredOutlives reads them.
  const auto named = namedOutlives(*clause);
  for (auto &relations : alternatives)
    relations.insert(relations.end(), named.begin(), named.end());
  return alternatives;
}

auto SemanticVisitor::Impl::validateCallLifetimeRequirements(
    const FunctionDecl &callee, std::span<const ExprPtr> arguments,
    const Token &site) -> void {
  std::vector<const ExprPtr *> inputs;
  for (const auto &argument : arguments)
    inputs.push_back(&argument);
  validateCallLifetimeRequirements(declaredOutlives(callee),
                                   callee.parameters(), inputs, site);
}

auto SemanticVisitor::Impl::validateCallLifetimeRequirements(
    std::vector<std::pair<std::string, std::string>> required,
    std::span<const Parameter> parameters,
    const std::vector<const ExprPtr *> &arguments, const Token &site,
    const std::vector<std::pair<std::string, std::string>> *substitution)
    -> void {
  // A parameter written in `static` requires a static argument (checked by
  // the unsubstituted call; a substituted check adds only its own regions).
  for (const auto &parameter : parameters)
    if (substitution == nullptr && parameter.type.referenceLifetime &&
        parameter.type.referenceLifetime->name.lexeme == "static")
      required.emplace_back("<static parameter " + parameter.name.lexeme + ">",
                            "static");
  // A region the use site writes for a callee binder: each argument naming
  // that binder must outlive the written region.
  const auto substituted =
      [&](const std::string &region) -> std::optional<std::string> {
    if (substitution != nullptr)
      for (const auto &[from, to] : *substitution)
        if (from == region && to != "_")
          return to;
    return std::nullopt;
  };
  if (substitution != nullptr)
    for (std::size_t index = 0;
         index < parameters.size() && index < arguments.size(); ++index) {
      std::vector<std::string> names;
      collectRegionNames(parameters[index].type, names);
      for (const auto &name : names)
        if (const auto written = substituted(name))
          required.emplace_back("<argument " + std::to_string(index) + ">",
                                "<written " + *written + ">");
    }
  if (required.empty() || currentFunctionDeclaration == nullptr)
    return;
  const FunctionDecl &caller = *currentFunctionDeclaration;
  // The caller-side region of an argument: a parameter's written region (an
  // elided one is its own anonymous region), `static` for a global, or a
  // local, which outlives no named region.
  const auto callerRegion = [&](const ExprPtr &argument) {
    return callerRegionOf(caller, argument);
  };
  std::vector<std::pair<std::string, std::string>> mapping;
  for (std::size_t index = 0;
       index < parameters.size() && index < arguments.size(); ++index) {
    if (parameters[index].pack)
      return;
    std::vector<std::string> names;
    collectRegionNames(parameters[index].type, names);
    // A by-value argument that carries no borrowed state borrows nothing:
    // its regions are unconstrained (a phantom region may be `static`).
    const auto *info = *arguments[index]
                           ? semanticModel.findExpression(**arguments[index])
                           : nullptr;
    const bool free = !parameters[index].type.reference && info != nullptr &&
                      !typeTraits(info->type).containsBorrowedState;
    for (const auto &name : names)
      if (name != "static")
        mapping.emplace_back(name, free ? std::string("<free>")
                                        : callerRegion(*arguments[index]));
    mapping.emplace_back("<static parameter " + parameters[index].name.lexeme +
                             ">",
                         callerRegion(*arguments[index]));
    mapping.emplace_back("<argument " + std::to_string(index) + ">",
                         free ? std::string("<free>")
                              : callerRegion(*arguments[index]));
  }
  const auto mapped =
      [&](const std::string &region) -> std::optional<std::string> {
    if (region == "static")
      return region;
    if (region.starts_with("<written "))
      return region.substr(9, region.size() - 10);
    for (const auto &[from, to] : mapping)
      if (from == region)
        return to;
    return std::nullopt;
  };
  const auto relations = declaredOutlives(caller);
  const auto holds = [&](const std::string &longer,
                         const std::string &shorter) {
    if (longer == shorter || longer == "static")
      return true;
    if (shorter == "<local>")
      return longer != "<local>";
    std::vector<std::string> reached{longer};
    for (bool grew = true; grew;) {
      grew = false;
      for (const auto &[from, to] : relations)
        if (contains(reached, from) && !contains(reached, to)) {
          reached.push_back(to);
          grew = true;
        }
    }
    return contains(reached, shorter);
  };
  for (const auto &[longer, shorter] : required) {
    const auto from = mapped(longer);
    const auto to = mapped(shorter);
    if (!from || !to || *from == "<free>" || *to == "<free>" ||
        holds(*from, *to))
      continue;
    const auto spell = [](const std::string &region) {
      return region.front() == '<' ? std::string("the argument's region")
                                   : "'" + region + "'";
    };
    report(site,
           "This call requires " + spell(*from) + " to outlive " + spell(*to) +
               ", which the caller does not establish.",
           "GTI-S2127");
    return;
  }
}

auto SemanticVisitor::Impl::validateWrittenPatternRegions(const TypeRef &use,
                                                          const Token &site)
    -> void {
  for (const auto &argument : use.arguments)
    validateWrittenPatternRegions(argument, site);
  if (use.lifetimeArguments.empty() || use.arguments.empty())
    return;
  auto id = resolveClassPath(use.name, currentNamespace);
  if (!id)
    id = resolveClassPathGlobally(use.name, currentNamespace);
  if (!id || *id > classes.size() || classes[*id - 1].declaration == nullptr)
    return;
  const ClassDecl &owner = *classes[*id - 1].declaration;
  const auto &patterns = owner.genericArgumentPatterns();
  if (patterns.size() != use.arguments.size())
    return;
  const auto binders = owner.lifetimeParameters();
  std::vector<std::string> typeBinders;
  for (const auto &parameter : owner.genericParameters())
    if (!parameter.lifetimeKeyword)
      typeBinders.push_back(parameter.name.lexeme);
  // The region the use supplies for a pattern region: a class binder takes
  // its written lifetime argument; `static` is itself.
  const auto expected =
      [&](const std::string &region) -> std::optional<std::string> {
    if (region == "static")
      return region;
    for (std::size_t index = 0;
         index < binders.size() && index < use.lifetimeArguments.size();
         ++index)
      if (binders[index].name.lexeme == region) {
        const auto &written = use.lifetimeArguments[index];
        if (written.name.segments.size() != 1 ||
            written.name.last().lexeme == "_")
          return std::nullopt;
        return written.name.last().lexeme;
      }
    return std::nullopt;
  };
  bool reported = false;
  const auto match = [&](const auto &self, const TypeRef &pattern,
                         const TypeRef &actual, bool inSignature) -> void {
    if (reported || pattern.packExpansion)
      return;
    if (pattern.referenceLifetime && actual.reference) {
      const auto want = expected(pattern.referenceLifetime->name.lexeme);
      // An elided reference inside a function type binds per invocation; it
      // cannot be the fixed region the pattern requires.
      const std::optional<std::string> have =
          actual.referenceLifetime
              ? std::optional<std::string>(
                    actual.referenceLifetime->name.lexeme)
              : (inSignature ? std::optional<std::string>("<invocation>")
                             : std::nullopt);
      if (want && have && *have != "_" && *have != *want) {
        report(site,
               "Type argument region " +
                   (*have == "<invocation>"
                        ? std::string("bound per invocation")
                        : "'" + *have + "'") +
                   " does not match the region '" + *want +
                   "' fixed by the patterned primary '" + owner.name().lexeme +
                   "'.",
               "GTI-S2127");
        reported = true;
        return;
      }
    }
    if (pattern.name.segments.size() == 1 && pattern.arguments.empty() &&
        contains(typeBinders, pattern.name.last().lexeme))
      return;
    const bool signature = pattern.isFunctionSignature();
    if (signature != actual.isFunctionSignature())
      return;
    const auto &left = pattern.arguments;
    const auto &right = actual.arguments;
    if (signature) {
      if (!left.empty() && !right.empty())
        self(self, left.back(), right.back(), true);
      const bool pack =
          std::any_of(left.begin(), left.end(), [](const auto &p) {
            return p.packExpansion.has_value();
          });
      if (pack || left.size() != right.size())
        return;
      for (std::size_t index = 0; index + 1 < left.size(); ++index)
        self(self, left[index], right[index], true);
      return;
    }
    if (left.size() != right.size())
      return;
    for (std::size_t index = 0; index < left.size(); ++index)
      self(self, left[index], right[index], inSignature);
  };
  for (std::size_t index = 0; index < patterns.size(); ++index)
    match(match, patterns[index], use.arguments[index], false);
}

auto SemanticVisitor::Impl::validateWrittenConstructionRegions(
    const VariableDecl &declaration) -> void {
  validateWrittenPatternRegions(declaration.type(), declaration.name());
  // An alias keeps the regions its target writes; only `static` means the
  // same thing at the use, so a bound named in the alias's scope is skipped.
  const TypeRef *written = &declaration.type();
  std::optional<TypeRef> aliased;
  if (written->lifetimeArguments.empty())
    if (const auto alias = resolveTypeAliasPath(*written, currentNamespace);
        alias && alias->declaration != nullptr) {
      aliased = alias->declaration->target();
      for (auto &argument : aliased->lifetimeArguments)
        if (argument.name.segments.size() != 1 ||
            argument.name.last().lexeme != "static")
          argument.name.segments.back().lexeme = "_";
      written = &*aliased;
    }
  const TypeRef &type = *written;
  const auto &initializer = declaration.initializer();
  if (!initializer ||
      (type.lifetimeArguments.empty() && type.arguments.empty()))
    return;
  const auto *direct = exprAs<DirectInitializer>(initializer.get());
  if (direct == nullptr || !direct->designators().empty())
    return;
  const auto *construction = semanticModel.findConstruction(*initializer);
  if (construction == nullptr || construction->declaration == nullptr)
    return;
  auto id = resolveClassPath(type.name, currentNamespace);
  if (!id)
    id = resolveClassPathGlobally(type.name, currentNamespace);
  if (!id || *id > classes.size() || classes[*id - 1].declaration == nullptr)
    return;
  const ClassDecl &owner = *classes[*id - 1].declaration;
  const auto binders = owner.lifetimeParameters();
  std::vector<std::pair<std::string, std::string>> substitution;
  for (std::size_t index = 0;
       index < binders.size() && index < type.lifetimeArguments.size(); ++index)
    if (type.lifetimeArguments[index].name.segments.size() == 1)
      substitution.emplace_back(
          binders[index].name.lexeme,
          type.lifetimeArguments[index].name.last().lexeme);
  // A patterned primary binds a region where its pattern writes one:
  // `Default<int&<source>>` used as `Default<int&<static>>` fixes source.
  const auto bind = [&](const auto &self, const TypeRef &pattern,
                        const TypeRef &actual) -> void {
    if (pattern.referenceLifetime && actual.referenceLifetime &&
        pattern.referenceLifetime->name.lexeme != "static")
      substitution.emplace_back(pattern.referenceLifetime->name.lexeme,
                                actual.referenceLifetime->name.lexeme);
    if (pattern.arguments.size() == actual.arguments.size() &&
        pattern.isFunctionSignature() == actual.isFunctionSignature())
      for (std::size_t index = 0; index < pattern.arguments.size(); ++index)
        if (!pattern.arguments[index].packExpansion)
          self(self, pattern.arguments[index], actual.arguments[index]);
  };
  const auto &patterns = owner.genericArgumentPatterns();
  if (patterns.size() == type.arguments.size())
    for (std::size_t index = 0; index < patterns.size(); ++index)
      bind(bind, patterns[index], type.arguments[index]);
  if (substitution.empty())
    return;
  std::vector<const ExprPtr *> inputs;
  for (const auto &argument : direct->arguments())
    inputs.push_back(&argument);
  validateCallLifetimeRequirements({}, construction->declaration->parameters(),
                                   inputs, direct->brace(), &substitution);
}

auto SemanticVisitor::Impl::validateReceiverCallRegions(
    const Expr &receiver, const FunctionDecl &callee,
    std::span<const ExprPtr> arguments, const Token &site) -> void {
  if (currentFunctionDeclaration == nullptr)
    return;
  const Expr *expression = &receiver;
  while (const auto *group = exprAs<Grouping>(expression))
    expression = group->expression().get();
  const auto *variable = exprAs<Variable>(expression);
  const Symbol *symbol = variable ? resolve(variable->name()) : nullptr;
  if (symbol == nullptr)
    return;
  const TypeRef *written = nullptr;
  if (symbol->bindingKind == SemanticBindingKind::Parameter) {
    for (const auto &parameter : currentFunctionDeclaration->parameters())
      if (parameter.name.lexeme == variable->name().lexeme)
        written = &parameter.type;
  } else if (symbol->variableDeclaration != nullptr) {
    written = &symbol->variableDeclaration->type();
  }
  if (written == nullptr || written->lifetimeArguments.empty())
    return;
  auto id = resolveClassPath(written->name, currentNamespace);
  if (!id)
    id = resolveClassPathGlobally(written->name, currentNamespace);
  if (!id || *id > classes.size() || classes[*id - 1].declaration == nullptr)
    return;
  const auto binders = classes[*id - 1].declaration->lifetimeParameters();
  std::vector<std::pair<std::string, std::string>> substitution;
  for (std::size_t index = 0;
       index < binders.size() && index < written->lifetimeArguments.size();
       ++index)
    if (written->lifetimeArguments[index].name.segments.size() == 1)
      substitution.emplace_back(
          binders[index].name.lexeme,
          written->lifetimeArguments[index].name.last().lexeme);
  std::vector<const ExprPtr *> inputs;
  for (const auto &argument : arguments)
    inputs.push_back(&argument);
  validateCallLifetimeRequirements({}, callee.parameters(), inputs, site,
                                   &substitution);
}

auto SemanticVisitor::Impl::validateConstructorFieldRegions(
    const ConstructorDecl &constructor, const ClassDecl &owner) -> void {
  std::vector<std::string> binders;
  for (const auto &parameter : owner.lifetimeParameters())
    binders.push_back(parameter.name.lexeme);
  if (binders.empty())
    return; // The implicit region is fed by every borrow input.
  for (const auto &initializer : constructor.initializers()) {
    if (initializer.target.name.segments.size() != 1 ||
        initializer.arguments.size() != 1)
      continue;
    const VariableDecl *field = nullptr;
    for (const auto &member : owner.members())
      if (const auto *candidate = stmtAs<VariableDecl>(member.get());
          candidate != nullptr &&
          candidate->name().lexeme == initializer.target.name.last().lexeme)
        field = candidate;
    if (field == nullptr || !field->type().referenceLifetime)
      continue;
    const std::string &region = field->type().referenceLifetime->name.lexeme;
    if (!contains(binders, region))
      continue;
    const Expr *argument = initializer.arguments.front().get();
    while (const auto *group = exprAs<Grouping>(argument))
      argument = group->expression().get();
    const auto *variable = exprAs<Variable>(argument);
    if (variable == nullptr)
      continue;
    for (const auto &parameter : constructor.parameters()) {
      if (parameter.name.lexeme != variable->name().lexeme ||
          !parameter.type.reference)
        continue;
      const auto written = parameter.type.referenceLifetime
                               ? parameter.type.referenceLifetime->name.lexeme
                               : std::string();
      if (written == region || written == "static")
        break;
      // A constructor region declared to outlive the field's region supplies
      // it too (`requires outlives<input, a>`).
      bool declaredLonger = false;
      if (const auto &clause = constructor.requiresClause())
        for (const auto &requirement : clause->expression.lifetimeRequirements)
          if (requirement.arguments.size() == 2) {
            const auto longer = argumentRegion(requirement.arguments[0]);
            const auto shorter = argumentRegion(requirement.arguments[1]);
            declaredLonger |=
                longer && shorter && *longer == written && *shorter == region;
          }
      if (declaredLonger)
        break;
      report(variable->name(),
             "Field '" + field->name().lexeme + "' stores region '" + region +
                 "', but parameter '" + parameter.name.lexeme + "' " +
                 (written.empty() ? std::string("elides its region")
                                  : "is written in region '" + written + "'") +
                 "; write `" + parameter.name.lexeme + "` as `&<" + region +
                 ">`.",
             "GTI-S2127");
      break;
    }
  }
}

auto SemanticVisitor::Impl::callerRegionOf(const FunctionDecl &caller,
                                           const ExprPtr &argument)
    -> std::string {
  // A moved value keeps the regions it carries.
  if (const ExprPtr *moved = explicitMoveSource(argument);
      moved != nullptr && *moved)
    return callerRegionOf(caller, *moved);
  const Expr *expression = argument.get();
  while (const auto *group = exprAs<Grouping>(expression))
    expression = group->expression().get();
  // Every lane of a forwarded pack carries its parameter's written region.
  if (const auto *pack = exprAs<PackExpansion>(expression)) {
    for (const auto &parameter : caller.parameters())
      if (parameter.pack && parameter.name.lexeme == pack->name().lexeme) {
        if (const auto region = argumentRegion(parameter.type);
            region && *region != "_" &&
            parameter.type.name.last().lexeme != *region)
          return *region;
        return "<param " + parameter.name.lexeme + ">";
      }
    return "<local>";
  }
  const auto *variable = exprAs<Variable>(expression);
  if (variable == nullptr)
    return "<local>";
  const Symbol *symbol = resolve(variable->name());
  if (symbol == nullptr)
    return "<local>";
  if (symbol->bindingKind == SemanticBindingKind::GlobalVariable &&
      (symbol->variableDeclaration == nullptr ||
       !symbol->variableDeclaration->isThreadLocal()))
    return "static";
  if (symbol->bindingKind == SemanticBindingKind::Parameter)
    for (const auto &parameter : caller.parameters())
      if (parameter.name.lexeme == variable->name().lexeme) {
        if (const auto region = argumentRegion(parameter.type);
            region && *region != "_" &&
            parameter.type.name.last().lexeme != *region)
          return *region;
        return "<param " + parameter.name.lexeme + ">";
      }
  return "<local>";
}

auto SemanticVisitor::Impl::validateReturnedCallRegion(
    const FunctionDecl &syntax, const ExprPtr &value) -> bool {
  if (!value)
    return true;
  const auto declared = resultRegion(syntax.returnType());
  if (!declared || declared->kind != TokenKind::IDENTIFIER)
    return true;
  const std::string target = declared->lexeme;
  bool functionBound = false;
  for (const auto &parameter : syntax.lifetimeParameters())
    functionBound |= parameter.name.lexeme == target;
  if (!functionBound)
    return true;
  const Expr *returned = value.get();
  while (const auto *group = exprAs<Grouping>(returned))
    returned = group->expression().get();
  const auto relations = declaredOutlives(syntax);
  // A disjunctive requires clause proves the relation only when every
  // alternative does (each one, plus the conjunctive relations).
  const auto alternatives =
      declaredOutlivesAlternatives(syntax.requiresClause());
  const auto outlivesTarget = [&](const std::string &longer) {
    if (longer == target || longer == "static")
      return true;
    const auto reaches = [&](const auto &extra) {
      std::vector<std::string> reached{longer};
      for (bool grew = true; grew;) {
        grew = false;
        for (const auto *set : {&relations, &extra})
          for (const auto &[from, to] : *set)
            if (contains(reached, from) && !contains(reached, to)) {
              reached.push_back(to);
              grew = true;
            }
      }
      return contains(reached, target);
    };
    if (alternatives.empty())
      return reaches(decltype(relations){});
    return std::all_of(alternatives.begin(), alternatives.end(), reaches);
  };
  // A returned reference parameter supplies its own written region.
  if (const auto *variable = exprAs<Variable>(returned)) {
    for (const auto &parameter : syntax.parameters())
      if (parameter.name.lexeme == variable->name().lexeme &&
          parameter.type.reference && parameter.type.referenceLifetime) {
        const std::string supplier =
            parameter.type.referenceLifetime->name.lexeme;
        if (supplier != "_" && !outlivesTarget(supplier)) {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2127", DiagnosticPhase::Semantics, variable->name(),
              "The returned parameter's region '" + supplier +
                  "' is not declared to outlive the result's region '" +
                  target + "'.");
          diagnostic.related.push_back(
              {tokenSpan(parameter.type.referenceLifetime->name),
               "Parameter '" + parameter.name.lexeme +
                   "' supplies a reference valid for region '" + supplier +
                   "'."});
          diagnostic.related.push_back(
              {tokenSpan(*declared),
               "The result requires a reference valid for region '" + target +
                   "'."});
          diagnostic.hints.emplace_back(
              "Return a value satisfying the result's lifetime, or change the "
              "signature to express the intended relationship. A requires "
              "outlives<" +
              supplier + ", " + target +
              "> constraint makes callers prove that the input lives long "
              "enough; it does not extend the input's lifetime.");
          diagnostics.emplace_back(std::move(diagnostic));
          return false;
        }
      }
    return true;
  }
  // A reference field read from a by-value carrier parameter lives in the
  // region the parameter's type supplies for that field: a written lifetime
  // argument, or a fresh elided region no declaration relates to the result.
  if (const auto *get = exprAs<Get>(returned)) {
    const auto *object = exprAs<Variable>(get->object().get());
    for (const auto &parameter : syntax.parameters()) {
      if (object == nullptr || parameter.type.reference || parameter.pack ||
          parameter.name.lexeme != object->name().lexeme)
        continue;
      const SemanticType type = typeOf(parameter);
      if (type.kind != SemanticType::Class || type.classId == 0 ||
          type.classId > classes.size())
        return true;
      const auto *declaration = classInfo(type.classId).declaration;
      if (declaration == nullptr)
        return true;
      const auto binders = declaration->lifetimeParameters();
      for (const auto &member : declaration->members()) {
        const auto *field = stmtAs<VariableDecl>(member.get());
        if (field == nullptr || field->name().lexeme != get->name().lexeme ||
            !field->type().reference)
          continue;
        std::optional<std::size_t> index;
        if (field->type().referenceLifetime) {
          for (std::size_t at = 0; at < binders.size(); ++at)
            if (binders[at].name.lexeme ==
                field->type().referenceLifetime->name.lexeme)
              index = at;
        } else if (binders.size() <= 1) {
          index = 0; // the class's one (possibly implicit) region
        }
        if (!index)
          return true;
        const auto &written = parameter.type.lifetimeArguments;
        const auto supplier = *index < written.size()
                                  ? writtenRegion(written[*index])
                                  : std::nullopt;
        if (supplier && (*supplier == "_" || outlivesTarget(*supplier)))
          return true;
        report(get->name(),
               "Field '" + get->name().lexeme + "' of parameter '" +
                   parameter.name.lexeme + "' borrows " +
                   (supplier ? "region '" + *supplier + "'"
                             : std::string("the parameter's elided region")) +
                   ", which is not declared to outlive the result's region '" +
                   target + "'.",
               "GTI-S2127");
        return false;
      }
      return true;
    }
    return true;
  }
  const auto *call = exprAs<Call>(returned);
  const auto *resolved = call ? semanticModel.findCall(*call) : nullptr;
  if (resolved == nullptr || resolved->declaration == nullptr)
    return true;
  const FunctionDecl &callee = *resolved->declaration;
  const auto calleeRegion = resultRegion(callee.returnType());
  if (!calleeRegion || calleeRegion->kind != TokenKind::IDENTIFIER)
    return true;
  const auto outlives = [&](const std::string &longer) {
    if (longer == target || longer == "static")
      return true;
    std::vector<std::string> reached{longer};
    for (bool grew = true; grew;) {
      grew = false;
      for (const auto &[from, to] : relations)
        if (contains(reached, from) && !contains(reached, to)) {
          reached.push_back(to);
          grew = true;
        }
    }
    return contains(reached, target);
  };
  const auto fieldRecord = [&](SymbolId id) -> const SymbolRecord * {
    const SymbolRecord *field = semanticModel.database().findSymbol(id);
    if (field == nullptr && instanceBaseModel != nullptr)
      field = instanceBaseModel->database().findSymbol(id);
    return field;
  };
  const auto knownClass = [&](ClassId id) {
    return id != 0 && id <= classes.size() &&
           classInfo(id).declaration != nullptr;
  };
  // Regions of a class as the caller names them; nullopt where unknown.
  using Regions =
      std::vector<std::pair<std::string, std::optional<std::string>>>;
  const auto mappedRegion =
      [](const Regions &map,
         const std::string &name) -> std::optional<std::string> {
    for (const auto &[from, to] : map)
      if (from == name)
        return to;
    return std::nullopt;
  };
  const auto bindRegions = [&](const ClassDecl &declaration,
                               const std::vector<TypeRef> &arguments,
                               const Regions &outer) {
    Regions bound;
    const auto binders = declaration.lifetimeParameters();
    for (std::size_t index = 0; index < binders.size(); ++index) {
      std::optional<std::string> region;
      if (index < arguments.size() &&
          arguments[index].name.segments.size() == 1 &&
          arguments[index].name.last().lexeme != "_")
        region = mappedRegion(outer, arguments[index].name.last().lexeme);
      bound.emplace_back(binders[index].name.lexeme, std::move(region));
    }
    return bound;
  };
  // A local reference aliases the place it was bound to, and a reference
  // binding is never rebound. It supplies the region of what it designates:
  // a reference parameter's referent lives for that reference's region, and
  // a referent reached through one stored reference of it lives for the
  // region that reference holds as the parameter's type binds it.
  const auto aliasedRegion =
      [&](const ExprPtr &argument) -> std::optional<std::string> {
    const Expr *expression = argument.get();
    while (const auto *group = exprAs<Grouping>(expression))
      expression = group->expression().get();
    const auto *variable = exprAs<Variable>(expression);
    const Symbol *symbol =
        variable == nullptr ? nullptr : resolve(variable->name());
    if (symbol == nullptr ||
        symbol->bindingKind != SemanticBindingKind::LocalVariable ||
        symbol->type.kind != SemanticType::Reference ||
        symbol->retainedDependency == nullptr)
      return std::nullopt;
    const SemanticLoanPlace &place = symbol->retainedDependency->place;
    if (place.receiver || place.root == 0)
      return std::nullopt;
    for (const auto &parameter : syntax.parameters()) {
      if (!parameter.type.reference || parameter.pack)
        continue;
      const Symbol *formal = resolve(parameter.name);
      if (formal == nullptr ||
          formal->bindingKind != SemanticBindingKind::Parameter ||
          toolingSymbolFor(*formal) != place.root)
        continue;
      // The referent lives for the reference's own region, not for a
      // region its type carries.
      std::optional<std::string> region;
      if (parameter.type.referenceLifetime &&
          parameter.type.referenceLifetime->name.kind ==
              TokenKind::IDENTIFIER &&
          parameter.type.referenceLifetime->name.lexeme != "_")
        region = parameter.type.referenceLifetime->name.lexeme;
      // Walk the referent's fields; its regions are the ones the
      // parameter's type writes.
      ClassId current =
          formal->type.kind == SemanticType::Reference &&
                  !formal->type.arguments.empty() &&
                  formal->type.arguments.front().kind == SemanticType::Class
              ? formal->type.arguments.front().classId
              : 0;
      Regions map;
      for (const auto &binder : syntax.lifetimeParameters())
        map.emplace_back(binder.name.lexeme, binder.name.lexeme);
      if (knownClass(current))
        map = bindRegions(*classInfo(current).declaration,
                          parameter.type.lifetimeArguments, map);
      // The field path from the parameter's referent: the place's own
      // projections, or, when the alias designates what a stored reference
      // of it holds (a stored dependency), the member chain it was bound to.
      struct Step {
        std::string name;
        SemanticType type;
      };
      std::vector<Step> path;
      if (symbol->retainedDependency->storedDependency == nullptr) {
        for (const auto &projection : place.projections) {
          if (projection.kind == PlaceProjectionKind::ConstantIndex ||
              projection.kind == PlaceProjectionKind::DynamicIndex)
            continue;
          if (projection.kind != PlaceProjectionKind::Field)
            return std::nullopt;
          const SymbolRecord *record = fieldRecord(projection.field);
          if (record == nullptr)
            return std::nullopt;
          path.push_back({record->name, record->type});
        }
      } else {
        if (!place.projections.empty())
          return std::nullopt;
        // The initializer the alias was bound to, rooted at this parameter.
        std::vector<const Get *> members;
        const Expr *chain =
            symbol->variableDeclaration != nullptr
                ? symbol->variableDeclaration->initializer().get()
                : nullptr;
        while (chain != nullptr) {
          if (const auto *group = exprAs<Grouping>(chain)) {
            chain = group->expression().get();
          } else if (const auto *get = exprAs<Get>(chain)) {
            if (get->access().kind == TokenKind::ARROW)
              return std::nullopt;
            members.insert(members.begin(), get);
            chain = get->object().get();
          } else {
            break;
          }
        }
        const auto *root = exprAs<Variable>(chain);
        if (root == nullptr || root->name().lexeme != parameter.name.lexeme)
          return std::nullopt;
        for (const Get *get : members) {
          const MemberInfo *member =
              findMember(memberAccessObjectType(*get), get->name());
          if (member == nullptr ||
              member->symbol.type == SemanticType::Function)
            return std::nullopt;
          path.push_back({get->name().lexeme, member->symbol.type});
        }
      }
      bool crossed = false;
      for (std::size_t step = 0; step < path.size(); ++step) {
        const std::string &name = path[step].name;
        const SemanticType &type = path[step].type;
        if (crossed || !knownClass(current))
          return std::nullopt;
        const VariableDecl *field = nullptr;
        for (const auto &member : classInfo(current).declaration->members())
          if (const auto *candidate = stmtAs<VariableDecl>(member.get());
              candidate != nullptr && candidate->name().lexeme == name)
            field = candidate;
        if (field == nullptr)
          return std::nullopt;
        if (field->type().reference) {
          // A referent reached through one stored reference lives for the
          // region that reference holds; a second crossing is not followed.
          if (!field->type().referenceLifetime)
            return std::nullopt;
          region =
              mappedRegion(map, field->type().referenceLifetime->name.lexeme);
          if (!region)
            return std::nullopt;
          crossed = true;
          continue;
        }
        if (type.kind != SemanticType::Class) {
          // A scalar or array part of the referent keeps its region.
          if (step + 1 != path.size())
            return std::nullopt;
          continue;
        }
        if (!knownClass(type.classId))
          return std::nullopt;
        map = bindRegions(*classInfo(type.classId).declaration,
                          field->type().lifetimeArguments, map);
        current = type.classId;
      }
      if (region)
        return *region;
      return "<param " + parameter.name.lexeme + ">";
    }
    return std::nullopt;
  };
  // The first supplier of a returned call's region that does not outlive the
  // result's. An argument that is itself such a call supplies through its
  // own suppliers, so a nested relay checks each of them.
  std::function<std::optional<std::string>(const Call &, const FunctionDecl &,
                                           const Token &)>
      failingSupplier;
  failingSupplier = [&](const Call &relay, const FunctionDecl &relayed,
                        const Token &region) -> std::optional<std::string> {
    const auto &parameters = relayed.parameters();
    const auto &arguments = relay.arguments();
    // A final forwarded pack supplies every remaining fixed parameter.
    const bool forwardedPack =
        !arguments.empty() &&
        exprAs<PackExpansion>(arguments.back().get()) != nullptr;
    for (std::size_t index = 0; index < parameters.size() &&
                                (index < arguments.size() || forwardedPack);
         ++index) {
      if (parameters[index].pack)
        return std::nullopt;
      std::vector<std::string> names;
      collectRegionNames(parameters[index].type, names);
      if (!contains(names, region.lexeme))
        continue;
      const ExprPtr &argument =
          arguments[std::min(index, arguments.size() - 1)];
      std::string supplier = callerRegionOf(syntax, argument);
      if (supplier == "<local>")
        if (auto aliased = aliasedRegion(argument))
          supplier = std::move(*aliased);
      if (supplier == "<local>" && parameters[index].type.reference) {
        const Expr *nested = argument.get();
        while (const auto *group = exprAs<Grouping>(nested))
          nested = group->expression().get();
        const auto *inner = exprAs<Call>(nested);
        const auto *innerResolved =
            inner ? semanticModel.findCall(*inner) : nullptr;
        const FunctionDecl *innerCallee =
            innerResolved ? innerResolved->declaration : nullptr;
        const auto innerRegion = innerCallee
                                     ? resultRegion(innerCallee->returnType())
                                     : std::nullopt;
        bool innerBound = false;
        if (innerRegion && innerRegion->kind == TokenKind::IDENTIFIER &&
            innerResolved->borrowOrigin == BorrowOriginKind::Argument)
          for (const auto &parameter : innerCallee->lifetimeParameters())
            innerBound |= parameter.name.lexeme == innerRegion->lexeme;
        if (innerBound) {
          if (auto failed = failingSupplier(*inner, *innerCallee, *innerRegion))
            return failed;
          continue;
        }
      }
      if (!outlives(supplier))
        return supplier;
    }
    return std::nullopt;
  };
  if (const auto supplier = failingSupplier(*call, callee, *calleeRegion)) {
    report(call->paren(),
           "The returned call's region is supplied by " +
               (supplier->front() == '<' ? std::string("an argument's region")
                                         : "'" + *supplier + "'") +
               ", which is not declared to outlive the result's region '" +
               target + "'.",
           "GTI-S2127");
    return false;
  }
  return true;
}

auto SemanticVisitor::Impl::validateCallTypeOutlives(const Call &call,
                                                     const FunctionDecl &callee)
    -> void {
  if (currentFunctionDeclaration == nullptr)
    return;
  const FunctionDecl &caller = *currentFunctionDeclaration;
  std::vector<std::string> calleeTypes;
  for (const auto &parameter : callee.genericParameters())
    if (!parameter.lifetimeKeyword)
      calleeTypes.push_back(parameter.name.lexeme);
  std::vector<std::string> callerTypes;
  for (const auto &parameter : caller.genericParameters())
    if (!parameter.lifetimeKeyword)
      callerTypes.push_back(parameter.name.lexeme);
  // What the call binds to a callee type parameter: an explicit type
  // argument, or the written type of an argument whose parameter is T.
  const auto binding = [&](const std::string &name) -> std::optional<TypeRef> {
    for (std::size_t index = 0;
         index < calleeTypes.size() && index < call.typeArguments().size();
         ++index)
      if (calleeTypes[index] == name)
        return call.typeArguments()[index];
    const auto &parameters = callee.parameters();
    for (std::size_t index = 0;
         index < parameters.size() && index < call.arguments().size();
         ++index) {
      if (parameters[index].pack ||
          parameters[index].type.name.segments.size() != 1 ||
          parameters[index].type.name.last().lexeme != name)
        continue;
      const Expr *argument = call.arguments()[index].get();
      while (const auto *group = exprAs<Grouping>(argument))
        argument = group->expression().get();
      const auto *variable = exprAs<Variable>(argument);
      if (variable == nullptr)
        continue;
      for (const auto &own : caller.parameters())
        if (own.name.lexeme == variable->name().lexeme &&
            own.type.name.segments.size() == 1 &&
            contains(callerTypes, own.type.name.last().lexeme)) {
          TypeRef bound = own.type;
          bound.reference.reset();
          return bound;
        }
    }
    return std::nullopt;
  };
  // Declared relations, plus implied bounds: a parameter written `X&<r>`
  // implies that every caller type parameter X stores outlives r. Storage is
  // structural: fields, bases and enum payloads after substitution. A raw
  // pointee and a phantom argument are not stored, so they gain nothing.
  auto relations = declaredOutlives(caller);
  const auto stored = [&](const auto &self, const TypeRef &type,
                          std::vector<std::string> &out,
                          std::size_t depth) -> void {
    if (depth > 8 || !type.pointers.empty())
      return;
    if (type.name.segments.size() == 1 && type.arguments.empty() &&
        contains(callerTypes, type.name.last().lexeme)) {
      if (!contains(out, type.name.last().lexeme))
        out.push_back(type.name.last().lexeme);
      return;
    }
    if (type.arguments.empty())
      return;
    std::vector<std::string> formals;
    std::vector<const TypeRef *> members;
    std::vector<TypeRef> payloads;
    const auto formalsOf = [&](const std::vector<GenericParameter> &params) {
      for (const auto &parameter : params)
        if (!parameter.lifetimeKeyword)
          formals.push_back(parameter.name.lexeme);
    };
    auto classId = resolveClassPath(type.name, currentNamespace);
    if (!classId)
      classId = resolveClassPathGlobally(type.name, currentNamespace);
    auto enumId =
        classId ? std::nullopt : resolveEnumPath(type.name, currentNamespace);
    if (!classId && !enumId)
      enumId = resolveEnumPathGlobally(type.name, currentNamespace);
    if (classId && *classId <= classes.size() &&
        classes[*classId - 1].declaration != nullptr) {
      const ClassDecl &decl = *classes[*classId - 1].declaration;
      formalsOf(decl.genericParameters());
      for (const auto &base : decl.bases())
        members.push_back(&base.type);
      for (const auto &member : decl.members())
        if (const auto *field = stmtAs<VariableDecl>(member.get());
            field != nullptr && !field->isStatic())
          members.push_back(&field->type());
    } else if (enumId && *enumId <= enums.size() &&
               enums[*enumId - 1].declaration != nullptr) {
      const EnumDecl &decl = *enums[*enumId - 1].declaration;
      formalsOf(decl.genericParameters());
      for (const auto &enumerator : decl.enumerators())
        for (const auto &field : enumerator.payload)
          members.push_back(&field.type);
    } else {
      // An unresolved container: conservatively, every argument is stored.
      for (const auto &argument : type.arguments)
        self(self, argument, out, depth + 1);
      return;
    }
    // Substitute the container's formals by this use's arguments.
    const auto substitute = [&](const auto &again,
                                const TypeRef &member) -> TypeRef {
      if (member.name.segments.size() == 1 && member.arguments.empty())
        for (std::size_t index = 0;
             index < formals.size() && index < type.arguments.size(); ++index)
          if (formals[index] == member.name.last().lexeme) {
            TypeRef actual = type.arguments[index];
            actual.pointers.insert(actual.pointers.end(),
                                   member.pointers.begin(),
                                   member.pointers.end());
            if (member.reference)
              actual.reference = member.reference;
            return actual;
          }
      TypeRef copy = member;
      for (auto &argument : copy.arguments)
        argument = again(again, argument);
      return copy;
    };
    for (const TypeRef *member : members)
      self(self, substitute(substitute, *member), out, depth + 1);
  };
  for (const auto &parameter : caller.parameters())
    if (parameter.type.referenceLifetime) {
      TypeRef referent = parameter.type;
      referent.reference.reset();
      referent.referenceLifetime.reset();
      std::vector<std::string> reached;
      stored(stored, referent, reached, 0);
      for (const auto &name : reached)
        relations.emplace_back(name,
                               parameter.type.referenceLifetime->name.lexeme);
    }
  const auto holds = [&](const std::string &longer,
                         const std::string &shorter) {
    if (longer == shorter)
      return true;
    std::vector<std::string> reached{longer};
    for (bool grew = true; grew;) {
      grew = false;
      for (const auto &[from, to] : relations)
        if (contains(reached, from) &&
            (!contains(reached, to) || to == "static")) {
          if (to == shorter || to == "static")
            return true;
          reached.push_back(to);
          grew = true;
        }
    }
    return false;
  };
  for (const auto &[longer, shorter] : declaredOutlives(callee)) {
    if (!contains(calleeTypes, longer) || shorter != "static")
      continue;
    const auto bound = binding(longer);
    if (!bound || bound->name.segments.size() != 1 || bound->reference)
      continue;
    const std::string &name = bound->name.last().lexeme;
    if (!contains(callerTypes, name))
      continue; // A concrete type: its own regions decide (not checked here).
    if (holds(name, shorter))
      continue;
    report(call.paren(),
           "This call requires type '" + name + "' to outlive '" + shorter +
               "', which the caller does not establish.",
           "GTI-S2127");
    return;
  }
}

auto SemanticVisitor::Impl::hasExplicitBorrowContract(
    const FunctionDecl &syntax) -> bool {
  // `static` keeps the inferred static-storage origin; the declared region is
  // checked against it, not substituted for it.
  // Only `this` and a region the function itself binds state a contract:
  // class-bound (stored) regions are what the existing inference derives
  // from the receiver's fields.
  const auto region = resultRegion(syntax.returnType());
  if (!region)
    return false;
  // `this` states the receiver contract.
  if (region->kind == TokenKind::THIS || region->lexeme == "this")
    return true;
  const auto parameters = syntax.lifetimeParameters();
  if (std::any_of(parameters.begin(), parameters.end(),
                  [&](const GenericParameter &parameter) {
                    return parameter.name.lexeme == region->lexeme;
                  }))
    return true;
  // A class-bound region that a parameter is also written in names that
  // parameter as a supplier: the signature states the relationship.
  if (region->kind == TokenKind::STATIC || region->lexeme == "static" ||
      region->lexeme == "_")
    return false;
  return std::any_of(syntax.parameters().begin(), syntax.parameters().end(),
                     [&](const Parameter &parameter) {
                       std::vector<std::string> names;
                       collectRegionNames(parameter.type, names);
                       return contains(names, region->lexeme);
                     });
}

// The one parameter a declaration's explicit result contract names: the
// parameter its result region binds. Empty for an inferred, receiver or
// multi-supplier one.
auto SemanticVisitor::Impl::declaredContractParameter(
    const FunctionDecl &syntax, const SemanticType &returnType,
    std::span<const SemanticType> parameterTypes) const
    -> std::optional<std::size_t> {
  if (!hasExplicitBorrowContract(syntax))
    return std::nullopt;
  (void)returnType;
  const auto lifetime = explicitLifetimeResult(syntax, parameterTypes, 0);
  if (lifetime.kind != ExplicitLifetimeResult::Kind::Region ||
      lifetime.parameters.size() != 1)
    return std::nullopt;
  return lifetime.parameters.front();
}

// RFC 0004 elision, as Rust: a free function result written as a bare type
// parameter (`fn f<I>(i: I, v: &T) -> I`) carries exactly the regions of that
// parameter's argument, so only inputs whose written types mention it supply
// the result.
auto SemanticVisitor::Impl::typeParameterResultSuppliers(
    const FunctionInfo &function) -> std::optional<std::vector<bool>> {
  if (function.explicitReturnBorrowOrigin ||
      (function.ownerClass != 0 && !function.staticMember))
    return std::nullopt;
  const FunctionDecl *syntax = function.bodyDeclaration();
  if (syntax == nullptr ||
      syntax->parameters().size() != function.parameterTypes.size() ||
      std::any_of(syntax->parameters().begin(), syntax->parameters().end(),
                  [](const Parameter &candidate) { return candidate.pack; }))
    return std::nullopt;
  const TypeRef &written = syntax->returnType();
  if (written.reference || !written.pointers.empty() ||
      !written.arguments.empty() || !written.lifetimeArguments.empty() ||
      written.qualifiedOwner || written.name.segments.size() != 1)
    return std::nullopt;
  const std::string &name = written.name.last().lexeme;
  if (std::none_of(syntax->genericParameters().begin(),
                   syntax->genericParameters().end(),
                   [&](const GenericParameter &parameter) {
                     return !parameter.pack && !parameter.valueType &&
                            parameter.name.lexeme == name;
                   }))
    return std::nullopt;
  const std::function<bool(const TypeRef &)> mentions =
      [&](const TypeRef &type) -> bool {
    if (type.name.segments.size() == 1 && type.name.last().lexeme == name)
      return true;
    if (type.qualifiedOwner && mentions(*type.qualifiedOwner))
      return true;
    return std::any_of(type.arguments.begin(), type.arguments.end(), mentions);
  };
  std::vector<bool> suppliers;
  for (const Parameter &parameter : syntax->parameters())
    suppliers.push_back(mentions(parameter.type));
  return suppliers;
}

auto SemanticVisitor::Impl::narrowToTypeParameterSuppliers(
    FunctionInfo &function) -> void {
  if (function.returnBorrowOrigin != BorrowOriginKind::Argument)
    return;
  const auto suppliers = typeParameterResultSuppliers(function);
  if (!suppliers)
    return;
  std::vector<std::size_t> kept;
  const auto candidates =
      function.returnBorrowParameters.empty()
          ? std::vector<std::size_t>{function.returnBorrowParameter}
          : function.returnBorrowParameters;
  for (const std::size_t index : candidates)
    if (index < suppliers->size() && (*suppliers)[index])
      kept.push_back(index);
  function.returnBorrowParameters.clear();
  if (kept.empty()) {
    function.returnBorrowOrigin = BorrowOriginKind::None;
    function.returnBorrowParameter = 0;
    return;
  }
  function.returnBorrowParameter = kept.front();
  if (kept.size() > 1)
    function.returnBorrowParameters = std::move(kept);
}

// RFC 0004: a free or static function whose carrier result no input can
// supply (no reference parameter and no parameter holding a borrow) has an
// implicitly `static` result. It names no exact place; MIR proves that every
// stored dependency of every returned value is rooted in immutable static
// storage. A mutable stored path is never static.
auto SemanticVisitor::Impl::implicitStaticCarrierResult(
    const SemanticType &returnType,
    const std::vector<SemanticType> &parameterTypes, ClassId ownerClass,
    bool staticMember, IntrinsicKind intrinsic) const -> bool {
  if (intrinsic != IntrinsicKind::None || (ownerClass != 0 && !staticMember) ||
      (returnType.kind != SemanticType::Class &&
       returnType.kind != SemanticType::Enum) ||
      containsOpenTypeParameter(returnType) ||
      !typeTraits(returnType).containsBorrowedState)
    return false;
  std::vector<SemanticType> expanded;
  for (const SemanticType &type : parameterTypes)
    SemanticType::appendExpandedArgument(expanded, type);
  for (const SemanticType &parameter : expanded)
    if (parameter.kind == SemanticType::Reference ||
        parameter.kind == SemanticType::Unknown ||
        containsOpenTypeParameter(parameter) ||
        typeTraits(parameter).containsBorrowedState)
      return false;
  const auto inventory = storedReferenceInventory(returnType);
  return inventory && !inventory->empty() &&
         std::all_of(inventory->begin(), inventory->end(),
                     [](const StoredReferencePath &path) {
                       return path.reference.kind == SemanticType::Reference &&
                              path.reference.referenceAccess ==
                                  AccessMode::ReadOnly;
                     });
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
  const auto alternatives =
      directReference ? std::optional{std::vector<BorrowAlternativePath>{}}
                      : storedBorrowAlternativeInventory(result);
  if (!alternatives)
    return std::nullopt;
  // Complete contracts cannot describe an escaping mutable dependency graph:
  // an unconditional carrier result with a mutable stored path keeps its base
  // origin. A conditional one (a payload enum alternative) needs its complete
  // set for a selection; each of its mutable paths must then have exactly
  // one supplier, a single exclusive edge (Rust's sole-input elision).
  if (!directReference && alternatives->empty() &&
      std::any_of(inventory->begin(), inventory->end(), [](const auto &path) {
        return path.reference.referenceAccess == AccessMode::Mutable;
      }))
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
    SymbolId globalRoot = 0;
  };
  std::vector<Source> sources;
  // A value's stored regions, each reached through its own field path.
  // The class region a stored-region result names (`int&<a> get()`).
  std::string storedRegion;
  const auto addStored = [&](BorrowOriginKind origin, std::size_t parameter,
                             const SemanticType &type) -> bool {
    const auto paths = storedReferenceInventory(type);
    if (!paths)
      return false;
    for (const auto &path : *paths) {
      // A result in one class region is supplied only by fields written in
      // that region; a field that writes no region holds the implicit one.
      if (origin == BorrowOriginKind::Receiver && !storedRegion.empty() &&
          !path.fields.empty() && path.fields.front().declaration != nullptr) {
        std::vector<std::string> names;
        collectRegionNames(path.fields.front().declaration->type(), names);
        if (!names.empty() && !contains(names, storedRegion))
          continue;
      }
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
  bool storedOnly = false;
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
  } else if (!method &&
             function.returnBorrowOrigin == BorrowOriginKind::Argument) {
    // The elided base origin already names the suppliers, such as the one
    // parameter an iterator result moves from; the complete contract maps
    // fields of those suppliers only.
    namedParameters = function.returnBorrowParameters.empty()
                          ? std::vector{function.returnBorrowParameter}
                          : function.returnBorrowParameters;
  }
  // A result written with a region the class binds names the receiver's
  // stored regions, not the receiver itself (`int&<data> get()` in
  // `S<lifetime data>`), whether or not that counts as an explicit contract.
  if (method)
    if (const FunctionDecl *syntax = function.bodyDeclaration()) {
      const auto lifetime = explicitLifetimeResult(
          *syntax, function.parameterTypes, function.ownerClass);
      storedOnly = lifetime.classBinder;
      if (lifetime.classBinder)
        storedRegion = lifetime.region;
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
    if (receiverMutability != ReceiverMutability::Consuming && !storedOnly)
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
  // A free function's result written as a bare type parameter carries
  // exactly that parameter's regions; a reference to one supplies what its
  // referent stores, not itself.
  std::optional<std::vector<bool>> typeParameterSuppliers;
  if (parametersSupply && !function.explicitReturnBorrowOrigin)
    typeParameterSuppliers = typeParameterResultSuppliers(function);
  if (parametersSupply) {
    // Named positions are expanded: a concrete pack supplies per lane.
    std::vector<SemanticType> expanded;
    for (const SemanticType &type : function.parameterTypes)
      SemanticType::appendExpandedArgument(expanded, type);
    if (typeParameterSuppliers &&
        typeParameterSuppliers->size() != expanded.size())
      typeParameterSuppliers.reset();
    for (std::size_t index = 0; index < expanded.size(); ++index) {
      if (!named(index))
        continue;
      const SemanticType &parameter = expanded[index];
      if (typeParameterSuppliers) {
        if (!(*typeParameterSuppliers)[index])
          continue;
        const SemanticType &stored =
            parameter.kind == SemanticType::Reference &&
                    parameter.arguments.size() == 1
                ? parameter.arguments.front()
                : parameter;
        if (typeTraits(stored).containsBorrowedState &&
            !addStored(BorrowOriginKind::Argument, index, stored))
          return std::nullopt;
        continue;
      }
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

  // A result with no borrow-carrying input is `static`; an exact static
  // place the body refines it to supplies every stored path. MIR proves each
  // returned loan against it.
  if (sources.empty() && !method &&
      function.returnBorrowOrigin == BorrowOriginKind::Global &&
      function.returnBorrowPlace && function.returnBorrowPlace->valid()) {
    Source global{.origin = BorrowOriginKind::Global,
                  .parameter = 0,
                  .access = AccessMode::ReadOnly,
                  .globalRoot = function.returnBorrowPlace->root};
    for (const PlaceProjection &projection :
         function.returnBorrowPlace->projections) {
      if (projection.kind != PlaceProjectionKind::Field)
        return std::nullopt;
      global.fields.push_back(projection.field);
    }
    sources.push_back(std::move(global));
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
    std::size_t suppliers = 0;
    // Rust elision: a borrowed receiver supplies the result whole; what it
    // stores is reached through it. Its stored regions supply a path only
    // when the receiver itself cannot (a mutable path from a read-only one).
    const bool wholeReceiver =
        std::any_of(sources.begin(), sources.end(), [&](const Source &source) {
          return source.origin == BorrowOriginKind::Receiver &&
                 source.fields.empty() && source.conditions.empty() &&
                 source.reference.kind != SemanticType::Reference &&
                 (access != AccessMode::Mutable ||
                  source.access == AccessMode::Mutable);
        });
    for (const Source &source : sources) {
      if (access == AccessMode::Mutable && source.access != AccessMode::Mutable)
        continue;
      if (wholeReceiver && source.origin == BorrowOriginKind::Receiver &&
          source.reference.kind == SemanticType::Reference)
        continue;
      // A stored reference supplies only a referent of its own type, except
      // that a result in its class region may also borrow storage inside
      // that referent (Rust: `&'r R` supplies `&'r i32` to a field of R).
      if (source.reference.kind == SemanticType::Reference &&
          !(storedOnly && source.origin == BorrowOriginKind::Receiver) &&
          (source.reference.arguments.size() != 1 ||
           path.reference.arguments.size() != 1 ||
           source.reference.arguments.front() !=
               path.reference.arguments.front()))
        continue;
      supplied = true;
      ++suppliers;
      BorrowedFieldOrigin dependency{.resultFields = fields,
                                     .origin = source.origin,
                                     .parameter = source.parameter,
                                     .globalRoot = source.globalRoot,
                                     .sourceFields = source.fields,
                                     .access = access,
                                     .resultConditions = *conditions,
                                     .sourceConditions = source.conditions};
      if (std::find(summary.dependencies.begin(), summary.dependencies.end(),
                    dependency) == summary.dependencies.end())
        summary.dependencies.push_back(std::move(dependency));
    }
    if (!supplied ||
        (!directReference && access == AccessMode::Mutable && suppliers > 1))
      return std::nullopt;
  }
  // A result borrowed whole from one supplier, in every stored path and with
  // no conditional alternative, is exactly its base origin; a complete
  // contract would only restate it.
  const auto &first = summary.dependencies.front();
  if (summary.possibleAlternatives.empty() &&
      std::all_of(summary.dependencies.begin(), summary.dependencies.end(),
                  [&](const BorrowedFieldOrigin &dependency) {
                    return dependency.origin == first.origin &&
                           dependency.parameter == first.parameter &&
                           dependency.sourceFields.empty() &&
                           dependency.sourceConditions.empty() &&
                           dependency.resultConditions.empty();
                  }))
    return std::nullopt;
  return summary;
}

auto SemanticVisitor::Impl::conformanceOperationLifetimeFailure(
    const ConformanceDecl &block, const ConceptDecl &conceptDecl,
    const FunctionDecl &declared, const FunctionDecl &written) const
    -> std::optional<std::string> {
  // The head's lifetime arguments substitute the concept's lifetime
  // parameters. A parameter the head does not bind keeps a name no block
  // region can spell.
  const auto conceptLifetimes = conceptDecl.lifetimeParameters();
  const auto &headLifetimes = block.target().lifetimeArguments;
  const auto rename = [&](const std::string &region) -> std::string {
    for (std::size_t index = 0; index < conceptLifetimes.size(); ++index)
      if (conceptLifetimes[index].name.lexeme == region)
        return index < headLifetimes.size()
                   ? writtenRegion(headLifetimes[index])
                         .value_or("<elided " + region + ">")
                   : "<concept " + region + ">";
    return region;
  };
  const auto spell = [](const std::string &region) {
    return region.front() == '<' ? std::string("an elided region")
                                 : "'" + region + "'";
  };

  // Parameter regions. An elided expected region accepts an elided or
  // inferred one; a written region requires the substituted name exactly.
  const auto &conceptTypes = conceptDecl.typeParameters();
  std::optional<std::string> failure;
  const auto compare = [&](const std::optional<std::string> &expected,
                           const std::optional<std::string> &actual) {
    if (failure || !actual)
      return;
    if (!expected) {
      if (*actual == "static")
        failure = "writes region 'static' where the concept's signature "
                  "leaves the region to the caller";
      return;
    }
    if (*expected != *actual)
      failure = "writes region '" + *actual +
                "' where the concept's "
                "signature requires " +
                spell(*expected);
  };
  const auto match = [&](const auto &self, const TypeRef &expected,
                         const TypeRef &actual, bool head) -> void {
    const auto expectedReference =
        writtenReferenceRegion(expected.referenceLifetime);
    compare(expectedReference && !head
                ? std::optional<std::string>(rename(*expectedReference))
                : expectedReference,
            writtenReferenceRegion(actual.referenceLifetime));
    const TypeRef *source = &expected;
    if (!head && expected.name.segments.size() == 1 &&
        expected.arguments.empty() && expected.lifetimeArguments.empty())
      for (std::size_t index = 0; index < conceptTypes.size(); ++index)
        if (conceptTypes[index].lexeme == expected.name.last().lexeme &&
            index < block.target().arguments.size()) {
          source = &block.target().arguments[index];
          head = true;
        }
    for (std::size_t index = 0; index < actual.lifetimeArguments.size();
         ++index) {
      std::optional<std::string> region;
      if (index < source->lifetimeArguments.size())
        region = writtenRegion(source->lifetimeArguments[index]);
      if (region && !head)
        region = rename(*region);
      compare(region, writtenRegion(actual.lifetimeArguments[index]));
    }
    if (source->arguments.size() == actual.arguments.size())
      for (std::size_t index = 0; index < actual.arguments.size(); ++index)
        self(self, source->arguments[index], actual.arguments[index], head);
  };
  const auto &expectedParameters = declared.parameters();
  const auto &actualParameters = written.parameters();
  for (std::size_t index = 0; index < expectedParameters.size() &&
                              index < actualParameters.size() && !failure;
       ++index) {
    match(match, expectedParameters[index].type, actualParameters[index].type,
          false);
    if (failure)
      return "parameter '" + actualParameters[index].name.lexeme + "' " +
             *failure;
  }

  // Requirements: in every alternative the concept operation and the block
  // establish, some alternative of the written clause must hold.
  auto assumed = declaredOutlivesAlternatives(declared.requiresClause());
  for (auto &relations : assumed)
    for (auto &[longer, shorter] : relations) {
      longer = rename(longer);
      shorter = rename(shorter);
    }
  const auto blockRelations =
      declaredOutlivesAlternatives(block.requiresClause());
  std::vector<Relations> combined;
  for (const auto &first : assumed)
    for (const auto &second : blockRelations) {
      auto both = first;
      both.insert(both.end(), second.begin(), second.end());
      combined.push_back(std::move(both));
    }
  const auto required = declaredOutlivesAlternatives(written.requiresClause());
  for (const auto &assumption : combined) {
    const auto satisfied = [&](const Relations &relations) {
      return std::all_of(
          relations.begin(), relations.end(), [&](const auto &relation) {
            return outlivesUnder(assumption, relation.first, relation.second);
          });
    };
    if (std::any_of(required.begin(), required.end(), satisfied))
      continue;
    for (const auto &[longer, shorter] : required.front())
      if (!outlivesUnder(assumption, longer, shorter))
        return "requires '" + longer + "' to outlive '" + shorter +
               "', which the concept's operation does not declare";
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::validateConformanceAssociatedRegions(
    const ConformanceTypeCheck &check) -> void {
  const ConformanceDecl &block = *check.declaration;
  std::vector<std::string> blockTypes;
  for (const auto &parameter : check.genericParameters)
    blockTypes.push_back(parameter.name.lexeme);
  std::vector<std::string> blockRegions{"static"};
  for (const auto &parameter : block.lifetimeParameters())
    blockRegions.push_back(parameter.name.lexeme);
  // The (type argument, region argument) pairs a nominal application's
  // fields require, where both name block parameters.
  const auto applicationBounds = [&](const auto &self, const TypeRef &type,
                                     Relations &bounds) -> void {
    const SemanticType resolved = typeOf(type, check.namespaceScope);
    if (resolved.kind == SemanticType::Class && resolved.classId != 0 &&
        resolved.classId <= classes.size())
      if (const auto *declaration = classInfo(resolved.classId).declaration)
        for (const auto &[typeIndex, lifetimeIndex] :
             classOutlivesBounds(*declaration)) {
          if (typeIndex >= type.arguments.size() ||
              lifetimeIndex >= type.lifetimeArguments.size())
            continue;
          const TypeRef &argument = type.arguments[typeIndex];
          const auto region =
              writtenRegion(type.lifetimeArguments[lifetimeIndex]);
          if (!region || argument.reference ||
              argument.name.segments.size() != 1 ||
              !argument.arguments.empty() ||
              !contains(blockTypes, argument.name.last().lexeme) ||
              !contains(blockRegions, *region))
            continue;
          bounds.emplace_back(argument.name.last().lexeme, *region);
        }
    for (const auto &argument : type.arguments)
      self(self, argument, bounds);
  };
  // What the head establishes: its patterns' implied bounds, in every
  // alternative of the block's own declared relations.
  Relations implied;
  for (const TypeRef *pattern : check.types)
    applicationBounds(applicationBounds, *pattern, implied);
  auto alternatives = declaredOutlivesAlternatives(block.requiresClause());
  for (auto &relations : alternatives)
    relations.insert(relations.end(), implied.begin(), implied.end());
  for (const auto &associated : check.associatedTypes) {
    Relations required;
    applicationBounds(applicationBounds, associated.declaration->target(),
                      required);
    for (const auto &[longer, shorter] : required) {
      const bool holds =
          std::all_of(alternatives.begin(), alternatives.end(),
                      [&](const Relations &relations) {
                        return outlivesUnder(relations, longer, shorter);
                      });
      if (holds)
        continue;
      report(associated.declaration->name(),
             "Associated type '" + associated.declaration->name().lexeme +
                 "' requires type '" + longer + "' to outlive '" + shorter +
                 "', which the conformance block's head does not establish.",
             "GTI-S2127");
      break;
    }
  }
}

} // namespace lang
