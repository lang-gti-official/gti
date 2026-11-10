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

// The region one `outlives` argument names: a bare region, or a type's
// written reference region (`int&<a>`) or first lifetime argument.
std::optional<std::string> argumentRegion(const TypeRef &argument) {
  if (argument.referenceLifetime)
    return argument.referenceLifetime->name.lexeme;
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

auto SemanticVisitor::Impl::validateSignatureRegions(
    const FunctionDecl &syntax, const FunctionInfo &function) -> void {
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
  for (const auto &application : clause->expression.applications) {
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
    const std::vector<const ExprPtr *> &arguments, const Token &site) -> void {
  // A parameter written in `static` requires a static argument.
  for (const auto &parameter : parameters)
    if (parameter.type.referenceLifetime &&
        parameter.type.referenceLifetime->name.lexeme == "static")
      required.emplace_back("<static parameter " + parameter.name.lexeme + ">",
                            "static");
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
  }
  const auto mapped =
      [&](const std::string &region) -> std::optional<std::string> {
    if (region == "static")
      return region;
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
    const FunctionDecl &syntax, const ExprPtr &value) -> void {
  if (!value)
    return;
  const auto declared = resultRegion(syntax.returnType());
  if (!declared || declared->kind != TokenKind::IDENTIFIER)
    return;
  const std::string target = declared->lexeme;
  bool functionBound = false;
  for (const auto &parameter : syntax.lifetimeParameters())
    functionBound |= parameter.name.lexeme == target;
  if (!functionBound)
    return;
  const Expr *returned = value.get();
  while (const auto *group = exprAs<Grouping>(returned))
    returned = group->expression().get();
  const auto *call = exprAs<Call>(returned);
  const auto *resolved = call ? semanticModel.findCall(*call) : nullptr;
  if (resolved == nullptr || resolved->declaration == nullptr)
    return;
  const FunctionDecl &callee = *resolved->declaration;
  const auto calleeRegion = resultRegion(callee.returnType());
  if (!calleeRegion || calleeRegion->kind != TokenKind::IDENTIFIER)
    return;
  const auto relations = declaredOutlives(syntax);
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
  const auto &parameters = callee.parameters();
  const auto &arguments = call->arguments();
  for (std::size_t index = 0;
       index < parameters.size() && index < arguments.size(); ++index) {
    if (parameters[index].pack)
      return;
    std::vector<std::string> names;
    collectRegionNames(parameters[index].type, names);
    if (!contains(names, calleeRegion->lexeme))
      continue;
    const std::string supplier = callerRegionOf(syntax, arguments[index]);
    if (outlives(supplier))
      continue;
    report(call->paren(),
           "The returned call's region is supplied by " +
               (supplier.front() == '<' ? std::string("an argument's region")
                                        : "'" + supplier + "'") +
               ", which is not declared to outlive the result's region '" +
               target + "'.",
           "GTI-S2127");
    return;
  }
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
  // Declared relations, plus Rust's implied bounds: a parameter written
  // `X<..T..>&<r>` implies T outlives r.
  auto relations = declaredOutlives(caller);
  const auto implied = [&](const auto &self, const TypeRef &type,
                           const std::string &region) -> void {
    for (const auto &argument : type.arguments) {
      if (argument.name.segments.size() == 1 &&
          contains(callerTypes, argument.name.last().lexeme))
        relations.emplace_back(argument.name.last().lexeme, region);
      self(self, argument, region);
    }
  };
  for (const auto &parameter : caller.parameters())
    if (parameter.type.referenceLifetime)
      implied(implied, parameter.type,
              parameter.type.referenceLifetime->name.lexeme);
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

} // namespace lang
