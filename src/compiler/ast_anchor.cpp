// The source token that anchors diagnostics for an expression. AST-only:
// semantics and MIR both report against it.

#include "gti/ast.h"

namespace lang {

Token expressionAnchorToken(const Expr &expression) {
  switch (expression.exprKind()) {
  case ExprKind::Assign:
    return static_cast<const Assign &>(expression).oper();
  case ExprKind::ArrayInitializer:
    return static_cast<const ArrayInitializer &>(expression).brace();
  case ExprKind::Binary:
    return static_cast<const Binary &>(expression).oper();
  case ExprKind::Call:
    return static_cast<const Call &>(expression).paren();
  case ExprKind::ConditionalExpr:
    return static_cast<const ConditionalExpr &>(expression).question();
  case ExprKind::Conversion:
    return static_cast<const Conversion &>(expression).targetType().name.last();
  case ExprKind::DirectInitializer:
    return static_cast<const DirectInitializer &>(expression).brace();
  case ExprKind::DereferenceSet:
    return static_cast<const DereferenceSet &>(expression).dereference();
  case ExprKind::PlaceSet:
    return static_cast<const PlaceSet &>(expression).oper();
  case ExprKind::Get:
    return static_cast<const Get &>(expression).name();
  case ExprKind::Grouping:
    return expressionAnchorToken(
        *static_cast<const Grouping &>(expression).expression());
  case ExprKind::Index:
    return static_cast<const Index &>(expression).bracket();
  case ExprKind::IndexSet:
    return static_cast<const IndexSet &>(expression).bracket();
  case ExprKind::Lambda:
    return static_cast<const Lambda &>(expression).bracket();
  case ExprKind::LayoutQuery:
    return static_cast<const LayoutQuery &>(expression).keyword();
  case ExprKind::MemberRequirement:
    return static_cast<const MemberRequirement &>(expression).keyword();
  case ExprKind::ConceptTest:
    return static_cast<const ConceptTest &>(expression)
        .application()
        .name.last();
  case ExprKind::LiteralExpr:
    return static_cast<const LiteralExpr &>(expression).token();
  case ExprKind::Logical:
    return static_cast<const Logical &>(expression).oper();
  case ExprKind::PackFold:
    return static_cast<const PackFold &>(expression).ellipsis();
  case ExprKind::PackExpansion:
    return static_cast<const PackExpansion &>(expression).ellipsis();
  case ExprKind::Postfix:
    return static_cast<const Postfix &>(expression).oper();
  case ExprKind::QualifiedName:
    return static_cast<const QualifiedName &>(expression).name().last();
  case ExprKind::This:
    return static_cast<const This &>(expression).keyword();
  case ExprKind::Set:
    return static_cast<const Set &>(expression).name();
  case ExprKind::Unary:
    return static_cast<const Unary &>(expression).oper();
  case ExprKind::Variable:
    return static_cast<const Variable &>(expression).name();
  }
  return Token{};
}

Token writtenPlaceToken(const Expr &expression) {
  switch (expression.exprKind()) {
  case ExprKind::Assign:
    return static_cast<const Assign &>(expression).name();
  case ExprKind::PlaceSet:
    if (const auto &target = static_cast<const PlaceSet &>(expression).target())
      return expressionAnchorToken(*target);
    break;
  case ExprKind::Grouping:
    if (const auto &inner =
            static_cast<const Grouping &>(expression).expression())
      return writtenPlaceToken(*inner);
    break;
  default:
    break;
  }
  return expressionAnchorToken(expression);
}

} // namespace lang
