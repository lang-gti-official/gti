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
        parameters[index].pack ? expanded.size() - (parameterTypes.size() - 1) : 1;
    std::vector<std::string> names;
    collectRegionNames(parameters[index].type, names);
    const bool supplies = std::any_of(names.begin(), names.end(),
        [&](const std::string &candidate) { return contains(suppliers, candidate); });
    if (supplies)
      for (std::size_t element = position;
           element < position + width && element < expanded.size(); ++element)
        result.parameters.push_back(element);
    position += width;
  }
  return result;
}

auto SemanticVisitor::Impl::hasExplicitBorrowContract(
    const FunctionDecl &syntax) -> bool {
  if (syntax.hasFunctionAttribute("borrowed_from"))
    return true;
  // `static` keeps the inferred static-storage origin; the declared region is
  // checked against it, not substituted for it.
  const auto region = resultRegion(syntax.returnType());
  return region && region->lexeme != "_" && region->kind != TokenKind::STATIC &&
         region->lexeme != "static";
}

} // namespace lang
