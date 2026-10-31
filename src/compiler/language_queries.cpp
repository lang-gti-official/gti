#include "gti/language_queries.h"

#include "gti/lexer.h"
#include "gti/source_owner.h"
#include "language_snippets.h"
#include "semantic_spelling.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lang {

SignaturePrinter::SignaturePrinter(const SemanticModel &semantics)
    : semantics(semantics), types(semantics) {}

std::string SignaturePrinter::nativeCall(const ResolvedCallInfo &selected,
                                         std::string_view name) const {
  std::string result = "unsafe " + types.print(selected.returnType) + " ";
  result += name;
  result += '(';
  appendParameters(result, selected.parameterTypes, nullptr, false);
  result += ')';
  return result;
}

std::string
SignaturePrinter::forwardedMemberCall(const FunctionInfo &info,
                                      const ResolvedCallInfo &selected,
                                      std::string_view name) const {
  if (selected.argumentReceiver == nullptr || selected.parameterTypes.empty() ||
      selected.parameterTypes.front().kind != SemanticType::Reference ||
      selected.parameterTypes.front().arguments.size() != 1)
    return {};
  auto labels = parameterLabels(
      selected.parameterTypes,
      info.declaration ? &info.declaration->parameters() : nullptr, false);
  std::string result =
      types.print(selected.returnType) + " " +
      types.print(selected.parameterTypes.front().arguments.front()) +
      "::" + std::string(name) + "(";
  for (std::size_t index = 1; index < labels.size(); ++index) {
    if (index != 1)
      result += ", ";
    result += labels[index];
  }
  return result + ")";
}

[[nodiscard]] std::string
SignaturePrinter::function(const FunctionInfo &info,
                           const ResolvedCallInfo *selected) const {
  const FunctionDecl *declaration = info.declaration;
  const SemanticType &returnType =
      selected == nullptr ? info.returnType : selected->returnType;
  const std::vector<SemanticType> &parameterTypes =
      selected == nullptr ? info.parameterTypes : selected->parameterTypes;
  std::string result;
  if (info.neverReturns) {
    result += "[[noreturn]] ";
  }
  if (info.testFunction) {
    result += "[[test]] ";
  }
  if (info.tracksCaller) {
    result += "[[track_caller]] ";
  }
  if (info.discardableResult) {
    result += "[[discardable]] ";
  }
  if (info.threadConfinedCallback) {
    result += "[[thread_confined_callback]] ";
  }
  if (info.explicitReturnBorrowOrigin) {
    if (info.returnBorrowOrigin == BorrowOriginKind::Receiver) {
      result += "[[borrowed_from(this)]] ";
    } else if (info.returnBorrowOrigin == BorrowOriginKind::Argument &&
               declaration != nullptr &&
               info.returnBorrowParameter < declaration->parameters().size()) {
      result +=
          "[[borrowed_from(" +
          declaration->parameters()[info.returnBorrowParameter].name.lexeme +
          ")]] ";
    }
  }
  if (info.requiresUnsafe) {
    result += "unsafe ";
  }
  if (declaration != nullptr && declaration->nativeCArray()) {
    result += "[[c_array(" +
              declaration->nativeCArray()->countParameter.lexeme + ")]] ";
  }
  if (declaration != nullptr && declaration->nativeLinkName()) {
    result += "[[link_name(\"" +
              std::string(declaration->nativeLinkName()->name()) + "\")]] ";
  }
  if (info.declaration != nullptr && info.declaration->isStatic()) {
    result += "static ";
  }
  if (declaration != nullptr && declaration->isConstexpr()) {
    result += declaration->isConsteval() ? "consteval " : "constexpr ";
  }
  const bool conversion =
      declaration != nullptr && declaration->operatorName() &&
      (declaration->operatorName()->kind == OverloadedOperator::Conversion ||
       declaration->operatorName()->kind == OverloadedOperator::ContextualBool);
  if (conversion) {
    // A conversion operator is named by its target; no result type precedes
    // it.
    if (declaration->implicitKeyword()) {
      result += "implicit ";
    }
    result += functionName(info);
  } else if (info.macroExpansion) {
    result += "#macro ";
    if (declaration == nullptr ||
        !declaration->returnType().name.last().generated)
      result += types.print(returnType) + " ";
    result += functionName(info);
  } else {
    result += types.print(returnType) + " " + functionName(info);
  }
  if (selected != nullptr &&
      (!selected->typeArguments.empty() || !selected->valueArguments.empty())) {
    result += '<';
    appendSelectedGenericArguments(result, info.genericParameters,
                                   selected->typeArguments,
                                   selected->valueArguments);
    result += '>';
  } else if (selected == nullptr && !info.genericParameters.empty()) {
    result += '<';
    appendGenericParameters(result, info.genericParameters);
    result += '>';
  }
  result += '(';
  appendParameters(result, parameterTypes,
                   declaration == nullptr ? nullptr
                                          : &declaration->parameters(),
                   selected == nullptr);
  result += ')';
  if (declaration != nullptr && !declaration->isStatic() &&
      declaration->receiverMutability() == ReceiverMutability::Mutable) {
    result += " mut";
  } else if (declaration != nullptr && !declaration->isStatic() &&
             declaration->receiverMutability() ==
                 ReceiverMutability::Consuming) {
    result += " &&";
  }
  appendRequirements(result, info, selected);
  return result;
}

[[nodiscard]]
std::vector<std::string> SignaturePrinter::parameterLabels(
    const std::vector<SemanticType> &parameterTypes,
    const std::vector<Parameter> *parameters, bool preservePackSyntax) const {
  std::vector<std::string> labels;
  labels.reserve(parameterTypes.size());
  for (std::size_t index = 0; index < parameterTypes.size(); ++index) {
    const Parameter *parameter =
        parameters != nullptr && index < parameters->size()
            ? &(*parameters)[index]
            : nullptr;
    std::string label;
    if (parameter != nullptr && parameter->mutability == Mutability::Mutable &&
        !(parameterTypes[index].kind == SemanticType::Reference &&
          parameterTypes[index].referenceAccess == AccessMode::Mutable)) {
      label += "mut ";
    }
    label += types.print(parameterTypes[index]);
    if (preservePackSyntax && parameter != nullptr && parameter->pack &&
        parameterTypes[index].kind != SemanticType::TypePack) {
      label += "...";
    }
    if (parameter != nullptr && !parameter->name.lexeme.empty()) {
      label += " " + parameter->name.lexeme;
    }
    if (parameter != nullptr && parameter->hasDefault()) {
      label += " = <default>";
    }
    labels.push_back(std::move(label));
  }
  return labels;
}

std::string
SignaturePrinter::conceptSignature(const SymbolRecord &symbol,
                                   const ConceptDecl *declaration) const {
  std::string result = "concept " + symbol.qualifiedName;
  if (declaration == nullptr) {
    return result;
  }
  if (declaration->explanation()) {
    result = "[[unsatisfied(" + declaration->explanation()->literal.lexeme +
             ")]] " + result;
  }

  result += '<';
  for (std::size_t index = 0; index < declaration->typeParameters().size();
       ++index) {
    if (index != 0) {
      result += ", ";
    }
    result += declaration->typeParameters()[index].lexeme;
  }
  result += '>';
  if (!declaration->requirements().empty()) {
    result += " = ";
    const ConstraintExpression &expression =
        declaration->constraintExpression();
    std::function<void(std::size_t, int)> appendNode =
        [&](std::size_t nodeIndex, int parentPrecedence) {
          if (nodeIndex >= expression.nodes.size()) {
            return;
          }
          const ConstraintExpressionNode &node = expression.nodes[nodeIndex];
          if (node.kind == ConstraintExpressionKind::Application) {
            if (node.application < expression.applications.size()) {
              appendConceptApplication(
                  result, expression.applications[node.application]);
            }
            return;
          }
          const int precedence =
              node.kind == ConstraintExpressionKind::Conjunction ? 2 : 1;
          const bool parenthesize = precedence < parentPrecedence;
          if (parenthesize) {
            result += '(';
          }
          appendNode(node.left, precedence);
          result += node.kind == ConstraintExpressionKind::Conjunction ? " && "
                                                                       : " || ";
          appendNode(node.right, precedence);
          if (parenthesize) {
            result += ')';
          }
        };
    appendNode(expression.root, 0);
  }
  return result;
}

[[nodiscard]] std::string
SignaturePrinter::constructor(const ClassTypeInfo &owner,
                              const ResolvedConstructionInfo &selected) const {
  const std::string constructedType =
      selected.constructedType.kind == SemanticType::Class
          ? types.print(selected.constructedType)
          : owner.qualifiedName;
  if (selected.kind != ConstructorKind::Ordinary) {
    return constructedType + "(" + constructedType +
           (selected.kind == ConstructorKind::Move ? "&&)" : "&)");
  }
  std::string result =
      selected.declaration != nullptr && selected.declaration->isConstexpr()
          ? std::string(selected.declaration->isConsteval() ? "consteval "
                                                            : "constexpr ") +
                constructedType
          : constructedType;
  if (selected.declaration != nullptr && selected.declaration->unsafeKeyword())
    result = "unsafe " + result;
  if (!selected.typeArguments.empty() || !selected.valueArguments.empty()) {
    const ConstructorInfo *constructor =
        selected.declaration == nullptr
            ? nullptr
            : semantics.findConstructor(*selected.declaration);
    if (constructor != nullptr) {
      result += "::" + selected.declaration->name().lexeme;
      result += '<';
      appendSelectedGenericArguments(result, constructor->genericParameters,
                                     selected.typeArguments,
                                     selected.valueArguments);
      result += '>';
    }
  }
  result += '(';
  const std::vector<Parameter> *parameters =
      selected.declaration == nullptr ? nullptr
                                      : &selected.declaration->parameters();
  appendParameters(result, selected.parameterTypes, parameters, false);
  result += ')';
  if (selected.declaration != nullptr) {
    appendRequirements(result, selected.declaration->requiresClause(),
                       selected.requirements);
  }
  return result;
}

[[nodiscard]] std::string
SignaturePrinter::constructor(const ClassTypeInfo &owner,
                              const ConstructorInfo &info) const {
  const std::string ownerName =
      owner.exactSpecializationPrimary == 0
          ? owner.qualifiedName
          : types.print(SemanticType::classType(owner.id));
  const auto appendSpecifier = [&](std::string result) {
    if (info.declaration != nullptr && info.declaration->specifier()) {
      result += info.declaration->specifier()->kind ==
                        SpecialMemberSpecifierKind::Defaulted
                    ? " = default"
                    : " = delete";
    }
    return result;
  };
  if (info.kind != ConstructorKind::Ordinary) {
    return appendSpecifier(ownerName + "(" + ownerName +
                           (info.kind == ConstructorKind::Move ? "&&)" : "&)"));
  }
  std::string result =
      info.declaration != nullptr && info.declaration->isConstexpr()
          ? std::string(info.declaration->isConsteval() ? "consteval "
                                                        : "constexpr ") +
                ownerName
          : ownerName;
  if (info.requiresUnsafe)
    result = "unsafe " + result;
  if (!info.genericParameters.empty()) {
    result += '<';
    appendGenericParameters(result, info.genericParameters);
    result += '>';
  }
  result += '(';
  appendParameters(
      result, info.parameterTypes,
      info.declaration == nullptr ? nullptr : &info.declaration->parameters(),
      true);
  result += ')';
  if (info.declaration != nullptr) {
    appendRequirements(result, info.declaration->requiresClause(),
                       info.requirements);
  }
  return appendSpecifier(std::move(result));
}

[[nodiscard]] std::string
SignaturePrinter::classType(const ClassTypeInfo &info) const {
  const ClassKind kind =
      info.declaration == nullptr ? ClassKind::Class : info.declaration->kind();
  std::string result = info.cAbiRecord      ? "[[c_abi]]\n"
                       : info.cOpaqueHandle ? "[[c_opaque]]\n"
                                            : "";
  switch (kind) {
  case ClassKind::Class:
    result += "class ";
    break;
  case ClassKind::Struct:
    result += "struct ";
    break;
  case ClassKind::Interface:
    result += "interface ";
    break;
  case ClassKind::Union:
    result += "union ";
    break;
  }
  if (info.exactSpecializationPrimary != 0) {
    result += types.print(SemanticType::classType(info.id));
  } else {
    result += info.qualifiedName;
  }
  if (info.exactSpecializationPrimary == 0 && !info.genericParameters.empty()) {
    result += '<';
    if (info.genericArgumentPatterns.empty()) {
      appendGenericParameters(result, info.genericParameters);
    } else {
      appendTypes(result, info.genericArgumentPatterns);
    }
    result += '>';
  }
  if (info.cOpaqueHandle) {
    result += ';';
  }
  return result;
}

[[nodiscard]] std::string
SignaturePrinter::enumType(const EnumTypeInfo &info) const {
  std::string result = "enum ";
  if (info.declaration != nullptr && info.declaration->classKeyword()) {
    result += "class ";
  }
  result += info.qualifiedName;
  if (!info.genericParameters.empty()) {
    result += '<';
    appendGenericParameters(result, info.genericParameters);
    result += '>';
  }
  if (!info.payload) {
    result += " : " + types.print(info.underlyingType);
  }
  return result;
}

[[nodiscard]] std::string
SignaturePrinter::enumerator(const EnumTypeInfo &owner,
                             std::string_view name) const {
  const std::size_t separator = name.rfind("::");
  const std::string_view unqualified =
      separator == std::string_view::npos ? name : name.substr(separator + 2);
  const auto found =
      std::find_if(owner.enumerators.begin(), owner.enumerators.end(),
                   [&](const EnumeratorInfo &candidate) {
                     return candidate.declaration != nullptr &&
                            candidate.declaration->name.lexeme == unqualified;
                   });
  if (!owner.payload || found == owner.enumerators.end()) {
    return owner.qualifiedName + " " + owner.qualifiedName +
           "::" + std::string(unqualified);
  }
  std::string result = owner.qualifiedName + " " + owner.qualifiedName +
                       "::" + std::string(unqualified);
  if (!found->payloadTypes.empty()) {
    result += '(';
    const std::vector<Parameter> *parameters =
        found->declaration == nullptr ? nullptr : &found->declaration->payload;
    appendParameters(result, found->payloadTypes, parameters, true);
    result += ')';
  }
  return result;
}

[[nodiscard]] std::string
SignaturePrinter::typeAlias(const TypeAliasInfo &info) const {
  return "using " + info.qualifiedName + " = " + types.print(info.type);
}

[[nodiscard]] std::string
SignaturePrinter::binding(const SemanticOccurrence &occurrence) const {
  const bool typeCarriesMutability =
      occurrence.type.kind == SemanticType::Reference &&
      occurrence.type.referenceAccess == AccessMode::Mutable;
  std::string prefix = occurrence.staticMember ? "static " : "";
  if (occurrence.threadLocal)
    prefix += "thread_local ";
  if (occurrence.mutableBinding && !typeCarriesMutability) {
    prefix += "mut ";
  }
  std::string result =
      prefix + types.print(occurrence.type) + " " + occurrence.name;
  if (const auto *storage = semantics.findForeignStorage(occurrence.symbol)) {
    result = "extern \"C\" { " +
             (storage->externalSymbol != occurrence.name
                  ? "[[link_name(\"" + storage->externalSymbol + "\")]] "
                  : std::string()) +
             result + "; }";
  }
  return result;
}

[[nodiscard]] std::string
SignaturePrinter::destructor(const DestructorInfo &info) const {
  const ClassTypeInfo *owner = semantics.findClassType(info.owner);
  if (owner == nullptr || owner->declaration == nullptr) {
    return "destructor";
  }
  const std::string ownerName =
      owner->exactSpecializationPrimary == 0
          ? owner->qualifiedName
          : types.print(SemanticType::classType(owner->id));
  std::string result =
      ownerName + "::~" + owner->declaration->name().lexeme + "()";
  if (info.declaration != nullptr && info.declaration->specifier()) {
    result += info.declaration->specifier()->kind ==
                      SpecialMemberSpecifierKind::Defaulted
                  ? " = default"
                  : " = delete";
  }
  return result;
}

[[nodiscard]] std::string SignaturePrinter::path(const NamePath &name) {
  std::string result;
  for (const Token &segment : name.segments) {
    if (!result.empty()) {
      result += "::";
    }
    result += segment.lexeme;
  }
  return result;
}

[[nodiscard]] std::string
SignaturePrinter::functionName(const FunctionInfo &info) const {
  const ClassTypeInfo *owner =
      info.ownerClass == 0 ? nullptr : semantics.findClassType(info.ownerClass);
  const std::string exactOwner =
      owner != nullptr && owner->exactSpecializationPrimary != 0
          ? types.print(SemanticType::classType(owner->id)) + "::"
          : std::string{};
  if (info.declaration == nullptr || !info.declaration->operatorName()) {
    return exactOwner.empty() ? info.qualifiedName
                              : exactOwner + info.declaration->name().lexeme;
  }
  const std::size_t separator = info.qualifiedName.rfind("::");
  const std::string scope = separator == std::string::npos
                                ? std::string{}
                                : info.qualifiedName.substr(0, separator + 2);
  if (info.declaration->operatorName()->kind ==
      OverloadedOperator::Conversion) {
    return (exactOwner.empty() ? scope : exactOwner) + "operator " +
           types.print(info.returnType);
  }
  return (exactOwner.empty() ? scope : exactOwner) +
         std::string(
             operatorSourceSpelling(info.declaration->operatorName()->kind));
}

void SignaturePrinter::appendTypes(
    std::string &result, const std::vector<SemanticType> &arguments) const {
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (index != 0) {
      result += ", ";
    }
    result += types.print(arguments[index]);
  }
}

void SignaturePrinter::appendSelectedGenericArguments(
    std::string &result, const std::vector<GenericParameterInfo> &parameters,
    const std::vector<SemanticType> &typeArguments,
    const std::vector<CompileTimeValue> &valueArguments) const {
  std::size_t typeIndex = 0;
  std::size_t valueIndex = 0;
  bool first = true;
  for (const GenericParameterInfo &parameter : parameters) {
    if (!first) {
      result += ", ";
    }
    first = false;
    if (!parameter.value) {
      result += typeIndex < typeArguments.size()
                    ? types.print(typeArguments[typeIndex++])
                    : parameter.name.lexeme;
      continue;
    }
    if (valueIndex >= valueArguments.size()) {
      result += parameter.name.lexeme;
      continue;
    }
    const CompileTimeValue &value = valueArguments[valueIndex++];
    if (value.kind == CompileTimeValue::UInt64) {
      result += std::to_string(value.value);
    } else if (value.kind == CompileTimeValue::Parameter) {
      const GenericParameterInfo *resolved =
          semantics.findGenericParameter(value.parameterId);
      result +=
          resolved == nullptr ? parameter.name.lexeme : resolved->name.lexeme;
    } else {
      result += "unknown";
    }
  }
}

void SignaturePrinter::appendGenericParameters(
    std::string &result,
    const std::vector<GenericParameterInfo> &parameters) const {
  const auto appendValueDefault =
      [&](const auto &self, const GenericValueExpression &expression) -> void {
    if (expression.kind == GenericValueExpression::UInt64) {
      result += std::to_string(expression.value);
      return;
    }
    if (expression.kind == GenericValueExpression::Parameter) {
      const GenericParameterInfo *parameter =
          semantics.findGenericParameter(expression.parameterId);
      result += parameter == nullptr ? expression.token.lexeme
                                     : parameter->name.lexeme;
      return;
    }
    result += '(';
    if (expression.left) {
      self(self, *expression.left);
    }
    result += ' ';
    result += expression.token.lexeme;
    result += ' ';
    if (expression.right) {
      self(self, *expression.right);
    }
    result += ')';
  };
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (index != 0) {
      result += ", ";
    }
    const GenericParameterInfo &parameter = parameters[index];
    if (parameter.value) {
      result += "uint64_t ";
    } else if (parameter.constraintName) {
      result += path(*parameter.constraintName) + " ";
    }
    result += parameter.name.lexeme;
    if (parameter.pack) {
      result += "...";
    }
    if (parameter.defaultType) {
      result += " = " + types.print(*parameter.defaultType);
    } else if (parameter.defaultValue) {
      result += " = ";
      appendValueDefault(appendValueDefault, *parameter.defaultValue);
    }
  }
}

void SignaturePrinter::appendParameters(
    std::string &result, const std::vector<SemanticType> &parameterTypes,
    const std::vector<Parameter> *parameters, bool preservePackSyntax) const {
  for (std::size_t index = 0; index < parameterTypes.size(); ++index) {
    if (index != 0) {
      result += ", ";
    }
    const Parameter *parameter =
        parameters != nullptr && index < parameters->size()
            ? &(*parameters)[index]
            : nullptr;
    if (parameter != nullptr && parameter->mutability == Mutability::Mutable &&
        !(parameterTypes[index].kind == SemanticType::Reference &&
          parameterTypes[index].referenceAccess == AccessMode::Mutable)) {
      result += "mut ";
    }
    result += types.print(parameterTypes[index]);
    if (preservePackSyntax && parameter != nullptr && parameter->pack &&
        parameterTypes[index].kind != SemanticType::TypePack) {
      result += "...";
    }
    if (parameter != nullptr && !parameter->name.lexeme.empty()) {
      result += " " + parameter->name.lexeme;
    }
    if (parameter != nullptr && parameter->hasDefault()) {
      result += " = <default>";
    }
  }
}

void SignaturePrinter::appendConceptApplication(
    std::string &result, const ConceptApplication &application) {
  if (application.predicate) {
    result += semantic_spelling::expressionSpelling(*application.predicate);
    return;
  }
  if (application.memberRequirement) {
    result +=
        semantic_spelling::expressionSpelling(*application.memberRequirement);
    return;
  }
  result += path(application.name);
  result += '<';
  for (std::size_t index = 0; index < application.arguments.size(); ++index) {
    if (index != 0) {
      result += ", ";
    }
    result += semantic_spelling::typeRefSpelling(application.arguments[index]);
  }
  result += '>';
}

void SignaturePrinter::appendRequirements(
    std::string &result, const FunctionInfo &info,
    const ResolvedCallInfo *selected) const {
  if (info.declaration == nullptr || !info.declaration->requiresClause()) {
    return;
  }
  appendRequirements(result, info.declaration->requiresClause(),
                     selected == nullptr ? info.requirements
                                         : selected->requirements);
}

void SignaturePrinter::appendRequirements(
    std::string &result, const std::optional<RequiresClause> &requiresClause,
    const std::vector<AppliedConceptRequirement> &requirements) const {
  if (!requiresClause) {
    return;
  }
  const ConstraintExpression &expression = requiresClause->expression;
  const std::vector<ConceptApplication> &syntax = expression.applications;
  const auto appendApplication = [&](std::size_t index) {
    if (index >= syntax.size()) {
      return;
    }
    if (syntax[index].memberRequirement) {
      appendConceptApplication(result, syntax[index]);
      return;
    }
    const AppliedConceptRequirement *resolvedRequirement = nullptr;
    for (const AppliedConceptRequirement &requirement : requirements) {
      if (requirement.expression == &expression &&
          requirement.expressionApplication == index) {
        resolvedRequirement = &requirement;
        break;
      }
    }
    result += path(syntax[index].name);
    result += '<';
    for (std::size_t argumentIndex = 0;
         argumentIndex < syntax[index].arguments.size(); ++argumentIndex) {
      if (argumentIndex != 0) {
        result += ", ";
      }
      if (resolvedRequirement != nullptr &&
          argumentIndex < resolvedRequirement->arguments.size()) {
        const std::string printed =
            types.print(resolvedRequirement->arguments[argumentIndex]);
        result += printed == "unknown"
                      ? semantic_spelling::typeRefSpelling(
                            syntax[index].arguments[argumentIndex])
                      : printed;
      } else {
        result += semantic_spelling::typeRefSpelling(
            syntax[index].arguments[argumentIndex]);
      }
    }
    result += '>';
    if (syntax[index].predicate) {
      result += "(";
      if (const auto *call = exprAs<Call>(syntax[index].predicate.get()))
        for (std::size_t argument = 0; argument < call->arguments().size();
             ++argument) {
          if (argument != 0)
            result += ", ";
          result += semantic_spelling::expressionSpelling(
              *call->arguments()[argument]);
        }
      result += ")";
    }
  };
  result += " requires ";
  std::function<void(std::size_t, int)> appendNode = [&](std::size_t nodeIndex,
                                                         int parentPrecedence) {
    if (nodeIndex >= expression.nodes.size()) {
      return;
    }
    const ConstraintExpressionNode &node = expression.nodes[nodeIndex];
    if (node.kind == ConstraintExpressionKind::Application) {
      appendApplication(node.application);
      return;
    }
    const int precedence =
        node.kind == ConstraintExpressionKind::Conjunction ? 2 : 1;
    const bool parenthesize = precedence < parentPrecedence;
    if (parenthesize) {
      result += '(';
    }
    appendNode(node.left, precedence);
    result +=
        node.kind == ConstraintExpressionKind::Conjunction ? " && " : " || ";
    appendNode(node.right, precedence);
    if (parenthesize) {
      result += ')';
    }
  };
  appendNode(expression.root, 0);
}

namespace {

struct ModuleQueryLocation {
  SourceSpan origin;
  const SourceUnit *interface = nullptr;
};

bool spanContains(const SourceSpan &span, std::size_t offset) {
  return span.start <= offset && offset < span.end;
}

SourceSpan moduleNameSpan(const SourceUnit &unit) {
  return unit.moduleDeclarationSpan.value_or(
      tokenSpan(*unit.moduleDeclaration));
}

std::optional<ModuleQueryLocation> moduleAt(const FrontendResult &snapshot,
                                            SourceUnitId requester,
                                            std::size_t offset) {
  for (const auto &import : snapshot.sourceGraph.imports()) {
    if (import.requester != requester || !spanContains(import.name, offset) ||
        import.resolution != SourceImportResolution::Resolved || !import.target)
      continue;
    const auto *target = snapshot.sourceGraph.findUnit(*import.target);
    if (target && target->moduleDeclaration)
      return ModuleQueryLocation{import.name, target};
  }
  const auto *unit = snapshot.sourceGraph.findUnit(requester);
  if (!unit)
    return std::nullopt;
  if (unit->moduleDeclaration && spanContains(moduleNameSpan(*unit), offset))
    return ModuleQueryLocation{moduleNameSpan(*unit), unit};
  if (unit->implementationDeclaration) {
    const auto range = unit->implementationDeclarationSpan.value_or(
        tokenSpan(*unit->implementationDeclaration));
    const auto *module = snapshot.sourceGraph.findModule(unit->moduleId);
    const auto *target =
        module ? snapshot.sourceGraph.findUnit(module->interfaceUnit) : nullptr;
    if (spanContains(range, offset) && target && target->moduleDeclaration)
      return ModuleQueryLocation{range, target};
  }
  return std::nullopt;
}

std::optional<CompletionResult>
completeModuleImport(const FrontendResult &snapshot,
                     const CompletionInput &input) {
  const SourceImport *request = nullptr;
  for (const auto &import : snapshot.sourceGraph.imports()) {
    if (import.requester == snapshot.sourceGraph.entryUnit() &&
        !import.quoted && import.name.start <= input.byteOffset &&
        input.byteOffset <= import.name.end) {
      request = &import;
      break;
    }
  }
  if (!request)
    return std::nullopt;
  CompletionResult result;
  const auto registry = snapshot.sourceGraph.sourceOwners();
  if (!registry || request->name.end > input.source.size())
    return result;
  const auto *entry = snapshot.sourceGraph.findUnit(request->requester);
  const auto *owner = entry ? registry->ownerForPath(entry->path) : nullptr;
  const std::string prefix =
      request->completionPrefix.value_or(input.source.substr(
          request->name.start, input.byteOffset - request->name.start));
  struct Choice {
    std::filesystem::path target;
    bool ambiguous = false;
  };
  std::unordered_map<std::string, Choice> choices;
  const auto add = [&](std::string name, const std::filesystem::path &path) {
    const auto [position, added] =
        choices.emplace(std::move(name), Choice{path});
    if (!added)
      position->second.ambiguous = true;
  };
  const auto addOwner = [&](const SourceOwnerDescriptor &selected,
                            const std::string &alias) {
    for (const auto &[name, path] : selected.modules) {
      add(alias.empty() ? name : alias + "." + name, path);
      if (!alias.empty() && name == selected.defaultModule)
        add(alias, path);
    }
  };
  if (owner) {
    addOwner(*owner, "");
    for (const auto &dependency : owner->dependencies)
      if (const auto *target = registry->find(dependency.targetIdentity))
        addOwner(*target, dependency.alias);
  }
  for (const auto &candidate : registry->owners())
    if (candidate.implicitAlias)
      addOwner(candidate, *candidate.implicitAlias);
  for (const auto &[name, choice] : choices) {
    if (choice.ambiguous || (entry && choice.target == entry->path) ||
        !name.starts_with(prefix))
      continue;
    result.candidates.push_back({.kind = CompletionCandidateKind::Namespace,
                                 .label = name,
                                 .detail = "module " + name,
                                 .insertion = name,
                                 .sortText = name,
                                 .replacementRange = request->name});
  }
  std::sort(result.candidates.begin(), result.candidates.end(),
            [](const auto &left, const auto &right) {
              return left.label < right.label;
            });
  return result;
}

// Inspection is deliberately separate from lowering. This bounded printer
// renders retained checked syntax and marks omissions rather than attempting
// to reconstruct an executable program or its ownership schedule.
class MacroExpansionDisplay {
public:
  explicit MacroExpansionDisplay(const SemanticModel &semantics)
      : semantics(semantics) {}

  std::string print(const Stmt &body) {
    append(
        "// Explanatory GTI macro expansion; read-only, not executable "
        "source.\n"
        "// Arguments remain expressions: repeated uses repeat evaluation.\n"
        "// __macro_local_N labels distinguish checked hygienic bindings.\n");
    statement(body, 0);
    if (truncated_)
      output += "\n// Expansion display truncated (16 KiB / 4096 nodes / 64 "
                "levels).\n";
    return std::move(output);
  }

  bool truncated() const { return truncated_; }

private:
  static constexpr std::size_t byteLimit = 16000;
  const SemanticModel &semantics;
  std::string output;
  std::size_t visited = 0;
  bool truncated_ = false;
  std::unordered_map<SymbolId, std::string> localNames;

  void append(std::string_view text) {
    if (truncated_)
      return;
    if (text.size() <= byteLimit - output.size()) {
      output += text;
      return;
    }
    std::size_t length = byteLimit - output.size();
    // JSON/editor text must remain valid UTF-8 even at the output bound.
    while (length > 0 && length < text.size() &&
           (static_cast<unsigned char>(text[length]) & 0xc0U) == 0x80U)
      --length;
    output.append(text.substr(0, length));
    truncated_ = true;
  }

  bool enter(std::size_t depth) {
    if (truncated_)
      return false;
    if (++visited > 4096 || depth >= 64) {
      truncated_ = true;
      return false;
    }
    return true;
  }

  void indentation(std::size_t depth) { append(std::string(depth * 2, ' ')); }

  void path(const NamePath &name) {
    for (std::size_t index = 0; index < name.segments.size(); ++index) {
      if (index)
        append("::");
      append(name.segments[index].lexeme);
    }
  }

  void arguments(const ExprList &values, std::size_t depth,
                 const std::vector<Token> *designators = nullptr) {
    for (std::size_t index = 0; index < values.size() && !truncated_; ++index) {
      if (index)
        append(", ");
      if (designators && index < designators->size() &&
          !(*designators)[index].lexeme.empty()) {
        append(".");
        append((*designators)[index].lexeme);
        append(" = ");
      }
      expression(values[index].get(), depth + 1);
    }
  }

  void expression(const Expr *value, std::size_t depth) {
    if (!value || !enter(depth))
      return;
    if (const auto *node = exprAs<Variable>(value)) {
      const PlaceKey *place = semantics.findPlace(*node);
      const auto local =
          place ? localNames.find(place->root) : localNames.end();
      append(local == localNames.end() ? node->name().lexeme : local->second);
    } else if (const auto *node = exprAs<LiteralExpr>(value)) {
      append(node->token().lexeme.empty() ? "/* synthesized literal */"
                                          : node->token().lexeme);
    } else if (const auto *node = exprAs<QualifiedName>(value)) {
      if (node->ownerType()) {
        append(semantic_spelling::typeRefSpelling(*node->ownerType()));
        append("::");
        append(node->name().last().lexeme);
      } else {
        path(node->name());
      }
    } else if (exprAs<This>(value)) {
      append("this");
    } else if (const auto *node = exprAs<Binary>(value)) {
      binary(*node->left(), node->oper().lexeme, *node->right(), depth);
    } else if (const auto *node = exprAs<Logical>(value)) {
      binary(*node->left(), node->oper().lexeme, *node->right(), depth);
    } else if (const auto *node = exprAs<ConditionalExpr>(value)) {
      append("(");
      expression(node->condition().get(), depth + 1);
      append(" ? ");
      expression(node->thenExpression().get(), depth + 1);
      append(" : ");
      expression(node->elseExpression().get(), depth + 1);
      append(")");
    } else if (const auto *node = exprAs<Unary>(value)) {
      append("(");
      append(node->oper().lexeme);
      append(" ");
      expression(node->right().get(), depth + 1);
      append(")");
    } else if (const auto *node = exprAs<Postfix>(value)) {
      append("(");
      expression(node->expression().get(), depth + 1);
      append(node->oper().lexeme);
      append(")");
    } else if (const auto *node = exprAs<Grouping>(value)) {
      append(node->unsafeKeyword() ? "unsafe(" : "(");
      expression(node->expression().get(), depth + 1);
      append(")");
    } else if (const auto *node = exprAs<Call>(value)) {
      expression(node->callee().get(), depth + 1);
      if (!node->typeArguments().empty()) {
        append("<");
        for (std::size_t index = 0; index < node->typeArguments().size();
             ++index) {
          if (index)
            append(", ");
          append(
              semantic_spelling::typeRefSpelling(node->typeArguments()[index]));
        }
        append(">");
      }
      append("(");
      arguments(node->arguments(), depth);
      append(")");
    } else if (const auto *node = exprAs<Get>(value)) {
      expression(node->object().get(), depth + 1);
      append(node->access().lexeme);
      append(node->name().lexeme);
    } else if (const auto *node = exprAs<Index>(value)) {
      expression(node->object().get(), depth + 1);
      append("[");
      expression(node->index().get(), depth + 1);
      append("]");
    } else if (const auto *node = exprAs<Assign>(value)) {
      const PlaceKey *place = semantics.findPlace(*node);
      const auto local =
          place ? localNames.find(place->root) : localNames.end();
      if (local != localNames.end())
        append(local->second);
      else
        path(node->path());
      append(" ");
      append(node->oper().lexeme);
      append(" ");
      expression(node->value().get(), depth + 1);
    } else if (const auto *node = exprAs<PlaceSet>(value)) {
      binary(*node->target(), node->oper().lexeme, *node->value(), depth);
    } else if (const auto *node = exprAs<Set>(value)) {
      expression(node->object().get(), depth + 1);
      append(node->access().lexeme);
      append(node->name().lexeme);
      append(" ");
      append(node->oper().lexeme);
      append(" ");
      expression(node->value().get(), depth + 1);
    } else if (const auto *node = exprAs<IndexSet>(value)) {
      expression(node->object().get(), depth + 1);
      append("[");
      expression(node->index().get(), depth + 1);
      append("] ");
      append(node->oper().lexeme);
      append(" ");
      expression(node->value().get(), depth + 1);
    } else if (const auto *node = exprAs<DereferenceSet>(value)) {
      append("*");
      expression(node->object().get(), depth + 1);
      append(" ");
      append(node->oper().lexeme);
      append(" ");
      expression(node->value().get(), depth + 1);
    } else if (const auto *node = exprAs<Conversion>(value)) {
      append(semantic_spelling::typeRefSpelling(node->targetType()));
      append("(");
      expression(node->value().get(), depth + 1);
      append(")");
    } else if (const auto *node = exprAs<DirectInitializer>(value)) {
      append("{");
      arguments(node->arguments(), depth, &node->designators());
      append("}");
    } else if (const auto *node = exprAs<ArrayInitializer>(value)) {
      append("{");
      arguments(node->elements(), depth);
      append("}");
    } else if (const auto *node = exprAs<LayoutQuery>(value)) {
      append(node->keyword().lexeme);
      append("(");
      append(semantic_spelling::typeRefSpelling(node->type()));
      append(")");
    } else if (const auto *node = exprAs<PackExpansion>(value)) {
      append(node->name().lexeme);
      append("...");
    } else {
      // Lambdas/unevaluated requirements supplied by an argument may remain
      // in checked syntax. Do not misrepresent their source as fully rendered.
      append("/* expression omitted from inspection */");
    }
  }

  void binary(const Expr &left, std::string_view oper, const Expr &right,
              std::size_t depth) {
    append("(");
    expression(&left, depth + 1);
    append(" ");
    append(oper);
    append(" ");
    expression(&right, depth + 1);
    append(")");
  }

  void statement(const Stmt &value, std::size_t depth) {
    if (!enter(depth))
      return;
    if (const auto *node = stmtAs<BlockStmt>(&value)) {
      indentation(depth);
      append("{\n");
      for (const auto &child : node->statements()) {
        if (truncated_)
          break;
        if (child)
          statement(*child, depth + 1);
      }
      indentation(depth);
      append("}\n");
    } else if (const auto *node = stmtAs<ExpressionStmt>(&value)) {
      if (const Stmt *expanded = semantics.findMacroExpansion(*node)) {
        indentation(depth);
        append("// Nested macro expansion:\n");
        statement(*expanded, depth + 1);
      } else {
        indentation(depth);
        if (node->discardAttribute())
          append("[[discard]] ");
        expression(node->expression().get(), depth + 1);
        append(";\n");
      }
    } else if (const auto *node = stmtAs<VariableDecl>(&value)) {
      indentation(depth);
      variable(*node, depth);
      append(";\n");
    } else if (const auto *node = stmtAs<IfStmt>(&value)) {
      indentation(depth);
      append(node->isConstexpr() ? "if constexpr (" : "if (");
      expression(node->condition().get(), depth + 1);
      append(")\n");
      if (node->thenBranch())
        statement(*node->thenBranch(), depth + 1);
      if (node->elseBranch()) {
        indentation(depth);
        append("else\n");
        statement(*node->elseBranch(), depth + 1);
      }
    } else if (const auto *node = stmtAs<ForStmt>(&value)) {
      indentation(depth);
      append("for (");
      if (const auto *initializer =
              stmtAs<VariableDecl>(node->initializer().get()))
        variable(*initializer, depth);
      else if (const auto *initializer =
                   stmtAs<ExpressionStmt>(node->initializer().get()))
        expression(initializer->expression().get(), depth + 1);
      append("; ");
      expression(node->condition().get(), depth + 1);
      append("; ");
      expression(node->increment().get(), depth + 1);
      append(")\n");
      if (node->body())
        statement(*node->body(), depth + 1);
    } else if (const auto *node = stmtAs<WhileStmt>(&value)) {
      indentation(depth);
      append("while (");
      expression(node->condition().get(), depth + 1);
      append(")\n");
      if (node->body())
        statement(*node->body(), depth + 1);
    } else if (const auto *node = stmtAs<DoWhileStmt>(&value)) {
      indentation(depth);
      append("do\n");
      if (node->body())
        statement(*node->body(), depth + 1);
      indentation(depth);
      append("while (");
      expression(node->condition().get(), depth + 1);
      append(");\n");
    } else if (const auto *node = stmtAs<LoopControlStmt>(&value)) {
      indentation(depth);
      append(node->keyword().lexeme);
      append(";\n");
    } else {
      indentation(depth);
      append(stmtAs<EmptyStmt>(&value)
                 ? ";\n"
                 : "/* statement omitted from inspection */\n");
    }
  }

  void variable(const VariableDecl &node, std::size_t depth) {
    if (node.isMutable())
      append("mut ");
    append(semantic_spelling::typeRefSpelling(node.type()));
    append(" ");
    const auto *binding = semantics.findBinding(node);
    std::string name = node.name().lexeme;
    if (binding && binding->symbol) {
      name += "__macro_local_" + std::to_string(localNames.size() + 1);
      localNames.insert_or_assign(binding->symbol, name);
    }
    append(name);
    if (node.initializer()) {
      append(" = ");
      expression(node.initializer().get(), depth + 1);
    }
  }
};

} // namespace

class LanguageQueriesImpl {
public:
  [[nodiscard]] std::optional<HoverInfo> hover(const FrontendResult &snapshot,
                                               SourceUnitId sourceUnit,
                                               std::size_t byteOffset) const {
    const SemanticOccurrence *occurrence =
        snapshot.semantics.database().findOccurrence(sourceUnit, byteOffset);
    if (occurrence == nullptr) {
      return std::nullopt;
    }
    if (!snapshot.semantics.canPresent(sourceUnit, *occurrence,
                                       snapshot.sourceGraph)) {
      return std::nullopt;
    }

    const SemanticModel &semantics = snapshot.semantics;
    const SignaturePrinter signatures(semantics);
    const SemanticTypePrinter types(semantics);
    HoverInfo result{.range = occurrence->span};
    const FunctionInfo *hoveredFunction = nullptr;
    const ResolvedCallInfo *hoveredCall = nullptr;
    switch (occurrence->kind) {
    case SemanticOccurrenceKind::SelectedCall: {
      const ResolvedCallInfo &selected = *occurrence->selectedCall;
      if (selected.nativeCalleeType) {
        result.signature = signatures.nativeCall(selected, occurrence->name);
        break;
      }
      const FunctionInfo *function = semantics.findFunction(selected.function);
      if (function != nullptr) {
        result.signature = selected.argumentReceiver != nullptr
                               ? signatures.forwardedMemberCall(
                                     *function, selected, occurrence->name)
                               : signatures.function(*function, &selected);
        hoveredFunction = function;
        hoveredCall = &selected;
      }
      break;
    }
    case SemanticOccurrenceKind::SelectedConstruction: {
      const ResolvedConstructionInfo &selected =
          *occurrence->selectedConstruction;
      const ClassTypeInfo *owner =
          semantics.findClassType(selected.constructedType.classId);
      if (owner != nullptr) {
        result.signature = signatures.constructor(*owner, selected);
        if (selected.directInitialization)
          result.notes.emplace_back(
              "direct initialization from a fresh same-type result; no "
              "additional copy, move, or temporary lifetime");
      }
      break;
    }
    case SemanticOccurrenceKind::Function:
      if (occurrence->function != nullptr) {
        if (const FunctionInfo *function =
                semantics.findFunction(*occurrence->function)) {
          result.signature = signatures.function(*function);
          hoveredFunction = function;
        }
      }
      break;
    case SemanticOccurrenceKind::ClassType:
      if (occurrence->classType != nullptr) {
        if (const ClassTypeInfo *type =
                semantics.findClassType(*occurrence->classType)) {
          result.signature = signatures.classType(*type);
        }
      }
      break;
    case SemanticOccurrenceKind::EnumType:
      if (occurrence->enumType != nullptr) {
        if (const EnumTypeInfo *type =
                semantics.findEnumType(*occurrence->enumType)) {
          result.signature = signatures.enumType(*type);
        }
      }
      break;
    case SemanticOccurrenceKind::TypeAlias:
      if (occurrence->typeAlias != nullptr) {
        if (const TypeAliasInfo *alias =
                semantics.findTypeAlias(*occurrence->typeAlias)) {
          result.signature = signatures.typeAlias(*alias);
        }
      }
      break;
    case SemanticOccurrenceKind::Constructor:
      if (occurrence->constructor != nullptr) {
        const ConstructorInfo *constructor =
            semantics.findConstructor(*occurrence->constructor);
        const ClassTypeInfo *owner =
            constructor == nullptr
                ? nullptr
                : semantics.findClassType(constructor->owner);
        if (constructor != nullptr && owner != nullptr) {
          result.signature = signatures.constructor(*owner, *constructor);
        }
      }
      break;
    case SemanticOccurrenceKind::Destructor:
      if (occurrence->destructor != nullptr) {
        if (const DestructorInfo *destructor =
                semantics.findDestructor(*occurrence->destructor)) {
          result.signature = signatures.destructor(*destructor);
        }
      }
      break;
    case SemanticOccurrenceKind::Binding: {
      const SymbolRecord *symbol =
          semantics.database().findSymbol(occurrence->symbol);
      if (occurrence->type == SemanticType::Unknown && symbol != nullptr &&
          symbol->instanceDependentType) {
        result.signature = occurrence->mutableBinding ? "mut auto " : "auto ";
        result.signature += occurrence->name;
        result.notes.emplace_back(
            "type is resolved independently for each concrete generic "
            "instance");
      } else {
        result.signature = signatures.binding(*occurrence);
        if (semantics.findForeignStorage(occurrence->symbol) != nullptr) {
          result.notes.emplace_back(
              "Foreign storage: reads and writes require unsafe; address "
              "formation does not certify lifetime or initialization.");
        }
      }
      break;
    }
    case SemanticOccurrenceKind::Symbol: {
      const SymbolRecord *symbol =
          snapshot.semantics.database().findSymbol(occurrence->symbol);
      if (symbol == nullptr) {
        break;
      }
      switch (symbol->kind) {
      case SymbolKind::Namespace:
        result.signature = "namespace " + symbol->qualifiedName;
        break;
      case SymbolKind::NamespaceAlias:
        result.signature = "namespace " + symbol->qualifiedName;
        break;
      case SymbolKind::Concept:
        result.signature = signatures.conceptSignature(
            *symbol, findConceptDeclaration(snapshot.program.declarations(),
                                            symbol->nameSpan));
        break;
      case SymbolKind::Class:
      case SymbolKind::Struct:
        if (occurrence->type.kind == SemanticType::Class) {
          if (const ClassTypeInfo *type =
                  semantics.findClassType(occurrence->type.classId)) {
            result.signature = signatures.classType(*type);
            break;
          }
        }
        result.signature = types.print(symbol->type);
        break;
      case SymbolKind::Enumerator:
        if (occurrence->type.kind == SemanticType::Enum) {
          if (auto owner = semantics.enumTypeFor(occurrence->type)) {
            // Resolution owns the concrete signature. The primary symbol
            // still supplies declaration identity and documentation, not the
            // payload types selected by this application.
            owner->qualifiedName = types.print(occurrence->type);
            result.signature = signatures.enumerator(*owner, symbol->name);
            break;
          }
        }
        if (symbol->type.kind == SemanticType::Enum) {
          if (const auto *owner = semantics.findEnumType(symbol->type.enumId)) {
            result.signature = signatures.enumerator(*owner, symbol->name);
            break;
          }
        }
        result.signature =
            types.print(symbol->type) + " " + symbol->qualifiedName;
        break;
      case SymbolKind::TypeParameter:
        result.signature = "type parameter " + symbol->name;
        break;
      case SymbolKind::TypeAlias:
        result.signature = "using " + symbol->qualifiedName + " = " +
                           types.print(occurrence->type != SemanticType::Unknown
                                           ? occurrence->type
                                           : symbol->type);
        break;
      case SymbolKind::ValueParameter:
        result.signature = "uint64_t " + symbol->name;
        break;
      default:
        if (symbol->type != SemanticType::Unknown) {
          result.signature = types.print(symbol->type) + " " + symbol->name;
          if (const auto *storage = semantics.findForeignStorage(symbol->id)) {
            result.signature =
                "extern \"C\" { " +
                (storage->externalSymbol != symbol->name
                     ? "[[link_name(\"" + storage->externalSymbol + "\")]] "
                     : std::string()) +
                (storage->access == AccessMode::Mutable ? "mut " : "") +
                result.signature + "; }";
            result.notes.emplace_back(
                "Foreign storage: reads and writes require unsafe; address "
                "formation does not certify lifetime or initialization.");
          }
        }
        break;
      }
      break;
    }
    case SemanticOccurrenceKind::InferredType: {
      const SymbolRecord *symbol =
          semantics.database().findSymbol(occurrence->symbol);
      if (occurrence->type == SemanticType::Unknown && symbol != nullptr &&
          symbol->instanceDependentType) {
        result.signature = "auto";
        result.notes.emplace_back(
            "type is resolved independently for each concrete generic "
            "instance");
      } else {
        result.signature = types.print(occurrence->type);
      }
      break;
    }
    case SemanticOccurrenceKind::Expression:
      if (semantics.findForeignStorage(occurrence->symbol) != nullptr) {
        result.signature = signatures.binding(*occurrence);
        result.notes.emplace_back(
            "Foreign storage: reads and writes require unsafe; address "
            "formation does not certify lifetime or initialization.");
      } else if (occurrence->threadLocal) {
        result.signature = signatures.binding(*occurrence);
      } else {
        result.signature = types.print(occurrence->type);
      }
      break;
    }
    if (result.signature.empty() || result.signature == "unknown") {
      return std::nullopt;
    }
    if (occurrence->symbol != 0) {
      if (const SymbolRecord *symbol =
              semantics.database().findSymbol(occurrence->symbol);
          symbol != nullptr && symbol->documentationMarkdown) {
        result.documentationMarkdown = symbol->documentationMarkdown;
      }
    }
    if (hoveredFunction != nullptr) {
      appendCallableContractNotes(result, semantics, *hoveredFunction,
                                  hoveredCall, types);
    }
    if (const std::optional<LambdaCaptureMode> captureMode =
            semantics.lambdaCaptureMode(occurrence->symbol)) {
      result.notes.emplace_back(*captureMode == LambdaCaptureMode::Move
                                    ? "owned move capture"
                                    : "immutable copy-snapshot capture");
    }
    if (occurrence->kind == SemanticOccurrenceKind::ClassType &&
        occurrence->classType != nullptr) {
      const ClassTypeInfo *type =
          snapshot.semantics.findClassType(*occurrence->classType);
      const auto capabilityNote = [](std::string_view name, bool capable,
                                     ConcurrencyCapabilityPolicy policy) {
        std::string note = capable ? std::string(name) + "-capable"
                                   : "not " + std::string(name) + "-capable";
        if (policy == ConcurrencyCapabilityPolicy::Denied) {
          note += " (explicit opt-out)";
        } else if (policy == ConcurrencyCapabilityPolicy::UnsafeAsserted) {
          note += " (unsafe nominal assertion)";
        } else if (policy == ConcurrencyCapabilityPolicy::UnsafeConditional) {
          note += " (conditional unsafe nominal assertion)";
        } else if (policy == ConcurrencyCapabilityPolicy::Required) {
          note += " (interface requirement)";
        }
        return note;
      };
      if (type != nullptr) {
        if (type->unsafeOwned)
          result.notes.emplace_back("unsafe ownership assertion: generic "
                                    "payloads independently checked; "
                                    "no thread capability granted");
        if (type->cOpaqueHandle) {
          result.notes.emplace_back(
              "opaque C handle: address-only through raw pointers");
        } else if (type->cAbiRecord && type->cAbiLayout) {
          result.notes.emplace_back(
              "C ABI record: size " +
              std::to_string(type->cAbiLayout->sizeBytes) +
              " bytes, ABI alignment " +
              std::to_string(type->cAbiLayout->abiAlignmentBytes) + " bytes");
        }
        if (!type->cOpaqueHandle) {
          result.notes.emplace_back(
              capabilityNote("transfer", occurrence->traits.transferCapable,
                             type->transferPolicy));
          result.notes.emplace_back(capabilityNote(
              "share", occurrence->traits.shareCapable, type->sharePolicy));
        }
      }
    } else if (occurrence->traits.ownership == OwnershipKind::Unique) {
      result.notes.emplace_back("move-only owner");
    } else if (occurrence->access == AccessMode::Mutable) {
      result.notes.emplace_back("mutable place");
    }
    return result;
  }

  [[nodiscard]] std::optional<DefinitionInfo>
  definition(const FrontendResult &snapshot, SourceUnitId sourceUnit,
             std::size_t byteOffset) const {
    const SemanticDatabase &database = snapshot.semantics.database();
    const SemanticOccurrence *occurrence =
        database.findOccurrence(sourceUnit, byteOffset);
    if (occurrence == nullptr || occurrence->symbol == 0) {
      return std::nullopt;
    }
    if (!snapshot.semantics.canPresent(sourceUnit, *occurrence,
                                       snapshot.sourceGraph)) {
      return std::nullopt;
    }
    const SymbolRecord *symbol = database.findSymbol(occurrence->symbol);
    if (symbol == nullptr) {
      return std::nullopt;
    }
    if (!snapshot.semantics.canPresent(sourceUnit, *symbol,
                                       snapshot.sourceGraph)) {
      return std::nullopt;
    }
    return DefinitionInfo{
        .symbol = symbol->id,
        .origin = occurrence->span,
        .target = symbol->definitionSpan.value_or(symbol->declarationSpan),
    };
  }

  [[nodiscard]] std::optional<ReferencesInfo>
  references(const FrontendResult &snapshot, SourceUnitId sourceUnit,
             std::size_t byteOffset, bool includeDeclaration) const {
    const SemanticDatabase &database = snapshot.semantics.database();
    const SemanticOccurrence *occurrence =
        database.findOccurrence(sourceUnit, byteOffset);
    if (occurrence == nullptr || occurrence->symbol == 0) {
      return std::nullopt;
    }
    if (!snapshot.semantics.canPresent(sourceUnit, *occurrence,
                                       snapshot.sourceGraph)) {
      return std::nullopt;
    }
    const SymbolRecord *symbol = database.findSymbol(occurrence->symbol);
    if (symbol == nullptr || !snapshot.semantics.canPresent(
                                 sourceUnit, *symbol, snapshot.sourceGraph)) {
      return std::nullopt;
    }

    ReferencesInfo result{.symbol = symbol->id};
    result.sites = collectSites(snapshot, sourceUnit,
                                writtenBindingSymbols(snapshot, *symbol));
    if (!includeDeclaration) {
      std::erase_if(result.sites, [&](const ReferenceSite &site) {
        return (hasRole(site.roles, OccurrenceRole::Declaration) ||
                hasRole(site.roles, OccurrenceRole::Definition)) &&
               !hasRole(site.roles, OccurrenceRole::Reference);
      });
    }
    if (result.sites.empty()) {
      return std::nullopt;
    }
    return result;
  }

  [[nodiscard]] std::optional<RenamePreparation>
  prepareRename(const FrontendResult &snapshot, SourceUnitId sourceUnit,
                std::size_t byteOffset) const {
    const RenameCore core = renameCore(snapshot, sourceUnit, byteOffset);
    if (core.symbol == nullptr || !core.failure.empty()) {
      return std::nullopt;
    }
    return RenamePreparation{.symbol = core.symbol->id,
                             .origin = core.origin,
                             .placeholder = core.symbol->name};
  }

  [[nodiscard]] RenameOutcome rename(const FrontendResult &snapshot,
                                     SourceUnitId sourceUnit,
                                     std::size_t byteOffset,
                                     std::string_view newName) const {
    RenameOutcome outcome;
    const RenameCore core = renameCore(snapshot, sourceUnit, byteOffset);
    if (core.symbol == nullptr) {
      outcome.failure = "No renamable symbol at this position.";
      return outcome;
    }
    if (!core.failure.empty()) {
      outcome.failure = core.failure;
      return outcome;
    }
    if (std::string validation = validateNewName(newName);
        !validation.empty()) {
      outcome.failure = std::move(validation);
      return outcome;
    }
    if (newName != core.symbol->name &&
        conflictsWithVisibleName(snapshot, core, newName)) {
      outcome.failure = "Renaming to '" + std::string(newName) +
                        "' could change meaning: that name is already used "
                        "in a source unit this rename touches.";
      return outcome;
    }

    RenameEdits edits{.symbol = core.symbol->id};
    edits.spans.reserve(core.sites.size());
    for (const ReferenceSite &site : core.sites) {
      edits.spans.push_back(site.span);
    }
    outcome.edits = std::move(edits);
    return outcome;
  }

  [[nodiscard]] std::optional<SignatureHelpInfo>
  signatureHelp(const FrontendResult &snapshot, SourceUnitId sourceUnit,
                std::size_t byteOffset) const {
    // The innermost call whose parser-recorded argument list contains the
    // offset wins: among containing geometries, the one whose '(' is
    // closest to the offset.
    const SemanticOccurrence *selected = nullptr;
    for (const SemanticOccurrence &occurrence :
         snapshot.semantics.database().occurrences(sourceUnit)) {
      if (!occurrence.callGeometry) {
        continue;
      }
      const SemanticCallGeometry &geometry = *occurrence.callGeometry;
      if (byteOffset <= geometry.leftDelimiter ||
          byteOffset > geometry.rightDelimiter) {
        continue;
      }
      if (!snapshot.semantics.canPresent(sourceUnit, occurrence,
                                         snapshot.sourceGraph)) {
        continue;
      }
      if (selected == nullptr ||
          selected->callGeometry->leftDelimiter < geometry.leftDelimiter) {
        selected = &occurrence;
      }
    }
    if (selected == nullptr) {
      return std::nullopt;
    }

    const SemanticModel &semantics = snapshot.semantics;
    const SignaturePrinter signatures(semantics);
    SignatureHelpInfo result;
    if (selected->kind == SemanticOccurrenceKind::SelectedCall &&
        selected->selectedCall) {
      const ResolvedCallInfo &resolved = *selected->selectedCall;
      if (resolved.nativeCalleeType) {
        result.label = signatures.nativeCall(resolved, selected->name);
        result.parameterLabels =
            signatures.parameterLabels(resolved.parameterTypes, nullptr, false);
      } else {
        const FunctionInfo *function =
            semantics.findFunction(resolved.function);
        if (function == nullptr) {
          return std::nullopt;
        }
        result.label = resolved.argumentReceiver != nullptr
                           ? signatures.forwardedMemberCall(*function, resolved,
                                                            selected->name)
                           : signatures.function(*function, &resolved);
        result.parameterLabels = signatures.parameterLabels(
            resolved.parameterTypes,
            function->declaration == nullptr
                ? nullptr
                : &function->declaration->parameters(),
            false);
        if (resolved.argumentReceiver != nullptr &&
            !result.parameterLabels.empty())
          result.parameterLabels.erase(result.parameterLabels.begin());
      }
    } else if (selected->kind == SemanticOccurrenceKind::SelectedConstruction &&
               selected->selectedConstruction) {
      const ResolvedConstructionInfo &resolved =
          *selected->selectedConstruction;
      const ClassTypeInfo *owner =
          resolved.constructedType.kind == SemanticType::Class
              ? semantics.findClassType(resolved.constructedType.classId)
              : nullptr;
      if (owner == nullptr) {
        return std::nullopt;
      }
      result.label = signatures.constructor(*owner, resolved);
      result.parameterLabels = signatures.parameterLabels(
          resolved.parameterTypes,
          resolved.declaration == nullptr ? nullptr
                                          : &resolved.declaration->parameters(),
          false);
    } else {
      return std::nullopt;
    }

    // The call-site occurrence resolves to the selected callable's symbol,
    // so its retained `///` documentation is the same value hover shows.
    if (selected->symbol != 0) {
      if (const SymbolRecord *symbol =
              semantics.database().findSymbol(selected->symbol);
          symbol != nullptr && symbol->documentationMarkdown) {
        result.documentationMarkdown = symbol->documentationMarkdown;
      }
    }

    const SemanticCallGeometry &geometry = *selected->callGeometry;
    std::size_t active = 0;
    for (const std::size_t separator : geometry.argumentSeparators) {
      if (separator < byteOffset) {
        ++active;
      }
    }
    if (!result.parameterLabels.empty() &&
        active >= result.parameterLabels.size()) {
      active = result.parameterLabels.size() - 1;
    }
    result.activeParameter = active;
    return result;
  }

  [[nodiscard]] std::vector<DocumentSymbolInfo>
  documentSymbols(const FrontendResult &snapshot,
                  SourceUnitId sourceUnit) const {
    std::vector<DocumentSymbolInfo> result;
    const SourceUnit *unit = snapshot.sourceGraph.findUnit(sourceUnit);
    if (unit == nullptr) {
      return result;
    }
    const SemanticAnalysisSeal &seal = snapshot.semantics.analysisSeal();
    const TargetInfo target =
        seal.programSnapshot == 0 ? TargetInfo::host() : seal.target;
    appendOutline(snapshot, sourceUnit, unit->path.string(), target,
                  snapshot.program.declarations(), false, result);
    return result;
  }

  [[nodiscard]] CompletionResult complete(const CompletionInput &input) const {
    CompletionResult result;
    if (input.entryPath.empty() || input.byteOffset > input.source.size()) {
      return result;
    }

    FrontendOptions options;
    options.target = input.target;
    options.configurationFlags = input.configurationFlags;
    options.declaredConfigurationFlags = input.declaredConfigurationFlags;
    options.rawPointerAudit = input.rawPointerAudit;
    options.trustedBodies = input.trustedBodies;
    options.analyzeRecoveredProgram = true;
    options.analyzeRecoveredSourceGraph = true;
    options.completionOffset = input.byteOffset;
    options.workMetrics = input.workMetrics;
    options.cache = input.cache;
    options.sourceGenerations = input.sourceGenerations;
    options.sourceDigests = input.sourceDigests;
    const FrontendResult snapshot = Frontend(options).analyze(
        input.entryPath, input.source, input.preludePaths,
        input.sourceOverrides, input.standardLibraryRoots,
        input.packageSourceRoots);
    if (auto modules = completeModuleImport(snapshot, input))
      return std::move(*modules);
    const std::optional<SemanticCompletionContext> &context =
        snapshot.semantics.completionContext();
    const SourceUnit *completionUnit =
        snapshot.sourceGraph.findUnit(snapshot.sourceGraph.entryUnit());
    const bool inactiveSyntax =
        input.includeSnippets && snapshot.syntaxCompletion && completionUnit &&
        std::any_of(
            completionUnit->inactiveSpans.begin(),
            completionUnit->inactiveSpans.end(), [&](const SourceSpan &span) {
              return span.start <= snapshot.syntaxCompletion->token.position &&
                     snapshot.syntaxCompletion->token.position < span.end;
            });
    if (input.includeSnippets && snapshot.sourceValid &&
        snapshot.syntaxCompletion && !inactiveSyntax) {
      result.candidates =
          completionSnippets(*snapshot.syntaxCompletion, input.source);
      for (CompletionCandidate &candidate : result.candidates)
        candidate.sortText = completionSortText(99999, candidate.label);
    }
    if (!context) {
      return result;
    }

    const SemanticModel &semantics = snapshot.semantics;
    const SignaturePrinter signatures(semantics);
    const SemanticTypePrinter types(semantics);
    struct RankedCandidate {
      CompletionCandidate candidate;
      std::size_t score = 0;
    };
    std::vector<RankedCandidate> ranked;
    for (CompletionCandidate &candidate : result.candidates)
      ranked.push_back({.candidate = std::move(candidate), .score = 99999});
    result.candidates.clear();
    std::unordered_set<std::string> seen;
    for (const SemanticCompletionCandidateRecord &record :
         context->candidates) {
      if (!snapshot.sourceGraph.isCompilerTrusted(context->sourceUnit) &&
          (record.compilerPrivate ||
           semantics.isCompilerPrivateType(record.type))) {
        continue;
      }
      const SymbolRecord *symbol =
          record.symbol == 0 ? nullptr
                             : semantics.database().findSymbol(record.symbol);
      if (symbol != nullptr &&
          !semantics.canPresent(context->sourceUnit, *symbol,
                                snapshot.sourceGraph)) {
        continue;
      }
      const std::optional<std::size_t> prefixRank =
          completionPrefixRank(record.name, context->prefix);
      if (!prefixRank) {
        continue;
      }

      CompletionCandidate candidate{
          .kind = completionKind(record.kind),
          .label = record.name,
          .detail = completionDetail(record, semantics, signatures, types),
          .insertion = record.name,
          .snippet = completionSnippet(record, semantics),
          .replacementRange = context->replacementRange,
          .documentationMarkdown =
              symbol == nullptr ? std::nullopt : symbol->documentationMarkdown};
      const std::string duplicateKey =
          std::to_string(static_cast<int>(candidate.kind)) + '\n' +
          candidate.label + '\n' + candidate.detail;
      if (!seen.insert(duplicateKey).second) {
        continue;
      }
      const std::size_t score =
          *prefixRank * 100000 +
          std::min(record.scopeDistance, std::size_t{99}) * 1000 +
          completionKindRank(candidate.kind) * 10;
      candidate.sortText = completionSortText(score, candidate.label);
      ranked.push_back({.candidate = std::move(candidate), .score = score});
    }

    std::stable_sort(
        ranked.begin(), ranked.end(),
        [](const RankedCandidate &left, const RankedCandidate &right) {
          if (left.score != right.score) {
            return left.score < right.score;
          }
          if (left.candidate.label != right.candidate.label) {
            return left.candidate.label < right.candidate.label;
          }
          return left.candidate.detail < right.candidate.detail;
        });
    constexpr std::size_t candidateLimit = 100;
    result.isIncomplete = ranked.size() > candidateLimit;
    const std::size_t count = std::min(ranked.size(), candidateLimit);
    result.candidates.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      result.candidates.push_back(std::move(ranked[index].candidate));
    }
    return result;
  }

private:
  [[nodiscard]] static TypeSubstitution
  selectedCallTypeSubstitution(const SemanticModel &semantics,
                               const FunctionInfo &function,
                               const ResolvedCallInfo &selected) {
    TypeSubstitution substitution;
    if (selected.dispatchOwner.kind == SemanticType::Class) {
      if (const ClassTypeInfo *owner =
              semantics.findClassType(selected.dispatchOwner.classId)) {
        std::size_t typeIndex = 0;
        for (const GenericParameterInfo &parameter : owner->genericParameters) {
          if (parameter.value) {
            continue;
          }
          if (typeIndex >= selected.dispatchOwner.arguments.size()) {
            break;
          }
          substitution.emplace(parameter.id,
                               selected.dispatchOwner.arguments[typeIndex++]);
        }
      }
    }

    const std::size_t fixedGenericCount =
        !function.genericParameters.empty() &&
                function.genericParameters.back().pack
            ? function.genericParameters.size() - 1
            : function.genericParameters.size();
    const std::size_t count =
        std::min(fixedGenericCount, selected.typeArguments.size());
    for (std::size_t index = 0; index < count; ++index) {
      const GenericParameterInfo &parameter = function.genericParameters[index];
      if (!parameter.value) {
        substitution.emplace(parameter.id, selected.typeArguments[index]);
      }
    }
    return substitution;
  }

  [[nodiscard]] static SemanticType
  substituteSelectedCallType(const SemanticType &type,
                             const TypeSubstitution &substitution) {
    if (type.kind == SemanticType::TypeParameter) {
      const auto found = substitution.find(type.genericParameterId);
      return found == substitution.end() ? type : found->second;
    }

    SemanticType result = type;
    for (SemanticType &argument : result.arguments) {
      argument = substituteSelectedCallType(argument, substitution);
    }
    return result;
  }

  static void appendCallableContractNotes(HoverInfo &result,
                                          const SemanticModel &semantics,
                                          const FunctionInfo &function,
                                          const ResolvedCallInfo *selected,
                                          const SemanticTypePrinter &types) {
    const TypeSubstitution substitution =
        selected == nullptr
            ? TypeSubstitution{}
            : selectedCallTypeSubstitution(semantics, function, *selected);
    std::vector<const CallableParameterContract *> contracts;
    contracts.reserve(function.callableParameters.size());
    for (const CallableParameterContract &contract :
         function.callableParameters) {
      contracts.push_back(&contract);
    }
    std::stable_sort(contracts.begin(), contracts.end(),
                     [](const CallableParameterContract *left,
                        const CallableParameterContract *right) {
                       return left->parameterIndex < right->parameterIndex;
                     });

    for (const CallableParameterContract *contract : contracts) {
      const std::string_view boundary =
          contract->boundary == CallableBoundary::Confined ? "confined"
                                                           : "owned";
      std::string note(boundary);
      note += " callable parameter ";
      if (function.declaration != nullptr &&
          contract->parameterIndex <
              function.declaration->parameters().size() &&
          !function.declaration->parameters()[contract->parameterIndex]
               .name.lexeme.empty()) {
        note += "'" +
                function.declaration->parameters()[contract->parameterIndex]
                    .name.lexeme +
                "'";
      } else {
        note += "#" + std::to_string(contract->parameterIndex + 1);
      }
      if (contract->boundary == CallableBoundary::Owned) {
        note += " (explicit ownership move)";
      } else {
        const std::string_view access =
            contract->access == AccessMode::Mutable ? "mutable" : "read-only";
        note += " (" + std::string(access) + " access)";
      }

      std::vector<std::string> renderedSignatures;
      renderedSignatures.reserve(contract->signatures.size());
      bool deferredSignature = false;
      for (const CallableSignatureRequirement &signature :
           contract->signatures) {
        std::string rendered;
        switch (signature.capability) {
        case CallableInvocationCapability::Read:
          rendered = "read-callable (";
          break;
        case CallableInvocationCapability::Mutable:
          rendered = "mut-callable (";
          break;
        case CallableInvocationCapability::Once:
          rendered = "once-callable (";
          break;
        }
        for (std::size_t index = 0; index < signature.parameterTypes.size();
             ++index) {
          if (index != 0) {
            rendered += ", ";
          }
          const SemanticType parameter = substituteSelectedCallType(
              signature.parameterTypes[index], substitution);
          deferredSignature |= parameter == SemanticType::Unknown;
          rendered += parameter == SemanticType::Unknown
                          ? "resolved on instantiation"
                          : types.print(parameter);
        }
        deferredSignature |= signature.inferResultFromInstantiation;
        rendered += ") -> ";
        rendered += signature.inferResultFromInstantiation
                        ? "result from selected callable"
                        : types.print(substituteSelectedCallType(
                              signature.returnType, substitution));
        if (std::find(renderedSignatures.begin(), renderedSignatures.end(),
                      rendered) == renderedSignatures.end()) {
          renderedSignatures.push_back(std::move(rendered));
        }
      }

      if (contract->boundary == CallableBoundary::Owned &&
          contract->ownedTransport) {
        if (contract->ownedTransport->kind ==
            CallableOwnedTransportKind::ThreadTask) {
          note += ", exact transport: managed thread task (void(), once)";
        } else {
          note += ", exact transport: ";
          note += contract->ownedTransport->kind ==
                          CallableOwnedTransportKind::ExactReturn
                      ? "return "
                      : "field of ";
          note += types.print(substituteSelectedCallType(
              contract->ownedTransport->destinationType, substitution));
        }
      } else if (renderedSignatures.empty()) {
        note += contract->forwardings.empty()
                    ? "; no direct invocation signature"
                    : "; forwarded only";
      } else {
        if (deferredSignature)
          note += renderedSignatures.size() == 1
                      ? ", invocation requirement: "
                      : ", invocation requirements: ";
        else
          note += renderedSignatures.size() == 1 ? ", exact signature: "
                                                 : ", exact signatures: ";
        for (std::size_t index = 0; index < renderedSignatures.size();
             ++index) {
          if (index != 0) {
            note += "; ";
          }
          note += renderedSignatures[index];
        }
      }
      result.notes.push_back(std::move(note));
    }
  }

  [[nodiscard]] static bool sameSpan(const SourceSpan &left,
                                     const SourceSpan &right) {
    return left.source == right.source && left.start == right.start &&
           left.end == right.end;
  }

  // Several checked expansion-local symbols can denote one written binder.
  // Link only exact compiler-recorded declaration identity inside a retained
  // macro definition, never same-spelled caller names or unrelated scopes.
  static std::vector<SymbolId>
  writtenBindingSymbols(const FrontendResult &snapshot,
                        const SymbolRecord &symbol) {
    std::vector<SymbolId> result{symbol.id};
    if (symbol.kind != SymbolKind::LocalVariable || !symbol.expansion)
      return result;
    const auto sites = snapshot.semantics.macroExpansionSites();
    const bool macroLocal = std::any_of(
        sites.begin(), sites.end(), [&](const MacroExpansionSite &site) {
          if (!site.declaration || !site.declaration->extent())
            return false;
          const auto &extent = *site.declaration->extent();
          return extent.source == symbol.nameSpan.source &&
                 extent.start <= symbol.nameSpan.start &&
                 symbol.nameSpan.end <= extent.end;
        });
    if (!macroLocal)
      return result;
    for (const auto &candidate : snapshot.semantics.database().symbols())
      if (candidate.id != symbol.id && candidate.expansion &&
          candidate.kind == symbol.kind &&
          sameSpan(candidate.nameSpan, symbol.nameSpan) &&
          sameSpan(candidate.declarationSpan, symbol.declarationSpan))
        result.push_back(candidate.id);
    return result;
  }

  // Deduplicated, ordered occurrence sites for a set of snapshot symbols.
  // Sites the requesting unit may not present are dropped, so results fail
  // closed for compiler-private records.
  [[nodiscard]] static std::vector<ReferenceSite>
  collectSites(const FrontendResult &snapshot, SourceUnitId sourceUnit,
               const std::vector<SymbolId> &symbols) {
    const SemanticDatabase &database = snapshot.semantics.database();
    std::vector<ReferenceSite> sites;
    for (const SymbolId symbol : symbols) {
      for (const SemanticOccurrence *occurrence :
           database.occurrencesForSymbol(symbol)) {
        if (!snapshot.semantics.canPresent(sourceUnit, *occurrence,
                                           snapshot.sourceGraph)) {
          continue;
        }
        sites.push_back({.span = occurrence->span, .roles = occurrence->roles});
      }
    }
    std::sort(sites.begin(), sites.end(),
              [](const ReferenceSite &left, const ReferenceSite &right) {
                if (left.span.source != right.span.source) {
                  return left.span.source < right.span.source;
                }
                if (left.span.start != right.span.start) {
                  return left.span.start < right.span.start;
                }
                return left.span.end < right.span.end;
              });
    std::vector<ReferenceSite> merged;
    merged.reserve(sites.size());
    for (ReferenceSite &site : sites) {
      if (!merged.empty() && sameSpan(merged.back().span, site.span)) {
        merged.back().roles |= site.roles;
        continue;
      }
      merged.push_back(std::move(site));
    }
    return merged;
  }

  struct RenameCore {
    const SymbolRecord *symbol = nullptr;
    std::vector<SymbolId> closure;
    std::vector<ReferenceSite> sites;
    SourceSpan origin;
    std::string failure;
  };

  [[nodiscard]] static bool renamableKind(SymbolKind kind) {
    switch (kind) {
    case SymbolKind::LocalVariable:
    case SymbolKind::Parameter:
    case SymbolKind::RequirementParameter:
    case SymbolKind::TypeParameter:
    case SymbolKind::ValueParameter:
    case SymbolKind::LambdaCapture:
      return true;
    default:
      return false;
    }
  }

  // Resolves the symbol under the cursor and proves rename coverage. Only
  // function-local identities are accepted: every reference to them lies in
  // the current snapshot, so the editor cannot silently miss call sites in
  // unopened dependants. Copy-snapshot lambda captures couple the capture
  // target with its source binding, so the closure renames both together.
  [[nodiscard]] static RenameCore renameCore(const FrontendResult &snapshot,
                                             SourceUnitId sourceUnit,
                                             std::size_t byteOffset) {
    RenameCore core;
    const SemanticDatabase &database = snapshot.semantics.database();
    const SemanticOccurrence *occurrence =
        database.findOccurrence(sourceUnit, byteOffset);
    if (occurrence == nullptr || occurrence->symbol == 0 ||
        !snapshot.semantics.canPresent(sourceUnit, *occurrence,
                                       snapshot.sourceGraph)) {
      return core;
    }
    const SymbolRecord *symbol = database.findSymbol(occurrence->symbol);
    if (symbol == nullptr || !snapshot.semantics.canPresent(
                                 sourceUnit, *symbol, snapshot.sourceGraph)) {
      return core;
    }
    core.symbol = symbol;
    core.origin = occurrence->span;
    if (symbol->name.empty() || symbol->generated || symbol->compilerPrivate) {
      core.failure = "Compiler-generated names cannot be renamed.";
      return core;
    }
    if (symbol->defaultLibrary) {
      core.failure = "Standard library declarations cannot be renamed.";
      return core;
    }
    if (!renamableKind(symbol->kind)) {
      core.failure =
          "Rename is currently limited to function-local names; '" +
          symbol->name +
          "' may be referenced by source the current analysis cannot see.";
      return core;
    }

    std::vector<SymbolId> closure = writtenBindingSymbols(snapshot, *symbol);
    const std::vector<LambdaCaptureLink> links =
        snapshot.semantics.lambdaCaptureLinks();
    const auto contains = [&closure](SymbolId id) {
      return std::find(closure.begin(), closure.end(), id) != closure.end();
    };
    bool grew = true;
    while (grew) {
      grew = false;
      for (const LambdaCaptureLink &link : links) {
        if (link.mode != LambdaCaptureMode::Copy) {
          continue;
        }
        if (contains(link.source) && !contains(link.binding)) {
          closure.push_back(link.binding);
          grew = true;
        }
        if (contains(link.binding) && !contains(link.source)) {
          closure.push_back(link.source);
          grew = true;
        }
      }
    }
    for (const SymbolId member : closure) {
      const SymbolRecord *record = database.findSymbol(member);
      if (record == nullptr || record->generated || record->compilerPrivate ||
          record->defaultLibrary || !renamableKind(record->kind) ||
          record->name != symbol->name) {
        core.failure = "Rename coverage for '" + symbol->name +
                       "' is incomplete in this snapshot.";
        return core;
      }
    }

    core.closure = std::move(closure);
    core.sites = collectSites(snapshot, sourceUnit, core.closure);
    if (core.sites.empty()) {
      core.failure = "Rename coverage for '" + symbol->name +
                     "' is incomplete in this snapshot.";
      return core;
    }
    // Every edited range must spell exactly the current name; anything else
    // means the occurrence model and the source disagree, so fail closed
    // instead of producing a corrupting edit.
    for (const ReferenceSite &site : core.sites) {
      const std::string *text = snapshot.sources.find(site.span.source);
      if (text == nullptr || site.span.end > text->size() ||
          site.span.end - site.span.start != symbol->name.size() ||
          text->compare(site.span.start, symbol->name.size(), symbol->name) !=
              0) {
        core.failure = "Rename coverage for '" + symbol->name +
                       "' is incomplete in this snapshot.";
        return core;
      }
    }
    return core;
  }

  [[nodiscard]] static std::string validateNewName(std::string_view newName) {
    if (isCppReservedIdentifier(newName)) {
      return "'" + std::string(newName) +
             "' is a reserved C++ core keyword and cannot name a GTI "
             "declaration.";
    }
    Lexer lexer;
    std::vector<Token> tokens = lexer.scan(std::string(newName), "<rename>");
    const bool singleIdentifier =
        !lexer.hadError() && tokens.size() == 2 &&
        tokens.front().kind == TokenKind::IDENTIFIER &&
        tokens.front().lexeme == newName &&
        tokens.back().kind == TokenKind::END_OF_FILE;
    if (!singleIdentifier) {
      return "'" + std::string(newName) + "' is not a valid GTI identifier.";
    }
    return {};
  }

  // Conservative shadowing guard: reject a new name that is already visible
  // anywhere in the source units this rename touches. This over-rejects
  // unrelated scopes but can never silently rebind a reference.
  [[nodiscard]] static bool
  conflictsWithVisibleName(const FrontendResult &snapshot,
                           const RenameCore &core, std::string_view newName) {
    const SemanticDatabase &database = snapshot.semantics.database();
    const auto inClosure = [&core](SymbolId id) {
      return std::find(core.closure.begin(), core.closure.end(), id) !=
             core.closure.end();
    };
    std::unordered_set<SourceUnitId> units;
    for (const ReferenceSite &site : core.sites) {
      const SourceUnitId unit =
          snapshot.sourceGraph.sourceUnitForPath(site.span.source);
      if (unit == 0 || !units.insert(unit).second) {
        continue;
      }
      for (const SemanticOccurrence &occurrence : database.occurrences(unit)) {
        if (occurrence.name == newName && !inClosure(occurrence.symbol)) {
          return true;
        }
      }
    }
    for (const SymbolRecord &record : database.symbols()) {
      if (record.name == newName && units.contains(record.sourceUnit) &&
          !inClosure(record.id)) {
        return true;
      }
    }
    return false;
  }

  // Builds the outline for one source unit from the recovered AST and the
  // parser-recorded extents. Only declaration structure is read here; the
  // rendered detail reuses the hover signature path so both features present
  // the same compiler-owned facts.
  void appendOutline(const FrontendResult &snapshot, SourceUnitId sourceUnit,
                     const std::string &unitPath, const TargetInfo &target,
                     const StmtList &statements, bool insideClass,
                     std::vector<DocumentSymbolInfo> &result) const {
    for (const StmtPtr &statement : statements) {
      if (statement == nullptr) {
        continue;
      }
      if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
        if (conditional->directive().source != unitPath) {
          continue;
        }
        if (const StmtList *branch = conditional->activeBranch(target)) {
          appendOutline(snapshot, sourceUnit, unitPath, target, *branch,
                        insideClass, result);
        }
        continue;
      }
      if (const auto *group = stmtAs<MutableFieldGroupDecl>(statement.get())) {
        appendOutline(snapshot, sourceUnit, unitPath, target, group->members(),
                      insideClass, result);
        continue;
      }
      if (const auto *foreign = stmtAs<ExternCDecl>(statement.get())) {
        if (foreign->keyword().source != unitPath) {
          continue;
        }
        appendOutline(snapshot, sourceUnit, unitPath, target,
                      foreign->declarations(), insideClass, result);
        continue;
      }
      std::optional<DocumentSymbolInfo> info = outlineNode(
          snapshot, sourceUnit, unitPath, target, *statement, insideClass);
      if (info) {
        result.push_back(std::move(*info));
      }
    }
  }

  [[nodiscard]] std::optional<DocumentSymbolInfo>
  outlineNode(const FrontendResult &snapshot, SourceUnitId sourceUnit,
              const std::string &unitPath, const TargetInfo &target,
              const Stmt &statement, bool insideClass) const {
    const auto makeInfo =
        [&](const Token &name, DocumentSymbolKind kind,
            std::string displayName = {}) -> std::optional<DocumentSymbolInfo> {
      if (name.generated || name.lexeme.empty() || name.source != unitPath) {
        return std::nullopt;
      }
      DocumentSymbolInfo info;
      info.name = displayName.empty() ? name.lexeme : std::move(displayName);
      info.kind = kind;
      info.selectionRange = tokenSpan(name);
      info.range = info.selectionRange;
      if (const std::optional<SourceSpan> &extent = statement.extent();
          extent && extent->source == info.selectionRange.source &&
          extent->start <= info.selectionRange.start &&
          info.selectionRange.end <= extent->end) {
        info.range = *extent;
      }
      if (const std::optional<HoverInfo> rendered =
              hover(snapshot, sourceUnit, info.selectionRange.start)) {
        info.detail = rendered->signature;
      }
      return info;
    };

    if (const auto *namespaceDecl = stmtAs<NamespaceDecl>(&statement)) {
      // A conformance block's generated namespace outlines as the block,
      // with its associated types and operations as children.
      const ConformanceDecl *block =
          namespaceDecl->name().generated &&
                  !namespaceDecl->declarations().empty()
              ? stmtAs<ConformanceDecl>(
                    namespaceDecl->declarations().front().get())
              : nullptr;
      if (block != nullptr) {
        std::string spelling =
            "for concept " +
            semantic_spelling::pathSpelling(block->target().name) + "<";
        for (std::size_t index = 0; index < block->target().arguments.size();
             ++index) {
          spelling +=
              (index == 0 ? "" : ", ") + semantic_spelling::typeRefSpelling(
                                             block->target().arguments[index]);
        }
        std::optional<DocumentSymbolInfo> info =
            makeInfo(block->target().name.last(), DocumentSymbolKind::Concept,
                     spelling + ">");
        if (info) {
          appendOutline(snapshot, sourceUnit, unitPath, target,
                        block->members(), false, info->children);
        }
        return info;
      }
      std::optional<DocumentSymbolInfo> info =
          makeInfo(namespaceDecl->name(), DocumentSymbolKind::Namespace);
      if (info) {
        appendOutline(snapshot, sourceUnit, unitPath, target,
                      namespaceDecl->declarations(), false, info->children);
      }
      return info;
    }
    if (const auto *alias = stmtAs<NamespaceAliasDecl>(&statement)) {
      return makeInfo(alias->name(), DocumentSymbolKind::Namespace);
    }
    if (const auto *classDecl = stmtAs<ClassDecl>(&statement)) {
      DocumentSymbolKind kind = DocumentSymbolKind::Class;
      switch (classDecl->kind()) {
      case ClassKind::Class:
        kind = DocumentSymbolKind::Class;
        break;
      case ClassKind::Struct:
        kind = DocumentSymbolKind::Struct;
        break;
      case ClassKind::Interface:
        kind = DocumentSymbolKind::Interface;
        break;
      case ClassKind::Union:
        kind = DocumentSymbolKind::Union;
        break;
      }
      std::optional<DocumentSymbolInfo> info =
          makeInfo(classDecl->name(), kind);
      if (info) {
        if (classDecl->isExactSpecialization()) {
          if (const ClassTypeInfo *classType =
                  snapshot.semantics.findClassType(*classDecl)) {
            info->name = SemanticTypePrinter(snapshot.semantics)
                             .print(SemanticType::classType(classType->id));
          }
        }
        appendOutline(snapshot, sourceUnit, unitPath, target,
                      classDecl->members(), true, info->children);
      }
      return info;
    }
    if (const auto *enumDecl = stmtAs<EnumDecl>(&statement)) {
      std::optional<DocumentSymbolInfo> info =
          makeInfo(enumDecl->name(), DocumentSymbolKind::Enum);
      if (info) {
        for (const EnumeratorDecl &enumerator : enumDecl->enumerators()) {
          if (enumerator.name.generated || enumerator.name.lexeme.empty() ||
              enumerator.name.source != unitPath) {
            continue;
          }
          DocumentSymbolInfo child;
          child.name = enumerator.name.lexeme;
          child.kind = DocumentSymbolKind::Enumerator;
          child.selectionRange = tokenSpan(enumerator.name);
          child.range = child.selectionRange;
          if (const std::optional<HoverInfo> rendered =
                  hover(snapshot, sourceUnit, child.selectionRange.start)) {
            child.detail = rendered->signature;
          }
          info->children.push_back(std::move(child));
        }
      }
      return info;
    }
    if (const auto *function = stmtAs<FunctionDecl>(&statement)) {
      if (function->operatorName()) {
        return makeInfo(
            function->name(), DocumentSymbolKind::Operator,
            function->operatorName()->kind == OverloadedOperator::Conversion
                ? "operator " +
                      semantic_spelling::typeRefSpelling(function->returnType())
                : std::string(
                      operatorSourceSpelling(function->operatorName()->kind)));
      }
      return makeInfo(function->name(), insideClass
                                            ? DocumentSymbolKind::Method
                                            : DocumentSymbolKind::Function);
    }
    if (const auto *constructor = stmtAs<ConstructorDecl>(&statement)) {
      return makeInfo(constructor->name(), DocumentSymbolKind::Constructor);
    }
    if (const auto *destructor = stmtAs<DestructorDecl>(&statement)) {
      return makeInfo(destructor->name(), DocumentSymbolKind::Destructor,
                      "~" + destructor->name().lexeme);
    }
    if (const auto *variable = stmtAs<VariableDecl>(&statement)) {
      return makeInfo(variable->name(), insideClass
                                            ? DocumentSymbolKind::Field
                                            : DocumentSymbolKind::Variable);
    }
    if (const auto *alias = stmtAs<TypeAliasDecl>(&statement)) {
      return makeInfo(alias->name(), DocumentSymbolKind::TypeAlias);
    }
    if (const auto *conceptDecl = stmtAs<ConceptDecl>(&statement)) {
      return makeInfo(conceptDecl->name(), DocumentSymbolKind::Concept);
    }
    return std::nullopt;
  }

  [[nodiscard]] static const ConceptDecl *
  findConceptDeclaration(const StmtList &statements,
                         const SourceSpan &declarationSpan) {
    for (const StmtPtr &statement : statements) {
      if (const auto *declaration = stmtAs<ConceptDecl>(statement.get());
          declaration != nullptr &&
          sameSpan(tokenSpan(declaration->name()), declarationSpan)) {
        return declaration;
      }
      if (const auto *namespaceDeclaration =
              stmtAs<NamespaceDecl>(statement.get())) {
        if (const ConceptDecl *found = findConceptDeclaration(
                namespaceDeclaration->declarations(), declarationSpan)) {
          return found;
        }
      } else if (const auto *conditional =
                     stmtAs<ConditionalStmt>(statement.get())) {
        for (const ConditionalBranch &branch : conditional->branches()) {
          if (const ConceptDecl *found =
                  findConceptDeclaration(branch.statements, declarationSpan)) {
            return found;
          }
        }
      } else if (const auto *foreign = stmtAs<ExternCDecl>(statement.get())) {
        if (const ConceptDecl *found = findConceptDeclaration(
                foreign->declarations(), declarationSpan)) {
          return found;
        }
      }
    }
    return nullptr;
  }

  [[nodiscard]] static std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                     return static_cast<char>(std::tolower(character));
                   });
    return result;
  }

  [[nodiscard]] static std::optional<std::size_t>
  completionPrefixRank(std::string_view candidate, std::string_view prefix) {
    if (prefix.empty()) {
      return 1;
    }
    if (candidate == prefix) {
      return 0;
    }
    if (candidate.starts_with(prefix)) {
      return 1;
    }
    const std::string foldedCandidate = lower(candidate);
    const std::string foldedPrefix = lower(prefix);
    if (foldedCandidate.starts_with(foldedPrefix)) {
      return 2;
    }
    if (foldedCandidate.find(foldedPrefix) != std::string::npos) {
      return 3;
    }
    return std::nullopt;
  }

  [[nodiscard]] static CompletionCandidateKind
  completionKind(SemanticCompletionCandidateKind kind) {
    switch (kind) {
    case SemanticCompletionCandidateKind::Namespace:
      return CompletionCandidateKind::Namespace;
    case SemanticCompletionCandidateKind::TypeAlias:
      return CompletionCandidateKind::TypeAlias;
    case SemanticCompletionCandidateKind::Class:
      return CompletionCandidateKind::Class;
    case SemanticCompletionCandidateKind::Struct:
      return CompletionCandidateKind::Struct;
    case SemanticCompletionCandidateKind::Enum:
      return CompletionCandidateKind::Enum;
    case SemanticCompletionCandidateKind::Enumerator:
      return CompletionCandidateKind::Enumerator;
    case SemanticCompletionCandidateKind::Function:
      return CompletionCandidateKind::Function;
    case SemanticCompletionCandidateKind::Method:
      return CompletionCandidateKind::Method;
    case SemanticCompletionCandidateKind::Field:
      return CompletionCandidateKind::Field;
    case SemanticCompletionCandidateKind::Parameter:
    case SemanticCompletionCandidateKind::ValueParameter:
      return CompletionCandidateKind::Parameter;
    case SemanticCompletionCandidateKind::TypeParameter:
      return CompletionCandidateKind::TypeParameter;
    case SemanticCompletionCandidateKind::GlobalVariable:
    case SemanticCompletionCandidateKind::LocalVariable:
      return CompletionCandidateKind::Variable;
    }
    return CompletionCandidateKind::Variable;
  }

  [[nodiscard]] static std::size_t
  completionKindRank(CompletionCandidateKind kind) {
    switch (kind) {
    case CompletionCandidateKind::Parameter:
    case CompletionCandidateKind::Variable:
    case CompletionCandidateKind::Field:
      return 0;
    case CompletionCandidateKind::Method:
    case CompletionCandidateKind::Function:
      return 1;
    case CompletionCandidateKind::Class:
    case CompletionCandidateKind::Struct:
    case CompletionCandidateKind::Enum:
    case CompletionCandidateKind::TypeAlias:
    case CompletionCandidateKind::TypeParameter:
      return 2;
    case CompletionCandidateKind::Enumerator:
      return 3;
    case CompletionCandidateKind::Namespace:
      return 4;
    case CompletionCandidateKind::Snippet:
      return 5;
    }
    return 5;
  }

  [[nodiscard]] static std::string
  completionDetail(const SemanticCompletionCandidateRecord &record,
                   const SemanticModel &semantics,
                   const SignaturePrinter &signatures,
                   const SemanticTypePrinter &types) {
    if (!record.detail.empty()) {
      return record.detail;
    }
    if (record.function != 0) {
      if (const FunctionInfo *function =
              semantics.findFunction(record.function)) {
        if (record.substitutedCallable) {
          ResolvedCallInfo selected{.function = record.function,
                                    .declaration = function->declaration,
                                    .returnType = record.type,
                                    .parameterTypes = record.parameterTypes,
                                    .requirements = record.requirements};
          return signatures.function(*function, &selected);
        }
        return signatures.function(*function);
      }
    }
    if (record.classType != 0) {
      if (const ClassTypeInfo *type =
              semantics.findClassType(record.classType)) {
        return signatures.classType(*type);
      }
    }
    if (record.enumType != 0) {
      if (record.kind == SemanticCompletionCandidateKind::Enumerator &&
          record.type.kind == SemanticType::Enum) {
        if (auto type = semantics.enumTypeFor(record.type)) {
          type->qualifiedName = types.print(record.type);
          return signatures.enumerator(*type, record.qualifiedName);
        }
      }
      if (const EnumTypeInfo *type = semantics.findEnumType(record.enumType)) {
        if (record.kind == SemanticCompletionCandidateKind::Enumerator) {
          return signatures.enumerator(*type, record.qualifiedName);
        }
        return signatures.enumType(*type);
      }
    }
    if (record.typeAlias != nullptr) {
      if (const TypeAliasInfo *alias =
              semantics.findTypeAlias(*record.typeAlias)) {
        return signatures.typeAlias(*alias);
      }
    }
    if (record.kind == SemanticCompletionCandidateKind::Namespace) {
      return "namespace " + record.qualifiedName;
    }
    const bool typeCarriesMutability =
        record.type.kind == SemanticType::Reference &&
        record.type.referenceAccess == AccessMode::Mutable;
    std::string prefix = record.staticMember ? "static " : "";
    if (record.threadLocal)
      prefix += "thread_local ";
    if (record.mutableBinding && !typeCarriesMutability) {
      prefix += "mut ";
    }
    return prefix + types.print(record.type) + " " + record.name;
  }

  [[nodiscard]] static std::optional<std::string>
  completionSnippet(const SemanticCompletionCandidateRecord &record,
                    const SemanticModel &semantics) {
    if (record.kind != SemanticCompletionCandidateKind::Function &&
        record.kind != SemanticCompletionCandidateKind::Method) {
      return std::nullopt;
    }
    const FunctionInfo *function =
        record.function == 0 ? nullptr
                             : semantics.findFunction(record.function);
    if (function == nullptr || function->declaration == nullptr) {
      return record.detail.find('(') == std::string::npos
                 ? std::nullopt
                 : std::optional<std::string>(record.name + "()");
    }
    std::string result = record.name + '(';
    const std::vector<Parameter> &parameters =
        function->declaration->parameters();
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      if (index != 0) {
        result += ", ";
      }
      result += "${" + std::to_string(index + 1) + ":";
      result += parameters[index].name.lexeme.empty()
                    ? "value"
                    : parameters[index].name.lexeme;
      result += '}';
    }
    result += ')';
    return result;
  }

  [[nodiscard]] static std::string completionSortText(std::size_t score,
                                                      std::string_view label) {
    std::ostringstream stream;
    stream << std::setfill('0') << std::setw(8) << score << ':' << label;
    return stream.str();
  }
};

std::optional<HoverInfo> LanguageQueries::hover(const FrontendResult &snapshot,
                                                SourceUnitId sourceUnit,
                                                std::size_t byteOffset) const {
  if (const auto module = moduleAt(snapshot, sourceUnit, byteOffset)) {
    return HoverInfo{.range = module->origin,
                     .signature = "export module " +
                                  module->interface->moduleDeclaration->lexeme +
                                  ";"};
  }
  return LanguageQueriesImpl().hover(snapshot, sourceUnit, byteOffset);
}

std::optional<MacroExpansionInfo>
LanguageQueries::macroExpansion(const FrontendResult &snapshot,
                                SourceUnitId sourceUnit,
                                std::size_t byteOffset) const {
  const SourceUnit *unit = snapshot.sourceGraph.findUnit(sourceUnit);
  if (!unit)
    return std::nullopt;
  const std::string *source = snapshot.sources.find(unit->path.string());
  if (!source || byteOffset >= source->size())
    return std::nullopt;
  std::optional<MacroExpansionSite> selected;
  SourceSpan selectedSpan;
  bool ambiguous = false;
  for (const auto &site : snapshot.semantics.macroExpansionSites()) {
    if ((!site.invocation && !site.call) || !site.body || !site.declaration)
      continue;
    const auto *call = site.call
                           ? site.call
                           : exprAs<Call>(site.invocation->expression().get());
    if (!call)
      continue;
    if (!site.invocation && !call->writtenOrigin())
      continue;
    SourceSpan span = site.invocation
                          ? tokenSpan(site.invocation->expressionStart())
                          : *call->writtenOrigin();
    span.end = tokenSpan(call->paren()).end;
    if (span.source != unit->path.string() || span.end > source->size() ||
        span.start > byteOffset || byteOffset >= span.end)
      continue;
    const std::size_t length = span.end - span.start;
    if (!selected || length < selectedSpan.end - selectedSpan.start) {
      selected = site;
      selectedSpan = std::move(span);
      ambiguous = false;
    } else if (span == selectedSpan && site.body != selected->body) {
      // A written call instantiated with several concrete meanings has no
      // single honest expansion to display. Never select by map/pointer order.
      ambiguous = true;
    }
  }
  if (!selected || ambiguous)
    return std::nullopt;
  MacroExpansionDisplay display(snapshot.semantics);
  std::string text = display.print(*selected->body);
  return MacroExpansionInfo{.invocation = std::move(selectedSpan),
                            .declaration =
                                tokenSpan(selected->declaration->name()),
                            .text = std::move(text),
                            .truncated = display.truncated()};
}

std::optional<DefinitionInfo>
LanguageQueries::definition(const FrontendResult &snapshot,
                            SourceUnitId sourceUnit,
                            std::size_t byteOffset) const {
  if (const auto module = moduleAt(snapshot, sourceUnit, byteOffset))
    return DefinitionInfo{.origin = module->origin,
                          .target = moduleNameSpan(*module->interface)};
  return LanguageQueriesImpl().definition(snapshot, sourceUnit, byteOffset);
}

std::optional<ReferencesInfo>
LanguageQueries::references(const FrontendResult &snapshot,
                            SourceUnitId sourceUnit, std::size_t byteOffset,
                            bool includeDeclaration) const {
  if (const auto module = moduleAt(snapshot, sourceUnit, byteOffset)) {
    ReferencesInfo result;
    if (includeDeclaration)
      result.sites.push_back(
          {moduleNameSpan(*module->interface), OccurrenceRole::Declaration});
    for (const auto &import : snapshot.sourceGraph.imports())
      if (import.target && *import.target == module->interface->id &&
          import.resolution == SourceImportResolution::Resolved)
        result.sites.push_back({import.name, OccurrenceRole::Reference});
    for (const auto &unit : snapshot.sourceGraph.sourceUnits())
      if (unit.implementationDeclaration && unit.moduleId != 0 &&
          unit.moduleId == module->interface->moduleId)
        result.sites.push_back({unit.implementationDeclarationSpan.value_or(
                                    tokenSpan(*unit.implementationDeclaration)),
                                OccurrenceRole::Reference});
    std::sort(
        result.sites.begin(), result.sites.end(),
        [](const auto &left, const auto &right) {
          return std::tie(left.span.source, left.span.start, left.span.end) <
                 std::tie(right.span.source, right.span.start, right.span.end);
        });
    return result;
  }
  return LanguageQueriesImpl().references(snapshot, sourceUnit, byteOffset,
                                          includeDeclaration);
}

std::optional<RenamePreparation>
LanguageQueries::prepareRename(const FrontendResult &snapshot,
                               SourceUnitId sourceUnit,
                               std::size_t byteOffset) const {
  return LanguageQueriesImpl().prepareRename(snapshot, sourceUnit, byteOffset);
}

RenameOutcome LanguageQueries::rename(const FrontendResult &snapshot,
                                      SourceUnitId sourceUnit,
                                      std::size_t byteOffset,
                                      std::string_view newName) const {
  return LanguageQueriesImpl().rename(snapshot, sourceUnit, byteOffset,
                                      newName);
}

std::vector<DocumentSymbolInfo>
LanguageQueries::documentSymbols(const FrontendResult &snapshot,
                                 SourceUnitId sourceUnit) const {
  return LanguageQueriesImpl().documentSymbols(snapshot, sourceUnit);
}

std::optional<SignatureHelpInfo>
LanguageQueries::signatureHelp(const FrontendResult &snapshot,
                               SourceUnitId sourceUnit,
                               std::size_t byteOffset) const {
  return LanguageQueriesImpl().signatureHelp(snapshot, sourceUnit, byteOffset);
}

CompletionResult LanguageQueries::complete(const CompletionInput &input) const {
  return LanguageQueriesImpl().complete(input);
}

} // namespace lang
