#include "semantic_spelling.h"

namespace lang::semantic_spelling {

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
  std::string result = type.pointeeConst ? "const " : "";
  result += pathSpelling(type.name);
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
  if (type.pointer) {
    result += '*';
  }
  if (type.outerPointer) {
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
  if (const auto *variable = dynamic_cast<const Variable *>(callee.get())) {
    return variable->name().lexeme;
  }
  if (const auto *qualified =
          dynamic_cast<const QualifiedName *>(callee.get())) {
    return pathSpelling(qualified->name());
  }
  if (const auto *member = dynamic_cast<const Get *>(callee.get())) {
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
