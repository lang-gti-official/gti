#pragma once

#include "gti/target.h"

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
using NativeSourceId = std::uint32_t;

// One file the translation unit consumed (C-05). The root header is not a
// table entry: `SourceOrigin::file == 0` names it, so raw header text with
// no include graph needs no table at all.
struct NativeSourceFile {
  std::string path;
  // True when the file entered through a system include; system provenance
  // is what the default import policy excludes.
  bool system = false;
};

struct SourceOrigin {
  // Original (presumed) line and column: line markers in preprocessed input
  // map every declaration back to the file that spelled it.
  std::uint32_t line = 0;
  std::uint32_t column = 0;
  // 0 is the root header; otherwise a 1-based NativeModel::sources index.
  NativeSourceId file = 0;
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
  // Plain `char` has its own C declaration identity and selected-target value
  // domain; it is not signed char, unsigned char, or a GTI code unit.
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
  NativeCScalarKind scalarIdentity = NativeCScalarKind::None;
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
  // An object-like macro whose body is a bounded constant expression,
  // imported with its evaluated value (C-06).
  Constant,
};

// What a `Constant` declaration's body evaluated to. C macro bodies carry no
// type, so the evaluated form is the fact the planner maps.
enum class NativeConstantKind {
  Integer,
  Floating,
  String,
};

// Layout facts a record's attributes assert (C-06). GTI's [[c_abi]] surface
// has no spelling for either, so the planner degrades such a record to an
// opaque handle; recording them keeps the reason exact instead of generic.
struct NativeRecordLayout {
  bool packed = false;
  // Explicit alignment in bytes, or 0 when unspecified.
  std::uint32_t alignment = 0;

  [[nodiscard]] bool constrained() const { return packed || alignment != 0; }
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
  // Layout attributes the record declares, when any.
  NativeRecordLayout layout;
  // Enum.
  std::vector<NativeEnumerator> enumerators;
  // Typedef: the aliased type.
  NativeTypeId aliased = 0;
  // Function: a NativeTypeKind::Function node carrying the signature.
  NativeTypeId type = 0;
  std::vector<NativeParameter> parameterNames;
  // Function ABI facts (C-06): an assembler label renaming the link symbol
  // and a non-C calling convention name; empty when absent. The planner
  // owns the rejection policy for both.
  std::string asmLabel;
  std::string callingConvention;
  // Import policy (C-05): false for a declaration that arrived from an
  // expanded system header. The planner models it — uses resolve through
  // it — but does not emit it; a referenced system record is pulled in as
  // an opaque handle on demand.
  bool inPolicy = true;
  // Constant: the evaluated macro value, tagged by the form it evaluated
  // to. C macro redefinition keeps the declaration at its first position
  // with its latest value.
  NativeConstantKind constantKind = NativeConstantKind::Integer;
  std::int64_t constantValue = 0;
  double constantFloating = 0.0;
  // The exact C spelling of a string macro's body, escapes intact.
  std::string constantText;
};

// Ids are one-based; zero is the invalid id everywhere.
struct NativeModel {
  std::vector<NativeScope> scopes;
  std::vector<NativeType> types;
  std::vector<NativeDeclaration> declarations;
  // Declaration ids in header source order; emission preserves this order.
  std::vector<NativeDeclarationId> order;
  // The transitive file set the translation unit consumed (C-05), in
  // first-appearance order of the input's line markers.
  std::vector<NativeSourceFile> sources;

  [[nodiscard]] NativeScopeId addScope(NativeScope scope);
  [[nodiscard]] NativeTypeId addType(NativeType type);
  [[nodiscard]] NativeDeclarationId addDeclaration(NativeDeclaration value);

  [[nodiscard]] const NativeScope *findScope(NativeScopeId id) const;
  [[nodiscard]] const NativeType *findType(NativeTypeId id) const;
  [[nodiscard]] const NativeDeclaration *
  findDeclaration(NativeDeclarationId id) const;
  [[nodiscard]] NativeDeclaration *findDeclaration(NativeDeclarationId id);
  [[nodiscard]] const NativeSourceFile *findSource(NativeSourceId id) const;
};

// Deterministic full rendering for golden tests and debugging.
[[nodiscard]] std::string printNativeModel(const NativeModel &model);

} // namespace lang::bindgen
