#include "gti/target.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/TargetParser/Triple.h"

namespace lang {

std::optional<TargetInfo> parseTargetTriple(std::string_view text) {
  const llvm::Triple triple(
      llvm::Triple::normalize(llvm::StringRef(text.data(), text.size())));

  // Triple supplies parsing and normalization only; the accepted vocabulary
  // and the layout commitments below remain GTI decisions.
  TargetInfo target;
  switch (triple.getArch()) {
  case llvm::Triple::aarch64:
    target.arch = "arm64";
    break;
  case llvm::Triple::x86_64:
    target.arch = "x86_64";
    break;
  default:
    // Unknown architectures are malformed input; known but non-64-bit or
    // big-endian architectures are outside GTI's current layout support.
    return std::nullopt;
  }
  if (!triple.isArch64Bit() || !triple.isLittleEndian()) {
    return std::nullopt;
  }
  target.pointerWidth = 64;
  target.littleEndian = true;

  switch (triple.getOS()) {
  case llvm::Triple::Darwin:
  case llvm::Triple::MacOSX:
    target.os = "macos";
    break;
  case llvm::Triple::Linux:
    target.os = "linux";
    break;
  case llvm::Triple::Win32:
    target.os = "windows";
    break;
  case llvm::Triple::UnknownOS:
    target.os = "unknown";
    break;
  default:
    return std::nullopt;
  }

  switch (triple.getVendor()) {
  case llvm::Triple::Apple:
    target.vendor = "apple";
    break;
  case llvm::Triple::PC:
    target.vendor = "pc";
    break;
  default:
    target.vendor = "unknown";
    break;
  }
  return target;
}

bool targetTripleParsingAvailable() { return true; }

} // namespace lang
