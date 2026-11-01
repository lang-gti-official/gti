#include "gti/driver/library_bundle.h"
#include "sha256.h"

#include <algorithm>
#include <array>
#include <utility>

namespace lang::driver {
LibraryBundleError::LibraryBundleError(LibraryBundleErrorKind kind,
                                       std::string message)
    : std::runtime_error(std::move(message)), errorKind(kind) {}
namespace {
constexpr std::string_view magic = "GTILIB\r\n";
using Kind = LibraryBundleErrorKind;
[[noreturn]] void reject(Kind kind, const char *message) {
  throw LibraryBundleError(kind, message);
}
std::string digest(std::string_view bytes) {
  Sha256 hash;
  hash.update(bytes);
  return hash.finishHex();
}
bool hashIdentity(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}
bool portable(std::string_view value) {
  return !value.empty() && value.size() <= 255 &&
         std::all_of(value.begin(), value.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.';
         });
}
auto fields(const LibraryBundleIdentity &id) {
  return std::array<std::string_view, 6>{id.toolchain,     id.target,
                                         id.configuration, id.runtime,
                                         id.library,       id.sources};
}
void validateIdentity(const LibraryBundleIdentity &id) {
  const auto values = fields(id);
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i == 1 ? (!portable(values[i]) || values[i].find('-') == values[i].npos)
               : !hashIdentity(values[i]))
      reject(Kind::InvalidFormat, "invalid library compatibility identity");
  }
}
void validateDependencies(const std::vector<LibraryBundleDependency> &deps,
                          std::string_view owner) {
  if (deps.size() > libraryBundleEntryLimit)
    reject(Kind::ResourceLimit, "too many library dependencies");
  std::vector<std::string_view> keys;
  for (const auto &dep : deps) {
    if (!hashIdentity(dep.library) || !hashIdentity(dep.bundle) ||
        dep.library == owner)
      reject(Kind::InvalidFormat, "invalid or self library dependency");
    keys.push_back(dep.library);
  }
  std::sort(keys.begin(), keys.end());
  if (std::adjacent_find(keys.begin(), keys.end()) != keys.end())
    reject(Kind::InvalidFormat, "duplicate library dependency");
}
std::size_t validate(const LibraryBundle &bundle) {
  validateIdentity(bundle.identity);
  validateDependencies(bundle.dependencies, bundle.identity.library);
  if (bundle.objects.empty() || bundle.objects.size() > libraryBundleEntryLimit)
    reject(Kind::ResourceLimit, "invalid library object count");
  if (bundle.interfaceMetadata.size() > 1024U * 1024U)
    reject(Kind::ResourceLimit, "library interface exceeds byte limit");
  std::size_t size =
      4 + bundle.interfaceMetadata.size() + magic.size() + 4 + 4 + 4 + 4 + 64;
  for (const auto field : fields(bundle.identity))
    size += 4 + field.size();
  size += bundle.dependencies.size() * (4 + 64 + 4 + 64);
  std::vector<std::string_view> names;
  for (const auto &member : bundle.objects) {
    // Deliberately stricter than filesystem names; no directories, dot
    // segments, platform device basenames, aliases, absolute paths or
    // extraction semantics.
    if (!portable(member.name) || !member.name.ends_with(".o") ||
        member.name.front() == '.' ||
        member.name.find("..") != member.name.npos ||
        member.name.find('.') != member.name.size() - 2)
      reject(Kind::InvalidFormat, "invalid library object name");
    const auto stem = member.name.substr(0, member.name.size() - 2);
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
        (stem.size() == 4 &&
         (stem.starts_with("com") || stem.starts_with("lpt")) &&
         stem[3] >= '0' && stem[3] <= '9'))
      reject(Kind::InvalidFormat, "reserved library object name");
    if (member.bytes.empty() || member.bytes.size() > libraryBundleObjectLimit)
      reject(Kind::ResourceLimit, "invalid library object size");
    const auto addition = 4 + member.name.size() + 4 + member.bytes.size();
    if (addition > libraryBundleByteLimit - size)
      reject(Kind::ResourceLimit, "library bundle exceeds byte limit");
    size += addition;
    names.push_back(member.name);
  }
  std::sort(names.begin(), names.end());
  if (std::adjacent_find(names.begin(), names.end()) != names.end())
    reject(Kind::InvalidFormat, "duplicate library object name");
  return size;
}
void number(std::string &out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    out.push_back(static_cast<char>((value >> shift) & 255U));
}
void blob(std::string &out, std::string_view value) {
  number(out, static_cast<std::uint32_t>(value.size()));
  out.append(value);
}
struct Reader {
  std::string_view remaining;
  std::string_view take(std::size_t count) {
    if (count > remaining.size())
      reject(Kind::InvalidFormat, "truncated library bundle");
    auto result = remaining.substr(0, count);
    remaining.remove_prefix(count);
    return result;
  }
  std::uint32_t number() {
    auto bytes = take(4);
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
      value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i]))
               << (8 * i);
    return value;
  }
  std::string blob(std::size_t limit) {
    auto length = number();
    if (length > limit)
      reject(Kind::ResourceLimit, "library field exceeds limit");
    return std::string(take(length));
  }
  std::uint32_t count() {
    auto value = number();
    if (value > libraryBundleEntryLimit)
      reject(Kind::ResourceLimit, "library entry count exceeds limit");
    return value;
  }
};
} // namespace
std::string encodeLibraryBundle(const LibraryBundle &bundle) {
  const auto size = validate(bundle);
  std::string result;
  result.reserve(size);
  result.append(magic);
  number(result, libraryBundleFormatVersion);
  number(result, 1); // Envelope schema, independent of container framing.
  for (auto field : fields(bundle.identity))
    blob(result, field);
  blob(result, bundle.interfaceMetadata);
  auto deps = bundle.dependencies;
  std::sort(deps.begin(), deps.end(),
            [](const auto &a, const auto &b) { return a.library < b.library; });
  number(result, static_cast<std::uint32_t>(deps.size()));
  for (const auto &dep : deps) {
    blob(result, dep.library);
    blob(result, dep.bundle);
  }
  std::vector<const LibraryBundleMember *> objects;
  for (const auto &member : bundle.objects)
    objects.push_back(&member);
  std::sort(objects.begin(), objects.end(),
            [](auto a, auto b) { return a->name < b->name; });
  number(result, static_cast<std::uint32_t>(objects.size()));
  for (const auto *member : objects) {
    blob(result, member->name);
    blob(result, member->bytes);
  }
  result += digest(result);
  return result;
}
LibraryBundle decodeLibraryBundle(std::string_view bytes) {
  if (bytes.size() > libraryBundleByteLimit)
    reject(Kind::ResourceLimit, "library bundle exceeds byte limit");
  Reader reader{bytes};
  if (reader.take(magic.size()) != magic)
    reject(Kind::InvalidFormat, "invalid library bundle magic");
  if (reader.number() != libraryBundleFormatVersion || reader.number() != 1)
    reject(Kind::UnsupportedVersion,
           "unsupported library container or envelope version");
  if (reader.remaining.size() < 64)
    reject(Kind::InvalidFormat, "truncated library digest");
  if (digest(bytes.substr(0, bytes.size() - 64)) !=
      bytes.substr(bytes.size() - 64))
    reject(Kind::Integrity, "library bundle checksum mismatch");
  reader.remaining.remove_suffix(64);
  LibraryBundle result;
  auto &id = result.identity;
  id.toolchain = reader.blob(64);
  id.target = reader.blob(255);
  id.configuration = reader.blob(64);
  id.runtime = reader.blob(64);
  id.library = reader.blob(64);
  id.sources = reader.blob(64);
  validateIdentity(id);
  result.interfaceMetadata = reader.blob(1024U * 1024U);
  const auto depCount = reader.count();
  for (std::uint32_t i = 0; i < depCount; ++i) {
    LibraryBundleDependency dep{reader.blob(64), reader.blob(64)};
    if (!result.dependencies.empty() &&
        result.dependencies.back().library >= dep.library)
      reject(Kind::InvalidFormat, "noncanonical library dependency order");
    result.dependencies.push_back(std::move(dep));
  }
  const auto objectCount = reader.count();
  for (std::uint32_t i = 0; i < objectCount; ++i) {
    LibraryBundleMember member{reader.blob(255),
                               reader.blob(libraryBundleObjectLimit)};
    if (!result.objects.empty() && result.objects.back().name >= member.name)
      reject(Kind::InvalidFormat, "noncanonical library object order");
    result.objects.push_back(std::move(member));
  }
  if (!reader.remaining.empty())
    reject(Kind::InvalidFormat, "trailing library bundle data");
  validate(result);
  return result;
}
void requireLibraryBundleCompatibility(
    const LibraryBundle &bundle, const LibraryBundleIdentity &expected,
    const std::vector<LibraryBundleDependency> &dependencies) {
  validate(bundle);
  validateIdentity(expected);
  validateDependencies(dependencies, expected.library);
  auto actual = bundle.dependencies;
  auto wanted = dependencies;
  auto less = [](const auto &a, const auto &b) {
    return a.library < b.library;
  };
  std::sort(actual.begin(), actual.end(), less);
  std::sort(wanted.begin(), wanted.end(), less);
  constexpr std::array names{"toolchain", "target",  "configuration",
                             "runtime",   "library", "sources"};
  const auto actualFields = fields(bundle.identity);
  const auto expectedFields = fields(expected);
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (actualFields[i] != expectedFields[i])
      throw LibraryBundleError(Kind::Incompatible,
                               std::string("incompatible library ") + names[i] +
                                   " identity");
  }
  if (actual != wanted)
    reject(Kind::Incompatible, "incompatible library dependency graph");
}
} // namespace lang::driver
