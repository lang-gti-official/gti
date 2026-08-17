// Emits one GTI source down both C++ representation paths so an external
// driver can compare observable program behavior.
//
// Both paths are existing public APIs, so this helper adds no production
// surface and no fallback route:
//
//   * the MIR-preferring path is `CppBackend::generate`, which selects every
//     eligible verified-MIR body family and sends the rest through the
//     compatibility emitter;
//   * the compatibility path is the `CppEmitter` constructor that takes no
//     `MirProgram`. `docs/plans/implementation-sequence.md` retains that
//     "explicit public direct-emitter API" until the final cutover, and with
//     no MIR every family selector deselects, so the whole program is emitted
//     from AST/semantics/HIR.
//
// Per `docs/architecture/overview.md`, the Backend owns representation and
// artifact generation. Selecting between two backend representations of the
// same verified IR is therefore a backend-layer choice. A driver or CLI switch
// would place a representation policy in a layer whose table entry forbids it,
// and would create exactly the durable fallback surface that
// `implementation-sequence.md` prohibits for a migrated family.

#include "gti/backend.h"
#include "gti/cpp_backend.h"
#include "gti/cpp_emitter.h"
#include "gti/frontend.h"
#include "gti/optimizer.h"

#include "../src/compiler/cpp_mir_body_emitter.h"
#include "../src/compiler/cpp_mir_representation_snapshot.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int usage() {
  std::cerr << "usage: gti_mir_differential_helper <source.gti> <out-dir> "
               "[<prelude-root>]\n";
  return 2;
}

bool writeFile(const std::filesystem::path &path, const std::string &contents) {
  std::ofstream out(path, std::ios::binary);
  out << contents;
  return static_cast<bool>(out);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3 || argc > 4) {
    return usage();
  }
  const std::filesystem::path source = argv[1];
  const std::filesystem::path outputDirectory = argv[2];

  std::ifstream input(source);
  if (!input) {
    std::cerr << "cannot read source: " << source.string() << '\n';
    return 2;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();

  lang::FrontendResult frontend = [&] {
    if (argc == 4) {
      const std::filesystem::path root = argv[3];
      return lang::Frontend().analyze(source.filename().string(), buffer.str(),
                                      {root / "prelude.gti"}, {}, {root});
    }
    return lang::Frontend().analyze(source.filename().string(), buffer.str());
  }();

  if (!frontend.canGenerateCode()) {
    // Not a differential result: a source the frontend rejects has no
    // representation on either path.
    std::cout << "status: frontend-rejected\n";
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return 3;
  }

  const lang::OptimizationPipeline pipeline;
  const lang::OptimizationResult compatibilityOptimizations =
      pipeline.run(frontend.hir, lang::OptimizationLevel::O0);
  const lang::OptimizedProgram optimized =
      pipeline.run({.hir = frontend.hir,
                    .mir = frontend.mir,
                    .level = lang::OptimizationLevel::O0,
                    .compatibility = &compatibilityOptimizations});
  if (!optimized.valid()) {
    std::cout << "status: optimization-invalid\n";
    return 3;
  }

  std::string mirContents;
  try {
    const lang::BackendArtifact artifact = lang::CppBackend().generate(
        {.program = frontend.program,
         .semantics = frontend.semantics,
         .hir = frontend.hir,
         .mir = optimized.mir,
         .sourceMir = &frontend.mir,
         .optimizations = compatibilityOptimizations});
    mirContents = artifact.contents;
  } catch (const std::exception &error) {
    std::cout << "status: mir-path-rejected\n";
    std::cerr << error.what() << '\n';
    return 3;
  }

  const std::string compatibilityContents =
      lang::CppEmitter(frontend.semantics, frontend.hir,
                       lang::CppStandard::Cpp23, lang::TargetInfo::host(),
                       &compatibilityOptimizations)
          .emit(frontend.program);

  std::error_code directoryError;
  std::filesystem::create_directories(outputDirectory, directoryError);
  const std::filesystem::path mirPath = outputDirectory / "mir.cpp";
  const std::filesystem::path compatibilityPath =
      outputDirectory / "compatibility.cpp";
  if (!writeFile(mirPath, mirContents) ||
      !writeFile(compatibilityPath, compatibilityContents)) {
    std::cerr << "cannot write emitted sources\n";
    return 2;
  }

  // The marker count is how many bodies the MIR path actually emitted from
  // verified MIR. It bounds what a behavioral comparison can attribute.
  std::size_t mirEmittedBodies = 0;
  const std::string marker = "// GTI verified-MIR body:";
  for (std::size_t position = mirContents.find(marker);
       position != std::string::npos;
       position = mirContents.find(marker, position + marker.size())) {
    ++mirEmittedBodies;
  }

  // The program's complete body inventory, enumerated by the same
  // analysis the production admission loop runs, so the emitted count is
  // always reported against the real total rather than a re-derivation.
  const lang::CppMirBodyEmissionMap rows(lang::buildCppMirBodyEmissionMapRows(
      frontend.semantics, optimized.mir, lang::CppStandard::Cpp23));
  const std::size_t mirTotalBodies =
      lang::CppMirBodyEmitter(optimized.mir, rows)
          .analyzeProgram()
          .bodies.size();

  std::cout << "status: emitted\n";
  std::cout << "mir-emitted-bodies: " << mirEmittedBodies << '\n';
  std::cout << "mir-total-bodies: " << mirTotalBodies << '\n';
  std::cout << "identical-text: "
            << (mirContents == compatibilityContents ? "yes" : "no") << '\n';
  return 0;
}
