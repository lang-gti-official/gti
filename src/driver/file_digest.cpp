#include "file_digest.h"
#include <array>
#include <fstream>
#include <llvm/ADT/StringExtras.h>
#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#else
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
  CC_SHA256_CTX hash;
  CC_SHA256_Init(&hash);
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
#if defined(__APPLE__)
    CC_SHA256_Update(&hash, buffer.data(), static_cast<CC_LONG>(count));
#else
    hash.update(llvm::StringRef(buffer.data(), count));
#endif
  }
  if (input.bad() || !input.eof() || bytes != expectedSize) {
    errorMessage = "cannot finish reading '" + path.string() + "'";
    return std::nullopt;
  }
#if defined(__APPLE__)
  std::array<uint8_t, CC_SHA256_DIGEST_LENGTH> result;
  CC_SHA256_Final(result.data(), &hash);
#else
  const auto result = hash.final();
#endif
  return FileDigest{llvm::toHex(result, true), bytes};
}

} // namespace lang::driver
