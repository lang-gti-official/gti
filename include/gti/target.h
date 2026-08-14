#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lang {

enum class ExecutionProfile {
  SingleThreaded,
  Concurrent,
};

[[nodiscard]] std::string_view executionProfileName(ExecutionProfile profile);

[[nodiscard]] std::optional<ExecutionProfile>
parseExecutionProfile(std::string_view text);

enum class TargetProperty {
  Os,
  Vendor,
  Arch,
};

[[nodiscard]] std::string_view targetPropertyName(TargetProperty property);

[[nodiscard]] std::optional<TargetProperty>
parseTargetProperty(std::string_view text);

enum class TargetEndianness : std::uint8_t {
  Little,
  Big,
};

[[nodiscard]] std::string_view
targetEndiannessName(TargetEndianness endianness);

// These are the scalar representation domains whose layout is a current GTI
// language fact. Source types that are not represented here do not yet have a
// public layout contract.
enum class TargetScalarKind : std::uint8_t {
  Bool,
  Char,
  Int8,
  Int16,
  Int32,
  Int64,
  UInt8,
  UInt16,
  UInt32,
  UInt64,
  Float32,
  Float64,
  Pointer,
  Count,
};

inline constexpr std::size_t targetScalarKindCount =
    static_cast<std::size_t>(TargetScalarKind::Count);

[[nodiscard]] std::string_view targetScalarKindName(TargetScalarKind kind);

struct TargetTypeLayout {
  std::uint32_t sizeBytes = 0;
  std::uint32_t abiAlignmentBytes = 0;
  std::uint32_t preferredAlignmentBytes = 0;

  friend bool operator==(const TargetTypeLayout &,
                         const TargetTypeLayout &) = default;
};

// A backend-neutral, immutable description of the representation facts GTI
// currently supports. The default is the canonical 64-bit little-endian
// scalar layout used by every accepted target triple. An unsupported value is
// explicit and must be rejected before semantic analysis or code generation.
class TargetDataLayout final {
public:
  constexpr TargetDataLayout()
      : scalarLayouts({TargetTypeLayout{1, 1, 1}, TargetTypeLayout{1, 1, 1},
                       TargetTypeLayout{1, 1, 1}, TargetTypeLayout{2, 2, 2},
                       TargetTypeLayout{4, 4, 4}, TargetTypeLayout{8, 8, 8},
                       TargetTypeLayout{1, 1, 1}, TargetTypeLayout{2, 2, 2},
                       TargetTypeLayout{4, 4, 4}, TargetTypeLayout{8, 8, 8},
                       TargetTypeLayout{4, 4, 4}, TargetTypeLayout{8, 8, 8},
                       TargetTypeLayout{8, 8, 8}}) {}

  [[nodiscard]] static constexpr TargetDataLayout canonical64LittleEndian() {
    return TargetDataLayout{};
  }

  [[nodiscard]] static constexpr TargetDataLayout unsupported() {
    TargetDataLayout result;
    result.supported_ = false;
    result.pointerWidthBits_ = 0;
    return result;
  }

  [[nodiscard]] constexpr bool supported() const { return supported_; }

  [[nodiscard]] constexpr TargetEndianness endianness() const {
    return endianness_;
  }

  [[nodiscard]] constexpr bool littleEndian() const {
    return endianness_ == TargetEndianness::Little;
  }

  [[nodiscard]] constexpr std::uint32_t pointerWidthBits() const {
    return pointerWidthBits_;
  }

  [[nodiscard]] constexpr std::optional<TargetTypeLayout>
  scalarLayout(TargetScalarKind kind) const {
    const std::size_t index = static_cast<std::size_t>(kind);
    if (!supported_ || index >= scalarLayouts.size()) {
      return std::nullopt;
    }
    return scalarLayouts[index];
  }

  friend bool operator==(const TargetDataLayout &,
                         const TargetDataLayout &) = default;

private:
  bool supported_ = true;
  TargetEndianness endianness_ = TargetEndianness::Little;
  std::uint32_t pointerWidthBits_ = 64;
  std::array<TargetTypeLayout, targetScalarKindCount> scalarLayouts;
};

struct TargetInfo {
  std::string os;
  std::string vendor;
  std::string arch;
  TargetDataLayout dataLayout;
  ExecutionProfile executionProfile = ExecutionProfile::SingleThreaded;

  [[nodiscard]] std::string_view value(TargetProperty property) const;

  [[nodiscard]] bool supported() const;

  [[nodiscard]] static TargetInfo host();
};

enum class TargetTripleError : std::uint8_t {
  None,
  Malformed,
  UnsupportedArchitecture,
  UnsupportedEndianness,
  UnsupportedOperatingSystem,
};

[[nodiscard]] std::string_view
targetTripleErrorMessage(TargetTripleError error);

struct TargetTripleParseResult {
  std::optional<TargetInfo> target;
  TargetTripleError error = TargetTripleError::None;

  [[nodiscard]] bool succeeded() const { return target.has_value(); }
};

// Parses and normalizes an explicit target triple such as "arm64-apple-macos"
// into GTI's target vocabulary (aarch64 -> arm64, darwin/macosx -> macos).
// The detailed form preserves why a triple was rejected; the optional wrapper
// remains for callers that only need success or failure.
[[nodiscard]] TargetTripleParseResult
parseTargetTripleResult(std::string_view text);

[[nodiscard]] std::optional<TargetInfo>
parseTargetTriple(std::string_view text);

// True when parseTargetTriple can parse triples. Retained as a capability API
// for callers even though LLVM support is mandatory.
[[nodiscard]] bool targetTripleParsingAvailable();

} // namespace lang
