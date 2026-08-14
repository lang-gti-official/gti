#include "gti/target.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/TargetParser/Triple.h"

#include <bit>
#include <utility>

namespace lang {

std::string_view executionProfileName(ExecutionProfile profile) {
  switch (profile) {
  case ExecutionProfile::SingleThreaded:
    return "single-threaded";
  case ExecutionProfile::Concurrent:
    return "concurrent";
  }
  return "single-threaded";
}

std::optional<ExecutionProfile> parseExecutionProfile(std::string_view text) {
  if (text == "single-threaded") {
    return ExecutionProfile::SingleThreaded;
  }
  if (text == "concurrent") {
    return ExecutionProfile::Concurrent;
  }
  return std::nullopt;
}

std::string_view targetPropertyName(TargetProperty property) {
  switch (property) {
  case TargetProperty::Os:
    return "os";
  case TargetProperty::Vendor:
    return "vendor";
  case TargetProperty::Arch:
    return "arch";
  }
  return {};
}

std::optional<TargetProperty> parseTargetProperty(std::string_view text) {
  if (text == "os") {
    return TargetProperty::Os;
  }
  if (text == "vendor") {
    return TargetProperty::Vendor;
  }
  if (text == "arch") {
    return TargetProperty::Arch;
  }
  return std::nullopt;
}

std::string_view targetEndiannessName(TargetEndianness endianness) {
  switch (endianness) {
  case TargetEndianness::Little:
    return "little";
  case TargetEndianness::Big:
    return "big";
  }
  return {};
}

std::string_view targetScalarKindName(TargetScalarKind kind) {
  switch (kind) {
  case TargetScalarKind::Bool:
    return "bool";
  case TargetScalarKind::Char:
    return "char";
  case TargetScalarKind::Int8:
    return "int8_t";
  case TargetScalarKind::Int16:
    return "int16_t";
  case TargetScalarKind::Int32:
    return "int32_t";
  case TargetScalarKind::Int64:
    return "int64_t";
  case TargetScalarKind::UInt8:
    return "uint8_t";
  case TargetScalarKind::UInt16:
    return "uint16_t";
  case TargetScalarKind::UInt32:
    return "uint32_t";
  case TargetScalarKind::UInt64:
    return "uint64_t";
  case TargetScalarKind::Float32:
    return "float";
  case TargetScalarKind::Float64:
    return "double";
  case TargetScalarKind::Pointer:
    return "pointer";
  case TargetScalarKind::Count:
    break;
  }
  return {};
}

std::string_view TargetInfo::value(TargetProperty property) const {
  switch (property) {
  case TargetProperty::Os:
    return os;
  case TargetProperty::Vendor:
    return vendor;
  case TargetProperty::Arch:
    return arch;
  }
  return {};
}

bool TargetInfo::supported() const {
  const bool supportedOs = os == "macos" || os == "linux" || os == "windows";
  const bool supportedVendor =
      vendor == "apple" || vendor == "pc" || vendor == "unknown";
  const bool supportedArch = arch == "arm64" || arch == "x86_64";
  return supportedOs && supportedVendor && supportedArch &&
         dataLayout.supported();
}

TargetInfo TargetInfo::host() {
  TargetInfo target;
  bool supportedOs = true;
  bool supportedArch = true;

#if defined(_WIN32)
  target.os = "windows";
  target.vendor = "pc";
#elif defined(__APPLE__)
  target.os = "macos";
  target.vendor = "apple";
#elif defined(__linux__)
  target.os = "linux";
  target.vendor = "unknown";
#else
  target.os = "unknown";
  target.vendor = "unknown";
  supportedOs = false;
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
  target.arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  target.arch = "x86_64";
#else
  target.arch = "unknown";
  supportedArch = false;
#endif

  if (!supportedOs || !supportedArch || sizeof(void *) != 8 ||
      std::endian::native != std::endian::little) {
    target.dataLayout = TargetDataLayout::unsupported();
  }
  return target;
}

std::string_view targetTripleErrorMessage(TargetTripleError error) {
  switch (error) {
  case TargetTripleError::None:
    return {};
  case TargetTripleError::Malformed:
    return "The target triple is malformed or has an unknown architecture.";
  case TargetTripleError::UnsupportedArchitecture:
    return "GTI supports only arm64 and x86_64 target architectures.";
  case TargetTripleError::UnsupportedEndianness:
    return "GTI currently supports only little-endian targets.";
  case TargetTripleError::UnsupportedOperatingSystem:
    return "GTI supports only macOS, Linux, and Windows target operating "
           "systems.";
  }
  return {};
}

TargetTripleParseResult parseTargetTripleResult(std::string_view text) {
  if (text.empty()) {
    return {.error = TargetTripleError::Malformed};
  }

  const llvm::Triple triple(
      llvm::Triple::normalize(llvm::StringRef(text.data(), text.size())));
  if (triple.getArch() == llvm::Triple::UnknownArch) {
    return {.error = TargetTripleError::Malformed};
  }

  // Triple supplies parsing and normalization only; the accepted vocabulary
  // and layout commitments below remain GTI decisions.
  TargetInfo target;
  switch (triple.getArch()) {
  case llvm::Triple::aarch64:
    target.arch = "arm64";
    break;
  case llvm::Triple::x86_64:
    target.arch = "x86_64";
    break;
  case llvm::Triple::aarch64_be:
    return {.error = TargetTripleError::UnsupportedEndianness};
  default:
    return {.error = TargetTripleError::UnsupportedArchitecture};
  }
  if (!triple.isArch64Bit()) {
    return {.error = TargetTripleError::UnsupportedArchitecture};
  }
  if (!triple.isLittleEndian()) {
    return {.error = TargetTripleError::UnsupportedEndianness};
  }

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
  default:
    return {.error = TargetTripleError::UnsupportedOperatingSystem};
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
  if (!target.supported()) {
    return {.error = TargetTripleError::Malformed};
  }
  return {.target = std::move(target)};
}

std::optional<TargetInfo> parseTargetTriple(std::string_view text) {
  return parseTargetTripleResult(text).target;
}

bool targetTripleParsingAvailable() { return true; }

} // namespace lang
