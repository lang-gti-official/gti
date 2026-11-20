#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace lang::driver {

struct FileDigest {
  std::string hash;
  std::uintmax_t size = 0;
  bool operator==(const FileDigest &) const = default;
};

// The lowercase SHA-256 and byte count of the exact file contents read.
std::optional<FileDigest> digestFile(const std::filesystem::path &path,
                                     std::string &errorMessage);

} // namespace lang::driver
