#include "gti/driver/manifest.h"
#include "gti/driver/registry.h"

#include <algorithm>
#include <map>
#include <set>

namespace lang::driver {
namespace {
constexpr std::size_t archiveLimit = 256U * 1024U * 1024U;
constexpr std::size_t fileLimit = 64U * 1024U * 1024U;
constexpr std::string_view magic = "GTIPKG1\n";
[[noreturn]] void rejected(std::string detail) {
  throw RegistryError("GTI-B1718", "Rejected package archive: " + detail + ".");
}
std::string folded(std::string text) {
  for (auto &c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return text;
}
void validateEntries(const std::vector<PackageArchiveEntry> &entries) {
  if (entries.empty() || entries.size() > 10000) {
    rejected("invalid file count");
  }
  std::map<std::string, std::string> spellings;
  std::set<std::string> files;
  std::size_t bytes = magic.size() + 4;
  bool manifest = false;
  for (const auto &entry : entries) {
    const std::string &path = entry.path;
    if (path.empty() || path.size() > 4096 || path.front() == '/' ||
        path.back() == '/' || entry.contents.size() > fileLimit) {
      rejected("invalid path or oversized file");
    }
    std::string prefix;
    std::size_t start = 0;
    for (;;) {
      const auto slash = path.find('/', start);
      const auto component =
          path.substr(start, slash == path.npos ? slash : slash - start);
      if (component.empty() || component == "." || component == ".." ||
          component.back() == '.' || component.back() == ' ' ||
          !std::all_of(component.begin(), component.end(), [](unsigned char c) {
            return c >= 0x20 && c <= 0x7e && c != '\\' && c != ':' &&
                   c != '*' && c != '?' && c != '"' && c != '<' && c != '>' &&
                   c != '|';
          })) {
        rejected("nonportable path");
      }
      const auto key = folded(component);
      const auto stem = key.substr(0, key.find('.'));
      if (key == ".git" || key == ".env" || key.starts_with(".env.") ||
          (start == 0 && (key == "build" || key == "toolchain" ||
                          key == "node_modules" || key == "gti.lock")) ||
          stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
          (stem.size() == 4 &&
           (stem.starts_with("com") || stem.starts_with("lpt")) &&
           stem.back() >= '1' && stem.back() <= '9')) {
        rejected("reserved, generated or credential-bearing path");
      }
      if (!prefix.empty()) {
        prefix += '/';
      }
      prefix += component;
      const auto foldedPrefix = folded(prefix);
      const auto [found, inserted] = spellings.emplace(foldedPrefix, prefix);
      if ((!inserted && found->second != prefix) ||
          (slash != path.npos && files.contains(foldedPrefix))) {
        rejected("path collision");
      }
      if (slash == path.npos) {
        if (!files.insert(foldedPrefix).second) {
          rejected("duplicate file");
        }
        // Reject a file occupying an earlier file's parent directory.
        const auto child = spellings.lower_bound(foldedPrefix + '/');
        if (child != spellings.end() &&
            child->first.starts_with(foldedPrefix + '/')) {
          rejected("file/directory collision");
        }
        break;
      }
      start = slash + 1;
    }
    manifest |= path == "gti.toml";
    if (path == "gti.toml" && entry.executable) {
      rejected("executable manifest");
    }
    bytes += 13 + path.size() + entry.contents.size();
    if (bytes > archiveLimit) {
      rejected("archive exceeds 256 MiB");
    }
  }
  if (!manifest) {
    rejected("missing root gti.toml");
  }
}
void integer(std::string &output, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) {
    output += static_cast<char>((value >> (8U * i)) & 255U);
  }
}
std::uint64_t integer(std::string_view &input, unsigned width) {
  if (input.size() < width) {
    rejected("truncated record");
  }
  std::uint64_t value = 0;
  for (unsigned i = 0; i < width; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(input[i]))
             << (8U * i);
  }
  input.remove_prefix(width);
  return value;
}
} // namespace

std::string encodePackageArchive(std::vector<PackageArchiveEntry> entries) {
  validateEntries(entries);
  std::sort(entries.begin(), entries.end(),
            [](const auto &a, const auto &b) { return a.path < b.path; });
  std::string bytes(magic);
  integer(bytes, entries.size(), 4);
  for (const auto &entry : entries) {
    integer(bytes, entry.path.size(), 4);
    integer(bytes, entry.contents.size(), 8);
    integer(bytes, entry.executable, 1);
    bytes += entry.path;
    bytes += entry.contents;
  }
  return bytes;
}
std::vector<PackageArchiveEntry> decodePackageArchive(std::string_view bytes) {
  if (bytes.size() > archiveLimit || !bytes.starts_with(magic)) {
    rejected("unknown format or oversized archive");
  }
  bytes.remove_prefix(magic.size());
  const auto count = integer(bytes, 4);
  if (count == 0 || count > 10000) {
    rejected("invalid file count");
  }
  std::vector<PackageArchiveEntry> entries;
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto pathSize = integer(bytes, 4), dataSize = integer(bytes, 8),
               executable = integer(bytes, 1);
    if (pathSize > 4096 || dataSize > fileLimit || executable > 1 ||
        pathSize > bytes.size() || dataSize > bytes.size() - pathSize) {
      rejected("invalid or truncated file");
    }
    entries.push_back({std::string(bytes.substr(0, pathSize)),
                       std::string(bytes.substr(pathSize, dataSize)),
                       executable != 0});
    bytes.remove_prefix(pathSize + dataSize);
  }
  if (!bytes.empty()) {
    rejected("trailing bytes");
  }
  validateEntries(entries);
  return entries;
}
} // namespace lang::driver
