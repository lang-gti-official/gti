#include "file_digest.h"
#include <array>
#include <fstream>
#include <string_view>
#if defined(__APPLE__)
#include "sha256.h"
#else
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/SHA256.h>
#endif

namespace lang::driver {

std::optional<FileDigest> digestFile(const std::filesystem::path &path,
                                     std::string &errorMessage) {
  std::error_code error;
  const auto expectedSize = std::filesystem::file_size(path, error);
  if (error) {
    errorMessage = "cannot inspect '" + path.string() + "': " + error.message();
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    errorMessage = "cannot read '" + path.string() + "'";
    return std::nullopt;
  }
#if defined(__APPLE__)
  Sha256 hash;
#else
  llvm::SHA256 hash;
#endif
  std::uintmax_t bytes = 0;
  std::array<char, 64 * 1024> buffer;
  while (input) {
    input.read(buffer.data(), buffer.size());
    const auto count = static_cast<size_t>(input.gcount());
    if (count == 0)
      break;
    bytes += count;
    hash.update(std::string_view(buffer.data(), count));
  }
  if (input.bad() || !input.eof() || bytes != expectedSize) {
    errorMessage = "cannot finish reading '" + path.string() + "'";
    return std::nullopt;
  }
#if defined(__APPLE__)
  return FileDigest{hash.finishHex(), bytes};
#else
  return FileDigest{llvm::toHex(hash.final(), true), bytes};
#endif
}

} // namespace lang::driver
