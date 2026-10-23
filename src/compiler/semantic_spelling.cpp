#include "semantic_spelling.h"

namespace lang::semantic_spelling {

std::string expressionSpelling(const Expr &expression) {
  if (const auto *variable = exprAs<Variable>(&expression))
    return variable->name().lexeme;
  if (const auto *qualified = exprAs<QualifiedName>(&expression))
    return pathSpelling(qualified->name());
  if (const auto *literal = exprAs<LiteralExpr>(&expression))
    return literal->token().lexeme;
  if (const auto *group = exprAs<Grouping>(&expression))
    return "(" + expressionSpelling(*group->expression()) + ")";
  if (const auto *binary = exprAs<Binary>(&expression))
    return "(" + expressionSpelling(*binary->left()) + " " +
           binary->oper().lexeme + " " + expressionSpelling(*binary->right()) + ")";
  if (const auto *logical = exprAs<Logical>(&expression))
    return "(" + expressionSpelling(*logical->left()) + " " +
           logical->oper().lexeme + " " + expressionSpelling(*logical->right()) + ")";
  if (const auto *unary = exprAs<Unary>(&expression))
    return unary->oper().lexeme + expressionSpelling(*unary->right());
  if (const auto *get = exprAs<Get>(&expression))
    return expressionSpelling(*get->object()) + get->access().lexeme + get->name().lexeme;
  if (const auto *call = exprAs<Call>(&expression)) {
    std::string result = expressionSpelling(*call->callee());
    if (!call->typeArguments().empty()) {
      result += "<";
      for (std::size_t index = 0; index < call->typeArguments().size(); ++index) {
        if (index != 0)
          result += ", ";
        result += typeRefSpelling(call->typeArguments()[index]);
      }
      result += ">";
    }
    result += "(";
    for (std::size_t index = 0; index < call->arguments().size(); ++index) {
      if (index != 0)
        result += ", ";
      result += expressionSpelling(*call->arguments()[index]);
    }
    return result + ")";
  }
  return "<expression>";
}

std::string qualifiedName(const std::vector<std::string> &scope,
                          std::size_t segmentCount, std::string_view name) {
  std::string result;
  for (std::size_t index = 0; index < segmentCount; ++index) {
    if (!result.empty()) {
      result += "::";
    }
    result += scope[index];
  }
  if (!result.empty()) {
    result += "::";
  }
  result += name;
  return result;
}

std::string qualifiedName(const std::vector<std::string> &scope,
                          std::string_view name) {
  return qualifiedName(scope, scope.size(), name);
}

std::string pathSpelling(const NamePath &path) {
  std::string result;
  for (const Token &segment : path.segments) {
    if (!result.empty()) {
      result += "::";
    }
    result += segment.lexeme;
  }
  return result;
}

std::string typeRefSpelling(const TypeRef &type) {
  if (type.genericValueExpression) {
    return arrayExtentSpelling(*type.genericValueExpression);
  }
  std::string result =
      !type.pointers.empty() && type.pointers.front().pointeeConst ? "const "
                                                                   : "";
  result += type.qualifiedOwner ? typeRefSpelling(*type.qualifiedOwner) +
                                      "::" + type.name.last().lexeme
                                : pathSpelling(type.name);
  if (!type.arguments.empty()) {
    result += '<';
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      if (index != 0) {
        result += ", ";
      }
      result += typeRefSpelling(type.arguments[index]);
    }
    result += '>';
  }
  for (std::size_t index = 0; index < type.pointers.size(); ++index) {
    if (index != 0 && type.pointers[index].pointeeConst) {
      result += " const";
    }
    result += '*';
  }
  for (const ArrayExtentExprPtr &extent : type.arrayExtents) {
    result +=
        '[' + (extent ? arrayExtentSpelling(*extent) : std::string("?")) + ']';
  }
  if (type.reference) {
    result += type.reference->lexeme;
  }
  return result;
}

std::string callableSpelling(const ExprPtr &callee) {
  if (const auto *variable = exprAs<Variable>(callee.get())) {
    return variable->name().lexeme;
  }
  if (const auto *qualified = exprAs<QualifiedName>(callee.get())) {
    return pathSpelling(qualified->name());
  }
  if (const auto *member = exprAs<Get>(callee.get())) {
    return member->name().lexeme;
  }
  return "function";
}

std::string_view classKindSpelling(ClassKind kind) {
  switch (kind) {
  case ClassKind::Class:
    return "class";
  case ClassKind::Struct:
    return "struct";
  case ClassKind::Interface:
    return "interface";
  case ClassKind::Union:
    return "union";
  }
  return "type";
}

std::string typeSpelling(const SemanticModel &semantics,
                         const SemanticType &type) {
  return SemanticTypePrinter(semantics).print(type);
}

} // namespace lang::semantic_spelling
