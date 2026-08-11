#pragma once

#include "gti/ast.h"
#include "gti/diagnostic.h"
#include "gti/semantic_analyzer.h"
#include "gti/target.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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
using HirLambdaId = std::size_t;

enum class HirValueKind {
  Assignment,
  ArrayInitializer,
  Binary,
  Call,
  Conditional,
  Move,
  Conversion,
  DirectInitializer,
  DereferenceSet,
  MemberAccess,
  Grouping,
  Index,
  IndexSet,
  Lambda,
  Literal,
  Logical,
  PackExpansion,
  Postfix,
  QualifiedName,
  This,
  MemberSet,
  Unary,
  Unexpected,
  Variable,
};

enum class HirStatementKind {
  Block,
  CompileTimeBranch,
  DoWhile,
  Empty,
  Expression,
  For,
  RangeFor,
  If,
  Break,
  Continue,
  Return,
  Switch,
  StructuredBinding,
  Variable,
  While,
};

struct HirBinding {
  HirBindingId id = 0;
  const VariableDecl *variable = nullptr;
  const Parameter *parameter = nullptr;
  const StructuredBindingDecl *structuredSource = nullptr;
  BindingInfo info;
};

struct HirValue {
  HirValueId id = 0;
  HirValueKind kind = HirValueKind::Literal;
  const Expr *source = nullptr;
  ExpressionInfo info;
  UnsafeOperationKind unsafeOperation = UnsafeOperationKind::None;
  SymbolId symbol = 0;
  std::vector<HirValueId> operands;
  std::vector<SemanticType> parameterTypes;
  std::optional<TokenKind> operation;
  std::optional<Literal> literal;
  std::optional<ConstantValue> constant;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowArgument = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  bool storedReferenceAccess = false;
  CallDispatch dispatch = CallDispatch::Static;
  SemanticType dispatchOwner = SemanticType::Unknown;
  std::optional<HirValueId> receiver;
  std::optional<HirFunctionInstanceId> functionTarget;
  std::optional<HirFunctionInstanceId> contextualBoolTarget;
  std::optional<HirConstructorInstanceId> constructorTarget;
  ConstructorKind constructorKind = ConstructorKind::Ordinary;
  std::optional<HirLambdaId> lambdaTarget;
  std::vector<std::size_t> nonEscapingArguments;
  bool nonEscapingCallable = false;
  std::optional<EnumId> enumOwner;
  std::optional<EnumConstant> enumValue;
};

struct HirSwitchLabel {
  const SwitchLabel *source = nullptr;
  bool isDefault = false;
  std::optional<HirValueId> value;
  std::optional<SwitchCaseValue> constant;
};

struct HirSwitchArm {
  std::vector<HirSwitchLabel> labels;
  std::vector<HirStatementId> statements;
  std::vector<SemanticLoanId> entryEndedLoans;
};

enum class HirStructuredBindingProjectionKind {
  Field,
  ArrayElement,
};

struct HirStructuredBindingElement {
  HirBindingId binding = 0;
  HirStructuredBindingProjectionKind projection =
      HirStructuredBindingProjectionKind::Field;
  SymbolId field = 0;
  std::optional<HirValueId> index;
};

struct HirStatement {
  HirStatementId id = 0;
  HirStatementKind kind = HirStatementKind::Empty;
  const Stmt *source = nullptr;
  bool unsafeBlock = false;
  std::optional<HirBindingId> binding;
  std::optional<HirValueId> value;
  std::optional<HirValueId> condition;
  std::optional<HirValueId> increment;
  std::optional<HirStatementId> initializer;
  std::optional<HirStatementId> body;
  std::optional<HirStatementId> elseBranch;
  std::vector<HirStatementId> statements;
  std::vector<HirSwitchArm> switchArms;
  std::vector<HirStructuredBindingElement> structuredBindings;
  std::vector<SemanticLoanId> endedLoans;
  std::vector<SemanticLoanId> thenEntryEndedLoans;
  std::vector<SemanticLoanId> elseEntryEndedLoans;
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

struct HirLambda {
  HirLambdaId id = 0;
  LambdaId declaration = 0;
  const Lambda *source = nullptr;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<LambdaCaptureInfo> captures;
  SemanticTypeTraits traits{};
  HirBody body;
};

struct HirEnumerator {
  const EnumeratorDecl *source = nullptr;
  EnumConstant value;
};

struct HirEnum {
  EnumId declaration = 0;
  SourceUnitId sourceUnit = 0;
  const EnumDecl *source = nullptr;
  std::string qualifiedName;
  SemanticType underlyingType = SemanticType::Int32;
  std::vector<HirEnumerator> enumerators;
};

struct HirClassField {
  const VariableDecl *declaration = nullptr;
  HirBindingId binding = 0;
  std::optional<HirValueId> initializer;
  BindingInfo info;
};

struct HirBaseInstance {
  HirClassInstanceId instance = 0;
  SemanticType type = SemanticType::Unknown;
  bool interface = false;
};

struct HirClassInstance {
  HirClassInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  ClassId declaration = 0;
  const ClassDecl *source = nullptr;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits;
  ClassKind kind = ClassKind::Class;
  std::vector<HirBaseInstance> bases;
  bool abstract = false;
  bool polymorphic = false;
  std::vector<HirClassField> fields;
  HirBody fieldInitializers;
  std::vector<HirClassField> staticFields;
  HirBody staticFieldInitializers;
  std::optional<HirDestructorInstanceId> destructor;
};

struct HirCallableSignature {
  const Call *source = nullptr;
  SemanticType returnType = SemanticType::Void;
  std::vector<SemanticType> parameterTypes;
  std::optional<HirFunctionInstanceId> functionTarget;
  std::optional<HirLambdaId> lambdaTarget;
};

struct HirCallableForwarding {
  const Call *source = nullptr;
  std::size_t parameterIndex = 0;
  std::optional<HirFunctionInstanceId> functionTarget;
};

struct HirCallableParameter {
  std::size_t parameterIndex = 0;
  SemanticType callableType = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  bool nonEscaping = true;
  std::vector<HirCallableSignature> signatures;
  std::vector<HirCallableForwarding> forwardings;
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
  std::vector<HirBindingId> parameterBindings;
  BorrowOriginKind returnBorrowOrigin = BorrowOriginKind::None;
  std::size_t returnBorrowParameter = 0;
  AccessMode returnBorrowAccess = AccessMode::ReadOnly;
  HirBody body;
  std::optional<SourceSpan> instantiationSite;
  bool staticMember = false;
  bool internalLinkage = false;
  bool constexprFunction = false;
  LanguageLinkage linkage = LanguageLinkage::Gti;
  std::string externalSymbol;
  bool virtualMethod = false;
  bool pureVirtual = false;
  bool overrideMethod = false;
  std::vector<FunctionId> virtualRoots;
  std::vector<HirCallableParameter> callableParameters;
};

struct HirConstructorInitializer {
  const ConstructorInitializer *source = nullptr;
  ConstructorInitializerTargetKind kind =
      ConstructorInitializerTargetKind::Field;
  SemanticType targetType = SemanticType::Unknown;
  SymbolId field = 0;
  std::optional<HirClassInstanceId> base;
  std::optional<HirConstructorInstanceId> constructorTarget;
  std::vector<HirValueId> arguments;
  bool storesReference = false;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  bool generatedDefault = false;
};

struct HirConstructorInstance {
  HirConstructorInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  ConstructorId declaration = 0;
  const ConstructorDecl *source = nullptr;
  HirClassInstanceId owner = 0;
  std::vector<SemanticType> parameterTypes;
  std::vector<HirBindingId> parameterBindings;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowParameter = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  std::vector<HirConstructorInitializer> initializers;
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

  [[nodiscard]] const std::vector<HirEnum> &enumDeclarations() const {
    return enums;
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

  [[nodiscard]] const std::vector<HirLambda> &lambdaInstances() const {
    return lambdas;
  }

  [[nodiscard]] const HirBody &module() const { return moduleBody; }

  [[nodiscard]] std::size_t valueCount() const {
    std::size_t count = moduleBody.values.size();
    for (const HirClassInstance &classInstance : classes) {
      count += classInstance.fieldInitializers.values.size();
      count += classInstance.staticFieldInitializers.values.size();
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
    for (const HirLambda &lambda : lambdas) {
      count += lambda.body.values.size();
    }
    return count;
  }

  [[nodiscard]] std::size_t statementCount() const {
    std::size_t count = moduleBody.statements.size();
    for (const HirClassInstance &classInstance : classes) {
      count += classInstance.fieldInitializers.statements.size();
      count += classInstance.staticFieldInitializers.statements.size();
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
    for (const HirLambda &lambda : lambdas) {
      count += lambda.body.statements.size();
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

  [[nodiscard]] const HirLambda *findLambda(HirLambdaId id) const {
    return id == 0 || id > lambdas.size() ? nullptr : &lambdas[id - 1];
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
  std::vector<HirEnum> enums;
  std::vector<HirClassInstance> classes;
  std::vector<HirFunctionInstance> functions;
  std::vector<HirConstructorInstance> constructors;
  std::vector<HirDestructorInstance> destructors;
  std::vector<HirLambda> lambdas;
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
    lambdaTargets.clear();

    seedDeclarations(source.declarations(), std::nullopt);
    processPendingInstances();
    output.program.valid_ = output.diagnostics.empty();
    return std::move(output);
  }

private:
  [[nodiscard]] static SemanticType
  substitute(const SemanticType &type,
             const GenericSubstitution &substitution) {
    if (type.kind == SemanticType::TypeParameter) {
      const auto found = substitution.types.find(type.genericParameterId);
      return found == substitution.types.end() ? type : found->second;
    }
    SemanticType result = type;
    for (SemanticType &argument : result.arguments) {
      argument = substitute(argument, substitution);
    }
    for (CompileTimeValue &argument : result.valueArguments) {
      if (argument.kind != CompileTimeValue::Parameter) {
        continue;
      }
      const auto found = substitution.values.find(argument.parameterId);
      if (found != substitution.values.end()) {
        argument = found->second;
      }
    }
    if (result.arrayLengthParameterId != 0) {
      const auto found =
          substitution.values.find(result.arrayLengthParameterId);
      if (found != substitution.values.end() &&
          found->second.kind == CompileTimeValue::UInt64) {
        result.arrayLength = found->second.value;
        result.arrayLengthParameterId = 0;
      }
    }
    return result;
  }

  [[nodiscard]] GenericSubstitution
  classSubstitution(const ClassTypeInfo &declaration,
                    const std::vector<SemanticType> &typeArguments,
                    const std::vector<CompileTimeValue> &valueArguments) const {
    GenericSubstitution result;
    std::size_t typeIndex = 0;
    std::size_t valueIndex = 0;
    for (const GenericParameterInfo &parameter :
         declaration.genericParameters) {
      if (parameter.value) {
        if (valueIndex < valueArguments.size()) {
          result.values.emplace(parameter.id, valueArguments[valueIndex++]);
        }
      } else if (typeIndex < typeArguments.size()) {
        result.types.emplace(parameter.id, typeArguments[typeIndex++]);
      }
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
          instance.typeArguments == type.arguments &&
          instance.valueArguments == type.valueArguments) {
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
                                      .valueArguments = type.valueArguments,
                                      .type = type,
                                      .traits = analyzer->traitsFor(type),
                                      .kind = declaration->kind,
                                      .abstract = declaration->abstract,
                                      .polymorphic = declaration->polymorphic});
    return id;
  }

  [[nodiscard]] HirFunctionInstanceId
  enqueueFunction(const FunctionInfo &declaration,
                  const std::vector<SemanticType> &classTypeArguments,
                  const std::vector<CompileTimeValue> &classValueArguments,
                  std::vector<SemanticType> functionTypeArguments,
                  SemanticType returnType,
                  std::vector<SemanticType> parameterTypes,
                  std::optional<SourceSpan> site = std::nullopt) {
    for (const HirFunctionInstance &instance : output.program.functions) {
      const bool sameOwner =
          declaration.ownerClass == 0
              ? !instance.owner.has_value()
              : instance.owner &&
                    output.program.classes[*instance.owner - 1].typeArguments ==
                        classTypeArguments &&
                    output.program.classes[*instance.owner - 1]
                            .valueArguments == classValueArguments;
      if (instance.declaration == declaration.id && sameOwner &&
          instance.typeArguments == functionTypeArguments) {
        return instance.id;
      }
    }

    std::optional<HirClassInstanceId> owner;
    if (declaration.ownerClass != 0) {
      owner = enqueueClass(SemanticType::classType(
          declaration.ownerClass, classTypeArguments, classValueArguments));
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
         .returnBorrowOrigin = declaration.returnBorrowOrigin,
         .returnBorrowParameter = declaration.returnBorrowParameter,
         .returnBorrowAccess = declaration.returnBorrowAccess,
         .instantiationSite = std::move(site),
         .staticMember = declaration.staticMember,
         .internalLinkage = declaration.internalLinkage,
         .constexprFunction = declaration.constexprFunction,
         .linkage = declaration.linkage,
         .externalSymbol = declaration.externalSymbol,
         .virtualMethod = declaration.virtualMethod,
         .pureVirtual = declaration.pureVirtual,
         .overrideMethod = declaration.overrideMethod,
         .virtualRoots = declaration.virtualRoots});
    return id;
  }

  [[nodiscard]] HirConstructorInstanceId
  enqueueConstructor(const ResolvedConstructionInfo &construction,
                     std::optional<SourceSpan> site = std::nullopt) {
    const std::optional<HirClassInstanceId> owner =
        enqueueClass(construction.constructedType);
    if (!owner || construction.kind != ConstructorKind::Ordinary ||
        construction.declaration == nullptr || construction.constructor == 0) {
      return 0;
    }
    for (HirConstructorInstance &instance : output.program.constructors) {
      if (instance.declaration == construction.constructor &&
          instance.owner == *owner) {
        if (construction.borrowOrigin != BorrowOriginKind::None) {
          instance.borrowOrigin = construction.borrowOrigin;
          instance.borrowParameter = construction.borrowArgument;
          instance.borrowAccess = construction.borrowAccess;
        }
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
         .borrowOrigin = construction.borrowOrigin,
         .borrowParameter = construction.borrowArgument,
         .borrowAccess = construction.borrowAccess,
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
      if (const auto *externC =
              dynamic_cast<const ExternCDecl *>(statement.get())) {
        seedDeclarations(externC->declarations(), enclosingClass);
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
      if (const auto *enumDeclaration =
              dynamic_cast<const EnumDecl *>(statement.get())) {
        const EnumTypeInfo *info = baseModel->findEnumType(*enumDeclaration);
        if (info != nullptr) {
          HirEnum lowered{.declaration = info->id,
                          .sourceUnit = info->sourceUnit,
                          .source = enumDeclaration,
                          .qualifiedName = info->qualifiedName,
                          .underlyingType = info->underlyingType};
          lowered.enumerators.reserve(info->enumerators.size());
          for (const EnumeratorInfo &enumerator : info->enumerators) {
            lowered.enumerators.push_back(
                {.source = enumerator.declaration, .value = enumerator.value});
          }
          output.program.enums.push_back(std::move(lowered));
        }
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
        (void)enqueueFunction(*info, classArguments, {}, {}, info->returnType,
                              info->parameterTypes);
        continue;
      }
      if (const auto *variable =
              dynamic_cast<const VariableDecl *>(statement.get())) {
        if (!enclosingClass) {
          if (const std::optional<HirStatementId> root = lowerStatement(
                  variable, *baseModel, {}, {}, output.program.moduleBody)) {
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
    lambdaTargets.clear();
    const HirClassInstance snapshot = output.program.classes[index];
    const ClassTypeInfo *declaration =
        baseModel->findClassType(snapshot.declaration);
    if (declaration == nullptr) {
      return;
    }
    const GenericSubstitution substitution = classSubstitution(
        *declaration, snapshot.typeArguments, snapshot.valueArguments);
    std::vector<HirBaseInstance> bases;
    bases.reserve(declaration->bases.size());
    for (const ClassBaseTypeInfo &base : declaration->bases) {
      const SemanticType type = substitute(base.type, substitution);
      const std::optional<HirClassInstanceId> instance = enqueueClass(type);
      bases.push_back({.instance = instance.value_or(0),
                       .type = type,
                       .interface = base.interface});
    }
    output.program.classes[index].bases = std::move(bases);
    SemanticInstanceAnalysis analysis;
    const SemanticModel *model = baseModel;
    if (!snapshot.typeArguments.empty() || !snapshot.valueArguments.empty()) {
      analysis = analyzer->analyzeClassFieldInitializers(
          snapshot.declaration, snapshot.typeArguments,
          snapshot.valueArguments);
      appendInstanceDiagnostics(std::move(analysis.diagnostics), std::nullopt);
      model = &analysis.model;
    }
    std::vector<HirClassField> fields;
    HirBody fieldInitializers;
    fields.reserve(declaration->fields.size());
    for (const ClassFieldTypeInfo &field : declaration->fields) {
      const SemanticType type = substitute(field.type, substitution);
      (void)enqueueClass(type);
      BindingInfo info{.type = type,
                       .access = field.declaration != nullptr &&
                                         field.declaration->isMutable()
                                     ? AccessMode::Mutable
                                     : AccessMode::ReadOnly,
                       .traits = analyzer->traitsFor(type)};
      if (field.declaration != nullptr) {
        if (const BindingInfo *recorded =
                model->findBinding(*field.declaration)) {
          info.symbol = recorded->symbol;
          info.constant = recorded->constant;
        }
      }
      const HirBindingId binding =
          field.declaration == nullptr
              ? 0
              : lowerBinding(*field.declaration, info, fieldInitializers);
      const std::optional<HirValueId> initializer =
          field.declaration == nullptr
              ? std::nullopt
              : lowerExpression(field.declaration->initializer(), *model,
                                snapshot.typeArguments, snapshot.valueArguments,
                                fieldInitializers);
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
    std::vector<HirClassField> staticFields;
    HirBody staticFieldInitializers;
    staticFields.reserve(declaration->staticFields.size());
    for (const ClassFieldTypeInfo &field : declaration->staticFields) {
      const SemanticType type = substitute(field.type, substitution);
      (void)enqueueClass(type);
      BindingInfo info{.type = type,
                       .access = field.declaration != nullptr &&
                                         field.declaration->isMutable()
                                     ? AccessMode::Mutable
                                     : AccessMode::ReadOnly,
                       .traits = analyzer->traitsFor(type),
                       .staticStorage = true};
      if (field.declaration != nullptr) {
        if (const BindingInfo *recorded =
                model->findBinding(*field.declaration)) {
          info.symbol = recorded->symbol;
          info.constant = recorded->constant;
        }
      }
      const HirBindingId binding =
          field.declaration == nullptr
              ? 0
              : lowerBinding(*field.declaration, info, staticFieldInitializers);
      const std::optional<HirValueId> initializer =
          field.declaration == nullptr
              ? std::nullopt
              : lowerExpression(field.declaration->initializer(), *model,
                                snapshot.typeArguments, snapshot.valueArguments,
                                staticFieldInitializers);
      if (field.declaration != nullptr) {
        HirStatement statement{.kind = HirStatementKind::Variable,
                               .source = field.declaration,
                               .binding = binding,
                               .value = initializer};
        staticFieldInitializers.roots.push_back(
            appendStatement(std::move(statement), staticFieldInitializers));
      }
      staticFields.push_back({.declaration = field.declaration,
                              .binding = binding,
                              .initializer = initializer,
                              .info = info});
    }
    output.program.classes[index].staticFields = std::move(staticFields);
    output.program.classes[index].staticFieldInitializers =
        std::move(staticFieldInitializers);
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
    std::vector<CompileTimeValue> classValueArguments;
    if (snapshot.owner) {
      const HirClassInstance &owner =
          output.program.classes[*snapshot.owner - 1];
      classArguments = owner.typeArguments;
      classValueArguments = owner.valueArguments;
    }
    const SemanticType enclosingReceiverType = currentReceiverType;
    const AccessMode enclosingReceiverAccess = currentReceiverAccess;
    if (snapshot.owner) {
      currentReceiverType = output.program.classes[*snapshot.owner - 1].type;
      currentReceiverAccess = declaration->declaration->receiverMutability() ==
                                      ReceiverMutability::Mutable
                                  ? AccessMode::Mutable
                                  : AccessMode::ReadOnly;
    }

    const bool concreteInstance = !classArguments.empty() ||
                                  !classValueArguments.empty() ||
                                  !snapshot.typeArguments.empty();
    SemanticInstanceAnalysis analysis;
    const SemanticModel *model = baseModel;
    if (concreteInstance) {
      analysis = analyzer->analyzeFunctionInstance(
          declaration->id, classArguments, classValueArguments,
          snapshot.typeArguments);
      appendInstanceDiagnostics(std::move(analysis.diagnostics),
                                snapshot.instantiationSite);
      model = &analysis.model;
    }

    lambdaTargets.clear();
    HirBody body;
    std::vector<HirBindingId> parameterBindings;
    parameterBindings.reserve(declaration->declaration->parameters().size());
    for (const Parameter &parameter : declaration->declaration->parameters()) {
      parameterBindings.push_back(lowerBinding(parameter, *model, body));
    }
    if (declaration->declaration->body()) {
      body.roots =
          lowerStatements(declaration->declaration->body()->statements(),
                          *model, classArguments, classValueArguments, body);
    }
    std::vector<HirCallableParameter> callableParameters;
    callableParameters.reserve(declaration->callableParameters.size());
    for (const CallableParameterContract &parameter :
         declaration->callableParameters) {
      HirCallableParameter lowered{
          .parameterIndex = parameter.parameterIndex,
          .callableType =
              parameter.parameterIndex < snapshot.parameterTypes.size()
                  ? snapshot.parameterTypes[parameter.parameterIndex]
                  : SemanticType::Unknown,
          .access = parameter.access,
          .nonEscaping = parameter.nonEscaping};
      lowered.signatures.reserve(parameter.signatures.size());
      for (const CallableSignatureRequirement &signature :
           parameter.signatures) {
        HirCallableSignature concrete{.source = signature.source,
                                      .returnType = signature.returnType,
                                      .parameterTypes =
                                          signature.parameterTypes};
        if (signature.source != nullptr) {
          if (const ResolvedLambdaCallInfo *resolved =
                  model->findLambdaCall(*signature.source)) {
            concrete.returnType = resolved->returnType;
            concrete.parameterTypes = resolved->parameterTypes;
          } else if (const ResolvedOperatorInfo *resolved =
                         model->findOperator(*signature.source)) {
            concrete.returnType = resolved->returnType;
            concrete.parameterTypes = resolved->parameterTypes;
          }
          const auto value =
              std::find_if(body.values.begin(), body.values.end(),
                           [&](const HirValue &candidate) {
                             return candidate.source == signature.source;
                           });
          if (value != body.values.end()) {
            concrete.functionTarget = value->functionTarget;
            concrete.lambdaTarget = value->lambdaTarget;
          }
        }
        lowered.signatures.emplace_back(std::move(concrete));
      }
      lowered.forwardings.reserve(parameter.forwardings.size());
      for (const CallableForwardingRequirement &forwarding :
           parameter.forwardings) {
        HirCallableForwarding concrete{.source = forwarding.source,
                                       .parameterIndex =
                                           forwarding.parameterIndex};
        if (forwarding.source != nullptr) {
          const auto value =
              std::find_if(body.values.begin(), body.values.end(),
                           [&](const HirValue &candidate) {
                             return candidate.source == forwarding.source;
                           });
          if (value != body.values.end()) {
            concrete.functionTarget = value->functionTarget;
          }
        }
        lowered.forwardings.emplace_back(std::move(concrete));
      }
      callableParameters.emplace_back(std::move(lowered));
    }
    output.program.functions[index].body = std::move(body);
    output.program.functions[index].parameterBindings =
        std::move(parameterBindings);
    output.program.functions[index].callableParameters =
        std::move(callableParameters);
    currentReceiverType = enclosingReceiverType;
    currentReceiverAccess = enclosingReceiverAccess;
  }

  void processConstructor(std::size_t index) {
    const HirConstructorInstance snapshot = output.program.constructors[index];
    const HirClassInstance owner = output.program.classes[snapshot.owner - 1];
    const std::vector<SemanticType> &classArguments = owner.typeArguments;
    const std::vector<CompileTimeValue> &classValueArguments =
        owner.valueArguments;
    const SemanticType enclosingReceiverType = currentReceiverType;
    const AccessMode enclosingReceiverAccess = currentReceiverAccess;
    currentReceiverType = owner.type;
    currentReceiverAccess = AccessMode::Mutable;
    SemanticInstanceAnalysis analysis = analyzer->analyzeConstructorInstance(
        snapshot.declaration, classArguments, classValueArguments);
    appendInstanceDiagnostics(std::move(analysis.diagnostics),
                              snapshot.instantiationSite);

    lambdaTargets.clear();
    HirBody body;
    std::vector<HirBindingId> parameterBindings;
    parameterBindings.reserve(snapshot.source->parameters().size());
    for (const Parameter &parameter : snapshot.source->parameters()) {
      parameterBindings.push_back(
          lowerBinding(parameter, analysis.model, body));
    }
    std::vector<HirConstructorInitializer> initializers;
    std::vector<HirValueId> initializerValues;
    bool hasExplicitBase = false;
    for (const ConstructorInitializer &initializer :
         snapshot.source->initializers()) {
      HirConstructorInitializer lowered{.source = &initializer};
      const ResolvedConstructorInitializerInfo *resolved =
          analysis.model.findConstructorInitializer(initializer);
      if (resolved != nullptr) {
        lowered.kind = resolved->kind;
        lowered.targetType = resolved->targetType;
        lowered.field = resolved->field;
        lowered.storesReference = resolved->storesReference;
        lowered.borrowAccess = resolved->borrowAccess;
        lowered.generatedDefault = resolved->generatedDefault;
        if (resolved->kind == ConstructorInitializerTargetKind::Base) {
          hasExplicitBase = true;
          lowered.base = enqueueClass(resolved->targetType);
          const HirConstructorInstanceId target =
              enqueueConstructor(ResolvedConstructionInfo{
                  .constructor = resolved->constructor,
                  .declaration = resolved->declaration,
                  .constructedType = resolved->targetType,
                  .parameterTypes = resolved->parameterTypes,
                  .generatedDefault = resolved->generatedDefault});
          if (target != 0) {
            lowered.constructorTarget = target;
          }
        }
      }
      for (const ExprPtr &argument : initializer.arguments) {
        if (const std::optional<HirValueId> value =
                lowerExpression(argument, analysis.model, classArguments,
                                classValueArguments, body)) {
          lowered.arguments.push_back(*value);
          initializerValues.push_back(*value);
        }
      }
      initializers.push_back(std::move(lowered));
    }
    if (!hasExplicitBase) {
      const auto base = std::find_if(owner.bases.begin(), owner.bases.end(),
                                     [](const HirBaseInstance &candidate) {
                                       return !candidate.interface;
                                     });
      if (base != owner.bases.end()) {
        HirConstructorInitializer lowered{
            .kind = ConstructorInitializerTargetKind::Base,
            .targetType = base->type,
            .base = base->instance == 0
                        ? std::nullopt
                        : std::optional<HirClassInstanceId>(base->instance)};
        const ClassTypeInfo *baseType =
            baseModel->findClassType(base->type.classId);
        const ClassLifecycleInfo *lifecycle =
            baseType == nullptr || baseType->declaration == nullptr
                ? nullptr
                : baseModel->findClassLifecycle(*baseType->declaration);
        if (lifecycle != nullptr) {
          const auto declared = std::find_if(
              lifecycle->constructors.begin(), lifecycle->constructors.end(),
              [](const ConstructorInfo &candidate) {
                return candidate.parameterTypes.empty();
              });
          const bool generated = declared == lifecycle->constructors.end();
          lowered.generatedDefault = generated;
          const HirConstructorInstanceId target =
              enqueueConstructor(ResolvedConstructionInfo{
                  .constructor = generated ? 0 : declared->id,
                  .declaration = generated ? nullptr : declared->declaration,
                  .constructedType = base->type,
                  .generatedDefault = generated});
          if (target != 0) {
            lowered.constructorTarget = target;
          }
        }
        initializers.push_back(std::move(lowered));
      }
    }
    body.roots =
        lowerStatements(snapshot.source->body()->statements(), analysis.model,
                        classArguments, classValueArguments, body);
    output.program.constructors[index].initializerValues =
        std::move(initializerValues);
    output.program.constructors[index].parameterBindings =
        std::move(parameterBindings);
    output.program.constructors[index].initializers = std::move(initializers);
    output.program.constructors[index].body = std::move(body);
    currentReceiverType = enclosingReceiverType;
    currentReceiverAccess = enclosingReceiverAccess;
  }

  void processDestructor(std::size_t index) {
    const HirDestructorInstance snapshot = output.program.destructors[index];
    const HirClassInstance &owner = output.program.classes[snapshot.owner - 1];
    const SemanticType enclosingReceiverType = currentReceiverType;
    const AccessMode enclosingReceiverAccess = currentReceiverAccess;
    currentReceiverType = owner.type;
    currentReceiverAccess = AccessMode::Mutable;
    SemanticInstanceAnalysis analysis;
    const SemanticModel *model = baseModel;
    if (!owner.typeArguments.empty() || !owner.valueArguments.empty()) {
      analysis = analyzer->analyzeDestructorInstance(
          owner.declaration, owner.typeArguments, owner.valueArguments);
      appendInstanceDiagnostics(std::move(analysis.diagnostics), std::nullopt);
      model = &analysis.model;
    }

    lambdaTargets.clear();
    HirBody body;
    body.roots =
        lowerStatements(snapshot.source->body()->statements(), *model,
                        owner.typeArguments, owner.valueArguments, body);
    output.program.destructors[index].body = std::move(body);
    currentReceiverType = enclosingReceiverType;
    currentReceiverAccess = enclosingReceiverAccess;
  }

  [[nodiscard]] HirValueId lowerImplicitReceiver(HirBody &body) {
    const HirValueId id = nextValueId++;
    body.values.push_back(
        {.id = id,
         .kind = HirValueKind::This,
         .info = {.type = currentReceiverType,
                  .category = ValueCategory::Place,
                  .access = currentReceiverAccess,
                  .traits = analyzer->traitsFor(currentReceiverType)}});
    return id;
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

  [[nodiscard]] HirBindingId
  lowerStructuredSource(const StructuredBindingDecl &declaration,
                        const BindingInfo &info, HirBody &body) {
    const HirBindingId id = nextBindingId++;
    body.bindings.push_back(
        {.id = id, .structuredSource = &declaration, .info = info});
    (void)enqueueClass(info.type);
    return id;
  }

  [[nodiscard]] HirValueId lowerStructuredIndex(std::uint64_t index,
                                                HirBody &body) {
    const HirValueId id = nextValueId++;
    body.values.push_back({.id = id,
                           .kind = HirValueKind::Literal,
                           .info = makeExpressionInfo(SemanticType::UInt64),
                           .literal = Literal{index}});
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
                  const std::vector<CompileTimeValue> &classValueArguments,
                  HirBody &body) {
    std::vector<HirStatementId> result;
    for (const StmtPtr &statement : statements) {
      if (const std::optional<HirStatementId> lowered =
              lowerStatement(statement.get(), model, classArguments,
                             classValueArguments, body)) {
        result.push_back(*lowered);
      }
    }
    return result;
  }

  [[nodiscard]] std::optional<HirStatementId>
  lowerStatement(const Stmt *statement, const SemanticModel &model,
                 const std::vector<SemanticType> &classArguments,
                 const std::vector<CompileTimeValue> &classValueArguments,
                 HirBody &body) {
    const std::optional<HirStatementId> lowered = lowerStatementImpl(
        statement, model, classArguments, classValueArguments, body);
    if (lowered && statement != nullptr && !body.statements.empty() &&
        body.statements.back().id == *lowered) {
      body.statements.back().endedLoans = model.loansEndingAfter(*statement);
    }
    return lowered;
  }

  [[nodiscard]] std::optional<HirStatementId>
  lowerStatementImpl(const Stmt *statement, const SemanticModel &model,
                     const std::vector<SemanticType> &classArguments,
                     const std::vector<CompileTimeValue> &classValueArguments,
                     HirBody &body) {
    if (statement == nullptr) {
      return std::nullopt;
    }
    if (const auto *block = dynamic_cast<const BlockStmt *>(statement)) {
      return appendStatement({.kind = HirStatementKind::Block,
                              .source = statement,
                              .unsafeBlock = block->isUnsafe(),
                              .statements = lowerStatements(
                                  block->statements(), model, classArguments,
                                  classValueArguments, body)},
                             body);
    }
    if (const auto *conditional =
            dynamic_cast<const ConditionalStmt *>(statement)) {
      std::vector<HirStatementId> statements;
      if (const StmtList *branch = conditional->activeBranch(target)) {
        statements = lowerStatements(*branch, model, classArguments,
                                     classValueArguments, body);
      }
      return appendStatement({.kind = HirStatementKind::CompileTimeBranch,
                              .source = statement,
                              .statements = std::move(statements)},
                             body);
    }
    if (dynamic_cast<const CompileErrorDirective *>(statement) != nullptr) {
      return std::nullopt;
    }
    if (const auto *expression =
            dynamic_cast<const ExpressionStmt *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::Expression,
           .source = statement,
           .value = lowerExpression(expression->expression(), model,
                                    classArguments, classValueArguments, body)},
          body);
    }
    if (const auto *forStatement = dynamic_cast<const ForStmt *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::For,
           .source = statement,
           .condition =
               lowerExpression(forStatement->condition(), model, classArguments,
                               classValueArguments, body),
           .increment =
               lowerExpression(forStatement->increment(), model, classArguments,
                               classValueArguments, body),
           .initializer =
               lowerStatement(forStatement->initializer().get(), model,
                              classArguments, classValueArguments, body),
           .body = lowerStatement(forStatement->body().get(), model,
                                  classArguments, classValueArguments, body)},
          body);
    }
    if (const auto *rangeFor = dynamic_cast<const RangeForStmt *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::RangeFor,
           .source = statement,
           .body = lowerStatement(rangeFor->lowered().get(), model,
                                  classArguments, classValueArguments, body)},
          body);
    }
    if (const auto *ifStatement = dynamic_cast<const IfStmt *>(statement)) {
      if (ifStatement->isConstexpr()) {
        std::vector<HirStatementId> statements;
        if (const std::optional<bool> selected =
                model.findConstexprBranch(*ifStatement)) {
          const StmtPtr &branch =
              *selected ? ifStatement->thenBranch() : ifStatement->elseBranch();
          if (const std::optional<HirStatementId> lowered =
                  lowerStatement(branch.get(), model, classArguments,
                                 classValueArguments, body)) {
            statements.push_back(*lowered);
          }
        }
        return appendStatement({.kind = HirStatementKind::CompileTimeBranch,
                                .source = statement,
                                .statements = std::move(statements)},
                               body);
      }
      return appendStatement(
          {.kind = HirStatementKind::If,
           .source = statement,
           .condition =
               lowerExpression(ifStatement->condition(), model, classArguments,
                               classValueArguments, body),
           .body = lowerStatement(ifStatement->thenBranch().get(), model,
                                  classArguments, classValueArguments, body),
           .elseBranch =
               lowerStatement(ifStatement->elseBranch().get(), model,
                              classArguments, classValueArguments, body),
           .thenEntryEndedLoans =
               model.loansEndingAtConditionalEntry(*ifStatement, true),
           .elseEntryEndedLoans =
               model.loansEndingAtConditionalEntry(*ifStatement, false)},
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
                                    classArguments, classValueArguments, body)},
          body);
    }
    if (const auto *switchStatement =
            dynamic_cast<const SwitchStmt *>(statement)) {
      const std::optional<HirValueId> subject =
          lowerExpression(switchStatement->expression(), model, classArguments,
                          classValueArguments, body);
      std::vector<HirSwitchArm> arms;
      arms.reserve(switchStatement->arms().size());
      for (std::size_t armIndex = 0; armIndex < switchStatement->arms().size();
           ++armIndex) {
        const SwitchArm &arm = switchStatement->arms()[armIndex];
        HirSwitchArm loweredArm;
        loweredArm.labels.reserve(arm.labels.size());
        for (const SwitchLabel &label : arm.labels) {
          std::optional<HirValueId> value = lowerExpression(
              label.value, model, classArguments, classValueArguments, body);
          const SwitchCaseValue *constant =
              label.value ? model.findSwitchCase(*label.value) : nullptr;
          loweredArm.labels.push_back(
              {.source = &label,
               .isDefault = label.isDefault(),
               .value = value,
               .constant = constant == nullptr
                               ? std::nullopt
                               : std::optional<SwitchCaseValue>{*constant}});
        }
        loweredArm.statements = lowerStatements(
            arm.statements, model, classArguments, classValueArguments, body);
        loweredArm.entryEndedLoans =
            model.loansEndingAtSwitchArmEntry(*switchStatement, armIndex);
        arms.push_back(std::move(loweredArm));
      }
      return appendStatement({.kind = HirStatementKind::Switch,
                              .source = statement,
                              .value = subject,
                              .switchArms = std::move(arms)},
                             body);
    }
    if (const auto *structured =
            dynamic_cast<const StructuredBindingDecl *>(statement)) {
      const StructuredBindingInfo *info =
          model.findStructuredBinding(*structured);
      const BindingInfo sourceInfo =
          info == nullptr ? makeBindingInfo(SemanticType::Unknown)
                          : info->source;
      const HirBindingId source =
          lowerStructuredSource(*structured, sourceInfo, body);
      const std::optional<HirValueId> value =
          lowerExpression(structured->initializer(), model, classArguments,
                          classValueArguments, body);
      std::vector<HirStructuredBindingElement> bindings;
      if (info != nullptr) {
        bindings.reserve(info->elements.size());
        for (const StructuredBindingElementInfo &element : info->elements) {
          if (element.declaration == nullptr) {
            continue;
          }
          HirStructuredBindingElement lowered{
              .binding =
                  lowerBinding(*element.declaration, element.binding, body),
              .projection =
                  element.projection ==
                          StructuredBindingProjectionKind::ArrayElement
                      ? HirStructuredBindingProjectionKind::ArrayElement
                      : HirStructuredBindingProjectionKind::Field,
              .field = element.field};
          if (element.projection ==
              StructuredBindingProjectionKind::ArrayElement) {
            lowered.index = lowerStructuredIndex(element.index, body);
          }
          bindings.emplace_back(std::move(lowered));
        }
      }
      return appendStatement({.kind = HirStatementKind::StructuredBinding,
                              .source = statement,
                              .binding = source,
                              .value = value,
                              .structuredBindings = std::move(bindings)},
                             body);
    }
    if (const auto *variable = dynamic_cast<const VariableDecl *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::Variable,
           .source = statement,
           .binding = lowerBinding(*variable, model, body),
           .value = lowerExpression(variable->initializer(), model,
                                    classArguments, classValueArguments, body)},
          body);
    }
    if (const auto *doWhile = dynamic_cast<const DoWhileStmt *>(statement)) {
      const std::optional<HirStatementId> loweredBody =
          lowerStatement(doWhile->body().get(), model, classArguments,
                         classValueArguments, body);
      const std::optional<HirValueId> loweredCondition =
          lowerExpression(doWhile->condition(), model, classArguments,
                          classValueArguments, body);
      return appendStatement({.kind = HirStatementKind::DoWhile,
                              .source = statement,
                              .condition = loweredCondition,
                              .body = loweredBody},
                             body);
    }
    if (const auto *whileStatement =
            dynamic_cast<const WhileStmt *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::While,
           .source = statement,
           .condition =
               lowerExpression(whileStatement->condition(), model,
                               classArguments, classValueArguments, body),
           .body = lowerStatement(whileStatement->body().get(), model,
                                  classArguments, classValueArguments, body)},
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
      const SemanticType &dispatchOwner, const SemanticModel &model,
      const std::vector<SemanticType> &currentClassArguments) const {
    if (target.ownerClass == 0) {
      return {};
    }
    if (dispatchOwner.kind == SemanticType::Class &&
        dispatchOwner.classId == target.ownerClass) {
      return dispatchOwner.arguments;
    }
    if (const auto *member = dynamic_cast<const Get *>(callee.get())) {
      SemanticType receiver = model.typeOf(*member->object());
      if (receiver.kind == SemanticType::RawPointer &&
          receiver.arguments.size() == 1) {
        receiver = receiver.arguments.front();
      }
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

  [[nodiscard]] std::vector<CompileTimeValue> receiverClassValueArguments(
      const ExprPtr &callee, const FunctionInfo &target,
      const SemanticType &dispatchOwner, const SemanticModel &model,
      const std::vector<CompileTimeValue> &currentClassArguments) const {
    if (target.ownerClass == 0) {
      return {};
    }
    if (dispatchOwner.kind == SemanticType::Class &&
        dispatchOwner.classId == target.ownerClass) {
      return dispatchOwner.valueArguments;
    }
    if (const auto *member = dynamic_cast<const Get *>(callee.get())) {
      SemanticType receiver = model.typeOf(*member->object());
      if (receiver.kind == SemanticType::RawPointer &&
          receiver.arguments.size() == 1) {
        receiver = receiver.arguments.front();
      }
      if (receiver.kind == SemanticType::Class &&
          receiver.classId == target.ownerClass) {
        return receiver.valueArguments;
      }
      if (const ResolvedOperatorInfo *arrow = model.findOperator(*member);
          arrow != nullptr &&
          arrow->returnType.kind == SemanticType::Reference &&
          !arrow->returnType.arguments.empty()) {
        receiver = arrow->returnType.arguments.front();
        if (receiver.kind == SemanticType::Class &&
            receiver.classId == target.ownerClass) {
          return receiver.valueArguments;
        }
      }
    }
    return currentClassArguments;
  }

  [[nodiscard]] HirLambdaId
  lowerLambda(const Lambda &lambda, const SemanticModel &model,
              const std::vector<SemanticType> &classArguments,
              const std::vector<CompileTimeValue> &classValueArguments) {
    const LambdaInfo *info = model.findLambda(lambda);
    if (info == nullptr) {
      return 0;
    }
    if (const auto found = lambdaTargets.find(info->id);
        found != lambdaTargets.end()) {
      return found->second;
    }

    const HirLambdaId id = output.program.lambdas.size() + 1;
    lambdaTargets.emplace(info->id, id);
    output.program.lambdas.push_back({.id = id});

    (void)enqueueClass(info->returnType);
    for (const SemanticType &parameterType : info->parameterTypes) {
      (void)enqueueClass(parameterType);
    }
    for (const LambdaCaptureInfo &capture : info->captures) {
      (void)enqueueClass(capture.type);
    }

    HirBody body;
    for (const Parameter &parameter : lambda.parameters()) {
      (void)lowerBinding(parameter, model, body);
    }
    body.roots = lowerStatements(lambda.body(), model, classArguments,
                                 classValueArguments, body);
    output.program.lambdas[id - 1] = {
        .id = id,
        .declaration = info->id,
        .source = &lambda,
        .returnType = info->returnType,
        .parameterTypes = info->parameterTypes,
        .captures = info->captures,
        .traits = info->traits,
        .body = std::move(body),
    };
    return id;
  }

  [[nodiscard]] std::optional<HirValueId>
  lowerExpression(const ExprPtr &expression, const SemanticModel &model,
                  const std::vector<SemanticType> &classArguments,
                  const std::vector<CompileTimeValue> &classValueArguments,
                  HirBody &body) {
    if (!expression) {
      return std::nullopt;
    }
    const Expr *raw = expression.get();
    HirValueKind kind = HirValueKind::Literal;
    std::vector<HirValueId> operands;
    std::optional<TokenKind> operation;
    std::optional<Literal> literal;
    std::optional<HirValueId> receiver;
    std::optional<HirLambdaId> lambdaTarget;
    std::optional<EnumId> enumOwner;
    std::optional<EnumConstant> enumValue;
    const auto lowerOperand = [&](const ExprPtr &operand) {
      if (const std::optional<HirValueId> id = lowerExpression(
              operand, model, classArguments, classValueArguments, body)) {
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
      const ResolvedCallInfo *resolved = model.findCall(*call);
      kind = resolved != nullptr && resolved->intrinsic == IntrinsicKind::Move
                 ? HirValueKind::Move
                 : HirValueKind::Call;
      if (kind == HirValueKind::Call) {
        lowerOperand(call->callee());
        if (resolved != nullptr && resolved->declaration != nullptr &&
            !resolved->declaration->isStatic() &&
            dynamic_cast<const Variable *>(call->callee().get()) != nullptr &&
            currentReceiverType.kind == SemanticType::Class) {
          receiver = lowerImplicitReceiver(body);
        }
      }
      for (const ExprPtr &argument : call->arguments()) {
        lowerOperand(argument);
      }
    } else if (const auto *conditional =
                   dynamic_cast<const ConditionalExpr *>(raw)) {
      kind = HirValueKind::Conditional;
      lowerOperand(conditional->condition());
      lowerOperand(conditional->thenExpression());
      lowerOperand(conditional->elseExpression());
    } else if (const auto *conversion = dynamic_cast<const Conversion *>(raw)) {
      kind = HirValueKind::Conversion;
      lowerOperand(conversion->value());
    } else if (const auto *initializer =
                   dynamic_cast<const DirectInitializer *>(raw)) {
      kind = HirValueKind::DirectInitializer;
      for (const ExprPtr &argument : initializer->arguments()) {
        lowerOperand(argument);
      }
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
    } else if (const auto *lambda = dynamic_cast<const Lambda *>(raw)) {
      kind = HirValueKind::Lambda;
      const HirLambdaId target =
          lowerLambda(*lambda, model, classArguments, classValueArguments);
      if (target != 0) {
        lambdaTarget = target;
      }
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
    } else if (const auto *qualified =
                   dynamic_cast<const QualifiedName *>(raw)) {
      kind = HirValueKind::QualifiedName;
      if (const ResolvedEnumeratorInfo *resolved =
              model.findEnumerator(*qualified)) {
        enumOwner = resolved->owner;
        enumValue = resolved->value;
      }
    } else if (dynamic_cast<const This *>(raw) != nullptr) {
      kind = HirValueKind::This;
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
                   .unsafeOperation = model.unsafeOperation(*raw),
                   .symbol = model.findResolvedSymbol(*raw),
                   .operands = std::move(operands),
                   .operation = operation,
                   .literal = std::move(literal),
                   .constant = model.findConstant(*raw),
                   .receiver = receiver,
                   .lambdaTarget = lambdaTarget,
                   .enumOwner = enumOwner,
                   .enumValue = enumValue};
    if (const ExpressionInfo *info = model.findExpression(*raw)) {
      value.info = *info;
      (void)enqueueClass(info->type);
    }
    if (const auto *call = dynamic_cast<const Call *>(raw)) {
      if (const ResolvedCallInfo *resolved = model.findCall(*call)) {
        value.intrinsic = resolved->intrinsic;
        value.dispatch = resolved->dispatch;
        value.dispatchOwner = resolved->dispatchOwner;
        value.parameterTypes = resolved->parameterTypes;
        value.borrowOrigin = resolved->borrowOrigin;
        value.borrowArgument = resolved->borrowArgument;
        value.borrowAccess = resolved->borrowAccess;
        value.nonEscapingArguments = resolved->nonEscapingArguments;
        if (resolved->intrinsic == IntrinsicKind::None &&
            resolved->function != 0 && resolved->declaration != nullptr) {
          if (const FunctionInfo *target =
                  baseModel->findFunction(resolved->function)) {
            value.functionTarget = enqueueFunction(
                *target,
                receiverClassArguments(call->callee(), *target,
                                       resolved->dispatchOwner, model,
                                       classArguments),
                receiverClassValueArguments(call->callee(), *target,
                                            resolved->dispatchOwner, model,
                                            classValueArguments),
                resolved->typeArguments, resolved->returnType,
                resolved->parameterTypes, tokenSpan(call->paren()));
          }
        }
      }
      if (const ResolvedLambdaCallInfo *resolved =
              model.findLambdaCall(*call)) {
        value.parameterTypes = resolved->parameterTypes;
        value.nonEscapingCallable = resolved->nonEscaping;
        if (const auto target = lambdaTargets.find(resolved->lambda);
            target != lambdaTargets.end()) {
          value.lambdaTarget = target->second;
        } else {
          const auto existing = std::find_if(
              output.program.lambdas.begin(), output.program.lambdas.end(),
              [&](const HirLambda &candidate) {
                return candidate.declaration == resolved->lambda &&
                       candidate.returnType == resolved->returnType &&
                       candidate.parameterTypes == resolved->parameterTypes;
              });
          if (existing != output.program.lambdas.end()) {
            value.lambdaTarget = existing->id;
          }
        }
      }
      if (model.findLambdaCall(*call) == nullptr &&
          model.findOperator(*call) == nullptr) {
        if (const DeferredCallableCallInfo *deferred =
                model.findDeferredCallableCall(*call)) {
          value.parameterTypes = deferred->parameterTypes;
          value.nonEscapingCallable = deferred->nonEscaping;
        }
      }
    }
    if (const ResolvedConstructionInfo *construction =
            model.findConstruction(*raw)) {
      if (value.intrinsic != IntrinsicKind::StorageConstruct) {
        value.parameterTypes = construction->parameterTypes;
      }
      value.borrowOrigin = construction->borrowOrigin;
      value.borrowArgument = construction->borrowArgument;
      value.borrowAccess = construction->borrowAccess;
      value.constructorKind = construction->kind;
      std::optional<SourceSpan> site;
      if (const auto *call = dynamic_cast<const Call *>(raw)) {
        site = tokenSpan(call->paren());
      } else if (const auto *initializer =
                     dynamic_cast<const DirectInitializer *>(raw)) {
        site = tokenSpan(initializer->brace());
      }
      const HirConstructorInstanceId target =
          enqueueConstructor(*construction, std::move(site));
      if (target != 0) {
        value.constructorTarget = target;
      }
    }
    if (kind == HirValueKind::MemberAccess && value.symbol != 0) {
      const SymbolRecord *member = model.database().findSymbol(value.symbol);
      value.storedReferenceAccess =
          member != nullptr && member->type.kind == SemanticType::Reference;
    }
    if (const ResolvedOperatorInfo *resolved = model.findOperator(*raw);
        resolved != nullptr && resolved->function != 0) {
      value.parameterTypes = resolved->parameterTypes;
      value.dispatch = resolved->dispatch;
      value.dispatchOwner = resolved->dispatchOwner;
      value.nonEscapingCallable = resolved->nonEscaping;
      value.borrowOrigin = resolved->borrowOrigin;
      value.borrowArgument = resolved->borrowArgument;
      value.borrowAccess = resolved->borrowAccess;
      if (resolved->kind == OverloadedOperator::Call &&
          value.borrowOrigin == BorrowOriginKind::Receiver &&
          !value.operands.empty()) {
        value.receiver = value.operands.front();
      }
      if (const FunctionInfo *target =
              baseModel->findFunction(resolved->function)) {
        SemanticType receiverType = resolved->dispatchOwner;
        if (const auto *binary = dynamic_cast<const Binary *>(raw)) {
          if (receiverType.kind != SemanticType::Class ||
              receiverType.classId != target->ownerClass) {
            receiverType = model.typeOf(*binary->left());
          }
        } else if (const auto *get = dynamic_cast<const Get *>(raw)) {
          if (receiverType.kind != SemanticType::Class ||
              receiverType.classId != target->ownerClass) {
            receiverType = model.typeOf(*get->object());
          }
        } else if (const auto *index = dynamic_cast<const Index *>(raw)) {
          if (receiverType.kind != SemanticType::Class ||
              receiverType.classId != target->ownerClass) {
            receiverType = model.typeOf(*index->object());
          }
        } else if (const auto *call = dynamic_cast<const Call *>(raw)) {
          if (receiverType.kind != SemanticType::Class ||
              receiverType.classId != target->ownerClass) {
            receiverType = model.typeOf(*call->callee());
          }
        } else if (const auto *unary = dynamic_cast<const Unary *>(raw)) {
          if (receiverType.kind != SemanticType::Class ||
              receiverType.classId != target->ownerClass) {
            receiverType = model.typeOf(*unary->right());
          }
        }
        const std::vector<SemanticType> ownerArguments =
            receiverType.kind == SemanticType::Class ? receiverType.arguments
                                                     : classArguments;
        const std::vector<CompileTimeValue> ownerValueArguments =
            receiverType.kind == SemanticType::Class
                ? receiverType.valueArguments
                : classValueArguments;
        value.functionTarget =
            enqueueFunction(*target, ownerArguments, ownerValueArguments, {},
                            resolved->returnType, resolved->parameterTypes);
      }
    }
    if (const ResolvedOperatorInfo *resolved =
            model.findContextualConversion(*raw);
        resolved != nullptr && resolved->function != 0) {
      value.dispatch = resolved->dispatch;
      value.dispatchOwner = resolved->dispatchOwner;
      value.borrowOrigin = resolved->borrowOrigin;
      value.borrowArgument = resolved->borrowArgument;
      value.borrowAccess = resolved->borrowAccess;
      if (const FunctionInfo *target =
              baseModel->findFunction(resolved->function)) {
        SemanticType receiverType = resolved->dispatchOwner;
        if (receiverType.kind != SemanticType::Class ||
            receiverType.classId != target->ownerClass) {
          receiverType = model.typeOf(*raw);
        }
        const std::vector<SemanticType> ownerArguments =
            receiverType.kind == SemanticType::Class ? receiverType.arguments
                                                     : classArguments;
        const std::vector<CompileTimeValue> ownerValueArguments =
            receiverType.kind == SemanticType::Class
                ? receiverType.valueArguments
                : classValueArguments;
        value.contextualBoolTarget =
            enqueueFunction(*target, ownerArguments, ownerValueArguments, {},
                            resolved->returnType, resolved->parameterTypes);
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
  SemanticType currentReceiverType = SemanticType::Unknown;
  AccessMode currentReceiverAccess = AccessMode::ReadOnly;
  std::unordered_map<LambdaId, HirLambdaId> lambdaTargets;
};

} // namespace lang
