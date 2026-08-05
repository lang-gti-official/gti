#pragma once

#include "gti/ast.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace lang {

struct SemanticType {
  enum Kind {
    Unknown,
    Void,
    Int8,
    Int16,
    Int32,
    Int64,
    Float,
    Bool,
    String,
    NullPtr,
    Object,
    Function,
    Expected,
    Unexpected,
  };

  SemanticType(Kind kind = Unknown) : kind(kind) {}
  SemanticType(Kind kind, std::vector<SemanticType> arguments)
      : kind(kind), arguments(std::move(arguments)) {}

  friend bool operator==(const SemanticType &, const SemanticType &) = default;

  Kind kind;
  std::vector<SemanticType> arguments;
};

struct SemanticDiagnostic {
  Token token;
  std::string message;
};

class SemanticVisitor final : public ExprVisitor, public StmtVisitor {
public:
  explicit SemanticVisitor(TargetInfo target = TargetInfo::host())
      : target(std::move(target)) {}

  bool check(const Program &program) {
    diagnostics.clear();
    scopes.clear();
    namespaces.clear();
    namespaceAliases.clear();
    namespaceSymbols.clear();
    currentNamespace.clear();
    predeclaredVariables.clear();
    expressionTypes.clear();
    classDepth = 0;
    functionDepth = 0;
    currentReturnType = SemanticType::Unknown;

    registerNamespaces(program.declarations(), {});
    registerNamespaceSymbols(program.declarations(), {});
    beginScope();
    analyze(program.declarations());
    endScope();
    return !hadError();
  }

  bool check(const Expr &expr) {
    diagnostics.clear();
    scopes.clear();
    namespaces.clear();
    namespaceAliases.clear();
    namespaceSymbols.clear();
    currentNamespace.clear();
    expressionTypes.clear();
    beginScope();
    analyze(expr);
    endScope();
    return !hadError();
  }

  [[nodiscard]] bool hadError() const { return !diagnostics.empty(); }

  [[nodiscard]] const std::vector<SemanticDiagnostic> &errors() const {
    return diagnostics;
  }

  [[nodiscard]] SemanticType expressionType() const { return currentType; }

  void visitBlockStmt(const BlockStmt &stmt) override {
    beginScope();
    analyze(stmt.statements());
    endScope();
  }

  void visitClassDecl(const ClassDecl &stmt) override {
    ++classDepth;
    beginScope();
    predeclare(stmt.members(), true);
    analyze(stmt.members());
    endScope();
    --classDepth;
  }

  void visitConditionalStmt(const ConditionalStmt &stmt) override {
    if (const StmtList *branch = stmt.activeBranch(target)) {
      analyze(*branch);
    }
  }

  void visitEmptyStmt(const EmptyStmt &) override {}

  void visitExpressionStmt(const ExpressionStmt &stmt) override {
    const SemanticType resultType = analyze(stmt.expression());
    const Call *call = directCall(stmt.expression());

    if (stmt.discardAttribute()) {
      if (call == nullptr) {
        report(*stmt.discardAttribute(),
               "'[[discard]]' can only be applied to a function call.");
      } else if (resultType == SemanticType::Void) {
        report(*stmt.discardAttribute(),
               "'[[discard]]' cannot be applied to a void function call.");
      }
      return;
    }

    if (call != nullptr && resultType != SemanticType::Void &&
        resultType != SemanticType::Unknown) {
      report(call->paren(),
             "Function return value must be used or explicitly discarded "
             "with '[[discard]]'.");
    }
  }

  void visitForStmt(const ForStmt &stmt) override {
    beginScope();
    analyze(stmt.initializer());
    if (stmt.condition()) {
      requireBool(analyze(stmt.condition()), expressionToken(stmt.condition()),
                  "For-loop condition must be bool.");
    }
    analyze(stmt.increment());
    analyze(stmt.body());
    endScope();
  }

  void visitFunctionDecl(const FunctionDecl &stmt) override {
    validateRuntimeBinding(stmt);
    validateType(stmt.returnType());
    for (const Parameter &parameter : stmt.parameters()) {
      validateType(parameter.type);
      if (typeOf(parameter.type) == SemanticType::Void) {
        report(parameter.type.name.last(),
               "Function parameters cannot have type void.");
      }
    }
    if (!stmt.body()) {
      return;
    }

    const SemanticType enclosingReturnType = currentReturnType;
    currentReturnType = typeOf(stmt.returnType());
    ++functionDepth;
    beginScope();

    for (const Parameter &parameter : stmt.parameters()) {
      if (!parameter.name.lexeme.empty()) {
        declare(parameter.name, typeOf(parameter.type),
                parameter.mutability == Mutability::Mutable);
      }
    }

    // A function body owns the parameter scope, so do not add another scope.
    analyze(stmt.body()->statements());
    endScope();
    --functionDepth;
    currentReturnType = enclosingReturnType;
  }

  void visitIfStmt(const IfStmt &stmt) override {
    requireBool(analyze(stmt.condition()), expressionToken(stmt.condition()),
                "If condition must be bool.");
    analyze(stmt.thenBranch());
    analyze(stmt.elseBranch());
  }

  void visitNamespaceAliasDecl(const NamespaceAliasDecl &stmt) override {
    const std::optional<std::string> target =
        resolveNamespacePath(stmt.target());
    if (!target) {
      report(stmt.target().last(), "Unknown namespace in alias target.");
      return;
    }

    const std::string alias = qualifiedName(currentNamespace, stmt.name().lexeme);
    if (namespaces.contains(alias) || namespaceSymbols.contains(alias) ||
        namespaceAliases.contains(alias)) {
      report(stmt.name(), "Duplicate declaration of '" + stmt.name().lexeme +
                              "'.");
      return;
    }
    namespaceAliases.emplace(alias, *target);
  }

  void visitNamespaceDecl(const NamespaceDecl &stmt) override {
    currentNamespace.emplace_back(stmt.name().lexeme);
    analyze(stmt.declarations());
    currentNamespace.pop_back();
  }

  void visitReturnStmt(const ReturnStmt &stmt) override {
    if (functionDepth == 0) {
      report(stmt.keyword(), "Cannot return from outside a function.");
      return;
    }
    if (currentReturnType == SemanticType::Void) {
      if (stmt.value()) {
        analyze(stmt.value());
        report(stmt.keyword(), "Void function cannot return a value.");
      }
      return;
    }
    if (!stmt.value()) {
      if (isExpectedVoid(currentReturnType)) {
        return;
      }
      report(stmt.keyword(), "A value is required for this return type.");
      return;
    }

    const SemanticType valueType = analyze(stmt.value());
    if (!isAssignable(currentReturnType, valueType, stmt.value().get())) {
      report(stmt.keyword(), "Return value does not match the function type.");
    }
  }

  void visitVariableDecl(const VariableDecl &stmt) override {
    validateType(stmt.type());
    const SemanticType declaredType = typeOf(stmt.type());
    SemanticType initializerType = SemanticType::Unknown;
    if (declaredType == SemanticType::Void) {
      report(stmt.type().name.last(), "Variables cannot have type void.");
    } else if (!stmt.isMutable() && !stmt.initializer()) {
      report(stmt.name(), "Immutable variable must have an initializer.");
    }
    if (stmt.initializer()) {
      initializerType = analyze(stmt.initializer());
    }

    if (!predeclaredVariables.contains(&stmt)) {
      if (functionDepth == 0 && classDepth == 0) {
        declareNamespaceSymbol(currentNamespace, stmt.name(), declaredType,
                               stmt.isMutable());
      } else {
        declare(stmt.name(), declaredType, stmt.isMutable());
      }
    }

    if (stmt.initializer() &&
        !isAssignable(declaredType, initializerType,
                      stmt.initializer().get())) {
      report(stmt.name(), "Initializer does not match the declared type.");
    }
  }

  void visitWhileStmt(const WhileStmt &stmt) override {
    requireBool(analyze(stmt.condition()), expressionToken(stmt.condition()),
                "While condition must be bool.");
    analyze(stmt.body());
  }

  void visitAssignExpr(const Assign &expr) override {
    const SemanticType valueType = analyze(expr.value());
    const Symbol *symbol = resolve(expr.name());
    if (symbol == nullptr) {
      report(expr.name(), "Undefined variable '" + expr.name().lexeme + "'.");
      currentType = valueType;
      return;
    }
    if (!symbol->assignable) {
      report(expr.name(), "Name is not assignable.");
    }
    if (!isAssignable(symbol->type, valueType, expr.value().get())) {
      report(expr.oper(), "Assigned value does not match the variable type.");
    }
    if (expr.oper().kind != TokenKind::EQUAL &&
        ((symbol->type != SemanticType::Unknown && !isNumeric(symbol->type)) ||
         (valueType != SemanticType::Unknown && !isNumeric(valueType)))) {
      report(expr.oper(), "Compound assignment requires numeric operands.");
    }
    currentType = symbol->type;
  }

  void visitBinaryExpr(const Binary &expr) override {
    const SemanticType leftType = analyze(expr.left());
    const SemanticType rightType = analyze(expr.right());

    switch (expr.oper().kind) {
    case TokenKind::COMMA:
      currentType = rightType;
      return;
    case TokenKind::EQUAL_EQUAL:
    case TokenKind::BANG_EQUAL:
      if (!isComparable(leftType, rightType)) {
        report(expr.oper(), "Equality operands have incompatible types.");
      }
      currentType = SemanticType::Bool;
      return;
    case TokenKind::GREATER:
    case TokenKind::GREATER_EQUAL:
    case TokenKind::LESS:
    case TokenKind::LESS_EQUAL:
      requireNumeric(leftType, rightType, expr.oper());
      currentType = SemanticType::Bool;
      return;
    case TokenKind::PLUS:
    case TokenKind::MINUS:
    case TokenKind::STAR:
    case TokenKind::SLASH:
      requireNumeric(leftType, rightType, expr.oper());
      currentType = numericResult(leftType, rightType);
      return;
    default:
      currentType = SemanticType::Unknown;
    }
  }

  void visitCallExpr(const Call &expr) override {
    const SemanticType calleeType = analyze(expr.callee());
    std::vector<SemanticType> argumentTypes;
    argumentTypes.reserve(expr.arguments().size());
    for (const ExprPtr &argument : expr.arguments()) {
      argumentTypes.emplace_back(analyze(argument));
    }

    if (const auto *member =
            dynamic_cast<const Get *>(expr.callee().get())) {
      const auto objectType = expressionTypes.find(member->object().get());
      if (objectType != expressionTypes.end() &&
          objectType->second.kind == SemanticType::Expected) {
        analyzeExpectedMemberCall(*member, objectType->second, argumentTypes,
                                  expr.arguments(), expr.paren());
        return;
      }
    }

    const Symbol *callee = resolveExpressionSymbol(expr.callee());

    if (calleeType != SemanticType::Unknown &&
        calleeType != SemanticType::Function) {
      report(expr.paren(), "Can only call functions.");
      currentType = SemanticType::Unknown;
      return;
    }
    if (callee != nullptr && callee->type == SemanticType::Function) {
      if (argumentTypes.size() != callee->parameterTypes.size()) {
        report(expr.paren(), "Function called with the wrong number of arguments.");
      } else {
        for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
          if (!isAssignable(callee->parameterTypes[index], argumentTypes[index],
                            expr.arguments()[index].get())) {
            report(expressionToken(expr.arguments()[index]),
                   "Argument does not match the parameter type.");
          }
        }
      }
      currentType = callee->returnType;
      return;
    }
    currentType = SemanticType::Unknown;
  }

  void visitGetExpr(const Get &expr) override {
    const SemanticType objectType = analyze(expr.object());
    if (dynamic_cast<const Self *>(expr.object().get()) != nullptr) {
      const Symbol *member = resolve(expr.name());
      if (member == nullptr) {
        report(expr.name(), "Undefined member '" + expr.name().lexeme + "'.");
        currentType = SemanticType::Unknown;
      } else {
        currentType = member->type;
      }
      return;
    }
    if (objectType.kind == SemanticType::Expected) {
      if (expr.name().lexeme == "has_value" || expr.name().lexeme == "value" ||
          expr.name().lexeme == "error" || expr.name().lexeme == "value_or") {
        currentType = SemanticType::Function;
      } else {
        report(expr.name(), "Unknown expected member '" + expr.name().lexeme +
                                "'.");
        currentType = SemanticType::Unknown;
      }
      return;
    }
    // Member tables are intentionally deferred until user-defined types carry
    // identities instead of the current coarse Object type.
    currentType = SemanticType::Unknown;
  }

  void visitGroupingExpr(const Grouping &expr) override {
    currentType = analyze(expr.expression());
  }

  void visitLiteralExpr(const LiteralExpr &expr) override {
    currentType = literalType(expr.value());
    if (const auto *value = std::get_if<std::uint64_t>(&expr.value());
        value != nullptr &&
        *value >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
      report(expr.token(), "Integer literal exceeds the int64 range.");
    }
  }

  void visitLogicalExpr(const Logical &expr) override {
    const SemanticType leftType = analyze(expr.left());
    const SemanticType rightType = analyze(expr.right());
    if ((leftType != SemanticType::Unknown && !isContextuallyBool(leftType)) ||
        (rightType != SemanticType::Unknown &&
         !isContextuallyBool(rightType))) {
      report(expr.oper(), "Logical operands must be bool.");
    }
    currentType = SemanticType::Bool;
  }

  void visitPostfixExpr(const Postfix &expr) override {
    const SemanticType type = analyze(expr.expression());
    if (type != SemanticType::Unknown && !isNumeric(type)) {
      report(expr.oper(), "Increment and decrement require a numeric value.");
    }
    if (!isMutableTarget(expr.expression())) {
      report(expr.oper(), "Increment and decrement require an assignable value.");
    }
    currentType = type;
  }

  void visitQualifiedNameExpr(const QualifiedName &expr) override {
    const Symbol *symbol = resolveQualified(expr.name());
    if (symbol == nullptr) {
      report(expr.name().last(),
             "Undefined qualified name '" + pathSpelling(expr.name()) + "'.");
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = symbol->type;
  }

  void visitSelfExpr(const Self &expr) override {
    if (classDepth == 0) {
      report(expr.keyword(), "Cannot use 'self' outside a class.");
    }
    currentType = SemanticType::Object;
  }

  void visitSetExpr(const Set &expr) override {
    analyze(expr.object());
    const SemanticType valueType = analyze(expr.value());

    if (dynamic_cast<const Self *>(expr.object().get()) != nullptr) {
      const Symbol *member = resolve(expr.name());
      if (member == nullptr) {
        report(expr.name(), "Undefined member '" + expr.name().lexeme + "'.");
      } else {
        if (!member->assignable) {
          report(expr.name(), "Member is immutable.");
        }
        if (!isAssignable(member->type, valueType, expr.value().get())) {
          report(expr.oper(), "Assigned value does not match the member type.");
        }
        if (expr.oper().kind != TokenKind::EQUAL &&
            ((member->type != SemanticType::Unknown &&
              !isNumeric(member->type)) ||
             (valueType != SemanticType::Unknown &&
              !isNumeric(valueType)))) {
          report(expr.oper(), "Compound assignment requires numeric operands.");
        }
        currentType = member->type;
        return;
      }
    }

    currentType = valueType;
  }

  void visitUnaryExpr(const Unary &expr) override {
    if (expr.oper().kind == TokenKind::MINUS) {
      if (const auto *literal =
              dynamic_cast<const LiteralExpr *>(expr.right().get());
          literal != nullptr) {
        const auto *magnitude = std::get_if<std::uint64_t>(&literal->value());
        if (magnitude != nullptr &&
            *magnitude == (std::uint64_t{1} << 63U)) {
          currentType = SemanticType::Int64;
          return;
        }
      }
    }
    const SemanticType rightType = analyze(expr.right());

    if (expr.oper().kind == TokenKind::BANG) {
      requireBool(rightType, expr.oper(), "Logical negation requires bool.");
      currentType = SemanticType::Bool;
      return;
    }

    if (rightType != SemanticType::Unknown && !isNumeric(rightType)) {
      report(expr.oper(), "Unary operator requires a numeric value.");
    }
    if ((expr.oper().kind == TokenKind::PLUS_PLUS ||
         expr.oper().kind == TokenKind::MINUS_MINUS) &&
        !isMutableTarget(expr.right())) {
      report(expr.oper(), "Increment and decrement require an assignable value.");
    }
    currentType = rightType;
  }

  void visitUnexpectedExpr(const Unexpected &expr) override {
    currentType = SemanticType(
        SemanticType::Unexpected, {analyze(expr.error())});
  }

  void visitVariableExpr(const Variable &expr) override {
    const Symbol *symbol = resolve(expr.name());
    if (symbol == nullptr) {
      report(expr.name(), "Undefined name '" + expr.name().lexeme + "'.");
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = symbol->type;
  }

private:
  [[nodiscard]] static const Call *directCall(const ExprPtr &expression) {
    const Expr *candidate = expression.get();
    while (const auto *grouping = dynamic_cast<const Grouping *>(candidate)) {
      candidate = grouping->expression().get();
    }
    return dynamic_cast<const Call *>(candidate);
  }

  void analyzeExpectedMemberCall(
      const Get &member, const SemanticType &expected,
      const std::vector<SemanticType> &argumentTypes,
      const ExprList &arguments, const Token &paren) {
    const auto requireArity = [&](std::size_t expectedCount) {
      if (argumentTypes.size() != expectedCount) {
        report(paren, "Expected member called with the wrong number of arguments.");
        return false;
      }
      return true;
    };

    if (member.name().lexeme == "has_value") {
      requireArity(0);
      currentType = SemanticType::Bool;
      return;
    }
    if (member.name().lexeme == "value") {
      requireArity(0);
      currentType = expected.arguments[0];
      return;
    }
    if (member.name().lexeme == "error") {
      requireArity(0);
      currentType = expected.arguments[1];
      return;
    }
    if (member.name().lexeme == "value_or") {
      if (expected.arguments[0] == SemanticType::Void) {
        report(member.name(),
               "expected<void, E> does not provide 'value_or'.");
        currentType = SemanticType::Unknown;
        return;
      }
      if (requireArity(1) &&
          !isAssignable(expected.arguments[0], argumentTypes[0],
                        arguments[0].get())) {
        report(member.name(), "value_or fallback does not match the value type.");
      }
      currentType = expected.arguments[0];
    }
  }

  void validateType(const TypeRef &type) {
    for (const TypeRef &argument : type.arguments) {
      validateType(argument);
    }
    if (type.name.last().kind == TokenKind::EXPECTED &&
        (type.arguments.size() != 2 ||
         typeOf(type.arguments[1]) == SemanticType::Void)) {
      report(type.name.last(),
             "expected<T, E> requires a non-void error type.");
    }
  }

  struct Symbol {
    SemanticType type = SemanticType::Unknown;
    bool assignable = false;
    SemanticType returnType = SemanticType::Unknown;
    std::vector<SemanticType> parameterTypes;
  };

  static std::string qualifiedName(const std::vector<std::string> &scope,
                                   std::string_view name) {
    std::string result;
    for (const std::string &segment : scope) {
      if (!result.empty()) {
        result += "::";
      }
      result += segment;
    }
    if (!result.empty()) {
      result += "::";
    }
    result += name;
    return result;
  }

  static std::string pathSpelling(const NamePath &path) {
    std::string result;
    for (const Token &segment : path.segments) {
      if (!result.empty()) {
        result += "::";
      }
      result += segment.lexeme;
    }
    return result;
  }

  static Symbol functionSymbol(const FunctionDecl &function) {
    Symbol symbol{.type = SemanticType::Function,
                  .assignable = false,
                  .returnType = typeOf(function.returnType())};
    symbol.parameterTypes.reserve(function.parameters().size());
    for (const Parameter &parameter : function.parameters()) {
      symbol.parameterTypes.emplace_back(typeOf(parameter.type));
    }
    return symbol;
  }

  void validateRuntimeBinding(const FunctionDecl &function) {
    if (!function.runtimeBinding()) {
      return;
    }

    const RuntimeBinding &binding = *function.runtimeBinding();
    const bool valid =
        binding.name == "stdout.write" &&
        qualifiedName(currentNamespace, function.name().lexeme) ==
            "gti_internal::runtime::write_stdout" &&
        typeOf(function.returnType()) == SemanticType::Void &&
        function.parameters().size() == 1 &&
        typeOf(function.parameters().front().type) == SemanticType::String &&
        function.parameters().front().mutability == Mutability::Immutable &&
        !function.body();
    if (!valid) {
      report(binding.attribute,
             "Invalid declaration for runtime binding '" + binding.name +
                 "'.");
    }
  }

  void registerNamespaces(const StmtList &statements,
                          std::vector<std::string> scope) {
    for (const StmtPtr &statement : statements) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          registerNamespaces(*branch, scope);
        }
        continue;
      }
      const auto *namespaceDecl =
          dynamic_cast<const NamespaceDecl *>(statement.get());
      if (namespaceDecl == nullptr) {
        continue;
      }
      const std::string name =
          qualifiedName(scope, namespaceDecl->name().lexeme);
      namespaces.insert(name);
      scope.emplace_back(namespaceDecl->name().lexeme);
      registerNamespaces(namespaceDecl->declarations(), scope);
      scope.pop_back();
    }
  }

  void registerNamespaceSymbols(const StmtList &statements,
                                std::vector<std::string> scope) {
    for (const StmtPtr &statement : statements) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          registerNamespaceSymbols(*branch, scope);
        }
      } else if (const auto *function =
              dynamic_cast<const FunctionDecl *>(statement.get())) {
        declareNamespaceSymbol(scope, function->name(),
                               functionSymbol(*function));
      } else if (const auto *classDecl =
                     dynamic_cast<const ClassDecl *>(statement.get())) {
        declareNamespaceSymbol(scope, classDecl->name(), SemanticType::Object,
                               false);
      } else if (const auto *namespaceDecl =
                     dynamic_cast<const NamespaceDecl *>(statement.get())) {
        scope.emplace_back(namespaceDecl->name().lexeme);
        registerNamespaceSymbols(namespaceDecl->declarations(), scope);
        scope.pop_back();
      }
    }
  }

  void analyze(const StmtList &statements) {
    for (const StmtPtr &statement : statements) {
      analyze(statement);
    }
  }

  void analyze(const StmtPtr &stmt) {
    if (stmt) {
      stmt->accept(*this);
    }
  }

  SemanticType analyze(const Expr &expr) {
    expr.accept(*this);
    expressionTypes.insert_or_assign(&expr, currentType);
    return currentType;
  }

  SemanticType analyze(const ExprPtr &expr) {
    return expr ? analyze(*expr) : SemanticType::Unknown;
  }

  void predeclare(const StmtList &statements, bool includeVariables) {
    for (const StmtPtr &statement : statements) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          predeclare(*branch, includeVariables);
        }
      } else if (const auto *function =
              dynamic_cast<const FunctionDecl *>(statement.get())) {
        declare(function->name(), functionSymbol(*function));
      } else if (const auto *classDecl =
                     dynamic_cast<const ClassDecl *>(statement.get())) {
        declare(classDecl->name(), SemanticType::Object, false);
      } else if (includeVariables) {
        if (const auto *variable =
                dynamic_cast<const VariableDecl *>(statement.get())) {
          if (declare(variable->name(), typeOf(variable->type()),
                      variable->isMutable())) {
            predeclaredVariables.insert(variable);
          }
        }
      }
    }
  }

  bool declare(const Token &name, SemanticType type, bool assignable) {
    return declare(name, Symbol{.type = type, .assignable = assignable});
  }

  bool declare(const Token &name, Symbol symbol) {
    const auto [_, inserted] = scopes.back().emplace(
        name.lexeme, std::move(symbol));
    if (!inserted) {
      report(name, "Duplicate declaration of '" + name.lexeme + "'.");
    }
    return inserted;
  }

  bool declareNamespaceSymbol(const std::vector<std::string> &scope,
                              const Token &name, SemanticType type,
                              bool assignable) {
    return declareNamespaceSymbol(
        scope, name, Symbol{.type = type, .assignable = assignable});
  }

  bool declareNamespaceSymbol(const std::vector<std::string> &scope,
                              const Token &name, Symbol symbol) {
    const std::string qualified = qualifiedName(scope, name.lexeme);
    if (namespaces.contains(qualified) ||
        namespaceAliases.contains(qualified)) {
      report(name, "Duplicate declaration of '" + name.lexeme + "'.");
      return false;
    }

    const auto [_, inserted] =
        namespaceSymbols.emplace(qualified, std::move(symbol));
    if (!inserted) {
      report(name, "Duplicate declaration of '" + name.lexeme + "'.");
    }
    return inserted;
  }

  [[nodiscard]] const Symbol *resolve(const Token &name) const {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      if (const auto found = scope->find(name.lexeme); found != scope->end()) {
        return &found->second;
      }
    }

    for (std::size_t depth = currentNamespace.size() + 1; depth > 0; --depth) {
      std::vector<std::string> scope(currentNamespace.begin(),
                                     currentNamespace.begin() + depth - 1);
      const auto found =
          namespaceSymbols.find(qualifiedName(scope, name.lexeme));
      if (found != namespaceSymbols.end()) {
        return &found->second;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::optional<std::string>
  resolveInitialNamespace(const Token &name) const {
    for (std::size_t depth = currentNamespace.size() + 1; depth > 0; --depth) {
      std::vector<std::string> scope(currentNamespace.begin(),
                                     currentNamespace.begin() + depth - 1);
      const std::string candidate = qualifiedName(scope, name.lexeme);
      if (const auto alias = namespaceAliases.find(candidate);
          alias != namespaceAliases.end()) {
        return alias->second;
      }
      if (namespaces.contains(candidate)) {
        return candidate;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::string>
  resolveNamespacePath(const NamePath &path) const {
    std::optional<std::string> current =
        resolveInitialNamespace(path.first());
    if (!current) {
      return std::nullopt;
    }

    for (std::size_t index = 1; index < path.segments.size(); ++index) {
      const std::string candidate = *current + "::" +
                                    path.segments[index].lexeme;
      if (const auto alias = namespaceAliases.find(candidate);
          alias != namespaceAliases.end()) {
        current = alias->second;
      } else if (namespaces.contains(candidate)) {
        current = candidate;
      } else {
        return std::nullopt;
      }
    }
    return current;
  }

  [[nodiscard]] const Symbol *resolveQualified(const NamePath &path) const {
    if (path.segments.size() < 2) {
      return resolve(path.last());
    }

    NamePath namespacePath(std::vector<Token>(path.segments.begin(),
                                              path.segments.end() - 1));
    const std::optional<std::string> resolvedNamespace =
        resolveNamespacePath(namespacePath);
    if (!resolvedNamespace) {
      return nullptr;
    }
    const auto symbol = namespaceSymbols.find(
        *resolvedNamespace + "::" + path.last().lexeme);
    return symbol == namespaceSymbols.end() ? nullptr : &symbol->second;
  }

  [[nodiscard]] const Symbol *
  resolveExpressionSymbol(const ExprPtr &expression) const {
    if (const auto *variable =
            dynamic_cast<const Variable *>(expression.get())) {
      return resolve(variable->name());
    }
    if (const auto *qualified =
            dynamic_cast<const QualifiedName *>(expression.get())) {
      return resolveQualified(qualified->name());
    }
    if (const auto *get = dynamic_cast<const Get *>(expression.get());
        get != nullptr &&
        dynamic_cast<const Self *>(get->object().get()) != nullptr) {
      return resolve(get->name());
    }
    return nullptr;
  }

  [[nodiscard]] bool isMutableTarget(const ExprPtr &expression) const {
    if (const auto *variable =
            dynamic_cast<const Variable *>(expression.get())) {
      const Symbol *symbol = resolve(variable->name());
      return symbol == nullptr || symbol->assignable;
    }
    if (const auto *get = dynamic_cast<const Get *>(expression.get())) {
      if (dynamic_cast<const Self *>(get->object().get()) == nullptr) {
        // General member lookup waits for nominal user-defined types.
        return true;
      }
      const Symbol *member = resolve(get->name());
      return member == nullptr || member->assignable;
    }
    return false;
  }

  void beginScope() { scopes.emplace_back(); }
  void endScope() { scopes.pop_back(); }

  void report(const Token &token, std::string message) {
    diagnostics.push_back({token, std::move(message)});
  }

  void requireBool(SemanticType type, const Token &token,
                   std::string_view message) {
    if (type != SemanticType::Unknown && !isContextuallyBool(type)) {
      report(token, std::string(message));
    }
  }

  void requireNumeric(SemanticType left, SemanticType right,
                      const Token &token) {
    if ((left != SemanticType::Unknown && !isNumeric(left)) ||
        (right != SemanticType::Unknown && !isNumeric(right))) {
      report(token, "Operator requires numeric operands.");
    }
  }

  [[nodiscard]] static bool isNumeric(SemanticType type) {
    return isInteger(type) || type == SemanticType::Float;
  }

  [[nodiscard]] static bool isInteger(SemanticType type) {
    return type == SemanticType::Int8 || type == SemanticType::Int16 ||
           type == SemanticType::Int32 || type == SemanticType::Int64;
  }

  [[nodiscard]] static int integerRank(SemanticType type) {
    switch (type.kind) {
    case SemanticType::Int8:
      return 8;
    case SemanticType::Int16:
      return 16;
    case SemanticType::Int32:
      return 32;
    case SemanticType::Int64:
      return 64;
    default:
      return 0;
    }
  }

  [[nodiscard]] static bool integerFits(SemanticType type,
                                        std::int64_t value) {
    switch (type.kind) {
    case SemanticType::Int8:
      return value >= std::numeric_limits<std::int8_t>::min() &&
             value <= std::numeric_limits<std::int8_t>::max();
    case SemanticType::Int16:
      return value >= std::numeric_limits<std::int16_t>::min() &&
             value <= std::numeric_limits<std::int16_t>::max();
    case SemanticType::Int32:
      return value >= std::numeric_limits<std::int32_t>::min() &&
             value <= std::numeric_limits<std::int32_t>::max();
    case SemanticType::Int64:
      return true;
    default:
      return false;
    }
  }

  [[nodiscard]] static std::optional<std::int64_t>
  integerConstant(const Expr *expression) {
    if (expression == nullptr) {
      return std::nullopt;
    }
    if (const auto *literal = dynamic_cast<const LiteralExpr *>(expression)) {
      const auto *magnitude = std::get_if<std::uint64_t>(&literal->value());
      if (magnitude == nullptr ||
          *magnitude >
              static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
      }
      return static_cast<std::int64_t>(*magnitude);
    }
    if (const auto *grouping = dynamic_cast<const Grouping *>(expression)) {
      return integerConstant(grouping->expression().get());
    }
    const auto *unary = dynamic_cast<const Unary *>(expression);
    if (unary == nullptr || (unary->oper().kind != TokenKind::MINUS &&
                             unary->oper().kind != TokenKind::PLUS)) {
      return std::nullopt;
    }
    if (unary->oper().kind == TokenKind::PLUS) {
      return integerConstant(unary->right().get());
    }
    if (const auto *literal =
            dynamic_cast<const LiteralExpr *>(unary->right().get())) {
      if (const auto *magnitude =
              std::get_if<std::uint64_t>(&literal->value())) {
        if (*magnitude == (std::uint64_t{1} << 63U)) {
          return std::numeric_limits<std::int64_t>::min();
        }
      }
    }
    const std::optional<std::int64_t> value =
        integerConstant(unary->right().get());
    if (!value || *value == std::numeric_limits<std::int64_t>::min()) {
      return std::nullopt;
    }
    return -*value;
  }

  [[nodiscard]] static bool isContextuallyBool(const SemanticType &type) {
    return type == SemanticType::Bool || type.kind == SemanticType::Expected;
  }

  [[nodiscard]] static bool isExpectedVoid(const SemanticType &type) {
    return type.kind == SemanticType::Expected && type.arguments.size() == 2 &&
           type.arguments[0] == SemanticType::Void;
  }

  [[nodiscard]] static SemanticType numericResult(SemanticType left,
                                                   SemanticType right) {
    if (left == SemanticType::Unknown || right == SemanticType::Unknown) {
      return SemanticType::Unknown;
    }
    if (left == SemanticType::Float || right == SemanticType::Float) {
      return SemanticType::Float;
    }
    if (left == SemanticType::Int64 || right == SemanticType::Int64) {
      return SemanticType::Int64;
    }
    // As in C++, arithmetic promotes 8- and 16-bit integers to 32 bits.
    return SemanticType::Int32;
  }

  [[nodiscard]] static bool isAssignable(SemanticType target,
                                         SemanticType value,
                                         const Expr *expression = nullptr) {
    if (target == SemanticType::Unknown || value == SemanticType::Unknown ||
        target == value) {
      return true;
    }
    if (target == SemanticType::Float && isInteger(value)) {
      return true;
    }
    if (isInteger(target) && isInteger(value)) {
      if (const std::optional<std::int64_t> constant =
              integerConstant(expression)) {
        return integerFits(target, *constant);
      }
      return integerRank(value) <= integerRank(target);
    }
    if (target == SemanticType::Object && value == SemanticType::NullPtr) {
      return true;
    }
    if (target.kind != SemanticType::Expected || target.arguments.size() != 2) {
      return false;
    }
    if (value.kind == SemanticType::Unexpected &&
        value.arguments.size() == 1) {
      const Expr *errorExpression = expression;
      if (const auto *unexpected =
              dynamic_cast<const Unexpected *>(expression)) {
        errorExpression = unexpected->error().get();
      }
      return isAssignable(target.arguments[1], value.arguments[0],
                          errorExpression);
    }
    return target.arguments[0] != SemanticType::Void &&
           isAssignable(target.arguments[0], value, expression);
  }

  [[nodiscard]] static bool isComparable(SemanticType left,
                                         SemanticType right) {
    return isAssignable(left, right) || isAssignable(right, left);
  }

  [[nodiscard]] static SemanticType typeOf(const TypeRef &type) {
    switch (type.name.last().kind) {
    case TokenKind::VOID:
      return SemanticType::Void;
    case TokenKind::INT:
    case TokenKind::INT32:
      return SemanticType::Int32;
    case TokenKind::INT8:
      return SemanticType::Int8;
    case TokenKind::INT16:
      return SemanticType::Int16;
    case TokenKind::INT64:
      return SemanticType::Int64;
    case TokenKind::FLOAT:
      return SemanticType::Float;
    case TokenKind::BOOL:
      return SemanticType::Bool;
    case TokenKind::STRING_TYPE:
      return SemanticType::String;
    case TokenKind::EXPECTED: {
      std::vector<SemanticType> arguments;
      arguments.reserve(type.arguments.size());
      for (const TypeRef &argument : type.arguments) {
        arguments.emplace_back(typeOf(argument));
      }
      return SemanticType(SemanticType::Expected, std::move(arguments));
    }
    default:
      return SemanticType::Object;
    }
  }

  [[nodiscard]] static SemanticType literalType(const Literal &literal) {
    if (const auto *value = std::get_if<std::uint64_t>(&literal)) {
      if (*value <=
          static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
        return SemanticType::Int32;
      }
      if (*value <=
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return SemanticType::Int64;
      }
      return SemanticType::Unknown;
    }
    if (std::holds_alternative<double>(literal)) {
      return SemanticType::Float;
    }
    if (std::holds_alternative<std::string>(literal)) {
      return SemanticType::String;
    }
    if (std::holds_alternative<bool>(literal)) {
      return SemanticType::Bool;
    }
    if (std::holds_alternative<std::nullptr_t>(literal)) {
      return SemanticType::NullPtr;
    }
    return SemanticType::Unknown;
  }

  [[nodiscard]] static Token expressionToken(const ExprPtr &expr) {
    if (const auto *literal = dynamic_cast<const LiteralExpr *>(expr.get())) {
      return literal->token();
    }
    if (const auto *variable = dynamic_cast<const Variable *>(expr.get())) {
      return variable->name();
    }
    if (const auto *self = dynamic_cast<const Self *>(expr.get())) {
      return self->keyword();
    }
    if (const auto *binary = dynamic_cast<const Binary *>(expr.get())) {
      return binary->oper();
    }
    if (const auto *logical = dynamic_cast<const Logical *>(expr.get())) {
      return logical->oper();
    }
    if (const auto *unary = dynamic_cast<const Unary *>(expr.get())) {
      return unary->oper();
    }
    if (const auto *unexpected =
            dynamic_cast<const Unexpected *>(expr.get())) {
      return unexpected->keyword();
    }
    if (const auto *postfix = dynamic_cast<const Postfix *>(expr.get())) {
      return postfix->oper();
    }
    if (const auto *qualified =
            dynamic_cast<const QualifiedName *>(expr.get())) {
      return qualified->name().last();
    }
    if (const auto *assign = dynamic_cast<const Assign *>(expr.get())) {
      return assign->oper();
    }
    if (const auto *call = dynamic_cast<const Call *>(expr.get())) {
      return call->paren();
    }
    if (const auto *get = dynamic_cast<const Get *>(expr.get())) {
      return get->name();
    }
    if (const auto *set = dynamic_cast<const Set *>(expr.get())) {
      return set->name();
    }
    if (const auto *grouping = dynamic_cast<const Grouping *>(expr.get())) {
      return expressionToken(grouping->expression());
    }
    return Token{};
  }

  std::vector<SemanticDiagnostic> diagnostics;
  std::vector<std::unordered_map<std::string, Symbol>> scopes;
  std::unordered_set<std::string> namespaces;
  std::unordered_map<std::string, std::string> namespaceAliases;
  std::unordered_map<std::string, Symbol> namespaceSymbols;
  std::unordered_map<const Expr *, SemanticType> expressionTypes;
  std::vector<std::string> currentNamespace;
  std::unordered_set<const VariableDecl *> predeclaredVariables;
  TargetInfo target;
  SemanticType currentType = SemanticType::Unknown;
  SemanticType currentReturnType = SemanticType::Unknown;
  std::size_t classDepth = 0;
  std::size_t functionDepth = 0;
};

} // namespace lang
