#pragma once

#include "gti/diagnostic.h"

#include <filesystem>
#include <string>
#include <vector>

namespace lang::driver {

// Resolved by the command adapter. Recording observes diagnostics; it never
// changes their meaning, severity, or the command's exit status.
struct DiagnosticRecordContext {
  std::filesystem::path projectRoot;
  std::vector<std::string> arguments;
  std::string compilerVersion;
  std::string target;
  std::string backend;
  std::string profile;
};

struct DiagnosticRecordResult {
  std::filesystem::path path;
  std::string error;
};

[[nodiscard]] DiagnosticRecordResult
recordDiagnostics(const DiagnosticRecordContext &context,
                  std::span<const Diagnostic> diagnostics,
                  const SourceManager &sources);

// Export the immutable evidence, never re-read project source or execute a
// saved command. The destination must not already exist.
[[nodiscard]] DiagnosticRecordResult
exportDiagnosticRecord(const std::filesystem::path &record,
                       const std::filesystem::path &destination,
                       std::string_view note = {});

} // namespace lang::driver
