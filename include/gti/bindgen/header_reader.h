#pragma once

#include "gti/bindgen/native_model.h"

#include <string>
#include <string_view>
#include <vector>

// The parsing seam. A NativeHeaderReader turns header text into the native
// model plus diagnostics. ClangCReader is the active implementation — a
// private libclang-backed C17 declaration reader (ADR 021). The pipeline
// depends only on this seam; a future C++ reader extends the implementation,
// never the pipeline.

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

// The libclang-backed C17 reader (ADR 021). The header text is parsed as one
// in-memory Clang translation unit for a canonical target matching the
// requested C data model, so preprocessing, parsing, and integer widths are
// Clang facts rather than GTI reimplementations. Includes are still not
// resolved at this seam: declarations come from the given text alone, and
// every `#include` reports the same unresolved-include note the pipeline has
// always emitted. libclang stays private to the implementation translation
// unit; no Clang type crosses this interface.
class ClangCReader final : public NativeHeaderReader {
public:
  explicit ClangCReader(CTargetProfile profile = CTargetProfile{});
  [[nodiscard]] ReadResult read(std::string_view headerText) override;

private:
  CTargetProfile profile_;
};

} // namespace lang::bindgen
