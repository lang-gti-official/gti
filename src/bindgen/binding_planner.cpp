#include "gti/bindgen/binding_planner.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>

// The mapping-policy phase: every native declaration becomes either the
// exact GTI binding it maps to under the documented bounded C ABI, or a
// recorded skip carrying its reason. Records whose layout the ABI cannot
// express degrade to [[c_opaque]] handles so pointer-based APIs through them
// stay importable; only by-value uses skip. The generated source has no
// special status — the frontend's native-interop validation remains the
// final ABI authority over everything planned here.

namespace lang::bindgen {

std::size_t BindingPlan::importedCount() const {
  std::size_t count = 0;
  for (const BindingItem &item : items) {
    count += item.kind != BindingItemKind::Skipped ? 1 : 0;
  }
  return count;
}

std::size_t BindingPlan::skippedCount() const {
  return items.size() - importedCount();
}

namespace {

// GTI rejects reserved C++ spellings as identifiers and the interop layer
// rejects implementation-reserved spellings on native-facing names. This
// list covers the words a C header could realistically use; the frontend
// remains the final authority when generated bindings are compiled.
[[nodiscard]] bool reservedWord(std::string_view name) {
  static constexpr std::array<std::string_view, 34> words = {
      "template", "class",   "typename", "namespace", "operator",  "this",
      "new",      "delete",  "public",   "private",   "protected", "virtual",
      "override", "using",   "mut",      "unsafe",    "constexpr", "auto",
      "requires", "concept", "export",   "friend",    "try",       "catch",
      "throw",    "switch",  "case",     "default",   "if",        "else",
      "while",    "for",     "return",   "match",
  };
  for (const std::string_view word : words) {
    if (word == name) {
      return true;
    }
  }
  return false;
}

// Names the generated native bridge already occupies in the C ordinary
// namespace, and the C17 keywords. GTI rejects both on a native-facing
// identifier (GTI-S2065); mirroring the list here turns what would be a
// frontend rejection of generated source into a recorded skip with its
// reason — which matters most for a configured rename, where the caller
// chose the spelling.
[[nodiscard]] bool reservedNativeName(std::string_view name) {
  static constexpr std::array<std::string_view, 51> names = {
      "FILE",           "gti_c_string_view",
      "ptrdiff_t",      "size_t",
      "max_align_t",    "wchar_t",
      "int8_t",         "int16_t",
      "int32_t",        "int64_t",
      "uint8_t",        "uint16_t",
      "uint32_t",       "uint64_t",
      "int_least8_t",   "int_least16_t",
      "int_least32_t",  "int_least64_t",
      "uint_least8_t",  "uint_least16_t",
      "uint_least32_t", "uint_least64_t",
      "int_fast8_t",    "int_fast16_t",
      "int_fast32_t",   "int_fast64_t",
      "uint_fast8_t",   "uint_fast16_t",
      "uint_fast32_t",  "uint_fast64_t",
      "intptr_t",       "uintptr_t",
      "intmax_t",       "uintmax_t",
      "char",           "const",
      "double",         "enum",
      "extern",         "float",
      "inline",         "int",
      "long",           "short",
      "signed",         "sizeof",
      "static",         "struct",
      "typedef",        "union",
      "unsigned",
  };
  return std::find(names.begin(), names.end(), name) != names.end();
}

// `fileScope` marks names the generated header surface would emit at C file
// scope (records, opaque handles, functions), which reject any leading
// underscore; reserved `__x` / `_X` spellings reject everywhere.
[[nodiscard]] std::optional<std::string>
identifierProblem(std::string_view name, bool fileScope) {
  if (name.empty()) {
    return "the declaration has no name";
  }
  if (reservedWord(name)) {
    return "'" + std::string(name) + "' is a reserved GTI spelling";
  }
  if (name.size() >= 2 && name[0] == '_' &&
      (name[1] == '_' || std::isupper(static_cast<unsigned char>(name[1])))) {
    return "'" + std::string(name) + "' is an implementation-reserved spelling";
  }
  if (fileScope && name.front() == '_') {
    return "'" + std::string(name) +
           "' is reserved at C file scope by the generated native surface";
  }
  if (fileScope && name.find("__") != std::string_view::npos) {
    // GTI-S2064 rejects the C++-reserved '__' spelling anywhere in a
    // native-facing name, not only at the front.
    return "'" + std::string(name) +
           "' contains an implementation-reserved '__' spelling";
  }
  if (fileScope && reservedNativeName(name)) {
    return "'" + std::string(name) +
           "' is reserved by C or by the generated native bridge headers";
  }
  return std::nullopt;
}

// `Unavailable` marks a record whose name the generated surface cannot
// spell at all (a reserved spelling); neither by-value nor pointer uses of
// it can import.
enum class RecordBinding { CAbi, Opaque, Unavailable };

// c_string is a call-boundary convenience, not a storage representation.
// RecordField also selects storage-faithful spelling for reusable typedefs.
// Recursive pointers and callback slots otherwise share one type family.
// `ArrayResult` marks the result of a function the caller annotated with
// `[[c_array(count)]]`; its count contract does not change pointer type
// identity.
enum class TypeUse {
  Ordinary,
  DirectFunctionParameter,
  RecordField,
  ArrayResult,
};

struct MappedType {
  bool supported = false;
  std::string spelling;
  std::string reason;
  bool namedType = false;
};

[[nodiscard]] MappedType supportedType(std::string spelling,
                                       bool named = false) {
  return {true, std::move(spelling), {}, named};
}

[[nodiscard]] MappedType unsupportedType(std::string reason) {
  return {false, {}, std::move(reason)};
}

class Planner {
public:
  Planner(const NativeModel &model, BindingOptions options)
      : model_(model), options_(std::move(options)) {}

  [[nodiscard]] BindingPlan run() {
    // Every header-provided name is reserved before any callback alias is
    // synthesized, so a generated name can never shadow a declaration that
    // appears later in the file.
    for (const NativeDeclarationId id : model_.order) {
      const NativeDeclaration *declaration = model_.findDeclaration(id);
      if (declaration == nullptr) {
        continue;
      }
      if (!declaration->name.empty()) {
        usedNames_.insert(declaration->name);
      }
      for (const NativeEnumerator &enumerator : declaration->enumerators) {
        usedNames_.insert(enumerator.name);
      }
    }
    classifyRecords();
    commitSynthesis_ = true;
    BindingPlan plan;
    plan.options = options_;
    plan.sourceFiles = model_.sources;
    for (const NativeDeclarationId id : model_.order) {
      const NativeDeclaration *declaration = model_.findDeclaration(id);
      if (declaration == nullptr) {
        continue;
      }
      if (declaration->kind == NativeDeclarationKind::Record &&
          duplicateRecords_.count(id) != 0) {
        // The first declaration of this name already produced the binding;
        // every use of this entity spells the same name.
        continue;
      }
      if (!admitted(*declaration)) {
        // Excluded by provenance (C-05) or by the configured policy (C-07):
        // the declaration resolves uses but is not part of the import. A
        // skipped-typedef outcome makes those uses spell the canonical type;
        // a referenced record is pulled below as an opaque handle.
        if (declaration->kind == NativeDeclarationKind::Typedef) {
          typedefImported_.emplace(id, false);
        }
        continue;
      }
      synthesisOrigin_ = declaration->origin;
      BindingItem planned;
      switch (declaration->kind) {
      case NativeDeclarationKind::Record:
        planned = planRecord(id, *declaration);
        break;
      case NativeDeclarationKind::Enum:
        planned = planEnum(*declaration);
        break;
      case NativeDeclarationKind::Typedef:
        planned = planTypedef(*declaration);
        break;
      case NativeDeclarationKind::Function:
        planned = planFunction(*declaration);
        break;
      case NativeDeclarationKind::Constant:
        planned = planConstant(*declaration);
        break;
      }
      if (planned.kind == BindingItemKind::Skipped) {
        // Aliases synthesized while this declaration was being planned have
        // no surviving use; releasing their names keeps the emitted file
        // free of orphans and lets a later use claim the shape again.
        for (const BindingItem &callback : pendingCallbacks_) {
          usedNames_.erase(callback.name);
          for (auto shape = callbackNameByShape_.begin();
               shape != callbackNameByShape_.end();) {
            shape = shape->second == callback.name
                        ? callbackNameByShape_.erase(shape)
                        : std::next(shape);
          }
        }
        pendingCallbacks_.clear();
      }
      if (declaration->kind == NativeDeclarationKind::Typedef) {
        typedefImported_.emplace(id, planned.kind != BindingItemKind::Skipped);
      }
      for (BindingItem &callback : pendingCallbacks_) {
        plan.items.push_back(std::move(callback));
      }
      pendingCallbacks_.clear();
      plan.items.push_back(std::move(planned));
    }
    // Out-of-policy records that in-policy bindings reference through
    // pointers surface as opaque handles ahead of every use, in model
    // order, so the emitted file stays self-contained.
    std::vector<BindingItem> pulled;
    for (const NativeDeclarationId id : model_.order) {
      if (pulledRecords_.count(id) == 0) {
        continue;
      }
      const NativeDeclaration *declaration = model_.findDeclaration(id);
      if (declaration == nullptr) {
        continue;
      }
      BindingItem handle;
      handle.kind = BindingItemKind::Opaque;
      handle.name = spelled(*declaration);
      handle.origin = declaration->origin;
      const NativeSourceFile *source =
          model_.findSource(declaration->origin.file);
      handle.reason =
          "declared in " +
          (source != nullptr ? source->path : std::string("a system header")) +
          "; imported as an opaque handle because pointer uses reach it";
      pulled.push_back(std::move(handle));
    }
    plan.items.insert(plan.items.begin(),
                      std::make_move_iterator(pulled.begin()),
                      std::make_move_iterator(pulled.end()));

    // A configured entry that matched nothing is almost always a typo; it
    // must be visible rather than silently doing nothing.
    for (const BindingNamePattern &pattern : options_.allow) {
      if (matchedPatterns_.count("allow:" + pattern.text) == 0) {
        plan.policyProblems.push_back("allow pattern '" + pattern.text +
                                      "' matched no declaration");
      }
    }
    for (const BindingNamePattern &pattern : options_.deny) {
      if (matchedPatterns_.count("deny:" + pattern.text) == 0) {
        plan.policyProblems.push_back("deny pattern '" + pattern.text +
                                      "' matched no declaration");
      }
    }
    for (const auto &[from, to] : options_.renames) {
      if (matchedRenames_.count(from) == 0) {
        plan.policyProblems.push_back("rename '" + from +
                                      "' matched no imported declaration");
      }
    }
    for (const BindingArrayReturn &entry : options_.arrayReturns) {
      if (matchedArrayReturns_.count(entry.function) == 0) {
        plan.policyProblems.push_back("array return '" + entry.function +
                                      "' matched no imported function");
      }
    }
    return plan;
  }

private:
  const NativeModel &model_;
  BindingOptions options_;
  std::unordered_map<NativeDeclarationId, RecordBinding> recordBindings_;
  std::unordered_map<NativeDeclarationId, std::string> recordReasons_;
  // Inline function-pointer uses bind through synthesized named aliases —
  // the language's own admitted callback form. Record classification only
  // asks whether a shape is admissible; names allocate exclusively during
  // the emission pass (`commitSynthesis_`), one alias per distinct shape, so
  // generated names are deterministic in plan order. The caches are mutable
  // because allocation happens inside the const type-mapping walk.
  bool commitSynthesis_ = false;
  std::string synthesisHint_;
  SourceOrigin synthesisOrigin_;
  mutable std::map<std::string, std::string> callbackNameByShape_;
  mutable std::vector<BindingItem> pendingCallbacks_;
  mutable std::set<std::string> usedNames_;
  // Typedef outcomes: a use of a skipped typedef must spell the canonical
  // type, because the alias name never reaches the generated file.
  std::unordered_map<NativeDeclarationId, bool> typedefImported_;
  // C prototype-scoped forward references ('struct sockaddr *' inside a
  // parameter list with no visible tag) are distinct native entities per
  // prototype but one GTI handle: the first record classification of a name
  // answers for every later declaration of the same name, and later
  // duplicates leave the plan.
  std::unordered_map<std::string, NativeDeclarationId> recordIdByName_;
  std::set<NativeDeclarationId> duplicateRecords_;
  // Out-of-policy records referenced by committed in-policy bindings; each
  // becomes one opaque handle at the head of the plan.
  mutable std::set<NativeDeclarationId> pulledRecords_;
  // Which configured entries actually applied, so the unmatched ones can be
  // reported instead of silently doing nothing.
  mutable std::set<std::string> matchedPatterns_;
  mutable std::set<std::string> matchedRenames_;
  std::set<std::string> matchedArrayReturns_;

  // -- Opt-in policy (C-07) -------------------------------------------------

  // Provenance (C-05) first, then the configured lists. A declaration the
  // policy excludes behaves exactly like a system one: it still resolves
  // uses, and a referenced record still pulls in as an opaque handle.
  [[nodiscard]] bool admitted(const NativeDeclaration &declaration) const {
    if (!declaration.inPolicy) {
      return false;
    }
    for (const BindingNamePattern &pattern : options_.deny) {
      if (pattern.matches(declaration.name)) {
        matchedPatterns_.insert("deny:" + pattern.text);
        return false;
      }
    }
    if (options_.allow.empty()) {
      return true;
    }
    for (const BindingNamePattern &pattern : options_.allow) {
      if (pattern.matches(declaration.name)) {
        matchedPatterns_.insert("allow:" + pattern.text);
        return true;
      }
    }
    return false;
  }

  // The GTI source spelling of a declaration's name: a configured rename, or
  // the C name. A renamed function retains its separate native symbol in the
  // emitted `[[link_name]]` attribute.
  [[nodiscard]] const std::string &
  spelled(const NativeDeclaration &declaration) const {
    const auto rename = options_.renames.find(declaration.name);
    if (rename == options_.renames.end()) {
      return declaration.name;
    }
    matchedRenames_.insert(declaration.name);
    return rename->second;
  }

  [[nodiscard]] static BindingItem skipped(const NativeDeclaration &declaration,
                                           std::string reason) {
    BindingItem item;
    item.kind = BindingItemKind::Skipped;
    item.name = declaration.name;
    item.origin = declaration.origin;
    item.reason = std::move(reason);
    return item;
  }

  // -- Type mapping ---------------------------------------------------------

  // Chases transparent typedef references for admissibility decisions.
  [[nodiscard]] const NativeType *canonical(NativeTypeId id) const {
    const NativeType *type = model_.findType(id);
    while (type != nullptr && type->kind == NativeTypeKind::Alias) {
      const NativeDeclaration *alias =
          model_.findDeclaration(type->declaration);
      if (alias == nullptr || alias->kind != NativeDeclarationKind::Typedef) {
        return nullptr;
      }
      type = model_.findType(alias->aliased);
    }
    return type;
  }

  [[nodiscard]] static std::string scalarSpelling(const NativeType &type) {
    if (type.scalarIdentity != NativeCScalarKind::None) {
      return std::string(nativeCScalarKindName(type.scalarIdentity));
    }
    if (type.kind == NativeTypeKind::Bool) {
      return "c_bool";
    }
    if (type.kind == NativeTypeKind::Char) {
      return "c_char";
    }
    if (type.kind == NativeTypeKind::Floating) {
      return type.bitWidth == 64 ? "double" : "float";
    }
    return (type.isSigned ? "int" : "uint") + std::to_string(type.bitWidth) +
           "_t";
  }

  [[nodiscard]] static bool admittedScalar(const NativeType &type) {
    if (type.kind == NativeTypeKind::Bool ||
        type.kind == NativeTypeKind::Char) {
      return true;
    }
    if (type.kind == NativeTypeKind::Integer) {
      return type.bitWidth == 8 || type.bitWidth == 16 || type.bitWidth == 32 ||
             type.bitWidth == 64;
    }
    return type.kind == NativeTypeKind::Floating &&
           (type.bitWidth == 32 || type.bitWidth == 64);
  }

  [[nodiscard]] MappedType
  enumIntegerMapping(const NativeDeclaration &declaration) const {
    const NativeType *integer = canonical(declaration.enumIntegerType);
    if (integer == nullptr || integer->kind != NativeTypeKind::Integer ||
        !admittedScalar(*integer)) {
      return unsupportedType(
          "enum '" + declaration.name +
          "' has no supported native integer representation");
    }
    if (!declaration.enumLayoutMatchesInteger) {
      return unsupportedType(
          "enum '" + declaration.name +
          "' has a layout different from its native integer");
    }
    for (const NativeEnumerator &enumerator : declaration.enumerators) {
      bool fits = false;
      if (integer->isSigned) {
        if (const auto *value = std::get_if<std::int64_t>(&enumerator.value)) {
          fits = integer->bitWidth == 64 ||
                 (*value >= -(std::int64_t(1) << (integer->bitWidth - 1)) &&
                  *value < (std::int64_t(1) << (integer->bitWidth - 1)));
        }
      } else if (const auto *value =
                     std::get_if<std::uint64_t>(&enumerator.value)) {
        fits = integer->bitWidth == 64 ||
               *value < (std::uint64_t(1) << integer->bitWidth);
      }
      if (!fits) {
        return unsupportedType(
            "enumerator '" + enumerator.name +
            "' does not fit its native integer representation");
      }
    }
    return supportedType(scalarSpelling(*integer));
  }

  [[nodiscard]] MappedType mapEnum(const NativeDeclaration &declaration) const {
    const MappedType integer = enumIntegerMapping(declaration);
    if (!integer.supported) {
      return integer;
    }
    // Excluded/system enums still have a known compatible integer. Never
    // reference an alias the import policy deliberately does not emit.
    if (!admitted(declaration)) {
      return integer;
    }
    if (!declaration.name.empty()) {
      if (const auto problem = identifierProblem(spelled(declaration), false)) {
        return unsupportedType("enum " + *problem);
      }
    }
    for (const NativeEnumerator &enumerator : declaration.enumerators) {
      if (const auto problem = identifierProblem(enumerator.name, false)) {
        return unsupportedType("enumerator " + *problem);
      }
    }
    return declaration.name.empty() ? integer
                                    : supportedType(spelled(declaration));
  }

  [[nodiscard]] const NativeType *callbackShape(const NativeType &type) const {
    if (type.kind == NativeTypeKind::Function) {
      return &type;
    }
    if (type.kind == NativeTypeKind::Pointer) {
      const NativeType *pointee = canonical(type.pointee);
      if (pointee && pointee->kind == NativeTypeKind::Function) {
        return pointee;
      }
    }
    return nullptr;
  }

  [[nodiscard]] MappedType mapNamedCallback(NativeDeclarationId id,
                                            const NativeDeclaration &alias,
                                            const NativeType &function) const {
    const MappedType shape = mapCallbackShape(function);
    if (!shape.supported) {
      return shape;
    }
    const auto outcome = typedefImported_.find(id);
    if (!admitted(alias) || identifierProblem(spelled(alias), false) ||
        (outcome != typedefImported_.end() && !outcome->second)) {
      // A hidden/invalid source name does not make its native signature
      // unusable. Reuse the ordinary deterministic inline-callback synthesis.
      return mapFunctionPointer(function);
    }
    return supportedType(spelled(alias), true);
  }

  // Maps one native type use to its GTI spelling under the bounded C ABI.
  // `pointerDepth` tracks how many pointer levels wrap this use.
  [[nodiscard]] MappedType mapType(NativeTypeId id, bool allowVoid,
                                   TypeUse use = TypeUse::Ordinary,
                                   std::size_t pointerDepth = 0) const {
    const NativeType *direct = model_.findType(id);
    if (direct == nullptr) {
      return unsupportedType("the type reference is invalid");
    }
    // A named typedef spells through its GTI alias, which is transparent to
    // the ABI; admissibility still follows the canonical type below.
    if (direct->kind == NativeTypeKind::Alias) {
      const NativeDeclaration *alias =
          model_.findDeclaration(direct->declaration);
      const NativeType *target = canonical(id);
      if (alias == nullptr || target == nullptr) {
        return unsupportedType("the typedef reference is invalid");
      }
      if (target->kind == NativeTypeKind::Function) {
        return unsupportedType(
            "a C function type requires a pointer or parameter decay");
      }
      if (const NativeType *shape = callbackShape(*target)) {
        return mapNamedCallback(direct->declaration, *alias, *shape);
      }
      const MappedType canonicalMapping =
          mapType(alias->aliased, allowVoid, use, pointerDepth);
      if (!canonicalMapping.supported) {
        return canonicalMapping;
      }
      if (canonicalMapping.spelling == "c_string") {
        // The declared typedef preserves storage as const c_char*. Retain
        // call-only string convenience without putting c_string in records.
        return canonicalMapping;
      }
      // A skipped typedef never reaches the generated file, so a use that is
      // valid through the canonical type spells that type directly.
      if (const auto outcome = typedefImported_.find(direct->declaration);
          outcome != typedefImported_.end() && !outcome->second) {
        return canonicalMapping;
      }
      return supportedType(spelled(*alias), true);
    }
    switch (direct->kind) {
    case NativeTypeKind::Void:
      if (allowVoid) {
        return supportedType("void");
      }
      return unsupportedType("'void' is only a result type");
    case NativeTypeKind::Bool:
    case NativeTypeKind::Char:
      return supportedType(scalarSpelling(*direct));
    case NativeTypeKind::Integer:
    case NativeTypeKind::Floating:
      if (!admittedScalar(*direct)) {
        return unsupportedType("no fixed-width GTI scalar matches this "
                               "type's width");
      }
      return supportedType(scalarSpelling(*direct));
    case NativeTypeKind::Pointer:
      return mapPointer(*direct, use, pointerDepth);
    case NativeTypeKind::Record: {
      const NativeDeclaration *record =
          model_.findDeclaration(direct->declaration);
      if (record == nullptr) {
        return unsupportedType("the record reference is invalid");
      }
      const auto binding = recordBindings_.find(direct->declaration);
      if (binding != recordBindings_.end() &&
          binding->second == RecordBinding::Unavailable) {
        return unsupportedType("record " +
                               recordReasons_.at(direct->declaration));
      }
      if (binding == recordBindings_.end() ||
          binding->second == RecordBinding::Opaque) {
        return unsupportedType(
            "record '" + record->name +
            "' is only available as an opaque handle; it can only be used "
            "through a pointer");
      }
      return supportedType(spelled(*record));
    }
    case NativeTypeKind::Enum: {
      const NativeDeclaration *target =
          model_.findDeclaration(direct->declaration);
      if (target == nullptr) {
        return unsupportedType("the enum reference is invalid");
      }
      return mapEnum(*target);
    }
    case NativeTypeKind::Function:
      return unsupportedType(
          "an unnamed function type is outside the bounded surface; name it "
          "with a typedef");
    case NativeTypeKind::Array:
      if (use == TypeUse::RecordField) {
        const ArrayShape shape = arrayShape(*direct);
        MappedType element =
            mapType(shape.elementId, false, TypeUse::RecordField);
        if (!element.supported) {
          return element;
        }
        for (const auto extent : shape.extents) {
          if (extent == 0) {
            return unsupportedType("fixed arrays require positive extents");
          }
          element.spelling += "[" + std::to_string(extent) + "]";
        }
        element.namedType = false;
        return element;
      }
      return unsupportedType("a fixed array is only a record field type");
    case NativeTypeKind::Alias:
      return unsupportedType("unreachable alias state");
    case NativeTypeKind::Unsupported:
      return unsupportedType(direct->reason);
    }
    return unsupportedType("unhandled native type");
  }

  [[nodiscard]] MappedType mapPointer(const NativeType &pointer, TypeUse use,
                                      std::size_t pointerDepth) const {
    if (pointerDepth >= 64) {
      return unsupportedType("native pointer nesting exceeds the source "
                             "parser's structural depth budget");
    }
    const NativeType *pointee = canonical(pointer.pointee);
    if (pointee == nullptr) {
      return unsupportedType("the pointee type is invalid");
    }
    if (pointee->kind == NativeTypeKind::Function) {
      const NativeType *written = model_.findType(pointer.pointee);
      if (written && written->kind == NativeTypeKind::Alias) {
        if (const auto *alias = model_.findDeclaration(written->declaration)) {
          return mapNamedCallback(written->declaration, *alias, *pointee);
        }
      }
      return mapFunctionPointer(*pointee);
    }
    if (pointee->kind == NativeTypeKind::Pointer) {
      // Nested slots are the same recursive C pointer family in parameters,
      // results and fields. Avoid the call-only c_string convenience spelling
      // inside this tree so every qualification level remains explicit.
      const MappedType inner = mapType(pointer.pointee, false,
                                       TypeUse::RecordField, pointerDepth + 1);
      if (!inner.supported) {
        return inner;
      }
      // GTI qualifies an alias as `const Alias*`, whereas a written pointer
      // chain qualifies that slot as `T* const*`. Preserve the rendering shape
      // rather than moving const onto the terminal object.
      return supportedType(
          (pointer.constPointee && inner.namedType ? "const " : "") +
          inner.spelling +
          (pointer.constPointee && !inner.namedType ? " const*" : "*"));
    }
    if (pointee->kind == NativeTypeKind::Char) {
      if (pointer.constPointee && use != TypeUse::RecordField) {
        return supportedType("c_string");
      }
      return supportedType(std::string(pointer.constPointee ? "const " : "") +
                           "c_char*");
    }
    const std::string qualifier = pointer.constPointee ? "const " : "";
    if (pointee->kind == NativeTypeKind::Void) {
      return supportedType(qualifier + "void*");
    }
    if (admittedScalar(*pointee)) {
      return supportedType(qualifier + scalarSpelling(*pointee) + "*");
    }
    if (pointee->kind == NativeTypeKind::Record) {
      const NativeDeclaration *record =
          model_.findDeclaration(pointee->declaration);
      if (record == nullptr) {
        return unsupportedType("the pointee record reference is invalid");
      }
      if (const auto binding = recordBindings_.find(pointee->declaration);
          binding != recordBindings_.end() &&
          binding->second == RecordBinding::Unavailable) {
        return unsupportedType("record " +
                               recordReasons_.at(pointee->declaration));
      }
      if (!admitted(*record) && commitSynthesis_) {
        // A committed use of a system record pulls its opaque handle into
        // the plan; name unification keeps one handle per tag name.
        NativeDeclarationId pulledId = pointee->declaration;
        if (const auto first = recordIdByName_.find(record->name);
            first != recordIdByName_.end()) {
          pulledId = first->second;
        }
        pulledRecords_.insert(pulledId);
      }
      // Both [[c_abi]] records and [[c_opaque]] handles admit one-level
      // pointers; degradation to opaque keeps exactly these uses alive.
      return supportedType(qualifier + spelled(*record) + "*");
    }
    if (pointee->kind == NativeTypeKind::Enum) {
      const NativeDeclaration *target =
          model_.findDeclaration(pointee->declaration);
      if (target == nullptr) {
        return unsupportedType("the pointee enum reference is invalid");
      }
      const MappedType mapped = mapEnum(*target);
      return mapped.supported ? supportedType(qualifier + mapped.spelling + "*")
                              : mapped;
    }
    return unsupportedType("this pointee type is outside the bounded GTI C "
                           "ABI pointer surface");
  }

  // Validates a callback shape: parameters and result recursively use the
  // bounded ABI family.
  [[nodiscard]] MappedType mapCallbackShape(const NativeType &function) const {
    if (function.variadic) {
      return unsupportedType("variadic callbacks are outside the bounded "
                             "GTI C ABI");
    }
    for (const NativeTypeId parameter : function.parameters) {
      const MappedType mapped = mapType(parameter, false);
      if (!mapped.supported) {
        return mapped;
      }
    }
    return mapType(function.result, true);
  }

  // An inline function-pointer use (a parameter, field, or result declared
  // as `ret (*)(params)` without a typedef) binds through a synthesized
  // named alias — the language's own admitted callback form. One alias
  // serves every use of the same shape; its name derives from the first
  // declaration that commits it, so output stays deterministic.
  [[nodiscard]] MappedType
  mapFunctionPointer(const NativeType &function) const {
    const MappedType shape = mapCallbackShape(function);
    if (!shape.supported) {
      return unsupportedType("inline function pointer: " + shape.reason);
    }
    std::string rendered = "(";
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
      const MappedType parameter = mapType(function.parameters[index], false);
      rendered += (index != 0 ? ", " : "") + parameter.spelling;
    }
    rendered += ") -> " + mapType(function.result, true).spelling;
    if (const auto existing = callbackNameByShape_.find(rendered);
        existing != callbackNameByShape_.end()) {
      return supportedType(existing->second, true);
    }
    if (!commitSynthesis_) {
      // Record classification only asks admissibility; the alias itself is
      // allocated when the emission pass reaches a committed use.
      return supportedType(rendered, true);
    }
    const std::string base =
        (synthesisHint_.empty() ? "callback" : synthesisHint_) + "_fn";
    std::string name = base;
    for (int ordinal = 2; usedNames_.count(name) != 0; ++ordinal) {
      name = base + "_" + std::to_string(ordinal);
    }
    usedNames_.insert(name);
    callbackNameByShape_.emplace(rendered, name);
    BindingItem alias;
    alias.kind = BindingItemKind::Callback;
    alias.name = name;
    alias.origin = synthesisOrigin_;
    alias.synthesized = true;
    alias.resultSpelling = mapType(function.result, true).spelling;
    for (const NativeTypeId parameter : function.parameters) {
      alias.parameters.push_back(
          {std::string(), mapType(parameter, false).spelling});
    }
    pendingCallbacks_.push_back(std::move(alias));
    return supportedType(name, true);
  }

  // -- Record classification ------------------------------------------------

  // Walks a (possibly nested) fixed-array field type to its ultimate element,
  // collecting the extents in declarator order: `int a[2][3]` yields {2, 3}.
  struct ArrayShape {
    NativeTypeId elementId = 0;
    std::vector<std::uint64_t> extents;
  };

  [[nodiscard]] ArrayShape arrayShape(const NativeType &array) const {
    ArrayShape shape;
    const NativeType *node = &array;
    while (node != nullptr && node->kind == NativeTypeKind::Array) {
      shape.extents.push_back(node->count);
      shape.elementId = node->element;
      node = canonical(node->element);
    }
    return shape;
  }

  [[nodiscard]] std::optional<std::string>
  fieldProblem(const NativeRecordField &field) const {
    if (const std::optional<std::string> problem =
            identifierProblem(field.name, false)) {
      return "field " + *problem;
    }
    const NativeType *type = canonical(field.type);
    if (type == nullptr) {
      return "field '" + field.name + "' has an invalid type";
    }
    if (type->kind == NativeTypeKind::Array) {
      // The ultimate element uses the ordinary field set: GTI computes array
      // layout recursively from admitted element facts (decision 013).
      const MappedType element =
          mapType(field.type, false, TypeUse::RecordField);
      if (!element.supported) {
        return "array field '" + field.name + "': " + element.reason;
      }
      return std::nullopt;
    }
    const MappedType mapped = mapType(field.type, false, TypeUse::RecordField);
    if (!mapped.supported) {
      return "field '" + field.name + "': " + mapped.reason;
    }
    return std::nullopt;
  }

  void classifyRecords() {
    // Header declaration order is definition-before-use for by-value field
    // nesting in C, so one forward pass classifies transitively.
    for (const NativeDeclarationId id : model_.order) {
      const NativeDeclaration *declaration = model_.findDeclaration(id);
      if (declaration == nullptr ||
          declaration->kind != NativeDeclarationKind::Record) {
        continue;
      }
      if (const std::optional<std::string> problem =
              identifierProblem(spelled(*declaration), true);
          problem.has_value() && !declaration->name.empty()) {
        recordBindings_.emplace(id, RecordBinding::Unavailable);
        recordReasons_.emplace(id, *problem);
        recordIdByName_.emplace(declaration->name, id);
        continue;
      }
      std::string reason;
      if (!admitted(*declaration)) {
        // A system record is a handle at most: binding a platform layout by
        // value would freeze facts the header owner never promised GTI.
        recordBindings_.emplace(id, RecordBinding::Opaque);
        if (!declaration->name.empty()) {
          recordIdByName_.emplace(declaration->name, id);
        }
        continue;
      }
      if (!declaration->complete) {
        // A genuinely opaque handle; no degradation note.
      } else if (declaration->recordIsUnion) {
        reason = "unions are outside the bounded GTI record surface";
      } else if (!declaration->layoutReason.empty()) {
        reason = declaration->layoutReason;
      } else if (declaration->fields.empty()) {
        reason = "an empty record has no GTI [[c_abi]] layout";
      } else {
        for (const NativeRecordField &field : declaration->fields) {
          if (const std::optional<std::string> problem = fieldProblem(field)) {
            reason = *problem;
            break;
          }
        }
      }
      if (!declaration->name.empty()) {
        if (const auto first = recordIdByName_.find(declaration->name);
            first != recordIdByName_.end()) {
          recordBindings_.emplace(id, recordBindings_.at(first->second));
          if (const auto firstReason = recordReasons_.find(first->second);
              firstReason != recordReasons_.end()) {
            recordReasons_.emplace(id, firstReason->second);
          }
          duplicateRecords_.insert(id);
          continue;
        }
        recordIdByName_.emplace(declaration->name, id);
      }
      if (declaration->complete && reason.empty()) {
        recordBindings_.emplace(id, RecordBinding::CAbi);
      } else {
        recordBindings_.emplace(id, RecordBinding::Opaque);
        if (!reason.empty()) {
          recordReasons_.emplace(id, std::move(reason));
        }
      }
    }
  }

  // -- Declaration planning -------------------------------------------------

  [[nodiscard]] BindingItem planRecord(NativeDeclarationId id,
                                       const NativeDeclaration &declaration) {
    BindingItem item;
    item.name = spelled(declaration);
    item.origin = declaration.origin;
    if (const std::optional<std::string> problem =
            identifierProblem(item.name, true)) {
      return skipped(declaration, *problem);
    }
    if (recordBindings_.at(id) == RecordBinding::Opaque) {
      item.kind = BindingItemKind::Opaque;
      if (const auto reason = recordReasons_.find(id);
          reason != recordReasons_.end()) {
        item.reason = reason->second;
      }
      return item;
    }
    item.kind = BindingItemKind::Record;
    for (const NativeRecordField &field : declaration.fields) {
      synthesisHint_ = declaration.name + "_" + field.name;
      const NativeType *type = canonical(field.type);
      if (type->kind == NativeTypeKind::Array) {
        const ArrayShape shape = arrayShape(*type);
        const MappedType element =
            mapType(shape.elementId, false, TypeUse::RecordField);
        std::string spelling = element.spelling + " " + field.name;
        for (const std::uint64_t extent : shape.extents) {
          spelling += "[" + std::to_string(extent) + "]";
        }
        item.fields.push_back({field.name, std::move(spelling)});
        continue;
      }
      const MappedType mapped =
          mapType(field.type, false, TypeUse::RecordField);
      item.fields.push_back({field.name, mapped.spelling + " " + field.name});
    }
    return item;
  }

  [[nodiscard]] BindingItem planEnum(const NativeDeclaration &declaration) {
    BindingItem item;
    item.name = spelled(declaration);
    item.origin = declaration.origin;
    const MappedType mapped = mapEnum(declaration);
    if (!mapped.supported) {
      return skipped(declaration, mapped.reason);
    }
    item.aliasSpelling = enumIntegerMapping(declaration).spelling;
    for (const NativeEnumerator &enumerator : declaration.enumerators) {
      item.constants.push_back(
          {enumerator.name,
           std::visit([](auto value) { return std::to_string(value); },
                      enumerator.value)});
    }
    item.kind = BindingItemKind::Enum;
    return item;
  }

  [[nodiscard]] BindingItem planTypedef(const NativeDeclaration &declaration) {
    BindingItem item;
    item.name = spelled(declaration);
    item.origin = declaration.origin;
    synthesisHint_ = declaration.name;
    if (const std::optional<std::string> problem =
            identifierProblem(item.name, false)) {
      return skipped(declaration, *problem);
    }
    const NativeType *target = canonical(declaration.aliased);
    if (target == nullptr) {
      return skipped(declaration, "the typedef target is invalid");
    }
    if (const NativeType *function = callbackShape(*target)) {
      const MappedType shape = mapCallbackShape(*function);
      if (!shape.supported) {
        return skipped(declaration, shape.reason);
      }
      item.kind = BindingItemKind::Callback;
      const MappedType result = mapType(function->result, true);
      item.resultSpelling = result.spelling;
      for (std::size_t index = 0; index < function->parameters.size();
           ++index) {
        const MappedType parameter =
            mapType(function->parameters[index], false);
        item.parameters.push_back({std::string(), parameter.spelling});
      }
      return item;
    }
    const MappedType mapped =
        mapType(declaration.aliased, false, TypeUse::RecordField);
    if (!mapped.supported) {
      return skipped(declaration, mapped.reason);
    }
    item.kind = BindingItemKind::Alias;
    item.aliasSpelling = mapped.spelling;
    return item;
  }

  [[nodiscard]] BindingItem planFunction(const NativeDeclaration &declaration) {
    BindingItem item;
    item.name = spelled(declaration);
    item.origin = declaration.origin;
    if (const std::optional<std::string> problem =
            identifierProblem(item.name, false)) {
      return skipped(declaration, *problem);
    }
    const std::string &nativeSymbol =
        declaration.asmLabel.empty() ? declaration.name : declaration.asmLabel;
    if (const std::optional<std::string> problem =
            identifierProblem(nativeSymbol, true)) {
      return skipped(declaration, "link symbol: " + *problem);
    }
    if (item.name != nativeSymbol) {
      item.linkName = nativeSymbol;
    }
    const NativeType *type = model_.findType(declaration.type);
    if (type == nullptr || type->kind != NativeTypeKind::Function) {
      return skipped(declaration, "the function type is invalid");
    }
    if (!declaration.callingConvention.empty()) {
      return skipped(declaration, "the '" + declaration.callingConvention +
                                      "' calling convention is outside the "
                                      "bounded C surface");
    }
    if (type->variadic) {
      return skipped(declaration,
                     "variadic C calls are outside the bounded GTI C ABI");
    }
    // A caller-declared pointer-plus-count contract (C-07) changes both what
    // the result may be and what the emitted declaration carries.
    const BindingArrayReturn *arrayReturn = nullptr;
    for (const BindingArrayReturn &candidate : options_.arrayReturns) {
      if (candidate.function == declaration.name) {
        arrayReturn = &candidate;
        matchedArrayReturns_.insert(candidate.function);
        break;
      }
    }
    synthesisHint_ = declaration.name + "_result";
    const MappedType result = mapType(
        type->result, true,
        arrayReturn != nullptr ? TypeUse::ArrayResult : TypeUse::Ordinary);
    if (!result.supported) {
      return skipped(declaration, "result: " + result.reason);
    }
    if (arrayReturn != nullptr &&
        result.spelling.find('*') == std::string::npos) {
      return skipped(declaration,
                     "the declared array return is not a pointer result, "
                     "which '[[c_array]]' requires");
    }
    item.resultSpelling = result.spelling;
    for (std::size_t index = 0; index < type->parameters.size(); ++index) {
      std::string name;
      if (index < declaration.parameterNames.size()) {
        name = declaration.parameterNames[index].name;
      }
      if (name.empty() || identifierProblem(name, false).has_value()) {
        name = "argument" + std::to_string(index);
      }
      synthesisHint_ = declaration.name + "_" + name;
      const MappedType parameter = mapType(type->parameters[index], false,
                                           TypeUse::DirectFunctionParameter);
      if (!parameter.supported) {
        return skipped(declaration, "parameter " + std::to_string(index + 1) +
                                        ": " + parameter.reason);
      }
      item.parameters.push_back({std::move(name), parameter.spelling});
    }
    if (arrayReturn != nullptr) {
      if (const std::optional<std::string> problem = arrayCountProblem(
              declaration, *type, arrayReturn->countParameter, item)) {
        return skipped(declaration, *problem);
      }
      item.arrayCountParameter = arrayReturn->countParameter;
    }
    item.kind = BindingItemKind::Function;
    return item;
  }

  // The language requires the named count to be a parameter of type `I*`
  // with `I` a fixed-width integer and the pointee writable. Emitting
  // `[[c_array]]` against anything else would produce source the frontend
  // rejects, so the contract is validated here and refused with its reason.
  [[nodiscard]] std::optional<std::string>
  arrayCountProblem(const NativeDeclaration &declaration,
                    const NativeType &signature, const std::string &countName,
                    const BindingItem &item) const {
    std::size_t index = item.parameters.size();
    for (std::size_t candidate = 0; candidate < item.parameters.size();
         ++candidate) {
      if (item.parameters[candidate].name == countName) {
        index = candidate;
        break;
      }
    }
    if (index == item.parameters.size()) {
      return "the declared array count '" + countName +
             "' does not name a parameter of '" + declaration.name + "'";
    }
    if (index >= signature.parameters.size()) {
      return "the declared array count '" + countName + "' has no type";
    }
    const NativeType *parameter = canonical(signature.parameters[index]);
    if (parameter == nullptr || parameter->kind != NativeTypeKind::Pointer ||
        parameter->constPointee) {
      return "the declared array count '" + countName +
             "' must be a pointer to a writable fixed-width integer";
    }
    const NativeType *pointee = canonical(parameter->pointee);
    if (pointee == nullptr || pointee->kind != NativeTypeKind::Integer ||
        !admittedScalar(*pointee)) {
      return "the declared array count '" + countName +
             "' must point to a fixed-width integer";
    }
    return std::nullopt;
  }

  // The GTI type of an imported macro constant follows its value: int32_t
  // when the value fits, int64_t otherwise. Both are ordinary constexpr
  // scalars on the GTI side.
  [[nodiscard]] BindingItem planConstant(const NativeDeclaration &declaration) {
    BindingItem item;
    item.name = spelled(declaration);
    item.origin = declaration.origin;
    if (const std::optional<std::string> problem =
            identifierProblem(item.name, false)) {
      return skipped(declaration, *problem);
    }
    item.kind = BindingItemKind::Constant;
    item.constantKind = declaration.constantKind;
    switch (declaration.constantKind) {
    case NativeConstantKind::Integer: {
      if (declaration.constantValue ==
          std::numeric_limits<std::int64_t>::min()) {
        // The one int64 value whose literal cannot be spelled directly.
        return skipped(declaration,
                       "the constant is outside the bounded int64 range");
      }
      item.constantValue = declaration.constantValue;
      const bool fits32 =
          declaration.constantValue >=
              std::numeric_limits<std::int32_t>::min() &&
          declaration.constantValue <= std::numeric_limits<std::int32_t>::max();
      item.aliasSpelling = fits32 ? "int32_t" : "int64_t";
      break;
    }
    case NativeConstantKind::Floating:
      item.constantFloating = declaration.constantFloating;
      item.aliasSpelling = "double";
      break;
    case NativeConstantKind::String:
      // A string constant is an ordinary GTI string view; the language's own
      // conversion carries it into a c_string parameter at a call site.
      item.constantText = declaration.constantText;
      item.aliasSpelling = "std::string_view";
      break;
    }
    return item;
  }
};

} // namespace

BindingPlan planBindings(const NativeModel &model, BindingOptions options) {
  return Planner(model, std::move(options)).run();
}

} // namespace lang::bindgen
