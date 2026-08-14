#include "gti/language_queries.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lang {

SignaturePrinter::SignaturePrinter(const SemanticModel &semantics)
    : semantics(semantics), types(semantics) {}

[[nodiscard]] std::string
SignaturePrinter::function(const FunctionInfo &info,
                           const ResolvedCallInfo *selected) const {
  const FunctionDecl *declaration = info.declaration;
  const SemanticType &returnType =
      selected == nullptr ? info.returnType : selected->returnType;
  const std::vector<SemanticType> &parameterTypes =
      selected == nullptr ? info.parameterTypes : selected->parameterTypes;
  std::string result =
      info.declaration != nullptr && info.declaration->isStatic() ? "static "
                                                                  : "";
  result += types.print(returnType) + " " + functionName(info);
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

[[nodiscard]] std::string
SignaturePrinter::conceptSignature(const SymbolRecord &symbol,
                                   const ConceptDecl *declaration) const {
  std::string result = "concept " + symbol.qualifiedName;
  if (declaration == nullptr) {
    return result;
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
    for (std::size_t index = 0; index < declaration->requirements().size();
         ++index) {
      if (index != 0) {
        result += " && ";
      }
      appendConceptApplication(result, declaration->requirements()[index]);
    }
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
  std::string result = constructedType;
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
  return result;
}

[[nodiscard]] std::string
SignaturePrinter::constructor(const ClassTypeInfo &owner,
                              const ConstructorInfo &info) const {
  if (info.kind != ConstructorKind::Ordinary) {
    return owner.qualifiedName + "(" + owner.qualifiedName +
           (info.kind == ConstructorKind::Move ? "&&)" : "&)");
  }
  std::string result = owner.qualifiedName;
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
  return result;
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
  result += info.qualifiedName;
  if (!info.genericParameters.empty()) {
    result += '<';
    appendGenericParameters(result, info.genericParameters);
    result += '>';
  }
  if (info.cOpaqueHandle) {
    result += ';';
  }
  return result;
}

[[nodiscard]] std::string
SignaturePrinter::enumType(const EnumTypeInfo &info) const {
  std::string result = "enum class " + info.qualifiedName;
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
  if (occurrence.mutableBinding && !typeCarriesMutability) {
    prefix += "mut ";
  }
  return prefix + types.print(occurrence.type) + " " + occurrence.name;
}

[[nodiscard]] std::string
SignaturePrinter::destructor(const DestructorInfo &info) const {
  const ClassTypeInfo *owner = semantics.findClassType(info.owner);
  if (owner == nullptr || owner->declaration == nullptr) {
    return "destructor";
  }
  return owner->qualifiedName + "::~" + owner->declaration->name().lexeme +
         "()";
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
    if (preservePackSyntax && parameter != nullptr && parameter->pack) {
      result += "...";
    }
    if (parameter != nullptr && !parameter->name.lexeme.empty()) {
      result += " " + parameter->name.lexeme;
    }
  }
}

void SignaturePrinter::appendConceptApplication(
    std::string &result, const ConceptApplication &application) {
  result += path(application.name);
  result += '<';
  for (std::size_t index = 0; index < application.arguments.size(); ++index) {
    if (index != 0) {
      result += ", ";
    }
    result += application.arguments[index].lexeme;
  }
  result += '>';
}

void SignaturePrinter::appendRequirements(
    std::string &result, const FunctionInfo &info,
    const ResolvedCallInfo *selected) const {
  if (info.declaration == nullptr || !info.declaration->requiresClause()) {
    return;
  }
  const std::vector<ConceptApplication> &syntax =
      info.declaration->requiresClause()->requirements;
  for (std::size_t index = 0; index < syntax.size(); ++index) {
    const AppliedConceptRequirement *resolvedRequirement = nullptr;
    const std::vector<AppliedConceptRequirement> &requirements =
        selected == nullptr ? info.requirements : selected->requirements;
    for (const AppliedConceptRequirement &requirement : requirements) {
      if (requirement.syntax == &syntax[index]) {
        resolvedRequirement = &requirement;
        break;
      }
    }
    result += index == 0 ? " requires " : " && ";
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
                      ? syntax[index].arguments[argumentIndex].lexeme
                      : printed;
      } else {
        result += syntax[index].arguments[argumentIndex].lexeme;
      }
    }
    result += '>';
  }
}

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
      const FunctionInfo *function = semantics.findFunction(selected.function);
      if (function != nullptr) {
        result.signature = signatures.function(*function, &selected);
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
    case SemanticOccurrenceKind::Binding:
      result.signature = signatures.binding(*occurrence);
      break;
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
      case SymbolKind::Enumerator:
        if (symbol->type.kind == SemanticType::Enum) {
          if (const EnumTypeInfo *owner =
                  semantics.findEnumType(symbol->type.enumId)) {
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
      case SymbolKind::ValueParameter:
        result.signature = "uint64_t " + symbol->name;
        break;
      default:
        if (symbol->type != SemanticType::Unknown) {
          result.signature = types.print(symbol->type) + " " + symbol->name;
        }
        break;
      }
      break;
    }
    case SemanticOccurrenceKind::InferredType:
    case SemanticOccurrenceKind::Expression:
      result.signature = types.print(occurrence->type);
      break;
    }
    if (result.signature.empty() || result.signature == "unknown") {
      return std::nullopt;
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
        } else if (policy == ConcurrencyCapabilityPolicy::Required) {
          note += " (interface requirement)";
        }
        return note;
      };
      if (type != nullptr) {
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

  [[nodiscard]] CompletionResult complete(const CompletionInput &input) const {
    CompletionResult result;
    if (input.entryPath.empty() || input.byteOffset > input.source.size()) {
      return result;
    }

    FrontendOptions options;
    options.analyzeRecoveredProgram = true;
    options.completionOffset = input.byteOffset;
    FrontendResult snapshot = Frontend(options).analyze(
        input.entryPath, input.source, input.preludePaths,
        input.sourceOverrides, input.standardLibraryRoots,
        input.packageSourceRoots);
    const std::optional<SemanticCompletionContext> &context =
        snapshot.semantics.completionContext();
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
    std::unordered_set<std::string> seen;
    for (const SemanticCompletionCandidateRecord &record :
         context->candidates) {
      if (!snapshot.sourceGraph.isCompilerTrusted(context->sourceUnit) &&
          (record.compilerPrivate ||
           semantics.isCompilerPrivateType(record.type))) {
        continue;
      }
      if (record.symbol != 0) {
        if (const SymbolRecord *symbol =
                semantics.database().findSymbol(record.symbol);
            symbol != nullptr &&
            !semantics.canPresent(context->sourceUnit, *symbol,
                                  snapshot.sourceGraph)) {
          continue;
        }
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
          .replacementRange = context->replacementRange};
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
          rendered += types.print(substituteSelectedCallType(
              signature.parameterTypes[index], substitution));
        }
        rendered += ") -> " + types.print(substituteSelectedCallType(
                                  signature.returnType, substitution));
        if (std::find(renderedSignatures.begin(), renderedSignatures.end(),
                      rendered) == renderedSignatures.end()) {
          renderedSignatures.push_back(std::move(rendered));
        }
      }

      if (contract->boundary == CallableBoundary::Owned &&
          contract->ownedTransport) {
        note += ", exact transport: ";
        note += contract->ownedTransport->kind ==
                        CallableOwnedTransportKind::ExactReturn
                    ? "return "
                    : "field of ";
        note += types.print(substituteSelectedCallType(
            contract->ownedTransport->destinationType, substitution));
      } else if (renderedSignatures.empty()) {
        note += contract->forwardings.empty()
                    ? "; no direct invocation signature"
                    : "; forwarded only";
      } else {
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

  [[nodiscard]] static const ConceptDecl *
  findConceptDeclaration(const StmtList &statements,
                         const SourceSpan &declarationSpan) {
    for (const StmtPtr &statement : statements) {
      if (const auto *declaration =
              dynamic_cast<const ConceptDecl *>(statement.get());
          declaration != nullptr &&
          sameSpan(tokenSpan(declaration->name()), declarationSpan)) {
        return declaration;
      }
      if (const auto *namespaceDeclaration =
              dynamic_cast<const NamespaceDecl *>(statement.get())) {
        if (const ConceptDecl *found = findConceptDeclaration(
                namespaceDeclaration->declarations(), declarationSpan)) {
          return found;
        }
      } else if (const auto *conditional =
                     dynamic_cast<const ConditionalStmt *>(statement.get())) {
        for (const ConditionalBranch &branch : conditional->branches()) {
          if (const ConceptDecl *found =
                  findConceptDeclaration(branch.statements, declarationSpan)) {
            return found;
          }
        }
      } else if (const auto *foreign =
                     dynamic_cast<const ExternCDecl *>(statement.get())) {
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
  return LanguageQueriesImpl().hover(snapshot, sourceUnit, byteOffset);
}

std::optional<DefinitionInfo>
LanguageQueries::definition(const FrontendResult &snapshot,
                            SourceUnitId sourceUnit,
                            std::size_t byteOffset) const {
  return LanguageQueriesImpl().definition(snapshot, sourceUnit, byteOffset);
}

CompletionResult LanguageQueries::complete(const CompletionInput &input) const {
  return LanguageQueriesImpl().complete(input);
}

} // namespace lang
