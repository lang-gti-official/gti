#include "gti/ast_clone.h"

#include <memory>
#include <utility>
#include <vector>

namespace lang {

namespace {

class Cloner {
public:
  explicit Cloner(std::uint32_t expansion,
                  const AstCloneOptions *options = nullptr)
      : expansion(expansion), options(options) {}

  AstCloneResult result;
  std::uint32_t expansion = 0;
  const AstCloneOptions *options = nullptr;

  [[nodiscard]] Token tok(const Token &token) const {
    Token copy = options && options->token ? options->token(token) : token;
    if (expansion != 0) {
      copy.expansion = expansion;
    }
    return copy;
  }

  [[nodiscard]] std::optional<Token>
  tok(const std::optional<Token> &token) const {
    return token ? std::optional<Token>{tok(*token)} : std::nullopt;
  }

  [[nodiscard]] std::vector<Token>
  toks(const std::vector<Token> &tokens) const {
    std::vector<Token> copies;
    copies.reserve(tokens.size());
    for (const Token &token : tokens) {
      copies.push_back(tok(token));
    }
    return copies;
  }

  [[nodiscard]] NamePath path(const NamePath &value) const {
    return NamePath(toks(value.segments));
  }

  [[nodiscard]] TypeRef type(const TypeRef &value) const {
    TypeRef copy = value;
    if (expansion != 0 || options) {
      for (Token &segment : copy.name.segments) {
        segment = tok(segment);
      }
      for (TypeRef &argument : copy.arguments) {
        argument = type(argument);
      }
      if (value.qualifiedOwner)
        copy.qualifiedOwner =
            std::make_shared<TypeRef>(type(*value.qualifiedOwner));
      for (auto &pointer : copy.pointers) {
        pointer.star = tok(pointer.star);
        pointer.pointeeConst = tok(pointer.pointeeConst);
      }
      for (auto &extent : copy.arrayExtents)
        extent = extentExpression(extent);
      copy.genericValueExpression =
          extentExpression(copy.genericValueExpression);
      copy.reference = tok(copy.reference);
      copy.referenceMutability = tok(copy.referenceMutability);
      copy.packExpansion = tok(copy.packExpansion);
      copy.functionSignatureQualifier = tok(copy.functionSignatureQualifier);
    }
    return copy;
  }

  [[nodiscard]] ArrayExtentExprPtr
  extentExpression(const ArrayExtentExprPtr &value) const {
    if (!value)
      return nullptr;
    auto copy = std::make_shared<ArrayExtentExpr>(tok(value->token));
    copy->left = extentExpression(value->left);
    copy->right = extentExpression(value->right);
    return copy;
  }

  [[nodiscard]] std::optional<TypeRef>
  type(const std::optional<TypeRef> &value) const {
    return value ? std::optional<TypeRef>{type(*value)} : std::nullopt;
  }

  [[nodiscard]] std::vector<TypeRef>
  types(const std::vector<TypeRef> &values) const {
    std::vector<TypeRef> copies;
    copies.reserve(values.size());
    for (const TypeRef &value : values) {
      copies.push_back(type(value));
    }
    return copies;
  }

  [[nodiscard]] ExprPtr expr(const ExprPtr &expression) {
    return expression ? expr(*expression) : nullptr;
  }

  [[nodiscard]] ExprList exprs(const ExprList &expressions) {
    ExprList copies;
    copies.reserve(expressions.size());
    for (const ExprPtr &expression : expressions) {
      if (const auto *pack = exprAs<PackExpansion>(expression.get());
          pack && options && options->pack) {
        if (auto values = options->pack(*pack)) {
          for (auto &value : *values)
            copies.emplace_back(std::move(value));
          continue;
        }
      }
      copies.emplace_back(expr(expression));
    }
    return copies;
  }

  [[nodiscard]] ExprPtr expr(const Expr &expression) {
    ExprPtr copy = options && options->expression
                       ? options->expression(expression)
                       : nullptr;
    if (!copy)
      copy = cloneExpression(expression);
    if (copy) {
      result.expressions.emplace(&expression, copy.get());
    }
    return copy;
  }

  [[nodiscard]] StmtPtr stmt(const StmtPtr &statement) {
    return statement ? stmt(*statement) : nullptr;
  }

  [[nodiscard]] bool stmts(const StmtList &statements, StmtList &copies) {
    copies.reserve(statements.size());
    for (const StmtPtr &statement : statements) {
      if (!statement) {
        copies.emplace_back(nullptr);
        continue;
      }
      StmtPtr copy = stmt(*statement);
      if (!copy) {
        return false;
      }
      copies.emplace_back(std::move(copy));
    }
    return true;
  }

  [[nodiscard]] StmtPtr stmt(const Stmt &statement) {
    StmtPtr copy = cloneStatement(statement);
    if (copy) {
      if (statement.extent()) {
        copy->setExtent(*statement.extent());
      }
      if (statement.documentationMarkdown()) {
        copy->setDocumentationMarkdown(*statement.documentationMarkdown());
      }
      if (statement.exportKeyword()) {
        copy->setExportKeyword(*statement.exportKeyword());
      }
      result.statements.emplace(&statement, copy.get());
    }
    return copy;
  }

private:
  [[nodiscard]] static std::optional<ParameterDefault>
  parameterDefault(const std::optional<ParameterDefault> &value, Cloner &self) {
    if (!value) {
      return std::nullopt;
    }
    return ParameterDefault(self.tok(value->equal),
                            self.expr(value->expression));
  }

  [[nodiscard]] std::vector<Parameter>
  parameters(const std::vector<Parameter> &values) {
    std::vector<Parameter> copies;
    copies.reserve(values.size());
    for (const Parameter &parameter : values) {
      copies.emplace_back(type(parameter.type), tok(parameter.name),
                          parameter.mutability, tok(parameter.pack),
                          parameterDefault(parameter.defaultArgument, *this));
    }
    return copies;
  }

  [[nodiscard]] std::unique_ptr<CompileCondition>
  condition(const std::unique_ptr<CompileCondition> &value) {
    if (!value) {
      return nullptr;
    }
    auto copy = std::make_unique<CompileCondition>();
    copy->kind = value->kind;
    copy->token = value->token;
    copy->property = value->property;
    copy->oper = value->oper;
    copy->valueToken = value->valueToken;
    copy->expectedValue = value->expectedValue;
    copy->flag = value->flag;
    copy->left = condition(value->left);
    copy->right = condition(value->right);
    copy->resolved = value->resolved;
    return copy;
  }

  [[nodiscard]] ExprPtr cloneExpression(const Expr &expression) {
    switch (expression.exprKind()) {
    case ExprKind::Assign: {
      const auto &node = static_cast<const Assign &>(expression);
      return std::make_unique<Assign>(path(node.path()), tok(node.oper()),
                                      expr(node.value()));
    }
    case ExprKind::ArrayInitializer: {
      const auto &node = static_cast<const ArrayInitializer &>(expression);
      return std::make_unique<ArrayInitializer>(
          tok(node.brace()), exprs(node.elements()), tok(node.closingBrace()));
    }
    case ExprKind::Binary: {
      const auto &node = static_cast<const Binary &>(expression);
      return std::make_unique<Binary>(expr(node.left()), tok(node.oper()),
                                      expr(node.right()));
    }
    case ExprKind::Call: {
      const auto &node = static_cast<const Call &>(expression);
      auto copy = std::make_unique<Call>(
          expr(node.callee()), types(node.typeArguments()), tok(node.paren()),
          exprs(node.arguments()));
      if (tok(node.leftParen())) {
        copy->setArgumentGeometry(*tok(node.leftParen()),
                                  toks(node.argumentCommas()));
      }
      return copy;
    }
    case ExprKind::ConditionalExpr: {
      const auto &node = static_cast<const ConditionalExpr &>(expression);
      return std::make_unique<ConditionalExpr>(
          expr(node.condition()), tok(node.question()),
          expr(node.thenExpression()), tok(node.colon()),
          expr(node.elseExpression()));
    }
    case ExprKind::Conversion: {
      const auto &node = static_cast<const Conversion &>(expression);
      return std::make_unique<Conversion>(
          type(node.targetType()), tok(node.paren()), expr(node.value()));
    }
    case ExprKind::DirectInitializer: {
      const auto &node = static_cast<const DirectInitializer &>(expression);
      return std::make_unique<DirectInitializer>(
          tok(node.brace()), exprs(node.arguments()), tok(node.closingBrace()),
          toks(node.designators()));
    }
    case ExprKind::DereferenceSet: {
      const auto &node = static_cast<const DereferenceSet &>(expression);
      return std::make_unique<DereferenceSet>(
          tok(node.dereference()), expr(node.object()), tok(node.oper()),
          expr(node.value()));
    }
    case ExprKind::PlaceSet: {
      const auto &node = static_cast<const PlaceSet &>(expression);
      return std::make_unique<PlaceSet>(expr(node.target()), tok(node.oper()),
                                        expr(node.value()));
    }
    case ExprKind::Get: {
      const auto &node = static_cast<const Get &>(expression);
      return std::make_unique<Get>(expr(node.object()), tok(node.access()),
                                   tok(node.name()));
    }
    case ExprKind::Grouping: {
      const auto &node = static_cast<const Grouping &>(expression);
      return std::make_unique<Grouping>(expr(node.expression()),
                                        tok(node.unsafeKeyword()));
    }
    case ExprKind::Index: {
      const auto &node = static_cast<const Index &>(expression);
      return std::make_unique<Index>(expr(node.object()), tok(node.bracket()),
                                     expr(node.index()));
    }
    case ExprKind::IndexSet: {
      const auto &node = static_cast<const IndexSet &>(expression);
      return std::make_unique<IndexSet>(expr(node.object()),
                                        tok(node.bracket()), expr(node.index()),
                                        tok(node.oper()), expr(node.value()));
    }
    case ExprKind::Lambda: {
      const auto &node = static_cast<const Lambda &>(expression);
      std::vector<LambdaCapture> captures;
      captures.reserve(node.captures().size());
      for (const LambdaCapture &capture : node.captures()) {
        captures.emplace_back(tok(capture.name), tok(capture.equal),
                              expr(capture.initializer), tok(capture.borrow),
                              tok(capture.mutKeyword));
      }
      StmtList body;
      if (!stmts(node.body(), body)) {
        return nullptr;
      }
      return std::make_unique<Lambda>(tok(node.bracket()), std::move(captures),
                                      parameters(node.parameters()),
                                      tok(node.arrow()),
                                      type(node.returnType()), std::move(body));
    }
    case ExprKind::LayoutQuery: {
      const auto &node = static_cast<const LayoutQuery &>(expression);
      return std::make_unique<LayoutQuery>(tok(node.keyword()),
                                           type(node.type()));
    }
    case ExprKind::MemberRequirement: {
      const auto &node = static_cast<const MemberRequirement &>(expression);
      if (node.isExpressionRequirement())
        return std::make_unique<MemberRequirement>(
            tok(node.keyword()), expr(node.expression()), type(node.result()),
            types(node.parameters()), toks(node.bindings()),
            tok(node.unsafeKeyword()), tok(node.borrowedFrom()),
            stmt(node.iteration()));
      return std::make_unique<MemberRequirement>(
          tok(node.keyword()), type(node.receiver()), tok(node.binding()),
          tok(node.receiverUse()), tok(node.member()), type(node.result()),
          types(node.parameters()), toks(node.bindings()),
          toks(node.arguments()), tok(node.unsafeKeyword()));
    }
    case ExprKind::LiteralExpr: {
      const auto &node = static_cast<const LiteralExpr &>(expression);
      return std::make_unique<LiteralExpr>(tok(node.token()), node.value());
    }
    case ExprKind::Logical: {
      const auto &node = static_cast<const Logical &>(expression);
      return std::make_unique<Logical>(expr(node.left()), tok(node.oper()),
                                       expr(node.right()));
    }
    case ExprKind::PackFold: {
      const auto &node = static_cast<const PackFold &>(expression);
      return std::make_unique<PackFold>(
          tok(node.leftParen()), expr(node.pattern()), tok(node.comma()),
          tok(node.ellipsis()), tok(node.rightParen()));
    }
    case ExprKind::PackExpansion: {
      const auto &node = static_cast<const PackExpansion &>(expression);
      return std::make_unique<PackExpansion>(tok(node.name()),
                                             tok(node.ellipsis()));
    }
    case ExprKind::Postfix: {
      const auto &node = static_cast<const Postfix &>(expression);
      return std::make_unique<Postfix>(expr(node.expression()),
                                       tok(node.oper()));
    }
    case ExprKind::QualifiedName: {
      const auto &node = static_cast<const QualifiedName &>(expression);
      return std::make_unique<QualifiedName>(path(node.name()),
                                             type(node.ownerType()));
    }
    case ExprKind::This: {
      const auto &node = static_cast<const This &>(expression);
      return std::make_unique<This>(tok(node.keyword()));
    }
    case ExprKind::Set: {
      const auto &node = static_cast<const Set &>(expression);
      return std::make_unique<Set>(expr(node.object()), tok(node.access()),
                                   tok(node.name()), tok(node.oper()),
                                   expr(node.value()));
    }
    case ExprKind::Unary: {
      const auto &node = static_cast<const Unary &>(expression);
      return std::make_unique<Unary>(tok(node.oper()), expr(node.right()),
                                     expr(node.rangeObject()));
    }
    case ExprKind::Variable: {
      const auto &node = static_cast<const Variable &>(expression);
      if (node.patternAuto() && node.patternReference()) {
        return std::make_unique<Variable>(
            tok(node.name()), tok(*node.patternAuto()),
            tok(*node.patternReference()), tok(node.patternMutability()));
      }
      return std::make_unique<Variable>(tok(node.name()));
    }
    }
    return nullptr;
  }

  [[nodiscard]] std::shared_ptr<VariableDecl>
  variable(const VariableDecl &node) {
    auto copy = std::make_shared<VariableDecl>(
        node.mutability(), type(node.type()), tok(node.name()),
        expr(node.initializer()), tok(node.staticKeyword()),
        node.isRangeBinding(), tok(node.constexprKeyword()),
        node.isRangeOwner(), node.isRangeIterator(), tok(node.mutKeyword()),
        node.isRangeElementOwner(), node.linkage(), node.nativeLinkName());
    if (node.extent()) {
      copy->setExtent(*node.extent());
    }
    result.statements.emplace(&node, copy.get());
    return copy;
  }

  template <typename Node>
  [[nodiscard]] const Node *mapped(const Node *original) const {
    if (original == nullptr) {
      return nullptr;
    }
    const auto found = result.statements.find(original);
    return found == result.statements.end()
               ? nullptr
               : static_cast<const Node *>(found->second);
  }

  [[nodiscard]] StmtPtr cloneStatement(const Stmt &statement) {
    switch (statement.stmtKind()) {
    case StmtKind::BlockStmt: {
      const auto &node = static_cast<const BlockStmt &>(statement);
      StmtList statements;
      if (!stmts(node.statements(), statements)) {
        return nullptr;
      }
      return std::make_shared<BlockStmt>(std::move(statements),
                                         tok(node.unsafeKeyword()));
    }
    case StmtKind::CompileErrorDirective: {
      const auto &node = static_cast<const CompileErrorDirective &>(statement);
      return std::make_shared<CompileErrorDirective>(tok(node.directive()),
                                                     tok(node.messageToken()));
    }
    case StmtKind::ConditionalStmt: {
      const auto &node = static_cast<const ConditionalStmt &>(statement);
      std::vector<ConditionalBranch> branches;
      branches.reserve(node.branches().size());
      for (const ConditionalBranch &branch : node.branches()) {
        ConditionalBranch copy;
        if (branch.condition) {
          CompileCondition value;
          value.kind = branch.condition->kind;
          value.token = branch.condition->token;
          value.property = branch.condition->property;
          value.oper = branch.condition->oper;
          value.valueToken = branch.condition->valueToken;
          value.expectedValue = branch.condition->expectedValue;
          value.flag = branch.condition->flag;
          value.left = condition(branch.condition->left);
          value.right = condition(branch.condition->right);
          value.resolved = branch.condition->resolved;
          copy.condition.emplace(std::move(value));
        }
        if (!stmts(branch.statements, copy.statements)) {
          return nullptr;
        }
        branches.emplace_back(std::move(copy));
      }
      return std::make_shared<ConditionalStmt>(tok(node.directive()),
                                               std::move(branches));
    }
    case StmtKind::ConstexprForStmt: {
      const auto &node = static_cast<const ConstexprForStmt &>(statement);
      StmtPtr body = stmt(node.body());
      StmtPtr binding = stmt(node.elementBinding());
      if (!body || !binding) {
        return nullptr;
      }
      const auto found = result.expressions.find(node.elementInitializer());
      const Call *initializer = found == result.expressions.end()
                                    ? nullptr
                                    : exprAs<Call>(found->second);
      return std::make_shared<ConstexprForStmt>(
          tok(node.constexprKeyword()), tok(node.forKeyword()),
          node.bindingMutability(), type(node.bindingType()),
          tok(node.bindingName()), tok(node.colon()), expr(node.sequence()),
          std::move(body), std::move(binding), initializer);
    }
    case StmtKind::DoWhileStmt: {
      const auto &node = static_cast<const DoWhileStmt &>(statement);
      StmtPtr body = stmt(node.body());
      if (!body) {
        return nullptr;
      }
      return std::make_shared<DoWhileStmt>(std::move(body),
                                           expr(node.condition()));
    }
    case StmtKind::EmptyStmt: {
      const auto &node = static_cast<const EmptyStmt &>(statement);
      return std::make_shared<EmptyStmt>(tok(node.semicolon()));
    }
    case StmtKind::ExpressionStmt: {
      const auto &node = static_cast<const ExpressionStmt &>(statement);
      return std::make_shared<ExpressionStmt>(tok(node.expressionStart()),
                                              expr(node.expression()),
                                              tok(node.discardAttribute()));
    }
    case StmtKind::ForStmt: {
      const auto &node = static_cast<const ForStmt &>(statement);
      StmtPtr initializer = stmt(node.initializer());
      if (node.initializer() && !initializer) {
        return nullptr;
      }
      StmtPtr body = stmt(node.body());
      if (node.body() && !body) {
        return nullptr;
      }
      return std::make_shared<ForStmt>(std::move(initializer),
                                       expr(node.condition()),
                                       expr(node.increment()), std::move(body));
    }
    case StmtKind::IfStmt: {
      const auto &node = static_cast<const IfStmt &>(statement);
      StmtPtr thenBranch = stmt(node.thenBranch());
      if (node.thenBranch() && !thenBranch) {
        return nullptr;
      }
      StmtPtr elseBranch = stmt(node.elseBranch());
      if (node.elseBranch() && !elseBranch) {
        return nullptr;
      }
      return std::make_shared<IfStmt>(
          expr(node.condition()), std::move(thenBranch), std::move(elseBranch),
          tok(node.constexprKeyword()));
    }
    case StmtKind::LoopControlStmt: {
      const auto &node = static_cast<const LoopControlStmt &>(statement);
      return std::make_shared<LoopControlStmt>(tok(node.keyword()));
    }
    case StmtKind::RangeForStmt: {
      const auto &node = static_cast<const RangeForStmt &>(statement);
      StmtPtr lowered = stmt(node.lowered());
      if (!lowered) {
        return nullptr;
      }
      return std::make_shared<RangeForStmt>(
          tok(node.keyword()), node.bindingMutability(),
          type(node.bindingType()), tok(node.bindingName()), tok(node.colon()),
          std::move(lowered), mapped(node.ownerBinding()),
          mapped(node.rangeBinding()), mapped(node.iteratorBinding()),
          mapped(node.sentinelBinding()), mapped(node.elementBinding()),
          mapped(node.coreLoop()), mapped(node.elementOwnerBinding()));
    }
    case StmtKind::ReturnStmt: {
      const auto &node = static_cast<const ReturnStmt &>(statement);
      return std::make_shared<ReturnStmt>(tok(node.keyword()),
                                          expr(node.value()));
    }
    case StmtKind::StaticAssertDecl: {
      const auto &node = static_cast<const StaticAssertDecl &>(statement);
      return std::make_shared<StaticAssertDecl>(
          tok(node.keyword()), tok(node.leftParen()),
          tok(node.conditionStart()), expr(node.condition()), tok(node.comma()),
          tok(node.messageToken()), tok(node.rightParen()),
          tok(node.semicolon()));
    }
    case StmtKind::SwitchStmt: {
      const auto &node = static_cast<const SwitchStmt &>(statement);
      std::vector<SwitchArm> arms;
      arms.reserve(node.arms().size());
      for (const SwitchArm &arm : node.arms()) {
        SwitchArm copy;
        for (const SwitchLabel &label : arm.labels) {
          copy.labels.push_back({.keyword = tok(label.keyword),
                                 .value = expr(label.value),
                                 .colon = tok(label.colon)});
        }
        if (!stmts(arm.statements, copy.statements)) {
          return nullptr;
        }
        arms.emplace_back(std::move(copy));
      }
      return std::make_shared<SwitchStmt>(
          tok(node.keyword()), expr(node.expression()), std::move(arms));
    }
    case StmtKind::StructuredBindingDecl: {
      const auto &node = static_cast<const StructuredBindingDecl &>(statement);
      std::vector<VariableDecl> bindings;
      bindings.reserve(node.bindings().size());
      for (const VariableDecl &binding : node.bindings()) {
        bindings.emplace_back(
            binding.mutability(), type(binding.type()), tok(binding.name()),
            expr(binding.initializer()), tok(binding.staticKeyword()),
            binding.isRangeBinding(), tok(binding.constexprKeyword()),
            binding.isRangeOwner(), binding.isRangeIterator(),
            tok(binding.mutKeyword()), binding.isRangeElementOwner());
      }
      auto copy = std::make_shared<StructuredBindingDecl>(
          tok(node.autoKeyword()), tok(node.leftBracket()), std::move(bindings),
          tok(node.rightBracket()), tok(node.equal()),
          expr(node.initializer()));
      for (std::size_t index = 0; index < node.bindings().size(); ++index) {
        result.statements.emplace(&node.bindings()[index],
                                  &copy->bindings()[index]);
      }
      return copy;
    }
    case StmtKind::VariableDecl:
      return variable(static_cast<const VariableDecl &>(statement));
    case StmtKind::WhileStmt: {
      const auto &node = static_cast<const WhileStmt &>(statement);
      StmtPtr body = stmt(node.body());
      if (!body) {
        return nullptr;
      }
      return std::make_shared<WhileStmt>(expr(node.condition()),
                                         std::move(body));
    }
    case StmtKind::AccessSpecifierDecl:
    case StmtKind::ClassDecl:
    case StmtKind::ConceptDecl:
    case StmtKind::ConformanceDecl:
    case StmtKind::ConstructorDecl:
    case StmtKind::DestructorDecl:
    case StmtKind::EnumDecl:
    case StmtKind::ExternCDecl:
    case StmtKind::FunctionDecl:
    case StmtKind::MutableFieldGroupDecl:
    case StmtKind::NamespaceAliasDecl:
    case StmtKind::NamespaceDecl:
    case StmtKind::TypeAliasDecl:
      return nullptr;
    }
    return nullptr;
  }
};

} // namespace

AstCloneResult cloneStmt(const Stmt &statement, std::uint32_t expansion) {
  Cloner cloner(expansion);
  cloner.result.statement = cloner.stmt(statement);
  return std::move(cloner.result);
}

AstCloneResult cloneStmt(const Stmt &statement, std::uint32_t expansion,
                         const AstCloneOptions &options) {
  Cloner cloner(expansion, &options);
  cloner.result.statement = cloner.stmt(statement);
  return std::move(cloner.result);
}

ExprPtr cloneExpr(const Expr &expression, std::uint32_t expansion) {
  Cloner cloner(expansion);
  return cloner.expr(expression);
}

ExprPtr cloneExpr(const Expr &expression, std::uint32_t expansion,
                  const AstCloneOptions &options) {
  Cloner cloner(expansion, &options);
  return cloner.expr(expression);
}

} // namespace lang
