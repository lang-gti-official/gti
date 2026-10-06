#include "gti/driver/semver.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <vector>

namespace lang::driver {
namespace {
std::vector<std::string_view> parts(std::string_view text) {
  std::vector<std::string_view> result;
  for (;;) {
    const auto dot = text.find('.');
    result.push_back(text.substr(0, dot));
    if (dot == text.npos) {
      return result;
    }
    text.remove_prefix(dot + 1);
  }
}
bool digits(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
    return c >= '0' && c <= '9';
  });
}
bool identifiers(std::string_view text, bool prerelease) {
  for (auto part : parts(text)) {
    if (part.empty() ||
        !std::all_of(part.begin(), part.end(),
                     [](char c) {
                       return (c >= 'a' && c <= 'z') ||
                              (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '-';
                     }) ||
        (prerelease && digits(part) && part.size() > 1 &&
         part.front() == '0')) {
      return false;
    }
  }
  return true;
}
} // namespace

std::optional<PackageVersion> parsePackageVersion(std::string_view text) {
  if (text.size() > 256) {
    return std::nullopt;
  }
  PackageVersion result;
  if (const auto plus = text.find('+'); plus != text.npos) {
    result.build = text.substr(plus + 1);
    if (!identifiers(result.build, false)) {
      return std::nullopt;
    }
    text = text.substr(0, plus);
  }
  if (const auto dash = text.find('-'); dash != text.npos) {
    result.prerelease = text.substr(dash + 1);
    if (!identifiers(result.prerelease, true)) {
      return std::nullopt;
    }
    text = text.substr(0, dash);
  }
  const auto components = parts(text);
  if (components.size() != 3) {
    return std::nullopt;
  }
  std::uint64_t *values[] = {&result.major, &result.minor, &result.patch};
  for (std::size_t i = 0; i < 3; ++i) {
    const auto part = components[i];
    if (!digits(part) || (part.size() > 1 && part.front() == '0')) {
      return std::nullopt;
    }
    const auto parsed =
        std::from_chars(part.data(), part.data() + part.size(), *values[i]);
    if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size()) {
      return std::nullopt;
    }
  }
  return result;
}

std::string PackageVersion::text() const {
  return std::to_string(major) + "." + std::to_string(minor) + "." +
         std::to_string(patch) + (prerelease.empty() ? "" : "-" + prerelease) +
         (build.empty() ? "" : "+" + build);
}

std::strong_ordering
PackageVersion::operator<=>(const PackageVersion &other) const {
  if (auto c = major <=> other.major; c != 0) {
    return c;
  }
  if (auto c = minor <=> other.minor; c != 0) {
    return c;
  }
  if (auto c = patch <=> other.patch; c != 0) {
    return c;
  }
  if (prerelease.empty() || other.prerelease.empty()) {
    return prerelease.empty() <=> other.prerelease.empty();
  }
  auto left = parts(prerelease), right = parts(other.prerelease);
  for (std::size_t i = 0; i < std::min(left.size(), right.size()); ++i) {
    const bool a = digits(left[i]), b = digits(right[i]);
    if (a != b) {
      return a ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    if (a && left[i].size() != right[i].size()) {
      return left[i].size() <=> right[i].size();
    }
    if (auto c = left[i] <=> right[i]; c != 0) {
      return c;
    }
  }
  return left.size() <=> right.size();
}
bool PackageVersion::operator==(const PackageVersion &other) const {
  return (*this <=> other) == 0;
}

std::optional<PackageVersionRequirement>
PackageVersionRequirement::parse(std::string_view text) {
  if (text == "*") {
    return PackageVersionRequirement{};
  }
  if (text.empty()) {
    return std::nullopt;
  }
  char kind = '^';
  if (text.front() == '^' || text.front() == '~' || text.front() == '=') {
    kind = text.front();
    text.remove_prefix(1);
  }
  const auto suffix = text.find_first_of("-+");
  const auto count = parts(text.substr(0, suffix)).size();
  if (count > 3 || (count < 3 && (kind == '=' || suffix != text.npos))) {
    return std::nullopt;
  }
  std::string expanded(text);
  for (std::size_t i = count; i < 3; ++i) {
    expanded += ".0";
  }
  auto version = parsePackageVersion(expanded);
  if (!version) {
    return std::nullopt;
  }
  PackageVersionRequirement result;
  result.lower = *version;
  result.exact = kind == '=';
  if (!result.exact) {
    PackageVersion upper = *version;
    upper.prerelease.clear();
    upper.build.clear();
    std::uint64_t *bump = nullptr;
    if ((kind == '~' && count == 1) ||
        (kind == '^' && (upper.major != 0 || count == 1))) {
      bump = &upper.major;
      upper.minor = upper.patch = 0;
    } else if (kind == '~' || upper.minor != 0 || count == 2) {
      bump = &upper.minor;
      upper.patch = 0;
    } else {
      bump = &upper.patch;
    }
    if (*bump == std::numeric_limits<std::uint64_t>::max()) {
      return std::nullopt;
    }
    ++*bump;
    result.upper = upper;
  }
  return result;
}
bool PackageVersionRequirement::accepts(const PackageVersion &version) const {
  if (!version.prerelease.empty() &&
      (lower.prerelease.empty() || lower.major != version.major ||
       lower.minor != version.minor || lower.patch != version.patch)) {
    return false;
  }
  return exact ? version == lower
               : version >= lower && (!upper || version < *upper);
}

} // namespace lang::driver
