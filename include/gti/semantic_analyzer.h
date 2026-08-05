#pragma once

#include "gti/ast.h"

#include <algorithm>
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

using ClassId = std::size_t;
using GenericParameterId = std::size_t;

struct GenericParameterInfo {
  GenericParameterId id = 0;
  Token name;
};

struct SemanticType {
  enum Kind {
    Unknown,
    Void,
    Int8,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Float,
    Bool,
    String,
    NullPtr,
    Class,
    TypeParameter,
    TypeName,
    Function,
    Expected,
    Unexpected,
  };

  SemanticType(Kind kind = Unknown) : kind(kind) {}
  SemanticType(Kind kind, std::vector<SemanticType> arguments)
      : kind(kind), arguments(std::move(arguments)) {}

  [[nodiscard]] static SemanticType
  classType(ClassId id, std::vector<SemanticType> arguments = {}) {
    SemanticType type(Class, std::move(arguments));
    type.classId = id;
    return type;
  }

  [[nodiscard]] static SemanticType typeParameter(GenericParameterId id) {
    SemanticType type(TypeParameter);
    type.genericParameterId = id;
    return type;
  }

  [[nodiscard]] static SemanticType typeName(ClassId id) {
    SemanticType type(TypeName);
    type.classId = id;
    return type;
  }

  friend bool operator==(const SemanticType &, const SemanticType &) = default;

  Kind kind;
  std::vector<SemanticType> arguments;
  ClassId classId = 0;
  GenericParameterId genericParameterId = 0;
};

using TypeSubstitution = std::unordered_map<GenericParameterId, SemanticType>;

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
    classIds.clear();
    classDeclIds.clear();
    functionGenericParameters.clear();
    classes.clear();
    typeParameterScopes.clear();
    nextGenericParameterId = 1;
    currentNamespace.clear();
    predeclaredVariables.clear();
    expressionTypes.clear();
    currentClass.reset();
    analyzingFieldInitializer = false;
    analyzingConstructorInitializer = false;
    currentReceiverMutability = ReceiverMutability::ReadOnly;
    constructorDepth = 0;
    functionDepth = 0;
    currentReturnType = SemanticType::Unknown;

    registerNamespaces(program.declarations(), {});
    registerNamespaceAliases(program.declarations(), {});
    registerClasses(program.declarations(), {});
    registerFunctionGenericParameters(program.declarations());
    registerNamespaceSymbols(program.declarations(), {});
    collectClassMembers(program.declarations(), {});
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
    classIds.clear();
    classDeclIds.clear();
    functionGenericParameters.clear();
    classes.clear();
    typeParameterScopes.clear();
    nextGenericParameterId = 1;
    currentNamespace.clear();
    predeclaredVariables.clear();
    expressionTypes.clear();
    currentClass.reset();
    analyzingFieldInitializer = false;
    analyzingConstructorInitializer = false;
    currentReceiverMutability = ReceiverMutability::ReadOnly;
    constructorDepth = 0;
    functionDepth = 0;
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

  void visitAccessSpecifierDecl(const AccessSpecifierDecl &) override {}

  void visitBlockStmt(const BlockStmt &stmt) override {
    beginScope();
    analyze(stmt.statements());
    endScope();
  }

  void visitClassDecl(const ClassDecl &stmt) override {
    const auto registered = classDeclIds.find(&stmt);
    if (registered == classDeclIds.end()) {
      return;
    }

    const std::optional<ClassId> enclosingClass = currentClass;
    currentClass = registered->second;
    const ClassInfo &info = classInfo(*currentClass);
    beginTypeParameterScope(info.genericParameters);
    if (!info.constructor) {
      for (const FieldInfo &field : info.fields) {
        if (!field.declaration->initializer()) {
          report(field.declaration->name(),
                 "Class and struct fields must have an initializer or be "
                 "initialized by a constructor.");
        }
      }
    }
    beginScope();
    for (const auto &[name, member] : info.members) {
      scopes.back().emplace(name, member.symbol);
    }
    analyze(stmt.members());
    endScope();
    endTypeParameterScope();
    currentClass = enclosingClass;
  }

  void visitConditionalStmt(const ConditionalStmt &stmt) override {
    if (const StmtList *branch = stmt.activeBranch(target)) {
      analyze(*branch);
    }
  }

  void visitConstructorDecl(const ConstructorDecl &stmt) override {
    if (!currentClass) {
      report(stmt.name(),
             "Constructors can only be declared in a class or struct.");
      return;
    }

    ClassInfo &owner = classInfo(*currentClass);
    for (const Parameter &parameter : stmt.parameters()) {
      validateType(parameter.type);
      if (typeOf(parameter.type) == SemanticType::Void) {
        report(parameter.type.name.last(),
               "Constructor parameters cannot have type void.");
      }
    }

    const SemanticType enclosingReturnType = currentReturnType;
    const ReceiverMutability enclosingReceiverMutability =
        currentReceiverMutability;
    currentReturnType = SemanticType::Void;
    currentReceiverMutability = ReceiverMutability::Mutable;
    ++functionDepth;
    ++constructorDepth;
    beginScope();

    for (const Parameter &parameter : stmt.parameters()) {
      if (!parameter.name.lexeme.empty()) {
        declare(parameter.name, typeOf(parameter.type),
                parameter.mutability == Mutability::Mutable);
      }
    }

    std::unordered_set<std::string> initializedFields;
    std::optional<std::size_t> previousFieldIndex;
    for (const ConstructorInitializer &initializer : stmt.initializers()) {
      const auto inserted = initializedFields.insert(initializer.field.lexeme);
      if (!inserted.second) {
        report(initializer.field, "Constructor field '" +
                                      initializer.field.lexeme +
                                      "' is initialized more than once.");
      }

      std::optional<std::size_t> fieldIndex;
      const VariableDecl *field = nullptr;
      for (std::size_t index = 0; index < owner.fields.size(); ++index) {
        if (owner.fields[index].declaration->name().lexeme ==
            initializer.field.lexeme) {
          fieldIndex = index;
          field = owner.fields[index].declaration;
          break;
        }
      }
      if (field == nullptr) {
        report(initializer.field,
               "Unknown constructor field '" + initializer.field.lexeme + "'.");
      } else {
        if (previousFieldIndex && *fieldIndex < *previousFieldIndex) {
          report(
              initializer.field,
              "Constructor initializers must follow field declaration order.");
        }
        previousFieldIndex = fieldIndex;
      }

      const bool enclosingConstructorInitializer =
          analyzingConstructorInitializer;
      analyzingConstructorInitializer = true;
      const SemanticType valueType = analyze(initializer.value);
      analyzingConstructorInitializer = enclosingConstructorInitializer;
      if (field != nullptr &&
          !isAssignable(typeOf(field->type(), owner.namespaceScope), valueType,
                        initializer.value.get())) {
        report(initializer.field,
               "Constructor initializer does not match the field type.");
      }
    }

    for (const FieldInfo &field : owner.fields) {
      if (!field.declaration->initializer() &&
          !initializedFields.contains(field.declaration->name().lexeme)) {
        report(field.declaration->name(),
               "Field must be initialized by this constructor.");
      }
    }

    analyze(stmt.body()->statements());
    endScope();
    --constructorDepth;
    --functionDepth;
    currentReceiverMutability = enclosingReceiverMutability;
    currentReturnType = enclosingReturnType;
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
    const std::vector<GenericParameterInfo> &genericParameters =
        genericParametersFor(stmt);
    beginTypeParameterScope(genericParameters);
    if (!genericParameters.empty() && currentNamespace.empty() &&
        !currentClass && stmt.name().lexeme == "main") {
      report(stmt.name(), "The main entry point cannot be generic.");
    }
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
      endTypeParameterScope();
      return;
    }

    const SemanticType enclosingReturnType = currentReturnType;
    const ReceiverMutability enclosingReceiverMutability =
        currentReceiverMutability;
    if (currentClass && functionDepth == 0) {
      currentReceiverMutability = stmt.receiverMutability();
    }
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
    currentReceiverMutability = enclosingReceiverMutability;
    currentReturnType = enclosingReturnType;
    endTypeParameterScope();
  }

  void visitIfStmt(const IfStmt &stmt) override {
    requireBool(analyze(stmt.condition()), expressionToken(stmt.condition()),
                "If condition must be bool.");
    analyze(stmt.thenBranch());
    analyze(stmt.elseBranch());
  }

  void visitNamespaceAliasDecl(const NamespaceAliasDecl &) override {}

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
    if (constructorDepth > 0) {
      if (stmt.value()) {
        analyze(stmt.value());
      }
      report(stmt.keyword(), "Constructors cannot contain return statements.");
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
    } else if (!stmt.initializer()) {
      const bool field = currentClass && functionDepth == 0;
      if (!field && declaredType.kind == SemanticType::Class) {
        report(stmt.name(),
               "Class and struct variables require explicit construction.");
      } else if (!field && !stmt.isMutable()) {
        report(stmt.name(), "Immutable variable must have an initializer.");
      }
    }
    if (stmt.initializer()) {
      const bool enclosingFieldInitializer = analyzingFieldInitializer;
      analyzingFieldInitializer = currentClass && functionDepth == 0;
      initializerType = analyze(stmt.initializer());
      analyzingFieldInitializer = enclosingFieldInitializer;
    }

    if (!predeclaredVariables.contains(&stmt)) {
      if (functionDepth == 0 && !currentClass) {
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
    } else if (symbol->ownerClass != 0 &&
               currentReceiverMutability != ReceiverMutability::Mutable) {
      report(expr.name(), "Cannot mutate through a read-only receiver.");
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
      if (!isComparable(leftType, rightType, expr.left().get(),
                        expr.right().get())) {
        report(expr.oper(), "Equality operands have incompatible types.");
      }
      currentType = SemanticType::Bool;
      return;
    case TokenKind::GREATER:
    case TokenKind::GREATER_EQUAL:
    case TokenKind::LESS:
    case TokenKind::LESS_EQUAL:
      requireNumeric(leftType, rightType, expr.oper());
      if (isInteger(leftType) && isInteger(rightType) &&
          numericResult(leftType, rightType, expr.left().get(),
                        expr.right().get()) == SemanticType::Unknown) {
        report(expr.oper(),
               "Signed and unsigned operands have no safe common type.");
      }
      currentType = SemanticType::Bool;
      return;
    case TokenKind::PLUS:
    case TokenKind::MINUS:
    case TokenKind::STAR:
    case TokenKind::SLASH:
      requireNumeric(leftType, rightType, expr.oper());
      currentType = numericResult(leftType, rightType, expr.left().get(),
                                  expr.right().get());
      if (isInteger(leftType) && isInteger(rightType) &&
          currentType == SemanticType::Unknown) {
        report(expr.oper(),
               "Signed and unsigned operands have no safe common type.");
      }
      return;
    case TokenKind::PERCENT:
    case TokenKind::AMPERSAND:
    case TokenKind::CARET:
    case TokenKind::PIPE:
      requireInteger(leftType, rightType, expr.oper());
      if (!isInteger(leftType) || !isInteger(rightType)) {
        currentType = SemanticType::Unknown;
        return;
      }
      currentType = numericResult(leftType, rightType, expr.left().get(),
                                  expr.right().get());
      if (isInteger(leftType) && isInteger(rightType) &&
          currentType == SemanticType::Unknown) {
        report(expr.oper(),
               "Signed and unsigned operands have no safe common type.");
      }
      if (expr.oper().kind == TokenKind::PERCENT) {
        if (const std::optional<IntegerConstant> divisor =
                integerConstant(expr.right().get());
            divisor && divisor->magnitude == 0) {
          report(expr.oper(), "Modulo divisor cannot be zero.");
        }
      }
      return;
    case TokenKind::SHIFT_LEFT:
    case TokenKind::SHIFT_RIGHT:
      requireInteger(leftType, rightType, expr.oper());
      if (!isInteger(leftType) || !isInteger(rightType)) {
        currentType = SemanticType::Unknown;
        return;
      }
      currentType = promotedInteger(leftType);
      validateShiftCount(currentType, expr.right().get(), expr.oper());
      return;
    default:
      currentType = SemanticType::Unknown;
    }
  }

  void visitCallExpr(const Call &expr) override {
    const SemanticType calleeType = analyze(expr.callee());
    std::vector<SemanticType> explicitTypeArguments;
    explicitTypeArguments.reserve(expr.typeArguments().size());
    for (const TypeRef &argument : expr.typeArguments()) {
      validateType(argument);
      const SemanticType argumentType = typeOf(argument);
      if (argumentType == SemanticType::Void) {
        report(argument.name.last(), "Generic type arguments cannot be void.");
      }
      explicitTypeArguments.emplace_back(argumentType);
    }

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
        if (!explicitTypeArguments.empty()) {
          report(expr.paren(),
                 "Expected member functions do not take generic arguments.");
        }
        analyzeExpectedMemberCall(*member, objectType->second, argumentTypes,
                                  expr.arguments(), expr.paren());
        return;
      }
    }

    const std::optional<Symbol> callee = resolveExpressionSymbol(expr.callee());

    if (calleeType.kind == SemanticType::TypeName) {
      analyzeConstructorCall(calleeType.classId, explicitTypeArguments,
                             argumentTypes, expr.arguments(), expr.paren());
      return;
    }

    if (calleeType != SemanticType::Unknown &&
        calleeType != SemanticType::Function) {
      report(expr.paren(), "Can only call functions.");
      currentType = SemanticType::Unknown;
      return;
    }
    if (callee && callee->type == SemanticType::Function) {
      Symbol resolvedCallee = *callee;
      applyFunctionTypeArguments(resolvedCallee, explicitTypeArguments,
                                 argumentTypes, expr.arguments(), expr.paren());
      if (callee->receiverMutability == ReceiverMutability::Mutable) {
        bool mutableReceiver =
            currentReceiverMutability == ReceiverMutability::Mutable;
        if (const auto *member =
                dynamic_cast<const Get *>(expr.callee().get())) {
          mutableReceiver = isMutableObject(member->object());
        }
        if (!mutableReceiver) {
          report(expr.paren(), "Mutable method requires a mutable receiver.");
        }
      }
      if (argumentTypes.size() != resolvedCallee.parameterTypes.size()) {
        report(expr.paren(), "Function called with the wrong number of arguments.");
      } else {
        for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
          if (!isAssignable(resolvedCallee.parameterTypes[index],
                            argumentTypes[index],
                            expr.arguments()[index].get())) {
            report(expressionToken(expr.arguments()[index]),
                   "Argument does not match the parameter type.");
          }
        }
      }
      currentType = resolvedCallee.returnType;
      return;
    }
    currentType = SemanticType::Unknown;
  }

  void visitGetExpr(const Get &expr) override {
    const SemanticType objectType = analyze(expr.object());
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
    const MemberInfo *member = resolveMember(objectType, expr.name());
    currentType = member == nullptr
                      ? SemanticType::Unknown
                      : substituteSymbol(member->symbol, objectType).type;
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
    if (analyzingConstructorInitializer) {
      report(expr.keyword(),
             "Cannot use 'self' in a constructor initializer expression.");
      currentType = SemanticType::Unknown;
      return;
    }
    if (!currentClass || functionDepth == 0) {
      report(expr.keyword(), "Cannot use 'self' outside a class or struct method.");
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = openClassType(*currentClass);
  }

  void visitSetExpr(const Set &expr) override {
    const SemanticType objectType = analyze(expr.object());
    const SemanticType valueType = analyze(expr.value());

    const MemberInfo *member = resolveMember(objectType, expr.name());
    if (member == nullptr) {
      currentType = SemanticType::Unknown;
      return;
    }
    const Symbol resolvedMember = substituteSymbol(member->symbol, objectType);
    if (resolvedMember.type == SemanticType::Function) {
      report(expr.name(), "Methods are not assignable.");
    } else if (!resolvedMember.assignable) {
      report(expr.name(), "Member is immutable.");
    } else if (!isMutableObject(expr.object())) {
      report(expr.name(), "Cannot mutate through a read-only receiver.");
    }
    if (!isAssignable(resolvedMember.type, valueType, expr.value().get())) {
      report(expr.oper(), "Assigned value does not match the member type.");
    }
    if (expr.oper().kind != TokenKind::EQUAL &&
        ((resolvedMember.type != SemanticType::Unknown &&
          !isNumeric(resolvedMember.type)) ||
         (valueType != SemanticType::Unknown && !isNumeric(valueType)))) {
      report(expr.oper(), "Compound assignment requires numeric operands.");
    }
    currentType = resolvedMember.type;
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

    if (expr.oper().kind == TokenKind::TILDE) {
      if (rightType != SemanticType::Unknown && !isInteger(rightType)) {
        report(expr.oper(), "Bitwise complement requires an integer operand.");
        currentType = SemanticType::Unknown;
      } else {
        currentType = promotedInteger(rightType);
      }
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
    if ((expr.oper().kind == TokenKind::PLUS ||
         expr.oper().kind == TokenKind::MINUS) &&
        (rightType == SemanticType::Int8 ||
         rightType == SemanticType::Int16 ||
         rightType == SemanticType::UInt8 ||
         rightType == SemanticType::UInt16)) {
      currentType = SemanticType::Int32;
      return;
    }
    if (expr.oper().kind == TokenKind::MINUS && isUnsignedInteger(rightType)) {
      report(expr.oper(),
             "Unary '-' cannot be applied to an unsigned integer.");
      currentType = SemanticType::Unknown;
      return;
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
    if (symbol->ownerClass != 0 &&
        (analyzingFieldInitializer || analyzingConstructorInitializer)) {
      report(expr.name(), analyzingConstructorInitializer
                              ? "Class and struct members cannot be referenced "
                                "from constructor initializer expressions."
                              : "Class and struct members cannot be referenced "
                                "from field initializers yet.");
    }
    currentType = symbol->type;
  }

private:
  struct Symbol {
    SemanticType type = SemanticType::Unknown;
    bool assignable = false;
    SemanticType returnType = SemanticType::Unknown;
    std::vector<SemanticType> parameterTypes;
    std::vector<GenericParameterInfo> genericParameters;
    ClassId ownerClass = 0;
    ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly;
  };

  struct MemberInfo {
    Symbol symbol;
    AccessModifier access = AccessModifier::Private;
  };

  struct FieldInfo {
    const VariableDecl *declaration = nullptr;
  };

  struct ConstructorInfo {
    const ConstructorDecl *declaration = nullptr;
    AccessModifier access = AccessModifier::Public;
    std::vector<SemanticType> parameterTypes;
  };

  struct ClassInfo {
    ClassId id = 0;
    Token name;
    ClassKind kind = ClassKind::Class;
    std::vector<std::string> namespaceScope;
    std::vector<GenericParameterInfo> genericParameters;
    std::unordered_map<std::string, MemberInfo> members;
    std::vector<FieldInfo> fields;
    std::optional<ConstructorInfo> constructor;
  };

  [[nodiscard]] static const Call *directCall(const ExprPtr &expression) {
    const Expr *candidate = expression.get();
    while (const auto *grouping = dynamic_cast<const Grouping *>(candidate)) {
      candidate = grouping->expression().get();
    }
    return dynamic_cast<const Call *>(candidate);
  }

  [[nodiscard]] static const GenericParameterInfo *
  findGenericParameter(const std::vector<GenericParameterInfo> &parameters,
                       GenericParameterId id) {
    for (const GenericParameterInfo &parameter : parameters) {
      if (parameter.id == id) {
        return &parameter;
      }
    }
    return nullptr;
  }

  bool inferTypeArguments(const SemanticType &pattern,
                          const SemanticType &argument,
                          const std::vector<GenericParameterInfo> &parameters,
                          TypeSubstitution &substitution,
                          const Token &argumentToken) {
    if (pattern.kind == SemanticType::TypeParameter &&
        findGenericParameter(parameters, pattern.genericParameterId) !=
            nullptr) {
      if (argument == SemanticType::Unknown) {
        return true;
      }
      const auto found = substitution.find(pattern.genericParameterId);
      if (found == substitution.end()) {
        substitution.emplace(pattern.genericParameterId, argument);
        return true;
      }
      if (found->second != argument) {
        const GenericParameterInfo *parameter =
            findGenericParameter(parameters, pattern.genericParameterId);
        report(argumentToken, "Conflicting types inferred for generic type "
                              "parameter '" +
                                  parameter->name.lexeme + "'.");
        return false;
      }
      return true;
    }

    if (pattern.kind != argument.kind || pattern.classId != argument.classId ||
        pattern.arguments.size() != argument.arguments.size()) {
      return true;
    }
    bool valid = true;
    for (std::size_t index = 0; index < pattern.arguments.size(); ++index) {
      valid = inferTypeArguments(pattern.arguments[index],
                                 argument.arguments[index], parameters,
                                 substitution, argumentToken) &&
              valid;
    }
    return valid;
  }

  void applyFunctionTypeArguments(
      Symbol &function, const std::vector<SemanticType> &explicitTypeArguments,
      const std::vector<SemanticType> &argumentTypes, const ExprList &arguments,
      const Token &paren) {
    if (function.genericParameters.empty()) {
      if (!explicitTypeArguments.empty()) {
        report(paren, "Non-generic functions do not take generic arguments.");
      }
      return;
    }

    TypeSubstitution substitution;
    if (!explicitTypeArguments.empty()) {
      if (explicitTypeArguments.size() != function.genericParameters.size()) {
        report(
            paren,
            "Generic function called with the wrong number of type arguments.");
      }
      const std::size_t count = std::min(explicitTypeArguments.size(),
                                         function.genericParameters.size());
      for (std::size_t index = 0; index < count; ++index) {
        substitution.emplace(function.genericParameters[index].id,
                             explicitTypeArguments[index]);
      }
    } else {
      const std::size_t count =
          std::min(argumentTypes.size(), function.parameterTypes.size());
      for (std::size_t index = 0; index < count; ++index) {
        inferTypeArguments(function.parameterTypes[index], argumentTypes[index],
                           function.genericParameters, substitution,
                           expressionToken(arguments[index]));
      }
    }

    for (const GenericParameterInfo &parameter : function.genericParameters) {
      if (!substitution.contains(parameter.id)) {
        report(paren, "Cannot infer generic type parameter '" +
                          parameter.name.lexeme +
                          "'; provide explicit type arguments.");
      }
    }

    function.returnType = substituteType(function.returnType, substitution);
    for (SemanticType &parameter : function.parameterTypes) {
      parameter = substituteType(parameter, substitution);
    }
  }

  void analyzeConstructorCall(ClassId classId,
                              const std::vector<SemanticType> &typeArguments,
                              const std::vector<SemanticType> &argumentTypes,
                              const ExprList &arguments, const Token &paren) {
    if (classId == 0 || classId > classes.size()) {
      currentType = SemanticType::Unknown;
      return;
    }

    const ClassInfo &owner = classInfo(classId);
    if (typeArguments.size() != owner.genericParameters.size()) {
      report(paren, "Type '" + owner.name.lexeme + "' requires " +
                        std::to_string(owner.genericParameters.size()) +
                        " generic type argument" +
                        (owner.genericParameters.size() == 1 ? "." : "s."));
    }
    TypeSubstitution substitution;
    const std::size_t typeArgumentCount =
        std::min(typeArguments.size(), owner.genericParameters.size());
    for (std::size_t index = 0; index < typeArgumentCount; ++index) {
      substitution.emplace(owner.genericParameters[index].id,
                           typeArguments[index]);
    }
    const SemanticType constructedType =
        SemanticType::classType(classId, typeArguments);
    if (!owner.constructor) {
      if (!argumentTypes.empty()) {
        report(paren, "Synthesized default constructor takes no arguments.");
      }
      currentType = constructedType;
      return;
    }

    const ConstructorInfo &constructor = *owner.constructor;
    if (constructor.access == AccessModifier::Private &&
        currentClass != classId) {
      report(paren, "Constructor of '" + owner.name.lexeme + "' is private.");
    }
    if (argumentTypes.size() != constructor.parameterTypes.size()) {
      report(paren, "Constructor called with the wrong number of arguments.");
    } else {
      for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
        if (!isAssignable(
                substituteType(constructor.parameterTypes[index], substitution),
                argumentTypes[index], arguments[index].get())) {
          report(expressionToken(arguments[index]),
                 "Constructor argument does not match the parameter type.");
        }
      }
    }
    currentType = constructedType;
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
    if (type.name.last().kind != TokenKind::IDENTIFIER) {
      return;
    }
    if (resolveTypeParameter(type.name)) {
      if (!type.arguments.empty()) {
        report(type.name.last(), "Generic type parameters cannot take type "
                                 "arguments.");
      }
      return;
    }

    const std::optional<ClassId> classId =
        resolveClassPath(type.name, currentNamespace);
    if (!classId) {
      report(type.name.last(), "Unknown type '" + pathSpelling(type.name) + "'.");
      return;
    }
    const std::size_t expectedArguments =
        classInfo(*classId).genericParameters.size();
    if (type.arguments.size() != expectedArguments) {
      report(type.name.last(),
             "Type '" + pathSpelling(type.name) + "' requires " +
                 std::to_string(expectedArguments) + " generic type argument" +
                 (expectedArguments == 1 ? "." : "s."));
    }
    for (const TypeRef &argument : type.arguments) {
      if (typeOf(argument) == SemanticType::Void) {
        report(argument.name.last(), "Generic type arguments cannot be void.");
      }
    }
  }

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

  std::vector<GenericParameterInfo>
  makeGenericParameters(const std::vector<GenericParameter> &parameters,
                        const Token &declarationName) {
    std::vector<GenericParameterInfo> result;
    result.reserve(parameters.size());
    std::unordered_set<std::string> names;
    for (const GenericParameter &parameter : parameters) {
      if (parameter.name.lexeme == declarationName.lexeme) {
        report(parameter.name, "Generic type parameter cannot have the same "
                               "name as its declaration.");
        continue;
      }
      if (!names.insert(parameter.name.lexeme).second) {
        report(parameter.name, "Duplicate generic type parameter '" +
                                   parameter.name.lexeme + "'.");
        continue;
      }
      result.push_back(
          GenericParameterInfo{nextGenericParameterId++, parameter.name});
    }
    return result;
  }

  [[nodiscard]] const std::vector<GenericParameterInfo> &
  genericParametersFor(const FunctionDecl &function) const {
    static const std::vector<GenericParameterInfo> empty;
    const auto found = functionGenericParameters.find(&function);
    return found == functionGenericParameters.end() ? empty : found->second;
  }

  void
  beginTypeParameterScope(const std::vector<GenericParameterInfo> &parameters) {
    auto &scope = typeParameterScopes.emplace_back();
    for (const GenericParameterInfo &parameter : parameters) {
      scope.emplace(parameter.name.lexeme,
                    SemanticType::typeParameter(parameter.id));
    }
  }

  void endTypeParameterScope() { typeParameterScopes.pop_back(); }

  [[nodiscard]] std::optional<SemanticType>
  resolveTypeParameter(const NamePath &name) const {
    if (name.segments.size() != 1) {
      return std::nullopt;
    }
    for (auto scope = typeParameterScopes.rbegin();
         scope != typeParameterScopes.rend(); ++scope) {
      if (const auto found = scope->find(name.last().lexeme);
          found != scope->end()) {
        return found->second;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] static SemanticType
  substituteType(const SemanticType &type,
                 const TypeSubstitution &substitution) {
    if (type.kind == SemanticType::TypeParameter) {
      const auto found = substitution.find(type.genericParameterId);
      return found == substitution.end() ? type : found->second;
    }

    SemanticType result = type;
    for (SemanticType &argument : result.arguments) {
      argument = substituteType(argument, substitution);
    }
    return result;
  }

  [[nodiscard]] TypeSubstitution
  classSubstitution(const SemanticType &objectType) const {
    TypeSubstitution result;
    if (objectType.kind != SemanticType::Class || objectType.classId == 0 ||
        objectType.classId > classes.size()) {
      return result;
    }
    const ClassInfo &owner = classInfo(objectType.classId);
    const std::size_t count =
        std::min(owner.genericParameters.size(), objectType.arguments.size());
    for (std::size_t index = 0; index < count; ++index) {
      result.emplace(owner.genericParameters[index].id,
                     objectType.arguments[index]);
    }
    return result;
  }

  [[nodiscard]] SemanticType openClassType(ClassId id) const {
    const ClassInfo &owner = classInfo(id);
    std::vector<SemanticType> arguments;
    arguments.reserve(owner.genericParameters.size());
    for (const GenericParameterInfo &parameter : owner.genericParameters) {
      arguments.emplace_back(SemanticType::typeParameter(parameter.id));
    }
    return SemanticType::classType(id, std::move(arguments));
  }

  [[nodiscard]] Symbol substituteSymbol(const Symbol &symbol,
                                        const SemanticType &objectType) const {
    Symbol result = symbol;
    const TypeSubstitution substitution = classSubstitution(objectType);
    result.type = substituteType(result.type, substitution);
    result.returnType = substituteType(result.returnType, substitution);
    for (SemanticType &parameter : result.parameterTypes) {
      parameter = substituteType(parameter, substitution);
    }
    return result;
  }

  Symbol functionSymbol(const FunctionDecl &function,
                        const std::vector<std::string> &scope) {
    const std::vector<GenericParameterInfo> &genericParameters =
        genericParametersFor(function);
    beginTypeParameterScope(genericParameters);
    Symbol symbol{.type = SemanticType::Function,
                  .assignable = false,
                  .returnType = typeOf(function.returnType(), scope),
                  .genericParameters = genericParameters,
                  .receiverMutability = function.receiverMutability()};
    symbol.parameterTypes.reserve(function.parameters().size());
    for (const Parameter &parameter : function.parameters()) {
      symbol.parameterTypes.emplace_back(typeOf(parameter.type, scope));
    }
    endTypeParameterScope();
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
        function.genericParameters().empty() && !function.body();
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

  void registerNamespaceAliases(const StmtList &statements,
                                std::vector<std::string> scope) {
    for (const StmtPtr &statement : statements) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          registerNamespaceAliases(*branch, scope);
        }
      } else if (const auto *alias =
                     dynamic_cast<const NamespaceAliasDecl *>(statement.get())) {
        const std::optional<std::string> targetNamespace =
            resolveNamespacePath(alias->target(), scope);
        if (!targetNamespace) {
          report(alias->target().last(), "Unknown namespace in alias target.");
          continue;
        }

        const std::string name = qualifiedName(scope, alias->name().lexeme);
        if (namespaces.contains(name) || namespaceAliases.contains(name)) {
          report(alias->name(), "Duplicate declaration of '" +
                                    alias->name().lexeme + "'.");
          continue;
        }
        namespaceAliases.emplace(name, *targetNamespace);
      } else if (const auto *namespaceDecl =
                     dynamic_cast<const NamespaceDecl *>(statement.get())) {
        scope.emplace_back(namespaceDecl->name().lexeme);
        registerNamespaceAliases(namespaceDecl->declarations(), scope);
        scope.pop_back();
      }
    }
  }

  void registerClasses(const StmtList &statements,
                       std::vector<std::string> scope) {
    for (const StmtPtr &statement : statements) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          registerClasses(*branch, scope);
        }
      } else if (const auto *classDecl =
                     dynamic_cast<const ClassDecl *>(statement.get())) {
        const std::string qualified =
            qualifiedName(scope, classDecl->name().lexeme);
        if (namespaces.contains(qualified) ||
            namespaceAliases.contains(qualified) || classIds.contains(qualified)) {
          report(classDecl->name(), "Duplicate declaration of '" +
                                        classDecl->name().lexeme + "'.");
          continue;
        }

        const ClassId id = classes.size() + 1;
        std::vector<GenericParameterInfo> genericParameters =
            makeGenericParameters(classDecl->genericParameters(),
                                  classDecl->name());
        classIds.emplace(qualified, id);
        classDeclIds.emplace(classDecl, id);
        classes.push_back(
            ClassInfo{.id = id,
                      .name = classDecl->name(),
                      .kind = classDecl->kind(),
                      .namespaceScope = scope,
                      .genericParameters = std::move(genericParameters)});
      } else if (const auto *namespaceDecl =
                     dynamic_cast<const NamespaceDecl *>(statement.get())) {
        scope.emplace_back(namespaceDecl->name().lexeme);
        registerClasses(namespaceDecl->declarations(), scope);
        scope.pop_back();
      }
    }
  }

  void registerFunctionGenericParameters(const StmtList &statements) {
    for (const StmtPtr &statement : statements) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          registerFunctionGenericParameters(*branch);
        }
      } else if (const auto *function =
                     dynamic_cast<const FunctionDecl *>(statement.get())) {
        functionGenericParameters.emplace(
            function, makeGenericParameters(function->genericParameters(),
                                            function->name()));
      } else if (const auto *classDecl =
                     dynamic_cast<const ClassDecl *>(statement.get())) {
        registerFunctionGenericParameters(classDecl->members());
      } else if (const auto *namespaceDecl =
                     dynamic_cast<const NamespaceDecl *>(statement.get())) {
        registerFunctionGenericParameters(namespaceDecl->declarations());
      }
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
                               functionSymbol(*function, scope));
      } else if (const auto *classDecl =
                     dynamic_cast<const ClassDecl *>(statement.get())) {
        if (const auto found = classDeclIds.find(classDecl);
            found != classDeclIds.end()) {
          declareNamespaceSymbol(scope, classDecl->name(),
                                 SemanticType::typeName(found->second), false);
        }
      } else if (const auto *namespaceDecl =
                     dynamic_cast<const NamespaceDecl *>(statement.get())) {
        scope.emplace_back(namespaceDecl->name().lexeme);
        registerNamespaceSymbols(namespaceDecl->declarations(), scope);
        scope.pop_back();
      }
    }
  }

  void collectClassMembers(const StmtList &statements,
                           std::vector<std::string> scope) {
    for (const StmtPtr &statement : statements) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          collectClassMembers(*branch, scope);
        }
      } else if (const auto *classDecl =
                     dynamic_cast<const ClassDecl *>(statement.get())) {
        const auto found = classDeclIds.find(classDecl);
        if (found == classDeclIds.end()) {
          continue;
        }
        ClassInfo &info = classInfo(found->second);
        AccessModifier access = info.kind == ClassKind::Class
                                    ? AccessModifier::Private
                                    : AccessModifier::Public;
        beginTypeParameterScope(info.genericParameters);
        collectMembers(classDecl->members(), info, access);
        endTypeParameterScope();
      } else if (const auto *namespaceDecl =
                     dynamic_cast<const NamespaceDecl *>(statement.get())) {
        scope.emplace_back(namespaceDecl->name().lexeme);
        collectClassMembers(namespaceDecl->declarations(), scope);
        scope.pop_back();
      }
    }
  }

  void collectMembers(const StmtList &members, ClassInfo &owner,
                      AccessModifier &access) {
    for (const StmtPtr &statement : members) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          collectMembers(*branch, owner, access);
        }
        continue;
      }
      if (const auto *specifier =
              dynamic_cast<const AccessSpecifierDecl *>(statement.get())) {
        access = specifier->modifier();
        continue;
      }

      if (const auto *constructor =
              dynamic_cast<const ConstructorDecl *>(statement.get())) {
        if (owner.constructor) {
          report(constructor->name(), "A class or struct cannot declare more "
                                      "than one constructor yet.");
          continue;
        }
        ConstructorInfo info{.declaration = constructor, .access = access};
        info.parameterTypes.reserve(constructor->parameters().size());
        for (const Parameter &parameter : constructor->parameters()) {
          info.parameterTypes.emplace_back(
              typeOf(parameter.type, owner.namespaceScope));
        }
        owner.constructor = std::move(info);
        continue;
      }

      const Token *name = nullptr;
      const VariableDecl *field = nullptr;
      Symbol symbol;
      if (const auto *function =
              dynamic_cast<const FunctionDecl *>(statement.get())) {
        for (const GenericParameter &methodParameter :
             function->genericParameters()) {
          for (const GenericParameterInfo &classParameter :
               owner.genericParameters) {
            if (methodParameter.name.lexeme == classParameter.name.lexeme) {
              report(methodParameter.name,
                     "Method generic type parameters cannot shadow class or "
                     "struct type parameters.");
            }
          }
        }
        name = &function->name();
        symbol = functionSymbol(*function, owner.namespaceScope);
      } else if (const auto *variable =
                     dynamic_cast<const VariableDecl *>(statement.get())) {
        name = &variable->name();
        field = variable;
        symbol = Symbol{.type = typeOf(variable->type(), owner.namespaceScope),
                        .assignable = variable->isMutable()};
        predeclaredVariables.insert(variable);
      }
      if (name == nullptr) {
        continue;
      }

      symbol.ownerClass = owner.id;
      const auto [_, inserted] = owner.members.emplace(
          name->lexeme, MemberInfo{.symbol = std::move(symbol), .access = access});
      if (!inserted) {
        report(*name, "Duplicate member declaration of '" + name->lexeme + "'.");
      } else if (field != nullptr) {
        owner.fields.push_back(FieldInfo{.declaration = field});
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
  resolveInitialNamespace(const Token &name,
                          const std::vector<std::string> &fromScope) const {
    for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
      std::vector<std::string> scope(fromScope.begin(),
                                     fromScope.begin() + depth - 1);
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
  resolveNamespacePath(const NamePath &path,
                       const std::vector<std::string> &fromScope) const {
    std::optional<std::string> current =
        resolveInitialNamespace(path.first(), fromScope);
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

  [[nodiscard]] std::optional<std::string>
  resolveNamespacePath(const NamePath &path) const {
    return resolveNamespacePath(path, currentNamespace);
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

  [[nodiscard]] std::optional<ClassId>
  resolveClassPath(const NamePath &path,
                   const std::vector<std::string> &fromScope) const {
    if (path.segments.size() == 1) {
      for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
        const std::vector<std::string> scope(fromScope.begin(),
                                             fromScope.begin() + depth - 1);
        const auto found =
            classIds.find(qualifiedName(scope, path.last().lexeme));
        if (found != classIds.end()) {
          return found->second;
        }
      }
      return std::nullopt;
    }

    NamePath namespacePath(std::vector<Token>(path.segments.begin(),
                                              path.segments.end() - 1));
    const std::optional<std::string> resolvedNamespace =
        resolveNamespacePath(namespacePath, fromScope);
    if (!resolvedNamespace) {
      return std::nullopt;
    }
    const auto found =
        classIds.find(*resolvedNamespace + "::" + path.last().lexeme);
    return found == classIds.end() ? std::nullopt
                                   : std::optional<ClassId>(found->second);
  }

  [[nodiscard]] std::optional<Symbol>
  resolveExpressionSymbol(const ExprPtr &expression) const {
    if (const auto *variable =
            dynamic_cast<const Variable *>(expression.get())) {
      const Symbol *symbol = resolve(variable->name());
      return symbol == nullptr ? std::nullopt : std::optional<Symbol>(*symbol);
    }
    if (const auto *qualified =
            dynamic_cast<const QualifiedName *>(expression.get())) {
      const Symbol *symbol = resolveQualified(qualified->name());
      return symbol == nullptr ? std::nullopt : std::optional<Symbol>(*symbol);
    }
    if (const auto *get = dynamic_cast<const Get *>(expression.get())) {
      const auto objectType = expressionTypes.find(get->object().get());
      if (objectType != expressionTypes.end()) {
        if (const MemberInfo *member =
                findMember(objectType->second, get->name())) {
          return substituteSymbol(member->symbol, objectType->second);
        }
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] bool isMutableTarget(const ExprPtr &expression) const {
    if (const auto *variable =
            dynamic_cast<const Variable *>(expression.get())) {
      const Symbol *symbol = resolve(variable->name());
      if (symbol == nullptr) {
        return true;
      }
      return symbol->assignable &&
             (symbol->ownerClass == 0 ||
              currentReceiverMutability == ReceiverMutability::Mutable);
    }
    if (const auto *get = dynamic_cast<const Get *>(expression.get())) {
      const auto objectType = expressionTypes.find(get->object().get());
      if (objectType == expressionTypes.end()) {
        return true;
      }
      const MemberInfo *member = findMember(objectType->second, get->name());
      return member == nullptr ||
             (member->symbol.assignable && isMutableObject(get->object()));
    }
    return false;
  }

  [[nodiscard]] bool isMutableObject(const ExprPtr &expression) const {
    if (dynamic_cast<const Self *>(expression.get()) != nullptr) {
      return currentReceiverMutability == ReceiverMutability::Mutable;
    }
    if (dynamic_cast<const Variable *>(expression.get()) != nullptr ||
        dynamic_cast<const Get *>(expression.get()) != nullptr) {
      return isMutableTarget(expression);
    }
    return false;
  }

  [[nodiscard]] const ClassInfo *classInfo(const SemanticType &type) const {
    if (type.kind != SemanticType::Class || type.classId == 0 ||
        type.classId > classes.size()) {
      return nullptr;
    }
    return &classes.at(type.classId - 1);
  }

  [[nodiscard]] ClassInfo &classInfo(ClassId id) {
    return classes.at(id - 1);
  }

  [[nodiscard]] const ClassInfo &classInfo(ClassId id) const {
    return classes.at(id - 1);
  }

  [[nodiscard]] const MemberInfo *findMember(const SemanticType &objectType,
                                             const Token &name) const {
    const ClassInfo *owner = classInfo(objectType);
    if (owner == nullptr) {
      return nullptr;
    }

    const auto found = owner->members.find(name.lexeme);
    return found == owner->members.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const MemberInfo *resolveMember(const SemanticType &objectType,
                                                const Token &name) {
    const ClassInfo *owner = classInfo(objectType);
    if (owner == nullptr) {
      if (objectType != SemanticType::Unknown) {
        report(name, "Member access requires a class or struct value.");
      }
      return nullptr;
    }

    const MemberInfo *member = findMember(objectType, name);
    if (member == nullptr) {
      report(name, "Unknown member '" + name.lexeme + "' on '" +
                       owner->name.lexeme + "'.");
      return nullptr;
    }
    if (member->access == AccessModifier::Private && currentClass != owner->id) {
      report(name, "Member '" + name.lexeme + "' of '" + owner->name.lexeme +
                       "' is private.");
    }
    return member;
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

  void requireInteger(SemanticType left, SemanticType right,
                      const Token &token) {
    if ((left != SemanticType::Unknown && !isInteger(left)) ||
        (right != SemanticType::Unknown && !isInteger(right))) {
      report(token, "Operator requires integer operands.");
    }
  }

  void validateShiftCount(SemanticType result, const Expr *right,
                          const Token &token) {
    const std::optional<IntegerConstant> count = integerConstant(right);
    if (!count) {
      return;
    }
    if (count->negative) {
      report(token, "Shift count cannot be negative.");
      return;
    }
    const int width = integerRank(result);
    if (width != 0 && count->magnitude >= static_cast<std::uint64_t>(width)) {
      report(token,
             "Shift count must be less than " + std::to_string(width) + ".");
    }
  }

  [[nodiscard]] static bool isNumeric(SemanticType type) {
    return isInteger(type) || type == SemanticType::Float;
  }

  [[nodiscard]] static bool isInteger(SemanticType type) {
    return type == SemanticType::Int8 || type == SemanticType::Int16 ||
           type == SemanticType::Int32 || type == SemanticType::Int64 ||
           type == SemanticType::UInt8 || type == SemanticType::UInt16 ||
           type == SemanticType::UInt32 || type == SemanticType::UInt64;
  }

  [[nodiscard]] static bool isSignedInteger(SemanticType type) {
    return type == SemanticType::Int8 || type == SemanticType::Int16 ||
           type == SemanticType::Int32 || type == SemanticType::Int64;
  }

  [[nodiscard]] static bool isUnsignedInteger(SemanticType type) {
    return type == SemanticType::UInt8 || type == SemanticType::UInt16 ||
           type == SemanticType::UInt32 || type == SemanticType::UInt64;
  }

  [[nodiscard]] static int integerRank(SemanticType type) {
    switch (type.kind) {
    case SemanticType::Int8:
    case SemanticType::UInt8:
      return 8;
    case SemanticType::Int16:
    case SemanticType::UInt16:
      return 16;
    case SemanticType::Int32:
    case SemanticType::UInt32:
      return 32;
    case SemanticType::Int64:
    case SemanticType::UInt64:
      return 64;
    default:
      return 0;
    }
  }

  struct IntegerConstant {
    bool negative = false;
    std::uint64_t magnitude = 0;
  };

  [[nodiscard]] static bool integerFits(SemanticType type,
                                        IntegerConstant value) {
    const int rank = integerRank(type);
    if (rank == 0) {
      return false;
    }
    if (isUnsignedInteger(type)) {
      const std::uint64_t maximum =
          rank == 64 ? std::numeric_limits<std::uint64_t>::max()
                     : (std::uint64_t{1} << rank) - 1;
      return !value.negative && value.magnitude <= maximum;
    }
    const std::uint64_t limit = std::uint64_t{1} << (rank - 1);
    return value.negative ? value.magnitude <= limit
                          : value.magnitude < limit;
  }

  [[nodiscard]] static std::optional<IntegerConstant>
  integerConstant(const Expr *expression) {
    if (expression == nullptr) {
      return std::nullopt;
    }
    if (const auto *literal = dynamic_cast<const LiteralExpr *>(expression)) {
      const auto *magnitude = std::get_if<std::uint64_t>(&literal->value());
      if (magnitude == nullptr) {
        return std::nullopt;
      }
      return IntegerConstant{.negative = false, .magnitude = *magnitude};
    }
    if (const auto *grouping = dynamic_cast<const Grouping *>(expression)) {
      return integerConstant(grouping->expression().get());
    }
    const auto *unary = dynamic_cast<const Unary *>(expression);
    if (unary == nullptr || (unary->oper().kind != TokenKind::MINUS &&
                             unary->oper().kind != TokenKind::PLUS)) {
      return std::nullopt;
    }
    std::optional<IntegerConstant> value =
        integerConstant(unary->right().get());
    if (!value || unary->oper().kind == TokenKind::PLUS) {
      return value;
    }
    value->negative = value->magnitude != 0 && !value->negative;
    return value;
  }

  [[nodiscard]] static bool integerRangeFits(SemanticType target,
                                             SemanticType value) {
    if (isSignedInteger(target) == isSignedInteger(value)) {
      return integerRank(value) <= integerRank(target);
    }
    return isSignedInteger(target) && isUnsignedInteger(value) &&
           integerRank(value) < integerRank(target);
  }

  [[nodiscard]] static SemanticType promotedInteger(SemanticType type) {
    if (type == SemanticType::Int8 || type == SemanticType::Int16 ||
        type == SemanticType::UInt8 || type == SemanticType::UInt16) {
      return SemanticType::Int32;
    }
    return type;
  }

  [[nodiscard]] static SemanticType widerInteger(SemanticType left,
                                                 SemanticType right) {
    return integerRank(left) >= integerRank(right) ? left : right;
  }

  [[nodiscard]] static bool canConvertToUnsigned(
      SemanticType originalType, const Expr *expression,
      SemanticType unsignedTarget) {
    if (isUnsignedInteger(originalType) &&
        integerRank(originalType) <= integerRank(unsignedTarget)) {
      return true;
    }
    if (const std::optional<IntegerConstant> constant =
            integerConstant(expression)) {
      return integerFits(unsignedTarget, *constant);
    }
    return false;
  }

  [[nodiscard]] static bool isContextuallyBool(const SemanticType &type) {
    return type == SemanticType::Bool || type.kind == SemanticType::Expected;
  }

  [[nodiscard]] static bool isExpectedVoid(const SemanticType &type) {
    return type.kind == SemanticType::Expected && type.arguments.size() == 2 &&
           type.arguments[0] == SemanticType::Void;
  }

  [[nodiscard]] static SemanticType numericResult(
      SemanticType left, SemanticType right, const Expr *leftExpression,
      const Expr *rightExpression) {
    if (left == SemanticType::Unknown || right == SemanticType::Unknown) {
      return SemanticType::Unknown;
    }
    if (left == SemanticType::Float || right == SemanticType::Float) {
      return SemanticType::Float;
    }
    if (!isInteger(left) || !isInteger(right)) {
      return SemanticType::Unknown;
    }

    if (isSignedInteger(left) != isSignedInteger(right)) {
      const SemanticType signedOriginal =
          isSignedInteger(left) ? left : right;
      const SemanticType unsignedOriginal =
          isUnsignedInteger(left) ? left : right;
      if (integerRangeFits(signedOriginal, unsignedOriginal)) {
        return promotedInteger(signedOriginal);
      }
      if (integerRank(left) < 32 && integerRank(right) < 32) {
        return SemanticType::Int32;
      }
    }

    const SemanticType promotedLeft = promotedInteger(left);
    const SemanticType promotedRight = promotedInteger(right);
    if (isSignedInteger(promotedLeft) == isSignedInteger(promotedRight)) {
      return widerInteger(promotedLeft, promotedRight);
    }

    const bool leftIsSigned = isSignedInteger(promotedLeft);
    const SemanticType signedType =
        leftIsSigned ? promotedLeft : promotedRight;
    const SemanticType unsignedType =
        leftIsSigned ? promotedRight : promotedLeft;
    if (integerRank(signedType) > integerRank(unsignedType)) {
      return signedType;
    }

    const SemanticType originalSignedType = leftIsSigned ? left : right;
    const Expr *signedExpression =
        leftIsSigned ? leftExpression : rightExpression;
    if (canConvertToUnsigned(originalSignedType, signedExpression,
                             unsignedType)) {
      return unsignedType;
    }
    return SemanticType::Unknown;
  }

  [[nodiscard]] static bool isAssignable(SemanticType target,
                                         SemanticType value,
                                         const Expr *expression = nullptr) {
    if (target == SemanticType::Unknown || value == SemanticType::Unknown) {
      return true;
    }
    if (target == SemanticType::Float && isInteger(value)) {
      return true;
    }
    if (isInteger(target) && isInteger(value)) {
      if (const std::optional<IntegerConstant> constant =
              integerConstant(expression)) {
        return integerFits(target, *constant);
      }
      return integerRangeFits(target, value);
    }
    if (target == value) {
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

  [[nodiscard]] static bool isComparable(SemanticType left, SemanticType right,
                                         const Expr *leftExpression,
                                         const Expr *rightExpression) {
    if (left.kind == SemanticType::TypeParameter ||
        right.kind == SemanticType::TypeParameter) {
      return false;
    }
    if (isInteger(left) && isInteger(right)) {
      return numericResult(left, right, leftExpression, rightExpression) !=
             SemanticType::Unknown;
    }
    return isAssignable(left, right, rightExpression) ||
           isAssignable(right, left, leftExpression);
  }

  [[nodiscard]] SemanticType
  typeOf(const TypeRef &type,
         const std::vector<std::string> &fromScope) const {
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
    case TokenKind::UINT:
    case TokenKind::UINT32:
      return SemanticType::UInt32;
    case TokenKind::UINT8:
      return SemanticType::UInt8;
    case TokenKind::UINT16:
      return SemanticType::UInt16;
    case TokenKind::UINT64:
      return SemanticType::UInt64;
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
        arguments.emplace_back(typeOf(argument, fromScope));
      }
      return SemanticType(SemanticType::Expected, std::move(arguments));
    }
    default:
      if (const std::optional<SemanticType> parameter =
              resolveTypeParameter(type.name)) {
        return type.arguments.empty() ? *parameter : SemanticType::Unknown;
      }
      if (const std::optional<ClassId> id =
              resolveClassPath(type.name, fromScope)) {
        std::vector<SemanticType> arguments;
        arguments.reserve(type.arguments.size());
        for (const TypeRef &argument : type.arguments) {
          arguments.emplace_back(typeOf(argument, fromScope));
        }
        return SemanticType::classType(*id, std::move(arguments));
      }
      return SemanticType::Unknown;
    }
  }

  [[nodiscard]] SemanticType typeOf(const TypeRef &type) const {
    return typeOf(type, currentNamespace);
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
      return SemanticType::UInt64;
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
  std::unordered_map<std::string, ClassId> classIds;
  std::unordered_map<const ClassDecl *, ClassId> classDeclIds;
  std::unordered_map<const FunctionDecl *, std::vector<GenericParameterInfo>>
      functionGenericParameters;
  std::vector<ClassInfo> classes;
  std::vector<std::unordered_map<std::string, SemanticType>>
      typeParameterScopes;
  std::unordered_map<const Expr *, SemanticType> expressionTypes;
  std::vector<std::string> currentNamespace;
  std::unordered_set<const VariableDecl *> predeclaredVariables;
  TargetInfo target;
  SemanticType currentType = SemanticType::Unknown;
  SemanticType currentReturnType = SemanticType::Unknown;
  std::optional<ClassId> currentClass;
  bool analyzingFieldInitializer = false;
  bool analyzingConstructorInitializer = false;
  ReceiverMutability currentReceiverMutability = ReceiverMutability::ReadOnly;
  std::size_t constructorDepth = 0;
  std::size_t functionDepth = 0;
  GenericParameterId nextGenericParameterId = 1;
};

} // namespace lang
