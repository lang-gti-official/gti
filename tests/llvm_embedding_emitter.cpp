#include "gti/driver/embedding_library.h"
#include "gti/frontend.h"
#include "gti/llvm_embedding.h"
#include "gti/lowered_program_builder.h"
#include "gti/native_header.h"
#include "gti/optimizer.h"

#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char **argv) {
  if (argc < 7)
    return 2;
  try {
    std::ifstream input(argv[1]);
    if (!input)
      return 2;
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    lang::ProgramRequest request{.kind = lang::ProgramKind::Library};
    for (int i = 6; i < argc; ++i) {
      const std::string spec = argv[i];
      const auto split = spec.find('=');
      if (split == std::string::npos)
        return 2;
      request.exports.push_back({.function = spec.substr(0, split),
                                 .symbol = spec.substr(split + 1)});
    }
    auto frontend =
        lang::Frontend({.program = request}).analyze(argv[1], source);
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      return 1;
    }
    const int level = std::stoi(argv[3]);
    lang::OptimizationPipeline pipeline;
    const auto optimization = static_cast<lang::OptimizationLevel>(level);
    const auto compatibility =
        pipeline.run(frontend.hir, optimization, lang::TargetInfo::host());
    auto optimized = pipeline.run({.hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .level = optimization,
                                   .target = lang::TargetInfo::host(),
                                   .compatibility = &compatibility});
    if (!optimized.valid())
      throw std::runtime_error("optimization rejected");
    auto lowered = lang::LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
        optimized.mir, optimized.report.transformations,
        lang::TargetInfo::host());
    if (!lowered.valid())
      throw std::runtime_error("lowering rejected");
    const auto artifacts = lang::generateLlvmEmbedding(
        *lowered.program,
        {.optimization = static_cast<lang::LlvmCodegenOptimization>(level)});
    // Generator and publisher are production APIs; only frontend request
    // assembly is test-specific while project CLI activation remains dormant.
    const auto published = lang::driver::publishEmbeddingLibrary(
        artifacts, {.compiler = argv[4],
                    .runtimeArchive = argv[5],
                    .generationDirectory = std::string(argv[2]) + "-generation",
                    .name = "module"});
    if (!published.succeeded())
      throw std::runtime_error(published.error + "\n" +
                               published.process.output);
    std::ofstream object(std::string(argv[2]) + ".o", std::ios::binary);
    object << artifacts.object.contents;
    std::ofstream generated(std::string(argv[2]) + ".h");
    generated << artifacts.header.contents;
    if (!object || !generated)
      return 2;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
