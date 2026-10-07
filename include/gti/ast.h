#pragma once

#include "gti/diagnostic.h"
#include "gti/target.h"
#include "gti/token.h"

#include <atomic>
#include <cstdint>
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

  [[nodiscard]] static TypeRef nativeFunction(Token arrow,
                                              std::vector<TypeRef> parameters,
                                              TypeRef returnType) {
    TypeRef type(std::move(arrow));
    type.nativeFunctionParameterCount = parameters.size();
    type.arguments = std::move(parameters);
    type.arguments.emplace_back(std::move(returnType));
    return type;
  }

  [[nodiscard]] bool isNativeFunction() const {
    return nativeFunctionParameterCount.has_value();
  }

  [[nodiscard]] static TypeRef
  functionSignature(TypeRef returnType, std::vector<TypeRef> parameters) {
    TypeRef type(returnType.name);
    type.functionSignatureParameterCount = parameters.size();
    type.arguments = std::move(parameters);
    type.arguments.emplace_back(std::move(returnType));
    return type;
  }

  [[nodiscard]] bool isFunctionSignature() const {
    return functionSignatureParameterCount.has_value();
  }

  NamePath name;
  std::vector<TypeRef> arguments;
  struct PointerLayer {
    Token star;
    // Qualifies this layer's pointee, including another pointer object.
    std::optional<Token> pointeeConst;
  };
  // Innermost first, matching written declarator order. Every layer retains
  // its own source token and permission; depth is not a nominal ABI exception.
  std::vector<PointerLayer> pointers;
  std::vector<ArrayExtentExprPtr> arrayExtents;
  std::optional<Token> reference;
  // Explicit access on a function-signature reference parameter component.
  std::optional<Token> referenceMutability;
  // A native C function type stores its parameter types followed by its
  // return type in `arguments`. It is admitted only as a named `using` target;
  // ordinary function declarations continue to use FunctionDecl.
  std::optional<std::size_t> nativeFunctionParameterCount;
  // A GTI signature describes a call, not a stored function pointer. Like
  // native function syntax, arguments hold parameters followed by the result.
  std::optional<std::size_t> functionSignatureParameterCount;
  // Written receiver capability of signature metadata: absent, `mut`, or `&&`.
  std::optional<Token> functionSignatureQualifier;
  // A type-list expansion, distinct from a named value parameter pack.
  std::optional<Token> packExpansion;
  GenericArgumentSyntax genericArgumentSyntax = GenericArgumentSyntax::Type;
  ArrayExtentExprPtr genericValueExpression;
};

enum class Mutability {
  Immutable,
  Mutable,
};

enum class ReceiverMutability {
  ReadOnly,
  Mutable,
  Consuming,
};

[[nodiscard]] constexpr bool
receiverAllowsMutation(ReceiverMutability mutability) {
  return mutability != ReceiverMutability::ReadOnly;
}

enum class LanguageLinkage {
  Gti,
  C,
};

enum class OverloadedOperator {
  Dereference,
  Arrow,
  PreIncrement,
  PreDecrement,
  Assignment,
  Plus,
  Minus,
  Multiply,
  Divide,
  Remainder,
  AddAssign,
  SubtractAssign,
  MultiplyAssign,
  DivideAssign,
  RemainderAssign,
  BitwiseNot,
  BitwiseAnd,
  BitwiseOr,
  BitwiseXor,
  ShiftLeft,
  ShiftRight,
  BitwiseAndAssign,
  BitwiseOrAssign,
  BitwiseXorAssign,
  ShiftLeftAssign,
  ShiftRightAssign,
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
  case OverloadedOperator::PreDecrement:
    return "__gti_operator_pre_decrement";
  case OverloadedOperator::Assignment:
    return "__gti_operator_assignment";
  case OverloadedOperator::Plus:
    return "__gti_operator_plus";
  case OverloadedOperator::Minus:
    return "__gti_operator_minus";
  case OverloadedOperator::Multiply:
    return "__gti_operator_multiply";
  case OverloadedOperator::Divide:
    return "__gti_operator_divide";
  case OverloadedOperator::Remainder:
    return "__gti_operator_remainder";
  case OverloadedOperator::AddAssign:
    return "__gti_operator_add_assign";
  case OverloadedOperator::SubtractAssign:
    return "__gti_operator_subtract_assign";
  case OverloadedOperator::MultiplyAssign:
    return "__gti_operator_multiply_assign";
  case OverloadedOperator::DivideAssign:
    return "__gti_operator_divide_assign";
  case OverloadedOperator::RemainderAssign:
    return "__gti_operator_remainder_assign";
  case OverloadedOperator::BitwiseNot:
    return "__gti_operator_bitwise_not";
  case OverloadedOperator::BitwiseAnd:
    return "__gti_operator_bitwise_and";
  case OverloadedOperator::BitwiseOr:
    return "__gti_operator_bitwise_or";
  case OverloadedOperator::BitwiseXor:
    return "__gti_operator_bitwise_xor";
  case OverloadedOperator::ShiftLeft:
    return "__gti_operator_shift_left";
  case OverloadedOperator::ShiftRight:
    return "__gti_operator_shift_right";
  case OverloadedOperator::BitwiseAndAssign:
    return "__gti_operator_bitwise_and_assign";
  case OverloadedOperator::BitwiseOrAssign:
    return "__gti_operator_bitwise_or_assign";
  case OverloadedOperator::BitwiseXorAssign:
    return "__gti_operator_bitwise_xor_assign";
  case OverloadedOperator::ShiftLeftAssign:
    return "__gti_operator_shift_left_assign";
  case OverloadedOperator::ShiftRightAssign:
    return "__gti_operator_shift_right_assign";
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
  case OverloadedOperator::PreDecrement:
    return "operator--";
  case OverloadedOperator::Assignment:
    return "operator=";
  case OverloadedOperator::Plus:
    return "operator+";
  case OverloadedOperator::Minus:
    return "operator-";
  case OverloadedOperator::Multiply:
    return "operator*";
  case OverloadedOperator::Divide:
    return "operator/";
  case OverloadedOperator::Remainder:
    return "operator%";
  case OverloadedOperator::AddAssign:
    return "operator+=";
  case OverloadedOperator::SubtractAssign:
    return "operator-=";
  case OverloadedOperator::MultiplyAssign:
    return "operator*=";
  case OverloadedOperator::DivideAssign:
    return "operator/=";
  case OverloadedOperator::RemainderAssign:
    return "operator%=";
  case OverloadedOperator::BitwiseNot:
    return "operator~";
  case OverloadedOperator::BitwiseAnd:
    return "operator&";
  case OverloadedOperator::BitwiseOr:
    return "operator|";
  case OverloadedOperator::BitwiseXor:
    return "operator^";
  case OverloadedOperator::ShiftLeft:
    return "operator<<";
  case OverloadedOperator::ShiftRight:
    return "operator>>";
  case OverloadedOperator::BitwiseAndAssign:
    return "operator&=";
  case OverloadedOperator::BitwiseOrAssign:
    return "operator|=";
  case OverloadedOperator::BitwiseXorAssign:
    return "operator^=";
  case OverloadedOperator::ShiftLeftAssign:
    return "operator<<=";
  case OverloadedOperator::ShiftRightAssign:
    return "operator>>=";
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
  Union,
};

enum class AccessModifier {
  Private,
  Public,
};

struct BaseSpecifier {
  std::optional<Token> access;
  TypeRef type;
};

struct GenericParameter {
  Token name;
  std::optional<Token> pack;
  std::optional<Token> valueType;
  std::optional<NamePath> constraint;
  std::optional<Token> equal;
  std::optional<TypeRef> defaultType;
  ArrayExtentExprPtr defaultValue;
};

struct RuntimeBinding {
  Token attribute;
  std::string name;
};

struct CompilerConstraintBinding {
  Token attribute;
  std::string name;
};

class MemberRequirement;

// A constraint leaf is either a named concept application or an exact member
// requirement. The latter's arguments are the receiver referent and result.
struct ConceptApplication {
  NamePath name;
  std::vector<TypeRef> arguments;
  std::shared_ptr<const MemberRequirement> memberRequirement;
};

enum class ConstraintExpressionKind {
  Application,
  Conjunction,
  Disjunction,
};

struct ConstraintExpressionNode {
  static constexpr std::size_t invalidIndex = static_cast<std::size_t>(-1);

  ConstraintExpressionKind kind = ConstraintExpressionKind::Application;
  std::size_t application = invalidIndex;
  std::size_t left = invalidIndex;
  std::size_t right = invalidIndex;
  Token operation;
};

struct ConstraintExpression {
  std::vector<ConceptApplication> applications;
  std::vector<ConstraintExpressionNode> nodes;
  std::size_t root = ConstraintExpressionNode::invalidIndex;

  [[nodiscard]] bool empty() const { return applications.empty(); }
};

struct RequiresClause {
  Token keyword;
  ConstraintExpression expression;

  [[nodiscard]] const std::vector<ConceptApplication> &requirements() const {
    return expression.applications;
  }
};

struct NativeCArrayAttribute {
  Token attribute;
  Token countParameter;
};

struct FunctionAttribute {
  Token name;
  std::optional<Token> argument;
};

struct NativeLinkNameAttribute {
  Token attribute;
  Token symbol;

  [[nodiscard]] std::string_view name() const {
    const auto *value = std::get_if<std::string>(&symbol.literal);
    return value == nullptr ? std::string_view{} : std::string_view(*value);
  }
};

struct PureSpecifier {
  Token equal;
  Token zero;
};

struct CompileCondition {
  enum class Kind {
    TargetComparison,
    Defined,
    Not,
    And,
    Or,
  };

  Kind kind = Kind::TargetComparison;
  Token token;
  std::optional<TargetProperty> property;
  Token oper;
  Token valueToken;
  std::string expectedValue;
  std::string flag;
  std::unique_ptr<CompileCondition> left;
  std::unique_ptr<CompileCondition> right;
  std::optional<bool> resolved;

  CompileCondition() = default;
  CompileCondition(CompileCondition &&) = default;
  CompileCondition(const CompileCondition &) = delete;
  CompileCondition &operator=(CompileCondition &&) = default;
  CompileCondition &operator=(const CompileCondition &) = delete;

  static CompileCondition targetComparison(Token propertyToken,
                                           TargetProperty property, Token oper,
                                           Token valueToken,
                                           std::string expectedValue) {
    CompileCondition condition;
    condition.kind = Kind::TargetComparison;
    condition.token = std::move(propertyToken);
    condition.property = property;
    condition.oper = std::move(oper);
    condition.valueToken = std::move(valueToken);
    condition.expectedValue = std::move(expectedValue);
    return condition;
  }

  static CompileCondition defined(Token flagToken) {
    CompileCondition condition;
    condition.kind = Kind::Defined;
    condition.flag = flagToken.lexeme;
    condition.resolved = flagToken.configurationFlagDefined;
    condition.token = std::move(flagToken);
    return condition;
  }

  static CompileCondition unary(Kind kind, Token oper,
                                CompileCondition operand) {
    CompileCondition condition;
    condition.kind = kind;
    condition.token = std::move(oper);
    condition.left = std::make_unique<CompileCondition>(std::move(operand));
    return condition;
  }

  static CompileCondition binary(Kind kind, CompileCondition left, Token oper,
                                 CompileCondition right) {
    CompileCondition condition;
    condition.kind = kind;
    condition.token = std::move(oper);
    condition.left = std::make_unique<CompileCondition>(std::move(left));
    condition.right = std::make_unique<CompileCondition>(std::move(right));
    return condition;
  }

  [[nodiscard]] bool matches(const TargetInfo &target) const {
    if (resolved) {
      return *resolved;
    }
    switch (kind) {
    case Kind::TargetComparison: {
      const bool equal = property && target.value(*property) == expectedValue;
      return oper.kind == TokenKind::BANG_EQUAL ? !equal : equal;
    }
    case Kind::Defined:
      return false;
    case Kind::Not:
      return left && !left->matches(target);
    case Kind::And:
      return left && right && left->matches(target) && right->matches(target);
    case Kind::Or:
      return left && right && (left->matches(target) || right->matches(target));
    }
    return false;
  }
};

class Assign;
class ArrayInitializer;
class Binary;
class Call;
class ConditionalExpr;
class Conversion;
class DirectInitializer;
class DereferenceSet;
class Get;
class Grouping;
class Index;
class IndexSet;
class Lambda;
class LayoutQuery;
class MemberRequirement;
class LiteralExpr;
class Logical;
class PackFold;
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
class ConceptDecl;
class ConditionalStmt;
class ConstructorDecl;
class DestructorDecl;
class DoWhileStmt;
class EmptyStmt;
class EnumDecl;
class ExternCDecl;
class ExpressionStmt;
class ForStmt;
class FunctionDecl;
class IfStmt;
class LoopControlStmt;
class NamespaceAliasDecl;
class NamespaceDecl;
class RangeForStmt;
class ReturnStmt;
class MutableFieldGroupDecl;
class StaticAssertDecl;
class SwitchStmt;
class StructuredBindingDecl;
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
  virtual void visitConditionalExpr(const ConditionalExpr &expr) = 0;
  virtual void visitConversionExpr(const Conversion &expr) = 0;
  virtual void visitDirectInitializerExpr(const DirectInitializer &expr) = 0;
  virtual void visitDereferenceSetExpr(const DereferenceSet &expr) = 0;
  virtual void visitGetExpr(const Get &expr) = 0;
  virtual void visitGroupingExpr(const Grouping &expr) = 0;
  virtual void visitIndexExpr(const Index &expr) = 0;
  virtual void visitIndexSetExpr(const IndexSet &expr) = 0;
  virtual void visitLambdaExpr(const Lambda &expr) = 0;
  virtual void visitLayoutQueryExpr(const LayoutQuery &expr) = 0;
  virtual void visitMemberRequirementExpr(const MemberRequirement &expr) = 0;
  virtual void visitLiteralExpr(const LiteralExpr &expr) = 0;
  virtual void visitLogicalExpr(const Logical &expr) = 0;
  virtual void visitPackFoldExpr(const PackFold &expr) = 0;
  virtual void visitPackExpansionExpr(const PackExpansion &expr) = 0;
  virtual void visitPostfixExpr(const Postfix &expr) = 0;
  virtual void visitQualifiedNameExpr(const QualifiedName &expr) = 0;
  virtual void visitThisExpr(const This &expr) = 0;
  virtual void visitSetExpr(const Set &expr) = 0;
  virtual void visitUnaryExpr(const Unary &expr) = 0;
  virtual void visitUnexpectedExpr(const Unexpected &expr) = 0;
  virtual void visitVariableExpr(const Variable &expr) = 0;
};

enum class ExprKind : std::uint8_t {
  Assign,
  ArrayInitializer,
  Binary,
  Call,
  ConditionalExpr,
  Conversion,
  DirectInitializer,
  DereferenceSet,
  Get,
  Grouping,
  Index,
  IndexSet,
  Lambda,
  LayoutQuery,
  MemberRequirement,
  LiteralExpr,
  Logical,
  PackFold,
  PackExpansion,
  Postfix,
  QualifiedName,
  This,
  Set,
  Unary,
  Unexpected,
  Variable,
};

class Expr {
public:
  Expr() = default;
  Expr(Expr &&) = default;
  Expr(const Expr &) = delete;
  Expr &operator=(Expr &&) = default;
  Expr &operator=(const Expr &) = delete;
  virtual ~Expr() = default;

  [[nodiscard]] virtual ExprKind exprKind() const = 0;
  virtual void accept(ExprVisitor &visitor) const = 0;
};

template <ExprKind Kind> class ExprOfKind : public Expr {
public:
  static constexpr ExprKind exprKindValue = Kind;
  [[nodiscard]] ExprKind exprKind() const final { return Kind; }
};

template <typename Expression>
[[nodiscard]] const Expression *exprAs(const Expr *expression) {
  return expression != nullptr &&
                 expression->exprKind() == Expression::exprKindValue
             ? static_cast<const Expression *>(expression)
             : nullptr;
}

template <typename Expression>
[[nodiscard]] Expression *exprAs(Expr *expression) {
  return expression != nullptr &&
                 expression->exprKind() == Expression::exprKindValue
             ? static_cast<Expression *>(expression)
             : nullptr;
}

using ExprPtr = std::unique_ptr<Expr>;
using ExprList = std::vector<ExprPtr>;

struct ParameterDefault {
  ParameterDefault(Token equal, ExprPtr expression)
      : equal(std::move(equal)), expression(std::move(expression)) {}
  ParameterDefault(ParameterDefault &&) = default;
  ParameterDefault(const ParameterDefault &) = delete;
  ParameterDefault &operator=(ParameterDefault &&) = default;
  ParameterDefault &operator=(const ParameterDefault &) = delete;

  Token equal;
  ExprPtr expression;
};

struct Parameter {
  Parameter(TypeRef type, Token name,
            Mutability mutability = Mutability::Immutable,
            std::optional<Token> pack = std::nullopt,
            std::optional<ParameterDefault> defaultArgument = std::nullopt)
      : type(std::move(type)), name(std::move(name)), mutability(mutability),
        pack(std::move(pack)), defaultArgument(std::move(defaultArgument)) {}
  Parameter(Parameter &&) = default;
  Parameter(const Parameter &) = delete;
  Parameter &operator=(Parameter &&) = default;
  Parameter &operator=(const Parameter &) = delete;

  [[nodiscard]] bool hasDefault() const { return defaultArgument.has_value(); }

  TypeRef type;
  Token name;
  Mutability mutability;
  std::optional<Token> pack;
  std::optional<ParameterDefault> defaultArgument;
};

class StmtVisitor {
public:
  virtual ~StmtVisitor() = default;

  virtual void visitAccessSpecifierDecl(const AccessSpecifierDecl &stmt) = 0;
  virtual void visitBlockStmt(const BlockStmt &stmt) = 0;
  virtual void visitClassDecl(const ClassDecl &stmt) = 0;
  virtual void
  visitCompileErrorDirective(const CompileErrorDirective &stmt) = 0;
  virtual void visitConceptDecl(const ConceptDecl &stmt) = 0;
  virtual void visitConditionalStmt(const ConditionalStmt &stmt) = 0;
  virtual void visitConstructorDecl(const ConstructorDecl &stmt) = 0;
  virtual void visitDestructorDecl(const DestructorDecl &stmt) = 0;
  virtual void visitDoWhileStmt(const DoWhileStmt &stmt) = 0;
  virtual void visitEmptyStmt(const EmptyStmt &stmt) = 0;
  virtual void visitEnumDecl(const EnumDecl &stmt) = 0;
  virtual void visitExternCDecl(const ExternCDecl &stmt) = 0;
  virtual void visitExpressionStmt(const ExpressionStmt &stmt) = 0;
  virtual void visitForStmt(const ForStmt &stmt) = 0;
  virtual void visitFunctionDecl(const FunctionDecl &stmt) = 0;
  virtual void visitIfStmt(const IfStmt &stmt) = 0;
  virtual void visitLoopControlStmt(const LoopControlStmt &stmt) = 0;
  virtual void
  visitMutableFieldGroupDecl(const MutableFieldGroupDecl &stmt) = 0;
  virtual void visitNamespaceAliasDecl(const NamespaceAliasDecl &stmt) = 0;
  virtual void visitNamespaceDecl(const NamespaceDecl &stmt) = 0;
  virtual void visitRangeForStmt(const RangeForStmt &stmt) = 0;
  virtual void visitReturnStmt(const ReturnStmt &stmt) = 0;
  virtual void visitStaticAssertDecl(const StaticAssertDecl &stmt) = 0;
  virtual void visitSwitchStmt(const SwitchStmt &stmt) = 0;
  virtual void
  visitStructuredBindingDecl(const StructuredBindingDecl &stmt) = 0;
  virtual void visitTypeAliasDecl(const TypeAliasDecl &stmt) = 0;
  virtual void visitVariableDecl(const VariableDecl &stmt) = 0;
  virtual void visitWhileStmt(const WhileStmt &stmt) = 0;
};

enum class StmtKind : std::uint8_t {
  AccessSpecifierDecl,
  BlockStmt,
  ClassDecl,
  CompileErrorDirective,
  ConceptDecl,
  ConditionalStmt,
  ConstructorDecl,
  DestructorDecl,
  DoWhileStmt,
  EmptyStmt,
  EnumDecl,
  ExternCDecl,
  ExpressionStmt,
  ForStmt,
  FunctionDecl,
  IfStmt,
  LoopControlStmt,
  MutableFieldGroupDecl,
  NamespaceAliasDecl,
  NamespaceDecl,
  RangeForStmt,
  ReturnStmt,
  StaticAssertDecl,
  SwitchStmt,
  StructuredBindingDecl,
  TypeAliasDecl,
  VariableDecl,
  WhileStmt,
};

class Stmt {
public:
  Stmt() = default;
  Stmt(Stmt &&) = default;
  Stmt(const Stmt &) = delete;
  Stmt &operator=(Stmt &&) = default;
  Stmt &operator=(const Stmt &) = delete;
  virtual ~Stmt() = default;

  [[nodiscard]] virtual StmtKind stmtKind() const = 0;
  virtual void accept(StmtVisitor &visitor) const = 0;

  // Parser-recorded full extent, from the statement's first token through its
  // last consumed token. Exact name spans stay separate in the semantic
  // database; tooling that needs enclosing ranges reads this instead of
  // re-deriving structure from punctuation.
  [[nodiscard]] const std::optional<SourceSpan> &extent() const {
    return extent_;
  }
  void setExtent(SourceSpan extent) { extent_ = std::move(extent); }

  [[nodiscard]] const std::optional<std::string> &
  documentationMarkdown() const {
    return documentationMarkdown_;
  }
  void setDocumentationMarkdown(std::string documentation) {
    documentationMarkdown_ = std::move(documentation);
  }

  [[nodiscard]] const std::optional<Token> &exportKeyword() const {
    return exportKeyword_;
  }
  void setExportKeyword(Token keyword) { exportKeyword_ = std::move(keyword); }

private:
  std::optional<SourceSpan> extent_;
  std::optional<std::string> documentationMarkdown_;
  std::optional<Token> exportKeyword_;
};

template <StmtKind Kind> class StmtOfKind : public Stmt {
public:
  static constexpr StmtKind stmtKindValue = Kind;
  [[nodiscard]] StmtKind stmtKind() const final { return Kind; }
};

template <typename Statement>
[[nodiscard]] const Statement *stmtAs(const Stmt *statement) {
  return statement != nullptr &&
                 statement->stmtKind() == Statement::stmtKindValue
             ? static_cast<const Statement *>(statement)
             : nullptr;
}

// Top-level parsed source units are immutable once Parser publishes them.
// Shared ownership lets compatible frontend snapshots reuse an unchanged
// unit's complete statement tree without retaining raw pointers into an
// evicted parser arena. Nested expression ownership remains unique.
using StmtPtr = std::shared_ptr<Stmt>;
using StmtList = std::vector<StmtPtr>;

struct ConditionalBranch {
  std::optional<CompileCondition> condition;
  StmtList statements;
};

class Assign final : public ExprOfKind<ExprKind::Assign> {
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

class ArrayInitializer final : public ExprOfKind<ExprKind::ArrayInitializer> {
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

class Binary final : public ExprOfKind<ExprKind::Binary> {
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

class ConditionalExpr final : public ExprOfKind<ExprKind::ConditionalExpr> {
public:
  ConditionalExpr(ExprPtr condition, Token question, ExprPtr thenExpression,
                  Token colon, ExprPtr elseExpression)
      : condition_(std::move(condition)), question_(std::move(question)),
        thenExpression_(std::move(thenExpression)), colon_(std::move(colon)),
        elseExpression_(std::move(elseExpression)) {}
  ConditionalExpr(ConditionalExpr &&) = default;
  ConditionalExpr(const ConditionalExpr &) = delete;
  ConditionalExpr &operator=(ConditionalExpr &&) = default;
  ConditionalExpr &operator=(const ConditionalExpr &) = delete;
  ~ConditionalExpr() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitConditionalExpr(*this);
  }

  [[nodiscard]] const ExprPtr &condition() const { return condition_; }
  [[nodiscard]] const Token &question() const { return question_; }
  [[nodiscard]] const ExprPtr &thenExpression() const {
    return thenExpression_;
  }
  [[nodiscard]] const Token &colon() const { return colon_; }
  [[nodiscard]] const ExprPtr &elseExpression() const {
    return elseExpression_;
  }

private:
  ExprPtr condition_;
  Token question_;
  ExprPtr thenExpression_;
  Token colon_;
  ExprPtr elseExpression_;
};

class Call final : public ExprOfKind<ExprKind::Call> {
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

  // Argument-list geometry recorded by the parser for editor tooling: the
  // opening parenthesis and each argument-separating comma, alongside the
  // closing paren(). Synthesized calls leave the geometry unset.
  [[nodiscard]] const std::optional<Token> &leftParen() const {
    return leftParen_;
  }
  [[nodiscard]] const std::vector<Token> &argumentCommas() const {
    return argumentCommas_;
  }
  void setArgumentGeometry(Token leftParen, std::vector<Token> commas) {
    leftParen_ = std::move(leftParen);
    argumentCommas_ = std::move(commas);
  }

private:
  ExprPtr callee_;
  std::vector<TypeRef> typeArguments_;
  Token paren_;
  ExprList arguments_;
  std::optional<Token> leftParen_;
  std::vector<Token> argumentCommas_;
};

class Conversion final : public ExprOfKind<ExprKind::Conversion> {
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

class DirectInitializer final : public ExprOfKind<ExprKind::DirectInitializer> {
public:
  DirectInitializer(Token brace, ExprList arguments, Token closingBrace,
                    std::vector<Token> designators = {})
      : brace_(std::move(brace)), arguments_(std::move(arguments)),
        closingBrace_(std::move(closingBrace)),
        designators_(std::move(designators)) {}
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
  [[nodiscard]] const std::vector<Token> &designators() const {
    return designators_;
  }
  [[nodiscard]] const Token &closingBrace() const { return closingBrace_; }

private:
  Token brace_;
  ExprList arguments_;
  Token closingBrace_;
  std::vector<Token> designators_;
};

class PackFold final : public ExprOfKind<ExprKind::PackFold> {
public:
  PackFold(Token leftParen, ExprPtr pattern, Token comma, Token ellipsis,
           Token rightParen)
      : leftParen_(std::move(leftParen)), pattern_(std::move(pattern)),
        comma_(std::move(comma)), ellipsis_(std::move(ellipsis)),
        rightParen_(std::move(rightParen)) {}
  PackFold(PackFold &&) = default;
  PackFold(const PackFold &) = delete;
  PackFold &operator=(PackFold &&) = default;
  PackFold &operator=(const PackFold &) = delete;
  ~PackFold() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitPackFoldExpr(*this);
  }

  [[nodiscard]] const Token &leftParen() const { return leftParen_; }
  [[nodiscard]] const ExprPtr &pattern() const { return pattern_; }
  [[nodiscard]] const Token &comma() const { return comma_; }
  [[nodiscard]] const Token &ellipsis() const { return ellipsis_; }
  [[nodiscard]] const Token &rightParen() const { return rightParen_; }

private:
  Token leftParen_;
  ExprPtr pattern_;
  Token comma_;
  Token ellipsis_;
  Token rightParen_;
};

class PackExpansion final : public ExprOfKind<ExprKind::PackExpansion> {
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

class DereferenceSet final : public ExprOfKind<ExprKind::DereferenceSet> {
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

class Get final : public ExprOfKind<ExprKind::Get> {
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

class Grouping final : public ExprOfKind<ExprKind::Grouping> {
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

class Index final : public ExprOfKind<ExprKind::Index> {
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

class IndexSet final : public ExprOfKind<ExprKind::IndexSet> {
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

enum class LambdaCaptureMode : std::uint8_t {
  Copy,
  Move,
};

struct LambdaCapture {
  LambdaCapture(Token name, std::optional<Token> equal, ExprPtr initializer)
      : name(std::move(name)), equal(std::move(equal)),
        initializer(std::move(initializer)) {}
  LambdaCapture(LambdaCapture &&) = default;
  LambdaCapture(const LambdaCapture &) = delete;
  LambdaCapture &operator=(LambdaCapture &&) = default;
  LambdaCapture &operator=(const LambdaCapture &) = delete;

  [[nodiscard]] bool explicitInitializer() const { return equal.has_value(); }

  Token name;
  std::optional<Token> equal;
  ExprPtr initializer;
};

class Lambda final : public ExprOfKind<ExprKind::Lambda> {
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

enum class LayoutQueryKind {
  Size,
  Alignment,
};

class LayoutQuery final : public ExprOfKind<ExprKind::LayoutQuery> {
public:
  LayoutQuery(Token keyword, TypeRef type)
      : keyword_(std::move(keyword)), type_(std::move(type)) {}

  void accept(ExprVisitor &visitor) const override {
    visitor.visitLayoutQueryExpr(*this);
  }

  [[nodiscard]] LayoutQueryKind kind() const {
    return keyword_.kind == TokenKind::SIZEOF ? LayoutQueryKind::Size
                                              : LayoutQueryKind::Alignment;
  }
  [[nodiscard]] const Token &keyword() const { return keyword_; }
  [[nodiscard]] const TypeRef &type() const { return type_; }

private:
  Token keyword_;
  TypeRef type_;
};

// A bounded unevaluated exact instance-member signature query. Neither
// the synthetic receiver nor the member invocation creates runtime storage.
class MemberRequirement final : public ExprOfKind<ExprKind::MemberRequirement> {
public:
  MemberRequirement(Token keyword, TypeRef receiver, Token binding,
                    Token receiverUse, Token member, TypeRef result,
                    std::vector<TypeRef> parameters = {},
                    std::vector<Token> bindings = {},
                    std::vector<Token> arguments = {},
                    std::optional<Token> unsafeKeyword = std::nullopt)
      : keyword_(std::move(keyword)), receiver_(std::move(receiver)),
        binding_(std::move(binding)), receiverUse_(std::move(receiverUse)),
        member_(std::move(member)), result_(std::move(result)),
        parameters_(std::move(parameters)), bindings_(std::move(bindings)),
        arguments_(std::move(arguments)),
        unsafeKeyword_(std::move(unsafeKeyword)) {}

  void accept(ExprVisitor &visitor) const override {
    visitor.visitMemberRequirementExpr(*this);
  }
  const Token &keyword() const { return keyword_; }
  const TypeRef &receiver() const { return receiver_; }
  const Token &binding() const { return binding_; }
  const Token &receiverUse() const { return receiverUse_; }
  const Token &member() const { return member_; }
  const TypeRef &result() const { return result_; }
  const std::vector<TypeRef> &parameters() const { return parameters_; }
  const std::vector<Token> &bindings() const { return bindings_; }
  const std::vector<Token> &arguments() const { return arguments_; }
  const std::optional<Token> &unsafeKeyword() const { return unsafeKeyword_; }

private:
  Token keyword_;
  TypeRef receiver_;
  Token binding_;
  Token receiverUse_;
  Token member_;
  TypeRef result_;
  std::vector<TypeRef> parameters_;
  std::vector<Token> bindings_;
  std::vector<Token> arguments_;
  std::optional<Token> unsafeKeyword_;
};

class LiteralExpr final : public ExprOfKind<ExprKind::LiteralExpr> {
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

class Logical final : public ExprOfKind<ExprKind::Logical> {
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

class Postfix final : public ExprOfKind<ExprKind::Postfix> {
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

class QualifiedName final : public ExprOfKind<ExprKind::QualifiedName> {
public:
  explicit QualifiedName(NamePath name,
                         std::optional<TypeRef> ownerType = std::nullopt)
      : name_(std::move(name)), ownerType_(std::move(ownerType)) {}

  void accept(ExprVisitor &visitor) const override {
    visitor.visitQualifiedNameExpr(*this);
  }

  [[nodiscard]] const NamePath &name() const { return name_; }
  [[nodiscard]] NamePath takeName() { return std::move(name_); }
  [[nodiscard]] const std::optional<TypeRef> &ownerType() const {
    return ownerType_;
  }

private:
  NamePath name_;
  std::optional<TypeRef> ownerType_;
};

class This final : public ExprOfKind<ExprKind::This> {
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

class Set final : public ExprOfKind<ExprKind::Set> {
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

class Unary final : public ExprOfKind<ExprKind::Unary> {
public:
  Unary(Token oper, ExprPtr right, ExprPtr rangeObject = nullptr)
      : oper_(std::move(oper)), right_(std::move(right)),
        rangeObject_(std::move(rangeObject)) {}
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
  // Parser-generated range element access retains its stable range object so
  // semantic/HIR lowering can select the built-in fixed-array index strategy
  // without reconstructing ownership from a generated iterator spelling.
  [[nodiscard]] const ExprPtr &rangeObject() const { return rangeObject_; }
  [[nodiscard]] ExprPtr takeRight() { return std::move(right_); }

private:
  Token oper_;
  ExprPtr right_;
  ExprPtr rangeObject_;
};

class Unexpected final : public ExprOfKind<ExprKind::Unexpected> {
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

class Variable final : public ExprOfKind<ExprKind::Variable> {
public:
  explicit Variable(Token name) : name_(std::move(name)) {}
  Variable(Token name, Token patternAuto, Token patternReference,
           std::optional<Token> patternMutability)
      : name_(std::move(name)), patternAuto_(std::move(patternAuto)),
        patternReference_(std::move(patternReference)),
        patternMutability_(std::move(patternMutability)) {}
  Variable(Variable &&) = default;
  Variable(const Variable &) = delete;
  Variable &operator=(Variable &&) = default;
  Variable &operator=(const Variable &) = delete;
  ~Variable() override = default;

  void accept(ExprVisitor &visitor) const override {
    visitor.visitVariableExpr(*this);
  }

  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] bool isReferencePattern() const {
    return patternReference_.has_value();
  }
  [[nodiscard]] bool isMutablePattern() const {
    return patternMutability_.has_value();
  }

private:
  Token name_;
  std::optional<Token> patternAuto_;
  std::optional<Token> patternReference_;
  std::optional<Token> patternMutability_;
};

class BlockStmt final : public StmtOfKind<StmtKind::BlockStmt> {
public:
  explicit BlockStmt(StmtList statements,
                     std::optional<Token> unsafeKeyword = std::nullopt)
      : statements_(std::move(statements)),
        unsafeKeyword_(std::move(unsafeKeyword)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitBlockStmt(*this);
  }

  [[nodiscard]] const StmtList &statements() const { return statements_; }
  [[nodiscard]] bool isUnsafe() const { return unsafeKeyword_.has_value(); }
  [[nodiscard]] const std::optional<Token> &unsafeKeyword() const {
    return unsafeKeyword_;
  }

private:
  StmtList statements_;
  std::optional<Token> unsafeKeyword_;
};

class AccessSpecifierDecl final
    : public StmtOfKind<StmtKind::AccessSpecifierDecl> {
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

class MutableFieldGroupDecl final
    : public StmtOfKind<StmtKind::MutableFieldGroupDecl> {
public:
  MutableFieldGroupDecl(Token keyword, Token leftBrace, StmtList members,
                        Token rightBrace)
      : keyword_(std::move(keyword)), leftBrace_(std::move(leftBrace)),
        members_(std::move(members)), rightBrace_(std::move(rightBrace)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitMutableFieldGroupDecl(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }
  [[nodiscard]] const Token &leftBrace() const { return leftBrace_; }
  [[nodiscard]] const StmtList &members() const { return members_; }
  [[nodiscard]] const Token &rightBrace() const { return rightBrace_; }

private:
  Token keyword_;
  Token leftBrace_;
  StmtList members_;
  Token rightBrace_;
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

class ConstructorDecl final : public StmtOfKind<StmtKind::ConstructorDecl> {
public:
  ConstructorDecl(std::optional<Token> constexprKeyword, Token name,
                  std::vector<GenericParameter> genericParameters,
                  std::vector<Parameter> parameters,
                  std::optional<RequiresClause> requiresClause,
                  std::vector<ConstructorInitializer> initializers,
                  std::optional<SpecialMemberSpecifier> specifier,
                  std::unique_ptr<BlockStmt> body)
      : constexprKeyword_(std::move(constexprKeyword)), name_(std::move(name)),
        genericParameters_(std::move(genericParameters)),
        parameters_(std::move(parameters)),
        requiresClause_(std::move(requiresClause)),
        initializers_(std::move(initializers)),
        specifier_(std::move(specifier)), body_(std::move(body)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitConstructorDecl(*this);
  }

  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const std::optional<Token> &constexprKeyword() const {
    return constexprKeyword_;
  }
  [[nodiscard]] bool isConstexpr() const {
    return constexprKeyword_.has_value();
  }
  [[nodiscard]] const std::vector<GenericParameter> &genericParameters() const {
    return genericParameters_;
  }
  [[nodiscard]] const std::vector<Parameter> &parameters() const {
    return parameters_;
  }
  [[nodiscard]] const std::optional<RequiresClause> &requiresClause() const {
    return requiresClause_;
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
  std::optional<Token> constexprKeyword_;
  Token name_;
  std::vector<GenericParameter> genericParameters_;
  std::vector<Parameter> parameters_;
  std::optional<RequiresClause> requiresClause_;
  std::vector<ConstructorInitializer> initializers_;
  std::optional<SpecialMemberSpecifier> specifier_;
  std::unique_ptr<BlockStmt> body_;
};

class DestructorDecl final : public StmtOfKind<StmtKind::DestructorDecl> {
public:
  DestructorDecl(Token tilde, Token name,
                 std::optional<SpecialMemberSpecifier> specifier,
                 std::unique_ptr<BlockStmt> body)
      : tilde_(std::move(tilde)), name_(std::move(name)),
        specifier_(std::move(specifier)), body_(std::move(body)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitDestructorDecl(*this);
  }

  [[nodiscard]] const Token &tilde() const { return tilde_; }
  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const std::optional<SpecialMemberSpecifier> &specifier() const {
    return specifier_;
  }
  [[nodiscard]] const std::unique_ptr<BlockStmt> &body() const { return body_; }

private:
  Token tilde_;
  Token name_;
  std::optional<SpecialMemberSpecifier> specifier_;
  std::unique_ptr<BlockStmt> body_;
};

class ClassDecl final : public StmtOfKind<StmtKind::ClassDecl> {
public:
  ClassDecl(std::vector<Token> attributes, Token keyword, ClassKind kind,
            Token name, std::optional<TypeRef> exactSpecialization,
            std::vector<GenericParameter> genericParameters,
            std::vector<BaseSpecifier> bases, StmtList members,
            bool forwardDeclaration = false,
            std::vector<TypeRef> genericArgumentPatterns = {})
      : attributes_(std::move(attributes)), keyword_(std::move(keyword)),
        kind_(kind), name_(std::move(name)),
        exactSpecialization_(std::move(exactSpecialization)),
        genericParameters_(std::move(genericParameters)),
        genericArgumentPatterns_(std::move(genericArgumentPatterns)),
        bases_(std::move(bases)), members_(std::move(members)),
        forwardDeclaration_(forwardDeclaration) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitClassDecl(*this);
  }

  [[nodiscard]] const std::vector<Token> &attributes() const {
    return attributes_;
  }
  [[nodiscard]] const Token &keyword() const { return keyword_; }
  [[nodiscard]] ClassKind kind() const { return kind_; }
  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const std::optional<TypeRef> &exactSpecialization() const {
    return exactSpecialization_;
  }
  [[nodiscard]] bool isExactSpecialization() const {
    return exactSpecialization_.has_value();
  }
  [[nodiscard]] const std::vector<GenericParameter> &genericParameters() const {
    return genericParameters_;
  }
  // A patterned primary declares the shapes of its public arguments, not a
  // flat list of already-resolved binders. Semantic analysis owns binding the
  // names inside these types; retain the original type syntax and spans here.
  [[nodiscard]] const std::vector<TypeRef> &genericArgumentPatterns() const {
    return genericArgumentPatterns_;
  }
  [[nodiscard]] const std::vector<BaseSpecifier> &bases() const {
    return bases_;
  }
  [[nodiscard]] const StmtList &members() const { return members_; }
  [[nodiscard]] bool isForwardDeclaration() const {
    return forwardDeclaration_;
  }

private:
  std::vector<Token> attributes_;
  Token keyword_;
  ClassKind kind_;
  Token name_;
  std::optional<TypeRef> exactSpecialization_;
  std::vector<GenericParameter> genericParameters_;
  std::vector<TypeRef> genericArgumentPatterns_;
  std::vector<BaseSpecifier> bases_;
  StmtList members_;
  bool forwardDeclaration_ = false;
};

struct EnumeratorDecl {
  Token name;
  ExprPtr initializer;
  std::vector<Parameter> payload;
};

class EnumDecl final : public StmtOfKind<StmtKind::EnumDecl> {
public:
  EnumDecl(Token keyword, std::optional<Token> classKeyword, Token name,
           std::optional<TypeRef> underlyingType,
           std::vector<EnumeratorDecl> enumerators,
           std::vector<GenericParameter> genericParameters = {})
      : keyword_(std::move(keyword)), classKeyword_(std::move(classKeyword)),
        name_(std::move(name)), underlyingType_(std::move(underlyingType)),
        enumerators_(std::move(enumerators)),
        genericParameters_(std::move(genericParameters)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitEnumDecl(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }
  [[nodiscard]] const std::optional<Token> &classKeyword() const {
    return classKeyword_;
  }
  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const std::optional<TypeRef> &underlyingType() const {
    return underlyingType_;
  }
  [[nodiscard]] const std::vector<EnumeratorDecl> &enumerators() const {
    return enumerators_;
  }
  [[nodiscard]] const std::vector<GenericParameter> &genericParameters() const {
    return genericParameters_;
  }

private:
  Token keyword_;
  std::optional<Token> classKeyword_;
  Token name_;
  std::optional<TypeRef> underlyingType_;
  std::vector<EnumeratorDecl> enumerators_;
  std::vector<GenericParameter> genericParameters_;
};

class ConditionalStmt final : public StmtOfKind<StmtKind::ConditionalStmt> {
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

class CompileErrorDirective final
    : public StmtOfKind<StmtKind::CompileErrorDirective> {
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

class StaticAssertDecl final : public StmtOfKind<StmtKind::StaticAssertDecl> {
public:
  StaticAssertDecl(Token keyword, Token leftParen, Token conditionStart,
                   ExprPtr condition, std::optional<Token> comma,
                   std::optional<Token> message, Token rightParen,
                   Token semicolon)
      : keyword_(std::move(keyword)), leftParen_(std::move(leftParen)),
        conditionStart_(std::move(conditionStart)),
        condition_(std::move(condition)), comma_(std::move(comma)),
        message_(std::move(message)), rightParen_(std::move(rightParen)),
        semicolon_(std::move(semicolon)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitStaticAssertDecl(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }
  [[nodiscard]] const Token &leftParen() const { return leftParen_; }
  [[nodiscard]] const Token &conditionStart() const { return conditionStart_; }
  [[nodiscard]] const ExprPtr &condition() const { return condition_; }
  [[nodiscard]] const std::optional<Token> &comma() const { return comma_; }
  [[nodiscard]] const std::optional<Token> &messageToken() const {
    return message_;
  }
  [[nodiscard]] const std::string &message() const {
    if (message_) {
      if (const auto *text = std::get_if<std::string>(&message_->literal)) {
        return *text;
      }
    }
    static const std::string empty;
    return empty;
  }
  [[nodiscard]] const Token &rightParen() const { return rightParen_; }
  [[nodiscard]] const Token &semicolon() const { return semicolon_; }

private:
  Token keyword_;
  Token leftParen_;
  Token conditionStart_;
  ExprPtr condition_;
  std::optional<Token> comma_;
  std::optional<Token> message_;
  Token rightParen_;
  Token semicolon_;
};

class EmptyStmt final : public StmtOfKind<StmtKind::EmptyStmt> {
public:
  explicit EmptyStmt(Token semicolon) : semicolon_(std::move(semicolon)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitEmptyStmt(*this);
  }

  [[nodiscard]] const Token &semicolon() const { return semicolon_; }

private:
  Token semicolon_;
};

class ExternCDecl final : public StmtOfKind<StmtKind::ExternCDecl> {
public:
  ExternCDecl(Token keyword, Token language, StmtList declarations)
      : keyword_(std::move(keyword)), language_(std::move(language)),
        declarations_(std::move(declarations)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitExternCDecl(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }
  [[nodiscard]] const Token &language() const { return language_; }
  [[nodiscard]] const StmtList &declarations() const { return declarations_; }

private:
  Token keyword_;
  Token language_;
  StmtList declarations_;
};

class ExpressionStmt final : public StmtOfKind<StmtKind::ExpressionStmt> {
public:
  ExpressionStmt(Token expressionStart, ExprPtr expression,
                 std::optional<Token> discardAttribute = std::nullopt)
      : expressionStart_(std::move(expressionStart)),
        expression_(std::move(expression)),
        discardAttribute_(std::move(discardAttribute)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitExpressionStmt(*this);
  }

  [[nodiscard]] const Token &expressionStart() const {
    return expressionStart_;
  }
  [[nodiscard]] const ExprPtr &expression() const { return expression_; }
  [[nodiscard]] const std::optional<Token> &discardAttribute() const {
    return discardAttribute_;
  }

private:
  Token expressionStart_;
  ExprPtr expression_;
  std::optional<Token> discardAttribute_;
};

class ForStmt final : public StmtOfKind<StmtKind::ForStmt> {
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

class RangeForStmt final : public StmtOfKind<StmtKind::RangeForStmt> {
public:
  RangeForStmt(Token keyword, Mutability bindingMutability, TypeRef bindingType,
               Token bindingName, Token colon, StmtPtr lowered,
               const VariableDecl *ownerBinding,
               const VariableDecl *rangeBinding,
               const VariableDecl *iteratorBinding,
               const VariableDecl *sentinelBinding,
               const VariableDecl *elementBinding, const ForStmt *coreLoop)
      : keyword_(std::move(keyword)), bindingMutability_(bindingMutability),
        bindingType_(std::move(bindingType)),
        bindingName_(std::move(bindingName)), colon_(std::move(colon)),
        lowered_(std::move(lowered)), ownerBinding_(ownerBinding),
        rangeBinding_(rangeBinding), iteratorBinding_(iteratorBinding),
        sentinelBinding_(sentinelBinding), elementBinding_(elementBinding),
        coreLoop_(coreLoop) {}

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
  [[nodiscard]] const VariableDecl *ownerBinding() const {
    return ownerBinding_;
  }
  [[nodiscard]] const VariableDecl *rangeBinding() const {
    return rangeBinding_;
  }
  [[nodiscard]] const VariableDecl *iteratorBinding() const {
    return iteratorBinding_;
  }
  [[nodiscard]] const VariableDecl *sentinelBinding() const {
    return sentinelBinding_;
  }
  [[nodiscard]] const VariableDecl *elementBinding() const {
    return elementBinding_;
  }
  [[nodiscard]] const ForStmt *coreLoop() const { return coreLoop_; }

private:
  Token keyword_;
  Mutability bindingMutability_;
  TypeRef bindingType_;
  Token bindingName_;
  Token colon_;
  StmtPtr lowered_;
  const VariableDecl *ownerBinding_ = nullptr;
  const VariableDecl *rangeBinding_ = nullptr;
  const VariableDecl *iteratorBinding_ = nullptr;
  const VariableDecl *sentinelBinding_ = nullptr;
  const VariableDecl *elementBinding_ = nullptr;
  const ForStmt *coreLoop_ = nullptr;
};

class FunctionDecl final : public StmtOfKind<StmtKind::FunctionDecl> {
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
      std::optional<PureSpecifier> pureSpecifier = std::nullopt,
      LanguageLinkage linkage = LanguageLinkage::Gti,
      std::optional<Token> constexprKeyword = std::nullopt,
      std::optional<RequiresClause> requiresClause = std::nullopt,
      std::optional<NativeCArrayAttribute> nativeCArray = std::nullopt,
      std::optional<NativeLinkNameAttribute> nativeLinkName = std::nullopt,
      std::optional<Token> unsafeKeyword = std::nullopt,
      std::vector<FunctionAttribute> functionAttributes = {})
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
        pureSpecifier_(std::move(pureSpecifier)), linkage_(linkage),
        constexprKeyword_(std::move(constexprKeyword)),
        requiresClause_(std::move(requiresClause)),
        nativeCArray_(std::move(nativeCArray)),
        nativeLinkName_(std::move(nativeLinkName)),
        unsafeKeyword_(std::move(unsafeKeyword)),
        functionAttributes_(std::move(functionAttributes)) {}

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
  [[nodiscard]] LanguageLinkage linkage() const { return linkage_; }
  [[nodiscard]] bool hasCLinkage() const {
    return linkage_ == LanguageLinkage::C;
  }
  [[nodiscard]] bool isConstexpr() const {
    return constexprKeyword_.has_value();
  }
  [[nodiscard]] const std::optional<Token> &constexprKeyword() const {
    return constexprKeyword_;
  }
  [[nodiscard]] const std::optional<RequiresClause> &requiresClause() const {
    return requiresClause_;
  }
  [[nodiscard]] const std::optional<NativeCArrayAttribute> &
  nativeCArray() const {
    return nativeCArray_;
  }
  [[nodiscard]] const std::optional<NativeLinkNameAttribute> &
  nativeLinkName() const {
    return nativeLinkName_;
  }
  [[nodiscard]] const std::optional<Token> &unsafeKeyword() const {
    return unsafeKeyword_;
  }
  [[nodiscard]] const std::vector<FunctionAttribute> &
  functionAttributes() const {
    return functionAttributes_;
  }
  [[nodiscard]] const FunctionAttribute *
  findFunctionAttribute(std::string_view name) const {
    for (const FunctionAttribute &attribute : functionAttributes_) {
      if (attribute.name.lexeme == name) {
        return &attribute;
      }
    }
    return nullptr;
  }
  [[nodiscard]] bool hasFunctionAttribute(std::string_view name) const {
    return findFunctionAttribute(name) != nullptr;
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
  LanguageLinkage linkage_ = LanguageLinkage::Gti;
  std::optional<Token> constexprKeyword_;
  std::optional<RequiresClause> requiresClause_;
  std::optional<NativeCArrayAttribute> nativeCArray_;
  std::optional<NativeLinkNameAttribute> nativeLinkName_;
  std::optional<Token> unsafeKeyword_;
  std::vector<FunctionAttribute> functionAttributes_;
};

class IfStmt final : public StmtOfKind<StmtKind::IfStmt> {
public:
  IfStmt(ExprPtr condition, StmtPtr thenBranch, StmtPtr elseBranch,
         std::optional<Token> constexprKeyword = std::nullopt)
      : condition_(std::move(condition)), thenBranch_(std::move(thenBranch)),
        elseBranch_(std::move(elseBranch)),
        constexprKeyword_(std::move(constexprKeyword)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitIfStmt(*this);
  }

  [[nodiscard]] const ExprPtr &condition() const { return condition_; }
  [[nodiscard]] const StmtPtr &thenBranch() const { return thenBranch_; }
  [[nodiscard]] const StmtPtr &elseBranch() const { return elseBranch_; }
  [[nodiscard]] bool isConstexpr() const {
    return constexprKeyword_.has_value();
  }
  [[nodiscard]] const std::optional<Token> &constexprKeyword() const {
    return constexprKeyword_;
  }

private:
  ExprPtr condition_;
  StmtPtr thenBranch_;
  StmtPtr elseBranch_;
  std::optional<Token> constexprKeyword_;
};

class LoopControlStmt final : public StmtOfKind<StmtKind::LoopControlStmt> {
public:
  explicit LoopControlStmt(Token keyword) : keyword_(std::move(keyword)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitLoopControlStmt(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }

private:
  Token keyword_;
};

class NamespaceAliasDecl final
    : public StmtOfKind<StmtKind::NamespaceAliasDecl> {
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

class NamespaceDecl final : public StmtOfKind<StmtKind::NamespaceDecl> {
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

class ReturnStmt final : public StmtOfKind<StmtKind::ReturnStmt> {
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

class SwitchStmt final : public StmtOfKind<StmtKind::SwitchStmt> {
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

class TypeAliasDecl final : public StmtOfKind<StmtKind::TypeAliasDecl> {
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

class ConceptDecl final : public StmtOfKind<StmtKind::ConceptDecl> {
public:
  ConceptDecl(
      Token keyword, Token name, std::vector<Token> typeParameters,
      ConstraintExpression requirements,
      std::optional<CompilerConstraintBinding> compilerBinding = std::nullopt)
      : keyword_(std::move(keyword)), name_(std::move(name)),
        typeParameters_(std::move(typeParameters)),
        requirements_(std::move(requirements)),
        compilerBinding_(std::move(compilerBinding)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitConceptDecl(*this);
  }

  [[nodiscard]] const Token &keyword() const { return keyword_; }
  [[nodiscard]] const Token &name() const { return name_; }
  [[nodiscard]] const std::vector<Token> &typeParameters() const {
    return typeParameters_;
  }
  [[nodiscard]] const std::vector<ConceptApplication> &requirements() const {
    return requirements_.applications;
  }
  [[nodiscard]] const ConstraintExpression &constraintExpression() const {
    return requirements_;
  }
  [[nodiscard]] const std::optional<CompilerConstraintBinding> &
  compilerBinding() const {
    return compilerBinding_;
  }

private:
  Token keyword_;
  Token name_;
  std::vector<Token> typeParameters_;
  ConstraintExpression requirements_;
  std::optional<CompilerConstraintBinding> compilerBinding_;
};

class VariableDecl final : public StmtOfKind<StmtKind::VariableDecl> {
public:
  VariableDecl(Mutability mutability, TypeRef type, Token name,
               ExprPtr initializer,
               std::optional<Token> staticKeyword = std::nullopt,
               bool rangeBinding = false,
               std::optional<Token> constexprKeyword = std::nullopt,
               bool rangeOwner = false, bool rangeIterator = false,
               std::optional<Token> mutKeyword = std::nullopt)
      : mutability_(mutability), type_(std::move(type)), name_(std::move(name)),
        initializer_(std::move(initializer)),
        staticKeyword_(std::move(staticKeyword)), rangeBinding_(rangeBinding),
        constexprKeyword_(std::move(constexprKeyword)), rangeOwner_(rangeOwner),
        rangeIterator_(rangeIterator), mutKeyword_(std::move(mutKeyword)) {}

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
  [[nodiscard]] bool isRangeOwner() const { return rangeOwner_; }
  [[nodiscard]] bool isRangeIterator() const { return rangeIterator_; }
  [[nodiscard]] bool isConstexpr() const {
    return constexprKeyword_.has_value();
  }
  [[nodiscard]] const std::optional<Token> &constexprKeyword() const {
    return constexprKeyword_;
  }
  [[nodiscard]] const std::optional<Token> &mutKeyword() const {
    return mutKeyword_;
  }

private:
  Mutability mutability_;
  TypeRef type_;
  Token name_;
  ExprPtr initializer_;
  std::optional<Token> staticKeyword_;
  bool rangeBinding_ = false;
  std::optional<Token> constexprKeyword_;
  bool rangeOwner_ = false;
  bool rangeIterator_ = false;
  std::optional<Token> mutKeyword_;
};

class StructuredBindingDecl final
    : public StmtOfKind<StmtKind::StructuredBindingDecl> {
public:
  StructuredBindingDecl(Token autoKeyword, Token leftBracket,
                        std::vector<VariableDecl> bindings, Token rightBracket,
                        Token equal, ExprPtr initializer)
      : autoKeyword_(std::move(autoKeyword)),
        leftBracket_(std::move(leftBracket)), bindings_(std::move(bindings)),
        rightBracket_(std::move(rightBracket)), equal_(std::move(equal)),
        initializer_(std::move(initializer)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitStructuredBindingDecl(*this);
  }

  [[nodiscard]] const Token &autoKeyword() const { return autoKeyword_; }
  [[nodiscard]] const Token &leftBracket() const { return leftBracket_; }
  [[nodiscard]] const std::vector<VariableDecl> &bindings() const {
    return bindings_;
  }
  [[nodiscard]] const Token &rightBracket() const { return rightBracket_; }
  [[nodiscard]] const Token &equal() const { return equal_; }
  [[nodiscard]] const ExprPtr &initializer() const { return initializer_; }

private:
  Token autoKeyword_;
  Token leftBracket_;
  std::vector<VariableDecl> bindings_;
  Token rightBracket_;
  Token equal_;
  ExprPtr initializer_;
};

class DoWhileStmt final : public StmtOfKind<StmtKind::DoWhileStmt> {
public:
  DoWhileStmt(StmtPtr body, ExprPtr condition)
      : body_(std::move(body)), condition_(std::move(condition)) {}

  void accept(StmtVisitor &visitor) const override {
    visitor.visitDoWhileStmt(*this);
  }

  [[nodiscard]] const StmtPtr &body() const { return body_; }
  [[nodiscard]] const ExprPtr &condition() const { return condition_; }

private:
  StmtPtr body_;
  ExprPtr condition_;
};

class WhileStmt final : public StmtOfKind<StmtKind::WhileStmt> {
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
      : declarations_(std::move(declarations)), snapshotId_(nextSnapshotId()) {}

  Program(const Program &) = delete;
  Program &operator=(const Program &) = delete;

  Program(Program &&other) noexcept
      : declarations_(std::move(other.declarations_)),
        snapshotId_(std::exchange(other.snapshotId_, nextSnapshotId())) {}

  Program &operator=(Program &&other) noexcept {
    if (this != &other) {
      declarations_ = std::move(other.declarations_);
      snapshotId_ = std::exchange(other.snapshotId_, nextSnapshotId());
    }
    return *this;
  }

  [[nodiscard]] const StmtList &declarations() const { return declarations_; }

  [[nodiscard]] std::uint64_t snapshotId() const { return snapshotId_; }

  [[nodiscard]] StmtList takeDeclarations() { return std::move(declarations_); }

private:
  [[nodiscard]] static std::uint64_t nextSnapshotId() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
  }

  StmtList declarations_;
  std::uint64_t snapshotId_ = 0;
};

} // namespace lang
