#pragma once

#include "gti/ast.h"

#include <initializer_list>
#include <string>
#include <string_view>

namespace lang {

class AstPrinter final : public ExprVisitor {
public:
  std::string print(const Expr &expr);

  void visitAssignExpr(const Assign &expr) override;
  void visitArrayInitializerExpr(const ArrayInitializer &expr) override;
  void visitBinaryExpr(const Binary &expr) override;
  void visitCallExpr(const Call &expr) override;
  void visitConditionalExpr(const ConditionalExpr &expr) override;
  void visitConversionExpr(const Conversion &expr) override;
  void visitDirectInitializerExpr(const DirectInitializer &expr) override;
  void visitDereferenceSetExpr(const DereferenceSet &expr) override;
  void visitGetExpr(const Get &expr) override;
  void visitGroupingExpr(const Grouping &expr) override;
  void visitIndexExpr(const Index &expr) override;
  void visitIndexSetExpr(const IndexSet &expr) override;
  void visitLambdaExpr(const Lambda &expr) override;
  void visitLayoutQueryExpr(const LayoutQuery &expr) override;
  void visitLiteralExpr(const LiteralExpr &expr) override;
  void visitLogicalExpr(const Logical &expr) override;
  void visitPackExpansionExpr(const PackExpansion &expr) override;
  void visitPostfixExpr(const Postfix &expr) override;
  void visitQualifiedNameExpr(const QualifiedName &expr) override;
  void visitThisExpr(const This &expr) override;
  void visitSetExpr(const Set &expr) override;
  void visitUnaryExpr(const Unary &expr) override;
  void visitUnexpectedExpr(const Unexpected &expr) override;
  void visitVariableExpr(const Variable &expr) override;

private:
  static std::string typeToString(const TypeRef &type);
  std::string printPtr(const ExprPtr &expr);
  std::string parenthesize(std::string_view name,
                           std::initializer_list<const Expr *> expressions);
  std::string literalToString(const Literal &literal);

  std::string result;
};

} // namespace lang
