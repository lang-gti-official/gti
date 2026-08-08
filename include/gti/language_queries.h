#pragma once

#include "gti/frontend.h"

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
    std::string result =
        info.declaration != nullptr && info.declaration->isStatic() ? "static "
                                                                    : "";
    result += types.print(returnType) + " " + functionName(info);
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
    if (declaration != nullptr && !declaration->isStatic() &&
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
    std::string prefix = occurrence.staticMember ? "static " : "";
    if (occurrence.mutableBinding && !typeCarriesMutability) {
      prefix += "mut ";
    }
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

struct DefinitionInfo {
  SymbolId symbol = 0;
  SourceSpan origin;
  SourceSpan target;
};

enum class CompletionCandidateKind {
  Namespace,
  TypeAlias,
  Class,
  Struct,
  Enum,
  Enumerator,
  Function,
  Method,
  Field,
  Variable,
  Parameter,
  TypeParameter,
};

struct CompletionCandidate {
  CompletionCandidateKind kind = CompletionCandidateKind::Variable;
  std::string label;
  std::string detail;
  std::string insertion;
  std::optional<std::string> snippet;
  std::string sortText;
  SourceSpan replacementRange;
};

struct CompletionResult {
  std::vector<CompletionCandidate> candidates;
  bool isIncomplete = false;
};

struct CompletionInput {
  std::filesystem::path entryPath;
  std::string source;
  std::size_t byteOffset = 0;
  std::vector<std::filesystem::path> preludePaths;
  std::unordered_map<std::string, std::string> sourceOverrides;
  std::vector<std::filesystem::path> standardLibraryRoots;
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
      case SymbolKind::Enumerator:
        result.signature =
            types.print(symbol->type) + " " + symbol->qualifiedName;
        break;
      case SymbolKind::TypeParameter:
        result.signature = "type parameter " + symbol->name;
        break;
      case SymbolKind::ValueParameter:
        result.signature = "uint64 " + symbol->name;
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
    if (occurrence->traits.ownership == OwnershipKind::Unique) {
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
    const SymbolRecord *symbol = database.findSymbol(occurrence->symbol);
    if (symbol == nullptr) {
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
        input.sourceOverrides, input.standardLibraryRoots);
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
                                    .parameterTypes = record.parameterTypes};
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
          return type->qualifiedName + " " + record.qualifiedName;
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

} // namespace lang
