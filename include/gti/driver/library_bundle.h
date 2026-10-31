#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lang::driver {

// Development container: interface metadata is opaque to this codec. Identities
// are supplied by the caller, not inferred from an object file. No filesystem
// access or archive extraction. Profile importers perform additional
// validation.
struct LibraryBundleIdentity {
  std::string toolchain; // SHA-256 of the complete compiler/toolchain inputs.
  std::string target;    // Canonical target triple.
  // SHA-256 of resolved configuration, layout and backend.
  std::string configuration;
  std::string runtime; // SHA-256 of runtime ABI and implementation inputs.
  std::string library; // SHA-256 of package + target logical identity.
  std::string sources; // SHA-256 of complete library input graph.
  friend bool operator==(const LibraryBundleIdentity &,
                         const LibraryBundleIdentity &) = default;
};

struct LibraryBundleDependency {
  std::string library; // Logical identity SHA-256, not a dependency alias/path.
  std::string bundle;  // SHA-256 of complete dependency bundle bytes.
  friend bool operator==(const LibraryBundleDependency &,
                         const LibraryBundleDependency &) = default;
};

struct LibraryBundleMember {
  // Portable lowercase basename ending in .o. Names are opaque keys, never
  // paths.
  std::string name;
  std::string bytes;
  friend bool operator==(const LibraryBundleMember &,
                         const LibraryBundleMember &) = default;
};

struct LibraryBundle {
  LibraryBundleIdentity identity;
  std::vector<LibraryBundleDependency> dependencies;
  std::vector<LibraryBundleMember> objects;
  std::string interfaceMetadata; // Versioned compiler metadata; opaque to the
                                 // container.
  friend bool operator==(const LibraryBundle &,
                         const LibraryBundle &) = default;
};

enum class LibraryBundleErrorKind {
  InvalidFormat,
  UnsupportedVersion,
  ResourceLimit,
  Integrity,
  Incompatible
};
class LibraryBundleError final : public std::runtime_error {
public:
  LibraryBundleError(LibraryBundleErrorKind kind, std::string message);
  [[nodiscard]] LibraryBundleErrorKind kind() const { return errorKind; }

private:
  LibraryBundleErrorKind errorKind;
};

inline constexpr std::uint32_t libraryBundleFormatVersion = 2;
inline constexpr std::size_t libraryBundleByteLimit = 256U * 1024U * 1024U;
inline constexpr std::size_t libraryBundleObjectLimit = 64U * 1024U * 1024U;
inline constexpr std::size_t libraryBundleEntryLimit = 4096;

// Canonicalizes dependency/member order. Rejects duplicates, invalid
// identities, unsafe names and limits before allocating the output. Objects
// remain opaque.
[[nodiscard]] std::string encodeLibraryBundle(const LibraryBundle &bundle);
// Checks the complete digest, envelope, canonical order and limits before
// returning owned values. The returned envelope/member inventory is inspection.
[[nodiscard]] LibraryBundle decodeLibraryBundle(std::string_view bytes);
// Exact conservative compatibility, including the selected dependency set.
// Call only with independently resolved expected identities, never artifact-
// supplied expectations. This does not establish semantic consumability.
void requireLibraryBundleCompatibility(
    const LibraryBundle &bundle, const LibraryBundleIdentity &expected,
    const std::vector<LibraryBundleDependency> &dependencies);

} // namespace lang::driver
