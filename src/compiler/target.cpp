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

std::string_view nativeCScalarKindName(NativeCScalarKind kind) {
  switch (kind) {
  case NativeCScalarKind::Bool:
    return "c_bool";
  case NativeCScalarKind::Char:
    return "c_char";
  case NativeCScalarKind::SignedChar:
    return "c_schar";
  case NativeCScalarKind::UnsignedChar:
    return "c_uchar";
  case NativeCScalarKind::Short:
    return "c_short";
  case NativeCScalarKind::UnsignedShort:
    return "c_ushort";
  case NativeCScalarKind::Int:
    return "c_int";
  case NativeCScalarKind::UnsignedInt:
    return "c_uint";
  case NativeCScalarKind::Long:
    return "c_long";
  case NativeCScalarKind::UnsignedLong:
    return "c_ulong";
  case NativeCScalarKind::LongLong:
    return "c_longlong";
  case NativeCScalarKind::UnsignedLongLong:
    return "c_ulonglong";
  case NativeCScalarKind::Float:
    return "c_float";
  case NativeCScalarKind::Double:
    return "c_double";
  case NativeCScalarKind::Size:
    return "c_size_t";
  case NativeCScalarKind::Ptrdiff:
    return "c_ptrdiff_t";
  case NativeCScalarKind::Intptr:
    return "c_intptr_t";
  case NativeCScalarKind::Uintptr:
    return "c_uintptr_t";
  case NativeCScalarKind::None:
  case NativeCScalarKind::Count:
    return {};
  }
  return {};
}

std::optional<NativeCScalarKind> parseNativeCScalarKind(std::string_view name) {
  for (unsigned index = 1;
       index < static_cast<unsigned>(NativeCScalarKind::Count); ++index) {
    const auto kind = static_cast<NativeCScalarKind>(index);
    if (nativeCScalarKindName(kind) == name) {
      return kind;
    }
  }
  return std::nullopt;
}

std::string_view nativeCScalarDeclarationName(NativeCScalarKind kind) {
  switch (kind) {
  case NativeCScalarKind::Bool:
    return "_Bool";
  case NativeCScalarKind::Char:
    return "char";
  case NativeCScalarKind::SignedChar:
    return "signed char";
  case NativeCScalarKind::UnsignedChar:
    return "unsigned char";
  case NativeCScalarKind::Short:
    return "short";
  case NativeCScalarKind::UnsignedShort:
    return "unsigned short";
  case NativeCScalarKind::Int:
    return "int";
  case NativeCScalarKind::UnsignedInt:
    return "unsigned int";
  case NativeCScalarKind::Long:
    return "long";
  case NativeCScalarKind::UnsignedLong:
    return "unsigned long";
  case NativeCScalarKind::LongLong:
    return "long long";
  case NativeCScalarKind::UnsignedLongLong:
    return "unsigned long long";
  case NativeCScalarKind::Float:
    return "float";
  case NativeCScalarKind::Double:
    return "double";
  case NativeCScalarKind::Size:
    return "size_t";
  case NativeCScalarKind::Ptrdiff:
    return "ptrdiff_t";
  case NativeCScalarKind::Intptr:
    return "intptr_t";
  case NativeCScalarKind::Uintptr:
    return "uintptr_t";
  case NativeCScalarKind::None:
  case NativeCScalarKind::Count:
    return {};
  }
  return {};
}

std::optional<TargetCScalarInfo>
TargetInfo::nativeCScalar(NativeCScalarKind kind) const {
  if (!supported()) {
    return std::nullopt;
  }
  // All accepted Unix targets use LP64; accepted Windows targets use LLP64.
  // Their pointer-sized C typedefs therefore select different primitive
  // declaration identities even though all of them occupy eight bytes.
  const bool llp64 = os == "windows";
  NativeCScalarKind declaration = kind;
  switch (kind) {
  case NativeCScalarKind::Size:
  case NativeCScalarKind::Uintptr:
    declaration = llp64 ? NativeCScalarKind::UnsignedLongLong
                        : NativeCScalarKind::UnsignedLong;
    break;
  case NativeCScalarKind::Ptrdiff:
  case NativeCScalarKind::Intptr:
    declaration = llp64 ? NativeCScalarKind::LongLong : NativeCScalarKind::Long;
    break;
  default:
    break;
  }
  TargetScalarKind value = TargetScalarKind::Count;
  switch (declaration) {
  case NativeCScalarKind::Bool:
    value = TargetScalarKind::Bool;
    break;
  case NativeCScalarKind::Char:
    value = os == "linux" && arch == "arm64" ? TargetScalarKind::UInt8
                                             : TargetScalarKind::Int8;
    break;
  case NativeCScalarKind::SignedChar:
    value = TargetScalarKind::Int8;
    break;
  case NativeCScalarKind::UnsignedChar:
    value = TargetScalarKind::UInt8;
    break;
  case NativeCScalarKind::Short:
    value = TargetScalarKind::Int16;
    break;
  case NativeCScalarKind::UnsignedShort:
    value = TargetScalarKind::UInt16;
    break;
  case NativeCScalarKind::Int:
    value = TargetScalarKind::Int32;
    break;
  case NativeCScalarKind::UnsignedInt:
    value = TargetScalarKind::UInt32;
    break;
  case NativeCScalarKind::Long:
    value = llp64 ? TargetScalarKind::Int32 : TargetScalarKind::Int64;
    break;
  case NativeCScalarKind::UnsignedLong:
    value = llp64 ? TargetScalarKind::UInt32 : TargetScalarKind::UInt64;
    break;
  case NativeCScalarKind::LongLong:
    value = TargetScalarKind::Int64;
    break;
  case NativeCScalarKind::UnsignedLongLong:
    value = TargetScalarKind::UInt64;
    break;
  case NativeCScalarKind::Float:
    value = TargetScalarKind::Float32;
    break;
  case NativeCScalarKind::Double:
    value = TargetScalarKind::Float64;
    break;
  default:
    return std::nullopt;
  }
  const auto layout = dataLayout.scalarLayout(value);
  if (!layout) {
    return std::nullopt;
  }
  return TargetCScalarInfo{declaration, value, *layout};
}

std::optional<NativeCScalarKind>
TargetInfo::fixedCScalarIdentity(TargetScalarKind kind) const {
  if (!supported()) {
    return std::nullopt;
  }
  switch (kind) {
  case TargetScalarKind::Bool:
    return NativeCScalarKind::Bool;
  case TargetScalarKind::Int8:
    return NativeCScalarKind::SignedChar;
  case TargetScalarKind::UInt8:
    return NativeCScalarKind::UnsignedChar;
  case TargetScalarKind::Int16:
    return NativeCScalarKind::Short;
  case TargetScalarKind::UInt16:
    return NativeCScalarKind::UnsignedShort;
  case TargetScalarKind::Int32:
    return NativeCScalarKind::Int;
  case TargetScalarKind::UInt32:
    return NativeCScalarKind::UnsignedInt;
  case TargetScalarKind::Int64:
    return os == "linux" ? NativeCScalarKind::Long
                         : NativeCScalarKind::LongLong;
  case TargetScalarKind::UInt64:
    return os == "linux" ? NativeCScalarKind::UnsignedLong
                         : NativeCScalarKind::UnsignedLongLong;
  case TargetScalarKind::Float32:
    return NativeCScalarKind::Float;
  case TargetScalarKind::Float64:
    return NativeCScalarKind::Double;
  // GTI char is a distinct code unit, not C's implementation-defined char.
  case TargetScalarKind::Char:
  case TargetScalarKind::Pointer:
  case TargetScalarKind::Count:
    return std::nullopt;
  }
  return std::nullopt;
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
