#include "../src/driver/sha256.h"
#include "gti/driver/library_bundle.h"
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
using namespace lang::driver;
namespace {
void expect(bool value, const char *why) {
  if (!value) {
    std::cerr << why << '\n';
    std::exit(1);
  }
}
void rejects(const std::function<void()> &f, LibraryBundleErrorKind kind) {
  try {
    f();
  } catch (const LibraryBundleError &e) {
    expect(e.kind() == kind, e.what());
    return;
  }
  expect(false, "accepted invalid bundle");
}
std::string hash(std::string_view bytes) {
  Sha256 h;
  h.update(bytes);
  return h.finishHex();
}
void seal(std::string &bytes) {
  bytes.resize(bytes.size() - 64);
  bytes += hash(bytes);
}
void put(std::string &s, std::size_t offset, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    s[offset + i] = static_cast<char>((value >> (8 * i)) & 255);
}
} // namespace
int main() {
  using K = LibraryBundleErrorKind;
  expect(hash("abc") ==
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "SHA-256 known vector");
  LibraryBundle b{
      {hash("compiler"), "aarch64-apple-darwin", hash("config"),
       hash("runtime"), hash("package:target"), hash("sources")},
      {{hash("dep-b"), hash("bundle-b")}, {hash("dep-a"), hash("bundle-a")}},
      {{"z.o", std::string("\0\xff\n", 3)}, {"a.o", "object"}}};
  auto bytes = encodeLibraryBundle(b);
  expect(bytes.substr(0, 8) == "GTILIB\r\n" && bytes[8] == 2 && bytes[12] == 1,
         "documented magic and version framing");
  expect(hash(bytes) ==
             "40665626934f08e2a7f617b6fb9b60fbc6f711ce64f0ec83733003d4fc80457a",
         "independent wire-format golden digest");
  auto decoded = decodeLibraryBundle(bytes);
  expect(decoded.identity == b.identity &&
             decoded.objects.front().name == "a.o" &&
             decoded.objects.back().bytes == b.objects.front().bytes,
         "round trip binary data");
  expect(encodeLibraryBundle(decoded) == bytes, "canonical round trip");
  std::reverse(b.objects.begin(), b.objects.end());
  std::reverse(b.dependencies.begin(), b.dependencies.end());
  expect(encodeLibraryBundle(b) == bytes, "input order independence");
  requireLibraryBundleCompatibility(decoded, b.identity, b.dependencies);
  for (int field = 0; field < 6; ++field) {
    auto id = b.identity;
    std::string *fields[] = {&id.toolchain, &id.target,  &id.configuration,
                             &id.runtime,   &id.library, &id.sources};
    *fields[field] = field == 1 ? "x86_64-unknown-linux-gnu" : hash("changed");
    rejects(
        [&] { requireLibraryBundleCompatibility(decoded, id, b.dependencies); },
        K::Incompatible);
  }
  auto deps = b.dependencies;
  deps.front().bundle = hash("changed");
  rejects([&] { requireLibraryBundleCompatibility(decoded, b.identity, deps); },
          K::Incompatible);
  rejects([&] { requireLibraryBundleCompatibility(decoded, b.identity, {}); },
          K::Incompatible);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    try {
      (void)decodeLibraryBundle(std::string_view(bytes).substr(0, i));
    } catch (const LibraryBundleError &) {
      continue;
    }
    expect(false, "truncation accepted");
  }
  for (std::size_t i = 16; i < bytes.size(); ++i) {
    auto bad = bytes;
    bad[i] ^= 1;
    rejects([&] { (void)decodeLibraryBundle(bad); }, K::Integrity);
  }
  for (auto offset : {8U, 12U}) {
    auto bad = bytes;
    put(bad, offset, 3);
    seal(bad);
    rejects([&] { (void)decodeLibraryBundle(bad); }, K::UnsupportedVersion);
  }
  auto bad = bytes;
  bad[0] = 'X';
  rejects([&] { (void)decodeLibraryBundle(bad); }, K::InvalidFormat);
  bad = bytes;
  put(bad, 16, 0xffffffffU);
  seal(bad);
  rejects([&] { (void)decodeLibraryBundle(bad); }, K::ResourceLimit);
  // Header, six identity fields and interface metadata precede dependencies.
  std::size_t depOffset = 16 + 6 * 4 + 5 * 64 + b.identity.target.size() + 4 +
                          b.interfaceMetadata.size();
  bad = bytes;
  put(bad, depOffset, 4097);
  seal(bad);
  rejects([&] { (void)decodeLibraryBundle(bad); }, K::ResourceLimit);
  bad = bytes;
  bad.insert(bad.size() - 64, "x");
  seal(bad);
  rejects([&] { (void)decodeLibraryBundle(bad); }, K::InvalidFormat);
  // Valid checksum cannot make noncanonical or duplicate dependency rows valid.
  bad = bytes;
  bad.replace(depOffset + 4 + 136, 136, bad.substr(depOffset + 4, 136));
  seal(bad);
  rejects([&] { (void)decodeLibraryBundle(bad); }, K::InvalidFormat);
  // Check semantic validation even when the checksum is recomputed.
  bad = bytes;
  bad.replace(bad.find("a.o"), 3, "../");
  seal(bad);
  rejects([&] { (void)decodeLibraryBundle(bad); }, K::InvalidFormat);
  bad = bytes;
  bad.replace(bad.find("z.o"), 3, "a.o");
  seal(bad);
  rejects([&] { (void)decodeLibraryBundle(bad); }, K::InvalidFormat);
  bad = bytes;
  bad.replace(bad.find("a.o"), 3, "z.o");
  bad.replace(bad.rfind("z.o"), 3, "a.o");
  seal(bad);
  rejects([&] { (void)decodeLibraryBundle(bad); }, K::InvalidFormat);
  bad = bytes;
  put(bad, depOffset + 4 + 2 * 136, 0xffffffffU);
  seal(bad);
  rejects([&] { (void)decodeLibraryBundle(bad); }, K::ResourceLimit);
  auto metadataBundle = b;
  metadataBundle.interfaceMetadata = "gti.scalar-interface.test\n";
  expect(decodeLibraryBundle(encodeLibraryBundle(metadataBundle))
                 .interfaceMetadata == metadataBundle.interfaceMetadata,
         "nonempty interface metadata round trip");
  metadataBundle.interfaceMetadata.resize(1024U * 1024U + 1);
  rejects([&] { (void)encodeLibraryBundle(metadataBundle); }, K::ResourceLimit);
  for (const auto *name : {"../x.o", "/x.o", "x/y.o", "x\\y.o", "X.o", "a..o",
                           "con.o", "com1.o", "a.o.", "a:", ".o"}) {
    auto invalid = b;
    invalid.objects.front().name = name;
    rejects([&] { (void)encodeLibraryBundle(invalid); }, K::InvalidFormat);
  }
  auto invalid = b;
  invalid.objects.push_back(invalid.objects.front());
  rejects([&] { (void)encodeLibraryBundle(invalid); }, K::InvalidFormat);
  invalid = b;
  invalid.dependencies.push_back(invalid.dependencies.front());
  rejects([&] { (void)encodeLibraryBundle(invalid); }, K::InvalidFormat);
  invalid = b;
  invalid.dependencies.front().library = b.identity.library;
  rejects([&] { (void)encodeLibraryBundle(invalid); }, K::InvalidFormat);
  invalid = b;
  invalid.identity.toolchain = "unknown";
  rejects([&] { (void)encodeLibraryBundle(invalid); }, K::InvalidFormat);
  invalid = b;
  invalid.objects.clear();
  rejects([&] { (void)encodeLibraryBundle(invalid); }, K::ResourceLimit);
  invalid = b;
  invalid.objects.front().bytes.clear();
  rejects([&] { (void)encodeLibraryBundle(invalid); }, K::ResourceLimit);
  invalid = b;
  invalid.objects.resize(libraryBundleEntryLimit + 1);
  rejects([&] { (void)encodeLibraryBundle(invalid); }, K::ResourceLimit);
  invalid = b;
  invalid.dependencies.resize(libraryBundleEntryLimit + 1);
  rejects([&] { (void)encodeLibraryBundle(invalid); }, K::ResourceLimit);
  invalid = b;
  invalid.objects.front().bytes.resize(libraryBundleObjectLimit + 1);
  rejects([&] { (void)encodeLibraryBundle(invalid); }, K::ResourceLimit);
  invalid = {};
  {
    std::string oversized(libraryBundleByteLimit + 1, 'x');
    rejects([&] { (void)decodeLibraryBundle(oversized); }, K::ResourceLimit);
  }
  std::cout << "library bundle round-trip, determinism, compatibility and "
               "rejection checks passed\n";
}
