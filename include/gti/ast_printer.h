#pragma once

#include "gti/ast.h"

#include <cstddef>
#include <initializer_list>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>

namespace lang {

class AstPrinter final : public ExprVisitor {
public:
  std::string print(const Expr &expr) {
    expr.accept(*this);
    return result;
  }

  void visitAssignExpr(const Assign &expr) override {
    std::string name;
    for (const Token &segment : expr.path().segments) {
      name += (name.empty() ? "" : "::") + segment.lexeme;
    }
    result = "(" + expr.oper().lexeme + " " + name + " " +
             printPtr(expr.value()) + ")";
  }

  void visitArrayInitializerExpr(const ArrayInitializer &expr) override {
    std::string text = "(array";
    for (const ExprPtr &element : expr.elements()) {
      text += " " + printPtr(element);
    }
    result = text + ")";
  }

  void visitBinaryExpr(const Binary &expr) override {
    result = parenthesize(expr.oper().lexeme,
                          {expr.left().get(), expr.right().get()});
  }

  void visitCallExpr(const Call &expr) override {
    std::string text = "(call " + printPtr(expr.callee());
    if (!expr.typeArguments().empty()) {
      text += "<";
      for (std::size_t index = 0; index < expr.typeArguments().size();
           ++index) {
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

  void visitConditionalExpr(const ConditionalExpr &expr) override {
    result =
        parenthesize("?:", {expr.condition().get(), expr.thenExpression().get(),
                            expr.elseExpression().get()});
  }

  void visitConversionExpr(const Conversion &expr) override {
    result = "(convert " + typeToString(expr.targetType()) + " " +
             printPtr(expr.value()) + ")";
  }

  void visitDirectInitializerExpr(const DirectInitializer &expr) override {
    std::string text = "(direct-init";
    for (const ExprPtr &argument : expr.arguments()) {
      text += " " + printPtr(argument);
    }
    result = text + ")";
  }

  void visitDereferenceSetExpr(const DereferenceSet &expr) override {
    result = "(" + expr.oper().lexeme + " *" + printPtr(expr.object()) + " " +
             printPtr(expr.value()) + ")";
  }

  void visitGetExpr(const Get &expr) override {
    result = "(" + expr.access().lexeme + " " + printPtr(expr.object()) + " " +
             expr.name().lexeme + ")";
  }

  void visitGroupingExpr(const Grouping &expr) override {
    result = parenthesize("group", {expr.expression().get()});
  }

  void visitIndexExpr(const Index &expr) override {
    result = parenthesize("index", {expr.object().get(), expr.index().get()});
  }

  void visitIndexSetExpr(const IndexSet &expr) override {
    result = "(" + expr.oper().lexeme + " " + printPtr(expr.object()) + "[" +
             printPtr(expr.index()) + "] " + printPtr(expr.value()) + ")";
  }

  void visitLambdaExpr(const Lambda &expr) override {
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

  void visitLayoutQueryExpr(const LayoutQuery &expr) override {
    result =
        "(" + expr.keyword().lexeme + " " + typeToString(expr.type()) + ")";
  }

  void visitLiteralExpr(const LiteralExpr &expr) override {
    result = literalToString(expr.value());
  }

  void visitLogicalExpr(const Logical &expr) override {
    result = parenthesize(expr.oper().lexeme,
                          {expr.left().get(), expr.right().get()});
  }

  void visitPackExpansionExpr(const PackExpansion &expr) override {
    result = expr.name().lexeme + "...";
  }

  void visitPostfixExpr(const Postfix &expr) override {
    result = "(" + printPtr(expr.expression()) + expr.oper().lexeme + ")";
  }

  void visitQualifiedNameExpr(const QualifiedName &expr) override {
    result.clear();
    for (const Token &segment : expr.name().segments) {
      if (!result.empty()) {
        result += "::";
      }
      result += segment.lexeme;
    }
  }

  void visitThisExpr(const This &expr) override {
    result = expr.keyword().lexeme;
  }

  void visitSetExpr(const Set &expr) override {
    result = "(" + expr.oper().lexeme + " " + printPtr(expr.object()) +
             expr.access().lexeme + expr.name().lexeme + " " +
             printPtr(expr.value()) + ")";
  }

  void visitUnaryExpr(const Unary &expr) override {
    result = parenthesize(expr.oper().lexeme, {expr.right().get()});
  }

  void visitUnexpectedExpr(const Unexpected &expr) override {
    result = parenthesize("unexpected", {expr.error().get()});
  }

  void visitVariableExpr(const Variable &expr) override {
    result = expr.name().lexeme;
  }

private:
  static std::string typeToString(const TypeRef &type) {
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
      text += "[" + (extent ? arrayExtentSpelling(*extent) : std::string("?")) +
              "]";
    }
    if (type.reference) {
      text += "&";
    }
    return text;
  }

  std::string printPtr(const ExprPtr &expr) {
    if (!expr) {
      return "<null>";
    }
    return print(*expr);
  }

  std::string parenthesize(std::string_view name,
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

  std::string literalToString(const Literal &literal) {
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

  std::string result;
};

} // namespace lang
