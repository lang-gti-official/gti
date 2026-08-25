#include "gti/bindgen/binding_planner.h"

#include <array>
#include <cctype>
#include <limits>
#include <optional>
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
  return std::nullopt;
}

enum class RecordBinding { CAbi, Opaque };

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
    classifyRecords();
    BindingPlan plan;
    plan.options = options_;
    for (const NativeDeclarationId id : model_.order) {
      const NativeDeclaration *declaration = model_.findDeclaration(id);
      if (declaration == nullptr) {
        continue;
      }
      switch (declaration->kind) {
      case NativeDeclarationKind::Record:
        plan.items.push_back(planRecord(id, *declaration));
        break;
      case NativeDeclarationKind::Enum:
        plan.items.push_back(planEnum(*declaration));
        break;
      case NativeDeclarationKind::Typedef:
        plan.items.push_back(planTypedef(*declaration));
        break;
      case NativeDeclarationKind::Function:
        plan.items.push_back(planFunction(*declaration));
        break;
      }
    }
    return plan;
  }

private:
  const NativeModel &model_;
  BindingOptions options_;
  std::unordered_map<NativeDeclarationId, RecordBinding> recordBindings_;
  std::unordered_map<NativeDeclarationId, std::string> recordReasons_;

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
          mapType(alias->aliased, allowVoid, pointerDepth);
      if (!canonicalMapping.supported) {
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
      return mapPointer(*direct, pointerDepth);
    case NativeTypeKind::Record: {
      const NativeDeclaration *record =
          model_.findDeclaration(direct->declaration);
      if (record == nullptr) {
        return unsupportedType("the record reference is invalid");
      }
      const auto binding = recordBindings_.find(direct->declaration);
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

  [[nodiscard]] MappedType mapPointer(const NativeType &pointer,
                                      std::size_t pointerDepth) const {
    if (pointerDepth >= 1) {
      return unsupportedType("multi-level pointers are outside the bounded "
                             "GTI C ABI");
    }
    const NativeType *pointee = canonical(pointer.pointee);
    if (pointee == nullptr) {
      return unsupportedType("the pointee type is invalid");
    }
    if (pointee->kind == NativeTypeKind::Pointer) {
      return unsupportedType("multi-level pointers are outside the bounded "
                             "GTI C ABI");
    }
    if (pointee->kind == NativeTypeKind::Char) {
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

  // -- Record classification ------------------------------------------------

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
      const NativeType *element = canonical(type->element);
      if (element == nullptr ||
          !(admittedScalar(*element) ||
            (element->kind == NativeTypeKind::Record &&
             recordBindings_.count(element->declaration) != 0 &&
             recordBindings_.at(element->declaration) ==
                 RecordBinding::CAbi))) {
        return "array field '" + field.name +
               "' has an element type outside the bounded surface";
      }
      return std::nullopt;
    }
    const MappedType mapped = mapType(field.type, false);
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
      const NativeType *type = model_.findType(field.type);
      if (type->kind == NativeTypeKind::Array) {
        const MappedType element = mapType(type->element, false);
        item.fields.push_back(
            {field.name, element.spelling + " " + field.name + "[" +
                             std::to_string(type->count) + "]"});
        continue;
      }
      const MappedType mapped = mapType(field.type, false);
      item.fields.push_back({field.name, mapped.spelling + " " + field.name});
    }
    return item;
  }

  [[nodiscard]] BindingItem planEnum(const NativeDeclaration &declaration) {
    BindingItem item;
    item.name = declaration.name;
    item.origin = declaration.origin;
    if (const std::optional<std::string> problem =
            identifierProblem(declaration.name, false)) {
      return skipped(declaration, *problem);
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
    const MappedType result = mapType(type->result, true);
    if (!result.supported) {
      return skipped(declaration, "result: " + result.reason);
    }
    item.resultSpelling = result.spelling;
    for (std::size_t index = 0; index < type->parameters.size(); ++index) {
      const MappedType parameter = mapType(type->parameters[index], false);
      if (!parameter.supported) {
        return skipped(declaration, "parameter " + std::to_string(index + 1) +
                                        ": " + parameter.reason);
      }
      std::string name;
      if (index < declaration.parameterNames.size()) {
        name = declaration.parameterNames[index].name;
      }
      if (name.empty() || identifierProblem(name, false).has_value()) {
        name = "argument" + std::to_string(index);
      }
      item.parameters.push_back({std::move(name), parameter.spelling});
    }
    item.kind = BindingItemKind::Function;
    return item;
  }
};

} // namespace

BindingPlan planBindings(const NativeModel &model, BindingOptions options) {
  return Planner(model, std::move(options)).run();
}

} // namespace lang::bindgen
