#include "../src/compiler/llvm/abi.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include "gti/optimizer.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace lang;
using namespace lang::native_llvm;

// Test-only composition of an explicitly supplied production pre-O3 packet.
// Never a backend admission path or a replacement execution engine. The shared
// Source definitions go through the production ABI hook, not symbol allowlists.
int replay(int argc, char **argv) {
  if (argc != 6)
    return 2;
  const std::filesystem::path source(argv[1]), library(argv[2]), packet(argv[3]), output(argv[4]);
  if (std::string(argv[5]) != "rooted") {
    std::cerr << "baseline requires an explicitly retained pre-hook emitter\n";
    return 2;
  }
  FrontendOptions options;
  options.requireEntryPoint = true;
  options.requireFunctionDefinitions = true;
  options.toolingOccurrences = false;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  options.trustedBodies = TrustedBodyAnalysis::Reachable;
  auto frontend = Frontend(options).analyze(source, std::nullopt,
      {library / "prelude.gti"}, {}, {library});
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    throw std::runtime_error("packet source does not complete verified analysis");
  }
  const OptimizationPipeline pipeline;
  const auto compatibility = pipeline.run(frontend.hir, OptimizationLevel::O3);
  auto optimized = pipeline.run(OptimizationRequest{
      .hir = frontend.hir, .mir = std::move(frontend.mir),
      .level = OptimizationLevel::O3, .compatibility = &compatibility});
  if (!optimized.valid())
    throw std::runtime_error("packet source failed MIR optimization verification");
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir,
      optimized.sourceMir, optimized.mir, optimized.report.transformations,
      TargetInfo::host());
  if (!lowered.valid())
    throw std::runtime_error("packet source failed lowered publication");
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(*lowered.program, issues);
  if (!input)
    throw std::runtime_error("packet source has no verified snapshot");
  Session session(*input, {.optimization = LlvmCodegenOptimization::Aggressive});
  auto buffer = llvm::MemoryBuffer::getFile(packet.string());
  if (!buffer)
    throw std::runtime_error("packet is unavailable");
  auto decoded = llvm::parseBitcodeFile((*buffer)->getMemBufferRef(), session.context());
  if (!decoded) {
    llvm::consumeError(decoded.takeError());
    throw std::runtime_error("packet is not valid LLVM bitcode");
  }
  if ((*decoded)->getDataLayout() != session.dataLayout() ||
      (*decoded)->getTargetTriple() != session.module().getTargetTriple())
    throw std::runtime_error("packet target differs from the sealed native source target");
  const auto *descriptor = (*decoded)->getGlobalVariable("gti.failure.canonical", true);
  const auto *bytes = descriptor ? llvm::dyn_cast<llvm::ConstantDataSequential>(descriptor->getInitializer()) : nullptr;
  const auto &expected = input->program().mir().failureMetadata().descriptorBytes();
  if (!bytes || bytes->getRawDataValues() != llvm::StringRef(
          reinterpret_cast<const char *>(expected.data()), expected.size()))
    throw std::runtime_error("packet failure descriptor differs from the verified source snapshot");
  if (llvm::Linker::linkModules(session.module(), std::move(*decoded)))
    throw std::runtime_error("packet module import failed");
  unsigned registered = 0;
  std::set<const llvm::Function *> matched;
  std::vector<std::pair<std::size_t, std::string>> symbols;
  bool entry = false;
  for (const auto &function : input->program().mir().functionInstances()) {
    if (function.definitionKind != MirDefinitionKind::Source)
      continue;
    const auto *origin = input->program().findFunctionInstance(function.id);
    if (!origin)
      throw std::runtime_error("source instance lost its sealed provenance");
    const auto equal = [&](const llvm::Function &body, unsigned index,
                           const std::string &value) {
      const auto *metadata = body.getMetadata("gti.source");
      if (!metadata || metadata->getNumOperands() != 3)
        return false;
      const auto *text = llvm::dyn_cast<llvm::MDString>(metadata->getOperand(index));
      return text && text->getString() == value;
    };
    bool emitted = false;
    for (const auto &body : session.module())
      emitted |= equal(body, 0, origin->source.source) &&
                 equal(body, 1, std::to_string(origin->source.start)) &&
                 equal(body, 2, std::to_string(origin->source.end));
    // Analyzed library instances need not be emitted. Conversely every emitted
    // source definition below must match exactly one producer identity + ABI.
    if (!emitted)
      continue;
    const auto count = session.module().size();
    auto *body = session.abi().declareFunction(function.id);
    if (count != session.module().size() || body->isDeclaration())
      throw std::runtime_error("packet lacks exact source definition " +
          body->getName().str() + " (instance " +
          std::to_string(function.id) + ")");
    if (!equal(*body, 0, origin->source.source) ||
        !equal(*body, 1, std::to_string(origin->source.start)) ||
        !equal(*body, 2, std::to_string(origin->source.end)) ||
        !matched.insert(body).second)
      throw std::runtime_error("packet source identity differs from the verified producer");
    entry |= input->program().mir().hostedStartupPlan() &&
             input->program().mir().hostedStartupPlan()->entry == function.id;
    symbols.emplace_back(function.id, body->getName().str());
    ++registered;
  }
  for (const auto &body : session.module())
    if (body.getMetadata("gti.source") && !matched.contains(&body))
      throw std::runtime_error("packet has an unverified source definition");
  if (!entry || !session.module().getFunction("main") ||
      session.module().getFunction("main")->isDeclaration())
    throw std::runtime_error("packet lacks a verified hosted entry");
  session.verify();
  const auto artifact = session.emitObject();
  std::ofstream object(output, std::ios::binary);
  object.write(artifact.contents.data(), artifact.contents.size());
  if (!object || artifact.contents.empty())
    throw std::runtime_error("packet object publication failed");
  std::error_code error;
  llvm::raw_fd_ostream ir(output.string() + ".ll", error, llvm::sys::fs::OF_None);
  if (error)
    throw std::runtime_error(error.message());
  session.module().print(ir, nullptr);
  llvm::json::Array definitions;
  for (const auto &[id, name] : symbols) {
    const auto *function = session.module().getFunction(name);
    definitions.push_back(llvm::json::Object{
        {"instance", static_cast<std::int64_t>(id)}, {"symbol", name},
        {"after", !function ? "removed" : function->hasExternalLinkage()
                                              ? "external" : "internal"}});
  }
  llvm::raw_fd_ostream report(output.string() + ".json", error,
                             llvm::sys::fs::OF_None);
  if (error)
    throw std::runtime_error(error.message());
  report << llvm::json::Value(llvm::json::Object{
      {"mode", argv[5]}, {"source_definitions", std::move(definitions)},
      {"triple", session.module().getTargetTriple().str()},
      {"data_layout", session.dataLayout().getStringRepresentation()}});
  std::cout << "verified packet source definitions=" << registered
            << " mode=" << argv[5] << '\n';
  return 0;
}

int main(int argc, char **argv) {
  try {
    return replay(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
