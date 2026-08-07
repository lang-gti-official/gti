#pragma once

#include "gti/ast.h"
#include "gti/diagnostic.h"
#include "gti/semantic_analyzer.h"
#include "gti/target.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lang {

using HirValueId = std::size_t;
using HirBindingId = std::size_t;
using HirStatementId = std::size_t;
using HirClassInstanceId = std::size_t;
using HirFunctionInstanceId = std::size_t;
using HirConstructorInstanceId = std::size_t;
using HirDestructorInstanceId = std::size_t;

enum class HirValueKind {
  Assignment,
  ArrayInitializer,
  Binary,
  Call,
  Conversion,
  DereferenceSet,
  MemberAccess,
  Grouping,
  Index,
  IndexSet,
  Literal,
  Logical,
  PackExpansion,
  Postfix,
  QualifiedName,
  Self,
  MemberSet,
  Unary,
  Unexpected,
  Variable,
};

enum class HirStatementKind {
  Block,
  CompileTimeBranch,
  Empty,
  Expression,
  For,
  If,
  Break,
  Continue,
  Return,
  Variable,
  While,
};

struct HirBinding {
  HirBindingId id = 0;
  const VariableDecl *variable = nullptr;
  const Parameter *parameter = nullptr;
  BindingInfo info;
};

struct HirValue {
  HirValueId id = 0;
  HirValueKind kind = HirValueKind::Literal;
  const Expr *source = nullptr;
  ExpressionInfo info;
  std::vector<HirValueId> operands;
  std::optional<TokenKind> operation;
  std::optional<Literal> literal;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  std::optional<HirFunctionInstanceId> functionTarget;
  std::optional<HirConstructorInstanceId> constructorTarget;
};

struct HirStatement {
  HirStatementId id = 0;
  HirStatementKind kind = HirStatementKind::Empty;
  const Stmt *source = nullptr;
  std::optional<HirBindingId> binding;
  std::optional<HirValueId> value;
  std::optional<HirValueId> condition;
  std::optional<HirValueId> increment;
  std::optional<HirStatementId> initializer;
  std::optional<HirStatementId> body;
  std::optional<HirStatementId> elseBranch;
  std::vector<HirStatementId> statements;
};

struct HirBody {
  std::vector<HirBinding> bindings;
  std::vector<HirValue> values;
  std::vector<HirStatement> statements;
  std::vector<HirStatementId> roots;

  [[nodiscard]] const HirValue *findValue(HirValueId id) const {
    const auto found =
        std::find_if(values.begin(), values.end(),
                     [id](const HirValue &value) { return value.id == id; });
    return found == values.end() ? nullptr : &*found;
  }

  [[nodiscard]] const HirStatement *findStatement(HirStatementId id) const {
    const auto found = std::find_if(
        statements.begin(), statements.end(),
        [id](const HirStatement &statement) { return statement.id == id; });
    return found == statements.end() ? nullptr : &*found;
  }
};

struct HirClassField {
  const VariableDecl *declaration = nullptr;
  HirBindingId binding = 0;
  std::optional<HirValueId> initializer;
  BindingInfo info;
};

struct HirClassInstance {
  HirClassInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  ClassId declaration = 0;
  const ClassDecl *source = nullptr;
  std::vector<SemanticType> typeArguments;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits;
  std::vector<HirClassField> fields;
  HirBody fieldInitializers;
  std::optional<HirDestructorInstanceId> destructor;
};

struct HirFunctionInstance {
  HirFunctionInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  FunctionId declaration = 0;
  const FunctionDecl *source = nullptr;
  std::optional<HirClassInstanceId> owner;
  std::vector<SemanticType> typeArguments;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  HirBody body;
  std::optional<SourceSpan> instantiationSite;
};

struct HirConstructorInstance {
  HirConstructorInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  ConstructorId declaration = 0;
  const ConstructorDecl *source = nullptr;
  HirClassInstanceId owner = 0;
  std::vector<SemanticType> parameterTypes;
  std::vector<HirValueId> initializerValues;
  HirBody body;
  std::optional<SourceSpan> instantiationSite;
};

struct HirDestructorInstance {
  HirDestructorInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  const DestructorDecl *source = nullptr;
  HirClassInstanceId owner = 0;
  HirBody body;
};

class HirProgram {
public:
  [[nodiscard]] bool valid() const { return valid_; }

  [[nodiscard]] const std::vector<HirClassInstance> &classInstances() const {
    return classes;
  }

  [[nodiscard]] const std::vector<HirFunctionInstance> &
  functionInstances() const {
    return functions;
  }

  [[nodiscard]] const std::vector<HirConstructorInstance> &
  constructorInstances() const {
    return constructors;
  }

  [[nodiscard]] const std::vector<HirDestructorInstance> &
  destructorInstances() const {
    return destructors;
  }

  [[nodiscard]] const HirBody &module() const { return moduleBody; }

  [[nodiscard]] std::size_t valueCount() const {
    std::size_t count = moduleBody.values.size();
    for (const HirClassInstance &classInstance : classes) {
      count += classInstance.fieldInitializers.values.size();
    }
    for (const HirFunctionInstance &function : functions) {
      count += function.body.values.size();
    }
    for (const HirConstructorInstance &constructor : constructors) {
      count += constructor.body.values.size();
    }
    for (const HirDestructorInstance &destructor : destructors) {
      count += destructor.body.values.size();
    }
    return count;
  }

  [[nodiscard]] std::size_t statementCount() const {
    std::size_t count = moduleBody.statements.size();
    for (const HirClassInstance &classInstance : classes) {
      count += classInstance.fieldInitializers.statements.size();
    }
    for (const HirFunctionInstance &function : functions) {
      count += function.body.statements.size();
    }
    for (const HirConstructorInstance &constructor : constructors) {
      count += constructor.body.statements.size();
    }
    for (const HirDestructorInstance &destructor : destructors) {
      count += destructor.body.statements.size();
    }
    return count;
  }

  [[nodiscard]] const HirFunctionInstance *
  findFunctionInstance(HirFunctionInstanceId id) const {
    return id == 0 || id > functions.size() ? nullptr : &functions[id - 1];
  }

  [[nodiscard]] const HirConstructorInstance *
  findConstructorInstance(HirConstructorInstanceId id) const {
    return id == 0 || id > constructors.size() ? nullptr
                                               : &constructors[id - 1];
  }

  [[nodiscard]] const HirDestructorInstance *
  findDestructorInstance(HirDestructorInstanceId id) const {
    return id == 0 || id > destructors.size() ? nullptr : &destructors[id - 1];
  }

  [[nodiscard]] const std::vector<HirValueId> &
  valueIdsForSource(const Expr &source) const {
    static const std::vector<HirValueId> empty;
    const auto found = sourceValueIds.find(&source);
    return found == sourceValueIds.end() ? empty : found->second;
  }

private:
  friend class HirLowerer;

  bool valid_ = true;
  std::vector<HirClassInstance> classes;
  std::vector<HirFunctionInstance> functions;
  std::vector<HirConstructorInstance> constructors;
  std::vector<HirDestructorInstance> destructors;
  HirBody moduleBody;
  std::unordered_map<const Expr *, std::vector<HirValueId>> sourceValueIds;
};

struct HirLoweringResult {
  HirProgram program;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool valid() const { return diagnostics.empty(); }
};

class HirLowerer {
public:
  explicit HirLowerer(TargetInfo target = TargetInfo::host())
      : target(std::move(target)) {}

  [[nodiscard]] HirLoweringResult lower(const Program &source,
                                        const SemanticVisitor &semantics) {
    analyzer = &semantics;
    baseModel = &semantics.model();
    output = {};
    nextValueId = 1;
    nextBindingId = 1;
    nextStatementId = 1;
    processedClasses = 0;
    processedFunctions = 0;
    processedConstructors = 0;
    processedDestructors = 0;

    seedDeclarations(source.declarations(), std::nullopt);
    processPendingInstances();
    output.program.valid_ = output.diagnostics.empty();
    return std::move(output);
  }

private:
  [[nodiscard]] static SemanticType
  substitute(const SemanticType &type, const TypeSubstitution &substitution) {
    if (type.kind == SemanticType::TypeParameter) {
      const auto found = substitution.find(type.genericParameterId);
      return found == substitution.end() ? type : found->second;
    }
    SemanticType result = type;
    for (SemanticType &argument : result.arguments) {
      argument = substitute(argument, substitution);
    }
    return result;
  }

  [[nodiscard]] TypeSubstitution
  classSubstitution(const ClassTypeInfo &declaration,
                    const std::vector<SemanticType> &typeArguments) const {
    TypeSubstitution result;
    const std::size_t count =
        std::min(declaration.genericParameters.size(), typeArguments.size());
    for (std::size_t index = 0; index < count; ++index) {
      result.emplace(declaration.genericParameters[index].id,
                     typeArguments[index]);
    }
    return result;
  }

  [[nodiscard]] std::optional<HirClassInstanceId>
  enqueueClass(const SemanticType &type) {
    for (const SemanticType &argument : type.arguments) {
      (void)enqueueClass(argument);
    }
    if (type.kind != SemanticType::Class || type.classId == 0) {
      return std::nullopt;
    }
    for (const HirClassInstance &instance : output.program.classes) {
      if (instance.declaration == type.classId &&
          instance.typeArguments == type.arguments) {
        return instance.id;
      }
    }
    const ClassTypeInfo *declaration = baseModel->findClassType(type.classId);
    if (declaration == nullptr || declaration->declaration == nullptr) {
      return std::nullopt;
    }
    const HirClassInstanceId id = output.program.classes.size() + 1;
    output.program.classes.push_back({.id = id,
                                      .sourceUnit = declaration->sourceUnit,
                                      .declaration = type.classId,
                                      .source = declaration->declaration,
                                      .typeArguments = type.arguments,
                                      .type = type,
                                      .traits = analyzer->traitsFor(type)});
    return id;
  }

  [[nodiscard]] HirFunctionInstanceId
  enqueueFunction(const FunctionInfo &declaration,
                  const std::vector<SemanticType> &classTypeArguments,
                  std::vector<SemanticType> functionTypeArguments,
                  SemanticType returnType,
                  std::vector<SemanticType> parameterTypes,
                  std::optional<SourceSpan> site = std::nullopt) {
    if (declaration.parameterPack && !declaration.genericParameters.empty()) {
      const std::size_t fixed = declaration.genericParameters.size() - 1;
      if (functionTypeArguments.size() == fixed &&
          parameterTypes.size() >= declaration.parameterTypes.size() - 1) {
        const std::size_t fixedParameters =
            declaration.parameterTypes.size() - 1;
        functionTypeArguments.insert(
            functionTypeArguments.end(),
            parameterTypes.begin() +
                static_cast<std::ptrdiff_t>(fixedParameters),
            parameterTypes.end());
      }
    }

    for (const HirFunctionInstance &instance : output.program.functions) {
      const bool sameOwner =
          declaration.ownerClass == 0
              ? !instance.owner.has_value()
              : instance.owner &&
                    output.program.classes[*instance.owner - 1].typeArguments ==
                        classTypeArguments;
      if (instance.declaration == declaration.id && sameOwner &&
          instance.typeArguments == functionTypeArguments) {
        return instance.id;
      }
    }

    std::optional<HirClassInstanceId> owner;
    if (declaration.ownerClass != 0) {
      owner = enqueueClass(
          SemanticType::classType(declaration.ownerClass, classTypeArguments));
    }
    (void)enqueueClass(returnType);
    for (const SemanticType &parameter : parameterTypes) {
      (void)enqueueClass(parameter);
    }
    const HirFunctionInstanceId id = output.program.functions.size() + 1;
    output.program.functions.push_back(
        {.id = id,
         .sourceUnit = declaration.sourceUnit,
         .declaration = declaration.id,
         .source = declaration.declaration,
         .owner = owner,
         .typeArguments = std::move(functionTypeArguments),
         .returnType = std::move(returnType),
         .parameterTypes = std::move(parameterTypes),
         .instantiationSite = std::move(site)});
    return id;
  }

  [[nodiscard]] HirConstructorInstanceId
  enqueueConstructor(const ResolvedConstructionInfo &construction,
                     std::optional<SourceSpan> site = std::nullopt) {
    const std::optional<HirClassInstanceId> owner =
        enqueueClass(construction.constructedType);
    if (!owner || construction.declaration == nullptr ||
        construction.constructor == 0) {
      return 0;
    }
    for (const HirConstructorInstance &instance : output.program.constructors) {
      if (instance.declaration == construction.constructor &&
          instance.owner == *owner) {
        return instance.id;
      }
    }
    const HirConstructorInstanceId id = output.program.constructors.size() + 1;
    output.program.constructors.push_back(
        {.id = id,
         .sourceUnit = output.program.classes[*owner - 1].sourceUnit,
         .declaration = construction.constructor,
         .source = construction.declaration,
         .owner = *owner,
         .parameterTypes = construction.parameterTypes,
         .instantiationSite = std::move(site)});
    return id;
  }

  [[nodiscard]] std::optional<HirDestructorInstanceId>
  enqueueDestructor(HirClassInstanceId owner) {
    if (owner == 0 || owner > output.program.classes.size()) {
      return std::nullopt;
    }
    const HirClassInstance &classInstance = output.program.classes[owner - 1];
    const ClassTypeInfo *classType =
        baseModel->findClassType(classInstance.declaration);
    const ClassLifecycleInfo *lifecycle =
        classType == nullptr || classType->declaration == nullptr
            ? nullptr
            : baseModel->findClassLifecycle(*classType->declaration);
    if (lifecycle == nullptr || !lifecycle->declaredDestructor ||
        lifecycle->declaredDestructor->declaration == nullptr) {
      return std::nullopt;
    }
    for (const HirDestructorInstance &instance : output.program.destructors) {
      if (instance.owner == owner) {
        return instance.id;
      }
    }
    const HirDestructorInstanceId id = output.program.destructors.size() + 1;
    output.program.destructors.push_back(
        {.id = id,
         .sourceUnit = classInstance.sourceUnit,
         .source = lifecycle->declaredDestructor->declaration,
         .owner = owner});
    return id;
  }

  void seedDeclarations(const StmtList &declarations,
                        std::optional<ClassId> enclosingClass) {
    for (const StmtPtr &statement : declarations) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          seedDeclarations(*branch, enclosingClass);
        }
        continue;
      }
      if (const auto *namespaceDeclaration =
              dynamic_cast<const NamespaceDecl *>(statement.get())) {
        seedDeclarations(namespaceDeclaration->declarations(), enclosingClass);
        continue;
      }
      if (const auto *classDeclaration =
              dynamic_cast<const ClassDecl *>(statement.get())) {
        const ClassTypeInfo *classInfo =
            baseModel->findClassType(*classDeclaration);
        if (classInfo != nullptr && classInfo->genericParameters.empty()) {
          (void)enqueueClass(SemanticType::classType(classInfo->id));
        }
        seedDeclarations(classDeclaration->members(),
                         classInfo == nullptr
                             ? std::optional<ClassId>{}
                             : std::optional<ClassId>{classInfo->id});
        continue;
      }
      if (const auto *function =
              dynamic_cast<const FunctionDecl *>(statement.get())) {
        const FunctionInfo *info = baseModel->findFunction(*function);
        if (info == nullptr || !info->genericParameters.empty()) {
          continue;
        }
        std::vector<SemanticType> classArguments;
        if (info->ownerClass != 0) {
          const ClassTypeInfo *owner =
              baseModel->findClassType(info->ownerClass);
          if (owner == nullptr || !owner->genericParameters.empty()) {
            continue;
          }
        }
        (void)enqueueFunction(*info, classArguments, {}, info->returnType,
                              info->parameterTypes);
        continue;
      }
      if (const auto *variable =
              dynamic_cast<const VariableDecl *>(statement.get())) {
        if (!enclosingClass) {
          if (const std::optional<HirStatementId> root = lowerStatement(
                  variable, *baseModel, {}, output.program.moduleBody)) {
            output.program.moduleBody.roots.push_back(*root);
          }
        }
      }
    }
  }

  void processPendingInstances() {
    while (processedClasses < output.program.classes.size() ||
           processedFunctions < output.program.functions.size() ||
           processedConstructors < output.program.constructors.size() ||
           processedDestructors < output.program.destructors.size()) {
      while (processedClasses < output.program.classes.size()) {
        processClass(processedClasses++);
      }
      while (processedFunctions < output.program.functions.size()) {
        processFunction(processedFunctions++);
      }
      while (processedConstructors < output.program.constructors.size()) {
        processConstructor(processedConstructors++);
      }
      while (processedDestructors < output.program.destructors.size()) {
        processDestructor(processedDestructors++);
      }
    }
  }

  void processClass(std::size_t index) {
    const HirClassInstance snapshot = output.program.classes[index];
    const ClassTypeInfo *declaration =
        baseModel->findClassType(snapshot.declaration);
    if (declaration == nullptr) {
      return;
    }
    const TypeSubstitution substitution =
        classSubstitution(*declaration, snapshot.typeArguments);
    SemanticInstanceAnalysis analysis;
    const SemanticModel *model = baseModel;
    if (!snapshot.typeArguments.empty()) {
      analysis = analyzer->analyzeClassFieldInitializers(
          snapshot.declaration, snapshot.typeArguments);
      appendInstanceDiagnostics(std::move(analysis.diagnostics), std::nullopt);
      model = &analysis.model;
    }
    std::vector<HirClassField> fields;
    HirBody fieldInitializers;
    fields.reserve(declaration->fields.size());
    for (const ClassFieldTypeInfo &field : declaration->fields) {
      const SemanticType type = substitute(field.type, substitution);
      (void)enqueueClass(type);
      const BindingInfo info{.type = type,
                             .access = field.declaration != nullptr &&
                                               field.declaration->isMutable()
                                           ? AccessMode::Mutable
                                           : AccessMode::ReadOnly,
                             .traits = analyzer->traitsFor(type)};
      const HirBindingId binding =
          field.declaration == nullptr
              ? 0
              : lowerBinding(*field.declaration, info, fieldInitializers);
      const std::optional<HirValueId> initializer =
          field.declaration == nullptr
              ? std::nullopt
              : lowerExpression(field.declaration->initializer(), *model,
                                snapshot.typeArguments, fieldInitializers);
      if (field.declaration != nullptr) {
        HirStatement statement{.kind = HirStatementKind::Variable,
                               .source = field.declaration,
                               .binding = binding,
                               .value = initializer};
        fieldInitializers.roots.push_back(
            appendStatement(std::move(statement), fieldInitializers));
      }
      fields.push_back({.declaration = field.declaration,
                        .binding = binding,
                        .initializer = initializer,
                        .info = info});
    }
    output.program.classes[index].fields = std::move(fields);
    output.program.classes[index].fieldInitializers =
        std::move(fieldInitializers);
    output.program.classes[index].destructor = enqueueDestructor(snapshot.id);
  }

  void appendInstanceDiagnostics(std::vector<Diagnostic> diagnostics,
                                 const std::optional<SourceSpan> &site) {
    for (Diagnostic &diagnostic : diagnostics) {
      if (site) {
        diagnostic.related.push_back(
            {*site, "Concrete generic instance requested here."});
      }
      output.diagnostics.push_back(std::move(diagnostic));
    }
  }

  void processFunction(std::size_t index) {
    const HirFunctionInstance snapshot = output.program.functions[index];
    const FunctionInfo *declaration =
        baseModel->findFunction(snapshot.declaration);
    if (declaration == nullptr || declaration->declaration == nullptr) {
      return;
    }
    std::vector<SemanticType> classArguments;
    if (snapshot.owner) {
      classArguments =
          output.program.classes[*snapshot.owner - 1].typeArguments;
    }

    const bool concreteInstance =
        !classArguments.empty() || !snapshot.typeArguments.empty();
    SemanticInstanceAnalysis analysis;
    const SemanticModel *model = baseModel;
    if (concreteInstance) {
      analysis = analyzer->analyzeFunctionInstance(
          declaration->id, classArguments, snapshot.typeArguments);
      appendInstanceDiagnostics(std::move(analysis.diagnostics),
                                snapshot.instantiationSite);
      model = &analysis.model;
    }

    HirBody body;
    for (const Parameter &parameter : declaration->declaration->parameters()) {
      (void)lowerBinding(parameter, *model, body);
    }
    if (declaration->declaration->body()) {
      body.roots =
          lowerStatements(declaration->declaration->body()->statements(),
                          *model, classArguments, body);
    }
    output.program.functions[index].body = std::move(body);
  }

  void processConstructor(std::size_t index) {
    const HirConstructorInstance snapshot = output.program.constructors[index];
    const std::vector<SemanticType> classArguments =
        output.program.classes[snapshot.owner - 1].typeArguments;
    SemanticInstanceAnalysis analysis = analyzer->analyzeConstructorInstance(
        snapshot.declaration, classArguments);
    appendInstanceDiagnostics(std::move(analysis.diagnostics),
                              snapshot.instantiationSite);

    HirBody body;
    for (const Parameter &parameter : snapshot.source->parameters()) {
      (void)lowerBinding(parameter, analysis.model, body);
    }
    std::vector<HirValueId> initializerValues;
    for (const ConstructorInitializer &initializer :
         snapshot.source->initializers()) {
      if (const std::optional<HirValueId> value = lowerExpression(
              initializer.value, analysis.model, classArguments, body)) {
        initializerValues.push_back(*value);
      }
    }
    body.roots = lowerStatements(snapshot.source->body()->statements(),
                                 analysis.model, classArguments, body);
    output.program.constructors[index].initializerValues =
        std::move(initializerValues);
    output.program.constructors[index].body = std::move(body);
  }

  void processDestructor(std::size_t index) {
    const HirDestructorInstance snapshot = output.program.destructors[index];
    const HirClassInstance &owner = output.program.classes[snapshot.owner - 1];
    SemanticInstanceAnalysis analysis;
    const SemanticModel *model = baseModel;
    if (!owner.typeArguments.empty()) {
      analysis = analyzer->analyzeDestructorInstance(owner.declaration,
                                                     owner.typeArguments);
      appendInstanceDiagnostics(std::move(analysis.diagnostics), std::nullopt);
      model = &analysis.model;
    }

    HirBody body;
    body.roots = lowerStatements(snapshot.source->body()->statements(), *model,
                                 owner.typeArguments, body);
    output.program.destructors[index].body = std::move(body);
  }

  [[nodiscard]] HirBindingId lowerBinding(const VariableDecl &declaration,
                                          const SemanticModel &model,
                                          HirBody &body) {
    const BindingInfo *info = model.findBinding(declaration);
    return lowerBinding(
        declaration,
        info == nullptr ? makeBindingInfo(SemanticType::Unknown) : *info, body);
  }

  [[nodiscard]] HirBindingId lowerBinding(const VariableDecl &declaration,
                                          const BindingInfo &info,
                                          HirBody &body) {
    const HirBindingId id = nextBindingId++;
    body.bindings.push_back({.id = id, .variable = &declaration, .info = info});
    (void)enqueueClass(info.type);
    return id;
  }

  [[nodiscard]] HirBindingId lowerBinding(const Parameter &parameter,
                                          const SemanticModel &model,
                                          HirBody &body) {
    const BindingInfo *info = model.findBinding(parameter);
    const HirBindingId id = nextBindingId++;
    body.bindings.push_back(
        {.id = id,
         .parameter = &parameter,
         .info =
             info == nullptr ? makeBindingInfo(SemanticType::Unknown) : *info});
    if (info != nullptr) {
      (void)enqueueClass(info->type);
    }
    return id;
  }

  [[nodiscard]] HirStatementId appendStatement(HirStatement statement,
                                               HirBody &body) {
    statement.id = nextStatementId++;
    const HirStatementId id = statement.id;
    body.statements.push_back(std::move(statement));
    return id;
  }

  [[nodiscard]] std::vector<HirStatementId>
  lowerStatements(const StmtList &statements, const SemanticModel &model,
                  const std::vector<SemanticType> &classArguments,
                  HirBody &body) {
    std::vector<HirStatementId> result;
    for (const StmtPtr &statement : statements) {
      if (const std::optional<HirStatementId> lowered =
              lowerStatement(statement.get(), model, classArguments, body)) {
        result.push_back(*lowered);
      }
    }
    return result;
  }

  [[nodiscard]] std::optional<HirStatementId>
  lowerStatement(const Stmt *statement, const SemanticModel &model,
                 const std::vector<SemanticType> &classArguments,
                 HirBody &body) {
    if (statement == nullptr) {
      return std::nullopt;
    }
    if (const auto *block = dynamic_cast<const BlockStmt *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::Block,
           .source = statement,
           .statements = lowerStatements(block->statements(), model,
                                         classArguments, body)},
          body);
    }
    if (const auto *conditional =
            dynamic_cast<const ConditionalStmt *>(statement)) {
      std::vector<HirStatementId> statements;
      if (const StmtList *branch = conditional->activeBranch(target)) {
        statements = lowerStatements(*branch, model, classArguments, body);
      }
      return appendStatement({.kind = HirStatementKind::CompileTimeBranch,
                              .source = statement,
                              .statements = std::move(statements)},
                             body);
    }
    if (const auto *expression =
            dynamic_cast<const ExpressionStmt *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::Expression,
           .source = statement,
           .value = lowerExpression(expression->expression(), model,
                                    classArguments, body)},
          body);
    }
    if (const auto *forStatement = dynamic_cast<const ForStmt *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::For,
           .source = statement,
           .condition = lowerExpression(forStatement->condition(), model,
                                        classArguments, body),
           .increment = lowerExpression(forStatement->increment(), model,
                                        classArguments, body),
           .initializer = lowerStatement(forStatement->initializer().get(),
                                         model, classArguments, body),
           .body = lowerStatement(forStatement->body().get(), model,
                                  classArguments, body)},
          body);
    }
    if (const auto *ifStatement = dynamic_cast<const IfStmt *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::If,
           .source = statement,
           .condition = lowerExpression(ifStatement->condition(), model,
                                        classArguments, body),
           .body = lowerStatement(ifStatement->thenBranch().get(), model,
                                  classArguments, body),
           .elseBranch = lowerStatement(ifStatement->elseBranch().get(), model,
                                        classArguments, body)},
          body);
    }
    if (const auto *loopControl =
            dynamic_cast<const LoopControlStmt *>(statement)) {
      return appendStatement(
          {.kind = loopControl->keyword().kind == TokenKind::BREAK
                       ? HirStatementKind::Break
                       : HirStatementKind::Continue,
           .source = statement},
          body);
    }
    if (const auto *returnStatement =
            dynamic_cast<const ReturnStmt *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::Return,
           .source = statement,
           .value = lowerExpression(returnStatement->value(), model,
                                    classArguments, body)},
          body);
    }
    if (const auto *variable = dynamic_cast<const VariableDecl *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::Variable,
           .source = statement,
           .binding = lowerBinding(*variable, model, body),
           .value = lowerExpression(variable->initializer(), model,
                                    classArguments, body)},
          body);
    }
    if (const auto *whileStatement =
            dynamic_cast<const WhileStmt *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::While,
           .source = statement,
           .condition = lowerExpression(whileStatement->condition(), model,
                                        classArguments, body),
           .body = lowerStatement(whileStatement->body().get(), model,
                                  classArguments, body)},
          body);
    }
    if (dynamic_cast<const EmptyStmt *>(statement) != nullptr) {
      return appendStatement(
          {.kind = HirStatementKind::Empty, .source = statement}, body);
    }
    return std::nullopt;
  }

  [[nodiscard]] std::vector<SemanticType> receiverClassArguments(
      const ExprPtr &callee, const FunctionInfo &target,
      const SemanticModel &model,
      const std::vector<SemanticType> &currentClassArguments) const {
    if (target.ownerClass == 0) {
      return {};
    }
    if (const auto *member = dynamic_cast<const Get *>(callee.get())) {
      SemanticType receiver = model.typeOf(*member->object());
      if (receiver.kind == SemanticType::Class &&
          receiver.classId == target.ownerClass) {
        return receiver.arguments;
      }
      if (const ResolvedOperatorInfo *arrow = model.findOperator(*member);
          arrow != nullptr &&
          arrow->returnType.kind == SemanticType::Reference &&
          !arrow->returnType.arguments.empty()) {
        receiver = arrow->returnType.arguments.front();
        if (receiver.kind == SemanticType::Class &&
            receiver.classId == target.ownerClass) {
          return receiver.arguments;
        }
      }
    }
    return currentClassArguments;
  }

  [[nodiscard]] std::optional<HirValueId>
  lowerExpression(const ExprPtr &expression, const SemanticModel &model,
                  const std::vector<SemanticType> &classArguments,
                  HirBody &body) {
    if (!expression) {
      return std::nullopt;
    }
    const Expr *raw = expression.get();
    HirValueKind kind = HirValueKind::Literal;
    std::vector<HirValueId> operands;
    std::optional<TokenKind> operation;
    std::optional<Literal> literal;
    const auto lowerOperand = [&](const ExprPtr &operand) {
      if (const std::optional<HirValueId> id =
              lowerExpression(operand, model, classArguments, body)) {
        operands.push_back(*id);
      }
    };

    if (const auto *assign = dynamic_cast<const Assign *>(raw)) {
      kind = HirValueKind::Assignment;
      operation = assign->oper().kind;
      lowerOperand(assign->value());
    } else if (const auto *initializer =
                   dynamic_cast<const ArrayInitializer *>(raw)) {
      kind = HirValueKind::ArrayInitializer;
      for (const ExprPtr &element : initializer->elements()) {
        lowerOperand(element);
      }
    } else if (const auto *binary = dynamic_cast<const Binary *>(raw)) {
      kind = HirValueKind::Binary;
      operation = binary->oper().kind;
      lowerOperand(binary->left());
      lowerOperand(binary->right());
    } else if (const auto *call = dynamic_cast<const Call *>(raw)) {
      kind = HirValueKind::Call;
      lowerOperand(call->callee());
      for (const ExprPtr &argument : call->arguments()) {
        lowerOperand(argument);
      }
    } else if (const auto *conversion = dynamic_cast<const Conversion *>(raw)) {
      kind = HirValueKind::Conversion;
      lowerOperand(conversion->value());
    } else if (const auto *set = dynamic_cast<const DereferenceSet *>(raw)) {
      kind = HirValueKind::DereferenceSet;
      operation = set->oper().kind;
      lowerOperand(set->object());
      lowerOperand(set->value());
    } else if (const auto *get = dynamic_cast<const Get *>(raw)) {
      kind = HirValueKind::MemberAccess;
      operation = get->access().kind;
      lowerOperand(get->object());
    } else if (const auto *grouping = dynamic_cast<const Grouping *>(raw)) {
      kind = HirValueKind::Grouping;
      lowerOperand(grouping->expression());
    } else if (const auto *index = dynamic_cast<const Index *>(raw)) {
      kind = HirValueKind::Index;
      lowerOperand(index->object());
      lowerOperand(index->index());
    } else if (const auto *set = dynamic_cast<const IndexSet *>(raw)) {
      kind = HirValueKind::IndexSet;
      operation = set->oper().kind;
      lowerOperand(set->object());
      lowerOperand(set->index());
      lowerOperand(set->value());
    } else if (const auto *literalExpression =
                   dynamic_cast<const LiteralExpr *>(raw)) {
      kind = HirValueKind::Literal;
      literal = literalExpression->value();
    } else if (const auto *logical = dynamic_cast<const Logical *>(raw)) {
      kind = HirValueKind::Logical;
      operation = logical->oper().kind;
      lowerOperand(logical->left());
      lowerOperand(logical->right());
    } else if (dynamic_cast<const PackExpansion *>(raw) != nullptr) {
      kind = HirValueKind::PackExpansion;
    } else if (const auto *postfix = dynamic_cast<const Postfix *>(raw)) {
      kind = HirValueKind::Postfix;
      operation = postfix->oper().kind;
      lowerOperand(postfix->expression());
    } else if (dynamic_cast<const QualifiedName *>(raw) != nullptr) {
      kind = HirValueKind::QualifiedName;
    } else if (dynamic_cast<const Self *>(raw) != nullptr) {
      kind = HirValueKind::Self;
    } else if (const auto *set = dynamic_cast<const Set *>(raw)) {
      kind = HirValueKind::MemberSet;
      operation = set->oper().kind;
      lowerOperand(set->object());
      lowerOperand(set->value());
    } else if (const auto *unary = dynamic_cast<const Unary *>(raw)) {
      kind = HirValueKind::Unary;
      operation = unary->oper().kind;
      lowerOperand(unary->right());
    } else if (const auto *unexpected = dynamic_cast<const Unexpected *>(raw)) {
      kind = HirValueKind::Unexpected;
      lowerOperand(unexpected->error());
    } else if (dynamic_cast<const Variable *>(raw) != nullptr) {
      kind = HirValueKind::Variable;
    }

    HirValue value{.id = nextValueId++,
                   .kind = kind,
                   .source = raw,
                   .operands = std::move(operands),
                   .operation = operation,
                   .literal = std::move(literal)};
    if (const ExpressionInfo *info = model.findExpression(*raw)) {
      value.info = *info;
      (void)enqueueClass(info->type);
    }
    if (const auto *call = dynamic_cast<const Call *>(raw)) {
      if (const ResolvedCallInfo *resolved = model.findCall(*call)) {
        value.intrinsic = resolved->intrinsic;
        if (resolved->function != 0 && resolved->declaration != nullptr) {
          if (const FunctionInfo *target =
                  baseModel->findFunction(resolved->function)) {
            value.functionTarget = enqueueFunction(
                *target,
                receiverClassArguments(call->callee(), *target, model,
                                       classArguments),
                resolved->typeArguments, resolved->returnType,
                resolved->parameterTypes, tokenSpan(call->paren()));
          }
        }
      }
      if (const ResolvedConstructionInfo *construction =
              model.findConstruction(*call)) {
        const HirConstructorInstanceId target =
            enqueueConstructor(*construction, tokenSpan(call->paren()));
        if (target != 0) {
          value.constructorTarget = target;
        }
      }
    }
    if (const ResolvedOperatorInfo *resolved = model.findOperator(*raw);
        resolved != nullptr && resolved->function != 0) {
      if (const FunctionInfo *target =
              baseModel->findFunction(resolved->function)) {
        SemanticType receiverType = SemanticType::Unknown;
        if (const auto *binary = dynamic_cast<const Binary *>(raw)) {
          receiverType = model.typeOf(*binary->left());
        } else if (const auto *get = dynamic_cast<const Get *>(raw)) {
          receiverType = model.typeOf(*get->object());
        } else if (const auto *index = dynamic_cast<const Index *>(raw)) {
          receiverType = model.typeOf(*index->object());
        } else if (const auto *unary = dynamic_cast<const Unary *>(raw)) {
          receiverType = model.typeOf(*unary->right());
        }
        const std::vector<SemanticType> ownerArguments =
            receiverType.kind == SemanticType::Class ? receiverType.arguments
                                                     : classArguments;
        value.functionTarget =
            enqueueFunction(*target, ownerArguments, {}, resolved->returnType,
                            resolved->parameterTypes);
      }
    }
    const HirValueId id = value.id;
    body.values.push_back(std::move(value));
    output.program.sourceValueIds[raw].push_back(id);
    return id;
  }

  TargetInfo target;
  const SemanticVisitor *analyzer = nullptr;
  const SemanticModel *baseModel = nullptr;
  HirLoweringResult output;
  HirValueId nextValueId = 1;
  HirBindingId nextBindingId = 1;
  HirStatementId nextStatementId = 1;
  std::size_t processedClasses = 0;
  std::size_t processedFunctions = 0;
  std::size_t processedConstructors = 0;
  std::size_t processedDestructors = 0;
};

} // namespace lang
