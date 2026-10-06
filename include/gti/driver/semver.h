#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lang::driver {

struct PackageVersion {
  std::uint64_t major = 0;
  std::uint64_t minor = 0;
  std::uint64_t patch = 0;
  std::string prerelease;
  std::string build;

  [[nodiscard]] std::string text() const;
  [[nodiscard]] std::strong_ordering
  operator<=>(const PackageVersion &other) const;
  [[nodiscard]] bool operator==(const PackageVersion &other) const;
};

[[nodiscard]] std::optional<PackageVersion>
parsePackageVersion(std::string_view text);

// The bootstrap accepts caret (including bare requirements), tilde, * and exact
// requirements. Missing minor/patch components are zero-filled for ranges;
// exact requirements need all three. Prereleases require explicit opt-in at
// the same major/minor/patch, as in conventional SemVer range resolution.
class PackageVersionRequirement final {
public:
  [[nodiscard]] static std::optional<PackageVersionRequirement>
  parse(std::string_view text);
  [[nodiscard]] bool accepts(const PackageVersion &version) const;

private:
  PackageVersion lower;
  std::optional<PackageVersion> upper;
  bool exact = false;
};

} // namespace lang::driver
