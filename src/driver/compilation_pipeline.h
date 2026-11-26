#pragma once

#include "gti/driver/compilation.h"
#include "gti/frontend.h"

namespace lang::driver::detail {
void restoreInstalledDiagnosticSources(FrontendResult &result);

// Separate library producers need the checked public surface before code
// generation.
FrontendResult analyzeForArtifact(const CompilationRequest &, CompilationInputs,
                                  ProgressObserver *);
CompilationResult generateArtifact(const CompilationRequest &, FrontendResult,
                                   Backend &, ProgressObserver *);
} // namespace lang::driver::detail
