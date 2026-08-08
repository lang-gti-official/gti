#pragma once

#include "gti/frontend.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

class SignaturePrinter {
public:
  explicit SignaturePrinter(const SemanticModel &semantics)
      : semantics(semantics), types(semantics) {}

  [[nodiscard]] std::string
  function(const FunctionInfo &info,
           const ResolvedCallInfo *selected = nullptr) const {
    const FunctionDecl *declaration = info.declaration;
    const SemanticType &returnType =
        selected == nullptr ? info.returnType : selected->returnType;
    const std::vector<SemanticType> &parameterTypes =
        selected == nullptr ? info.parameterTypes : selected->parameterTypes;
    std::string result = types.print(returnType) + " " + functionName(info);
    if (selected != nullptr && !selected->typeArguments.empty()) {
      result += '<';
      appendTypes(result, selected->typeArguments);
      result += '>';
    } else if (selected == nullptr && !info.genericParameters.empty()) {
      result += '<';
      appendGenericParameters(result, info.genericParameters);
      result += '>';
    }
    result += '(';
    appendParameters(result, parameterTypes,
                     declaration == nullptr ? nullptr
                                            : &declaration->parameters());
    result += ')';
    if (declaration != nullptr &&
        declaration->receiverMutability() == ReceiverMutability::Mutable) {
      result += " mut";
    }
    return result;
  }

  [[nodiscard]] std::string
  constructor(const ClassTypeInfo &owner,
              const ResolvedConstructionInfo &selected) const {
    const std::string constructedType =
        selected.constructedType.kind == SemanticType::Class
            ? types.print(selected.constructedType)
            : owner.qualifiedName;
    std::string result = constructedType + '(';
    const std::vector<Parameter> *parameters =
        selected.declaration == nullptr ? nullptr
                                        : &selected.declaration->parameters();
    appendParameters(result, selected.parameterTypes, parameters);
    result += ')';
    return result;
  }

  [[nodiscard]] std::string constructor(const ClassTypeInfo &owner,
                                        const ConstructorInfo &info) const {
    std::string result = owner.qualifiedName + '(';
    appendParameters(result, info.parameterTypes,
                     info.declaration == nullptr
                         ? nullptr
                         : &info.declaration->parameters());
    result += ')';
    return result;
  }

  [[nodiscard]] std::string classType(const ClassTypeInfo &info) const {
    const ClassKind kind = info.declaration == nullptr
                               ? ClassKind::Class
                               : info.declaration->kind();
    std::string result =
        std::string(kind == ClassKind::Struct ? "struct " : "class ") +
        info.qualifiedName;
    if (!info.genericParameters.empty()) {
      result += '<';
      appendGenericParameters(result, info.genericParameters);
      result += '>';
    }
    return result;
  }

  [[nodiscard]] std::string enumType(const EnumTypeInfo &info) const {
    return "enum class " + info.qualifiedName + " : " +
           types.print(info.underlyingType);
  }

  [[nodiscard]] std::string typeAlias(const TypeAliasInfo &info) const {
    return "using " + info.qualifiedName + " = " + types.print(info.type);
  }

  [[nodiscard]] std::string
  binding(const SemanticOccurrence &occurrence) const {
    const bool typeCarriesMutability =
        occurrence.type.kind == SemanticType::Reference &&
        occurrence.type.referenceAccess == AccessMode::Mutable;
    const std::string prefix =
        occurrence.mutableBinding && !typeCarriesMutability ? "mut " : "";
    return prefix + types.print(occurrence.type) + " " + occurrence.name;
  }

  [[nodiscard]] std::string destructor(const DestructorInfo &info) const {
    const ClassTypeInfo *owner = semantics.findClassType(info.owner);
    if (owner == nullptr || owner->declaration == nullptr) {
      return "destructor";
    }
    return owner->qualifiedName + "::~" + owner->declaration->name().lexeme +
           "()";
  }

private:
  [[nodiscard]] static std::string path(const NamePath &name) {
    std::string result;
    for (const Token &segment : name.segments) {
      if (!result.empty()) {
        result += "::";
      }
      result += segment.lexeme;
    }
    return result;
  }

  [[nodiscard]] std::string functionName(const FunctionInfo &info) const {
    if (info.declaration == nullptr || !info.declaration->operatorName()) {
      return info.qualifiedName;
    }
    const std::size_t separator = info.qualifiedName.rfind("::");
    const std::string scope = separator == std::string::npos
                                  ? std::string{}
                                  : info.qualifiedName.substr(0, separator + 2);
    return scope + std::string(operatorSourceSpelling(
                       info.declaration->operatorName()->kind));
  }

  void appendTypes(std::string &result,
                   const std::vector<SemanticType> &arguments) const {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      if (index != 0) {
        result += ", ";
      }
      result += types.print(arguments[index]);
    }
  }

  void appendGenericParameters(
      std::string &result,
      const std::vector<GenericParameterInfo> &parameters) const {
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      if (index != 0) {
        result += ", ";
      }
      const GenericParameterInfo &parameter = parameters[index];
      if (parameter.value) {
        result += "uint64 ";
      } else if (parameter.constraintName) {
        result += path(*parameter.constraintName) + " ";
      }
      result += parameter.name.lexeme;
      if (parameter.pack) {
        result += "...";
      }
    }
  }

  void appendParameters(std::string &result,
                        const std::vector<SemanticType> &parameterTypes,
                        const std::vector<Parameter> *parameters) const {
    for (std::size_t index = 0; index < parameterTypes.size(); ++index) {
      if (index != 0) {
        result += ", ";
      }
      const Parameter *parameter =
          parameters != nullptr && index < parameters->size()
              ? &(*parameters)[index]
              : nullptr;
      if (parameter != nullptr &&
          parameter->mutability == Mutability::Mutable &&
          !(parameterTypes[index].kind == SemanticType::Reference &&
            parameterTypes[index].referenceAccess == AccessMode::Mutable)) {
        result += "mut ";
      }
      result += types.print(parameterTypes[index]);
      if (parameter != nullptr && !parameter->name.lexeme.empty()) {
        result += " " + parameter->name.lexeme;
      }
    }
  }

  const SemanticModel &semantics;
  SemanticTypePrinter types;
};

struct HoverInfo {
  SourceSpan range;
  std::string signature;
  std::optional<std::string> documentationMarkdown;
  std::vector<std::string> notes;
};

class LanguageQueries {
public:
  [[nodiscard]] std::optional<HoverInfo> hover(const FrontendResult &snapshot,
                                               SourceUnitId sourceUnit,
                                               std::size_t byteOffset) const {
    const SemanticOccurrence *occurrence =
        snapshot.semantics.database().findOccurrence(sourceUnit, byteOffset);
    if (occurrence == nullptr) {
      return std::nullopt;
    }

    const SemanticModel &semantics = snapshot.semantics;
    const SignaturePrinter signatures(semantics);
    const SemanticTypePrinter types(semantics);
    HoverInfo result{.range = occurrence->span};
    switch (occurrence->kind) {
    case SemanticOccurrenceKind::SelectedCall: {
      const ResolvedCallInfo &selected = *occurrence->selectedCall;
      const FunctionInfo *function = semantics.findFunction(selected.function);
      if (function != nullptr) {
        result.signature = signatures.function(*function, &selected);
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
      }
      break;
    }
    case SemanticOccurrenceKind::Function:
      if (occurrence->function != nullptr) {
        if (const FunctionInfo *function =
                semantics.findFunction(*occurrence->function)) {
          result.signature = signatures.function(*function);
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
    case SemanticOccurrenceKind::Binding:
      result.signature = signatures.binding(*occurrence);
      break;
    case SemanticOccurrenceKind::InferredType:
    case SemanticOccurrenceKind::Expression:
      result.signature = types.print(occurrence->type);
      break;
    }
    if (result.signature.empty() || result.signature == "unknown") {
      return std::nullopt;
    }
    if (occurrence->traits.ownership == OwnershipKind::Unique) {
      result.notes.emplace_back("move-only owner");
    } else if (occurrence->access == AccessMode::Mutable) {
      result.notes.emplace_back("mutable place");
    }
    return result;
  }
};

} // namespace lang
