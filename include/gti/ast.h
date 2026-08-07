#pragma once

#include "gti/target.h"
#include "gti/token.h"

#include <memory>
#include <optional>
#include <string>
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

struct TypeRef {
  explicit TypeRef(Token name) : name(std::move(name)) {}
  explicit TypeRef(NamePath name) : name(std::move(name)) {}
  TypeRef(Token name, std::vector<TypeRef> arguments)
      : name(std::move(name)), arguments(std::move(arguments)) {}
  TypeRef(NamePath name, std::vector<TypeRef> arguments)
      : name(std::move(name)), arguments(std::move(arguments)) {}

  NamePath name;
  std::vector<TypeRef> arguments;
  std::vector<Token> arrayExtents;
  std::optional<Token> reference;
};

enum class Mutability {
  Immutable,
  Mutable,
};

enum class ReceiverMutability {
  ReadOnly,
  Mutable,
};

enum class ClassKind {
  Class,
  Struct,
};

enum class AccessModifier {
  Private,
  Public,
};

struct Parameter {
  Parameter(TypeRef type, Token name,
            Mutability mutability = Mutability::Immutable)
      : type(std::move(type)), name(std::move(name)), mutability(mutability) {}

  TypeRef type;
  Token name;
  Mutability mutability;
};

struct GenericParameter {
  Token name;
};

struct RuntimeBinding {
  Token attribute;
  std::string name;
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
class Get;
class Grouping;
class Index;
class IndexSet;
class LiteralExpr;
class Logical;
class Postfix;
class QualifiedName;
class Self;
class Set;
class Unary;
class Unexpected;
class Variable;

class BlockStmt;
class AccessSpecifierDecl;
class ClassDecl;
class ConditionalStmt;
class ConstructorDecl;
class DestructorDecl;
class EmptyStmt;
class ExpressionStmt;
class ForStmt;
class FunctionDecl;
class IfStmt;
class LoopControlStmt;
class NamespaceAliasDecl;
class NamespaceDecl;
class ReturnStmt;
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
  virtual void visitGetExpr(const Get &expr) = 0;
  virtual void visitGroupingExpr(const Grouping &expr) = 0;
  virtual void visitIndexExpr(const Index &expr) = 0;
  virtual void visitIndexSetExpr(const IndexSet &expr) = 0;
  virtual void visitLiteralExpr(const LiteralExpr &expr) = 0;
  virtual void visitLogicalExpr(const Logical &expr) = 0;
  virtual void visitPostfixExpr(const Postfix &expr) = 0;
  virtual void visitQualifiedNameExpr(const QualifiedName &expr) = 0;
  virtual void visitSelfExpr(const Self &expr) = 0;
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
  virtual void visitConditionalStmt(const ConditionalStmt &stmt) = 0;
  virtual void visitConstructorDecl(const ConstructorDecl &stmt) = 0;
  virtual void visitDestructorDecl(const DestructorDecl &stmt) = 0;
  virtual void visitEmptyStmt(const EmptyStmt &stmt) = 0;
  virtual void visitExpressionStmt(const ExpressionStmt &stmt) = 0;
  virtual void visitForStmt(const ForStmt &stmt) = 0;
  virtual void visitFunctionDecl(const FunctionDecl &stmt) = 0;
  virtual void visitIfStmt(const IfStmt &stmt) = 0;
  virtual void visitLoopControlStmt(const LoopControlStmt &stmt) = 0;
  virtual void visitNamespaceAliasDecl(const NamespaceAliasDecl &stmt) = 0;
  virtual void visitNamespaceDecl(const NamespaceDecl &stmt) = 0;
  virtual void visitReturnStmt(const ReturnStmt &stmt) = 0;
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
      : name_(std::move(name)), oper_(std::move(oper)),
        value_(std::move(value)) {}
  Assign(Assign &&) = default;
  Assign(const Assign &) = delete;
  Assign &operator=(Assign &&) = default;
  Assign &operator=(const Assign &) = delete;
  ~Assign() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitAssignExpr(*this);
  }

  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const Token &oper() const { return oper_; }
  [[nodiscard]] const ExprPtr &value() const { return value_; }

private:
  Token name_;
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

private:
  NamePath name_;
};

class Self final : public Expr {
public:
  explicit Self(Token keyword) : keyword_(std::move(keyword)) {}
  Self(Self &&) = default;
  Self(const Self &) = delete;
  Self &operator=(Self &&) = default;
  Self &operator=(const Self &) = delete;
  ~Self() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitSelfExpr(*this);
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
  Token field;
  ExprPtr value;
};

class ConstructorDecl final : public Stmt {
public:
  ConstructorDecl(Token name, std::vector<Parameter> parameters,
                  std::vector<ConstructorInitializer> initializers,
                  std::unique_ptr<BlockStmt> body)
      : name_(std::move(name)), parameters_(std::move(parameters)),
        initializers_(std::move(initializers)), body_(std::move(body)) {}

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
  [[nodiscard]] const std::unique_ptr<BlockStmt> &body() const { return body_; }

private:
  Token name_;
  std::vector<Parameter> parameters_;
  std::vector<ConstructorInitializer> initializers_;
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
            std::vector<GenericParameter> genericParameters, StmtList members)
      : keyword_(std::move(keyword)), kind_(kind), name_(std::move(name)),
        genericParameters_(std::move(genericParameters)),
        members_(std::move(members)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitClassDecl(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }
  [[nodiscard]] ClassKind kind() const { return kind_; }
  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const std::vector<GenericParameter> &genericParameters() const {
    return genericParameters_;
  }
  [[nodiscard]] const StmtList &members() const { return members_; }

private:
  Token keyword_;
  ClassKind kind_;
  Token name_;
  std::vector<GenericParameter> genericParameters_;
  StmtList members_;
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

class FunctionDecl final : public Stmt {
public:
  FunctionDecl(
      TypeRef returnType, Token name,
      std::vector<GenericParameter> genericParameters,
      std::vector<Parameter> parameters, std::unique_ptr<BlockStmt> body,
      std::optional<RuntimeBinding> runtimeBinding = std::nullopt,
      ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly)
      : returnType_(std::move(returnType)), name_(std::move(name)),
        genericParameters_(std::move(genericParameters)),
        parameters_(std::move(parameters)), body_(std::move(body)),
        runtimeBinding_(std::move(runtimeBinding)),
        receiverMutability_(receiverMutability) {}

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

private:
  TypeRef returnType_;
  Token name_;
  std::vector<GenericParameter> genericParameters_;
  std::vector<Parameter> parameters_;
  std::unique_ptr<BlockStmt> body_;
  std::optional<RuntimeBinding> runtimeBinding_;
  ReceiverMutability receiverMutability_;
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

class VariableDecl final : public Stmt {
public:
  VariableDecl(Mutability mutability, TypeRef type, Token name,
               ExprPtr initializer)
      : mutability_(mutability), type_(std::move(type)), name_(std::move(name)),
        initializer_(std::move(initializer)) {}

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

private:
  Mutability mutability_;
  TypeRef type_;
  Token name_;
  ExprPtr initializer_;
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

private:
  StmtList declarations_;
};

} // namespace lang
