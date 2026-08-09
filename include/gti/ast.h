#pragma once

#include "gti/target.h"
#include "gti/token.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lang {

struct NamePath {
  explicit NamePath(Token name) { segments.emplace_back(std::move(name)); }
  explicit NamePath(std::vector<Token> segments)
      : segments(std::move(segments)) {}

  [[nodiscard]] const Token &first() const { return segments.front(); }
  [[nodiscard]] const Token &last() const { return segments.back(); }

  std::vector<Token> segments;
};

enum class GenericArgumentSyntax {
  Type,
  Value,
  UnresolvedIdentifier,
};

struct ArrayExtentExpr;
using ArrayExtentExprPtr = std::shared_ptr<const ArrayExtentExpr>;

struct ArrayExtentExpr {
  explicit ArrayExtentExpr(Token atom) : token(std::move(atom)) {}
  ArrayExtentExpr(ArrayExtentExprPtr left, Token oper, ArrayExtentExprPtr right)
      : token(std::move(oper)), left(std::move(left)), right(std::move(right)) {
  }

  [[nodiscard]] bool isAtom() const { return !left && !right; }

  Token token;
  ArrayExtentExprPtr left;
  ArrayExtentExprPtr right;
};

[[nodiscard]] inline std::string
arrayExtentSpelling(const ArrayExtentExpr &expression) {
  if (expression.isAtom()) {
    return expression.token.lexeme;
  }
  if (!expression.left || !expression.right) {
    return expression.token.lexeme;
  }
  return "(" + arrayExtentSpelling(*expression.left) + " " +
         expression.token.lexeme + " " +
         arrayExtentSpelling(*expression.right) + ")";
}

struct TypeRef {
  explicit TypeRef(Token name) : name(std::move(name)) {}
  explicit TypeRef(NamePath name) : name(std::move(name)) {}
  TypeRef(Token name, std::vector<TypeRef> arguments)
      : name(std::move(name)), arguments(std::move(arguments)) {}
  TypeRef(NamePath name, std::vector<TypeRef> arguments)
      : name(std::move(name)), arguments(std::move(arguments)) {}

  NamePath name;
  std::vector<TypeRef> arguments;
  std::vector<ArrayExtentExprPtr> arrayExtents;
  std::optional<Token> reference;
  GenericArgumentSyntax genericArgumentSyntax = GenericArgumentSyntax::Type;
};

enum class Mutability {
  Immutable,
  Mutable,
};

enum class ReceiverMutability {
  ReadOnly,
  Mutable,
};

enum class OverloadedOperator {
  Dereference,
  Arrow,
  PreIncrement,
  Subscript,
  Call,
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  ContextualBool,
};

struct OperatorName {
  OverloadedOperator kind;
  Token keyword;
  Token symbol;
};

[[nodiscard]] inline std::string_view
operatorFunctionName(OverloadedOperator kind) {
  switch (kind) {
  case OverloadedOperator::Dereference:
    return "__gti_operator_dereference";
  case OverloadedOperator::Arrow:
    return "__gti_operator_arrow";
  case OverloadedOperator::PreIncrement:
    return "__gti_operator_pre_increment";
  case OverloadedOperator::Subscript:
    return "__gti_operator_subscript";
  case OverloadedOperator::Call:
    return "__gti_operator_call";
  case OverloadedOperator::Equal:
    return "__gti_operator_equal";
  case OverloadedOperator::NotEqual:
    return "__gti_operator_not_equal";
  case OverloadedOperator::Less:
    return "__gti_operator_less";
  case OverloadedOperator::LessEqual:
    return "__gti_operator_less_equal";
  case OverloadedOperator::Greater:
    return "__gti_operator_greater";
  case OverloadedOperator::GreaterEqual:
    return "__gti_operator_greater_equal";
  case OverloadedOperator::ContextualBool:
    return "__gti_operator_bool";
  }
  return "__gti_operator_unknown";
}

[[nodiscard]] inline std::string_view
operatorSourceSpelling(OverloadedOperator kind) {
  switch (kind) {
  case OverloadedOperator::Dereference:
    return "operator*";
  case OverloadedOperator::Arrow:
    return "operator->";
  case OverloadedOperator::PreIncrement:
    return "operator++";
  case OverloadedOperator::Subscript:
    return "operator[]";
  case OverloadedOperator::Call:
    return "operator()";
  case OverloadedOperator::Equal:
    return "operator==";
  case OverloadedOperator::NotEqual:
    return "operator!=";
  case OverloadedOperator::Less:
    return "operator<";
  case OverloadedOperator::LessEqual:
    return "operator<=";
  case OverloadedOperator::Greater:
    return "operator>";
  case OverloadedOperator::GreaterEqual:
    return "operator>=";
  case OverloadedOperator::ContextualBool:
    return "operator bool";
  }
  return "operator";
}

enum class ClassKind {
  Class,
  Struct,
  Interface,
};

enum class AccessModifier {
  Private,
  Public,
};

struct BaseSpecifier {
  std::optional<Token> access;
  TypeRef type;
};

struct Parameter {
  Parameter(TypeRef type, Token name,
            Mutability mutability = Mutability::Immutable,
            std::optional<Token> pack = std::nullopt)
      : type(std::move(type)), name(std::move(name)), mutability(mutability),
        pack(std::move(pack)) {}

  TypeRef type;
  Token name;
  Mutability mutability;
  std::optional<Token> pack;
};

struct GenericParameter {
  Token name;
  std::optional<Token> pack;
  std::optional<Token> valueType;
  std::optional<NamePath> constraint;
};

struct RuntimeBinding {
  Token attribute;
  std::string name;
};

struct PureSpecifier {
  Token equal;
  Token zero;
};

struct CompileCondition {
  Token propertyToken;
  TargetProperty property;
  Token oper;
  Token valueToken;
  std::string expectedValue;

  [[nodiscard]] bool matches(const TargetInfo &target) const {
    const bool equal = target.value(property) == expectedValue;
    return oper.kind == TokenKind::BANG_EQUAL ? !equal : equal;
  }
};

class Assign;
class ArrayInitializer;
class Binary;
class Call;
class Conversion;
class DirectInitializer;
class DereferenceSet;
class Get;
class Grouping;
class Index;
class IndexSet;
class Lambda;
class LiteralExpr;
class Logical;
class PackExpansion;
class Postfix;
class QualifiedName;
class This;
class Set;
class Unary;
class Unexpected;
class Variable;

class BlockStmt;
class AccessSpecifierDecl;
class ClassDecl;
class CompileErrorDirective;
class ConditionalStmt;
class ConstructorDecl;
class DestructorDecl;
class EmptyStmt;
class EnumDecl;
class ExpressionStmt;
class ForStmt;
class FunctionDecl;
class IfStmt;
class LoopControlStmt;
class NamespaceAliasDecl;
class NamespaceDecl;
class RangeForStmt;
class ReturnStmt;
class SwitchStmt;
class TypeAliasDecl;
class VariableDecl;
class WhileStmt;

class ExprVisitor {
public:
  ExprVisitor() = default;
  ExprVisitor(ExprVisitor &&) = default;
  ExprVisitor(const ExprVisitor &) = default;
  ExprVisitor &operator=(ExprVisitor &&) = default;
  ExprVisitor &operator=(const ExprVisitor &) = default;
  virtual ~ExprVisitor() = default;

  virtual void visitAssignExpr(const Assign &expr) = 0;
  virtual void visitArrayInitializerExpr(const ArrayInitializer &expr) = 0;
  virtual void visitBinaryExpr(const Binary &expr) = 0;
  virtual void visitCallExpr(const Call &expr) = 0;
  virtual void visitConversionExpr(const Conversion &expr) = 0;
  virtual void visitDirectInitializerExpr(const DirectInitializer &expr) = 0;
  virtual void visitDereferenceSetExpr(const DereferenceSet &expr) = 0;
  virtual void visitGetExpr(const Get &expr) = 0;
  virtual void visitGroupingExpr(const Grouping &expr) = 0;
  virtual void visitIndexExpr(const Index &expr) = 0;
  virtual void visitIndexSetExpr(const IndexSet &expr) = 0;
  virtual void visitLambdaExpr(const Lambda &expr) = 0;
  virtual void visitLiteralExpr(const LiteralExpr &expr) = 0;
  virtual void visitLogicalExpr(const Logical &expr) = 0;
  virtual void visitPackExpansionExpr(const PackExpansion &expr) = 0;
  virtual void visitPostfixExpr(const Postfix &expr) = 0;
  virtual void visitQualifiedNameExpr(const QualifiedName &expr) = 0;
  virtual void visitThisExpr(const This &expr) = 0;
  virtual void visitSetExpr(const Set &expr) = 0;
  virtual void visitUnaryExpr(const Unary &expr) = 0;
  virtual void visitUnexpectedExpr(const Unexpected &expr) = 0;
  virtual void visitVariableExpr(const Variable &expr) = 0;
};

class Expr {
public:
  Expr() = default;
  Expr(Expr &&) = default;
  Expr(const Expr &) = delete;
  Expr &operator=(Expr &&) = default;
  Expr &operator=(const Expr &) = delete;
  virtual ~Expr() = default;

  virtual void accept(ExprVisitor &visitor) const = 0;
};

using ExprPtr = std::unique_ptr<Expr>;
using ExprList = std::vector<ExprPtr>;

class StmtVisitor {
public:
  virtual ~StmtVisitor() = default;

  virtual void visitAccessSpecifierDecl(const AccessSpecifierDecl &stmt) = 0;
  virtual void visitBlockStmt(const BlockStmt &stmt) = 0;
  virtual void visitClassDecl(const ClassDecl &stmt) = 0;
  virtual void
  visitCompileErrorDirective(const CompileErrorDirective &stmt) = 0;
  virtual void visitConditionalStmt(const ConditionalStmt &stmt) = 0;
  virtual void visitConstructorDecl(const ConstructorDecl &stmt) = 0;
  virtual void visitDestructorDecl(const DestructorDecl &stmt) = 0;
  virtual void visitEmptyStmt(const EmptyStmt &stmt) = 0;
  virtual void visitEnumDecl(const EnumDecl &stmt) = 0;
  virtual void visitExpressionStmt(const ExpressionStmt &stmt) = 0;
  virtual void visitForStmt(const ForStmt &stmt) = 0;
  virtual void visitFunctionDecl(const FunctionDecl &stmt) = 0;
  virtual void visitIfStmt(const IfStmt &stmt) = 0;
  virtual void visitLoopControlStmt(const LoopControlStmt &stmt) = 0;
  virtual void visitNamespaceAliasDecl(const NamespaceAliasDecl &stmt) = 0;
  virtual void visitNamespaceDecl(const NamespaceDecl &stmt) = 0;
  virtual void visitRangeForStmt(const RangeForStmt &stmt) = 0;
  virtual void visitReturnStmt(const ReturnStmt &stmt) = 0;
  virtual void visitSwitchStmt(const SwitchStmt &stmt) = 0;
  virtual void visitTypeAliasDecl(const TypeAliasDecl &stmt) = 0;
  virtual void visitVariableDecl(const VariableDecl &stmt) = 0;
  virtual void visitWhileStmt(const WhileStmt &stmt) = 0;
};

class Stmt {
public:
  Stmt() = default;
  Stmt(Stmt &&) = default;
  Stmt(const Stmt &) = delete;
  Stmt &operator=(Stmt &&) = default;
  Stmt &operator=(const Stmt &) = delete;
  virtual ~Stmt() = default;

  virtual void accept(StmtVisitor &visitor) const = 0;
};

using StmtPtr = std::unique_ptr<Stmt>;
using StmtList = std::vector<StmtPtr>;

struct ConditionalBranch {
  std::optional<CompileCondition> condition;
  StmtList statements;
};

class Assign final : public Expr {
public:
  Assign(Token name, Token oper, ExprPtr value)
      : Assign(NamePath(std::move(name)), std::move(oper), std::move(value)) {}
  Assign(NamePath path, Token oper, ExprPtr value)
      : path_(std::move(path)), oper_(std::move(oper)),
        value_(std::move(value)) {}
  Assign(Assign &&) = default;
  Assign(const Assign &) = delete;
  Assign &operator=(Assign &&) = default;
  Assign &operator=(const Assign &) = delete;
  ~Assign() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitAssignExpr(*this);
  }

  [[nodiscard]] const NamePath &path() const { return path_; }
  [[nodiscard]] const Token &name() const { return path_.last(); }
  [[nodiscard]] const Token &oper() const { return oper_; }
  [[nodiscard]] const ExprPtr &value() const { return value_; }

private:
  NamePath path_;
  Token oper_;
  ExprPtr value_;
};

class ArrayInitializer final : public Expr {
public:
  ArrayInitializer(Token brace, ExprList elements, Token closingBrace)
      : brace_(std::move(brace)), elements_(std::move(elements)),
        closingBrace_(std::move(closingBrace)) {}
  ArrayInitializer(ArrayInitializer &&) = default;
  ArrayInitializer(const ArrayInitializer &) = delete;
  ArrayInitializer &operator=(ArrayInitializer &&) = default;
  ArrayInitializer &operator=(const ArrayInitializer &) = delete;
  ~ArrayInitializer() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitArrayInitializerExpr(*this);
  }

  [[nodiscard]] const Token &brace() const { return brace_; }
  [[nodiscard]] const ExprList &elements() const { return elements_; }
  [[nodiscard]] const Token &closingBrace() const { return closingBrace_; }

private:
  Token brace_;
  ExprList elements_;
  Token closingBrace_;
};

class Binary final : public Expr {
public:
  Binary(ExprPtr left, Token oper, ExprPtr right)
      : left_(std::move(left)), oper_(std::move(oper)),
        right_(std::move(right)) {}
  Binary(Binary &&) = default;
  Binary(const Binary &) = delete;
  Binary &operator=(Binary &&) = default;
  Binary &operator=(const Binary &) = delete;
  ~Binary() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitBinaryExpr(*this);
  }

  [[nodiscard]] const ExprPtr &left() const { return left_; }
  [[nodiscard]] const Token &oper() const { return oper_; }
  [[nodiscard]] const ExprPtr &right() const { return right_; }

private:
  ExprPtr left_;
  Token oper_;
  ExprPtr right_;
};

class Call final : public Expr {
public:
  Call(ExprPtr callee, std::vector<TypeRef> typeArguments, Token paren,
       ExprList arguments)
      : callee_(std::move(callee)), typeArguments_(std::move(typeArguments)),
        paren_(std::move(paren)), arguments_(std::move(arguments)) {}
  Call(Call &&) = default;
  Call(const Call &) = delete;
  Call &operator=(Call &&) = default;
  Call &operator=(const Call &) = delete;
  ~Call() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitCallExpr(*this);
  }

  [[nodiscard]] const ExprPtr &callee() const { return callee_; }
  [[nodiscard]] const std::vector<TypeRef> &typeArguments() const {
    return typeArguments_;
  }
  [[nodiscard]] const Token &paren() const { return paren_; }
  [[nodiscard]] const ExprList &arguments() const { return arguments_; }

private:
  ExprPtr callee_;
  std::vector<TypeRef> typeArguments_;
  Token paren_;
  ExprList arguments_;
};

class Conversion final : public Expr {
public:
  Conversion(TypeRef targetType, Token paren, ExprPtr value)
      : targetType_(std::move(targetType)), paren_(std::move(paren)),
        value_(std::move(value)) {}
  Conversion(Conversion &&) = default;
  Conversion(const Conversion &) = delete;
  Conversion &operator=(Conversion &&) = default;
  Conversion &operator=(const Conversion &) = delete;
  ~Conversion() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitConversionExpr(*this);
  }

  [[nodiscard]] const TypeRef &targetType() const { return targetType_; }
  [[nodiscard]] const Token &paren() const { return paren_; }
  [[nodiscard]] const ExprPtr &value() const { return value_; }

private:
  TypeRef targetType_;
  Token paren_;
  ExprPtr value_;
};

class DirectInitializer final : public Expr {
public:
  DirectInitializer(Token brace, ExprList arguments, Token closingBrace)
      : brace_(std::move(brace)), arguments_(std::move(arguments)),
        closingBrace_(std::move(closingBrace)) {}
  DirectInitializer(DirectInitializer &&) = default;
  DirectInitializer(const DirectInitializer &) = delete;
  DirectInitializer &operator=(DirectInitializer &&) = default;
  DirectInitializer &operator=(const DirectInitializer &) = delete;
  ~DirectInitializer() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitDirectInitializerExpr(*this);
  }

  [[nodiscard]] const Token &brace() const { return brace_; }
  [[nodiscard]] const ExprList &arguments() const { return arguments_; }
  [[nodiscard]] const Token &closingBrace() const { return closingBrace_; }

private:
  Token brace_;
  ExprList arguments_;
  Token closingBrace_;
};

class PackExpansion final : public Expr {
public:
  PackExpansion(Token name, Token ellipsis)
      : name_(std::move(name)), ellipsis_(std::move(ellipsis)) {}
  PackExpansion(PackExpansion &&) = default;
  PackExpansion(const PackExpansion &) = delete;
  PackExpansion &operator=(PackExpansion &&) = default;
  PackExpansion &operator=(const PackExpansion &) = delete;
  ~PackExpansion() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitPackExpansionExpr(*this);
  }

  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const Token &ellipsis() const { return ellipsis_; }

private:
  Token name_;
  Token ellipsis_;
};

class DereferenceSet final : public Expr {
public:
  DereferenceSet(Token dereference, ExprPtr object, Token oper, ExprPtr value)
      : dereference_(std::move(dereference)), object_(std::move(object)),
        oper_(std::move(oper)), value_(std::move(value)) {}

  void accept(ExprVisitor &visitor) const override {
    visitor.visitDereferenceSetExpr(*this);
  }

  [[nodiscard]] const Token &dereference() const { return dereference_; }
  [[nodiscard]] const ExprPtr &object() const { return object_; }
  [[nodiscard]] const Token &oper() const { return oper_; }
  [[nodiscard]] const ExprPtr &value() const { return value_; }

private:
  Token dereference_;
  ExprPtr object_;
  Token oper_;
  ExprPtr value_;
};

class Get final : public Expr {
public:
  Get(ExprPtr object, Token access, Token name)
      : object_(std::move(object)), access_(std::move(access)),
        name_(std::move(name)) {}
  Get(Get &&) = default;
  Get(const Get &) = delete;
  Get &operator=(Get &&) = default;
  Get &operator=(const Get &) = delete;
  ~Get() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitGetExpr(*this);
  }

  [[nodiscard]] const ExprPtr &object() const { return object_; }
  [[nodiscard]] const Token &access() const { return access_; }
  [[nodiscard]] const Token &name() const { return name_; }
  ExprPtr takeObject() { return std::move(object_); }

private:
  ExprPtr object_;
  Token access_;
  Token name_;
};

class Grouping final : public Expr {
public:
  explicit Grouping(ExprPtr expression) : expression_(std::move(expression)) {}
  Grouping(Grouping &&) = default;
  Grouping(const Grouping &) = delete;
  Grouping &operator=(Grouping &&) = default;
  Grouping &operator=(const Grouping &) = delete;
  ~Grouping() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitGroupingExpr(*this);
  }

  [[nodiscard]] const ExprPtr &expression() const { return expression_; }

private:
  ExprPtr expression_;
};

class Index final : public Expr {
public:
  Index(ExprPtr object, Token bracket, ExprPtr index)
      : object_(std::move(object)), bracket_(std::move(bracket)),
        index_(std::move(index)) {}
  Index(Index &&) = default;
  Index(const Index &) = delete;
  Index &operator=(Index &&) = default;
  Index &operator=(const Index &) = delete;
  ~Index() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitIndexExpr(*this);
  }

  [[nodiscard]] const ExprPtr &object() const { return object_; }
  [[nodiscard]] const Token &bracket() const { return bracket_; }
  [[nodiscard]] const ExprPtr &index() const { return index_; }
  ExprPtr takeObject() { return std::move(object_); }
  ExprPtr takeIndex() { return std::move(index_); }

private:
  ExprPtr object_;
  Token bracket_;
  ExprPtr index_;
};

class IndexSet final : public Expr {
public:
  IndexSet(ExprPtr object, Token bracket, ExprPtr index, Token oper,
           ExprPtr value)
      : object_(std::move(object)), bracket_(std::move(bracket)),
        index_(std::move(index)), oper_(std::move(oper)),
        value_(std::move(value)) {}
  IndexSet(IndexSet &&) = default;
  IndexSet(const IndexSet &) = delete;
  IndexSet &operator=(IndexSet &&) = default;
  IndexSet &operator=(const IndexSet &) = delete;
  ~IndexSet() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitIndexSetExpr(*this);
  }

  [[nodiscard]] const ExprPtr &object() const { return object_; }
  [[nodiscard]] const Token &bracket() const { return bracket_; }
  [[nodiscard]] const ExprPtr &index() const { return index_; }
  [[nodiscard]] const Token &oper() const { return oper_; }
  [[nodiscard]] const ExprPtr &value() const { return value_; }

private:
  ExprPtr object_;
  Token bracket_;
  ExprPtr index_;
  Token oper_;
  ExprPtr value_;
};

struct LambdaCapture {
  Token name;
};

class Lambda final : public Expr {
public:
  Lambda(Token bracket, std::vector<LambdaCapture> captures,
         std::vector<Parameter> parameters, Token arrow, TypeRef returnType,
         StmtList body)
      : bracket_(std::move(bracket)), captures_(std::move(captures)),
        parameters_(std::move(parameters)), arrow_(std::move(arrow)),
        returnType_(std::move(returnType)), body_(std::move(body)) {}
  Lambda(Lambda &&) = default;
  Lambda(const Lambda &) = delete;
  Lambda &operator=(Lambda &&) = default;
  Lambda &operator=(const Lambda &) = delete;
  ~Lambda() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitLambdaExpr(*this);
  }

  [[nodiscard]] const Token &bracket() const { return bracket_; }
  [[nodiscard]] const std::vector<LambdaCapture> &captures() const {
    return captures_;
  }
  [[nodiscard]] const std::vector<Parameter> &parameters() const {
    return parameters_;
  }
  [[nodiscard]] const Token &arrow() const { return arrow_; }
  [[nodiscard]] const TypeRef &returnType() const { return returnType_; }
  [[nodiscard]] const StmtList &body() const { return body_; }

private:
  Token bracket_;
  std::vector<LambdaCapture> captures_;
  std::vector<Parameter> parameters_;
  Token arrow_;
  TypeRef returnType_;
  StmtList body_;
};

class LiteralExpr final : public Expr {
public:
  explicit LiteralExpr(Literal value) : value_(std::move(value)) {}
  LiteralExpr(Token token, Literal value)
      : token_(std::move(token)), value_(std::move(value)) {}
  LiteralExpr(LiteralExpr &&) = default;
  LiteralExpr(const LiteralExpr &) = delete;
  LiteralExpr &operator=(LiteralExpr &&) = default;
  LiteralExpr &operator=(const LiteralExpr &) = delete;
  ~LiteralExpr() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitLiteralExpr(*this);
  }

  [[nodiscard]] const Token &token() const { return token_; }
  [[nodiscard]] const Literal &value() const { return value_; }

private:
  Token token_;
  Literal value_;
};

class Logical final : public Expr {
public:
  Logical(ExprPtr left, Token oper, ExprPtr right)
      : left_(std::move(left)), oper_(std::move(oper)),
        right_(std::move(right)) {}
  Logical(Logical &&) = default;
  Logical(const Logical &) = delete;
  Logical &operator=(Logical &&) = default;
  Logical &operator=(const Logical &) = delete;
  ~Logical() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitLogicalExpr(*this);
  }

  [[nodiscard]] const ExprPtr &left() const { return left_; }
  [[nodiscard]] const Token &oper() const { return oper_; }
  [[nodiscard]] const ExprPtr &right() const { return right_; }
  ExprPtr takeRight() { return std::move(right_); }

private:
  ExprPtr left_;
  Token oper_;
  ExprPtr right_;
};

class Postfix final : public Expr {
public:
  Postfix(ExprPtr expression, Token oper)
      : expression_(std::move(expression)), oper_(std::move(oper)) {}
  Postfix(Postfix &&) = default;
  Postfix(const Postfix &) = delete;
  Postfix &operator=(Postfix &&) = default;
  Postfix &operator=(const Postfix &) = delete;
  ~Postfix() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitPostfixExpr(*this);
  }

  [[nodiscard]] const ExprPtr &expression() const { return expression_; }
  [[nodiscard]] const Token &oper() const { return oper_; }

private:
  ExprPtr expression_;
  Token oper_;
};

class QualifiedName final : public Expr {
public:
  explicit QualifiedName(NamePath name) : name_(std::move(name)) {}

  void accept(ExprVisitor &visitor) const override {
    visitor.visitQualifiedNameExpr(*this);
  }

  [[nodiscard]] const NamePath &name() const { return name_; }
  [[nodiscard]] NamePath takeName() { return std::move(name_); }

private:
  NamePath name_;
};

class This final : public Expr {
public:
  explicit This(Token keyword) : keyword_(std::move(keyword)) {}
  This(This &&) = default;
  This(const This &) = delete;
  This &operator=(This &&) = default;
  This &operator=(const This &) = delete;
  ~This() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitThisExpr(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }

private:
  Token keyword_;
};

class Set final : public Expr {
public:
  Set(ExprPtr object, Token access, Token name, Token oper, ExprPtr value)
      : object_(std::move(object)), access_(std::move(access)),
        name_(std::move(name)), oper_(std::move(oper)),
        value_(std::move(value)) {}
  Set(Set &&) = default;
  Set(const Set &) = delete;
  Set &operator=(Set &&) = default;
  Set &operator=(const Set &) = delete;
  ~Set() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitSetExpr(*this);
  }

  [[nodiscard]] const ExprPtr &object() const { return object_; }
  [[nodiscard]] const Token &access() const { return access_; }
  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const Token &oper() const { return oper_; }
  [[nodiscard]] const ExprPtr &value() const { return value_; }

private:
  ExprPtr object_;
  Token access_;
  Token name_;
  Token oper_;
  ExprPtr value_;
};

class Unary final : public Expr {
public:
  Unary(Token oper, ExprPtr right)
      : oper_(std::move(oper)), right_(std::move(right)) {}
  Unary(Unary &&) = default;
  Unary(const Unary &) = delete;
  Unary &operator=(Unary &&) = default;
  Unary &operator=(const Unary &) = delete;
  ~Unary() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitUnaryExpr(*this);
  }

  [[nodiscard]] const Token &oper() const { return oper_; }
  [[nodiscard]] const ExprPtr &right() const { return right_; }
  [[nodiscard]] ExprPtr takeRight() { return std::move(right_); }

private:
  Token oper_;
  ExprPtr right_;
};

class Unexpected final : public Expr {
public:
  Unexpected(Token keyword, ExprPtr error)
      : keyword_(std::move(keyword)), error_(std::move(error)) {}
  Unexpected(Unexpected &&) = default;
  Unexpected(const Unexpected &) = delete;
  Unexpected &operator=(Unexpected &&) = default;
  Unexpected &operator=(const Unexpected &) = delete;
  ~Unexpected() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitUnexpectedExpr(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }
  [[nodiscard]] const ExprPtr &error() const { return error_; }

private:
  Token keyword_;
  ExprPtr error_;
};

class Variable final : public Expr {
public:
  explicit Variable(Token name) : name_(std::move(name)) {}
  Variable(Variable &&) = default;
  Variable(const Variable &) = delete;
  Variable &operator=(Variable &&) = default;
  Variable &operator=(const Variable &) = delete;
  ~Variable() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitVariableExpr(*this);
  }

  [[nodiscard]] const Token &name() const { return name_; }

private:
  Token name_;
};

class BlockStmt final : public Stmt {
public:
  explicit BlockStmt(StmtList statements)
      : statements_(std::move(statements)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitBlockStmt(*this);
  }

  [[nodiscard]] const StmtList &statements() const { return statements_; }

private:
  StmtList statements_;
};

class AccessSpecifierDecl final : public Stmt {
public:
  AccessSpecifierDecl(Token keyword, AccessModifier modifier)
      : keyword_(std::move(keyword)), modifier_(modifier) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitAccessSpecifierDecl(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }
  [[nodiscard]] AccessModifier modifier() const { return modifier_; }

private:
  Token keyword_;
  AccessModifier modifier_;
};

struct ConstructorInitializer {
  TypeRef target;
  ExprList arguments;
};

enum class SpecialMemberSpecifierKind {
  Defaulted,
  Deleted,
};

struct SpecialMemberSpecifier {
  Token equal;
  Token keyword;
  SpecialMemberSpecifierKind kind = SpecialMemberSpecifierKind::Defaulted;
};

class ConstructorDecl final : public Stmt {
public:
  ConstructorDecl(Token name, std::vector<Parameter> parameters,
                  std::vector<ConstructorInitializer> initializers,
                  std::optional<SpecialMemberSpecifier> specifier,
                  std::unique_ptr<BlockStmt> body)
      : name_(std::move(name)), parameters_(std::move(parameters)),
        initializers_(std::move(initializers)),
        specifier_(std::move(specifier)), body_(std::move(body)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitConstructorDecl(*this);
  }

  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const std::vector<Parameter> &parameters() const {
    return parameters_;
  }
  [[nodiscard]] const std::vector<ConstructorInitializer> &
  initializers() const {
    return initializers_;
  }
  [[nodiscard]] const std::optional<SpecialMemberSpecifier> &specifier() const {
    return specifier_;
  }
  [[nodiscard]] const std::unique_ptr<BlockStmt> &body() const { return body_; }

private:
  Token name_;
  std::vector<Parameter> parameters_;
  std::vector<ConstructorInitializer> initializers_;
  std::optional<SpecialMemberSpecifier> specifier_;
  std::unique_ptr<BlockStmt> body_;
};

class DestructorDecl final : public Stmt {
public:
  DestructorDecl(Token tilde, Token name, std::unique_ptr<BlockStmt> body)
      : tilde_(std::move(tilde)), name_(std::move(name)),
        body_(std::move(body)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitDestructorDecl(*this);
  }

  [[nodiscard]] const Token &tilde() const { return tilde_; }
  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const std::unique_ptr<BlockStmt> &body() const { return body_; }

private:
  Token tilde_;
  Token name_;
  std::unique_ptr<BlockStmt> body_;
};

class ClassDecl final : public Stmt {
public:
  ClassDecl(Token keyword, ClassKind kind, Token name,
            std::vector<GenericParameter> genericParameters,
            std::vector<BaseSpecifier> bases, StmtList members)
      : keyword_(std::move(keyword)), kind_(kind), name_(std::move(name)),
        genericParameters_(std::move(genericParameters)),
        bases_(std::move(bases)), members_(std::move(members)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitClassDecl(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }
  [[nodiscard]] ClassKind kind() const { return kind_; }
  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const std::vector<GenericParameter> &genericParameters() const {
    return genericParameters_;
  }
  [[nodiscard]] const std::vector<BaseSpecifier> &bases() const {
    return bases_;
  }
  [[nodiscard]] const StmtList &members() const { return members_; }

private:
  Token keyword_;
  ClassKind kind_;
  Token name_;
  std::vector<GenericParameter> genericParameters_;
  std::vector<BaseSpecifier> bases_;
  StmtList members_;
};

struct EnumeratorDecl {
  Token name;
  ExprPtr initializer;
};

class EnumDecl final : public Stmt {
public:
  EnumDecl(Token keyword, Token classKeyword, Token name,
           std::optional<TypeRef> underlyingType,
           std::vector<EnumeratorDecl> enumerators)
      : keyword_(std::move(keyword)), classKeyword_(std::move(classKeyword)),
        name_(std::move(name)), underlyingType_(std::move(underlyingType)),
        enumerators_(std::move(enumerators)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitEnumDecl(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }
  [[nodiscard]] const Token &classKeyword() const { return classKeyword_; }
  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const std::optional<TypeRef> &underlyingType() const {
    return underlyingType_;
  }
  [[nodiscard]] const std::vector<EnumeratorDecl> &enumerators() const {
    return enumerators_;
  }

private:
  Token keyword_;
  Token classKeyword_;
  Token name_;
  std::optional<TypeRef> underlyingType_;
  std::vector<EnumeratorDecl> enumerators_;
};

class ConditionalStmt final : public Stmt {
public:
  ConditionalStmt(Token directive, std::vector<ConditionalBranch> branches)
      : directive_(std::move(directive)), branches_(std::move(branches)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitConditionalStmt(*this);
  }

  [[nodiscard]] const Token &directive() const { return directive_; }
  [[nodiscard]] const std::vector<ConditionalBranch> &branches() const {
    return branches_;
  }

  [[nodiscard]] const StmtList *activeBranch(const TargetInfo &target) const {
    for (const ConditionalBranch &branch : branches_) {
      if (!branch.condition || branch.condition->matches(target)) {
        return &branch.statements;
      }
    }
    return nullptr;
  }

private:
  Token directive_;
  std::vector<ConditionalBranch> branches_;
};

class CompileErrorDirective final : public Stmt {
public:
  CompileErrorDirective(Token directive, Token message)
      : directive_(std::move(directive)), message_(std::move(message)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitCompileErrorDirective(*this);
  }

  [[nodiscard]] const Token &directive() const { return directive_; }
  [[nodiscard]] const Token &messageToken() const { return message_; }
  [[nodiscard]] const std::string &message() const {
    if (const auto *text = std::get_if<std::string>(&message_.literal)) {
      return *text;
    }
    static const std::string empty;
    return empty;
  }

private:
  Token directive_;
  Token message_;
};

class EmptyStmt final : public Stmt {
public:
  explicit EmptyStmt(Token semicolon) : semicolon_(std::move(semicolon)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitEmptyStmt(*this);
  }

  [[nodiscard]] const Token &semicolon() const { return semicolon_; }

private:
  Token semicolon_;
};

class ExpressionStmt final : public Stmt {
public:
  explicit ExpressionStmt(ExprPtr expression,
                          std::optional<Token> discardAttribute = std::nullopt)
      : expression_(std::move(expression)),
        discardAttribute_(std::move(discardAttribute)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitExpressionStmt(*this);
  }

  [[nodiscard]] const ExprPtr &expression() const { return expression_; }
  [[nodiscard]] const std::optional<Token> &discardAttribute() const {
    return discardAttribute_;
  }

private:
  ExprPtr expression_;
  std::optional<Token> discardAttribute_;
};

class ForStmt final : public Stmt {
public:
  ForStmt(StmtPtr initializer, ExprPtr condition, ExprPtr increment,
          StmtPtr body)
      : initializer_(std::move(initializer)), condition_(std::move(condition)),
        increment_(std::move(increment)), body_(std::move(body)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitForStmt(*this);
  }

  [[nodiscard]] const StmtPtr &initializer() const { return initializer_; }
  [[nodiscard]] const ExprPtr &condition() const { return condition_; }
  [[nodiscard]] const ExprPtr &increment() const { return increment_; }
  [[nodiscard]] const StmtPtr &body() const { return body_; }

private:
  StmtPtr initializer_;
  ExprPtr condition_;
  ExprPtr increment_;
  StmtPtr body_;
};

class RangeForStmt final : public Stmt {
public:
  RangeForStmt(Token keyword, Mutability bindingMutability, TypeRef bindingType,
               Token bindingName, Token colon, StmtPtr lowered)
      : keyword_(std::move(keyword)), bindingMutability_(bindingMutability),
        bindingType_(std::move(bindingType)),
        bindingName_(std::move(bindingName)), colon_(std::move(colon)),
        lowered_(std::move(lowered)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitRangeForStmt(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }
  [[nodiscard]] Mutability bindingMutability() const {
    return bindingMutability_;
  }
  [[nodiscard]] const TypeRef &bindingType() const { return bindingType_; }
  [[nodiscard]] const Token &bindingName() const { return bindingName_; }
  [[nodiscard]] const Token &colon() const { return colon_; }
  [[nodiscard]] const StmtPtr &lowered() const { return lowered_; }

private:
  Token keyword_;
  Mutability bindingMutability_;
  TypeRef bindingType_;
  Token bindingName_;
  Token colon_;
  StmtPtr lowered_;
};

class FunctionDecl final : public Stmt {
public:
  FunctionDecl(
      TypeRef returnType, Token name,
      std::vector<GenericParameter> genericParameters,
      std::vector<Parameter> parameters, std::unique_ptr<BlockStmt> body,
      std::optional<RuntimeBinding> runtimeBinding = std::nullopt,
      ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly,
      Mutability returnMutability = Mutability::Immutable,
      std::optional<OperatorName> operatorName = std::nullopt,
      std::optional<Token> staticKeyword = std::nullopt,
      std::optional<Token> virtualKeyword = std::nullopt,
      std::optional<Token> overrideKeyword = std::nullopt,
      std::optional<PureSpecifier> pureSpecifier = std::nullopt)
      : returnType_(std::move(returnType)), name_(std::move(name)),
        genericParameters_(std::move(genericParameters)),
        parameters_(std::move(parameters)), body_(std::move(body)),
        runtimeBinding_(std::move(runtimeBinding)),
        receiverMutability_(receiverMutability),
        returnMutability_(returnMutability),
        operatorName_(std::move(operatorName)),
        staticKeyword_(std::move(staticKeyword)),
        virtualKeyword_(std::move(virtualKeyword)),
        overrideKeyword_(std::move(overrideKeyword)),
        pureSpecifier_(std::move(pureSpecifier)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitFunctionDecl(*this);
  }

  [[nodiscard]] const TypeRef &returnType() const { return returnType_; }
  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const std::vector<GenericParameter> &genericParameters() const {
    return genericParameters_;
  }
  [[nodiscard]] const std::vector<Parameter> &parameters() const {
    return parameters_;
  }
  [[nodiscard]] const std::unique_ptr<BlockStmt> &body() const { return body_; }
  [[nodiscard]] const std::optional<RuntimeBinding> &runtimeBinding() const {
    return runtimeBinding_;
  }
  [[nodiscard]] ReceiverMutability receiverMutability() const {
    return receiverMutability_;
  }
  [[nodiscard]] Mutability returnMutability() const {
    return returnMutability_;
  }
  [[nodiscard]] const std::optional<OperatorName> &operatorName() const {
    return operatorName_;
  }
  [[nodiscard]] bool isStatic() const { return staticKeyword_.has_value(); }
  [[nodiscard]] const std::optional<Token> &staticKeyword() const {
    return staticKeyword_;
  }
  [[nodiscard]] bool isVirtual() const { return virtualKeyword_.has_value(); }
  [[nodiscard]] const std::optional<Token> &virtualKeyword() const {
    return virtualKeyword_;
  }
  [[nodiscard]] bool isOverride() const { return overrideKeyword_.has_value(); }
  [[nodiscard]] const std::optional<Token> &overrideKeyword() const {
    return overrideKeyword_;
  }
  [[nodiscard]] bool isPure() const { return pureSpecifier_.has_value(); }
  [[nodiscard]] const std::optional<PureSpecifier> &pureSpecifier() const {
    return pureSpecifier_;
  }

private:
  TypeRef returnType_;
  Token name_;
  std::vector<GenericParameter> genericParameters_;
  std::vector<Parameter> parameters_;
  std::unique_ptr<BlockStmt> body_;
  std::optional<RuntimeBinding> runtimeBinding_;
  ReceiverMutability receiverMutability_;
  Mutability returnMutability_;
  std::optional<OperatorName> operatorName_;
  std::optional<Token> staticKeyword_;
  std::optional<Token> virtualKeyword_;
  std::optional<Token> overrideKeyword_;
  std::optional<PureSpecifier> pureSpecifier_;
};

class IfStmt final : public Stmt {
public:
  IfStmt(ExprPtr condition, StmtPtr thenBranch, StmtPtr elseBranch)
      : condition_(std::move(condition)), thenBranch_(std::move(thenBranch)),
        elseBranch_(std::move(elseBranch)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitIfStmt(*this);
  }

  [[nodiscard]] const ExprPtr &condition() const { return condition_; }
  [[nodiscard]] const StmtPtr &thenBranch() const { return thenBranch_; }
  [[nodiscard]] const StmtPtr &elseBranch() const { return elseBranch_; }

private:
  ExprPtr condition_;
  StmtPtr thenBranch_;
  StmtPtr elseBranch_;
};

class LoopControlStmt final : public Stmt {
public:
  explicit LoopControlStmt(Token keyword) : keyword_(std::move(keyword)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitLoopControlStmt(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }

private:
  Token keyword_;
};

class NamespaceAliasDecl final : public Stmt {
public:
  NamespaceAliasDecl(Token name, NamePath target)
      : name_(std::move(name)), target_(std::move(target)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitNamespaceAliasDecl(*this);
  }

  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const NamePath &target() const { return target_; }

private:
  Token name_;
  NamePath target_;
};

class NamespaceDecl final : public Stmt {
public:
  NamespaceDecl(Token name, StmtList declarations)
      : name_(std::move(name)), declarations_(std::move(declarations)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitNamespaceDecl(*this);
  }

  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const StmtList &declarations() const { return declarations_; }

private:
  Token name_;
  StmtList declarations_;
};

class ReturnStmt final : public Stmt {
public:
  ReturnStmt(Token keyword, ExprPtr value)
      : keyword_(std::move(keyword)), value_(std::move(value)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitReturnStmt(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }
  [[nodiscard]] const ExprPtr &value() const { return value_; }

private:
  Token keyword_;
  ExprPtr value_;
};

struct SwitchLabel {
  Token keyword;
  ExprPtr value;
  Token colon;

  [[nodiscard]] bool isDefault() const {
    return keyword.kind == TokenKind::DEFAULT;
  }
};

struct SwitchArm {
  std::vector<SwitchLabel> labels;
  StmtList statements;
};

class SwitchStmt final : public Stmt {
public:
  SwitchStmt(Token keyword, ExprPtr expression, std::vector<SwitchArm> arms)
      : keyword_(std::move(keyword)), expression_(std::move(expression)),
        arms_(std::move(arms)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitSwitchStmt(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }
  [[nodiscard]] const ExprPtr &expression() const { return expression_; }
  [[nodiscard]] const std::vector<SwitchArm> &arms() const { return arms_; }

private:
  Token keyword_;
  ExprPtr expression_;
  std::vector<SwitchArm> arms_;
};

class TypeAliasDecl final : public Stmt {
public:
  TypeAliasDecl(Token keyword, Token name, TypeRef target)
      : keyword_(std::move(keyword)), name_(std::move(name)),
        target_(std::move(target)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitTypeAliasDecl(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }
  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const TypeRef &target() const { return target_; }

private:
  Token keyword_;
  Token name_;
  TypeRef target_;
};

class VariableDecl final : public Stmt {
public:
  VariableDecl(Mutability mutability, TypeRef type, Token name,
               ExprPtr initializer,
               std::optional<Token> staticKeyword = std::nullopt,
               bool rangeBinding = false)
      : mutability_(mutability), type_(std::move(type)), name_(std::move(name)),
        initializer_(std::move(initializer)),
        staticKeyword_(std::move(staticKeyword)), rangeBinding_(rangeBinding) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitVariableDecl(*this);
  }

  [[nodiscard]] Mutability mutability() const { return mutability_; }
  [[nodiscard]] bool isMutable() const {
    return mutability_ == Mutability::Mutable;
  }
  [[nodiscard]] const TypeRef &type() const { return type_; }
  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const ExprPtr &initializer() const { return initializer_; }
  [[nodiscard]] bool isStatic() const { return staticKeyword_.has_value(); }
  [[nodiscard]] const std::optional<Token> &staticKeyword() const {
    return staticKeyword_;
  }
  [[nodiscard]] bool isRangeBinding() const { return rangeBinding_; }

private:
  Mutability mutability_;
  TypeRef type_;
  Token name_;
  ExprPtr initializer_;
  std::optional<Token> staticKeyword_;
  bool rangeBinding_ = false;
};

class WhileStmt final : public Stmt {
public:
  WhileStmt(ExprPtr condition, StmtPtr body)
      : condition_(std::move(condition)), body_(std::move(body)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitWhileStmt(*this);
  }

  [[nodiscard]] const ExprPtr &condition() const { return condition_; }
  [[nodiscard]] const StmtPtr &body() const { return body_; }

private:
  ExprPtr condition_;
  StmtPtr body_;
};

class Program {
public:
  explicit Program(StmtList declarations = {})
      : declarations_(std::move(declarations)) {}

  [[nodiscard]] const StmtList &declarations() const { return declarations_; }

  [[nodiscard]] StmtList takeDeclarations() { return std::move(declarations_); }

private:
  StmtList declarations_;
};

} // namespace lang
