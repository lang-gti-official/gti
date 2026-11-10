#include "gti/persistent_declaration.h"
#include <array>
#include <charconv>
#include <stdexcept>

namespace lang {
namespace {
constexpr std::size_t byteLimit = 16384, fieldLimit = 1024,
                      parameterLimit = 256;
constexpr std::string_view magic = "gti.function-key.1:";
// Wire spellings are explicit, never SemanticType::Kind ordinals.
constexpr std::array types{std::pair{"void", SemanticType::Void},
                           std::pair{"i8", SemanticType::Int8},
                           std::pair{"i16", SemanticType::Int16},
                           std::pair{"i32", SemanticType::Int32},
                           std::pair{"i64", SemanticType::Int64},
                           std::pair{"u8", SemanticType::UInt8},
                           std::pair{"u16", SemanticType::UInt16},
                           std::pair{"u32", SemanticType::UInt32},
                           std::pair{"u64", SemanticType::UInt64},
                           std::pair{"f32", SemanticType::Float},
                           std::pair{"f64", SemanticType::Double},
                           std::pair{"bool", SemanticType::Bool},
                           std::pair{"char", SemanticType::Char}};
[[noreturn]] void reject(const char *why) { throw std::invalid_argument(why); }
void text(std::string_view value) {
  if (value.empty() || value.size() > fieldLimit)
    reject("invalid persistent declaration field size");
  for (unsigned char c : value)
    if (c < 32 || c == 127)
      reject("control byte in persistent declaration identity");
}
void logicalUnit(std::string_view value) {
  text(value);
  if (value.front() == '/' || value.find('\\') != value.npos ||
      value.find(':') != value.npos)
    reject("persistent source identity must be a logical relative path");
  while (!value.empty()) {
    auto end = value.find('/');
    auto part = value.substr(0, end);
    if (part.empty() || part == "." || part == "..")
      reject("invalid logical source component");
    if (end == value.npos)
      break;
    value.remove_prefix(end + 1);
    if (value.empty())
      reject("empty logical source component");
  }
}
std::string_view tag(const SemanticType &type) {
  for (const auto &[name, kind] : types)
    if (type == SemanticType(kind) && kind != SemanticType::Void)
      return name;
  reject("persistent function key requires canonical fixed scalar parameters");
}
void append(std::string &out, std::string_view value,
            std::size_t limit = fieldLimit) {
  if (value.size() > limit)
    reject("persistent declaration field exceeds limit");
  auto length = std::to_string(value.size());
  if (length.size() + 1 + value.size() > byteLimit - out.size())
    reject("persistent declaration key exceeds byte limit");
  out += length;
  out += ':';
  out += value;
}
struct Reader {
  std::string_view bytes;
  std::string_view field(std::size_t limit = fieldLimit) {
    auto colon = bytes.find(':');
    if (colon == bytes.npos || colon == 0 || colon > 5 ||
        (colon > 1 && bytes.front() == '0'))
      reject("invalid persistent field length");
    std::size_t size = 0;
    auto result = std::from_chars(bytes.data(), bytes.data() + colon, size);
    if (result.ec != std::errc{} || result.ptr != bytes.data() + colon ||
        size > limit)
      reject("invalid persistent field length");
    bytes.remove_prefix(colon + 1);
    if (size > bytes.size())
      reject("truncated persistent declaration key");
    auto value = bytes.substr(0, size);
    bytes.remove_prefix(size);
    return value;
  }
};
} // namespace
namespace {
std::string writeFunctionKey(const PersistentFunctionKey &key,
                             const PersistentTypeRegistry *registry) {
  text(key.owner.package);
  text(key.owner.target);
  text(key.owner.module);
  text(key.qualifiedName);
  if (key.sourceLocal)
    logicalUnit(key.owner.unit);
  else if (!key.owner.unit.empty())
    reject("module declaration must not carry a source unit key");
  if (key.parameters.size() > parameterLimit)
    reject("too many persistent parameters");
  std::string out(registry ? "gti.function-key.2:" : magic);
  for (std::string_view field :
       {std::string_view(key.owner.package), std::string_view(key.owner.target),
        std::string_view(key.owner.module), std::string_view(key.owner.unit),
        std::string_view(key.qualifiedName)})
    append(out, field);
  append(out, key.sourceLocal ? "local" : "module");
  append(out, std::to_string(key.parameters.size()));
  for (const auto &type : key.parameters) {
    if (type == SemanticType::Void)
      reject("void is not a parameter type");
    append(out,
           registry ? writePersistentType(type, *registry)
                    : std::string(tag(type)),
           byteLimit);
  }
  return out;
}
PersistentFunctionKey readFunctionKey(std::string_view bytes,
                                      const PersistentTypeRegistry *registry) {
  const std::string_view prefix = registry ? "gti.function-key.2:" : magic;
  if (bytes.size() > byteLimit || !bytes.starts_with(prefix))
    reject("unsupported persistent function key format");
  const auto original = bytes;
  bytes.remove_prefix(prefix.size());
  Reader reader{bytes};
  PersistentFunctionKey result;
  result.owner.package = reader.field();
  result.owner.target = reader.field();
  result.owner.module = reader.field();
  result.owner.unit = reader.field();
  result.qualifiedName = reader.field();
  auto scope = reader.field();
  if (scope != "module" && scope != "local")
    reject("invalid persistent declaration scope");
  result.sourceLocal = scope == "local";
  auto count = reader.field();
  std::size_t parameters = 0;
  auto parsed =
      std::from_chars(count.data(), count.data() + count.size(), parameters);
  if (parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size() ||
      parameters > parameterLimit)
    reject("invalid persistent parameter count");
  for (std::size_t i = 0; i < parameters; ++i) {
    auto name = reader.field(registry ? byteLimit : fieldLimit);
    if (registry) {
      auto type = readPersistentType(name, *registry);
      if (type == SemanticType::Void)
        reject("void is not a parameter type");
      result.parameters.push_back(std::move(type));
      continue;
    }
    bool found = false;
    for (const auto &[wire, kind] : types)
      if (name == wire && kind != SemanticType::Void) {
        result.parameters.emplace_back(kind);
        found = true;
        break;
      }
    if (!found)
      reject("unsupported persistent parameter type");
  }
  if (!reader.bytes.empty() || writeFunctionKey(result, registry) != original)
    reject("noncanonical persistent declaration key");
  return result;
}
} // namespace
std::string writePersistentFunctionKey(const PersistentFunctionKey &key) {
  return writeFunctionKey(key, nullptr);
}
std::string writePersistentFunctionKey(const PersistentFunctionKey &key,
                                       const PersistentTypeRegistry &registry) {
  return writeFunctionKey(key, &registry);
}
PersistentFunctionKey readPersistentFunctionKey(std::string_view bytes) {
  return readFunctionKey(bytes, nullptr);
}
PersistentFunctionKey
readPersistentFunctionKey(std::string_view bytes,
                          const PersistentTypeRegistry &registry) {
  return readFunctionKey(bytes, bytes.starts_with(magic) ? nullptr : &registry);
}
std::string persistentFunctionKey(const FunctionInfo &function,
                                  const PersistentDeclarationOwner &owner) {
  if (function.ownerClass || !function.genericParameters.empty() ||
      function.parameterPack || !function.requirements.empty() ||
      function.linkage != LanguageLinkage::Gti || function.compilerPrivate ||
      function.macroExpansion || function.intrinsic != IntrinsicKind::None)
    reject("unsupported persistent function declaration profile");
  return writePersistentFunctionKey({owner, function.qualifiedName,
                                     function.internalLinkage,
                                     function.parameterTypes});
}

namespace {
constexpr std::string_view nominalMagic = "gti.nominal-key.2:";
std::string nominalKey(std::string_view kind, std::string_view name,
                       const PersistentDeclarationOwner &owner,
                       std::string_view shape) {
  text(owner.package);
  text(owner.target);
  text(owner.module);
  text(name);
  if (!owner.unit.empty())
    reject("nominal declaration must have module ownership");
  std::string out(nominalMagic);
  for (auto field :
       {kind, std::string_view(owner.package), std::string_view(owner.target),
        std::string_view(owner.module), name, shape})
    append(out, field);
  return out;
}
void validateShape(std::string_view shape) {
  if (shape.size() > 256)
    reject("too many generic declaration parameters");
  for (std::size_t i = 0; i < shape.size(); ++i)
    if (shape[i] != 't' && shape[i] != 'v' &&
        !(shape[i] == 'p' && i + 1 == shape.size()))
      reject("unsupported generic parameter shape");
}
std::string genericShape(const std::vector<GenericParameterInfo> &parameters) {
  std::string shape;
  if (parameters.size() > 256)
    reject("too many generic declaration parameters");
  for (const auto &p : parameters) {
    if (!p.typeArgumentPath.empty() || (p.pack && p.value))
      reject("unsupported generic parameter projection");
    shape += p.pack ? 'p' : p.value ? 'v' : 't';
  }
  validateShape(shape);
  return shape;
}
struct NominalIdentity {
  SemanticType::Kind kind;
  std::string shape;
  std::string name;
};
NominalIdentity nominalIdentity(std::string_view key) {
  if (key.size() > byteLimit || !key.starts_with(nominalMagic))
    reject("unsupported nominal identity");
  Reader r{key.substr(nominalMagic.size())};
  auto kind = r.field();
  if (kind != "class" && kind != "enum")
    reject("unknown nominal identity kind");
  PersistentDeclarationOwner owner;
  owner.package = r.field();
  owner.target = r.field();
  owner.module = r.field();
  auto name = r.field();
  auto shape = r.field();
  validateShape(shape);
  if (!r.bytes.empty() || nominalKey(kind, name, owner, shape) != key)
    reject("noncanonical nominal identity");
  return {kind == "class" ? SemanticType::Class : SemanticType::Enum,
          std::string(shape), std::string(name)};
}
void validateArguments(const SemanticType &type, std::string_view shape) {
  std::size_t types = 0, values = 0;
  for (char p : shape) {
    types += p == 't';
    values += p == 'v';
  }
  const bool pack = !shape.empty() && shape.back() == 'p';
  if (type.arguments.size() > 256 || type.valueArguments.size() > 256 ||
      type.arguments.size() < types ||
      (!pack && type.arguments.size() != types) ||
      type.valueArguments.size() != values)
    reject("generic nominal argument count mismatch");
  for (const auto &value : type.valueArguments)
    if (value != CompileTimeValue::uint64(value.value))
      reject("generic value must be a resolved uint64");
}
SemanticType plainNominal(SemanticType::Kind kind, std::size_t id) {
  SemanticType type(kind);
  if (kind == SemanticType::Class)
    type.classId = id;
  else
    type.enumId = id;
  return type;
}
std::size_t nominalId(const SemanticType &type) {
  if (type.kind != SemanticType::Class && type.kind != SemanticType::Enum)
    reject("not a nominal type");
  auto id = type.kind == SemanticType::Class ? type.classId : type.enumId;
  auto expected = plainNominal(type.kind, id);
  expected.arguments = type.arguments;
  expected.valueArguments = type.valueArguments;
  if (!id || type != expected)
    reject("unsupported nominal instance profile");
  return id;
}
struct TypeBudget {
  std::size_t nodes = 0;
  void visit(std::size_t depth) {
    if (depth > 32 || ++nodes > 256)
      reject("persistent type complexity exceeds limit");
  }
};
// Bound caller-owned trees before any canonical-shape comparison or copying.
void preflightType(const SemanticType &type, TypeBudget &budget,
                   std::size_t depth) {
  budget.visit(depth);
  if (type.arguments.size() > 256 || type.valueArguments.size() > 256)
    reject("too many type recipe arguments");
  for (const auto &value : type.valueArguments) {
    budget.visit(depth + 1);
    if (value != CompileTimeValue::uint64(value.value))
      reject("generic value must be a resolved uint64");
  }
  for (const auto &argument : type.arguments)
    preflightType(argument, budget, depth + 1);
}
void writeType(std::string &out, const SemanticType &type,
               const PersistentTypeRegistry &registry, TypeBudget &budget,
               std::size_t depth) {
  budget.visit(depth);
  for (auto [name, kind] : types)
    if (type == SemanticType(kind)) {
      append(out, name);
      return;
    }
  if (type.kind == SemanticType::Class || type.kind == SemanticType::Enum) {
    append(out, "nominal");
    append(out, registry.key(type), byteLimit);
    append(out, std::to_string(type.arguments.size()));
    for (const auto &argument : type.arguments)
      writeType(out, argument, registry, budget, depth + 1);
    append(out, std::to_string(type.valueArguments.size()));
    for (const auto &value : type.valueArguments) {
      budget.visit(depth + 1);
      append(out, std::to_string(value.value));
    }
    return;
  }
  if (type.arguments.size() != 1)
    reject("unsupported persistent type shape");
  const auto &element = type.arguments.front();
  SemanticType expected;
  std::string_view wire;
  switch (type.kind) {
  case SemanticType::Reference:
    if (type.referenceAccess != AccessMode::ReadOnly &&
        type.referenceAccess != AccessMode::Mutable)
      reject("invalid reference access");
    expected = SemanticType::referenceTo(element, type.referenceAccess);
    wire = type.referenceAccess == AccessMode::ReadOnly ? "ref" : "mut-ref";
    break;
  case SemanticType::RawPointer:
    if (type.pointerAccess != AccessMode::ReadOnly &&
        type.pointerAccess != AccessMode::Mutable)
      reject("invalid pointer access");
    expected = SemanticType::rawPointerTo(element, type.pointerAccess);
    wire = type.pointerAccess == AccessMode::ReadOnly ? "ptr" : "mut-ptr";
    break;
  case SemanticType::Array:
    expected = SemanticType::arrayOf(element, type.arrayLength);
    wire = "array";
    break;
  default:
    reject("unsupported persistent type profile");
  }
  if (type != expected)
    reject("noncanonical semantic type payload");
  append(out, wire);
  if (type.kind == SemanticType::Array)
    append(out, std::to_string(type.arrayLength));
  writeType(out, element, registry, budget, depth + 1);
}
SemanticType readType(Reader &r, const PersistentTypeRegistry &registry,
                      TypeBudget &budget, std::size_t depth) {
  budget.visit(depth);
  auto wire = r.field();
  for (auto [name, kind] : types)
    if (wire == name)
      return SemanticType(kind);
  if (wire == "nominal") {
    const auto key = r.field(byteLimit);
    auto count = [&]() {
      auto field = r.field();
      std::size_t value = 0;
      auto parsed =
          std::from_chars(field.data(), field.data() + field.size(), value);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != field.data() + field.size() || value > 256)
        reject("invalid generic argument count");
      return value;
    };
    std::vector<SemanticType> arguments;
    const auto typeCount = count();
    for (std::size_t i = 0; i < typeCount; ++i)
      arguments.push_back(readType(r, registry, budget, depth + 1));
    std::vector<CompileTimeValue> values;
    const auto valueCount = count();
    for (std::size_t i = 0; i < valueCount; ++i) {
      budget.visit(depth + 1);
      auto field = r.field();
      std::uint64_t value = 0;
      auto parsed =
          std::from_chars(field.data(), field.data() + field.size(), value);
      if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size())
        reject("invalid generic value");
      values.push_back(CompileTimeValue::uint64(value));
    }
    return registry.resolve(key, std::move(arguments), std::move(values));
  }
  std::uint64_t length = 0;
  if (wire == "array") {
    auto number = r.field();
    auto parsed =
        std::from_chars(number.data(), number.data() + number.size(), length);
    if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size())
      reject("invalid fixed-array extent");
  } else if (wire != "ref" && wire != "mut-ref" && wire != "ptr" &&
             wire != "mut-ptr")
    reject("unknown persistent type tag");
  auto element = readType(r, registry, budget, depth + 1);
  if (wire == "array")
    return SemanticType::arrayOf(std::move(element), length);
  if (wire == "ref" || wire == "mut-ref")
    return SemanticType::referenceTo(std::move(element),
                                     wire == "ref" ? AccessMode::ReadOnly
                                                   : AccessMode::Mutable);
  return SemanticType::rawPointerTo(std::move(element),
                                    wire == "ptr" ? AccessMode::ReadOnly
                                                  : AccessMode::Mutable);
}
} // namespace
std::string persistentClassKey(const ClassTypeInfo &info,
                               const PersistentDeclarationOwner &owner) {
  if (info.enclosingClass || info.definitionFamilyPrimary ||
      info.exactSpecializationPrimary || !info.exactTypeArguments.empty() ||
      !info.exactValueArguments.empty() ||
      !info.genericArgumentPatterns.empty() || info.compilerPrivate ||
      info.compilerCapability != CompilerCapabilityTypeKind::None ||
      info.computedTypeProjection != ComputedTypeProjectionKind::None)
    reject("unsupported persistent class declaration profile");
  return nominalKey("class", info.qualifiedName, owner,
                    genericShape(info.genericParameters));
}
std::string persistentEnumKey(const EnumTypeInfo &info,
                              const PersistentDeclarationOwner &owner) {
  if (info.compilerPrivate)
    reject("unsupported persistent enum declaration profile");
  return nominalKey("enum", info.qualifiedName, owner,
                    genericShape(info.genericParameters));
}
void PersistentTypeRegistry::bindKey(std::string key, SemanticType type) {
  auto id = nominalId(type);
  if (nominalIdentity(key).kind != type.kind)
    reject("nominal key kind mismatch");
  if (auto found = byKey.find(key); found != byKey.end()) {
    if (found->second != type)
      reject("nominal key already bound to another local declaration");
    return;
  }
  auto local = std::pair{type.kind, id};
  if (byId.contains(local))
    reject("local declaration already has a different persistent identity");
  if (byKey.size() >= 4096)
    reject("too many persistent nominal bindings");
  auto [position, inserted] = byKey.emplace(key, type);
  try {
    byId.emplace(local, std::move(key));
  } catch (...) {
    byKey.erase(position);
    throw;
  }
}
void PersistentTypeRegistry::bind(const ClassTypeInfo &info,
                                  const PersistentDeclarationOwner &owner) {
  bindKey(persistentClassKey(info, owner),
          plainNominal(SemanticType::Class, info.id));
}
void PersistentTypeRegistry::bind(const EnumTypeInfo &info,
                                  const PersistentDeclarationOwner &owner) {
  bindKey(persistentEnumKey(info, owner),
          plainNominal(SemanticType::Enum, info.id));
}
std::string PersistentTypeRegistry::key(const SemanticType &type) const {
  auto found = byId.find({type.kind, nominalId(type)});
  if (found == byId.end())
    reject("unregistered nominal declaration");
  validateArguments(type, nominalIdentity(found->second).shape);
  return found->second;
}
std::string
PersistentTypeRegistry::qualifiedName(const SemanticType &type) const {
  return nominalIdentity(key(type)).name;
}
SemanticType
PersistentTypeRegistry::resolve(std::string_view key,
                                std::vector<SemanticType> arguments,
                                std::vector<CompileTimeValue> values) const {
  const auto identity = nominalIdentity(key);
  auto found = byKey.find(key);
  if (found == byKey.end())
    reject("unresolved persistent nominal reference");
  auto result = found->second;
  result.arguments = std::move(arguments);
  result.valueArguments = std::move(values);
  validateArguments(result, identity.shape);
  return result;
}
std::string writePersistentType(const SemanticType &type,
                                const PersistentTypeRegistry &registry) {
  TypeBudget preflight;
  preflightType(type, preflight, 0);
  std::string out = "gti.type.2:";
  TypeBudget budget;
  writeType(out, type, registry, budget, 0);
  return out;
}
SemanticType readPersistentType(std::string_view bytes,
                                const PersistentTypeRegistry &registry) {
  constexpr std::string_view prefix = "gti.type.2:";
  if (bytes.size() > byteLimit || !bytes.starts_with(prefix))
    reject("unsupported persistent type format");
  Reader r{bytes.substr(prefix.size())};
  TypeBudget budget;
  auto result = readType(r, registry, budget, 0);
  if (!r.bytes.empty() || writePersistentType(result, registry) != bytes)
    reject("noncanonical persistent type recipe");
  return result;
}
} // namespace lang
