#pragma once

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

} // namespace lang
