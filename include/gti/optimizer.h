#pragma once

#include "gti/ast.h"
#include "gti/semantic_analyzer.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

namespace lang {

enum class OptimizationLevel {
  O0,
  O1,
  O2,
  O3,
};

struct IntegerConstant {
  bool negative = false;
  std::uint64_t magnitude = 0;
  SemanticType type = SemanticType::Unknown;

  friend bool operator==(const IntegerConstant &,
                         const IntegerConstant &) = default;
};

struct NullConstant {
  friend bool operator==(NullConstant, NullConstant) = default;
};

using ConstantValue =
    std::variant<IntegerConstant, double, std::string, bool, NullConstant>;

class OptimizationResult {
public:
  [[nodiscard]] const ConstantValue *replacement(const Expr &expression) const {
    const auto found = constants.find(&expression);
    return found == constants.end() ? nullptr : &found->second;
  }

  [[nodiscard]] std::size_t foldedExpressionCount() const {
    return constants.size();
  }

  void setReplacement(const Expr &expression, ConstantValue value) {
    constants.insert_or_assign(&expression, std::move(value));
  }

private:
  std::unordered_map<const Expr *, ConstantValue> constants;
};

struct OptimizationContext {
  const Program &program;
  const SemanticModel &semantics;
  OptimizationLevel level;
  TargetInfo target;
};

class OptimizationPass {
public:
  OptimizationPass() = default;
  OptimizationPass(const OptimizationPass &) = delete;
  OptimizationPass &operator=(const OptimizationPass &) = delete;
  virtual ~OptimizationPass() = default;

  [[nodiscard]] virtual std::string_view name() const = 0;
  virtual void run(const OptimizationContext &context,
                   OptimizationResult &result) = 0;
};

class ConstantFoldingPass final : public OptimizationPass,
                                  public ExprVisitor,
                                  public StmtVisitor {
public:
  [[nodiscard]] std::string_view name() const override {
    return "constant-folding";
  }

  void run(const OptimizationContext &context,
           OptimizationResult &output) override {
    current.reset();
    result = &output;
    semantics = &context.semantics;
    target = context.target;
    analyze(context.program.declarations());
  }

  void visitAccessSpecifierDecl(const AccessSpecifierDecl &) override {}

  void visitBlockStmt(const BlockStmt &stmt) override {
    analyze(stmt.statements());
  }

  void visitClassDecl(const ClassDecl &stmt) override {
    analyze(stmt.members());
  }

  void visitConditionalStmt(const ConditionalStmt &stmt) override {
    if (const StmtList *branch = stmt.activeBranch(target)) {
      analyze(*branch);
    }
  }

  void visitConstructorDecl(const ConstructorDecl &stmt) override {
    for (const ConstructorInitializer &initializer : stmt.initializers()) {
      evaluate(initializer.value);
    }
    analyze(stmt.body());
  }

  void visitEmptyStmt(const EmptyStmt &) override {}

  void visitExpressionStmt(const ExpressionStmt &stmt) override {
    evaluate(stmt.expression());
  }

  void visitForStmt(const ForStmt &stmt) override {
    analyze(stmt.initializer());
    evaluate(stmt.condition());
    evaluate(stmt.increment());
    analyze(stmt.body());
  }

  void visitFunctionDecl(const FunctionDecl &stmt) override {
    analyze(stmt.body());
  }

  void visitIfStmt(const IfStmt &stmt) override {
    evaluate(stmt.condition());
    analyze(stmt.thenBranch());
    analyze(stmt.elseBranch());
  }

  void visitNamespaceAliasDecl(const NamespaceAliasDecl &) override {}

  void visitNamespaceDecl(const NamespaceDecl &stmt) override {
    analyze(stmt.declarations());
  }

  void visitReturnStmt(const ReturnStmt &stmt) override {
    evaluate(stmt.value());
  }

  void visitVariableDecl(const VariableDecl &stmt) override {
    evaluate(stmt.initializer());
  }

  void visitWhileStmt(const WhileStmt &stmt) override {
    evaluate(stmt.condition());
    analyze(stmt.body());
  }

  void visitAssignExpr(const Assign &expr) override {
    evaluate(expr.value());
    current.reset();
  }

  void visitBinaryExpr(const Binary &expr) override {
    const std::optional<ConstantValue> left = evaluate(expr.left());
    const std::optional<ConstantValue> right = evaluate(expr.right());
    current = foldComparison(expr.oper().kind, left, right);
  }

  void visitCallExpr(const Call &expr) override {
    evaluate(expr.callee());
    for (const ExprPtr &argument : expr.arguments()) {
      evaluate(argument);
    }
    current.reset();
  }

  void visitGetExpr(const Get &expr) override {
    evaluate(expr.object());
    current.reset();
  }

  void visitGroupingExpr(const Grouping &expr) override {
    current = evaluate(expr.expression());
  }

  void visitLiteralExpr(const LiteralExpr &expr) override {
    const Literal &literal = expr.value();
    if (const auto *value = std::get_if<std::uint64_t>(&literal)) {
      SemanticType type = semantics->typeOf(expr);
      if (type == SemanticType::Unknown) {
        type = *value <= static_cast<std::uint64_t>(
                             std::numeric_limits<std::int32_t>::max())
                   ? SemanticType::Int32
               : *value <= static_cast<std::uint64_t>(
                               std::numeric_limits<std::int64_t>::max())
                   ? SemanticType::Int64
                   : SemanticType::UInt64;
      }
      current = IntegerConstant{.magnitude = *value, .type = type};
    } else if (const auto *value = std::get_if<double>(&literal)) {
      current = *value;
    } else if (const auto *value = std::get_if<std::string>(&literal)) {
      current = *value;
    } else if (const auto *value = std::get_if<bool>(&literal)) {
      current = *value;
    } else if (std::holds_alternative<std::nullptr_t>(literal)) {
      current = NullConstant{};
    } else {
      current.reset();
    }
  }

  void visitLogicalExpr(const Logical &expr) override {
    const std::optional<ConstantValue> left = evaluate(expr.left());
    const bool *leftBool = constant<bool>(left);
    if (leftBool != nullptr && expr.oper().kind == TokenKind::AND &&
        !*leftBool) {
      evaluate(expr.right());
      current = false;
      return;
    }
    if (leftBool != nullptr && expr.oper().kind == TokenKind::OR && *leftBool) {
      evaluate(expr.right());
      current = true;
      return;
    }

    const std::optional<ConstantValue> right = evaluate(expr.right());
    const bool *rightBool = constant<bool>(right);
    if (leftBool == nullptr || rightBool == nullptr) {
      current.reset();
      return;
    }
    current = expr.oper().kind == TokenKind::AND ? *leftBool && *rightBool
                                                 : *leftBool || *rightBool;
  }

  void visitPostfixExpr(const Postfix &expr) override {
    evaluate(expr.expression());
    current.reset();
  }

  void visitQualifiedNameExpr(const QualifiedName &) override {
    current.reset();
  }

  void visitSelfExpr(const Self &) override { current.reset(); }

  void visitSetExpr(const Set &expr) override {
    evaluate(expr.object());
    evaluate(expr.value());
    current.reset();
  }

  void visitUnaryExpr(const Unary &expr) override {
    const std::optional<ConstantValue> right = evaluate(expr.right());
    if (!right) {
      current.reset();
      return;
    }

    if (expr.oper().kind == TokenKind::BANG) {
      if (const bool *value = constant<bool>(right)) {
        current = !*value;
      } else {
        current.reset();
      }
      return;
    }
    if (expr.oper().kind == TokenKind::PLUS) {
      if (const auto *value = constant<IntegerConstant>(right)) {
        IntegerConstant folded = *value;
        folded.type = semantics->typeOf(expr);
        current = folded;
      } else if (const auto *value = constant<double>(right)) {
        current = *value;
      } else {
        current.reset();
      }
      return;
    }
    if (expr.oper().kind == TokenKind::MINUS) {
      if (const auto *value = constant<IntegerConstant>(right)) {
        IntegerConstant folded = *value;
        folded.negative = folded.magnitude != 0 && !folded.negative;
        folded.type = semantics->typeOf(expr);
        current = folded;
      } else if (const auto *value = constant<double>(right)) {
        current = -*value;
      } else {
        current.reset();
      }
      return;
    }
    current.reset();
  }

  void visitUnexpectedExpr(const Unexpected &expr) override {
    evaluate(expr.error());
    current.reset();
  }

  void visitVariableExpr(const Variable &) override { current.reset(); }

private:
  template <typename Value>
  [[nodiscard]] static const Value *
  constant(const std::optional<ConstantValue> &value) {
    return value ? std::get_if<Value>(&*value) : nullptr;
  }

  void analyze(const StmtList &statements) {
    for (const StmtPtr &statement : statements) {
      analyze(statement);
    }
  }

  void analyze(const StmtPtr &statement) {
    if (statement) {
      statement->accept(*this);
    }
  }

  void analyze(const std::unique_ptr<BlockStmt> &block) {
    if (block) {
      block->accept(*this);
    }
  }

  std::optional<ConstantValue> evaluate(const ExprPtr &expression) {
    return expression ? evaluate(*expression) : std::nullopt;
  }

  std::optional<ConstantValue> evaluate(const Expr &expression) {
    current.reset();
    expression.accept(*this);
    std::optional<ConstantValue> value = current;
    if (value && dynamic_cast<const LiteralExpr *>(&expression) == nullptr) {
      result->setReplacement(expression, *value);
    }
    return value;
  }

  [[nodiscard]] static int compare(const IntegerConstant &left,
                                   const IntegerConstant &right) {
    if (left.negative != right.negative) {
      return left.negative ? -1 : 1;
    }
    if (left.magnitude == right.magnitude) {
      return 0;
    }
    if (left.negative) {
      return left.magnitude > right.magnitude ? -1 : 1;
    }
    return left.magnitude < right.magnitude ? -1 : 1;
  }

  [[nodiscard]] static std::optional<ConstantValue>
  foldComparison(TokenKind oper, const std::optional<ConstantValue> &left,
                 const std::optional<ConstantValue> &right) {
    if (!left || !right) {
      return std::nullopt;
    }

    std::optional<int> ordering;
    if (const auto *leftInteger = constant<IntegerConstant>(left)) {
      if (const auto *rightInteger = constant<IntegerConstant>(right)) {
        ordering = compare(*leftInteger, *rightInteger);
      }
    } else if (const auto *leftFloat = constant<double>(left)) {
      if (const auto *rightFloat = constant<double>(right)) {
        if (std::isnan(*leftFloat) || std::isnan(*rightFloat)) {
          switch (oper) {
          case TokenKind::EQUAL_EQUAL:
            return false;
          case TokenKind::BANG_EQUAL:
            return true;
          case TokenKind::LESS:
          case TokenKind::LESS_EQUAL:
          case TokenKind::GREATER:
          case TokenKind::GREATER_EQUAL:
            return false;
          default:
            return std::nullopt;
          }
        }
        if (*leftFloat < *rightFloat) {
          ordering = -1;
        } else if (*leftFloat > *rightFloat) {
          ordering = 1;
        } else {
          ordering = 0;
        }
      }
    } else if (const auto *leftString = constant<std::string>(left)) {
      if (const auto *rightString = constant<std::string>(right)) {
        ordering = leftString->compare(*rightString);
      }
    } else if (const auto *leftBool = constant<bool>(left)) {
      if (const auto *rightBool = constant<bool>(right)) {
        ordering = *leftBool == *rightBool ? 0 : (*leftBool ? 1 : -1);
      }
    } else if (constant<NullConstant>(left) != nullptr &&
               constant<NullConstant>(right) != nullptr) {
      ordering = 0;
    }

    if (!ordering) {
      return std::nullopt;
    }
    switch (oper) {
    case TokenKind::EQUAL_EQUAL:
      return *ordering == 0;
    case TokenKind::BANG_EQUAL:
      return *ordering != 0;
    case TokenKind::LESS:
      return *ordering < 0;
    case TokenKind::LESS_EQUAL:
      return *ordering <= 0;
    case TokenKind::GREATER:
      return *ordering > 0;
    case TokenKind::GREATER_EQUAL:
      return *ordering >= 0;
    default:
      return std::nullopt;
    }
  }

  OptimizationResult *result = nullptr;
  std::optional<ConstantValue> current;
  const SemanticModel *semantics = nullptr;
  TargetInfo target;
};

class OptimizationPipeline {
public:
  [[nodiscard]] OptimizationResult
  run(const Program &program, const SemanticModel &semanticModel,
      OptimizationLevel level, TargetInfo target = TargetInfo::host()) const {
    OptimizationResult result;
    if (level == OptimizationLevel::O0) {
      return result;
    }

    const OptimizationContext context{.program = program,
                                      .semantics = semanticModel,
                                      .level = level,
                                      .target = std::move(target)};
    ConstantFoldingPass().run(context, result);
    return result;
  }
};

} // namespace lang
