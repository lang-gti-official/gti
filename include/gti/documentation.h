#pragma once

#include "gti/frontend.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

inline constexpr std::string_view documentationIndexSchema =
    "gti.documentation-index";
inline constexpr std::uint32_t documentationIndexSchemaVersion = 1;

enum class DocumentationKind {
  Namespace,
  NamespaceAlias,
  Concept,
  TypeAlias,
  Class,
  Struct,
  Interface,
  Union,
  Enum,
  Enumerator,
  Constructor,
  Destructor,
  Function,
  Method,
  Operator,
  Field,
  GlobalVariable,
  Macro,
};

[[nodiscard]] std::string_view documentationKindName(DocumentationKind kind);

struct DocumentationSource {
  // Stable source-loader identity such as "std/vector". Physical checkout
  // paths and snapshot-local SourceUnitIds deliberately do not cross this
  // boundary.
  std::string unit;
  std::size_t line = 1;
  std::size_t start = 0;
  std::size_t end = 0;

  friend bool operator==(const DocumentationSource &,
                         const DocumentationSource &) = default;
};

struct DocumentationTraits {
  std::string ownership;
  std::string drop;
  bool copyable = false;
  bool movable = false;
  bool copyAssignable = false;
  bool moveAssignable = false;
  bool containsBorrowedState = false;
  bool transferCapable = false;
  bool shareCapable = false;

  friend bool operator==(const DocumentationTraits &,
                         const DocumentationTraits &) = default;
};

// Pointer-free, deterministic presentation data derived from one valid
// frontend snapshot. key identifies one declaration/overload; groupKey
// identifies the reference-page group that may contain several overloads.
// Neither key is a compiler identity and neither should be treated as a URL.
struct DocumentationDeclaration {
  std::string key;
  std::string groupKey;
  DocumentationKind kind = DocumentationKind::GlobalVariable;
  std::string name;
  std::string qualifiedName;
  std::optional<std::string> ownerQualifiedName;
  std::optional<std::string> signature;
  std::optional<std::string> documentationMarkdown;
  std::optional<DocumentationTraits> traits;
  DocumentationSource source;
  bool mutableBinding = false;
  bool staticMember = false;

  friend bool operator==(const DocumentationDeclaration &,
                         const DocumentationDeclaration &) = default;
};

struct DocumentationIndex {
  std::uint32_t version = documentationIndexSchemaVersion;
  std::vector<DocumentationDeclaration> declarations;

  friend bool operator==(const DocumentationIndex &,
                         const DocumentationIndex &) = default;
};

struct DocumentationIndexResult {
  DocumentationIndex index;
  std::vector<std::string> errors;

  [[nodiscard]] bool valid() const { return errors.empty(); }
};

class DocumentationQueries {
public:
  // Projects only public declarations owned by loaded standard-library
  // source units. The frontend must have completed valid semantic analysis
  // with tooling occurrences enabled so exact declaration signatures remain
  // available.
  [[nodiscard]] DocumentationIndexResult
  standardLibraryIndex(const FrontendResult &snapshot) const;
};

// The serialized representation intentionally omits timestamps, checkout
// paths, SymbolIds, and all AST addresses, so identical source snapshots
// produce byte-identical output on every machine.
[[nodiscard]] std::string
serializeDocumentationIndexJson(const DocumentationIndex &index);

} // namespace lang
