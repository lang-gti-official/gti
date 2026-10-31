#include "gti/llvm_backend.h"

#include "gti/llvm_admission.h"
#include "gti/lowered_program.h"
#include "gti/native_header.h"
#include "gti/target.h"
#include "llvm/body.h"
#include "llvm/placement.h"
#include "llvm/session.h"
#include <llvm/Config/llvm-config.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBufferRef.h>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lang {
namespace {

bool sameNativeTarget(const TargetInfo &left, const TargetInfo &right) {
  return left.os == right.os && left.vendor == right.vendor &&
         left.arch == right.arch && left.dataLayout == right.dataLayout;
}

} // namespace

void verifyCompiledScalarObject(std::string_view bytes,
                                const TargetInfo &target,
                                const std::vector<std::string> &symbols,
                                const std::vector<std::string> &imports) {
  auto parsed = llvm::object::ObjectFile::createObjectFile(
      llvm::MemoryBufferRef(llvm::StringRef(bytes.data(), bytes.size()),
                            "compiled scalar object"));
  if (!parsed)
    throw std::invalid_argument("invalid compiled scalar object: " +
                                llvm::toString(parsed.takeError()));
  const auto machine = native_llvm::makeMachine(target, {});
  const auto &triple = machine->getTargetTriple();
  const auto &object = **parsed;
  if (!object.isRelocatableObject() || object.getArch() != triple.getArch() ||
      object.getBytesInAddress() != 8 ||
      object.isMachO() != triple.isOSBinFormatMachO() ||
      object.isELF() != triple.isOSBinFormatELF() ||
      object.isCOFF() != triple.isOSBinFormatCOFF())
    throw std::invalid_argument(
        "compiled scalar object target or format mismatch");
  std::set<std::string> expected;
  for (const auto &symbol : symbols)
    expected.insert(object.isMachO() ? "_" + symbol : symbol);
  std::set<std::string> allowedImports;
  for (const auto &symbol : imports)
    allowedImports.insert(object.isMachO() ? "_" + symbol : symbol);
  for (const auto &symbol : object.symbols()) {
    auto flags = symbol.getFlags();
    auto name = symbol.getName();
    if (!flags || !name) {
      if (!flags)
        llvm::consumeError(flags.takeError());
      if (!name)
        llvm::consumeError(name.takeError());
      throw std::invalid_argument(
          "invalid compiled scalar object symbol table");
    }
    if (*flags & llvm::object::SymbolRef::SF_Undefined) {
      if (!allowedImports.contains(name->str()))
        throw std::invalid_argument(
            "compiled scalar library has unresolved native dependency: " +
            name->str());
      continue;
    }
    if ((*flags & llvm::object::SymbolRef::SF_Global) &&
        !expected.erase(name->str()))
      throw std::invalid_argument(
          "unexpected compiled scalar external definition: " + name->str());
  }
  if (!expected.empty())
    throw std::invalid_argument(
        "compiled scalar object is missing an exported symbol");
}

std::string_view llvmInstrumentationModeName(LlvmInstrumentationMode mode) {
  switch (mode) {
  case LlvmInstrumentationMode::None:
    return "none";
  case LlvmInstrumentationMode::Address:
    return "address";
  case LlvmInstrumentationMode::UndefinedMemory:
    return "undefined-memory";
  case LlvmInstrumentationMode::AddressAndUndefinedMemory:
    return "address-and-undefined-memory";
  case LlvmInstrumentationMode::Thread:
    return "thread";
  }
  throw std::invalid_argument("invalid LLVM instrumentation policy");
}

std::optional<LlvmBackendIdentity>
queryLlvmBackendIdentity(const TargetInfo &target, LlvmBackendOptions options,
                         std::string &error) {
  try {
    const auto machine = native_llvm::makeMachine(target, options);
    const auto layout = machine->createDataLayout();
    llvm::LLVMContext context;
    native_llvm::verifyTargetLayout(target, layout, context);
    static int executableAnchor;
    const std::string executable =
        llvm::sys::fs::getMainExecutable(nullptr, &executableAnchor);
    if (executable.empty()) {
      error = "cannot locate the containing compiler executable";
      return std::nullopt;
    }
    return LlvmBackendIdentity{
        .schema = "gti-llvm-object-identity-v2",
        .llvmVersion = LLVM_VERSION_STRING,
        .compilerExecutable = executable,
        .triple = machine->getTargetTriple().str(),
        .dataLayout = layout.getStringRepresentation(),
        .cpu = machine->getTargetCPU().str(),
        .tuneCpu = std::string(native_llvm::tuneCpu(target)),
        .features = machine->getTargetFeatureString().str(),
        .privateAbi = native_llvm::executionAbiVersion,
        .relocation = static_cast<std::uint64_t>(machine->getRelocationModel()),
        .codeModel = static_cast<std::uint64_t>(machine->getCodeModel()),
        .codegenOptimization =
            static_cast<std::uint64_t>(machine->getOptLevel()),
        .debugInfo = options.debugInfo,
        .instrumentation =
            std::string(llvmInstrumentationModeName(options.instrumentation)),
    };
  } catch (const std::exception &failure) {
    error = failure.what();
    return std::nullopt;
  }
}

LlvmBackend::LlvmBackend(LlvmBackendOptions options) : options(options) {}

std::string_view LlvmBackend::name() const { return "llvm"; }

BackendArtifact LlvmBackend::generate(LoweredProgram program) {
  std::vector<LoweredProgramIssue> issues;
  std::optional<VerifiedLoweredProgram> verified =
      verifiedLoweredProgram(std::move(program), issues);
  if (!verified) {
    throw std::logic_error(
        "LLVM backend requires a verified lowered program: " +
        issues.front().detail);
  }

  if (!sameNativeTarget(verified->program().target(), TargetInfo::host())) {
    throw BackendUnsupportedError(
        "the LLVM backend foundation currently accepts only the host target");
  }

  // Admission, lowering and emission share one immutable program snapshot.
  const native_llvm::HierarchyStorageScope hierarchyStorage(
      verified->program().mir());
  const LlvmAdmissionPlan admission = planLlvmAdmission(*verified, options);
  if (const auto *unsupported = admission.firstUnsupported()) {
    if (!unsupported->source.source.empty()) {
      throw BackendUnsupportedError(admission.limitation(),
                                    unsupported->source);
    }
    throw BackendUnsupportedError(admission.limitation());
  }

  native_llvm::Session session(*verified, options);
  native_llvm::lowerScalarProgram(session, admission);
  auto artifact = session.emitObject();
  if (const auto &plan = verified->program().embeddingPlan()) {
    EmbeddingCompanion companion;
    companion.header =
        NativeHeaderBackend().generate(verified->program()).contents;
    companion.publicSymbols = {"gti_context_create_v1",
                               "gti_context_destroy_v1",
                               "gti_get_module_descriptor_v1"};
    for (const auto &selected : plan->exports)
      companion.publicSymbols.push_back(selected.symbol);
    artifact.embedding = std::move(companion);
  }
  return artifact;
}

} // namespace lang
