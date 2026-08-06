#pragma once

#include "gti/ast.h"
#include "gti/diagnostic.h"

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
using FunctionId = std::size_t;

struct GenericParameterInfo {
  GenericParameterId id = 0;
  Token name;
};

enum class ValueCategory {
  Value,
  Place,
};

enum class AccessMode {
  ReadOnly,
  Mutable,
};

enum class OwnershipKind {
  Value,
  Borrowed,
  Unique,
  Shared,
};

enum class DropKind {
  Trivial,
  Lexical,
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
    Array,
    Class,
    Reference,
    UniquePointer,
    SharedPointer,
    Storage,
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

  [[nodiscard]] static SemanticType arrayOf(SemanticType element,
                                            std::uint64_t length) {
    SemanticType type(Array, {std::move(element)});
    type.arrayLength = length;
    return type;
  }

  [[nodiscard]] static SemanticType
  referenceTo(SemanticType referent, AccessMode access = AccessMode::ReadOnly) {
    SemanticType type(Reference, {std::move(referent)});
    type.referenceAccess = access;
    return type;
  }

  [[nodiscard]] static SemanticType uniquePointerTo(SemanticType pointee) {
    return SemanticType(UniquePointer, {std::move(pointee)});
  }

  [[nodiscard]] static SemanticType sharedPointerTo(SemanticType pointee) {
    return SemanticType(SharedPointer, {std::move(pointee)});
  }

  [[nodiscard]] static SemanticType storageOf(SemanticType element) {
    return SemanticType(Storage, {std::move(element)});
  }

  friend bool operator==(const SemanticType &, const SemanticType &) = default;

  Kind kind;
  std::vector<SemanticType> arguments;
  ClassId classId = 0;
  GenericParameterId genericParameterId = 0;
  std::uint64_t arrayLength = 0;
  AccessMode referenceAccess = AccessMode::ReadOnly;
};

struct SemanticTypeTraits {
  OwnershipKind ownership = OwnershipKind::Value;
  DropKind drop = DropKind::Trivial;
  bool copyable = true;
  bool movable = true;
};

[[nodiscard]] inline SemanticTypeTraits
semanticTraits(const SemanticType &type) {
  SemanticTypeTraits traits;
  switch (type.kind) {
  case SemanticType::Unknown:
    traits.drop = DropKind::Lexical;
    traits.copyable = false;
    traits.movable = false;
    return traits;
  case SemanticType::Void:
  case SemanticType::TypeName:
  case SemanticType::Function:
    traits.copyable = false;
    traits.movable = false;
    return traits;
  case SemanticType::Reference:
    traits.ownership = OwnershipKind::Borrowed;
    return traits;
  case SemanticType::Array:
    if (type.arguments.size() == 1) {
      const SemanticTypeTraits element = semanticTraits(type.arguments[0]);
      traits.drop = element.drop;
      traits.copyable = element.copyable;
      traits.movable = element.movable;
      return traits;
    }
    traits.drop = DropKind::Lexical;
    traits.copyable = false;
    traits.movable = false;
    return traits;
  case SemanticType::UniquePointer:
    traits.ownership = OwnershipKind::Unique;
    traits.drop = DropKind::Lexical;
    traits.copyable = false;
    return traits;
  case SemanticType::Storage:
    traits.ownership = OwnershipKind::Unique;
    traits.drop = DropKind::Lexical;
    traits.copyable = false;
    return traits;
  case SemanticType::SharedPointer:
    traits.ownership = OwnershipKind::Shared;
    traits.drop = DropKind::Lexical;
    return traits;
  case SemanticType::String:
  case SemanticType::Class:
  case SemanticType::TypeParameter:
  case SemanticType::Expected:
  case SemanticType::Unexpected:
    traits.drop = DropKind::Lexical;
    return traits;
  default:
    return traits;
  }
}

struct ExpressionInfo {
  SemanticType type = SemanticType::Unknown;
  ValueCategory category = ValueCategory::Value;
  AccessMode access = AccessMode::ReadOnly;
  SemanticTypeTraits traits{};
};

struct BindingInfo {
  SemanticType type = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  SemanticTypeTraits traits{};
};

struct FunctionInfo {
  FunctionId id = 0;
  const FunctionDecl *declaration = nullptr;
  std::string qualifiedName;
  bool entryPoint = false;
};

enum class IntrinsicKind {
  None,
  MakeUnique,
  Move,
  AllocateStorage,
  StorageCapacity,
  StorageConstruct,
  StorageRead,
  StorageDestroy,
  StorageRelocate,
};

struct ResolvedCallInfo {
  FunctionId function = 0;
  const FunctionDecl *declaration = nullptr;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<SemanticType> typeArguments;
  IntrinsicKind intrinsic = IntrinsicKind::None;
};

[[nodiscard]] inline ExpressionInfo
makeExpressionInfo(SemanticType type,
                   ValueCategory category = ValueCategory::Value,
                   AccessMode access = AccessMode::ReadOnly) {
  const SemanticTypeTraits traits = semanticTraits(type);
  return ExpressionInfo{.type = std::move(type),
                        .category = category,
                        .access = access,
                        .traits = traits};
}

[[nodiscard]] inline BindingInfo
makeBindingInfo(SemanticType type, AccessMode access = AccessMode::ReadOnly) {
  const SemanticTypeTraits traits = semanticTraits(type);
  return BindingInfo{
      .type = std::move(type), .access = access, .traits = traits};
}

using TypeSubstitution = std::unordered_map<GenericParameterId, SemanticType>;

using SemanticDiagnostic = Diagnostic;

class SemanticModel {
public:
  // AST identities remain valid while the analyzed Program is alive.
  [[nodiscard]] const ExpressionInfo *
  findExpression(const Expr &expression) const {
    const auto found = expressions.find(&expression);
    return found == expressions.end() ? nullptr : &found->second;
  }

  [[nodiscard]] ExpressionInfo expressionInfo(const Expr &expression) const {
    const ExpressionInfo *info = findExpression(expression);
    return info == nullptr ? makeExpressionInfo(SemanticType::Unknown) : *info;
  }

  [[nodiscard]] const SemanticType *findType(const Expr &expression) const {
    const ExpressionInfo *info = findExpression(expression);
    return info == nullptr ? nullptr : &info->type;
  }

  [[nodiscard]] SemanticType typeOf(const Expr &expression) const {
    const SemanticType *type = findType(expression);
    return type == nullptr ? SemanticType::Unknown : *type;
  }

  [[nodiscard]] bool hasType(const Expr &expression) const {
    return expressions.contains(&expression);
  }

  [[nodiscard]] std::size_t expressionCount() const {
    return expressions.size();
  }

  [[nodiscard]] const BindingInfo *
  findBinding(const VariableDecl &declaration) const {
    const auto found = variableBindings.find(&declaration);
    return found == variableBindings.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const BindingInfo *
  findBinding(const Parameter &parameter) const {
    const auto found = parameterBindings.find(&parameter);
    return found == parameterBindings.end() ? nullptr : &found->second;
  }

  [[nodiscard]] std::size_t bindingCount() const {
    return variableBindings.size() + parameterBindings.size();
  }

  [[nodiscard]] const FunctionInfo *
  findFunction(const FunctionDecl &declaration) const {
    const auto found = functions.find(&declaration);
    return found == functions.end() ? nullptr : &found->second;
  }

  [[nodiscard]] const ResolvedCallInfo *findCall(const Call &call) const {
    const auto found = calls.find(&call);
    return found == calls.end() ? nullptr : &found->second;
  }

  [[nodiscard]] std::size_t functionCount() const { return functions.size(); }

  [[nodiscard]] std::size_t resolvedCallCount() const { return calls.size(); }

private:
  friend class SemanticVisitor;

  void clear() {
    expressions.clear();
    variableBindings.clear();
    parameterBindings.clear();
    functions.clear();
    calls.clear();
  }

  void record(const Expr &expression, ExpressionInfo info) {
    expressions.insert_or_assign(&expression, std::move(info));
  }

  void record(const VariableDecl &declaration, BindingInfo info) {
    variableBindings.insert_or_assign(&declaration, std::move(info));
  }

  void record(const Parameter &parameter, BindingInfo info) {
    parameterBindings.insert_or_assign(&parameter, std::move(info));
  }

  void record(const FunctionDecl &declaration, FunctionInfo info) {
    functions.insert_or_assign(&declaration, std::move(info));
  }

  void record(const Call &call, ResolvedCallInfo info) {
    calls.insert_or_assign(&call, std::move(info));
  }

  std::unordered_map<const Expr *, ExpressionInfo> expressions;
  std::unordered_map<const VariableDecl *, BindingInfo> variableBindings;
  std::unordered_map<const Parameter *, BindingInfo> parameterBindings;
  std::unordered_map<const FunctionDecl *, FunctionInfo> functions;
  std::unordered_map<const Call *, ResolvedCallInfo> calls;
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
    nextFunctionId = 1;
    currentNamespace.clear();
    predeclaredVariables.clear();
    semanticModel.clear();
    currentClass.reset();
    analyzingFieldInitializer = false;
    analyzingConstructorInitializer = false;
    analyzingCallCallee = false;
    contextualInitializerType.reset();
    currentReceiverMutability = ReceiverMutability::ReadOnly;
    constructorDepth = 0;
    functionDepth = 0;
    loopDepth = 0;
    currentReturnType = SemanticType::Unknown;

    registerNamespaces(program.declarations(), {});
    registerNamespaceAliases(program.declarations(), {});
    registerClasses(program.declarations(), {});
    registerFunctionGenericParameters(program.declarations(), {}, false);
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
    nextFunctionId = 1;
    currentNamespace.clear();
    predeclaredVariables.clear();
    semanticModel.clear();
    currentClass.reset();
    analyzingFieldInitializer = false;
    analyzingConstructorInitializer = false;
    analyzingCallCallee = false;
    contextualInitializerType.reset();
    currentReceiverMutability = ReceiverMutability::ReadOnly;
    constructorDepth = 0;
    functionDepth = 0;
    loopDepth = 0;
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

  [[nodiscard]] const SemanticModel &model() const { return semanticModel; }

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
      validateReferencePlacement(parameter.type, true, "constructor parameter");
      const SemanticType parameterType = typeOf(parameter);
      semanticModel.record(
          parameter, makeBindingInfo(parameterType,
                                     parameter.mutability == Mutability::Mutable
                                         ? AccessMode::Mutable
                                         : AccessMode::ReadOnly));
      if (parameterType == SemanticType::Void) {
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
        declare(parameter.name, typeOf(parameter),
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
      const SemanticType fieldType =
          field == nullptr ? SemanticType::Unknown
                           : typeOf(field->type(), owner.namespaceScope);
      const SemanticType valueType =
          field == nullptr ? analyze(initializer.value)
                           : analyzeInitializer(initializer.value, fieldType);
      analyzingConstructorInitializer = enclosingConstructorInitializer;
      if (field != nullptr &&
          !isOwnershipAssignable(fieldType, valueType, initializer.value)) {
        report(expressionToken(initializer.value),
               "Cannot initialize field '" + initializer.field.lexeme +
                   "' of type '" + typeSpelling(fieldType) +
                   "' with a value of type '" + typeSpelling(valueType) + "'.",
               "GTI-S2003");
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
             "with '[[discard]]'.",
             "GTI-S2009");
    }
  }

  void visitForStmt(const ForStmt &stmt) override {
    beginScope();
    analyze(stmt.initializer());
    if (stmt.condition()) {
      requireBool(analyze(stmt.condition()), expressionToken(stmt.condition()),
                  "For-loop condition must be bool.");
    }
    const ScopeStack beforeLoop = scopes;
    ++loopDepth;
    analyze(stmt.body());
    analyze(stmt.increment());
    --loopDepth;
    const ScopeStack afterIteration = scopes;
    scopes = beforeLoop;
    mergeOwnerStates(beforeLoop, beforeLoop, afterIteration);
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
    validateReferencePlacement(stmt.returnType(), false,
                               "function return type");
    for (const Parameter &parameter : stmt.parameters()) {
      validateType(parameter.type);
      validateReferencePlacement(parameter.type, true, "function parameter");
      const SemanticType parameterType = typeOf(parameter);
      semanticModel.record(
          parameter, makeBindingInfo(parameterType,
                                     parameter.mutability == Mutability::Mutable
                                         ? AccessMode::Mutable
                                         : AccessMode::ReadOnly));
      if (parameterType == SemanticType::Void) {
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
        declare(parameter.name, typeOf(parameter),
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
    const ScopeStack beforeBranches = scopes;
    analyze(stmt.thenBranch());
    const ScopeStack thenScopes = scopes;
    scopes = beforeBranches;
    if (stmt.elseBranch()) {
      analyze(stmt.elseBranch());
    }
    const ScopeStack elseScopes = scopes;
    scopes = beforeBranches;
    mergeOwnerStates(beforeBranches, thenScopes, elseScopes);
  }

  void visitLoopControlStmt(const LoopControlStmt &stmt) override {
    if (loopDepth == 0) {
      report(stmt.keyword(),
             "'" + stmt.keyword().lexeme + "' can only be used inside a loop.",
             "GTI-S2010");
    }
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

    const SemanticType valueType =
        analyzeInitializer(stmt.value(), currentReturnType);
    if (!isOwnershipAssignable(currentReturnType, valueType, stmt.value())) {
      report(expressionToken(stmt.value()),
             "Cannot return a value of type '" + typeSpelling(valueType) +
                 "' from a function returning '" +
                 typeSpelling(currentReturnType) + "'.",
             "GTI-S2003");
      if (isMoveOnlyOwnerType(currentReturnType) &&
          currentReturnType == valueType) {
        diagnostics.back().hints.emplace_back(
            "Return std::move(owner) to transfer ownership.");
      }
    }
  }

  void visitVariableDecl(const VariableDecl &stmt) override {
    validateType(stmt.type());
    const bool localReference = functionDepth > 0;
    validateReferencePlacement(stmt.type(), localReference,
                               localReference ? "local binding" : "storage");
    const SemanticType declaredType =
        typeOf(stmt.type(),
               stmt.isMutable() ? Mutability::Mutable : Mutability::Immutable);
    semanticModel.record(
        stmt,
        makeBindingInfo(declaredType, stmt.isMutable() ? AccessMode::Mutable
                                                       : AccessMode::ReadOnly));
    SemanticType initializerType = SemanticType::Unknown;
    if (declaredType == SemanticType::Void) {
      report(stmt.type().name.last(), "Variables cannot have type void.");
    } else if (declaredType.kind == SemanticType::UniquePointer &&
               functionDepth == 0) {
      report(stmt.name(),
             "Unique owners can only be local bindings or function values in "
             "this allocation layer.",
             "GTI-S2018");
    } else if (declaredType.kind == SemanticType::Storage &&
               functionDepth == 0 && !currentClass) {
      report(stmt.name(),
             "Compiler-private storage can only be used as a local binding or "
             "class field.",
             "GTI-S2019");
    } else if (declaredType.kind == SemanticType::Storage &&
               functionDepth > 0 && !stmt.initializer()) {
      report(stmt.name(),
             "Compiler-private storage bindings require an allocation "
             "initializer.",
             "GTI-S2019");
    } else if (declaredType.kind == SemanticType::UniquePointer &&
               !stmt.initializer()) {
      report(stmt.name(),
             "Unique owner bindings require an initializer; use nullptr for an "
             "empty owner.",
             "GTI-S2018");
    } else if (declaredType.kind == SemanticType::Reference &&
               !stmt.initializer()) {
      report(stmt.name(), "Reference bindings require an initializer.",
             "GTI-S2017");
    } else if (!stmt.initializer()) {
      const bool field = currentClass && functionDepth == 0;
      if (!field && declaredType.kind == SemanticType::Array) {
        report(stmt.name(),
               "Fixed array variables require an initializer; use '{}' to "
               "default-initialize every element.",
               "GTI-S2015");
      } else if (!field && declaredType.kind == SemanticType::Class) {
        report(stmt.name(),
               "Class and struct variables require explicit construction.");
      } else if (!field && !stmt.isMutable()) {
        report(stmt.name(), "Immutable variable must have an initializer.");
      }
    }
    if (stmt.initializer()) {
      const bool enclosingFieldInitializer = analyzingFieldInitializer;
      analyzingFieldInitializer = currentClass && functionDepth == 0;
      const SemanticType expectedInitializer =
          declaredType.kind == SemanticType::Reference &&
                  declaredType.arguments.size() == 1
              ? declaredType.arguments[0]
              : declaredType;
      initializerType =
          analyzeInitializer(stmt.initializer(), expectedInitializer);
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

    if (stmt.initializer() && declaredType.kind == SemanticType::Reference) {
      validateReferenceBinding(declaredType, initializerType,
                               stmt.initializer());
    } else if (stmt.initializer() &&
               !isOwnershipAssignable(declaredType, initializerType,
                                      stmt.initializer())) {
      report(expressionToken(stmt.initializer()),
             "Cannot initialize '" + stmt.name().lexeme + "' of type '" +
                 typeSpelling(declaredType) + "' with a value of type '" +
                 typeSpelling(initializerType) + "'.",
             "GTI-S2003");
      if (isMoveOnlyOwnerType(declaredType) &&
          declaredType == initializerType) {
        diagnostics.back().hints.emplace_back(
            "Move-only owners cannot be copied; transfer ownership explicitly "
            "with std::move(owner).");
      }
    }
  }

  void visitWhileStmt(const WhileStmt &stmt) override {
    requireBool(analyze(stmt.condition()), expressionToken(stmt.condition()),
                "While condition must be bool.");
    const ScopeStack beforeLoop = scopes;
    ++loopDepth;
    analyze(stmt.body());
    --loopDepth;
    const ScopeStack afterIteration = scopes;
    scopes = beforeLoop;
    mergeOwnerStates(beforeLoop, beforeLoop, afterIteration);
  }

  void visitAssignExpr(const Assign &expr) override {
    const Symbol *symbol = resolve(expr.name());
    if (symbol == nullptr) {
      report(expr.name(), "Undefined variable '" + expr.name().lexeme + "'.",
             "GTI-S2001");
      currentType = analyze(expr.value());
      return;
    }
    const SemanticType targetType =
        symbol->type.kind == SemanticType::Reference &&
                symbol->type.arguments.size() == 1
            ? symbol->type.arguments[0]
            : symbol->type;
    const SemanticType valueType = analyzeInitializer(expr.value(), targetType);
    if (!symbol->assignable) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2002", DiagnosticPhase::Semantics, expr.name(),
          "Cannot assign to immutable binding '" + expr.name().lexeme + "'.");
      if (!symbol->declaration.lexeme.empty()) {
        diagnostic.related.push_back(
            {tokenSpan(symbol->declaration), "Binding declared here."});
      }
      diagnostic.hints.emplace_back(
          "Bindings are immutable by default; add 'mut' to the declaration if "
          "mutation is required.");
      diagnostics.emplace_back(std::move(diagnostic));
    } else if (symbol->ownerClass != 0 &&
               currentReceiverMutability != ReceiverMutability::Mutable) {
      report(expr.name(), "Cannot mutate through a read-only receiver.");
    }
    const bool valueAssignable =
        isOwnershipAssignable(targetType, valueType, expr.value());
    if (!valueAssignable) {
      report(expressionToken(expr.value()),
             "Cannot assign a value of type '" + typeSpelling(valueType) +
                 "' to '" + expr.name().lexeme + "' of type '" +
                 typeSpelling(targetType) + "'.",
             "GTI-S2003");
      if (isMoveOnlyOwnerType(targetType) && targetType == valueType) {
        diagnostics.back().hints.emplace_back(
            "Move-only owners cannot be copied; use std::move(owner) to "
            "transfer "
            "ownership.");
      }
    }
    if (expr.oper().kind != TokenKind::EQUAL &&
        ((targetType != SemanticType::Unknown && !isNumeric(targetType)) ||
         (valueType != SemanticType::Unknown && !isNumeric(valueType)))) {
      report(expr.oper(), "Compound assignment requires numeric operands.");
    }
    if (valueAssignable && isMoveOnlyOwnerType(targetType)) {
      if (Symbol *target = resolveMutable(expr.name())) {
        target->ownerState = OwnerState::Available;
      }
    }
    currentType = targetType;
  }

  void visitArrayInitializerExpr(const ArrayInitializer &expr) override {
    if (!contextualInitializerType ||
        contextualInitializerType->kind != SemanticType::Array ||
        contextualInitializerType->arguments.size() != 1) {
      for (const ExprPtr &element : expr.elements()) {
        analyze(element);
      }
      report(expr.brace(),
             "Array initializer requires a fixed array type from its context.",
             "GTI-S2015");
      currentType = SemanticType::Unknown;
      return;
    }

    const SemanticType arrayType = *contextualInitializerType;
    const SemanticType elementType = arrayType.arguments[0];
    if (!expr.elements().empty() &&
        expr.elements().size() != arrayType.arrayLength) {
      report(expr.brace(),
             "Fixed array initializer provides " +
                 std::to_string(expr.elements().size()) + " elements but '" +
                 typeSpelling(arrayType) + "' requires exactly " +
                 std::to_string(arrayType.arrayLength) + ".",
             "GTI-S2015");
    }
    if (expr.elements().empty() && arrayType.arrayLength != 0 &&
        !isDefaultInitializable(elementType)) {
      report(expr.brace(),
             "Empty initialization of '" + typeSpelling(arrayType) +
                 "' requires a default-initializable element type.",
             "GTI-S2015");
    }

    for (const ExprPtr &element : expr.elements()) {
      const SemanticType valueType = analyzeInitializer(element, elementType);
      if (!isAssignable(elementType, valueType, element.get())) {
        report(expressionToken(element),
               "Cannot initialize array element of type '" +
                   typeSpelling(elementType) + "' with a value of type '" +
                   typeSpelling(valueType) + "'.",
               "GTI-S2003");
      }
    }
    currentType = arrayType;
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
    if (const IntrinsicKind intrinsic = intrinsicKind(expr.callee());
        intrinsic != IntrinsicKind::None) {
      analyzeIntrinsicCall(expr, intrinsic);
      return;
    }
    const bool enclosingCallCallee = analyzingCallCallee;
    analyzingCallCallee = true;
    const SemanticType calleeType = analyze(expr.callee());
    analyzingCallCallee = enclosingCallCallee;
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
      const SemanticType *objectType =
          semanticModel.findType(*member->object());
      if (objectType != nullptr && objectType->kind == SemanticType::Expected) {
        if (!explicitTypeArguments.empty()) {
          report(expr.paren(),
                 "Expected member functions do not take generic arguments.");
        }
        analyzeExpectedMemberCall(*member, *objectType, argumentTypes,
                                  expr.arguments(), expr.paren());
        return;
      }
      if (objectType != nullptr && objectType->kind == SemanticType::Array) {
        if (!explicitTypeArguments.empty()) {
          report(expr.paren(),
                 "Fixed array member functions do not take generic arguments.");
        }
        analyzeArrayMemberCall(*member, argumentTypes, expr.paren());
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
    if (!callee || callee->type != SemanticType::Function ||
        callee->overloads.empty()) {
      currentType = SemanticType::Unknown;
      return;
    }

    if (callee->overloads.size() == 1) {
      const FunctionCandidate &candidate = callee->overloads.front();
      FunctionCandidate resolved = candidate;
      bool valid = applyFunctionTypeArguments(resolved, explicitTypeArguments,
                                              argumentTypes, expr.arguments(),
                                              expr.paren());
      std::vector<SemanticType> resolvedTypeArguments;
      FunctionCandidate trial;
      if (tryInstantiateFunction(candidate, explicitTypeArguments,
                                 argumentTypes, trial, resolvedTypeArguments)) {
        resolved = std::move(trial);
      }

      if (argumentTypes.size() != resolved.parameterTypes.size()) {
        report(
            expr.paren(),
            "Function expects " +
                std::to_string(resolved.parameterTypes.size()) + " argument" +
                (resolved.parameterTypes.size() == 1 ? "" : "s") +
                " but received " + std::to_string(argumentTypes.size()) + ".",
            "GTI-S2005");
        valid = false;
      } else {
        for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
          if (argumentTypes[index] != SemanticType::Unknown &&
              resolved.parameterTypes[index] != SemanticType::Unknown &&
              !callArgumentMatches(resolved.parameterTypes[index],
                                   argumentTypes[index],
                                   expr.arguments()[index])) {
            reportCallArgumentMismatch(index, resolved.parameterTypes[index],
                                       argumentTypes[index],
                                       expr.arguments()[index], "Function");
            valid = false;
          }
        }
      }

      validateSelectedFunction(candidate, expr.callee(), expr.paren());
      if (valid) {
        recordResolvedCall(expr, resolved, resolvedTypeArguments);
      }
      currentType = resolved.returnType;
      return;
    }

    struct ViableOverload {
      FunctionCandidate function;
      std::vector<SemanticType> typeArguments;
    };
    std::vector<ViableOverload> viable;
    for (const FunctionCandidate &candidate : callee->overloads) {
      if (candidate.parameterTypes.size() != argumentTypes.size()) {
        continue;
      }
      FunctionCandidate resolved;
      std::vector<SemanticType> resolvedTypeArguments;
      if (!tryInstantiateFunction(candidate, explicitTypeArguments,
                                  argumentTypes, resolved,
                                  resolvedTypeArguments)) {
        continue;
      }
      bool exact = true;
      for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
        if (argumentTypes[index] != SemanticType::Unknown &&
            resolved.parameterTypes[index] != SemanticType::Unknown &&
            !callArgumentMatches(resolved.parameterTypes[index],
                                 argumentTypes[index],
                                 expr.arguments()[index])) {
          exact = false;
          break;
        }
      }
      if (exact) {
        viable.push_back(
            {std::move(resolved), std::move(resolvedTypeArguments)});
      }
    }

    const bool hasUnknownArgument = std::any_of(
        argumentTypes.begin(), argumentTypes.end(),
        [](const SemanticType &type) { return type == SemanticType::Unknown; });
    if (viable.size() != 1) {
      if (!hasUnknownArgument) {
        std::vector<const FunctionCandidate *> exactMatches;
        exactMatches.reserve(viable.size());
        for (const ViableOverload &match : viable) {
          exactMatches.emplace_back(&match.function);
        }
        reportOverloadResolutionFailure(expr, *callee, argumentTypes,
                                        exactMatches);
      }
      currentType = SemanticType::Unknown;
      return;
    }

    validateSelectedFunction(viable.front().function, expr.callee(),
                             expr.paren());
    recordResolvedCall(expr, viable.front().function,
                       viable.front().typeArguments);
    currentType = viable.front().function.returnType;
  }

  void visitConversionExpr(const Conversion &expr) override {
    validateType(expr.targetType());
    const SemanticType targetType = typeOf(expr.targetType());
    const SemanticType valueType = analyze(expr.value());
    if (!isNumeric(targetType)) {
      report(expr.targetType().name.last(),
             "Explicit conversions currently require a numeric target type.",
             "GTI-S2014");
      currentType = SemanticType::Unknown;
      return;
    }
    if (valueType != SemanticType::Unknown && !isNumeric(valueType)) {
      report(expressionToken(expr.value()),
             "Cannot explicitly convert '" + typeSpelling(valueType) +
                 "' to '" + typeSpelling(targetType) +
                 "'; numeric conversions require a numeric value.",
             "GTI-S2014");
      currentType = SemanticType::Unknown;
      return;
    }
    if (isInteger(targetType) && isInteger(valueType)) {
      if (const std::optional<IntegerConstant> constant =
              integerConstant(expr.value().get());
          constant && !integerFits(targetType, *constant)) {
        report(expressionToken(expr.value()),
               "Integer value is outside the range of '" +
                   typeSpelling(targetType) + "'.",
               "GTI-S2014");
      }
    }
    currentType = targetType;
  }

  void visitGetExpr(const Get &expr) override {
    SemanticType objectType = analyze(expr.object());
    if (expr.access().kind == TokenKind::ARROW) {
      if (objectType.kind != SemanticType::UniquePointer ||
          objectType.arguments.size() != 1) {
        report(expr.access(),
               "Operator '->' requires a std::unique_ptr<T> owner.",
               "GTI-S2018");
        currentType = SemanticType::Unknown;
        return;
      }
      objectType = objectType.arguments[0];
    } else if (objectType.kind == SemanticType::UniquePointer) {
      report(expr.access(),
             "Unique-owner members use '->'; '.' does not implicitly expose "
             "the owner representation.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    if (objectType.kind == SemanticType::Array) {
      if (expr.name().lexeme == "size") {
        if (!analyzingCallCallee) {
          report(expr.name(),
                 "Function names must be called; function values are not "
                 "supported yet.");
        }
        currentType = SemanticType::Function;
      } else {
        report(expr.name(),
               "Unknown fixed array member '" + expr.name().lexeme + "'.",
               "GTI-S2016");
        currentType = SemanticType::Unknown;
      }
      return;
    }
    if (objectType.kind == SemanticType::Expected) {
      if (expr.name().lexeme == "has_value" || expr.name().lexeme == "value" ||
          expr.name().lexeme == "error" || expr.name().lexeme == "value_or") {
        if (!analyzingCallCallee) {
          report(expr.name(),
                 "Function names must be called; function values are not "
                 "supported yet.");
        }
        currentType = SemanticType::Function;
      } else {
        report(expr.name(), "Unknown expected member '" + expr.name().lexeme +
                                "'.");
        currentType = SemanticType::Unknown;
      }
      return;
    }
    const MemberInfo *member = resolveMember(objectType, expr.name());
    if (member == nullptr) {
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = substituteSymbol(member->symbol, objectType).type;
    if (currentType == SemanticType::Function && !analyzingCallCallee) {
      report(expr.name(),
             "Function names must be called; function values are not "
             "supported yet.");
    }
  }

  void visitGroupingExpr(const Grouping &expr) override {
    currentType = analyze(expr.expression());
  }

  void visitIndexExpr(const Index &expr) override {
    currentType =
        analyzeArrayIndex(expr.object(), expr.index(), expr.bracket());
  }

  void visitIndexSetExpr(const IndexSet &expr) override {
    const SemanticType elementType =
        analyzeArrayIndex(expr.object(), expr.index(), expr.bracket());
    const SemanticType valueType =
        analyzeInitializer(expr.value(), elementType);
    if (!isMutableObject(expr.object())) {
      report(expr.bracket(),
             "Cannot assign through an immutable fixed array binding.",
             "GTI-S2002");
    }
    if (!isAssignable(elementType, valueType, expr.value().get())) {
      report(expressionToken(expr.value()),
             "Cannot assign a value of type '" + typeSpelling(valueType) +
                 "' to an array element of type '" + typeSpelling(elementType) +
                 "'.",
             "GTI-S2003");
    }
    if (expr.oper().kind != TokenKind::EQUAL &&
        ((elementType != SemanticType::Unknown && !isNumeric(elementType)) ||
         (valueType != SemanticType::Unknown && !isNumeric(valueType)))) {
      report(expr.oper(), "Compound assignment requires numeric operands.");
    }
    currentType = elementType;
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
    if (symbol->type == SemanticType::Function && !analyzingCallCallee) {
      report(expr.name().last(),
             "Function names must be called; function values are not "
             "supported yet.");
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
    SemanticType objectType = analyze(expr.object());
    if (expr.access().kind == TokenKind::ARROW) {
      if (objectType.kind != SemanticType::UniquePointer ||
          objectType.arguments.size() != 1) {
        report(expr.access(),
               "Operator '->' requires a std::unique_ptr<T> owner.",
               "GTI-S2018");
        analyze(expr.value());
        currentType = SemanticType::Unknown;
        return;
      }
      objectType = objectType.arguments[0];
    } else if (objectType.kind == SemanticType::UniquePointer) {
      report(expr.access(),
             "Unique-owner members use '->'; '.' does not implicitly expose "
             "the owner representation.",
             "GTI-S2018");
      analyze(expr.value());
      currentType = SemanticType::Unknown;
      return;
    }

    const MemberInfo *member = resolveMember(objectType, expr.name());
    if (member == nullptr) {
      analyze(expr.value());
      currentType = SemanticType::Unknown;
      return;
    }
    const Symbol resolvedMember = substituteSymbol(member->symbol, objectType);
    const SemanticType valueType =
        analyzeInitializer(expr.value(), resolvedMember.type);
    if (resolvedMember.type == SemanticType::Function) {
      report(expr.name(), "Methods are not assignable.");
    } else if (!resolvedMember.assignable) {
      report(expr.name(), "Member is immutable.");
    } else if (!isMutableObject(expr.object())) {
      report(expr.name(), "Cannot mutate through a read-only receiver.");
    }
    if (!isOwnershipAssignable(resolvedMember.type, valueType, expr.value())) {
      report(expressionToken(expr.value()),
             "Cannot assign a value of type '" + typeSpelling(valueType) +
                 "' to member '" + expr.name().lexeme + "' of type '" +
                 typeSpelling(resolvedMember.type) + "'.",
             "GTI-S2003");
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

    if (expr.oper().kind == TokenKind::STAR) {
      if (rightType.kind != SemanticType::UniquePointer ||
          rightType.arguments.size() != 1) {
        report(expr.oper(), "Dereference requires a std::unique_ptr<T> owner.",
               "GTI-S2018");
        currentType = SemanticType::Unknown;
      } else {
        currentType = rightType.arguments[0];
      }
      return;
    }

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
      report(expr.name(), "Undefined name '" + expr.name().lexeme + "'.",
             "GTI-S2001");
      currentType = SemanticType::Unknown;
      return;
    }
    if (symbol->type == SemanticType::Function && !analyzingCallCallee) {
      report(expr.name(),
             "Function names must be called; function values are not "
             "supported yet.");
    }
    if (symbol->ownerClass != 0 &&
        (analyzingFieldInitializer || analyzingConstructorInitializer)) {
      report(expr.name(), analyzingConstructorInitializer
                              ? "Class and struct members cannot be referenced "
                                "from constructor initializer expressions."
                              : "Class and struct members cannot be referenced "
                                "from field initializers yet.");
    }
    if (isMoveOnlyOwnerType(symbol->type) &&
        symbol->ownerState != OwnerState::Available) {
      report(expr.name(),
             symbol->ownerState == OwnerState::Moved
                 ? "Move-only owner '" + expr.name().lexeme +
                       "' has already been moved."
                 : "Move-only owner '" + expr.name().lexeme +
                       "' may have been moved on another control-flow path.",
             "GTI-S2018");
    }
    currentType = symbol->type.kind == SemanticType::Reference &&
                          symbol->type.arguments.size() == 1
                      ? symbol->type.arguments[0]
                      : symbol->type;
  }

private:
  enum class OwnerState {
    Available,
    Moved,
    MaybeMoved,
  };

  struct FunctionCandidate {
    FunctionId id = 0;
    const FunctionDecl *declaration = nullptr;
    SemanticType returnType = SemanticType::Unknown;
    std::vector<SemanticType> parameterTypes;
    std::vector<GenericParameterInfo> genericParameters;
    ClassId ownerClass = 0;
    ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly;
    AccessModifier access = AccessModifier::Public;
  };

  struct Symbol {
    SemanticType type = SemanticType::Unknown;
    bool assignable = false;
    OwnerState ownerState = OwnerState::Available;
    std::vector<FunctionCandidate> overloads;
    ClassId ownerClass = 0;
    Token declaration;
  };

  using Scope = std::unordered_map<std::string, Symbol>;
  using ScopeStack = std::vector<Scope>;

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

  [[nodiscard]] static bool isMoveOnlyOwnerType(const SemanticType &type) {
    return type.kind == SemanticType::UniquePointer ||
           type.kind == SemanticType::Storage;
  }

  [[nodiscard]] static IntrinsicKind intrinsicKind(const ExprPtr &callee) {
    const auto *qualified = dynamic_cast<const QualifiedName *>(callee.get());
    if (qualified == nullptr || qualified->name().segments.size() != 2) {
      return IntrinsicKind::None;
    }
    const std::string &owner = qualified->name().segments[0].lexeme;
    const std::string &name = qualified->name().segments[1].lexeme;
    if (owner == "std") {
      if (name == "make_unique") {
        return IntrinsicKind::MakeUnique;
      }
      if (name == "move") {
        return IntrinsicKind::Move;
      }
      return IntrinsicKind::None;
    }
    if (owner == "gti_internal") {
      if (name == "allocate_storage") {
        return IntrinsicKind::AllocateStorage;
      }
      if (name == "storage_capacity") {
        return IntrinsicKind::StorageCapacity;
      }
      if (name == "storage_construct") {
        return IntrinsicKind::StorageConstruct;
      }
      if (name == "storage_read") {
        return IntrinsicKind::StorageRead;
      }
      if (name == "storage_destroy") {
        return IntrinsicKind::StorageDestroy;
      }
      if (name == "storage_relocate") {
        return IntrinsicKind::StorageRelocate;
      }
    }
    return IntrinsicKind::None;
  }

  [[nodiscard]] static const Variable *
  movedVariable(const ExprPtr &expression) {
    const Expr *candidate = expression.get();
    while (const auto *grouping = dynamic_cast<const Grouping *>(candidate)) {
      candidate = grouping->expression().get();
    }
    return dynamic_cast<const Variable *>(candidate);
  }

  void analyzeIntrinsicCall(const Call &expr, IntrinsicKind intrinsic) {
    if (intrinsic != IntrinsicKind::MakeUnique &&
        intrinsic != IntrinsicKind::Move) {
      analyzeStorageIntrinsicCall(expr, intrinsic);
      return;
    }

    if (intrinsic == IntrinsicKind::MakeUnique) {
      if (expr.typeArguments().size() != 1) {
        for (const TypeRef &argument : expr.typeArguments()) {
          validateType(argument);
        }
        for (const ExprPtr &argument : expr.arguments()) {
          analyze(argument);
        }
        report(expr.paren(),
               "std::make_unique<T> requires exactly one allocated type.",
               "GTI-S2018");
        currentType = SemanticType::Unknown;
        return;
      }

      const TypeRef &targetRef = expr.typeArguments().front();
      validateType(targetRef);
      validateReferencePlacement(targetRef, false, "allocated type");
      const SemanticType targetType = typeOf(targetRef);
      std::vector<SemanticType> argumentTypes;
      argumentTypes.reserve(expr.arguments().size());
      for (const ExprPtr &argument : expr.arguments()) {
        argumentTypes.emplace_back(analyze(argument));
      }
      if (targetType.kind != SemanticType::Class) {
        report(targetRef.name.last(),
               "std::make_unique<T> currently requires a class or struct type.",
               "GTI-S2018");
        currentType = SemanticType::Unknown;
        return;
      }

      analyzeConstructorCall(targetType.classId, targetType.arguments,
                             argumentTypes, expr.arguments(), expr.paren());
      currentType = SemanticType::uniquePointerTo(targetType);
      semanticModel.record(
          expr, ResolvedCallInfo{.returnType = currentType,
                                 .typeArguments = {targetType},
                                 .intrinsic = IntrinsicKind::MakeUnique});
      return;
    }

    for (const TypeRef &argument : expr.typeArguments()) {
      validateType(argument);
    }
    if (!expr.typeArguments().empty()) {
      report(expr.paren(), "std::move does not take type arguments.",
             "GTI-S2018");
    }
    if (expr.arguments().size() != 1) {
      for (const ExprPtr &argument : expr.arguments()) {
        analyze(argument);
      }
      report(expr.paren(), "std::move expects exactly one move-only owner.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }

    const ExprPtr &argument = expr.arguments().front();
    const SemanticType ownerType = analyze(argument);
    const Variable *variable = movedVariable(argument);
    if (!isMoveOnlyOwnerType(ownerType)) {
      report(expressionToken(argument), "std::move requires a move-only owner.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    if (variable == nullptr) {
      report(expressionToken(argument),
             ownerType.kind == SemanticType::UniquePointer
                 ? "std::move requires a named unique owner."
                 : "std::move requires a named storage owner.",
             "GTI-S2018");
      currentType = SemanticType::Unknown;
      return;
    }
    if (Symbol *symbol = resolveMutable(variable->name())) {
      symbol->ownerState = OwnerState::Moved;
    }
    currentType = ownerType;
    semanticModel.record(expr,
                         ResolvedCallInfo{.returnType = currentType,
                                          .parameterTypes = {ownerType},
                                          .intrinsic = IntrinsicKind::Move});
  }

  void analyzeStorageIntrinsicCall(const Call &expr, IntrinsicKind intrinsic) {
    std::vector<SemanticType> argumentTypes;
    argumentTypes.reserve(expr.arguments().size());
    for (const ExprPtr &argument : expr.arguments()) {
      argumentTypes.emplace_back(analyze(argument));
    }

    if (intrinsic == IntrinsicKind::AllocateStorage) {
      analyzeStorageAllocation(expr, argumentTypes);
      return;
    }

    for (const TypeRef &argument : expr.typeArguments()) {
      validateType(argument);
    }
    if (!expr.typeArguments().empty()) {
      report(expr.paren(),
             "Storage operations infer their element type from the storage "
             "argument and do not take explicit type arguments.",
             "GTI-S2019");
    }

    const std::size_t expectedArguments =
        intrinsic == IntrinsicKind::StorageCapacity   ? 1
        : intrinsic == IntrinsicKind::StorageRelocate ? 3
                                                      : 2;
    if (intrinsic == IntrinsicKind::StorageConstruct) {
      if (expr.arguments().size() != 3) {
        reportStorageArity(expr, intrinsic, 3);
      }
    } else if (expr.arguments().size() != expectedArguments) {
      reportStorageArity(expr, intrinsic, expectedArguments);
    }

    if (argumentTypes.empty() ||
        argumentTypes.front().kind != SemanticType::Storage ||
        argumentTypes.front().arguments.size() != 1) {
      if (!argumentTypes.empty() &&
          argumentTypes.front() != SemanticType::Unknown) {
        report(expressionToken(expr.arguments().front()),
               storageIntrinsicName(intrinsic) +
                   " requires gti_internal::storage<T> as its first argument.",
               "GTI-S2019");
      }
      currentType = SemanticType::Unknown;
      return;
    }

    const SemanticType storageType = argumentTypes.front();
    const SemanticType elementType = storageType.arguments.front();
    const bool mutatesFirst = intrinsic == IntrinsicKind::StorageConstruct ||
                              intrinsic == IntrinsicKind::StorageDestroy ||
                              intrinsic == IntrinsicKind::StorageRelocate;
    if (mutatesFirst) {
      requireMutableStorage(expr.arguments().front(),
                            storageIntrinsicName(intrinsic));
    }

    if (intrinsic == IntrinsicKind::StorageCapacity) {
      currentType = SemanticType::UInt64;
    } else if (intrinsic == IntrinsicKind::StorageConstruct) {
      if (argumentTypes.size() >= 2) {
        requireStorageIndex(expr.arguments()[1], argumentTypes[1], intrinsic);
      }
      if (argumentTypes.size() >= 3 &&
          argumentTypes[2] != SemanticType::Unknown &&
          argumentTypes[2] != elementType) {
        report(expressionToken(expr.arguments()[2]),
               "Storage element has type '" + typeSpelling(argumentTypes[2]) +
                   "' but this storage contains '" + typeSpelling(elementType) +
                   "'.",
               "GTI-S2019");
      }
      currentType = SemanticType::Void;
    } else if (intrinsic == IntrinsicKind::StorageRead) {
      if (argumentTypes.size() >= 2) {
        requireStorageIndex(expr.arguments()[1], argumentTypes[1], intrinsic);
      }
      if (!semanticTraits(elementType).copyable) {
        report(expr.paren(),
               "storage_read cannot copy an element of move-only type '" +
                   typeSpelling(elementType) + "'.",
               "GTI-S2019");
      }
      currentType = elementType;
    } else if (intrinsic == IntrinsicKind::StorageDestroy) {
      if (argumentTypes.size() >= 2) {
        requireStorageIndex(expr.arguments()[1], argumentTypes[1], intrinsic);
      }
      currentType = SemanticType::Void;
    } else {
      if (argumentTypes.size() >= 2 &&
          argumentTypes[1] != SemanticType::Unknown) {
        if (argumentTypes[1] != storageType) {
          report(expressionToken(expr.arguments()[1]),
                 "storage_relocate requires source and destination storage to "
                 "have the same element type.",
                 "GTI-S2019");
        } else {
          requireMutableStorage(expr.arguments()[1], "storage_relocate");
        }
      }
      if (argumentTypes.size() >= 3) {
        requireStorageIndex(expr.arguments()[2], argumentTypes[2], intrinsic);
      }
      currentType = SemanticType::Void;
    }

    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = currentType,
                               .parameterTypes = std::move(argumentTypes),
                               .typeArguments = {elementType},
                               .intrinsic = intrinsic});
  }

  void
  analyzeStorageAllocation(const Call &expr,
                           const std::vector<SemanticType> &argumentTypes) {
    if (expr.typeArguments().size() != 1) {
      for (const TypeRef &argument : expr.typeArguments()) {
        validateType(argument);
      }
      report(expr.paren(),
             "gti_internal::allocate_storage<T> requires exactly one element "
             "type.",
             "GTI-S2019");
      currentType = SemanticType::Unknown;
      return;
    }

    const TypeRef &elementRef = expr.typeArguments().front();
    validateType(elementRef);
    validateReferencePlacement(elementRef, false, "storage element type");
    const SemanticType elementType = typeOf(elementRef);
    if (!isStorageElementType(elementType)) {
      report(elementRef.name.last(),
             "Compiler-private storage requires a concrete value element type.",
             "GTI-S2019");
    }
    if (argumentTypes.size() != 1) {
      reportStorageArity(expr, IntrinsicKind::AllocateStorage, 1);
    } else {
      requireStorageIndex(expr.arguments().front(), argumentTypes.front(),
                          IntrinsicKind::AllocateStorage);
    }

    currentType = SemanticType::storageOf(elementType);
    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = currentType,
                               .parameterTypes = argumentTypes,
                               .typeArguments = {elementType},
                               .intrinsic = IntrinsicKind::AllocateStorage});
  }

  [[nodiscard]] static bool isStorageElementType(const SemanticType &type) {
    switch (type.kind) {
    case SemanticType::Unknown:
    case SemanticType::Void:
    case SemanticType::NullPtr:
    case SemanticType::Reference:
    case SemanticType::UniquePointer:
    case SemanticType::SharedPointer:
    case SemanticType::Storage:
    case SemanticType::TypeName:
    case SemanticType::Function:
    case SemanticType::Unexpected:
      return false;
    default:
      return true;
    }
  }

  void requireMutableStorage(const ExprPtr &argument,
                             std::string_view operation) {
    const ExpressionInfo *info =
        argument ? semanticModel.findExpression(*argument) : nullptr;
    if (info == nullptr || info->category != ValueCategory::Place ||
        info->access != AccessMode::Mutable) {
      report(expressionToken(argument),
             std::string(operation) + " requires mutable storage.",
             "GTI-S2019");
    }
  }

  void requireStorageIndex(const ExprPtr &argument, const SemanticType &type,
                           IntrinsicKind intrinsic) {
    if (type != SemanticType::Unknown && type != SemanticType::UInt64) {
      report(expressionToken(argument),
             storageIntrinsicName(intrinsic) +
                 " requires a uint64 index or count.",
             "GTI-S2019");
    }
  }

  void reportStorageArity(const Call &expr, IntrinsicKind intrinsic,
                          std::size_t expected) {
    report(expr.paren(),
           storageIntrinsicName(intrinsic) + " expects " +
               std::to_string(expected) + " argument" +
               (expected == 1 ? "." : "s."),
           "GTI-S2019");
  }

  [[nodiscard]] static std::string
  storageIntrinsicName(IntrinsicKind intrinsic) {
    switch (intrinsic) {
    case IntrinsicKind::AllocateStorage:
      return "allocate_storage";
    case IntrinsicKind::StorageCapacity:
      return "storage_capacity";
    case IntrinsicKind::StorageConstruct:
      return "storage_construct";
    case IntrinsicKind::StorageRead:
      return "storage_read";
    case IntrinsicKind::StorageDestroy:
      return "storage_destroy";
    case IntrinsicKind::StorageRelocate:
      return "storage_relocate";
    default:
      return "storage operation";
    }
  }

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
    if (pattern.kind == SemanticType::Reference &&
        pattern.arguments.size() == 1) {
      return inferTypeArguments(pattern.arguments[0], argument, parameters,
                                substitution, argumentToken);
    }
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
        pattern.arrayLength != argument.arrayLength ||
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

  bool applyFunctionTypeArguments(
      FunctionCandidate &function,
      const std::vector<SemanticType> &explicitTypeArguments,
      const std::vector<SemanticType> &argumentTypes, const ExprList &arguments,
      const Token &paren) {
    bool valid = true;
    if (function.genericParameters.empty()) {
      if (!explicitTypeArguments.empty()) {
        report(paren, "Non-generic functions do not take generic arguments.");
        valid = false;
      }
      return valid;
    }

    TypeSubstitution substitution;
    if (!explicitTypeArguments.empty()) {
      if (explicitTypeArguments.size() != function.genericParameters.size()) {
        report(
            paren,
            "Generic function called with the wrong number of type arguments.");
        valid = false;
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
        valid = inferTypeArguments(function.parameterTypes[index],
                                   argumentTypes[index],
                                   function.genericParameters, substitution,
                                   expressionToken(arguments[index])) &&
                valid;
      }
    }

    for (const GenericParameterInfo &parameter : function.genericParameters) {
      if (!substitution.contains(parameter.id)) {
        report(paren, "Cannot infer generic type parameter '" +
                          parameter.name.lexeme +
                          "'; provide explicit type arguments.");
        valid = false;
      }
    }
    for (const auto &[_, argument] : substitution) {
      if (isMoveOnlyOwnerType(argument)) {
        report(
            paren,
            argument.kind == SemanticType::UniquePointer
                ? "Generic functions cannot be instantiated with unique owners "
                  "until ownership-aware monomorphization is available."
                : "Generic functions cannot be instantiated with compiler-"
                  "private storage until ownership-aware monomorphization is "
                  "available.",
            "GTI-S2018");
        valid = false;
      }
    }

    function.returnType = substituteType(function.returnType, substitution);
    for (SemanticType &parameter : function.parameterTypes) {
      parameter = substituteType(parameter, substitution);
    }
    return valid;
  }

  [[nodiscard]] static bool
  tryInferTypeArguments(const SemanticType &pattern,
                        const SemanticType &argument,
                        const std::vector<GenericParameterInfo> &parameters,
                        TypeSubstitution &substitution) {
    if (pattern.kind == SemanticType::Reference &&
        pattern.arguments.size() == 1) {
      return tryInferTypeArguments(pattern.arguments[0], argument, parameters,
                                   substitution);
    }
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
      return found->second == argument;
    }

    if (pattern.kind != argument.kind || pattern.classId != argument.classId ||
        pattern.arrayLength != argument.arrayLength ||
        pattern.arguments.size() != argument.arguments.size()) {
      return true;
    }
    for (std::size_t index = 0; index < pattern.arguments.size(); ++index) {
      if (!tryInferTypeArguments(pattern.arguments[index],
                                 argument.arguments[index], parameters,
                                 substitution)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] static bool
  tryInstantiateFunction(const FunctionCandidate &candidate,
                         const std::vector<SemanticType> &explicitTypeArguments,
                         const std::vector<SemanticType> &argumentTypes,
                         FunctionCandidate &resolved,
                         std::vector<SemanticType> &resolvedTypeArguments) {
    resolved = candidate;
    resolvedTypeArguments.clear();
    if (candidate.genericParameters.empty()) {
      return explicitTypeArguments.empty();
    }

    TypeSubstitution substitution;
    if (!explicitTypeArguments.empty()) {
      if (explicitTypeArguments.size() != candidate.genericParameters.size()) {
        return false;
      }
      for (std::size_t index = 0; index < explicitTypeArguments.size();
           ++index) {
        substitution.emplace(candidate.genericParameters[index].id,
                             explicitTypeArguments[index]);
      }
    } else {
      const std::size_t count =
          std::min(argumentTypes.size(), candidate.parameterTypes.size());
      for (std::size_t index = 0; index < count; ++index) {
        if (!tryInferTypeArguments(candidate.parameterTypes[index],
                                   argumentTypes[index],
                                   candidate.genericParameters, substitution)) {
          return false;
        }
      }
    }

    for (const GenericParameterInfo &parameter : candidate.genericParameters) {
      const auto found = substitution.find(parameter.id);
      if (found == substitution.end()) {
        return false;
      }
      resolvedTypeArguments.emplace_back(found->second);
    }
    if (std::any_of(resolvedTypeArguments.begin(), resolvedTypeArguments.end(),
                    [](const SemanticType &argument) {
                      return isMoveOnlyOwnerType(argument);
                    })) {
      return false;
    }

    resolved.returnType = substituteType(resolved.returnType, substitution);
    for (SemanticType &parameter : resolved.parameterTypes) {
      parameter = substituteType(parameter, substitution);
    }
    return true;
  }

  [[nodiscard]] static std::string callableSpelling(const ExprPtr &callee) {
    if (const auto *variable = dynamic_cast<const Variable *>(callee.get())) {
      return variable->name().lexeme;
    }
    if (const auto *qualified =
            dynamic_cast<const QualifiedName *>(callee.get())) {
      return pathSpelling(qualified->name());
    }
    if (const auto *member = dynamic_cast<const Get *>(callee.get())) {
      return member->name().lexeme;
    }
    return "function";
  }

  [[nodiscard]] static std::string typeRefSpelling(const TypeRef &type) {
    std::string result = pathSpelling(type.name);
    if (!type.arguments.empty()) {
      result += '<';
      for (std::size_t index = 0; index < type.arguments.size(); ++index) {
        if (index != 0) {
          result += ", ";
        }
        result += typeRefSpelling(type.arguments[index]);
      }
      result += '>';
    }
    for (const Token &extent : type.arrayExtents) {
      result += '[' + extent.lexeme + ']';
    }
    if (type.reference) {
      result += '&';
    }
    return result;
  }

  [[nodiscard]] static std::string
  functionSignatureSpelling(const FunctionCandidate &function) {
    if (function.declaration == nullptr) {
      return "function";
    }
    const FunctionDecl &declaration = *function.declaration;
    std::string result = declaration.name().lexeme;
    if (!declaration.genericParameters().empty()) {
      result += '<';
      for (std::size_t index = 0;
           index < declaration.genericParameters().size(); ++index) {
        if (index != 0) {
          result += ", ";
        }
        result += declaration.genericParameters()[index].name.lexeme;
      }
      result += '>';
    }
    result += '(';
    for (std::size_t index = 0; index < declaration.parameters().size();
         ++index) {
      if (index != 0) {
        result += ", ";
      }
      result += typeRefSpelling(declaration.parameters()[index].type);
    }
    result += ')';
    return result;
  }

  void validateSelectedFunction(const FunctionCandidate &function,
                                const ExprPtr &callee, const Token &paren) {
    if (function.ownerClass != 0 &&
        function.access == AccessModifier::Private &&
        currentClass != function.ownerClass) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2007", DiagnosticPhase::Semantics, paren,
          "Method '" + function.declaration->name().lexeme + "' of '" +
              classInfo(function.ownerClass).name.lexeme + "' is private.");
      diagnostic.related.push_back(
          {tokenSpan(function.declaration->name()), "Method declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
    }

    if (function.receiverMutability != ReceiverMutability::Mutable) {
      return;
    }
    bool mutableReceiver =
        currentReceiverMutability == ReceiverMutability::Mutable;
    if (const auto *member = dynamic_cast<const Get *>(callee.get())) {
      mutableReceiver = isMutableObject(member->object());
    }
    if (!mutableReceiver) {
      report(paren, "Mutable method requires a mutable receiver.");
    }
  }

  void recordResolvedCall(const Call &call, const FunctionCandidate &function,
                          std::vector<SemanticType> typeArguments) {
    semanticModel.record(
        call, ResolvedCallInfo{.function = function.id,
                               .declaration = function.declaration,
                               .returnType = function.returnType,
                               .parameterTypes = function.parameterTypes,
                               .typeArguments = std::move(typeArguments)});
  }

  void reportOverloadResolutionFailure(
      const Call &call, const Symbol &overloadSet,
      const std::vector<SemanticType> &argumentTypes,
      const std::vector<const FunctionCandidate *> &exactMatches) {
    const std::string name = callableSpelling(call.callee());
    Diagnostic diagnostic;
    if (exactMatches.empty()) {
      std::string arguments;
      for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
        if (index != 0) {
          arguments += ", ";
        }
        arguments += typeSpelling(argumentTypes[index]);
      }
      diagnostic = makeDiagnostic(
          "GTI-S2012", DiagnosticPhase::Semantics, call.paren(),
          "No overload of '" + name + "' exactly matches argument types (" +
              arguments + ").");
      diagnostic.hints.emplace_back(
          "Function calls do not perform implicit conversions; convert an "
          "argument explicitly with syntax such as 'uint64(value)'.");
      for (const FunctionCandidate &candidate : overloadSet.overloads) {
        if (candidate.declaration != nullptr) {
          diagnostic.related.push_back(
              {tokenSpan(candidate.declaration->name()),
               "Candidate: " + functionSignatureSpelling(candidate)});
        }
      }
    } else {
      diagnostic =
          makeDiagnostic("GTI-S2013", DiagnosticPhase::Semantics, call.paren(),
                         "Call to '" + name + "' is ambiguous; " +
                             std::to_string(exactMatches.size()) +
                             " overloads exactly match.");
      for (const FunctionCandidate *candidate : exactMatches) {
        if (candidate != nullptr && candidate->declaration != nullptr) {
          diagnostic.related.push_back(
              {tokenSpan(candidate->declaration->name()),
               "Exact candidate: " + functionSignatureSpelling(*candidate)});
        }
      }
    }
    diagnostics.emplace_back(std::move(diagnostic));
  }

  [[nodiscard]] bool
  callArgumentMatches(const SemanticType &parameter,
                      const SemanticType &argument, const ExprPtr &expression,
                      bool allowValueAssignment = false) const {
    if (parameter == SemanticType::Unknown ||
        argument == SemanticType::Unknown) {
      return true;
    }
    if (parameter.kind != SemanticType::Reference) {
      if (isMoveOnlyOwnerType(parameter)) {
        if (parameter != argument || !expression) {
          return false;
        }
        const ExpressionInfo *info = semanticModel.findExpression(*expression);
        return info != nullptr && info->category == ValueCategory::Value;
      }
      return allowValueAssignment
                 ? isAssignable(parameter, argument, expression.get())
                 : parameter == argument;
    }
    if (parameter.arguments.size() != 1 || parameter.arguments[0] != argument ||
        !expression) {
      return false;
    }
    const ExpressionInfo *info = semanticModel.findExpression(*expression);
    if (info == nullptr || info->category != ValueCategory::Place) {
      return false;
    }
    return parameter.referenceAccess != AccessMode::Mutable ||
           info->access == AccessMode::Mutable;
  }

  void reportCallArgumentMismatch(std::size_t index,
                                  const SemanticType &parameter,
                                  const SemanticType &argument,
                                  const ExprPtr &expression,
                                  std::string_view callable) {
    const std::string argumentLabel =
        callable == "Function" ? "Argument "
                               : std::string(callable) + " argument ";
    if (parameter.kind == SemanticType::Reference &&
        parameter.arguments.size() == 1 && parameter.arguments[0] == argument &&
        expression) {
      const ExpressionInfo *info = semanticModel.findExpression(*expression);
      if (info == nullptr || info->category != ValueCategory::Place) {
        report(expressionToken(expression),
               argumentLabel + std::to_string(index + 1) +
                   " cannot bind a reference to a temporary.",
               "GTI-S2017");
        return;
      }
      if (parameter.referenceAccess == AccessMode::Mutable &&
          info->access != AccessMode::Mutable) {
        report(expressionToken(expression),
               argumentLabel + std::to_string(index + 1) +
                   " requires a mutable value for parameter '" +
                   typeSpelling(parameter) + "'.",
               "GTI-S2017");
        return;
      }
    }
    if (isMoveOnlyOwnerType(parameter) && parameter == argument) {
      report(expressionToken(expression),
             argumentLabel + std::to_string(index + 1) +
                 (parameter.kind == SemanticType::UniquePointer
                      ? " would copy a unique owner; use std::move(owner) to "
                      : " would copy compiler-private storage; use "
                        "std::move(owner) to ") +
                 "transfer ownership.",
             "GTI-S2018");
      return;
    }
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2003", DiagnosticPhase::Semantics, expressionToken(expression),
        argumentLabel + std::to_string(index + 1) + " has type '" +
            typeSpelling(argument) + "' but the parameter requires '" +
            typeSpelling(parameter) + "'.");
    if (parameter.kind != SemanticType::Reference) {
      diagnostic.hints.emplace_back(
          "Function calls require exact argument types; use an explicit "
          "conversion such as '" +
          typeSpelling(parameter) +
          "(value)' when the conversion is intentional.");
    }
    diagnostics.emplace_back(std::move(diagnostic));
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
      report(paren,
             "Constructor expects " +
                 std::to_string(constructor.parameterTypes.size()) +
                 " argument" +
                 (constructor.parameterTypes.size() == 1 ? "" : "s") +
                 " but received " + std::to_string(argumentTypes.size()) + ".",
             "GTI-S2005");
    } else {
      for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
        const SemanticType expectedType =
            substituteType(constructor.parameterTypes[index], substitution);
        if (!callArgumentMatches(expectedType, argumentTypes[index],
                                 arguments[index], true)) {
          reportCallArgumentMismatch(index, expectedType, argumentTypes[index],
                                     arguments[index], "Constructor");
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
      if (requireArity(1) && argumentTypes[0] != SemanticType::Unknown &&
          argumentTypes[0] != expected.arguments[0]) {
        report(member.name(),
               "value_or fallback must exactly match the value type.");
      }
      currentType = expected.arguments[0];
    }
  }

  void analyzeArrayMemberCall(const Get &member,
                              const std::vector<SemanticType> &argumentTypes,
                              const Token &paren) {
    if (member.name().lexeme != "size") {
      currentType = SemanticType::Unknown;
      return;
    }
    if (!argumentTypes.empty()) {
      report(paren, "Fixed array 'size' expects no arguments.", "GTI-S2016");
    }
    currentType = SemanticType::UInt64;
  }

  SemanticType analyzeArrayIndex(const ExprPtr &object, const ExprPtr &index,
                                 const Token &bracket) {
    const SemanticType objectType = analyze(object);
    const SemanticType indexType = analyze(index);
    if (objectType != SemanticType::Unknown &&
        (objectType.kind != SemanticType::Array ||
         objectType.arguments.size() != 1)) {
      report(bracket, "Indexing requires a fixed array value.", "GTI-S2016");
      return SemanticType::Unknown;
    }
    if (indexType != SemanticType::Unknown && !isInteger(indexType)) {
      report(expressionToken(index),
             "Fixed array index must have an integer type, not '" +
                 typeSpelling(indexType) + "'.",
             "GTI-S2016");
    }
    if (objectType.kind != SemanticType::Array ||
        objectType.arguments.size() != 1) {
      return SemanticType::Unknown;
    }
    if (const std::optional<IntegerConstant> constant =
            integerConstant(index.get());
        constant &&
        (constant->negative || constant->magnitude >= objectType.arrayLength)) {
      report(expressionToken(index),
             "Fixed array index is outside the valid range [0, " +
                 std::to_string(objectType.arrayLength) + ").",
             "GTI-S2016");
    }
    return objectType.arguments[0];
  }

  [[nodiscard]] bool isDefaultInitializable(const SemanticType &type) const {
    switch (type.kind) {
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64:
    case SemanticType::Float:
    case SemanticType::Bool:
    case SemanticType::String:
      return true;
    case SemanticType::Array:
      return type.arrayLength == 0 ||
             (type.arguments.size() == 1 &&
              isDefaultInitializable(type.arguments[0]));
    case SemanticType::Class: {
      const ClassInfo *owner = classInfo(type);
      if (owner == nullptr) {
        return false;
      }
      if (owner->constructor) {
        return owner->constructor->parameterTypes.empty() &&
               (owner->constructor->access == AccessModifier::Public ||
                currentClass == owner->id);
      }
      return std::all_of(owner->fields.begin(), owner->fields.end(),
                         [](const FieldInfo &field) {
                           return field.declaration != nullptr &&
                                  field.declaration->initializer() != nullptr;
                         });
    }
    default:
      return false;
    }
  }

  void validateType(const TypeRef &type) {
    for (const TypeRef &argument : type.arguments) {
      validateType(argument);
    }
    if (isStdUniquePointer(type)) {
      if (type.arguments.size() != 1) {
        report(type.name.last(),
               "std::unique_ptr<T> requires exactly one pointee type.",
               "GTI-S2018");
      } else {
        const SemanticType pointee = typeOf(type.arguments[0]);
        if (pointee == SemanticType::Void ||
            pointee.kind == SemanticType::Reference ||
            pointee.kind == SemanticType::Array) {
          report(type.arguments[0].name.last(),
                 "std::unique_ptr<T> requires a concrete non-reference object "
                 "type.",
                 "GTI-S2018");
        }
      }
      if (!type.arrayExtents.empty()) {
        report(type.name.last(),
               "Fixed arrays of unique owners are not supported yet.",
               "GTI-S2018");
      }
      return;
    }
    if (isGtiInternalStorage(type)) {
      if (type.arguments.size() != 1) {
        report(type.name.last(),
               "gti_internal::storage<T> requires exactly one element type.",
               "GTI-S2019");
      } else if (!isStorageElementType(typeOf(type.arguments.front()))) {
        report(type.arguments.front().name.last(),
               "Compiler-private storage requires a concrete value element "
               "type.",
               "GTI-S2019");
      }
      if (!type.arrayExtents.empty()) {
        report(type.name.last(),
               "Fixed arrays of compiler-private storage are not supported.",
               "GTI-S2019");
      }
      return;
    }
    if (!type.arrayExtents.empty() &&
        baseTypeOf(type, currentNamespace) == SemanticType::Void) {
      report(type.name.last(), "Fixed array elements cannot have type void.",
             "GTI-S2015");
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
      if (isMoveOnlyOwnerType(typeOf(argument))) {
        report(argument.name.last(),
               "Move-only owners cannot be nested in ordinary generic types "
               "yet.",
               "GTI-S2018");
      }
    }
  }

  [[nodiscard]] static bool isStdUniquePointer(const TypeRef &type) {
    return type.name.segments.size() == 2 &&
           type.name.segments[0].lexeme == "std" &&
           type.name.segments[1].lexeme == "unique_ptr";
  }

  [[nodiscard]] static bool isGtiInternalStorage(const TypeRef &type) {
    return type.name.segments.size() == 2 &&
           type.name.segments[0].lexeme == "gti_internal" &&
           type.name.segments[1].lexeme == "storage";
  }

  [[nodiscard]] static bool containsReference(const TypeRef &type) {
    if (type.reference) {
      return true;
    }
    return std::any_of(
        type.arguments.begin(), type.arguments.end(),
        [](const TypeRef &argument) { return containsReference(argument); });
  }

  void validateReferencePlacement(const TypeRef &type, bool allowTopLevel,
                                  std::string_view context) {
    if (type.reference) {
      if (!allowTopLevel) {
        report(*type.reference,
               "References cannot be used as a " + std::string(context) +
                   " yet.",
               "GTI-S2017");
      }
      if (!type.arrayExtents.empty()) {
        report(*type.reference,
               "References to fixed arrays are not supported yet.",
               "GTI-S2017");
      }
      if (baseTypeOf(type, currentNamespace) == SemanticType::Void) {
        report(*type.reference, "References cannot refer to void.",
               "GTI-S2017");
      }
      if (isMoveOnlyOwnerType(baseTypeOf(type, currentNamespace))) {
        const bool unique = baseTypeOf(type, currentNamespace).kind ==
                            SemanticType::UniquePointer;
        report(
            *type.reference,
            unique
                ? "References to unique owners are not supported yet; borrow "
                  "the owned value instead."
                : "References to compiler-private storage are not supported; "
                  "use storage operations instead.",
            "GTI-S2018");
      }
    }
    for (const TypeRef &argument : type.arguments) {
      if (containsReference(argument)) {
        report(argument.reference ? *argument.reference : argument.name.last(),
               "References cannot be nested inside another type yet.",
               "GTI-S2017");
      }
    }
  }

  void validateReferenceBinding(const SemanticType &reference,
                                const SemanticType &initializerType,
                                const ExprPtr &initializer) {
    if (reference.arguments.size() != 1) {
      return;
    }
    const SemanticType &referent = reference.arguments[0];
    if (initializerType != SemanticType::Unknown &&
        initializerType != referent) {
      report(expressionToken(initializer),
             "Reference requires an exact '" + typeSpelling(referent) +
                 "' initializer, but received '" +
                 typeSpelling(initializerType) + "'.",
             "GTI-S2017");
      return;
    }
    const ExpressionInfo *info =
        initializer ? semanticModel.findExpression(*initializer) : nullptr;
    if (info == nullptr || info->category != ValueCategory::Place) {
      report(expressionToken(initializer),
             "Reference initializer must be an addressable value, not a "
             "temporary.",
             "GTI-S2017");
      return;
    }
    if (!hasStableBorrowStorage(initializer)) {
      report(expressionToken(initializer),
             "Reference initializer is derived from temporary storage that "
             "does not outlive the binding.",
             "GTI-S2017");
      return;
    }
    if (reference.referenceAccess == AccessMode::Mutable &&
        info->access != AccessMode::Mutable) {
      report(expressionToken(initializer),
             "A mutable reference requires a mutable initializer.",
             "GTI-S2017");
    }
  }

  [[nodiscard]] bool hasStableBorrowStorage(const ExprPtr &expression) const {
    if (!expression) {
      return false;
    }
    if (dynamic_cast<const Variable *>(expression.get()) != nullptr ||
        dynamic_cast<const QualifiedName *>(expression.get()) != nullptr ||
        dynamic_cast<const Self *>(expression.get()) != nullptr) {
      return true;
    }
    if (const auto *grouping =
            dynamic_cast<const Grouping *>(expression.get())) {
      return hasStableBorrowStorage(grouping->expression());
    }
    if (const auto *binary = dynamic_cast<const Binary *>(expression.get());
        binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
      return hasStableBorrowStorage(binary->right());
    }
    if (const auto *index = dynamic_cast<const Index *>(expression.get())) {
      return hasStableBorrowStorage(index->object());
    }
    if (const auto *member = dynamic_cast<const Get *>(expression.get())) {
      return hasStableBorrowStorage(member->object());
    }
    if (const auto *unary = dynamic_cast<const Unary *>(expression.get());
        unary != nullptr && unary->oper().kind == TokenKind::STAR) {
      const ExpressionInfo *owner =
          semanticModel.findExpression(*unary->right());
      return owner != nullptr && owner->category == ValueCategory::Place;
    }
    return false;
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
    for (FunctionCandidate &overload : result.overloads) {
      overload.returnType = substituteType(overload.returnType, substitution);
      for (SemanticType &parameter : overload.parameterTypes) {
        parameter = substituteType(parameter, substitution);
      }
    }
    return result;
  }

  Symbol functionSymbol(const FunctionDecl &function,
                        const std::vector<std::string> &scope) {
    const std::vector<GenericParameterInfo> &genericParameters =
        genericParametersFor(function);
    beginTypeParameterScope(genericParameters);
    FunctionCandidate candidate{
        .declaration = &function,
        .returnType = typeOf(function.returnType(), scope),
        .genericParameters = genericParameters,
        .receiverMutability = function.receiverMutability()};
    if (const FunctionInfo *info = semanticModel.findFunction(function)) {
      candidate.id = info->id;
    }
    candidate.parameterTypes.reserve(function.parameters().size());
    for (const Parameter &parameter : function.parameters()) {
      candidate.parameterTypes.emplace_back(typeOf(parameter, scope));
    }
    Symbol symbol{.type = SemanticType::Function,
                  .assignable = false,
                  .overloads = {std::move(candidate)},
                  .declaration = function.name()};
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
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2006", DiagnosticPhase::Semantics, classDecl->name(),
              "Duplicate declaration of '" + classDecl->name().lexeme + "'.");
          if (const auto existing = classIds.find(qualified);
              existing != classIds.end()) {
            diagnostic.related.push_back(
                {tokenSpan(classInfo(existing->second).name),
                 "Previous declaration is here."});
          }
          diagnostics.emplace_back(std::move(diagnostic));
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

  void registerFunctionGenericParameters(const StmtList &statements,
                                         std::vector<std::string> scope,
                                         bool classMember) {
    for (const StmtPtr &statement : statements) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          registerFunctionGenericParameters(*branch, scope, classMember);
        }
      } else if (const auto *function =
                     dynamic_cast<const FunctionDecl *>(statement.get())) {
        functionGenericParameters.emplace(
            function, makeGenericParameters(function->genericParameters(),
                                            function->name()));
        semanticModel.record(
            *function,
            FunctionInfo{.id = nextFunctionId++,
                         .declaration = function,
                         .qualifiedName =
                             qualifiedName(scope, function->name().lexeme),
                         .entryPoint = !classMember && scope.empty() &&
                                       function->name().lexeme == "main"});
      } else if (const auto *classDecl =
                     dynamic_cast<const ClassDecl *>(statement.get())) {
        std::vector<std::string> memberScope = scope;
        memberScope.emplace_back(classDecl->name().lexeme);
        registerFunctionGenericParameters(classDecl->members(),
                                          std::move(memberScope), true);
      } else if (const auto *namespaceDecl =
                     dynamic_cast<const NamespaceDecl *>(statement.get())) {
        scope.emplace_back(namespaceDecl->name().lexeme);
        registerFunctionGenericParameters(namespaceDecl->declarations(), scope,
                                          false);
        scope.pop_back();
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
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2006", DiagnosticPhase::Semantics, constructor->name(),
              "A class or struct cannot declare more than one constructor "
              "yet.");
          diagnostic.related.push_back(
              {tokenSpan(owner.constructor->declaration->name()),
               "Previous constructor is here."});
          diagnostics.emplace_back(std::move(diagnostic));
          continue;
        }
        ConstructorInfo info{.declaration = constructor, .access = access};
        info.parameterTypes.reserve(constructor->parameters().size());
        for (const Parameter &parameter : constructor->parameters()) {
          info.parameterTypes.emplace_back(
              typeOf(parameter, owner.namespaceScope));
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
                        .assignable = variable->isMutable(),
                        .declaration = variable->name()};
        predeclaredVariables.insert(variable);
      }
      if (name == nullptr) {
        continue;
      }

      symbol.ownerClass = owner.id;
      for (FunctionCandidate &overload : symbol.overloads) {
        overload.ownerClass = owner.id;
        overload.access = access;
      }
      const auto existing = owner.members.find(name->lexeme);
      if (existing != owner.members.end()) {
        if (appendFunctionOverload(existing->second.symbol, std::move(symbol),
                                   *name)) {
          continue;
        }
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2006", DiagnosticPhase::Semantics, *name,
            "Duplicate member declaration of '" + name->lexeme + "'.");
        diagnostic.related.push_back(
            {tokenSpan(existing->second.symbol.declaration),
             "Previous member declaration is here."});
        diagnostics.emplace_back(std::move(diagnostic));
        continue;
      }

      owner.members.emplace(
          name->lexeme,
          MemberInfo{.symbol = std::move(symbol), .access = access});
      if (field != nullptr) {
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

  [[nodiscard]] ExpressionInfo classifyExpression(const Expr &expr,
                                                  SemanticType type) const {
    const auto preserveCategory = [&](const Expr &source) {
      const ExpressionInfo *sourceInfo = semanticModel.findExpression(source);
      return sourceInfo == nullptr
                 ? makeExpressionInfo(std::move(type))
                 : makeExpressionInfo(std::move(type), sourceInfo->category,
                                      sourceInfo->access);
    };

    if (const auto *grouping = dynamic_cast<const Grouping *>(&expr)) {
      return preserveCategory(*grouping->expression());
    }
    if (const auto *binary = dynamic_cast<const Binary *>(&expr);
        binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
      return preserveCategory(*binary->right());
    }
    if (const auto *variable = dynamic_cast<const Variable *>(&expr)) {
      const Symbol *symbol = resolve(variable->name());
      if (symbol != nullptr && (symbol->type == SemanticType::Function ||
                                symbol->type == SemanticType::TypeName)) {
        return makeExpressionInfo(std::move(type));
      }
      const bool mutableAccess =
          symbol == nullptr ||
          (symbol->type.kind == SemanticType::Reference
               ? symbol->type.referenceAccess == AccessMode::Mutable
               : symbol->assignable && (symbol->ownerClass == 0 ||
                                        currentReceiverMutability ==
                                            ReceiverMutability::Mutable));
      return makeExpressionInfo(std::move(type), ValueCategory::Place,
                                mutableAccess ? AccessMode::Mutable
                                              : AccessMode::ReadOnly);
    }
    if (const auto *qualified = dynamic_cast<const QualifiedName *>(&expr)) {
      const Symbol *symbol = resolveQualified(qualified->name());
      if (symbol != nullptr && (symbol->type == SemanticType::Function ||
                                symbol->type == SemanticType::TypeName)) {
        return makeExpressionInfo(std::move(type));
      }
      return makeExpressionInfo(std::move(type), ValueCategory::Place,
                                symbol != nullptr && symbol->assignable
                                    ? AccessMode::Mutable
                                    : AccessMode::ReadOnly);
    }
    if (dynamic_cast<const Self *>(&expr) != nullptr) {
      return makeExpressionInfo(std::move(type), ValueCategory::Place,
                                currentReceiverMutability ==
                                        ReceiverMutability::Mutable
                                    ? AccessMode::Mutable
                                    : AccessMode::ReadOnly);
    }
    if (const auto *index = dynamic_cast<const Index *>(&expr)) {
      const ExpressionInfo *objectInfo =
          semanticModel.findExpression(*index->object());
      return makeExpressionInfo(
          std::move(type), ValueCategory::Place,
          objectInfo != nullptr &&
                  objectInfo->category == ValueCategory::Place &&
                  objectInfo->access == AccessMode::Mutable
              ? AccessMode::Mutable
              : AccessMode::ReadOnly);
    }
    if (const auto *unary = dynamic_cast<const Unary *>(&expr);
        unary != nullptr && unary->oper().kind == TokenKind::STAR) {
      const ExpressionInfo *ownerInfo =
          semanticModel.findExpression(*unary->right());
      return makeExpressionInfo(std::move(type), ValueCategory::Place,
                                ownerInfo != nullptr &&
                                        ownerInfo->access == AccessMode::Mutable
                                    ? AccessMode::Mutable
                                    : AccessMode::ReadOnly);
    }
    if (const auto *get = dynamic_cast<const Get *>(&expr)) {
      if (type == SemanticType::Function) {
        return makeExpressionInfo(std::move(type));
      }
      const SemanticType *objectType = semanticModel.findType(*get->object());
      SemanticType memberObjectType =
          objectType == nullptr ? SemanticType::Unknown : *objectType;
      if (get->access().kind == TokenKind::ARROW &&
          memberObjectType.kind == SemanticType::UniquePointer &&
          memberObjectType.arguments.size() == 1) {
        memberObjectType = memberObjectType.arguments[0];
      }
      const MemberInfo *member = findMember(memberObjectType, get->name());
      const ExpressionInfo *objectInfo =
          semanticModel.findExpression(*get->object());
      const bool mutableAccess =
          member == nullptr ||
          (member->symbol.assignable && objectInfo != nullptr &&
           objectInfo->category == ValueCategory::Place &&
           objectInfo->access == AccessMode::Mutable);
      return makeExpressionInfo(std::move(type), ValueCategory::Place,
                                mutableAccess ? AccessMode::Mutable
                                              : AccessMode::ReadOnly);
    }
    return makeExpressionInfo(std::move(type));
  }

  SemanticType analyze(const Expr &expr) {
    expr.accept(*this);
    SemanticType result = currentType;
    semanticModel.record(expr, classifyExpression(expr, result));
    currentType = result;
    return result;
  }

  SemanticType analyze(const ExprPtr &expr) {
    return expr ? analyze(*expr) : SemanticType::Unknown;
  }

  SemanticType analyzeInitializer(const ExprPtr &expr,
                                  const SemanticType &expectedType) {
    const std::optional<SemanticType> enclosingType = contextualInitializerType;
    contextualInitializerType = expectedType;
    const SemanticType result = analyze(expr);
    contextualInitializerType = enclosingType;
    return result;
  }

  [[nodiscard]] static std::optional<std::size_t>
  genericParameterIndex(const FunctionCandidate &function,
                        GenericParameterId id) {
    for (std::size_t index = 0; index < function.genericParameters.size();
         ++index) {
      if (function.genericParameters[index].id == id) {
        return index;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] static bool
  sameSignatureType(const SemanticType &left, const FunctionCandidate &leftFn,
                    const SemanticType &right,
                    const FunctionCandidate &rightFn) {
    const std::optional<std::size_t> leftParameter =
        left.kind == SemanticType::TypeParameter
            ? genericParameterIndex(leftFn, left.genericParameterId)
            : std::nullopt;
    const std::optional<std::size_t> rightParameter =
        right.kind == SemanticType::TypeParameter
            ? genericParameterIndex(rightFn, right.genericParameterId)
            : std::nullopt;
    if (leftParameter || rightParameter) {
      return leftParameter && rightParameter &&
             *leftParameter == *rightParameter;
    }
    if (left.kind != right.kind || left.classId != right.classId ||
        left.genericParameterId != right.genericParameterId ||
        left.arrayLength != right.arrayLength ||
        left.referenceAccess != right.referenceAccess ||
        left.arguments.size() != right.arguments.size()) {
      return false;
    }
    for (std::size_t index = 0; index < left.arguments.size(); ++index) {
      if (!sameSignatureType(left.arguments[index], leftFn,
                             right.arguments[index], rightFn)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] static bool
  sameFunctionSignature(const FunctionCandidate &left,
                        const FunctionCandidate &right) {
    if (left.genericParameters.size() != right.genericParameters.size() ||
        left.parameterTypes.size() != right.parameterTypes.size()) {
      return false;
    }
    for (std::size_t index = 0; index < left.parameterTypes.size(); ++index) {
      if (!sameSignatureType(left.parameterTypes[index], left,
                             right.parameterTypes[index], right)) {
        return false;
      }
    }
    return true;
  }

  bool appendFunctionOverload(Symbol &existing, Symbol incoming,
                              const Token &name) {
    if (existing.type != SemanticType::Function ||
        incoming.type != SemanticType::Function ||
        incoming.overloads.size() != 1) {
      return false;
    }

    FunctionCandidate candidate = std::move(incoming.overloads.front());
    const FunctionInfo *candidateInfo =
        candidate.declaration == nullptr
            ? nullptr
            : semanticModel.findFunction(*candidate.declaration);
    if (candidateInfo != nullptr && candidateInfo->entryPoint) {
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2011", DiagnosticPhase::Semantics, name,
                         "The main entry point cannot be overloaded.");
      diagnostic.related.push_back(
          {tokenSpan(existing.overloads.front().declaration->name()),
           "Previous main declaration is here."});
      diagnostics.emplace_back(std::move(diagnostic));
      return true;
    }

    for (const FunctionCandidate &previous : existing.overloads) {
      if ((previous.declaration != nullptr &&
           previous.declaration->runtimeBinding()) ||
          (candidate.declaration != nullptr &&
           candidate.declaration->runtimeBinding())) {
        Diagnostic diagnostic =
            makeDiagnostic("GTI-S2011", DiagnosticPhase::Semantics, name,
                           "Runtime-bound functions cannot be overloaded.");
        diagnostic.related.push_back(
            {tokenSpan(previous.declaration->name()),
             "Previous function declaration is here."});
        diagnostics.emplace_back(std::move(diagnostic));
        return true;
      }
      if (sameFunctionSignature(previous, candidate)) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2011", DiagnosticPhase::Semantics, name,
            "Duplicate overload signature for '" + name.lexeme + "'.");
        diagnostic.related.push_back(
            {tokenSpan(previous.declaration->name()),
             "Previous overload with this parameter signature is here."});
        diagnostics.emplace_back(std::move(diagnostic));
        return true;
      }
    }

    existing.overloads.emplace_back(std::move(candidate));
    return true;
  }

  bool declare(const Token &name, SemanticType type, bool assignable) {
    return declare(
        name,
        Symbol{.type = type, .assignable = assignable, .declaration = name});
  }

  bool declare(const Token &name, Symbol symbol) {
    const auto existing = scopes.back().find(name.lexeme);
    if (existing != scopes.back().end()) {
      if (appendFunctionOverload(existing->second, std::move(symbol), name)) {
        return true;
      }
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2006", DiagnosticPhase::Semantics, name,
                         "Duplicate declaration of '" + name.lexeme + "'.");
      diagnostic.related.push_back({tokenSpan(existing->second.declaration),
                                    "Previous declaration is here."});
      diagnostics.emplace_back(std::move(diagnostic));
      return false;
    }
    scopes.back().emplace(name.lexeme, std::move(symbol));
    return true;
  }

  bool declareNamespaceSymbol(const std::vector<std::string> &scope,
                              const Token &name, SemanticType type,
                              bool assignable) {
    return declareNamespaceSymbol(
        scope, name,
        Symbol{.type = type, .assignable = assignable, .declaration = name});
  }

  bool declareNamespaceSymbol(const std::vector<std::string> &scope,
                              const Token &name, Symbol symbol) {
    const std::string qualified = qualifiedName(scope, name.lexeme);
    if (namespaces.contains(qualified) ||
        namespaceAliases.contains(qualified)) {
      report(name, "Duplicate declaration of '" + name.lexeme + "'.");
      return false;
    }

    const auto existing = namespaceSymbols.find(qualified);
    if (existing != namespaceSymbols.end()) {
      if (appendFunctionOverload(existing->second, std::move(symbol), name)) {
        return true;
      }
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2006", DiagnosticPhase::Semantics, name,
                         "Duplicate declaration of '" + name.lexeme + "'.");
      diagnostic.related.push_back({tokenSpan(existing->second.declaration),
                                    "Previous declaration is here."});
      diagnostics.emplace_back(std::move(diagnostic));
      return false;
    }
    namespaceSymbols.emplace(qualified, std::move(symbol));
    return true;
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

  [[nodiscard]] Symbol *resolveMutable(const Token &name) {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      if (const auto found = scope->find(name.lexeme); found != scope->end()) {
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
      const SemanticType *objectType = semanticModel.findType(*get->object());
      if (objectType != nullptr) {
        SemanticType memberObjectType = *objectType;
        if (get->access().kind == TokenKind::ARROW &&
            memberObjectType.kind == SemanticType::UniquePointer &&
            memberObjectType.arguments.size() == 1) {
          memberObjectType = memberObjectType.arguments[0];
        }
        if (const MemberInfo *member =
                findMember(memberObjectType, get->name())) {
          return substituteSymbol(member->symbol, memberObjectType);
        }
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] bool isMutableTarget(const ExprPtr &expression) const {
    if (expression) {
      if (const ExpressionInfo *info =
              semanticModel.findExpression(*expression)) {
        return info->category == ValueCategory::Place &&
               info->access == AccessMode::Mutable;
      }
    }
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
      const SemanticType *objectType = semanticModel.findType(*get->object());
      if (objectType == nullptr) {
        return true;
      }
      SemanticType memberObjectType = *objectType;
      if (get->access().kind == TokenKind::ARROW &&
          memberObjectType.kind == SemanticType::UniquePointer &&
          memberObjectType.arguments.size() == 1) {
        memberObjectType = memberObjectType.arguments[0];
      }
      const MemberInfo *member = findMember(memberObjectType, get->name());
      return member == nullptr ||
             (member->symbol.assignable && isMutableObject(get->object()));
    }
    return false;
  }

  [[nodiscard]] bool isMutableObject(const ExprPtr &expression) const {
    if (expression) {
      if (const ExpressionInfo *info =
              semanticModel.findExpression(*expression)) {
        return info->category == ValueCategory::Place &&
               info->access == AccessMode::Mutable;
      }
    }
    return isMutableTarget(expression);
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
    if (member->symbol.type != SemanticType::Function &&
        member->access == AccessModifier::Private &&
        currentClass != owner->id) {
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2007", DiagnosticPhase::Semantics, name,
                         "Member '" + name.lexeme + "' of '" +
                             owner->name.lexeme + "' is private.");
      diagnostic.related.push_back(
          {tokenSpan(member->symbol.declaration), "Member declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
    }
    return member;
  }

  [[nodiscard]] std::string typeSpelling(const SemanticType &type) const {
    switch (type.kind) {
    case SemanticType::Unknown:
      return "unknown";
    case SemanticType::Void:
      return "void";
    case SemanticType::Int8:
      return "int8";
    case SemanticType::Int16:
      return "int16";
    case SemanticType::Int32:
      return "int32";
    case SemanticType::Int64:
      return "int64";
    case SemanticType::UInt8:
      return "uint8";
    case SemanticType::UInt16:
      return "uint16";
    case SemanticType::UInt32:
      return "uint32";
    case SemanticType::UInt64:
      return "uint64";
    case SemanticType::Float:
      return "float";
    case SemanticType::Bool:
      return "bool";
    case SemanticType::String:
      return "string";
    case SemanticType::NullPtr:
      return "nullptr";
    case SemanticType::Array:
      if (type.arguments.size() == 1) {
        return typeSpelling(type.arguments[0]) + "[" +
               std::to_string(type.arrayLength) + "]";
      }
      return "array";
    case SemanticType::Class: {
      const ClassInfo *owner = classInfo(type);
      if (owner == nullptr) {
        return "unknown class";
      }
      std::string result =
          qualifiedName(owner->namespaceScope, owner->name.lexeme);
      if (!type.arguments.empty()) {
        result += '<';
        for (std::size_t index = 0; index < type.arguments.size(); ++index) {
          if (index != 0) {
            result += ", ";
          }
          result += typeSpelling(type.arguments[index]);
        }
        result += '>';
      }
      return result;
    }
    case SemanticType::Reference:
      if (type.arguments.size() == 1) {
        return (type.referenceAccess == AccessMode::Mutable ? "mut " : "") +
               typeSpelling(type.arguments[0]) + "&";
      }
      return "reference";
    case SemanticType::UniquePointer:
      if (type.arguments.size() == 1) {
        return "std::unique_ptr<" + typeSpelling(type.arguments[0]) + ">";
      }
      return "std::unique_ptr";
    case SemanticType::SharedPointer:
      if (type.arguments.size() == 1) {
        return "std::shared_ptr<" + typeSpelling(type.arguments[0]) + ">";
      }
      return "std::shared_ptr";
    case SemanticType::Storage:
      if (type.arguments.size() == 1) {
        return "gti_internal::storage<" + typeSpelling(type.arguments[0]) + ">";
      }
      return "gti_internal::storage";
    case SemanticType::TypeParameter:
      for (auto scope = typeParameterScopes.rbegin();
           scope != typeParameterScopes.rend(); ++scope) {
        for (const auto &[name, candidate] : *scope) {
          if (candidate.kind == SemanticType::TypeParameter &&
              candidate.genericParameterId == type.genericParameterId) {
            return name;
          }
        }
      }
      return "type parameter";
    case SemanticType::TypeName:
      if (type.classId != 0 && type.classId <= classes.size()) {
        const ClassInfo &owner = classInfo(type.classId);
        return qualifiedName(owner.namespaceScope, owner.name.lexeme);
      }
      return "type";
    case SemanticType::Function:
      return "function";
    case SemanticType::Expected:
      if (type.arguments.size() == 2) {
        return "expected<" + typeSpelling(type.arguments[0]) + ", " +
               typeSpelling(type.arguments[1]) + ">";
      }
      return "expected";
    case SemanticType::Unexpected:
      if (type.arguments.size() == 1) {
        return "unexpected<" + typeSpelling(type.arguments[0]) + ">";
      }
      return "unexpected";
    }
    return "unknown";
  }

  [[nodiscard]] static OwnerState mergedOwnerState(OwnerState left,
                                                   OwnerState right) {
    return left == right ? left : OwnerState::MaybeMoved;
  }

  void mergeOwnerStates(const ScopeStack &base, const ScopeStack &left,
                        const ScopeStack &right) {
    const std::size_t depth =
        std::min({scopes.size(), base.size(), left.size(), right.size()});
    for (std::size_t scopeIndex = 0; scopeIndex < depth; ++scopeIndex) {
      for (const auto &[name, baseSymbol] : base[scopeIndex]) {
        if (!isMoveOnlyOwnerType(baseSymbol.type)) {
          continue;
        }
        const auto leftSymbol = left[scopeIndex].find(name);
        const auto rightSymbol = right[scopeIndex].find(name);
        const auto target = scopes[scopeIndex].find(name);
        if (leftSymbol != left[scopeIndex].end() &&
            rightSymbol != right[scopeIndex].end() &&
            target != scopes[scopeIndex].end()) {
          target->second.ownerState = mergedOwnerState(
              leftSymbol->second.ownerState, rightSymbol->second.ownerState);
        }
      }
    }
  }

  void beginScope() { scopes.emplace_back(); }
  void endScope() { scopes.pop_back(); }

  void report(const Token &token, std::string message,
              std::string code = "GTI-S2000") {
    diagnostics.push_back(makeDiagnostic(std::move(code),
                                         DiagnosticPhase::Semantics, token,
                                         std::move(message)));
  }

  void requireBool(SemanticType type, const Token &token,
                   std::string_view message) {
    if (type != SemanticType::Unknown && !isContextuallyBool(type)) {
      report(token,
             std::string(message) + " Found '" + typeSpelling(type) + "'.",
             "GTI-S2003");
    }
  }

  void requireNumeric(SemanticType left, SemanticType right,
                      const Token &token) {
    if ((left != SemanticType::Unknown && !isNumeric(left)) ||
        (right != SemanticType::Unknown && !isNumeric(right))) {
      report(token,
             "Operator '" + token.lexeme +
                 "' requires numeric operands but found '" +
                 typeSpelling(left) + "' and '" + typeSpelling(right) + "'.",
             "GTI-S2004");
    }
  }

  void requireInteger(SemanticType left, SemanticType right,
                      const Token &token) {
    if ((left != SemanticType::Unknown && !isInteger(left)) ||
        (right != SemanticType::Unknown && !isInteger(right))) {
      report(token,
             "Operator '" + token.lexeme +
                 "' requires integer operands but found '" +
                 typeSpelling(left) + "' and '" + typeSpelling(right) + "'.",
             "GTI-S2004");
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
    return type == SemanticType::Bool || type.kind == SemanticType::Expected ||
           type.kind == SemanticType::UniquePointer;
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

  [[nodiscard]] bool isOwnershipAssignable(const SemanticType &target,
                                           const SemanticType &value,
                                           const ExprPtr &expression) const {
    if (!isMoveOnlyOwnerType(target)) {
      return isAssignable(target, value, expression.get());
    }
    if (target.kind == SemanticType::UniquePointer &&
        value == SemanticType::NullPtr) {
      return true;
    }
    if (target != value || !expression) {
      return target == SemanticType::Unknown || value == SemanticType::Unknown;
    }
    const ExpressionInfo *info = semanticModel.findExpression(*expression);
    return info != nullptr && info->category == ValueCategory::Value;
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
    if ((left.kind == SemanticType::UniquePointer &&
         (right == left || right == SemanticType::NullPtr)) ||
        (right.kind == SemanticType::UniquePointer &&
         left == SemanticType::NullPtr)) {
      return true;
    }
    if (isInteger(left) && isInteger(right)) {
      return numericResult(left, right, leftExpression, rightExpression) !=
             SemanticType::Unknown;
    }
    return isAssignable(left, right, rightExpression) ||
           isAssignable(right, left, leftExpression);
  }

  [[nodiscard]] SemanticType
  baseTypeOf(const TypeRef &type,
             const std::vector<std::string> &fromScope) const {
    if (isStdUniquePointer(type)) {
      return type.arguments.size() == 1 ? SemanticType::uniquePointerTo(typeOf(
                                              type.arguments[0], fromScope))
                                        : SemanticType::Unknown;
    }
    if (isGtiInternalStorage(type)) {
      return type.arguments.size() == 1
                 ? SemanticType::storageOf(typeOf(type.arguments[0], fromScope))
                 : SemanticType::Unknown;
    }
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

  [[nodiscard]] SemanticType
  typeOf(const TypeRef &type, const std::vector<std::string> &fromScope) const {
    SemanticType result = baseTypeOf(type, fromScope);
    for (auto extent = type.arrayExtents.rbegin();
         extent != type.arrayExtents.rend(); ++extent) {
      const auto *length = std::get_if<std::uint64_t>(&extent->literal);
      if (length == nullptr) {
        return SemanticType::Unknown;
      }
      result = SemanticType::arrayOf(std::move(result), *length);
    }
    if (type.reference) {
      result = SemanticType::referenceTo(std::move(result));
    }
    return result;
  }

  [[nodiscard]] SemanticType typeOf(const TypeRef &type) const {
    return typeOf(type, currentNamespace);
  }

  [[nodiscard]] SemanticType
  typeOf(const TypeRef &type, Mutability mutability,
         const std::vector<std::string> &fromScope) const {
    SemanticType result = typeOf(type, fromScope);
    if (result.kind == SemanticType::Reference) {
      result.referenceAccess = mutability == Mutability::Mutable
                                   ? AccessMode::Mutable
                                   : AccessMode::ReadOnly;
    }
    return result;
  }

  [[nodiscard]] SemanticType typeOf(const TypeRef &type,
                                    Mutability mutability) const {
    return typeOf(type, mutability, currentNamespace);
  }

  [[nodiscard]] SemanticType
  typeOf(const Parameter &parameter,
         const std::vector<std::string> &fromScope) const {
    return typeOf(parameter.type, parameter.mutability, fromScope);
  }

  [[nodiscard]] SemanticType typeOf(const Parameter &parameter) const {
    return typeOf(parameter, currentNamespace);
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
    if (const auto *initializer =
            dynamic_cast<const ArrayInitializer *>(expr.get())) {
      return initializer->brace();
    }
    if (const auto *call = dynamic_cast<const Call *>(expr.get())) {
      return call->paren();
    }
    if (const auto *conversion = dynamic_cast<const Conversion *>(expr.get())) {
      return conversion->targetType().name.last();
    }
    if (const auto *get = dynamic_cast<const Get *>(expr.get())) {
      return get->name();
    }
    if (const auto *index = dynamic_cast<const Index *>(expr.get())) {
      return index->bracket();
    }
    if (const auto *indexSet = dynamic_cast<const IndexSet *>(expr.get())) {
      return indexSet->bracket();
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
  ScopeStack scopes;
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
  SemanticModel semanticModel;
  std::vector<std::string> currentNamespace;
  std::unordered_set<const VariableDecl *> predeclaredVariables;
  TargetInfo target;
  SemanticType currentType = SemanticType::Unknown;
  SemanticType currentReturnType = SemanticType::Unknown;
  std::optional<ClassId> currentClass;
  bool analyzingFieldInitializer = false;
  bool analyzingConstructorInitializer = false;
  bool analyzingCallCallee = false;
  std::optional<SemanticType> contextualInitializerType;
  ReceiverMutability currentReceiverMutability = ReceiverMutability::ReadOnly;
  std::size_t constructorDepth = 0;
  std::size_t functionDepth = 0;
  std::size_t loopDepth = 0;
  GenericParameterId nextGenericParameterId = 1;
  FunctionId nextFunctionId = 1;
};

} // namespace lang
