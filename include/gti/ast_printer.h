#pragma once

#include "gti/ast.h"

#include <cstddef>
#include <initializer_list>
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
    result = "(" + expr.oper().lexeme + " " + expr.name().lexeme + " " +
             printPtr(expr.value()) + ")";
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

  void visitGetExpr(const Get &expr) override {
    result = "(. " + printPtr(expr.object()) + " " + expr.name().lexeme + ")";
  }

  void visitGroupingExpr(const Grouping &expr) override {
    result = parenthesize("group", {expr.expression().get()});
  }

  void visitLiteralExpr(const LiteralExpr &expr) override {
    result = literalToString(expr.value());
  }

  void visitLogicalExpr(const Logical &expr) override {
    result = parenthesize(expr.oper().lexeme,
                          {expr.left().get(), expr.right().get()});
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

  void visitSelfExpr(const Self &expr) override { result = expr.keyword().lexeme; }

  void visitSetExpr(const Set &expr) override {
    result = "(" + expr.oper().lexeme + " " + printPtr(expr.object()) + "." +
             expr.name().lexeme + " " + printPtr(expr.value()) + ")";
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
    for (const Token &segment : type.name.segments) {
      if (!text.empty()) {
        text += "::";
      }
      text += segment.lexeme;
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
    if (const auto *value = std::get_if<double>(&literal)) {
      std::ostringstream stream;
      stream << *value;
      return stream.str();
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
