#pragma once

#include "gti/bindgen/native_model.h"

#include <string>
#include <string_view>
#include <vector>

// The parsing seam. A NativeHeaderReader turns header text into the native
// model plus diagnostics. BoundedCReader is the first implementation — a
// hand-written bounded C17 declaration parser; a future ClangReader for
// C/C++ replaces the implementation, never the pipeline.

namespace lang::bindgen {

enum class ReaderSeverity {
  // The model omits something the header contains; reading continued.
  Warning,
  // The header is structurally unreadable; the model is unusable.
  Error,
};

struct ReaderDiagnostic {
  ReaderSeverity severity = ReaderSeverity::Warning;
  SourceOrigin origin;
  std::string message;
};

struct ReadResult {
  NativeModel model;
  std::vector<ReaderDiagnostic> diagnostics;
  [[nodiscard]] bool usable() const {
    for (const ReaderDiagnostic &diagnostic : diagnostics) {
      if (diagnostic.severity == ReaderSeverity::Error) {
        return false;
      }
    }
    return true;
  }
};

enum class CDataModel {
  // 64-bit Unix targets: int=32, long=64, pointer/size=64.
  Lp64,
  // 64-bit Windows targets: int/long=32, pointer/size=64.
  Llp64,
};

// The platform seam for C's implementation-defined integer widths. GTI's
// supported targets are all 64-bit, so their C ABIs reduce to LP64 and LLP64.
// Keeping the choice explicit prevents a host assumption from leaking into a
// cross-target binding file; a future Clang-backed reader may replace these
// facts with Clang target information behind the same reader boundary.
class CTargetProfile final {
public:
  constexpr explicit CTargetProfile(CDataModel dataModel = CDataModel::Lp64)
      : dataModel_(dataModel) {}

  [[nodiscard]] constexpr CDataModel dataModel() const { return dataModel_; }
  [[nodiscard]] constexpr std::uint32_t intBits() const { return 32; }
  [[nodiscard]] constexpr std::uint32_t longBits() const {
    return dataModel_ == CDataModel::Llp64 ? 32 : 64;
  }
  [[nodiscard]] constexpr std::uint32_t longLongBits() const { return 64; }
  [[nodiscard]] constexpr std::uint32_t sizeBits() const { return 64; }

private:
  CDataModel dataModel_;
};

class NativeHeaderReader {
public:
  virtual ~NativeHeaderReader() = default;
  [[nodiscard]] virtual ReadResult read(std::string_view headerText) = 0;
};

class BoundedCReader final : public NativeHeaderReader {
public:
  explicit BoundedCReader(CTargetProfile profile = CTargetProfile{});
  [[nodiscard]] ReadResult read(std::string_view headerText) override;

private:
  CTargetProfile profile_;
};

} // namespace lang::bindgen
