// libFuzzer harness for the whole GTI analysis-to-backend pipeline
// (Q-FUZZ-01B, docs/plans/compiler-bug-finding-and-triage.md).
//
// The target class is "one phase accepts a value that a later phase cannot
// represent": a program that passes frontend analysis must flow through the
// optimizer, the lowered-program builder, and both backends, ending in either
// a valid artifact or an intentional structured rejection. A silent invalid
// result is a harness-reported failure; a crash or sanitizer report anywhere
// is a finding.
//
// Compiler meaning stays in the production implementations and their
// fail-closed verifiers; this harness only drives them and checks the
// artifact-or-rejection contract. Native C++ compilation and program
// execution never run in-process — they belong to the differential runner
// and the capture tool.

#include "fuzz_support.h"

#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include "gti/mir_backend.h"
#include "gti/optimizer.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <utility>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  if (size > gti_fuzz::kMaxPipelineInputBytes) {
    return 0;
  }
  gti_fuzz::checkInjectedFailure(data, size);

  std::string source(reinterpret_cast<const char *>(data), size);
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("fuzz.gti", std::move(source));

  if (!frontend.canGenerateCode()) {
    // A rejected program must be rejected out loud. Accepting silence here
    // would let a phase drop a program without ever telling the user why.
    if (frontend.diagnostics.empty()) {
      gti_fuzz::fail("pipeline", "program rejected with zero diagnostics");
    }
    return 0;
  }

  // Bounded optimization selection, deterministic per input so every corpus
  // entry replays identically: O0 always exercises the unoptimized path, and
  // inputs whose first byte is odd also cross the O2 rewrite surface.
  const lang::OptimizationLevel level = (size > 0 && (data[0] & 1) != 0)
                                            ? lang::OptimizationLevel::O2
                                            : lang::OptimizationLevel::O0;

  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir, level);
  const lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = level,
                                        .compatibility = &compatibility});
  if (!optimized.valid()) {
    // The optimizer's verifier rejected its own output for an accepted
    // program: exactly the drift this target hunts.
    gti_fuzz::fail("pipeline", "optimizer produced an incoherent program");
  }

  lang::LoweredProgramBuild lowered = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
      optimized.mir, optimized.report.transformations,
      lang::TargetInfo::host());
  if (!lowered.valid()) {
    if (lowered.issues.empty()) {
      gti_fuzz::fail("pipeline",
                     "lowered-program builder rejected without issues");
    }
    // Intentional structured rejection: the builder said why. Recorded by
    // the corpus, not a harness failure.
    return 0;
  }

  // A backend reports an internal error by throwing, and the driver turns
  // that into GTI-B0001. Letting it escape here would abort the process with
  // no diagnostic, so every ICE would look like a memory-safety crash and
  // the first one would end the run. Catch it, print what the user would
  // have seen, and then fail deliberately — an ICE is still a bug.
  try {
    const lang::BackendArtifact mirArtifact =
        lang::MirBackend().generate(*lowered.program);
    const lang::BackendArtifact cppArtifact =
        lang::CppBackend(lang::CppStandard::Cpp23).generate(*lowered.program);
    if (cppArtifact.contents.empty()) {
      gti_fuzz::fail("pipeline", "C++ backend produced an empty artifact");
    }
    (void)mirArtifact;
  } catch (const std::exception &exception) {
    std::fprintf(stderr, "gti-fuzz(pipeline): backend internal error: %s\n",
                 exception.what());
    gti_fuzz::fail("pipeline", "backend raised an internal error");
  }
  return 0;
}
