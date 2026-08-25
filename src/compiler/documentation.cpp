#include "gti/documentation.h"

#include "gti/language_queries.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lang {
namespace {

[[nodiscard]] std::optional<DocumentationKind>
documentationKind(const SymbolRecord &symbol,
                  const SemanticOccurrence *declaration,
                  const SemanticModel &semantics) {
  switch (symbol.kind) {
  case SymbolKind::Namespace:
    return DocumentationKind::Namespace;
  case SymbolKind::NamespaceAlias:
    return DocumentationKind::NamespaceAlias;
  case SymbolKind::Concept:
    return DocumentationKind::Concept;
  case SymbolKind::TypeAlias:
    return DocumentationKind::TypeAlias;
  case SymbolKind::Class:
  case SymbolKind::Struct:
    if (declaration != nullptr && declaration->classType != nullptr) {
      if (const ClassTypeInfo *type =
              semantics.findClassType(*declaration->classType)) {
        switch (type->kind) {
        case ClassKind::Class:
          return DocumentationKind::Class;
        case ClassKind::Struct:
          return DocumentationKind::Struct;
        case ClassKind::Interface:
          return DocumentationKind::Interface;
        case ClassKind::Union:
          return DocumentationKind::Union;
        }
      }
    }
    return symbol.kind == SymbolKind::Struct ? DocumentationKind::Struct
                                             : DocumentationKind::Class;
  case SymbolKind::Enum:
    return DocumentationKind::Enum;
  case SymbolKind::Enumerator:
    return DocumentationKind::Enumerator;
  case SymbolKind::Constructor:
    return DocumentationKind::Constructor;
  case SymbolKind::Destructor:
    return DocumentationKind::Destructor;
  case SymbolKind::Function:
    return DocumentationKind::Function;
  case SymbolKind::Method:
    return DocumentationKind::Method;
  case SymbolKind::Operator:
    return DocumentationKind::Operator;
  case SymbolKind::Field:
    return DocumentationKind::Field;
  case SymbolKind::GlobalVariable:
    return DocumentationKind::GlobalVariable;
  case SymbolKind::LocalVariable:
  case SymbolKind::Parameter:
  case SymbolKind::TypeParameter:
  case SymbolKind::ValueParameter:
  case SymbolKind::LambdaCapture:
    return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] const SemanticOccurrence *
declarationOccurrence(const SemanticDatabase &database, SymbolId symbol) {
  std::vector<const SemanticOccurrence *> occurrences =
      database.occurrencesForSymbol(symbol);
  std::erase_if(occurrences, [](const SemanticOccurrence *occurrence) {
    return occurrence == nullptr ||
           !hasRole(occurrence->roles, OccurrenceRole::Declaration);
  });
  std::sort(
      occurrences.begin(), occurrences.end(),
      [](const SemanticOccurrence *left, const SemanticOccurrence *right) {
        if (left->sourceUnit != right->sourceUnit) {
          return left->sourceUnit < right->sourceUnit;
        }
        if (left->span.start != right->span.start) {
          return left->span.start < right->span.start;
        }
        return left->span.end < right->span.end;
      });
  return occurrences.empty() ? nullptr : occurrences.front();
}

[[nodiscard]] bool requiresDeclarationOccurrence(SymbolKind kind) {
  switch (kind) {
  case SymbolKind::Concept:
  case SymbolKind::TypeAlias:
  case SymbolKind::Class:
  case SymbolKind::Struct:
  case SymbolKind::Enum:
  case SymbolKind::Enumerator:
  case SymbolKind::Constructor:
  case SymbolKind::Destructor:
  case SymbolKind::Function:
  case SymbolKind::Method:
  case SymbolKind::Operator:
  case SymbolKind::Field:
  case SymbolKind::GlobalVariable:
    return true;
  case SymbolKind::Namespace:
  case SymbolKind::NamespaceAlias:
  case SymbolKind::LocalVariable:
  case SymbolKind::Parameter:
  case SymbolKind::TypeParameter:
  case SymbolKind::ValueParameter:
  case SymbolKind::LambdaCapture:
    return false;
  }
  return false;
}

[[nodiscard]] std::optional<std::string>
ownerQualifiedName(const SymbolRecord &symbol,
                   const SemanticOccurrence *declaration,
                   const SemanticModel &semantics) {
  if (declaration != nullptr && declaration->function != nullptr) {
    if (const FunctionInfo *function =
            semantics.findFunction(*declaration->function);
        function != nullptr && function->ownerClass != 0) {
      if (const ClassTypeInfo *owner =
              semantics.findClassType(function->ownerClass)) {
        return owner->qualifiedName;
      }
    }
  }
  if (declaration != nullptr && declaration->constructor != nullptr) {
    if (const ConstructorInfo *constructor =
            semantics.findConstructor(*declaration->constructor)) {
      if (const ClassTypeInfo *owner =
              semantics.findClassType(constructor->owner)) {
        return owner->qualifiedName;
      }
    }
  }
  if (declaration != nullptr && declaration->destructor != nullptr) {
    if (const DestructorInfo *destructor =
            semantics.findDestructor(*declaration->destructor)) {
      if (const ClassTypeInfo *owner =
              semantics.findClassType(destructor->owner)) {
        return owner->qualifiedName;
      }
    }
  }
  if (symbol.kind == SymbolKind::Enumerator &&
      symbol.type.kind == SemanticType::Enum) {
    if (const EnumTypeInfo *owner =
            semantics.findEnumType(symbol.type.enumId)) {
      return owner->qualifiedName;
    }
  }
  switch (symbol.kind) {
  case SymbolKind::Method:
  case SymbolKind::Operator:
  case SymbolKind::Field:
  case SymbolKind::Constructor:
  case SymbolKind::Destructor:
  case SymbolKind::Enumerator: {
    const std::size_t separator = symbol.qualifiedName.rfind("::");
    if (separator != std::string::npos) {
      return symbol.qualifiedName.substr(0, separator);
    }
    break;
  }
  default:
    break;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string>
signature(const SymbolRecord &symbol, const SemanticOccurrence *declaration,
          const SemanticModel &semantics) {
  const SignaturePrinter signatures(semantics);
  switch (symbol.kind) {
  case SymbolKind::Namespace:
    return "namespace " + symbol.qualifiedName;
  case SymbolKind::NamespaceAlias:
    // The semantic database retains the resolved alias identity but not an
    // exact alias-target presentation record. Preserve the entity without
    // manufacturing a declaration; the schema permits a missing signature.
    return std::nullopt;
  case SymbolKind::Concept:
    return signatures.conceptSignature(
        symbol,
        declaration == nullptr ? nullptr : declaration->conceptDeclaration);
  case SymbolKind::TypeAlias:
    if (declaration != nullptr && declaration->typeAlias != nullptr) {
      if (const TypeAliasInfo *alias =
              semantics.findTypeAlias(*declaration->typeAlias)) {
        return signatures.typeAlias(*alias);
      }
    }
    return std::nullopt;
  case SymbolKind::Class:
  case SymbolKind::Struct:
    if (declaration != nullptr && declaration->classType != nullptr) {
      if (const ClassTypeInfo *type =
              semantics.findClassType(*declaration->classType)) {
        return signatures.classType(*type);
      }
    }
    return std::nullopt;
  case SymbolKind::Enum:
    if (declaration != nullptr && declaration->enumType != nullptr) {
      if (const EnumTypeInfo *type =
              semantics.findEnumType(*declaration->enumType)) {
        return signatures.enumType(*type);
      }
    }
    return std::nullopt;
  case SymbolKind::Enumerator:
    if (symbol.type.kind == SemanticType::Enum) {
      if (const EnumTypeInfo *owner =
              semantics.findEnumType(symbol.type.enumId)) {
        return signatures.enumerator(*owner, symbol.qualifiedName);
      }
    }
    return std::nullopt;
  case SymbolKind::Constructor:
    if (declaration != nullptr && declaration->constructor != nullptr) {
      if (const ConstructorInfo *constructor =
              semantics.findConstructor(*declaration->constructor)) {
        if (const ClassTypeInfo *owner =
                semantics.findClassType(constructor->owner)) {
          return signatures.constructor(*owner, *constructor);
        }
      }
    }
    return std::nullopt;
  case SymbolKind::Destructor:
    if (declaration != nullptr && declaration->destructor != nullptr) {
      if (const DestructorInfo *destructor =
              semantics.findDestructor(*declaration->destructor)) {
        return signatures.destructor(*destructor);
      }
    }
    return std::nullopt;
  case SymbolKind::Function:
  case SymbolKind::Method:
  case SymbolKind::Operator:
    if (declaration != nullptr && declaration->function != nullptr) {
      if (const FunctionInfo *function =
              semantics.findFunction(*declaration->function)) {
        return signatures.function(*function);
      }
    }
    return std::nullopt;
  case SymbolKind::Field:
  case SymbolKind::GlobalVariable:
    if (declaration != nullptr) {
      return signatures.binding(*declaration);
    }
    return std::nullopt;
  case SymbolKind::LocalVariable:
  case SymbolKind::Parameter:
  case SymbolKind::TypeParameter:
  case SymbolKind::ValueParameter:
  case SymbolKind::LambdaCapture:
    return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] std::string ownershipName(OwnershipKind ownership) {
  switch (ownership) {
  case OwnershipKind::Value:
    return "value";
  case OwnershipKind::Borrowed:
    return "borrowed";
  case OwnershipKind::Unique:
    return "unique";
  case OwnershipKind::Shared:
    return "shared";
  }
  return "value";
}

[[nodiscard]] std::string dropName(DropKind drop) {
  switch (drop) {
  case DropKind::Trivial:
    return "trivial";
  case DropKind::Lexical:
    return "lexical";
  }
  return "trivial";
}

[[nodiscard]] bool presentsTraits(DocumentationKind kind) {
  switch (kind) {
  case DocumentationKind::TypeAlias:
  case DocumentationKind::Class:
  case DocumentationKind::Struct:
  case DocumentationKind::Interface:
  case DocumentationKind::Union:
  case DocumentationKind::Enum:
  case DocumentationKind::Enumerator:
  case DocumentationKind::Field:
  case DocumentationKind::GlobalVariable:
    return true;
  case DocumentationKind::Namespace:
  case DocumentationKind::NamespaceAlias:
  case DocumentationKind::Concept:
  case DocumentationKind::Constructor:
  case DocumentationKind::Destructor:
  case DocumentationKind::Function:
  case DocumentationKind::Method:
  case DocumentationKind::Operator:
    return false;
  }
  return false;
}

[[nodiscard]] DocumentationTraits
documentationTraits(const SemanticTypeTraits &traits) {
  return {.ownership = ownershipName(traits.ownership),
          .drop = dropName(traits.drop),
          .copyable = traits.copyable,
          .movable = traits.movable,
          .copyAssignable = traits.copyAssignable,
          .moveAssignable = traits.moveAssignable,
          .containsBorrowedState = traits.containsBorrowedState,
          .transferCapable = traits.transferCapable,
          .shareCapable = traits.shareCapable};
}

[[nodiscard]] std::string makeGroupKey(std::string_view unit,
                                       DocumentationKind kind,
                                       std::string_view qualifiedName) {
  return std::string(unit) + '|' + std::string(documentationKindName(kind)) +
         '|' + std::string(qualifiedName);
}

[[nodiscard]] std::string makeKey(std::string_view groupKey,
                                  const std::optional<std::string> &signature) {
  return std::string(groupKey) + '|' + signature.value_or("<no-signature>");
}

[[nodiscard]] std::string presentedName(const SymbolRecord &symbol,
                                        const SemanticOccurrence *declaration) {
  if (symbol.kind == SymbolKind::Operator && declaration != nullptr &&
      !declaration->name.empty()) {
    return declaration->name;
  }
  return symbol.name;
}

[[nodiscard]] std::string
presentedQualifiedName(const SymbolRecord &symbol,
                       const SemanticOccurrence *declaration,
                       const std::optional<std::string> &owner) {
  if (symbol.kind != SymbolKind::Operator || declaration == nullptr ||
      declaration->name.empty()) {
    return symbol.qualifiedName;
  }
  if (owner) {
    return *owner + "::" + declaration->name;
  }
  const std::size_t separator = symbol.qualifiedName.rfind("::");
  return separator == std::string::npos
             ? declaration->name
             : symbol.qualifiedName.substr(0, separator + 2) +
                   declaration->name;
}

void appendEscapedJsonString(std::string &result, std::string_view value) {
  static constexpr std::array<char, 16> hexadecimal = {
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  result += '"';
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\b':
      result += "\\b";
      break;
    case '\f':
      result += "\\f";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      if (character < 0x20U) {
        result += "\\u00";
        result += hexadecimal[character >> 4U];
        result += hexadecimal[character & 0x0fU];
      } else {
        result += static_cast<char>(character);
      }
      break;
    }
  }
  result += '"';
}

void appendOptionalString(std::string &result,
                          const std::optional<std::string> &value) {
  if (!value) {
    result += "null";
    return;
  }
  appendEscapedJsonString(result, *value);
}

void appendBoolean(std::string &result, bool value) {
  result += value ? "true" : "false";
}

} // namespace

std::string_view documentationKindName(DocumentationKind kind) {
  switch (kind) {
  case DocumentationKind::Namespace:
    return "namespace";
  case DocumentationKind::NamespaceAlias:
    return "namespace-alias";
  case DocumentationKind::Concept:
    return "concept";
  case DocumentationKind::TypeAlias:
    return "type-alias";
  case DocumentationKind::Class:
    return "class";
  case DocumentationKind::Struct:
    return "struct";
  case DocumentationKind::Interface:
    return "interface";
  case DocumentationKind::Union:
    return "union";
  case DocumentationKind::Enum:
    return "enum";
  case DocumentationKind::Enumerator:
    return "enumerator";
  case DocumentationKind::Constructor:
    return "constructor";
  case DocumentationKind::Destructor:
    return "destructor";
  case DocumentationKind::Function:
    return "function";
  case DocumentationKind::Method:
    return "method";
  case DocumentationKind::Operator:
    return "operator";
  case DocumentationKind::Field:
    return "field";
  case DocumentationKind::GlobalVariable:
    return "global-variable";
  }
  return "global-variable";
}

DocumentationIndexResult DocumentationQueries::standardLibraryIndex(
    const FrontendResult &snapshot) const {
  DocumentationIndexResult result;
  if (!snapshot.sourceValid || !snapshot.syntaxValid ||
      !snapshot.semanticValid) {
    result.errors.emplace_back(
        "Documentation indexing requires a valid semantic frontend snapshot.");
    return result;
  }

  const SemanticModel &semantics = snapshot.semantics;
  const SemanticDatabase &database = semantics.database();
  std::unordered_set<std::string> seenKeys;
  for (const SymbolRecord &symbol : database.symbols()) {
    if (!symbol.defaultLibrary || symbol.access != AccessModifier::Public ||
        symbol.generated || symbol.compilerPrivate || symbol.internalLinkage) {
      continue;
    }
    const SourceUnit *unit = snapshot.sourceGraph.findUnit(symbol.sourceUnit);
    if (unit == nullptr || !unit->standardLibraryName) {
      continue;
    }
    const SemanticOccurrence *declaration =
        declarationOccurrence(database, symbol.id);
    const std::optional<DocumentationKind> kind =
        documentationKind(symbol, declaration, semantics);
    if (!kind) {
      continue;
    }
    if (declaration == nullptr && requiresDeclarationOccurrence(symbol.kind)) {
      result.errors.emplace_back(
          "Missing tooling declaration occurrence for '" +
          symbol.qualifiedName + "'.");
      continue;
    }

    DocumentationDeclaration entry;
    entry.kind = *kind;
    entry.ownerQualifiedName =
        ownerQualifiedName(symbol, declaration, semantics);
    entry.name = presentedName(symbol, declaration);
    entry.qualifiedName =
        presentedQualifiedName(symbol, declaration, entry.ownerQualifiedName);
    entry.signature = signature(symbol, declaration, semantics);
    entry.documentationMarkdown = symbol.documentationMarkdown;
    if (presentsTraits(*kind)) {
      entry.traits = documentationTraits(symbol.traits);
    }
    const SourceLocation location = snapshot.sources.locate(symbol.nameSpan);
    entry.source = {.unit = *unit->standardLibraryName,
                    .line = location.line,
                    .start = symbol.nameSpan.start,
                    .end = symbol.nameSpan.end};
    entry.mutableBinding = symbol.mutableBinding;
    entry.staticMember = symbol.staticMember;
    entry.groupKey =
        makeGroupKey(entry.source.unit, entry.kind, entry.qualifiedName);
    entry.key = makeKey(entry.groupKey, entry.signature);
    if (!seenKeys.insert(entry.key).second) {
      continue;
    }
    result.index.declarations.push_back(std::move(entry));
  }

  std::sort(result.index.declarations.begin(), result.index.declarations.end(),
            [](const DocumentationDeclaration &left,
               const DocumentationDeclaration &right) {
              if (left.source.unit != right.source.unit) {
                return left.source.unit < right.source.unit;
              }
              if (left.qualifiedName != right.qualifiedName) {
                return left.qualifiedName < right.qualifiedName;
              }
              if (left.kind != right.kind) {
                return left.kind < right.kind;
              }
              if (left.signature != right.signature) {
                return left.signature < right.signature;
              }
              if (left.source.start != right.source.start) {
                return left.source.start < right.source.start;
              }
              return left.key < right.key;
            });
  std::sort(result.errors.begin(), result.errors.end());
  result.errors.erase(std::unique(result.errors.begin(), result.errors.end()),
                      result.errors.end());
  return result;
}

std::string serializeDocumentationIndexJson(const DocumentationIndex &index) {
  std::string result;
  result += "{\n  \"schema\": ";
  appendEscapedJsonString(result, documentationIndexSchema);
  result += ",\n  \"version\": " + std::to_string(index.version) +
            ",\n  \"declarations\": [";
  for (std::size_t declarationIndex = 0;
       declarationIndex < index.declarations.size(); ++declarationIndex) {
    const DocumentationDeclaration &entry =
        index.declarations[declarationIndex];
    result += declarationIndex == 0 ? "\n" : ",\n";
    result += "    {\n      \"key\": ";
    appendEscapedJsonString(result, entry.key);
    result += ",\n      \"groupKey\": ";
    appendEscapedJsonString(result, entry.groupKey);
    result += ",\n      \"kind\": ";
    appendEscapedJsonString(result, documentationKindName(entry.kind));
    result += ",\n      \"name\": ";
    appendEscapedJsonString(result, entry.name);
    result += ",\n      \"qualifiedName\": ";
    appendEscapedJsonString(result, entry.qualifiedName);
    result += ",\n      \"ownerQualifiedName\": ";
    appendOptionalString(result, entry.ownerQualifiedName);
    result += ",\n      \"signature\": ";
    appendOptionalString(result, entry.signature);
    result += ",\n      \"documentationMarkdown\": ";
    appendOptionalString(result, entry.documentationMarkdown);
    result += ",\n      \"traits\": ";
    if (!entry.traits) {
      result += "null";
    } else {
      result += "{\n        \"ownership\": ";
      appendEscapedJsonString(result, entry.traits->ownership);
      result += ",\n        \"drop\": ";
      appendEscapedJsonString(result, entry.traits->drop);
      result += ",\n        \"copyable\": ";
      appendBoolean(result, entry.traits->copyable);
      result += ",\n        \"movable\": ";
      appendBoolean(result, entry.traits->movable);
      result += ",\n        \"copyAssignable\": ";
      appendBoolean(result, entry.traits->copyAssignable);
      result += ",\n        \"moveAssignable\": ";
      appendBoolean(result, entry.traits->moveAssignable);
      result += ",\n        \"containsBorrowedState\": ";
      appendBoolean(result, entry.traits->containsBorrowedState);
      result += ",\n        \"transferCapable\": ";
      appendBoolean(result, entry.traits->transferCapable);
      result += ",\n        \"shareCapable\": ";
      appendBoolean(result, entry.traits->shareCapable);
      result += "\n      }";
    }
    result += ",\n      \"source\": {\n        \"unit\": ";
    appendEscapedJsonString(result, entry.source.unit);
    result += ",\n        \"line\": " + std::to_string(entry.source.line) +
              ",\n        \"start\": " + std::to_string(entry.source.start) +
              ",\n        \"end\": " + std::to_string(entry.source.end) +
              "\n      },\n      \"mutableBinding\": ";
    appendBoolean(result, entry.mutableBinding);
    result += ",\n      \"staticMember\": ";
    appendBoolean(result, entry.staticMember);
    result += "\n    }";
  }
  if (!index.declarations.empty()) {
    result += '\n';
  }
  result += "  ]\n}\n";
  return result;
}

} // namespace lang
