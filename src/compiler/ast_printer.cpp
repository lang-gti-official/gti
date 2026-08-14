#include "gti/ast_printer.h"

#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>

namespace lang {

std::string AstPrinter::print(const Expr &expr) {
  expr.accept(*this);
  return result;
}

void AstPrinter::visitAssignExpr(const Assign &expr) {
  std::string name;
  for (const Token &segment : expr.path().segments) {
    name += (name.empty() ? "" : "::") + segment.lexeme;
  }
  result = "(" + expr.oper().lexeme + " " + name + " " +
           printPtr(expr.value()) + ")";
}

void AstPrinter::visitArrayInitializerExpr(const ArrayInitializer &expr) {
  std::string text = "(array";
  for (const ExprPtr &element : expr.elements()) {
    text += " " + printPtr(element);
  }
  result = text + ")";
}

void AstPrinter::visitBinaryExpr(const Binary &expr) {
  result =
      parenthesize(expr.oper().lexeme, {expr.left().get(), expr.right().get()});
}

void AstPrinter::visitCallExpr(const Call &expr) {
  std::string text = "(call " + printPtr(expr.callee());
  if (!expr.typeArguments().empty()) {
    text += "<";
    for (std::size_t index = 0; index < expr.typeArguments().size(); ++index) {
      if (index > 0) {
        text += ",";
      }
      text += typeToString(expr.typeArguments()[index]);
    }
    text += ">";
  }
  for (const auto &argument : expr.arguments()) {
    text += " " + printPtr(argument);
  }
  result = text + ")";
}

void AstPrinter::visitConditionalExpr(const ConditionalExpr &expr) {
  result =
      parenthesize("?:", {expr.condition().get(), expr.thenExpression().get(),
                          expr.elseExpression().get()});
}

void AstPrinter::visitConversionExpr(const Conversion &expr) {
  result = "(convert " + typeToString(expr.targetType()) + " " +
           printPtr(expr.value()) + ")";
}

void AstPrinter::visitDirectInitializerExpr(const DirectInitializer &expr) {
  std::string text = "(direct-init";
  for (const ExprPtr &argument : expr.arguments()) {
    text += " " + printPtr(argument);
  }
  result = text + ")";
}

void AstPrinter::visitDereferenceSetExpr(const DereferenceSet &expr) {
  result = "(" + expr.oper().lexeme + " *" + printPtr(expr.object()) + " " +
           printPtr(expr.value()) + ")";
}

void AstPrinter::visitGetExpr(const Get &expr) {
  result = "(" + expr.access().lexeme + " " + printPtr(expr.object()) + " " +
           expr.name().lexeme + ")";
}

void AstPrinter::visitGroupingExpr(const Grouping &expr) {
  result = parenthesize("group", {expr.expression().get()});
}

void AstPrinter::visitIndexExpr(const Index &expr) {
  result = parenthesize("index", {expr.object().get(), expr.index().get()});
}

void AstPrinter::visitIndexSetExpr(const IndexSet &expr) {
  result = "(" + expr.oper().lexeme + " " + printPtr(expr.object()) + "[" +
           printPtr(expr.index()) + "] " + printPtr(expr.value()) + ")";
}

void AstPrinter::visitLambdaExpr(const Lambda &expr) {
  std::ostringstream stream;
  stream << "(lambda [";
  for (std::size_t index = 0; index < expr.captures().size(); ++index) {
    if (index != 0) {
      stream << ",";
    }
    const LambdaCapture &capture = expr.captures()[index];
    stream << capture.name.lexeme;
    if (capture.explicitInitializer()) {
      stream << "=" << printPtr(capture.initializer);
    }
  }
  stream << "] -> " << typeToString(expr.returnType()) << ")";
  result = stream.str();
}

void AstPrinter::visitLayoutQueryExpr(const LayoutQuery &expr) {
  result = "(" + expr.keyword().lexeme + " " + typeToString(expr.type()) + ")";
}

void AstPrinter::visitLiteralExpr(const LiteralExpr &expr) {
  result = literalToString(expr.value());
}

void AstPrinter::visitLogicalExpr(const Logical &expr) {
  result =
      parenthesize(expr.oper().lexeme, {expr.left().get(), expr.right().get()});
}

void AstPrinter::visitPackExpansionExpr(const PackExpansion &expr) {
  result = expr.name().lexeme + "...";
}

void AstPrinter::visitPostfixExpr(const Postfix &expr) {
  result = "(" + printPtr(expr.expression()) + expr.oper().lexeme + ")";
}

void AstPrinter::visitQualifiedNameExpr(const QualifiedName &expr) {
  result.clear();
  for (const Token &segment : expr.name().segments) {
    if (!result.empty()) {
      result += "::";
    }
    result += segment.lexeme;
  }
}

void AstPrinter::visitThisExpr(const This &expr) {
  result = expr.keyword().lexeme;
}

void AstPrinter::visitSetExpr(const Set &expr) {
  result = "(" + expr.oper().lexeme + " " + printPtr(expr.object()) +
           expr.access().lexeme + expr.name().lexeme + " " +
           printPtr(expr.value()) + ")";
}

void AstPrinter::visitUnaryExpr(const Unary &expr) {
  result = parenthesize(expr.oper().lexeme, {expr.right().get()});
}

void AstPrinter::visitUnexpectedExpr(const Unexpected &expr) {
  result = parenthesize("unexpected", {expr.error().get()});
}

void AstPrinter::visitVariableExpr(const Variable &expr) {
  result = expr.name().lexeme;
}

std::string AstPrinter::typeToString(const TypeRef &type) {
  std::string text;
  for (std::size_t index = 0; index < type.name.segments.size(); ++index) {
    if (index != 0) {
      text += "::";
    }
    text += type.name.segments[index].lexeme;
  }
  if (type.pointeeConst) {
    text = "const " + text;
  }
  if (!type.arguments.empty()) {
    text += "<";
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      if (index > 0) {
        text += ",";
      }
      text += typeToString(type.arguments[index]);
    }
    text += ">";
  }
  if (type.pointer) {
    text += "*";
  }
  for (const ArrayExtentExprPtr &extent : type.arrayExtents) {
    text +=
        "[" + (extent ? arrayExtentSpelling(*extent) : std::string("?")) + "]";
  }
  if (type.reference) {
    text += "&";
  }
  return text;
}

std::string AstPrinter::printPtr(const ExprPtr &expr) {
  if (!expr) {
    return "<null>";
  }
  return print(*expr);
}

std::string
AstPrinter::parenthesize(std::string_view name,
                         std::initializer_list<const Expr *> expressions) {
  std::string text = "(";
  text += name;

  for (const Expr *expr : expressions) {
    text += " ";
    text += expr == nullptr ? "<null>" : print(*expr);
  }

  text += ")";
  return text;
}

std::string AstPrinter::literalToString(const Literal &literal) {
  if (std::holds_alternative<std::monostate>(literal)) {
    return "nil";
  }
  if (std::holds_alternative<std::nullptr_t>(literal)) {
    return "nullptr";
  }
  if (const auto *value = std::get_if<std::uint64_t>(&literal)) {
    return std::to_string(*value);
  }
  if (const auto *value = std::get_if<BinaryFloat>(&literal)) {
    std::ostringstream stream;
    const bool binary64 = value->format == BinaryFloatFormat::Binary64;
    stream << (binary64 ? "float64(0x" : "float32(0x") << std::hex
           << std::setw(binary64 ? 16 : 8) << std::setfill('0') << value->bits
           << ')';
    return stream.str();
  }
  if (const auto *value = std::get_if<CharacterLiteral>(&literal)) {
    switch (value->value) {
    case 0:
      return "'\\0'";
    case '\n':
      return "'\\n'";
    case '\r':
      return "'\\r'";
    case '\t':
      return "'\\t'";
    case '\\':
      return "'\\\\'";
    case '\'':
      return "'\\\''";
    default:
      if (value->value >= 32 && value->value <= 126) {
        return std::string("'") + static_cast<char>(value->value) + "'";
      }
      return "char(" + std::to_string(value->value) + ")";
    }
  }
  if (const auto *value = std::get_if<std::string>(&literal)) {
    return *value;
  }
  if (const auto *value = std::get_if<bool>(&literal)) {
    return *value ? "true" : "false";
  }

  return "nil";
}

} // namespace lang
