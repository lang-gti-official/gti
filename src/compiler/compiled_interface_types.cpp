#include "gti/compiled_interface_types.h"
#include "gti/compiled_interface_names.h"
#include "gti/compiled_scalar_signature.h"
#include "gti/lowered_program.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
namespace lang {
namespace {
constexpr std::string_view magic = "gti.nominal-interface.1:";
constexpr std::size_t limit = 1024 * 1024;
struct Field {
  std::string name, type, initial;
  AccessModifier access = AccessModifier::Public;
  bool mutableBinding = false;
};
struct Nominal {
  std::string name;
  bool enumeration = false, exported = false;
  ClassKind kind = ClassKind::Struct;
  bool defaultDeclared = false;
  AccessModifier defaultAccess = AccessModifier::Public;
  std::string underlying;
  std::vector<Field> fields;
  std::vector<std::pair<std::string, EnumConstant>> values;
};
struct Interface {
  PersistentDeclarationOwner owner;
  std::vector<Nominal> types;
};
[[noreturn]] void invalid(std::string_view message) {
  throw std::invalid_argument("compiled nominal interface: " +
                              std::string(message));
}
void identifier(std::string_view name) {
  auto alpha = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  if (name.empty() || name.size() > 1024 || !alpha(name.front()) ||
      !std::all_of(name.begin(), name.end(),
                   [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); }))
    invalid("requires unqualified identifiers");
}
void put(std::string &out, std::string_view value) {
  if (value.size() > 16384 || out.size() + value.size() + 8 > limit)
    invalid("metadata size limit exceeded");
  out += std::to_string(value.size()) + ":" + std::string(value);
}
struct Reader {
  std::string_view bytes;
  std::string get() {
    const auto colon = bytes.find(':');
    std::size_t size = 0;
    if (colon == bytes.npos || colon == 0 || colon > 5)
      invalid("malformed field");
    auto parsed = std::from_chars(bytes.data(), bytes.data() + colon, size);
    if (parsed.ec != std::errc{} || parsed.ptr != bytes.data() + colon ||
        size > 16384 || size > bytes.size() - colon - 1)
      invalid("malformed field length");
    bytes.remove_prefix(colon + 1);
    std::string result(bytes.substr(0, size));
    bytes.remove_prefix(size);
    return result;
  }
  std::uint64_t number(std::uint64_t maximum) {
    const auto value = get();
    std::uint64_t result = 0;
    auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        result > maximum || std::to_string(result) != value)
      invalid("invalid integer field");
    return result;
  }
};
std::string encode(const Interface &value) {
  if (value.types.empty())
    return {};
  std::string out(magic);
  put(out, value.owner.package);
  put(out, value.owner.target);
  put(out, value.owner.module);
  put(out, std::to_string(value.types.size()));
  for (const auto &type : value.types) {
    put(out, type.name);
    put(out, type.enumeration ? "enum" : "class");
    put(out, type.exported ? "1" : "0");
    put(out, std::to_string(static_cast<unsigned>(type.kind)));
    put(out, type.defaultDeclared ? "1" : "0");
    put(out, std::to_string(static_cast<unsigned>(type.defaultAccess)));
    put(out, type.underlying);
    put(out, std::to_string(type.fields.size()));
    for (const auto &f : type.fields) {
      put(out, f.name);
      put(out, f.type);
      put(out, std::to_string(static_cast<unsigned>(f.access)));
      put(out, f.mutableBinding ? "1" : "0");
      put(out, f.initial);
    }
    put(out, std::to_string(type.values.size()));
    for (const auto &[name, constant] : type.values) {
      put(out, name);
      put(out, constant.negative ? "1" : "0");
      put(out, std::to_string(constant.magnitude));
    }
  }
  return out;
}
Interface decode(std::string_view bytes) {
  if (bytes.empty())
    return {};
  if (bytes.size() > limit || !bytes.starts_with(magic))
    invalid("unsupported metadata");
  Reader reader{bytes.substr(magic.size())};
  Interface result;
  result.owner = {reader.get(), reader.get(), reader.get(), {}};
  const auto count = reader.number(64);
  std::size_t fields = 0;
  for (std::size_t i = 0; i < count; ++i) {
    Nominal t;
    t.name = reader.get();
    if (!isCompiledInterfaceName(t.name))
      invalid("invalid qualified declaration name");
    if (!result.types.empty() && result.types.back().name >= t.name)
      invalid("duplicate or unordered type");
    const auto kind = reader.get();
    if (kind != "enum" && kind != "class")
      invalid("unknown declaration kind");
    t.enumeration = kind == "enum";
    t.exported = reader.number(1);
    t.kind = static_cast<ClassKind>(
        reader.number(static_cast<unsigned>(ClassKind::Struct)));
    t.defaultDeclared = reader.number(1);
    t.defaultAccess = static_cast<AccessModifier>(
        reader.number(static_cast<unsigned>(AccessModifier::Public)));
    if ((!t.defaultDeclared && t.defaultAccess != AccessModifier::Public) ||
        (t.enumeration && t.defaultDeclared))
      invalid("invalid default constructor contract");
    t.underlying = reader.get();
    const auto size = reader.number(256);
    fields += size;
    if (fields > 1024)
      invalid("too many fields");
    std::set<std::string> names;
    for (std::size_t n = 0; n < size; ++n) {
      Field f;
      f.name = reader.get();
      identifier(f.name);
      if (!names.insert(f.name).second)
        invalid("duplicate field");
      f.type = reader.get();
      f.access = static_cast<AccessModifier>(
          reader.number(static_cast<unsigned>(AccessModifier::Public)));
      f.mutableBinding = reader.number(1);
      f.initial = reader.get();
      t.fields.push_back(std::move(f));
    }
    const auto values = reader.number(256);
    for (std::size_t n = 0; n < values; ++n) {
      auto name = reader.get();
      identifier(name);
      if (!names.insert(name).second)
        invalid("duplicate enumerator");
      const bool negative = reader.number(1);
      const auto magnitude = reader.number(UINT64_MAX);
      if (negative && !magnitude)
        invalid("negative zero enumerator");
      t.values.push_back({std::move(name), {negative, magnitude}});
    }
    if ((t.enumeration && (t.underlying.empty() || !t.fields.empty())) ||
        (!t.enumeration && (!t.underlying.empty() || !t.values.empty())))
      invalid("inconsistent declaration shape");
    result.types.push_back(std::move(t));
  }
  if (!reader.bytes.empty() || encode(result) != bytes)
    invalid("noncanonical metadata");
  return result;
}
void bind(PersistentTypeRegistry &registry, const Nominal &n,
          const PersistentDeclarationOwner &owner, std::size_t id) {
  if (n.enumeration) {
    EnumTypeInfo info;
    info.id = id;
    info.qualifiedName = n.name;
    registry.bind(info, owner);
  } else {
    ClassTypeInfo info;
    info.id = id;
    info.qualifiedName = n.name;
    info.kind = n.kind;
    registry.bind(info, owner);
  }
}
PersistentTypeRegistry synthetic(const Interface &value) {
  PersistentTypeRegistry registry;
  for (std::size_t i = 0; i < value.types.size(); ++i)
    bind(registry, value.types[i], value.owner, i + 1);
  return registry;
}
SemanticType nominalType(const Nominal &n, std::size_t id) {
  SemanticType t(n.enumeration ? SemanticType::Enum : SemanticType::Class);
  if (n.enumeration)
    t.enumId = id;
  else
    t.classId = id;
  return t;
}
bool primitive(const SemanticType &t) {
  return t.kind != SemanticType::Class && t.kind != SemanticType::Enum &&
         t.kind != SemanticType::Reference && isCompiledScalarResultType(t);
}
bool plainValue(const SemanticType &t) {
  if (t != SemanticType::Void && primitive(t))
    return true;
  if (t.kind != SemanticType::Class && t.kind != SemanticType::Enum)
    return false;
  return t == nominalType(Nominal{.enumeration = t.kind == SemanticType::Enum},
                          t.kind == SemanticType::Class ? t.classId : t.enumId);
}
const LoweredDeclaration &declaration(const LoweredProgram &program,
                                      const SemanticType &type) {
  for (const auto &d : program.declarations()) {
    if (const auto *c = std::get_if<LoweredClassDeclaration>(&d.payload);
        c && type.kind == SemanticType::Class && c->id == type.classId)
      return d;
    if (const auto *e = std::get_if<LoweredEnumDeclaration>(&d.payload);
        e && type.kind == SemanticType::Enum && e->id == type.enumId)
      return d;
  }
  invalid("missing nominal declaration");
}
std::string initializer(const LoweredProgram &program,
                        const LoweredStorageDeclaration &storage) {
  if (!storage.hasInitializer)
    return {};
  if (!storage.initializerConstant)
    invalid("field default requires a resolved scalar constant");
  const auto &value = *storage.initializerConstant;
  if (const auto *i = std::get_if<ConstantInteger>(&value))
    return "i" + std::string(i->negative ? "-" : "") +
           std::to_string(i->magnitude);
  if (const auto *b = std::get_if<bool>(&value))
    return *b ? "b1" : "b0";
  if (const auto *f = std::get_if<BinaryFloat>(&value))
    return "f" + std::to_string(f->bits);
  if (const auto *c = std::get_if<CharacterLiteral>(&value))
    return "c" + std::to_string(c->value);
  if (const auto *e = std::get_if<ConstantEnum>(&value)) {
    const auto &owner = std::get<LoweredEnumDeclaration>(
        declaration(program, storage.type).payload);
    for (const auto &item : owner.enumerators)
      if (item.value.negative == e->value.negative &&
          item.value.magnitude == e->value.magnitude)
        return "e" + item.name;
  }
  invalid("unsupported field default constant");
}
Nominal project(const LoweredProgram &program, SourceUnitId root,
                const SemanticType &type,
                const PersistentTypeRegistry &registry) {
  const auto &d = declaration(program, type);
  Nominal result;
  if (d.ownerClass)
    invalid("nested class-owned declarations require owner metadata");
  if (const auto *c = std::get_if<LoweredClassDeclaration>(&d.payload)) {
    if (c->declaresOtherConstructors || c->declaresMemberFunctions ||
        c->declaresDestructor || c->declaresNestedTypes ||
        c->sourceUnit != root || c->kind == ClassKind::Union ||
        c->kind == ClassKind::Interface || !c->genericParameters.empty() ||
        !c->bases.empty() || c->definitionFamilyPrimary ||
        c->exactSpecializationPrimary || c->forwardDeclaration ||
        c->polymorphic || c->compilerPrivate || c->reflectPrivate ||
        c->cAbiRecord || c->cOpaqueHandle || c->requiresActiveDropState ||
        c->traits.containsBorrowedState ||
        c->compilerCapability != CompilerCapabilityTypeKind::None ||
        c->computedTypeProjection != ComputedTypeProjectionKind::None ||
        c->transferPolicy != ConcurrencyCapabilityPolicy::Structural ||
        c->sharePolicy != ConcurrencyCapabilityPolicy::Structural ||
        c->defaultConstructor == SpecialMemberStatus::Declared ||
        c->copyConstructor != SpecialMemberStatus::Generated ||
        c->moveConstructor != SpecialMemberStatus::Generated ||
        c->copyAssignment == SpecialMemberStatus::Declared ||
        c->moveAssignment == SpecialMemberStatus::Declared ||
        c->destructor != SpecialMemberStatus::Generated)
      invalid("record requires concrete generated value lifecycle without "
              "bases or custom policy");
    result.name = c->qualifiedName;
    result.kind = c->kind;
    result.exported = c->explicitlyExported;
    result.defaultDeclared = c->defaultedDefaultAccess.has_value();
    result.defaultAccess =
        c->defaultedDefaultAccess.value_or(AccessModifier::Public);
    for (const auto &member : program.declarations()) {
      if (member.ownerClass != c->id)
        continue;
      // Source constructor policy comes from the declaration-complete class
      // record. This inventory contains only reached executable instances,
      // including consumer-local designated construction.
      if (std::get_if<LoweredConstructorDeclaration>(&member.payload))
        continue;
      if (std::get_if<LoweredFunctionDeclaration>(&member.payload) ||
          std::get_if<LoweredDestructorDeclaration>(&member.payload))
        invalid("record methods require callable member metadata");
      const auto *storage =
          std::get_if<LoweredStorageDeclaration>(&member.payload);
      if (!storage) {
        if (!std::get_if<LoweredAccessDeclaration>(&member.payload))
          invalid("record member requires additional interface metadata");
        continue;
      }
      if (storage->staticStorage || !plainValue(storage->type))
        invalid("record fields require plain values without static storage");
      const auto *symbol = program.findSymbol(storage->symbol);
      if (!symbol)
        invalid("missing field symbol");
      result.fields.push_back({symbol->name,
                               writePersistentType(storage->type, registry),
                               initializer(program, *storage), symbol->access,
                               symbol->mutableBinding});
    }
  } else {
    const auto &e = std::get<LoweredEnumDeclaration>(d.payload);
    if (e.sourceUnit != root || e.payload || e.compilerPrivate ||
        !e.genericParameters.empty())
      invalid("requires concrete integral enum");
    result.name = e.qualifiedName;
    result.enumeration = true;
    result.exported = e.explicitlyExported;
    result.underlying = writePersistentType(e.underlyingType, registry);
    for (const auto &v : e.enumerators)
      result.values.push_back({v.name, v.value});
  }
  if (!isCompiledInterfaceName(result.name))
    invalid("invalid qualified declaration name");
  return result;
}
std::uint64_t number(std::string_view text) {
  std::uint64_t value = 0;
  auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      std::to_string(value) != text)
    invalid("noncanonical field default");
  return value;
}
std::string initialSource(const Field &field, const Interface &interface,
                          const PersistentTypeRegistry &registry,
                          std::string_view aliasPrefix) {
  const auto type = readPersistentType(field.type, registry);
  if (field.initial.empty()) {
    if (type.kind != SemanticType::Class)
      invalid("scalar field requires a default");
    return {};
  }
  const char kind = field.initial.front();
  auto payload = std::string_view(field.initial).substr(1);
  if (kind == 'i') {
    const bool negative = payload.starts_with('-');
    if (negative)
      payload.remove_prefix(1);
    const auto magnitude = number(payload);
    const auto domain = constantIntegerDomain(type);
    if (!domain || (negative && magnitude == 0) ||
        !convertConstantToType(ConstantInteger{negative, magnitude, *domain},
                               type)
             .value)
      invalid("integer default does not fit field");
    return " = " + field.initial.substr(1);
  }
  if (kind == 'b' && type == SemanticType::Bool &&
      (payload == "0" || payload == "1"))
    return payload == "1" ? " = true" : " = false";
  if (kind == 'e' && type.kind == SemanticType::Enum) {
    const auto name = registry.qualifiedName(type);
    for (const auto &nominal : interface.types)
      if (nominal.name == name)
        for (const auto &[enumerator, value] : nominal.values)
          if (enumerator == payload)
            return " = " + compiledInterfaceAliasName(name, aliasPrefix) +
                   "::" + enumerator;
    invalid("unknown default enumerator");
  }
  if (kind == 'f' &&
      (type == SemanticType::Float || type == SemanticType::Double)) {
    const auto bits = number(payload);
    const bool wide = type == SemanticType::Double;
    if (!wide && bits > UINT32_MAX)
      invalid("float default width mismatch");
    const auto value = wide ? std::bit_cast<double>(bits)
                            : static_cast<double>(std::bit_cast<float>(
                                  static_cast<std::uint32_t>(bits)));
    if (!std::isfinite(value))
      invalid("nonfinite field default");
    char buffer[1024];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                         std::chars_format::fixed);
    if (converted.ec != std::errc{})
      invalid("float default spelling failed");
    std::string spelling(buffer, converted.ptr);
    if (spelling.find_first_of(".eE") == std::string::npos)
      spelling += ".0";
    return " = " + spelling + (wide ? "d" : "");
  }
  if (kind == 'c' && type == SemanticType::Char) {
    const auto value = number(payload);
    std::string spelling;
    switch (value) {
    case 0:
      spelling = "\\0";
      break;
    case '\n':
      spelling = "\\n";
      break;
    case '\r':
      spelling = "\\r";
      break;
    case '\t':
      spelling = "\\t";
      break;
    case '\\':
      spelling = "\\\\";
      break;
    case '\'':
      spelling = "\\'";
      break;
    default:
      if (value < 32 || value > 126)
        invalid("character default requires a source literal");
      spelling = static_cast<char>(value);
    }
    return " = '" + spelling + "'";
  }
  invalid("default kind does not match field type");
}
} // namespace
std::string compiledInterfaceNamespaceSource(std::string_view name,
                                             std::string declaration) {
  if (!isCompiledInterfaceName(name))
    invalid("invalid qualified declaration name");
  std::string source;
  std::size_t depth = 0;
  for (auto separator = name.find("::"); separator != name.npos;
       separator = name.find("::")) {
    source += "namespace " + std::string(name.substr(0, separator)) + " {\n";
    name.remove_prefix(separator + 2);
    ++depth;
  }
  source += declaration;
  for (std::size_t i = 0; i < depth; ++i)
    source += "}\n";
  return source;
}
std::string
compiledInterfaceTypeSpelling(const SemanticType &type,
                              const PersistentTypeRegistry &registry,
                              std::string_view aliasPrefix) {
  if (primitive(type))
    return std::string(compiledScalarTypeSpelling(type));
  if (type.kind == SemanticType::Reference && type.arguments.size() == 1)
    return (type.referenceAccess == AccessMode::Mutable ? "mut " : "") +
           compiledInterfaceTypeSpelling(type.arguments.front(), registry,
                                         aliasPrefix) +
           "&";
  if (plainValue(type))
    return compiledInterfaceAliasName(registry.qualifiedName(type),
                                      aliasPrefix);
  invalid("unsupported type recipe");
}
std::string
projectCompiledInterfaceTypes(const LoweredProgram &program, SourceUnitId root,
                              const PersistentDeclarationOwner &owner,
                              std::span<const SemanticType> roots) {
  std::map<std::string, SemanticType> closure;
  std::function<void(const SemanticType &)> collect =
      [&](const SemanticType &t) {
        if (t.kind == SemanticType::Reference && t.arguments.size() == 1) {
          collect(t.arguments.front());
          return;
        }
        if (primitive(t))
          return;
        if (!plainValue(t))
          invalid("unsupported nominal type arguments");
        const auto &d = declaration(program, t);
        const auto name =
            t.kind == SemanticType::Class
                ? std::get<LoweredClassDeclaration>(d.payload).qualifiedName
                : std::get<LoweredEnumDeclaration>(d.payload).qualifiedName;
        if (closure.contains(name)) {
          if (closure.at(name) != t)
            invalid("conflicting nominal names");
          return;
        }
        if (closure.size() >= 64)
          invalid("too many nominal declarations");
        closure.emplace(name, t);
        if (t.kind == SemanticType::Class)
          for (const auto &member : program.declarations())
            if (member.ownerClass == t.classId)
              if (const auto *field =
                      std::get_if<LoweredStorageDeclaration>(&member.payload))
                collect(field->type);
      };
  for (const auto &type : roots)
    collect(type);
  Interface value{owner, {}};
  PersistentTypeRegistry registry;
  for (const auto &[name, type] : closure)
    bind(registry,
         Nominal{.name = name, .enumeration = type.kind == SemanticType::Enum},
         owner, type.kind == SemanticType::Class ? type.classId : type.enumId);
  for (const auto &[name, type] : closure)
    value.types.push_back(project(program, root, type, registry));
  const auto bytes = encode(value);
  (void)compiledInterfaceTypeSource(
      bytes); // Also verify closure, recipes and cycles.
  return bytes;
}
PersistentTypeRegistry
compiledInterfaceSyntheticRegistry(std::string_view bytes) {
  const auto value = decode(bytes);
  auto registry = synthetic(value);
  // Source projection is the structural metadata validator, including cycles.
  (void)compiledInterfaceTypeSource(bytes);
  return registry;
}
PersistentTypeRegistry
compiledInterfaceTypeRegistry(const LoweredProgram &program, SourceUnitId root,
                              std::string_view bytes) {
  const auto value = decode(bytes);
  PersistentTypeRegistry registry;
  std::vector<SemanticType> types;
  for (const auto &n : value.types) {
    std::optional<SemanticType> type;
    for (const auto &d : program.declarations()) {
      if (const auto *c = std::get_if<LoweredClassDeclaration>(&d.payload);
          c && !n.enumeration && c->sourceUnit == root &&
          c->qualifiedName == n.name)
        type = nominalType(n, c->id);
      if (const auto *e = std::get_if<LoweredEnumDeclaration>(&d.payload);
          e && n.enumeration && e->sourceUnit == root &&
          e->qualifiedName == n.name)
        type = e->type;
    }
    if (!type)
      invalid("imported declaration is missing");
    bind(registry, n, value.owner,
         n.enumeration ? type->enumId : type->classId);
    types.push_back(*type);
  }
  Interface actual{value.owner, {}};
  for (const auto &type : types)
    actual.types.push_back(project(program, root, type, registry));
  if (encode(actual) != bytes)
    invalid("imported nominal declaration contract mismatch");
  return registry;
}
std::vector<std::string>
compiledInterfaceDeclarationNames(std::string_view bytes) {
  std::vector<std::string> result;
  for (const auto &type : decode(bytes).types)
    result.push_back(type.name);
  return result;
}
std::string compiledInterfaceTypeSource(std::string_view bytes,
                                        std::string_view aliasPrefix) {
  const auto value = decode(bytes);
  const auto registry = synthetic(value);
  std::map<std::string, std::size_t> names;
  for (std::size_t i = 0; i < value.types.size(); ++i)
    names.emplace(value.types[i].name, i);
  std::vector<unsigned> state(value.types.size());
  std::string source;
  std::function<void(std::size_t)> emit = [&](std::size_t index) {
    if (state[index] == 2)
      return;
    if (state[index] == 1)
      invalid("recursive value layout");
    state[index] = 1;
    const auto &n = value.types[index];
    for (const auto &f : n.fields) {
      const auto t = readPersistentType(f.type, registry);
      if (!plainValue(t))
        invalid("unsupported field type");
      if (t.kind == SemanticType::Class || t.kind == SemanticType::Enum)
        emit(names.at(registry.qualifiedName(t)));
    }
    const std::string base(compiledInterfaceBaseName(n.name));
    std::string definition = n.exported ? "export " : "";
    if (n.enumeration) {
      const auto underlying = readPersistentType(n.underlying, registry);
      if (underlying.kind < SemanticType::Int8 ||
          underlying.kind > SemanticType::UInt64)
        invalid("enum underlying type is not integral");
      definition += "enum class " + base + " : " +
                    compiledInterfaceTypeSpelling(underlying, registry) + " {";
      for (const auto &[name, c] : n.values)
        definition += name + " = " + (c.negative ? "-" : "") +
                      std::to_string(c.magnitude) + ",";
    } else {
      definition +=
          (n.kind == ClassKind::Struct ? "struct " : "class ") + base + " {";
      for (const auto &f : n.fields) {
        definition += f.access == AccessModifier::Public      ? "public: "
                      : f.access == AccessModifier::Protected ? "protected: "
                                                              : "private: ";
        definition +=
            (f.mutableBinding ? "mut " : "") +
            compiledInterfaceTypeSpelling(readPersistentType(f.type, registry),
                                          registry, aliasPrefix) +
            " " + f.name + initialSource(f, value, registry, aliasPrefix) + ";";
      }
      if (n.defaultDeclared) {
        definition += n.defaultAccess == AccessModifier::Public ? "public: "
                      : n.defaultAccess == AccessModifier::Protected
                          ? "protected: "
                          : "private: ";
        definition += base + "() = default;";
      }
    }
    definition += "};\n";
    source += compiledInterfaceNamespaceSource(n.name, std::move(definition));
    if (!aliasPrefix.empty())
      source += "using " + compiledInterfaceAliasName(n.name, aliasPrefix) +
                " = " + n.name + ";\n";
    state[index] = 2;
  };
  for (std::size_t i = 0; i < value.types.size(); ++i)
    emit(i);
  return source;
}
} // namespace lang
