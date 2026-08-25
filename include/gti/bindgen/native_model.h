#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The language-neutral native declaration model produced by a
// NativeHeaderReader and consumed by the binding planner. The model is an
// id/arena IR in the repository's MIR style: values are plain structs tagged
// by kind, references are indices into the owning NativeModel, and a
// deterministic printer supports golden tests. The scope tree is the C++
// growth seam: the bounded C reader only creates the translation-unit root,
// while namespace and class scopes are additional kinds, not a redesign.

namespace lang::bindgen {

using NativeTypeId = std::uint32_t;
using NativeDeclarationId = std::uint32_t;
using NativeScopeId = std::uint32_t;

struct SourceOrigin {
  std::uint32_t line = 0;
  std::uint32_t column = 0;
};

enum class NativeScopeKind {
  TranslationUnit,
  // Future C++ scope kinds extend here (Namespace, Class).
};

struct NativeScope {
  NativeScopeKind kind = NativeScopeKind::TranslationUnit;
  std::string name;
  NativeScopeId parent = 0;
};

enum class NativeTypeKind {
  Void,
  Bool,
  // Plain `char` stays distinct from the sized integers: the GTI ABI admits
  // `const char*` as `c_string` but bare `char` is not an ABI scalar.
  Char,
  Integer,
  Floating,
  Pointer,
  Record,
  Enum,
  Alias,
  Function,
  Array,
  // A spelling the reader recognized as a type position but cannot model;
  // the planner turns uses into recorded skips carrying this reason.
  Unsupported,
};

struct NativeType {
  NativeTypeKind kind = NativeTypeKind::Void;
  // Integer / Floating.
  std::uint32_t bitWidth = 0;
  bool isSigned = false;
  // Pointer.
  NativeTypeId pointee = 0;
  bool constPointee = false;
  // Record / Enum / Alias reference.
  NativeDeclarationId declaration = 0;
  // Function.
  NativeTypeId result = 0;
  std::vector<NativeTypeId> parameters;
  bool variadic = false;
  // Array.
  NativeTypeId element = 0;
  std::uint64_t count = 0;
  // Unsupported.
  std::string spelling;
  std::string reason;
};

struct NativeRecordField {
  std::string name;
  NativeTypeId type = 0;
  SourceOrigin origin;
};

struct NativeEnumerator {
  std::string name;
  std::int64_t value = 0;
};

struct NativeParameter {
  std::string name; // May be empty: C prototypes allow unnamed parameters.
  NativeTypeId type = 0;
};

enum class NativeDeclarationKind {
  Record,
  Enum,
  Typedef,
  Function,
};

struct NativeDeclaration {
  NativeDeclarationKind kind = NativeDeclarationKind::Record;
  std::string name;
  NativeScopeId scope = 0;
  SourceOrigin origin;
  // Record.
  bool recordIsUnion = false;
  bool complete = false;
  std::vector<NativeRecordField> fields;
  // Set when the record is complete but its layout is outside the bounded
  // surface (bit-fields, inexpressible fields); the planner degrades such a
  // record to an opaque handle carrying this reason.
  std::string layoutReason;
  // Enum.
  std::vector<NativeEnumerator> enumerators;
  // Typedef: the aliased type.
  NativeTypeId aliased = 0;
  // Function: a NativeTypeKind::Function node carrying the signature.
  NativeTypeId type = 0;
  std::vector<NativeParameter> parameterNames;
};

// Ids are one-based; zero is the invalid id everywhere.
struct NativeModel {
  std::vector<NativeScope> scopes;
  std::vector<NativeType> types;
  std::vector<NativeDeclaration> declarations;
  // Declaration ids in header source order; emission preserves this order.
  std::vector<NativeDeclarationId> order;

  [[nodiscard]] NativeScopeId addScope(NativeScope scope);
  [[nodiscard]] NativeTypeId addType(NativeType type);
  [[nodiscard]] NativeDeclarationId addDeclaration(NativeDeclaration value);

  [[nodiscard]] const NativeScope *findScope(NativeScopeId id) const;
  [[nodiscard]] const NativeType *findType(NativeTypeId id) const;
  [[nodiscard]] const NativeDeclaration *
  findDeclaration(NativeDeclarationId id) const;
  [[nodiscard]] NativeDeclaration *findDeclaration(NativeDeclarationId id);
};

// Deterministic full rendering for golden tests and debugging.
[[nodiscard]] std::string printNativeModel(const NativeModel &model);

} // namespace lang::bindgen
