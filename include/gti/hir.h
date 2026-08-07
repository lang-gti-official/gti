#pragma once

#include "gti/ast.h"
#include "gti/diagnostic.h"
#include "gti/semantic_analyzer.h"
#include "gti/target.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace lang {

using HirValueId = std::size_t;
using HirBindingId = std::size_t;
using HirClassInstanceId = std::size_t;
using HirFunctionInstanceId = std::size_t;
using HirConstructorInstanceId = std::size_t;

struct HirBinding {
  HirBindingId id = 0;
  const VariableDecl *variable = nullptr;
  const Parameter *parameter = nullptr;
  BindingInfo info;
};

struct HirValue {
  HirValueId id = 0;
  const Expr *source = nullptr;
  ExpressionInfo info;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  std::optional<HirFunctionInstanceId> functionTarget;
  std::optional<HirConstructorInstanceId> constructorTarget;
};

struct HirClassField {
  const VariableDecl *declaration = nullptr;
  BindingInfo info;
};

struct HirClassInstance {
  HirClassInstanceId id = 0;
  ClassId declaration = 0;
  const ClassDecl *source = nullptr;
  std::vector<SemanticType> typeArguments;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits;
  std::vector<HirClassField> fields;
};

struct HirFunctionInstance {
  HirFunctionInstanceId id = 0;
  FunctionId declaration = 0;
  const FunctionDecl *source = nullptr;
  std::optional<HirClassInstanceId> owner;
  std::vector<SemanticType> typeArguments;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<HirBinding> bindings;
  std::vector<HirValue> values;
  std::optional<SourceSpan> instantiationSite;
};

struct HirConstructorInstance {
  HirConstructorInstanceId id = 0;
  ConstructorId declaration = 0;
  const ConstructorDecl *source = nullptr;
  HirClassInstanceId owner = 0;
  std::vector<SemanticType> parameterTypes;
  std::vector<HirBinding> bindings;
  std::vector<HirValue> values;
  std::optional<SourceSpan> instantiationSite;
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

  [[nodiscard]] std::size_t valueCount() const {
    std::size_t count = moduleValues.size();
    for (const HirFunctionInstance &function : functions) {
      count += function.values.size();
    }
    for (const HirConstructorInstance &constructor : constructors) {
      count += constructor.values.size();
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

private:
  friend class HirLowerer;

  bool valid_ = true;
  std::vector<HirClassInstance> classes;
  std::vector<HirFunctionInstance> functions;
  std::vector<HirConstructorInstance> constructors;
  std::vector<HirBinding> moduleBindings;
  std::vector<HirValue> moduleValues;
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
    processedClasses = 0;
    processedFunctions = 0;
    processedConstructors = 0;

    seedDeclarations(source.declarations(), std::nullopt);
    processPendingInstances();
    output.program.valid_ = output.diagnostics.empty();
    return std::move(output);
  }

private:
  struct LoweredBody {
    std::vector<HirBinding> bindings;
    std::vector<HirValue> values;
  };

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
         .declaration = construction.constructor,
         .source = construction.declaration,
         .owner = *owner,
         .parameterTypes = construction.parameterTypes,
         .instantiationSite = std::move(site)});
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
        if (!enclosingClass && variable->initializer()) {
          LoweredBody body;
          lowerBinding(*variable, *baseModel, body);
          lowerExpression(variable->initializer(), *baseModel, {}, body);
          output.program.moduleBindings.insert(
              output.program.moduleBindings.end(), body.bindings.begin(),
              body.bindings.end());
          output.program.moduleValues.insert(output.program.moduleValues.end(),
                                             body.values.begin(),
                                             body.values.end());
        }
      }
    }
  }

  void processPendingInstances() {
    while (processedClasses < output.program.classes.size() ||
           processedFunctions < output.program.functions.size() ||
           processedConstructors < output.program.constructors.size()) {
      while (processedClasses < output.program.classes.size()) {
        processClass(processedClasses++);
      }
      while (processedFunctions < output.program.functions.size()) {
        processFunction(processedFunctions++);
      }
      while (processedConstructors < output.program.constructors.size()) {
        processConstructor(processedConstructors++);
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
    std::vector<HirClassField> fields;
    fields.reserve(declaration->fields.size());
    for (const ClassFieldTypeInfo &field : declaration->fields) {
      const SemanticType type = substitute(field.type, substitution);
      (void)enqueueClass(type);
      fields.push_back(
          {.declaration = field.declaration,
           .info = BindingInfo{.type = type,
                               .access = field.declaration != nullptr &&
                                                 field.declaration->isMutable()
                                             ? AccessMode::Mutable
                                             : AccessMode::ReadOnly,
                               .traits = analyzer->traitsFor(type)}});
    }
    output.program.classes[index].fields = std::move(fields);
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

    LoweredBody body;
    for (const Parameter &parameter : declaration->declaration->parameters()) {
      lowerBinding(parameter, *model, body);
    }
    if (declaration->declaration->body()) {
      lowerStatements(declaration->declaration->body()->statements(), *model,
                      classArguments, body);
    }
    output.program.functions[index].bindings = std::move(body.bindings);
    output.program.functions[index].values = std::move(body.values);
  }

  void processConstructor(std::size_t index) {
    const HirConstructorInstance snapshot = output.program.constructors[index];
    const std::vector<SemanticType> classArguments =
        output.program.classes[snapshot.owner - 1].typeArguments;
    SemanticInstanceAnalysis analysis = analyzer->analyzeConstructorInstance(
        snapshot.declaration, classArguments);
    appendInstanceDiagnostics(std::move(analysis.diagnostics),
                              snapshot.instantiationSite);

    LoweredBody body;
    for (const Parameter &parameter : snapshot.source->parameters()) {
      lowerBinding(parameter, analysis.model, body);
    }
    for (const ConstructorInitializer &initializer :
         snapshot.source->initializers()) {
      lowerExpression(initializer.value, analysis.model, classArguments, body);
    }
    lowerStatements(snapshot.source->body()->statements(), analysis.model,
                    classArguments, body);
    output.program.constructors[index].bindings = std::move(body.bindings);
    output.program.constructors[index].values = std::move(body.values);
  }

  void lowerBinding(const VariableDecl &declaration, const SemanticModel &model,
                    LoweredBody &body) {
    const BindingInfo *info = model.findBinding(declaration);
    body.bindings.push_back(
        {.id = nextBindingId++,
         .variable = &declaration,
         .info =
             info == nullptr ? makeBindingInfo(SemanticType::Unknown) : *info});
    if (info != nullptr) {
      (void)enqueueClass(info->type);
    }
  }

  void lowerBinding(const Parameter &parameter, const SemanticModel &model,
                    LoweredBody &body) {
    const BindingInfo *info = model.findBinding(parameter);
    body.bindings.push_back(
        {.id = nextBindingId++,
         .parameter = &parameter,
         .info =
             info == nullptr ? makeBindingInfo(SemanticType::Unknown) : *info});
    if (info != nullptr) {
      (void)enqueueClass(info->type);
    }
  }

  void lowerStatements(const StmtList &statements, const SemanticModel &model,
                       const std::vector<SemanticType> &classArguments,
                       LoweredBody &body) {
    for (const StmtPtr &statement : statements) {
      lowerStatement(statement.get(), model, classArguments, body);
    }
  }

  void lowerStatement(const Stmt *statement, const SemanticModel &model,
                      const std::vector<SemanticType> &classArguments,
                      LoweredBody &body) {
    if (statement == nullptr) {
      return;
    }
    if (const auto *block = dynamic_cast<const BlockStmt *>(statement)) {
      lowerStatements(block->statements(), model, classArguments, body);
    } else if (const auto *conditional =
                   dynamic_cast<const ConditionalStmt *>(statement)) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        lowerStatements(*branch, model, classArguments, body);
      }
    } else if (const auto *expression =
                   dynamic_cast<const ExpressionStmt *>(statement)) {
      lowerExpression(expression->expression(), model, classArguments, body);
    } else if (const auto *forStatement =
                   dynamic_cast<const ForStmt *>(statement)) {
      lowerStatement(forStatement->initializer().get(), model, classArguments,
                     body);
      lowerExpression(forStatement->condition(), model, classArguments, body);
      lowerExpression(forStatement->increment(), model, classArguments, body);
      lowerStatement(forStatement->body().get(), model, classArguments, body);
    } else if (const auto *ifStatement =
                   dynamic_cast<const IfStmt *>(statement)) {
      lowerExpression(ifStatement->condition(), model, classArguments, body);
      lowerStatement(ifStatement->thenBranch().get(), model, classArguments,
                     body);
      lowerStatement(ifStatement->elseBranch().get(), model, classArguments,
                     body);
    } else if (const auto *returnStatement =
                   dynamic_cast<const ReturnStmt *>(statement)) {
      lowerExpression(returnStatement->value(), model, classArguments, body);
    } else if (const auto *variable =
                   dynamic_cast<const VariableDecl *>(statement)) {
      lowerBinding(*variable, model, body);
      lowerExpression(variable->initializer(), model, classArguments, body);
    } else if (const auto *whileStatement =
                   dynamic_cast<const WhileStmt *>(statement)) {
      lowerExpression(whileStatement->condition(), model, classArguments, body);
      lowerStatement(whileStatement->body().get(), model, classArguments, body);
    }
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

  void lowerExpression(const ExprPtr &expression, const SemanticModel &model,
                       const std::vector<SemanticType> &classArguments,
                       LoweredBody &body) {
    if (!expression) {
      return;
    }
    const Expr *raw = expression.get();
    if (const auto *assign = dynamic_cast<const Assign *>(raw)) {
      lowerExpression(assign->value(), model, classArguments, body);
    } else if (const auto *initializer =
                   dynamic_cast<const ArrayInitializer *>(raw)) {
      for (const ExprPtr &element : initializer->elements()) {
        lowerExpression(element, model, classArguments, body);
      }
    } else if (const auto *binary = dynamic_cast<const Binary *>(raw)) {
      lowerExpression(binary->left(), model, classArguments, body);
      lowerExpression(binary->right(), model, classArguments, body);
    } else if (const auto *call = dynamic_cast<const Call *>(raw)) {
      lowerExpression(call->callee(), model, classArguments, body);
      for (const ExprPtr &argument : call->arguments()) {
        lowerExpression(argument, model, classArguments, body);
      }
    } else if (const auto *conversion = dynamic_cast<const Conversion *>(raw)) {
      lowerExpression(conversion->value(), model, classArguments, body);
    } else if (const auto *set = dynamic_cast<const DereferenceSet *>(raw)) {
      lowerExpression(set->object(), model, classArguments, body);
      lowerExpression(set->value(), model, classArguments, body);
    } else if (const auto *get = dynamic_cast<const Get *>(raw)) {
      lowerExpression(get->object(), model, classArguments, body);
    } else if (const auto *grouping = dynamic_cast<const Grouping *>(raw)) {
      lowerExpression(grouping->expression(), model, classArguments, body);
    } else if (const auto *index = dynamic_cast<const Index *>(raw)) {
      lowerExpression(index->object(), model, classArguments, body);
      lowerExpression(index->index(), model, classArguments, body);
    } else if (const auto *set = dynamic_cast<const IndexSet *>(raw)) {
      lowerExpression(set->object(), model, classArguments, body);
      lowerExpression(set->index(), model, classArguments, body);
      lowerExpression(set->value(), model, classArguments, body);
    } else if (const auto *logical = dynamic_cast<const Logical *>(raw)) {
      lowerExpression(logical->left(), model, classArguments, body);
      lowerExpression(logical->right(), model, classArguments, body);
    } else if (const auto *postfix = dynamic_cast<const Postfix *>(raw)) {
      lowerExpression(postfix->expression(), model, classArguments, body);
    } else if (const auto *set = dynamic_cast<const Set *>(raw)) {
      lowerExpression(set->object(), model, classArguments, body);
      lowerExpression(set->value(), model, classArguments, body);
    } else if (const auto *unary = dynamic_cast<const Unary *>(raw)) {
      lowerExpression(unary->right(), model, classArguments, body);
    } else if (const auto *unexpected = dynamic_cast<const Unexpected *>(raw)) {
      lowerExpression(unexpected->error(), model, classArguments, body);
    }

    HirValue value{.id = nextValueId++, .source = raw};
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
    body.values.push_back(std::move(value));
  }

  TargetInfo target;
  const SemanticVisitor *analyzer = nullptr;
  const SemanticModel *baseModel = nullptr;
  HirLoweringResult output;
  HirValueId nextValueId = 1;
  HirBindingId nextBindingId = 1;
  std::size_t processedClasses = 0;
  std::size_t processedFunctions = 0;
  std::size_t processedConstructors = 0;
};

} // namespace lang
