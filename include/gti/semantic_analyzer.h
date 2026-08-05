#pragma once

#include "gti/ast.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace lang {

enum class SemanticType {
  Unknown,
  Void,
  Int,
  Float,
  Bool,
  String,
  NullPtr,
  Object,
  Function,
};

struct SemanticDiagnostic {
  Token token;
  std::string message;
};

class SemanticVisitor final : public ExprVisitor, public StmtVisitor {
public:
  bool check(const Program &program) {
    diagnostics.clear();
    scopes.clear();
    namespaces.clear();
    namespaceAliases.clear();
    namespaceSymbols.clear();
    currentNamespace.clear();
    predeclaredVariables.clear();
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
    for (const Parameter &parameter : stmt.parameters()) {
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
      report(stmt.keyword(), "A value is required for this return type.");
      return;
    }

    const SemanticType valueType = analyze(stmt.value());
    if (!isAssignable(currentReturnType, valueType)) {
      report(stmt.keyword(), "Return value does not match the function type.");
    }
  }

  void visitVariableDecl(const VariableDecl &stmt) override {
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

    if (stmt.initializer() && !isAssignable(declaredType, initializerType)) {
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
    if (!isAssignable(symbol->type, valueType)) {
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
    const Symbol *callee = resolveExpressionSymbol(expr.callee());
    std::vector<SemanticType> argumentTypes;
    argumentTypes.reserve(expr.arguments().size());
    for (const ExprPtr &argument : expr.arguments()) {
      argumentTypes.emplace_back(analyze(argument));
    }

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
          if (!isAssignable(callee->parameterTypes[index],
                            argumentTypes[index])) {
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
    analyze(expr.object());
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
    // Member tables are intentionally deferred until user-defined types carry
    // identities instead of the current coarse Object type.
    currentType = SemanticType::Unknown;
  }

  void visitGroupingExpr(const Grouping &expr) override {
    currentType = analyze(expr.expression());
  }

  void visitLiteralExpr(const LiteralExpr &expr) override {
    currentType = literalType(expr.value());
  }

  void visitLogicalExpr(const Logical &expr) override {
    const SemanticType leftType = analyze(expr.left());
    const SemanticType rightType = analyze(expr.right());
    if ((leftType != SemanticType::Unknown && leftType != SemanticType::Bool) ||
        (rightType != SemanticType::Unknown &&
         rightType != SemanticType::Bool)) {
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
        if (!isAssignable(member->type, valueType)) {
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
      if (const auto *function =
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
    return currentType;
  }

  SemanticType analyze(const ExprPtr &expr) {
    return expr ? analyze(*expr) : SemanticType::Unknown;
  }

  void predeclare(const StmtList &statements, bool includeVariables) {
    for (const StmtPtr &statement : statements) {
      if (const auto *function =
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
    if (type != SemanticType::Unknown && type != SemanticType::Bool) {
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
    return type == SemanticType::Int || type == SemanticType::Float;
  }

  [[nodiscard]] static SemanticType numericResult(SemanticType left,
                                                   SemanticType right) {
    if (left == SemanticType::Unknown || right == SemanticType::Unknown) {
      return SemanticType::Unknown;
    }
    return left == SemanticType::Float || right == SemanticType::Float
               ? SemanticType::Float
               : SemanticType::Int;
  }

  [[nodiscard]] static bool isAssignable(SemanticType target,
                                         SemanticType value) {
    return target == SemanticType::Unknown || value == SemanticType::Unknown ||
           target == value ||
           (target == SemanticType::Float && value == SemanticType::Int) ||
           (target == SemanticType::Object &&
            value == SemanticType::NullPtr);
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
      return SemanticType::Int;
    case TokenKind::FLOAT:
      return SemanticType::Float;
    case TokenKind::BOOL:
      return SemanticType::Bool;
    case TokenKind::STRING_TYPE:
      return SemanticType::String;
    default:
      return SemanticType::Object;
    }
  }

  [[nodiscard]] static SemanticType literalType(const Literal &literal) {
    if (std::holds_alternative<int>(literal)) {
      return SemanticType::Int;
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
  std::vector<std::string> currentNamespace;
  std::unordered_set<const VariableDecl *> predeclaredVariables;
  SemanticType currentType = SemanticType::Unknown;
  SemanticType currentReturnType = SemanticType::Unknown;
  std::size_t classDepth = 0;
  std::size_t functionDepth = 0;
};

} // namespace lang
