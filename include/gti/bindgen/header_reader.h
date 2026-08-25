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

// The platform seam: how C's platform-width builtin spellings resolve to
// sized integers. The defaults describe the LP64 hosts the toolchain
// currently targets; a target-selected profile is the extension point.
struct CTargetProfile {
  std::uint32_t intBits = 32;
  std::uint32_t longBits = 64;
  std::uint32_t longLongBits = 64;
  std::uint32_t sizeBits = 64;
  bool plainCharIsSigned = true;
};

class NativeHeaderReader {
public:
  virtual ~NativeHeaderReader() = default;
  [[nodiscard]] virtual ReadResult read(std::string_view headerText) = 0;
};

class BoundedCReader final : public NativeHeaderReader {
public:
  explicit BoundedCReader(CTargetProfile profile = {});
  [[nodiscard]] ReadResult read(std::string_view headerText) override;

private:
  CTargetProfile profile_;
};

} // namespace lang::bindgen
