#include "gti/bindgen/binding_planner.h"

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
  return std::nullopt;
}

// `Unavailable` marks a record whose name the generated surface cannot
// spell at all (a reserved spelling); neither by-value nor pointer uses of
// it can import.
enum class RecordBinding { CAbi, Opaque, Unavailable };

// GTI's bounded ABI is position-aware: `c_string` crosses only function and
// callback boundaries, and the `Handle**` out-parameter form exists only as
// a direct function parameter. `RecordField` marks uses inside a [[c_abi]]
// record, whose field set admits neither.
enum class TypeUse { Ordinary, DirectFunctionParameter, RecordField };

struct MappedType {
  bool supported = false;
  std::string spelling;
  std::string reason;
};

[[nodiscard]] MappedType supportedType(std::string spelling) {
  return {true, std::move(spelling), {}};
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
    if (type.kind == NativeTypeKind::Floating) {
      return type.bitWidth == 64 ? "double" : "float";
    }
    return (type.isSigned ? "int" : "uint") + std::to_string(type.bitWidth) +
           "_t";
  }

  [[nodiscard]] static bool admittedScalar(const NativeType &type) {
    if (type.kind == NativeTypeKind::Integer) {
      return type.bitWidth == 8 || type.bitWidth == 16 || type.bitWidth == 32 ||
             type.bitWidth == 64;
    }
    return type.kind == NativeTypeKind::Floating &&
           (type.bitWidth == 32 || type.bitWidth == 64);
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
        // Callback alias: valid wherever the named callback type is.
        const MappedType shape = mapCallbackShape(*target);
        if (!shape.supported) {
          return shape;
        }
        return supportedType(alias->name);
      }
      const MappedType canonicalMapping =
          mapType(alias->aliased, allowVoid, use, pointerDepth);
      if (!canonicalMapping.supported) {
        return canonicalMapping;
      }
      // A skipped typedef never reaches the generated file, so a use that is
      // valid through the canonical type spells that type directly.
      if (const auto outcome = typedefImported_.find(direct->declaration);
          outcome != typedefImported_.end() && !outcome->second) {
        return canonicalMapping;
      }
      return supportedType(alias->name);
    }
    switch (direct->kind) {
    case NativeTypeKind::Void:
      if (allowVoid) {
        return supportedType("void");
      }
      return unsupportedType("'void' is only a result type");
    case NativeTypeKind::Bool:
      return unsupportedType(
          "'bool' is not a GTI C ABI scalar; use a fixed-width integer in "
          "the header");
    case NativeTypeKind::Char:
      return unsupportedType(
          "plain 'char' is not a GTI C ABI scalar; use int8_t/uint8_t, or "
          "'const char*' for strings");
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
      return supportedType(record->name);
    }
    case NativeTypeKind::Enum: {
      const NativeDeclaration *target =
          model_.findDeclaration(direct->declaration);
      if (target == nullptr) {
        return unsupportedType("the enum reference is invalid");
      }
      return supportedType(target->name);
    }
    case NativeTypeKind::Function:
      return unsupportedType(
          "an unnamed function type is outside the bounded surface; name it "
          "with a typedef");
    case NativeTypeKind::Array:
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
    if (pointerDepth >= 1) {
      return unsupportedType("pointers deeper than the bounded C out-parameter "
                             "surface are not supported");
    }
    const NativeType *pointee = canonical(pointer.pointee);
    if (pointee == nullptr) {
      return unsupportedType("the pointee type is invalid");
    }
    if (pointee->kind == NativeTypeKind::Function) {
      return mapFunctionPointer(*pointee);
    }
    if (pointee->kind == NativeTypeKind::Pointer) {
      if (use == TypeUse::RecordField) {
        return unsupportedType("a nested pointer has no GTI [[c_abi]] field "
                               "representation");
      }
      const NativeType *innerPointee = canonical(pointee->pointee);
      if (innerPointee == nullptr) {
        return unsupportedType("the nested pointee type is invalid");
      }
      if (innerPointee->kind == NativeTypeKind::Char && pointee->constPointee) {
        // `const char**` out parameters and `const char* const*` string
        // arrays share the language's c_string* spelling.
        return supportedType("c_string*");
      }
      if (pointer.constPointee) {
        return unsupportedType(
            "a const-qualified inner pointer has no bounded GTI C ABI "
            "out-parameter spelling");
      }
      if (use == TypeUse::DirectFunctionParameter && !pointee->constPointee &&
          innerPointee->kind == NativeTypeKind::Record) {
        const NativeDeclaration *record =
            model_.findDeclaration(innerPointee->declaration);
        const auto binding = recordBindings_.find(innerPointee->declaration);
        if (record != nullptr && binding != recordBindings_.end() &&
            binding->second == RecordBinding::Opaque) {
          return supportedType(record->name + "**");
        }
      }
      return unsupportedType(
          "two-level pointers are limited to const char** and exact opaque "
          "C handle out parameters");
    }
    if (pointee->kind == NativeTypeKind::Char) {
      if (use == TypeUse::RecordField) {
        return unsupportedType(
            "a 'char' pointer field has no GTI [[c_abi]] representation; "
            "c_string crosses only function and callback boundaries");
      }
      if (pointer.constPointee) {
        return supportedType("c_string");
      }
      return unsupportedType("a writable 'char*' has no GTI C ABI "
                             "representation; 'const char*' maps to "
                             "c_string");
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
      // Both [[c_abi]] records and [[c_opaque]] handles admit one-level
      // pointers; degradation to opaque keeps exactly these uses alive.
      return supportedType(qualifier + record->name + "*");
    }
    if (pointee->kind == NativeTypeKind::Enum) {
      const NativeDeclaration *target =
          model_.findDeclaration(pointee->declaration);
      if (target == nullptr) {
        return unsupportedType("the pointee enum reference is invalid");
      }
      return supportedType(qualifier + target->name + "*");
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
      return supportedType(existing->second);
    }
    if (!commitSynthesis_) {
      // Record classification only asks admissibility; the alias itself is
      // allocated when the emission pass reaches a committed use.
      return supportedType(rendered);
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
    return supportedType(name);
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
      node = model_.findType(node->element);
    }
    return shape;
  }

  [[nodiscard]] std::optional<std::string>
  fieldProblem(const NativeRecordField &field) const {
    if (const std::optional<std::string> problem =
            identifierProblem(field.name, false)) {
      return "field " + *problem;
    }
    const NativeType *type = model_.findType(field.type);
    if (type == nullptr) {
      return "field '" + field.name + "' has an invalid type";
    }
    if (type->kind == NativeTypeKind::Array) {
      // The ultimate element uses the ordinary field set: GTI computes array
      // layout recursively from admitted element facts (decision 013).
      const MappedType element =
          mapType(arrayShape(*type).elementId, false, TypeUse::RecordField);
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
              identifierProblem(declaration->name, true);
          problem.has_value() && !declaration->name.empty()) {
        recordBindings_.emplace(id, RecordBinding::Unavailable);
        recordReasons_.emplace(id, *problem);
        recordIdByName_.emplace(declaration->name, id);
        continue;
      }
      std::string reason;
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
    item.name = declaration.name;
    item.origin = declaration.origin;
    if (const std::optional<std::string> problem =
            identifierProblem(declaration.name, true)) {
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
      const NativeType *type = model_.findType(field.type);
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
    item.name = declaration.name;
    item.origin = declaration.origin;
    // An anonymous enum is a pure constant list: C cannot name it in a later
    // declaration, and GTI needs only the `constexpr int32_t` constants.
    if (!declaration.name.empty()) {
      if (const std::optional<std::string> problem =
              identifierProblem(declaration.name, false)) {
        return skipped(declaration, *problem);
      }
    }
    for (const NativeEnumerator &enumerator : declaration.enumerators) {
      if (const std::optional<std::string> problem =
              identifierProblem(enumerator.name, false)) {
        return skipped(declaration, "enumerator " + *problem);
      }
      if (enumerator.value < std::numeric_limits<std::int32_t>::min() ||
          enumerator.value > std::numeric_limits<std::int32_t>::max()) {
        return skipped(declaration,
                       "enumerator '" + enumerator.name +
                           "' is outside the int32_t range the C enum "
                           "mapping uses");
      }
      item.constants.push_back({enumerator.name, enumerator.value});
    }
    item.kind = BindingItemKind::Enum;
    return item;
  }

  [[nodiscard]] BindingItem planTypedef(const NativeDeclaration &declaration) {
    BindingItem item;
    item.name = declaration.name;
    item.origin = declaration.origin;
    synthesisHint_ = declaration.name;
    if (const std::optional<std::string> problem =
            identifierProblem(declaration.name, false)) {
      return skipped(declaration, *problem);
    }
    const NativeType *target = model_.findType(declaration.aliased);
    if (target == nullptr) {
      return skipped(declaration, "the typedef target is invalid");
    }
    if (target->kind == NativeTypeKind::Function) {
      const MappedType shape = mapCallbackShape(*target);
      if (!shape.supported) {
        return skipped(declaration, shape.reason);
      }
      item.kind = BindingItemKind::Callback;
      const MappedType result = mapType(target->result, true);
      item.resultSpelling = result.spelling;
      for (std::size_t index = 0; index < target->parameters.size(); ++index) {
        const MappedType parameter = mapType(target->parameters[index], false);
        item.parameters.push_back({std::string(), parameter.spelling});
      }
      return item;
    }
    const MappedType mapped = mapType(declaration.aliased, false);
    if (!mapped.supported) {
      return skipped(declaration, mapped.reason);
    }
    item.kind = BindingItemKind::Alias;
    item.aliasSpelling = mapped.spelling;
    return item;
  }

  [[nodiscard]] BindingItem planFunction(const NativeDeclaration &declaration) {
    BindingItem item;
    item.name = declaration.name;
    item.origin = declaration.origin;
    if (const std::optional<std::string> problem =
            identifierProblem(declaration.name, true)) {
      return skipped(declaration, *problem);
    }
    const NativeType *type = model_.findType(declaration.type);
    if (type == nullptr || type->kind != NativeTypeKind::Function) {
      return skipped(declaration, "the function type is invalid");
    }
    if (type->variadic) {
      return skipped(declaration,
                     "variadic C calls are outside the bounded GTI C ABI");
    }
    synthesisHint_ = declaration.name + "_result";
    const MappedType result = mapType(type->result, true);
    if (!result.supported) {
      return skipped(declaration, "result: " + result.reason);
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
    item.kind = BindingItemKind::Function;
    return item;
  }

  // The GTI type of an imported macro constant follows its value: int32_t
  // when the value fits, int64_t otherwise. Both are ordinary constexpr
  // scalars on the GTI side.
  [[nodiscard]] BindingItem planConstant(const NativeDeclaration &declaration) {
    BindingItem item;
    item.name = declaration.name;
    item.origin = declaration.origin;
    if (const std::optional<std::string> problem =
            identifierProblem(declaration.name, false)) {
      return skipped(declaration, *problem);
    }
    if (declaration.constantValue == std::numeric_limits<std::int64_t>::min()) {
      // The one int64 value whose literal cannot be spelled directly.
      return skipped(declaration,
                     "the constant is outside the bounded int64 range");
    }
    item.kind = BindingItemKind::Constant;
    item.constantValue = declaration.constantValue;
    const bool fits32 =
        declaration.constantValue >= std::numeric_limits<std::int32_t>::min() &&
        declaration.constantValue <= std::numeric_limits<std::int32_t>::max();
    item.aliasSpelling = fits32 ? "int32_t" : "int64_t";
    return item;
  }
};

} // namespace

BindingPlan planBindings(const NativeModel &model, BindingOptions options) {
  return Planner(model, std::move(options)).run();
}

} // namespace lang::bindgen
