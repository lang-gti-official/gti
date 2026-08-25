#pragma once

#include "gti/bindgen/binding_planner.h"
#include "gti/bindgen/header_reader.h"

#include <string>
#include <string_view>

// The pipeline facade. The CLI subcommand and the future driver/manifest
// integration are both thin callers of this one entry point.

namespace lang::bindgen {

struct BindgenResult {
  std::string gtiSource;
  BindingPlan plan;
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

[[nodiscard]] BindgenResult
runCBindgen(std::string_view headerText, std::string_view headerName,
            BindingOptions options = {},
            CTargetProfile profile = CTargetProfile{});

} // namespace lang::bindgen
