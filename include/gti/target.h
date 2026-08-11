#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace lang {

enum class TargetProperty {
  Os,
  Vendor,
  Arch,
};

struct TargetInfo {
  std::string os;
  std::string vendor;
  std::string arch;
  // Data-layout facts a target commits to. GTI currently supports only
  // 64-bit little-endian targets; parseTargetTriple rejects anything else,
  // and the standard-library size_t/ptrdiff_t aliases assume these values.
  unsigned pointerWidth = 64;
  bool littleEndian = true;

  [[nodiscard]] std::string_view value(TargetProperty property) const {
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

  [[nodiscard]] static TargetInfo host() {
    TargetInfo target;

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
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    target.arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    target.arch = "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    target.arch = "x86";
#else
    target.arch = "unknown";
#endif

    return target;
  }
};

// Parses and normalizes an explicit target triple such as "arm64-apple-macos"
// into GTI's target vocabulary (aarch64 -> arm64, darwin/macosx -> macos).
// Returns std::nullopt for a malformed triple or an unsupported (non-64-bit or
// big-endian) target. Implemented in src/compiler/target.cpp.
[[nodiscard]] std::optional<TargetInfo>
parseTargetTriple(std::string_view text);

// True when parseTargetTriple can parse triples. Retained as a capability API
// for callers even though LLVM support is now mandatory.
[[nodiscard]] bool targetTripleParsingAvailable();

} // namespace lang
