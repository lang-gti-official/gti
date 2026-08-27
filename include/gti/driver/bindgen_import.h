#pragma once

#include "gti/diagnostic.h"
#include "gti/driver/project.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Driver-owned generation for manifest-declared bindgen imports (BUILD-01 in
// docs/plans/native-bindings-import.md). Each `[bindgen.<alias>]` table a
// build plan carries is turned into `bindings.gti` inside its synthetic
// package source root before source loading, so `#include <alias/bindings>`
// resolves like any package include. Generation is deterministic and keyed:
// an unchanged preprocessed header, options, target, and tool identity reuse
// the published file without re-running the importer.

namespace lang::driver {

enum class BindgenImportStatus {
  Generated,
  Reused,
  PreprocessingFailure,
  GenerationFailure,
  PublicationFailure,
};

struct BindgenImportOutcome {
  BindgenImportStatus status = BindgenImportStatus::GenerationFailure;
  std::string packageName;
  std::string alias;
  std::filesystem::path output;
  std::size_t importedCount = 0;
  std::size_t skippedCount = 0;
  // Presentation lines (native preprocessor output, reader notes, and skip
  // reports); the CLI decides how much of this to show.
  std::vector<std::string> notes;
  // Failures, anchored at the import's manifest declaration.
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == BindgenImportStatus::Generated ||
           status == BindgenImportStatus::Reused;
  }
};

struct BindgenImportGenerationResult {
  std::vector<BindgenImportOutcome> outcomes;

  [[nodiscard]] bool succeeded() const {
    for (const BindgenImportOutcome &outcome : outcomes) {
      if (!outcome.succeeded()) {
        return false;
      }
    }
    return true;
  }
};

// Generates every bindgen import the plan carries, in plan order. The Clang
// executable follows discoverBindgenClang; `toolIdentity` joins the reuse
// key so a changed toolchain regenerates (the CLI passes the GTI release
// version).
[[nodiscard]] BindgenImportGenerationResult
generateBindgenImports(const ProjectBuildPlan &plan,
                       const std::string &clangExecutable,
                       std::string_view toolIdentity);

} // namespace lang::driver
