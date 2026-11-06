#include "gti/persistent_declaration.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <set>
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
  std::string out(registry ? "gti.function-key.3:" : magic);
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
  const std::string_view prefix = registry ? "gti.function-key.3:" : magic;
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
constexpr std::string_view nominalMagic = "gti.nominal-key.3:";
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
    if (shape[i] != 't' && shape[i] != 'v' && shape[i] != 'l' &&
        shape[i] != 'i' &&
        !(shape[i] == 'p' &&
          std::all_of(shape.begin() + i + 1, shape.end(),
                      [](char later) { return later == 'i'; })))
      reject("unsupported generic parameter shape");
}
std::string genericShape(const std::vector<GenericParameterInfo> &parameters) {
  std::string shape;
  if (parameters.size() > 256)
    reject("too many generic declaration parameters");
  for (const auto &p : parameters) {
    if (!p.typeArgumentPath.empty() || (p.pack && (p.value || p.lifetime)) ||
        (p.value && p.lifetime) || (p.implicit && !p.lifetime))
      reject("unsupported generic parameter projection");
    shape += p.pack       ? 'p'
             : p.lifetime ? (p.implicit ? 'i' : 'l')
             : p.value    ? 'v'
                          : 't';
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
  std::size_t types = 0, values = 0, lifetimes = 0;
  for (char p : shape) {
    types += p == 't';
    values += p == 'v';
    lifetimes += p == 'l' || p == 'i';
  }
  const bool pack = shape.find('p') != shape.npos;
  if (type.arguments.size() > 256 || type.valueArguments.size() > 256 ||
      type.arguments.size() < types ||
      (!pack && type.arguments.size() != types) ||
      type.valueArguments.size() != values ||
      type.lifetimeArguments.size() != lifetimes)
    reject("generic nominal argument count mismatch");
  for (const auto &value : type.valueArguments)
    if (value != CompileTimeValue::uint64(value.value))
      reject("generic value must be a resolved uint64");
  for (const Lifetime region : type.lifetimeArguments)
    if (!region.declarationRegion())
      reject("generic lifetime must be a resolved declaration region");
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
  expected.lifetimeArguments = type.lifetimeArguments;
  if (!id || type != expected)
    reject("unsupported nominal instance profile");
  return id;
}
constexpr std::string_view conceptMagic = "gti.concept-key.1:";
constexpr std::string_view associatedMagic = "gti.associated-type-key.1:";
std::string conceptKey(std::string_view name,
                       const PersistentDeclarationOwner &owner,
                       std::string_view shape) {
  text(owner.package);
  text(owner.target);
  text(owner.module);
  text(name);
  validateShape(shape);
  if (!owner.unit.empty() || shape.find('p') != shape.npos)
    reject("unsupported persistent concept ownership or pack shape");
  std::string result(conceptMagic);
  for (auto field :
       {std::string_view(owner.package), std::string_view(owner.target),
        std::string_view(owner.module), name, shape})
    append(result, field);
  return result;
}
struct ConceptIdentity {
  std::string name;
  std::string shape;
};
ConceptIdentity conceptIdentity(std::string_view key) {
  if (key.size() > byteLimit || !key.starts_with(conceptMagic))
    reject("unsupported persistent concept identity");
  Reader reader{key.substr(conceptMagic.size())};
  PersistentDeclarationOwner owner;
  owner.package = reader.field();
  owner.target = reader.field();
  owner.module = reader.field();
  const auto name = reader.field();
  const auto shape = reader.field();
  if (!reader.bytes.empty() || conceptKey(name, owner, shape) != key)
    reject("noncanonical persistent concept identity");
  return {std::string(name), std::string(shape)};
}
std::string associatedKey(std::string_view conceptIdentityValue,
                          std::string_view name, std::string_view shape) {
  (void)conceptIdentity(conceptIdentityValue);
  text(name);
  validateShape(shape);
  std::string result(associatedMagic);
  append(result, conceptIdentityValue, byteLimit);
  append(result, name);
  append(result, shape);
  return result;
}
struct AssociatedIdentity {
  std::string shape;
};
AssociatedIdentity associatedIdentity(std::string_view key) {
  if (key.size() > byteLimit || !key.starts_with(associatedMagic))
    reject("unsupported persistent associated-family identity");
  Reader reader{key.substr(associatedMagic.size())};
  const auto conceptIdentityValue = reader.field(byteLimit);
  const auto name = reader.field();
  const auto shape = reader.field();
  const auto owner = conceptIdentity(conceptIdentityValue);
  if (!reader.bytes.empty() ||
      associatedKey(conceptIdentityValue, name, shape) != key)
    reject("noncanonical persistent associated-family identity");
  return {owner.shape + std::string(shape)};
}
SemanticType plainProjection(ConceptId conceptIdentityValue,
                             std::uint32_t member) {
  auto result = SemanticType::typeParameter(0);
  result.projectionConcept = conceptIdentityValue;
  result.projectionMember = member;
  return result;
}
void validateProjection(const SemanticType &type) {
  auto expected =
      plainProjection(type.projectionConcept, type.projectionMember);
  expected.arguments = type.arguments;
  expected.valueArguments = type.valueArguments;
  expected.lifetimeArguments = type.lifetimeArguments;
  if (!type.isAssociatedTypeProjection() || type != expected)
    reject("noncanonical associated projection payload");
}
struct TypeBudget {
  std::size_t nodes = 0;
  void visit(std::size_t depth) {
    if (depth > 32 || ++nodes > 256)
      reject("persistent type complexity exceeds limit");
  }
};

std::size_t count(Reader &reader, std::size_t maximum = 256) {
  const auto field = reader.field();
  std::size_t result = 0;
  const auto parsed =
      std::from_chars(field.data(), field.data() + field.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size() ||
      result > maximum || std::to_string(result) != field)
    reject("invalid persistent count or binder position");
  return result;
}

class LifetimeCodec {
public:
  explicit LifetimeCodec(const PersistentLifetimeBindings &bindings,
                         bool allowQuantified = true)
      : bindings(bindings), scope(bindings.parameters),
        allowQuantified(allowQuantified) {
    if (bindings.parameters.size() > 256 ||
        bindings.quantifiedParameters.size() > 256 ||
        bindings.typeParameters.size() > 256)
      reject("too many persistent lifetime bindings");
    std::set<GenericParameterId> seen;
    for (const auto &ids : {bindings.parameters, bindings.quantifiedParameters,
                            bindings.typeParameters})
      for (GenericParameterId id : ids)
        if (id == 0 || !seen.insert(id).second)
          reject("invalid or duplicate persistent lifetime binding");
  }

  void write(std::string &out, Lifetime region) const {
    switch (region.kind) {
    case Lifetime::Kind::Unspecified:
      if (region.id != 0)
        reject("invalid unspecified lifetime");
      append(out, "none");
      return;
    case Lifetime::Kind::Static:
      if (region.id != 0)
        reject("invalid static lifetime");
      append(out, "static");
      return;
    case Lifetime::Kind::Parameter: {
      const auto found = std::find(scope.begin(), scope.end(), region.id);
      if (region.id == 0 || found == scope.end())
        reject("unbound persistent lifetime parameter");
      append(out, "parameter");
      append(out, std::to_string(std::distance(scope.begin(), found)));
      return;
    }
    case Lifetime::Kind::Inference:
    case Lifetime::Kind::Local:
      reject("body-local or inferred lifetime is not an interface contract");
    }
    reject("invalid lifetime kind");
  }

  Lifetime read(Reader &reader) const {
    const auto tag = reader.field();
    if (tag == "none")
      return {};
    if (tag == "static")
      return Lifetime::staticLifetime();
    if (tag != "parameter" || scope.empty())
      reject("invalid or unbound persistent lifetime");
    return Lifetime::parameter(scope[count(reader, scope.size() - 1)]);
  }

  void writeTypeParameter(std::string &out, GenericParameterId id) const {
    const auto found = std::find(bindings.typeParameters.begin(),
                                 bindings.typeParameters.end(), id);
    if (id == 0 || found == bindings.typeParameters.end())
      reject("unbound persistent type parameter");
    append(out, std::to_string(
                    std::distance(bindings.typeParameters.begin(), found)));
  }

  GenericParameterId readTypeParameter(Reader &reader) const {
    if (bindings.typeParameters.empty())
      reject("unbound persistent type parameter");
    return bindings
        .typeParameters[count(reader, bindings.typeParameters.size() - 1)];
  }

  void enterWritten(std::span<const GenericParameterId> parameters) {
    if (!allowQuantified && !parameters.empty())
      reject("quantified lifetime types require explicit binding context");
    for (GenericParameterId id : parameters) {
      if (id == 0 || std::find(scope.begin(), scope.end(), id) != scope.end())
        reject("invalid or shadowed quantified lifetime binding");
      if (!bindings.quantifiedParameters.empty() &&
          (quantified >= bindings.quantifiedParameters.size() ||
           bindings.quantifiedParameters[quantified] != id))
        reject("quantified lifetime bindings disagree with type");
      if (++quantified > 256)
        reject("too many quantified lifetime bindings");
      scope.push_back(id);
    }
  }

  std::vector<GenericParameterId> enterRead(std::size_t size) {
    if (size > bindings.quantifiedParameters.size() -
                   std::min(quantified, bindings.quantifiedParameters.size()))
      reject("missing quantified lifetime bindings");
    std::vector<GenericParameterId> result;
    for (std::size_t index = 0; index < size; ++index) {
      const GenericParameterId id = bindings.quantifiedParameters[quantified++];
      if (std::find(scope.begin(), scope.end(), id) != scope.end())
        reject("shadowed quantified lifetime binding");
      scope.push_back(id);
      result.push_back(id);
    }
    return result;
  }

  void leave(std::size_t size) { scope.resize(scope.size() - size); }
  void finish(bool reading) const {
    if ((reading || !bindings.quantifiedParameters.empty()) &&
        quantified != bindings.quantifiedParameters.size())
      reject("unused quantified lifetime bindings");
  }

private:
  const PersistentLifetimeBindings &bindings;
  std::vector<GenericParameterId> scope;
  std::size_t quantified = 0;
  bool allowQuantified = true;
};

// Bound caller-owned trees before canonical-shape comparison or copying.
void preflightType(const SemanticType &type, TypeBudget &budget,
                   std::size_t depth) {
  budget.visit(depth);
  if (type.arguments.size() > 256 || type.valueArguments.size() > 256 ||
      type.lifetimeArguments.size() > 256 ||
      type.quantifiedLifetimes.size() > 256)
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
               LifetimeCodec &lifetimes, std::size_t depth) {
  budget.visit(depth);
  for (auto [name, kind] : types)
    if (type == SemanticType(kind)) {
      append(out, name);
      return;
    }
  if (type.kind == SemanticType::Class || type.kind == SemanticType::Enum ||
      type.isAssociatedTypeProjection()) {
    const bool projection = type.isAssociatedTypeProjection();
    append(out, projection ? "projection" : "nominal");
    append(out, projection ? registry.projectionKey(type) : registry.key(type),
           byteLimit);
    append(out, std::to_string(type.arguments.size()));
    for (const auto &argument : type.arguments)
      writeType(out, argument, registry, budget, lifetimes, depth + 1);
    append(out, std::to_string(type.valueArguments.size()));
    for (const auto &value : type.valueArguments) {
      budget.visit(depth + 1);
      append(out, std::to_string(value.value));
    }
    append(out, std::to_string(type.lifetimeArguments.size()));
    for (Lifetime region : type.lifetimeArguments) {
      budget.visit(depth + 1);
      lifetimes.write(out, region);
    }
    return;
  }
  if (type.kind == SemanticType::TypeParameter) {
    if (type != SemanticType::typeParameter(type.genericParameterId))
      reject("unsupported persistent type parameter or associated projection");
    append(out, "type-parameter");
    lifetimes.writeTypeParameter(out, type.genericParameterId);
    return;
  }
  if (type.kind == SemanticType::StringView) {
    SemanticType expected(SemanticType::StringView);
    expected.lifetimeArguments = type.lifetimeArguments;
    if (type != expected || type.lifetimeArguments.size() > 1)
      reject("noncanonical text-view type payload");
    append(out, "text-view");
    append(out, std::to_string(type.lifetimeArguments.size()));
    for (Lifetime region : type.lifetimeArguments) {
      budget.visit(depth + 1);
      if (!region.declarationRegion())
        reject("text-view lifetime must be a resolved declaration region");
      lifetimes.write(out, region);
    }
    return;
  }
  if (type.kind == SemanticType::FunctionSignature) {
    if (type.arguments.empty())
      reject("function signature has no result type");
    auto expected = SemanticType::functionSignature(
        type.arguments.front(),
        std::vector<SemanticType>(type.arguments.begin() + 1,
                                  type.arguments.end()),
        type.signatureReceiver);
    expected.quantifiedLifetimes = type.quantifiedLifetimes;
    if (type != expected)
      reject("noncanonical function signature payload");
    const auto receiver = type.signatureReceiver;
    if (receiver != ReceiverMutability::ReadOnly &&
        receiver != ReceiverMutability::Mutable &&
        receiver != ReceiverMutability::Consuming)
      reject("invalid function signature receiver");
    append(out, "signature");
    append(out, receiver == ReceiverMutability::ReadOnly  ? "read"
                : receiver == ReceiverMutability::Mutable ? "mut"
                                                          : "consume");
    append(out, std::to_string(type.quantifiedLifetimes.size()));
    lifetimes.enterWritten(type.quantifiedLifetimes);
    append(out, std::to_string(type.arguments.size()));
    for (const auto &argument : type.arguments)
      writeType(out, argument, registry, budget, lifetimes, depth + 1);
    lifetimes.leave(type.quantifiedLifetimes.size());
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
    expected.referenceLifetime = type.referenceLifetime;
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
  if (type.kind == SemanticType::Reference)
    lifetimes.write(out, type.referenceLifetime);
  writeType(out, element, registry, budget, lifetimes, depth + 1);
}
SemanticType readType(Reader &r, const PersistentTypeRegistry &registry,
                      TypeBudget &budget, LifetimeCodec &lifetimes,
                      std::size_t depth) {
  budget.visit(depth);
  const auto wire = r.field();
  for (auto [name, kind] : types)
    if (wire == name)
      return SemanticType(kind);
  if (wire == "type-parameter")
    return SemanticType::typeParameter(lifetimes.readTypeParameter(r));
  if (wire == "nominal" || wire == "projection") {
    const auto key = r.field(byteLimit);
    std::vector<SemanticType> arguments;
    const auto typeCount = count(r);
    for (std::size_t i = 0; i < typeCount; ++i)
      arguments.push_back(readType(r, registry, budget, lifetimes, depth + 1));
    std::vector<CompileTimeValue> values;
    const auto valueCount = count(r);
    for (std::size_t i = 0; i < valueCount; ++i) {
      budget.visit(depth + 1);
      const auto field = r.field();
      std::uint64_t value = 0;
      const auto parsed =
          std::from_chars(field.data(), field.data() + field.size(), value);
      if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size())
        reject("invalid generic value");
      values.push_back(CompileTimeValue::uint64(value));
    }
    std::vector<Lifetime> regions;
    const auto lifetimeCount = count(r);
    for (std::size_t i = 0; i < lifetimeCount; ++i) {
      budget.visit(depth + 1);
      regions.push_back(lifetimes.read(r));
    }
    return wire == "projection"
               ? registry.resolveProjection(key, std::move(arguments),
                                            std::move(values),
                                            std::move(regions))
               : registry.resolve(key, std::move(arguments), std::move(values),
                                  std::move(regions));
  }
  if (wire == "text-view") {
    SemanticType result(SemanticType::StringView);
    const auto size = count(r, 1);
    for (std::size_t index = 0; index < size; ++index) {
      budget.visit(depth + 1);
      result.lifetimeArguments.push_back(lifetimes.read(r));
    }
    return result;
  }
  if (wire == "signature") {
    const auto receiver = r.field();
    if (receiver != "read" && receiver != "mut" && receiver != "consume")
      reject("invalid function signature receiver");
    auto parameters = lifetimes.enterRead(count(r));
    const auto size = count(r);
    if (size == 0)
      reject("function signature has no result type");
    std::vector<SemanticType> arguments;
    for (std::size_t index = 0; index < size; ++index)
      arguments.push_back(readType(r, registry, budget, lifetimes, depth + 1));
    auto result = SemanticType::functionSignature(
        arguments.front(),
        std::vector<SemanticType>(arguments.begin() + 1, arguments.end()),
        receiver == "read"  ? ReceiverMutability::ReadOnly
        : receiver == "mut" ? ReceiverMutability::Mutable
                            : ReceiverMutability::Consuming);
    result.quantifiedLifetimes = std::move(parameters);
    lifetimes.leave(result.quantifiedLifetimes.size());
    return result;
  }
  std::uint64_t length = 0;
  if (wire == "array") {
    const auto number = r.field();
    const auto parsed =
        std::from_chars(number.data(), number.data() + number.size(), length);
    if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size())
      reject("invalid fixed-array extent");
  } else if (wire != "ref" && wire != "mut-ref" && wire != "ptr" &&
             wire != "mut-ptr") {
    reject("unknown persistent type tag");
  }
  Lifetime region;
  if (wire == "ref" || wire == "mut-ref")
    region = lifetimes.read(r);
  auto element = readType(r, registry, budget, lifetimes, depth + 1);
  if (wire == "array")
    return SemanticType::arrayOf(std::move(element), length);
  if (wire == "ref" || wire == "mut-ref") {
    auto result = SemanticType::referenceTo(
        std::move(element),
        wire == "ref" ? AccessMode::ReadOnly : AccessMode::Mutable);
    result.referenceLifetime = region;
    return result;
  }
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
std::string persistentConceptKey(const ProtocolConceptSpelling &info,
                                 const PersistentDeclarationOwner &owner) {
  const auto shape = genericShape(info.genericParameters);
  const auto typeCount = std::count(shape.begin(), shape.end(), 't');
  if (static_cast<std::size_t>(typeCount) != info.parameterCount)
    reject("incomplete persistent concept generic-kind descriptor");
  return conceptKey(info.qualifiedName, owner, shape);
}
std::string
persistentAssociatedTypeKey(const ProtocolConceptSpelling &info,
                            std::size_t member,
                            const PersistentDeclarationOwner &owner) {
  if (info.associatedTypes.size() > 256 ||
      info.associatedTypes.size() != info.associatedTypeParameters.size() ||
      member >= info.associatedTypes.size())
    reject("incomplete persistent associated-family descriptor");
  return associatedKey(persistentConceptKey(info, owner),
                       info.associatedTypes[member],
                       genericShape(info.associatedTypeParameters[member]));
}
void PersistentTypeRegistry::bind(ConceptId id,
                                  const ProtocolConceptSpelling &info,
                                  const PersistentDeclarationOwner &owner) {
  if (id == 0 || info.associatedTypes.size() > 256 ||
      info.associatedTypes.size() != info.associatedTypeParameters.size())
    reject("invalid persistent concept declaration descriptor");
  const auto conceptIdentityValue = persistentConceptKey(info, owner);
  if (auto found = conceptsByKey.find(conceptIdentityValue);
      found != conceptsByKey.end() && found->second != id)
    reject("concept key already bound to another local declaration");
  if (auto found = conceptKeys.find(id);
      found != conceptKeys.end() && found->second != conceptIdentityValue)
    reject("local concept already has another persistent identity");
  std::vector<std::string> keys;
  std::set<std::string> names;
  std::size_t newKeys = 0;
  for (std::size_t member = 0; member < info.associatedTypes.size(); ++member) {
    if (!names.insert(info.associatedTypes[member]).second)
      reject("duplicate persistent associated-family name");
    auto key = persistentAssociatedTypeKey(info, member, owner);
    const auto expected =
        plainProjection(id, static_cast<std::uint32_t>(member));
    if (auto found = projectionsByKey.find(key);
        found != projectionsByKey.end()) {
      if (found->second != expected)
        reject(
            "associated-family key already bound to another local declaration");
    } else {
      ++newKeys;
    }
    if (auto found =
            projectionKeys.find({id, static_cast<std::uint32_t>(member)});
        found != projectionKeys.end() && found->second != key)
      reject("local associated family already has another persistent identity");
    keys.push_back(std::move(key));
  }
  if (byKey.size() + projectionsByKey.size() + newKeys > 4096 ||
      (conceptKeys.size() >= 4096 && !conceptKeys.contains(id)))
    reject("too many persistent declaration bindings");
  const bool newConcept = !conceptKeys.contains(id);
  std::vector<std::size_t> inserted;
  try {
    conceptsByKey.emplace(conceptIdentityValue, id);
    conceptKeys.emplace(id, conceptIdentityValue);
    for (std::size_t member = 0; member < keys.size(); ++member) {
      if (projectionsByKey.contains(keys[member]))
        continue;
      inserted.push_back(member);
      projectionsByKey.emplace(
          keys[member],
          plainProjection(id, static_cast<std::uint32_t>(member)));
      projectionKeys.emplace(std::pair{id, static_cast<std::uint32_t>(member)},
                             keys[member]);
    }
  } catch (...) {
    for (auto member : inserted) {
      projectionsByKey.erase(keys[member]);
      projectionKeys.erase({id, static_cast<std::uint32_t>(member)});
    }
    if (newConcept) {
      conceptsByKey.erase(conceptIdentityValue);
      conceptKeys.erase(id);
    }
    throw;
  }
}
std::string
PersistentTypeRegistry::projectionKey(const SemanticType &type) const {
  validateProjection(type);
  auto found =
      projectionKeys.find({type.projectionConcept, type.projectionMember});
  if (found == projectionKeys.end())
    reject("unregistered associated-type projection");
  validateArguments(type, associatedIdentity(found->second).shape);
  return found->second;
}
SemanticType PersistentTypeRegistry::resolveProjection(
    std::string_view key, std::vector<SemanticType> arguments,
    std::vector<CompileTimeValue> values,
    std::vector<Lifetime> lifetimes) const {
  const auto identity = associatedIdentity(key);
  auto found = projectionsByKey.find(key);
  if (found == projectionsByKey.end())
    reject("unresolved persistent associated-type projection");
  auto result = found->second;
  result.arguments = std::move(arguments);
  result.valueArguments = std::move(values);
  result.lifetimeArguments = std::move(lifetimes);
  validateArguments(result, identity.shape);
  return result;
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
  if (byKey.size() + projectionsByKey.size() >= 4096)
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
                                std::vector<CompileTimeValue> values,
                                std::vector<Lifetime> lifetimes) const {
  const auto identity = nominalIdentity(key);
  auto found = byKey.find(key);
  if (found == byKey.end())
    reject("unresolved persistent nominal reference");
  auto result = found->second;
  result.arguments = std::move(arguments);
  result.valueArguments = std::move(values);
  result.lifetimeArguments = std::move(lifetimes);
  validateArguments(result, identity.shape);
  return result;
}
std::string writePersistentType(const SemanticType &type,
                                const PersistentTypeRegistry &registry) {
  TypeBudget preflight;
  preflightType(type, preflight, 0);
  std::string out = "gti.type.3:";
  TypeBudget budget;
  const PersistentLifetimeBindings bindings;
  LifetimeCodec lifetimes(bindings, false);
  writeType(out, type, registry, budget, lifetimes, 0);
  lifetimes.finish(false);
  return out;
}
std::string writePersistentType(const SemanticType &type,
                                const PersistentTypeRegistry &registry,
                                const PersistentLifetimeBindings &bindings) {
  TypeBudget preflight;
  preflightType(type, preflight, 0);
  std::string out = "gti.type.3:";
  TypeBudget budget;
  LifetimeCodec lifetimes(bindings);
  writeType(out, type, registry, budget, lifetimes, 0);
  lifetimes.finish(false);
  return out;
}
SemanticType readPersistentType(std::string_view bytes,
                                const PersistentTypeRegistry &registry) {
  return readPersistentType(bytes, registry, {});
}
SemanticType readPersistentType(std::string_view bytes,
                                const PersistentTypeRegistry &registry,
                                const PersistentLifetimeBindings &bindings) {
  constexpr std::string_view prefix = "gti.type.3:";
  if (bytes.size() > byteLimit || !bytes.starts_with(prefix))
    reject("unsupported persistent type format");
  Reader r{bytes.substr(prefix.size())};
  TypeBudget budget;
  LifetimeCodec lifetimes(bindings);
  auto result = readType(r, registry, budget, lifetimes, 0);
  lifetimes.finish(true);
  if (!r.bytes.empty() ||
      writePersistentType(result, registry, bindings) != bytes)
    reject("noncanonical persistent type recipe");
  return result;
}

std::string
writePersistentLifetimeContract(const LifetimeSignatureContract &contract,
                                const PersistentLifetimeBindings &bindings) {
  return writePersistentLifetimeContract(contract, PersistentTypeRegistry{},
                                         bindings);
}
std::string
writePersistentLifetimeContract(const LifetimeSignatureContract &contract,
                                const PersistentTypeRegistry &registry,
                                const PersistentLifetimeBindings &bindings) {
  LifetimeCodec lifetimes(bindings);
  if (!contract.valid() ||
      contract.parameters.size() != bindings.parameters.size() ||
      contract.bounds.size() > 256 || contract.typeBounds.size() > 256)
    reject("invalid or incomplete persistent lifetime contract");
  for (std::size_t index = 0; index < contract.parameters.size(); ++index)
    if (contract.parameters[index] !=
        Lifetime::parameter(bindings.parameters[index]))
      reject("lifetime contract bindings disagree with declaration");
  std::string out = "gti.lifetime-contract.1:";
  const auto origin = contract.origin;
  if (origin != LifetimeContractOrigin::Declared &&
      origin != LifetimeContractOrigin::Elided &&
      origin != LifetimeContractOrigin::BodyInferred)
    reject("invalid lifetime contract origin");
  append(out, origin == LifetimeContractOrigin::Declared ? "declared"
              : origin == LifetimeContractOrigin::Elided ? "elided"
                                                         : "body-inferred");
  append(out, "complete");
  append(out, std::to_string(contract.parameters.size()));
  append(out, std::to_string(bindings.typeParameters.size()));
  lifetimes.write(out, contract.receiver);
  std::set<std::string> bounds;
  for (LifetimeBound bound : contract.bounds) {
    std::string encoded;
    lifetimes.write(encoded, bound.longer);
    lifetimes.write(encoded, bound.shorter);
    bounds.insert(std::move(encoded));
  }
  append(out, std::to_string(bounds.size()));
  for (const auto &bound : bounds)
    append(out, bound);
  std::set<std::string> typeBounds;
  TypeBudget preflight;
  TypeBudget budget;
  for (const LifetimeTypeBound &bound : contract.typeBounds) {
    preflightType(bound.type, preflight, 0);
    std::string encoded;
    writeType(encoded, bound.type, registry, budget, lifetimes, 0);
    lifetimes.write(encoded, bound.region);
    typeBounds.insert(std::move(encoded));
  }
  lifetimes.finish(false);
  append(out, std::to_string(typeBounds.size()));
  for (const auto &bound : typeBounds)
    append(out, bound, byteLimit);
  return out;
}
LifetimeSignatureContract
readPersistentLifetimeContract(std::string_view bytes,
                               const PersistentLifetimeBindings &bindings) {
  return readPersistentLifetimeContract(bytes, PersistentTypeRegistry{},
                                        bindings);
}
LifetimeSignatureContract
readPersistentLifetimeContract(std::string_view bytes,
                               const PersistentTypeRegistry &registry,
                               const PersistentLifetimeBindings &bindings) {
  constexpr std::string_view prefix = "gti.lifetime-contract.1:";
  if (bytes.size() > byteLimit || !bytes.starts_with(prefix))
    reject("unsupported persistent lifetime contract format");
  LifetimeCodec lifetimes(bindings);
  Reader reader{bytes.substr(prefix.size())};
  LifetimeSignatureContract result;
  const auto origin = reader.field();
  if (origin != "declared" && origin != "elided" && origin != "body-inferred")
    reject("invalid lifetime contract origin");
  result.origin = origin == "declared" ? LifetimeContractOrigin::Declared
                  : origin == "elided" ? LifetimeContractOrigin::Elided
                                       : LifetimeContractOrigin::BodyInferred;
  if (reader.field() != "complete" ||
      count(reader) != bindings.parameters.size() ||
      count(reader) != bindings.typeParameters.size())
    reject("incomplete lifetime contract or binding count mismatch");
  result.complete = true;
  for (GenericParameterId parameter : bindings.parameters)
    result.parameters.push_back(Lifetime::parameter(parameter));
  result.receiver = lifetimes.read(reader);
  const auto boundCount = count(reader);
  for (std::size_t index = 0; index < boundCount; ++index) {
    Reader bound{reader.field()};
    const Lifetime longer = lifetimes.read(bound);
    const Lifetime shorter = lifetimes.read(bound);
    if (!bound.bytes.empty())
      reject("trailing lifetime bound data");
    result.bounds.push_back({longer, shorter});
  }
  const auto typeBoundCount = count(reader);
  TypeBudget budget;
  for (std::size_t index = 0; index < typeBoundCount; ++index) {
    Reader bound{reader.field(byteLimit)};
    auto type = readType(bound, registry, budget, lifetimes, 0);
    const Lifetime region = lifetimes.read(bound);
    if (!bound.bytes.empty())
      reject("trailing type-outlives bound data");
    result.typeBounds.push_back({std::move(type), region});
  }
  lifetimes.finish(true);
  if (!reader.bytes.empty() ||
      writePersistentLifetimeContract(result, registry, bindings) != bytes)
    reject("noncanonical persistent lifetime contract");
  return result;
}
std::string writePersistentNominalVariance(const NominalVariance &variance) {
  if (!variance.complete || variance.types.size() > 256 ||
      variance.lifetimes.size() > 256)
    reject("persistent variance must be complete and bounded");
  const auto sequence = [](std::span<const TypeVariance> entries) {
    std::string result;
    for (const auto value : entries) {
      switch (value) {
      case TypeVariance::Bivariant:
        result += 'b';
        break;
      case TypeVariance::Covariant:
        result += 'c';
        break;
      case TypeVariance::Contravariant:
        result += 'n';
        break;
      case TypeVariance::Invariant:
        result += 'i';
        break;
      default:
        reject("invalid persistent variance kind");
      }
    }
    return result;
  };
  std::string result = "gti.nominal-variance.1";
  append(result, sequence(variance.types));
  append(result, sequence(variance.lifetimes));
  return result;
}

NominalVariance readPersistentNominalVariance(std::string_view bytes,
                                              std::size_t typeSlots,
                                              std::size_t lifetimeSlots) {
  constexpr std::string_view magic = "gti.nominal-variance.1";
  if (!bytes.starts_with(magic) || bytes.size() > byteLimit ||
      typeSlots > 256 || lifetimeSlots > 256)
    reject("unsupported persistent variance recipe");
  Reader reader{bytes.substr(magic.size())};
  const auto sequence = [&](std::size_t count) {
    const auto field = reader.field(256);
    if (field.size() != count)
      reject("persistent variance does not match declaration arity");
    std::vector<TypeVariance> result;
    for (const auto value : field) {
      switch (value) {
      case 'b':
        result.push_back(TypeVariance::Bivariant);
        break;
      case 'c':
        result.push_back(TypeVariance::Covariant);
        break;
      case 'n':
        result.push_back(TypeVariance::Contravariant);
        break;
      case 'i':
        result.push_back(TypeVariance::Invariant);
        break;
      default:
        reject("invalid persistent variance kind");
      }
    }
    return result;
  };
  NominalVariance result{.types = sequence(typeSlots),
                         .lifetimes = sequence(lifetimeSlots),
                         .complete = true};
  if (!reader.bytes.empty() || writePersistentNominalVariance(result) != bytes)
    reject("noncanonical persistent variance recipe");
  return result;
}
} // namespace lang
