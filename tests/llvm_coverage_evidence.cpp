#include "gti/build_identity_config.h"
#include "gti/driver/build.h"
#include "gti/driver/compilation.h"
#include "gti/driver/native_toolchain.h"
#include "gti/frontend.h"
#include "gti/llvm_admission.h"
#include "gti/lowered_program_builder.h"
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/SHA256.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef GTI_COVERAGE_BUILD_CONFIG
#define GTI_COVERAGE_BUILD_CONFIG "unknown"
#endif
#ifndef GTI_COVERAGE_SYSTEM_NAME
#define GTI_COVERAGE_SYSTEM_NAME "unknown"
#endif
#ifndef GTI_COVERAGE_SYSTEM_PROCESSOR
#define GTI_COVERAGE_SYSTEM_PROCESSOR "unknown"
#endif
#ifndef GTI_COVERAGE_COMPILER
#define GTI_COVERAGE_COMPILER "unknown"
#endif
#ifndef GTI_COVERAGE_LLVM_VERSION
#define GTI_COVERAGE_LLVM_VERSION "unknown"
#endif

namespace {

void writeJsonString(std::ostream &output, std::string_view value) {
  output << '"';
  for (const unsigned char byte : value) {
    switch (byte) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\b':
      output << "\\b";
      break;
    case '\f':
      output << "\\f";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (byte < 0x20U) {
        constexpr char hex[] = "0123456789abcdef";
        output << "\\u00" << hex[(byte >> 4U) & 0xfU] << hex[byte & 0xfU];
      } else {
        output << static_cast<char>(byte);
      }
    }
  }
  output << '"';
}

std::string_view stateName(lang::LlvmCoverageState state) {
  switch (state) {
  case lang::LlvmCoverageState::Pending:
    return "pending";
  case lang::LlvmCoverageState::Implemented:
    return "implemented";
  case lang::LlvmCoverageState::VerifiedNonExecuting:
    return "verified-nonexecuting";
  }
  return "unknown";
}

std::string_view ruleName(lang::LlvmNonExecutingRule rule) {
  switch (rule) {
  case lang::LlvmNonExecutingRule::None:
    return "none";
  case lang::LlvmNonExecutingRule::VerifiedMetadata:
    return "verified-metadata";
  case lang::LlvmNonExecutingRule::MustBeAbsent:
    return "must-be-absent";
  }
  return "unknown";
}

std::string_view closureName(lang::LlvmAdmissionClosure closure) {
  switch (closure) {
  case lang::LlvmAdmissionClosure::AllExecutableBodies:
    return "all-executable-bodies";
  case lang::LlvmAdmissionClosure::SealedEntryDependencies:
    return "sealed-entry-dependencies";
  }
  return "unknown";
}

void writeField(std::ostream &output, std::string_view name,
                std::string_view value) {
  writeJsonString(output, name);
  output << ':';
  writeJsonString(output, value);
}

int writeLedger() {
  const bool exactSource =
      std::string_view(GTI_COMPILER_SOURCE_STATE) == "clean" &&
      std::string_view(GTI_COMPILER_SOURCE_COMMIT).size() == 40;
  std::cout << "{\"schema\":1,\"source\":{";
  writeField(std::cout, "commit", GTI_COMPILER_SOURCE_COMMIT);
  std::cout << ',';
  writeField(std::cout, "state", GTI_COMPILER_SOURCE_STATE);
  std::cout << ",\"exact\":" << (exactSource ? "true" : "false")
            << "},\"build\":{";
  writeField(std::cout, "configuration", GTI_COVERAGE_BUILD_CONFIG);
  std::cout << ',';
  writeField(std::cout, "system", GTI_COVERAGE_SYSTEM_NAME);
  std::cout << ',';
  writeField(std::cout, "processor", GTI_COVERAGE_SYSTEM_PROCESSOR);
  std::cout << ',';
  writeField(std::cout, "compiler", GTI_COVERAGE_COMPILER);
  std::cout << ',';
  writeField(std::cout, "llvm", GTI_COVERAGE_LLVM_VERSION);
  std::cout << "},\"rows\":[";
  bool first = true;
  for (const lang::LlvmCoverageEntry &entry : lang::llvmCoverageLedger()) {
    if (!first)
      std::cout << ',';
    first = false;
    std::cout << '{';
    writeField(std::cout, "key", entry.key());
    std::cout << ",\"milestone\":" << entry.milestone << ',';
    writeField(std::cout, "state", stateName(entry.state));
    std::cout << ',';
    writeField(std::cout, "nonexecuting", ruleName(entry.nonExecuting));
    std::cout << ',';
    writeField(std::cout, "predicate", entry.predicate);
    std::cout << ",\"acceptance\":{";
    writeField(std::cout, "test", entry.positiveTest);
    std::cout << ',';
    writeField(std::cout, "assertion", entry.positiveAssertion);
    std::cout << ',';
    writeField(std::cout, "gap", entry.positiveEvidenceGap);
    std::cout << ",\"observed\":\"unrun\"},\"boundary\":{";
    writeField(std::cout, "test", entry.negativeTest);
    std::cout << ',';
    writeField(std::cout, "assertion", entry.negativeAssertion);
    std::cout << ",\"observed\":\"unrun\"}}";
  }
  std::cout << "]}\n";
  return 0;
}

void writeCoverageEntry(std::ostream &output,
                        const lang::LlvmCoverageEntry &entry) {
  output << '{';
  writeField(output, "key", entry.key());
  output << ',';
  writeField(output, "state", stateName(entry.state));
  output << ',';
  writeField(output, "nonexecuting", ruleName(entry.nonExecuting));
  output << ',';
  writeField(output, "predicate", entry.predicate);
  output << '}';
}

void writeTarget(std::ostream &output, const lang::TargetInfo &target) {
  output << '{';
  writeField(output, "os", target.os);
  output << ',';
  writeField(output, "vendor", target.vendor);
  output << ',';
  writeField(output, "arch", target.arch);
  output << ',';
  writeField(output, "endianness",
             lang::targetEndiannessName(target.dataLayout.endianness()));
  output << ",\"pointer_width_bits\":" << target.dataLayout.pointerWidthBits()
         << ',';
  writeField(output, "execution_profile",
             lang::executionProfileName(target.executionProfile));
  output << ",\"threads\":"
         << (target.runtimeCapabilities.threads ? "true" : "false") << '}';
}

void printDiagnostics(const lang::FrontendResult &frontend) {
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
}

std::optional<lang::VerifiedLoweredProgram>
lowerSource(const std::filesystem::path &root,
            const std::filesystem::path &source,
            lang::ExecutionProfile profile) {
  lang::FrontendOptions options;
  options.target = lang::TargetInfo::host();
  options.target.executionProfile = profile;
  options.target.runtimeCapabilities.threads =
      profile == lang::ExecutionProfile::Concurrent;
  const lang::TargetInfo target = options.target;
  options.lowerReachableOnly = true;
  const std::filesystem::path standardLibrary = root / "stdlib";
  const lang::FrontendResult frontend =
      lang::Frontend(std::move(options))
          .analyze(source, std::nullopt, {standardLibrary / "prelude.gti"}, {},
                   {standardLibrary});
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return std::nullopt;
  }
  lang::LoweredProgramBuild lowered = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, target, true);
  if (!lowered.valid()) {
    for (const lang::LoweredProgramIssue &issue : lowered.issues)
      std::cerr << issue.detail << '\n';
    return std::nullopt;
  }
  std::vector<lang::LoweredProgramIssue> issues;
  auto verified =
      lang::verifiedLoweredProgram(std::move(*lowered.program), issues);
  if (!verified) {
    for (const lang::LoweredProgramIssue &issue : issues)
      std::cerr << issue.detail << '\n';
    return std::nullopt;
  }
  return verified;
}

int probeSource(const std::filesystem::path &root,
                const std::filesystem::path &source) {
  auto verified =
      lowerSource(root, source, lang::ExecutionProfile::SingleThreaded);
  if (!verified)
    return 1;
  const lang::LlvmAdmissionPlan plan =
      lang::planLlvmAdmission(std::move(*verified));
  std::set<std::string> keys;
  for (const lang::LlvmCoverageUse &use : plan.requirements())
    if (use.entry) {
      keys.insert(use.entry->key());
      if (use.implementedShape)
        keys.insert(use.implementedShape->key());
    }
  for (const std::string &key : keys)
    std::cout << key << '\n';
  return 0;
}

void writeRequiredUses(std::ostream &output,
                       const lang::LlvmAdmissionPlan &plan) {
  const auto requirements = plan.requirements();
  const lang::LlvmCoverageUse *unsupported = plan.firstUnsupported();
  std::optional<std::size_t> unsupportedIndex;
  std::size_t requiredIndex = 0;
  std::size_t requiredCount = 0;
  for (const lang::LlvmCoverageUse &use : requirements)
    if (use.required) {
      if (&use == unsupported)
        unsupportedIndex = requiredIndex;
      ++requiredIndex;
      ++requiredCount;
    }

  output << "{\"schema\":\"gti.llvm.required-uses/v1\",\"compiler\":{";
  writeField(output, "commit", GTI_COMPILER_SOURCE_COMMIT);
  output << ',';
  writeField(output, "state", GTI_COMPILER_SOURCE_STATE);
  output << "},\"build\":{";
  writeField(output, "configuration", GTI_COVERAGE_BUILD_CONFIG);
  output << ',';
  writeField(output, "system", GTI_COVERAGE_SYSTEM_NAME);
  output << ',';
  writeField(output, "processor", GTI_COVERAGE_SYSTEM_PROCESSOR);
  output << ',';
  writeField(output, "compiler", GTI_COVERAGE_COMPILER);
  output << ',';
  writeField(output, "llvm", GTI_COVERAGE_LLVM_VERSION);
  output << "},\"target\":";
  writeTarget(output, plan.program().target());
  output << ',';
  writeField(output, "closure", closureName(plan.closure()));
  output << ",\"admitted\":" << (plan.admitted() ? "true" : "false")
         << ",\"first_unsupported\":";
  if (unsupportedIndex)
    output << *unsupportedIndex;
  else
    output << "null";
  output << ",\"use_count\":" << requiredCount << ",\"uses\":[";
  bool first = true;
  for (const lang::LlvmCoverageUse &use : requirements) {
    if (!use.required)
      continue;
    if (!first)
      output << ',';
    first = false;
    output << "{\"base\":";
    writeCoverageEntry(output, *use.entry);
    output << ",\"witness\":";
    if (use.implementedShape)
      writeCoverageEntry(output, *use.implementedShape);
    else
      output << "null";
    output << ",\"executable\":" << (use.executable ? "true" : "false")
           << ",\"body\":";
    if (use.body) {
      output << "{\"kind\":" << static_cast<unsigned>(use.body->kind)
             << ",\"owner\":" << use.body->owner << '}';
    } else {
      output << "null";
    }
    output << ",\"block\":" << use.block
           << ",\"instruction\":" << use.instruction
           << ",\"occurrences\":" << use.occurrences << ",\"source\":{";
    writeField(output, "path", use.source.source);
    output << ",\"start\":" << use.source.start << ",\"end\":" << use.source.end
           << ",\"line\":" << use.source.line << "}}";
  }
  output << "]}\n";
}

int probeUses(const std::filesystem::path &root,
              const std::filesystem::path &source,
              lang::ExecutionProfile profile) {
  auto verified = lowerSource(root, source, profile);
  if (!verified)
    return 1;
  const lang::LlvmAdmissionPlan plan =
      lang::planLlvmAdmission(std::move(*verified));
  writeRequiredUses(std::cout, plan);
  return 0;
}

std::string finishHash(llvm::SHA256 &hash) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (const auto byte : hash.final()) {
    result += hex[byte >> 4U];
    result += hex[byte & 15U];
  }
  return result;
}

std::string hashBytes(std::string_view bytes) {
  llvm::SHA256 hash;
  hash.update(llvm::StringRef(bytes.data(), bytes.size()));
  return finishHash(hash);
}

std::optional<std::string> hashFile(const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error))
    return std::nullopt;
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::nullopt;
  llvm::SHA256 hash;
  char bytes[65536];
  while (input) {
    input.read(bytes, sizeof(bytes));
    hash.update(llvm::StringRef(bytes, input.gcount()));
  }
  if (!input.eof())
    return std::nullopt;
  return finishHash(hash);
}

void writeHash(std::ostream &output, const std::optional<std::string> &hash) {
  if (hash)
    writeJsonString(output, *hash);
  else
    output << "null";
}

// This test backend only observes the canonical driver's detached program.
// Its successful metadata artifact is NOT LLVM code generation or admission.
class AdmissionObserver final : public lang::Backend {
public:
  std::string_view name() const override { return "test-admission-observer"; }

  lang::BackendArtifact generate(lang::LoweredProgram program) override {
    std::vector<lang::LoweredProgramIssue> issues;
    auto verified = lang::verifiedLoweredProgram(std::move(program), issues);
    if (!verified)
      throw std::runtime_error(
          "admission observer received invalid LoweredProgram");
    const auto plan = lang::planLlvmAdmission(std::move(*verified));
    std::ostringstream output;
    writeRequiredUses(output, plan);
    return {lang::BackendArtifactKind::Source, output.str(), ".json"};
  }
};

int observeCanonicalHost(const char *driver, const std::filesystem::path &root,
                         const std::filesystem::path &source,
                         lang::ExecutionProfile profile,
                         lang::OptimizationLevel optimization,
                         std::string_view optimizationName) {
  auto resources = lang::driver::discoverToolchainLayout(driver);
  resources.runtimeInclude =
      std::filesystem::weakly_canonical(resources.runtimeInclude);
  resources.runtimeLibrary =
      std::filesystem::weakly_canonical(resources.runtimeLibrary);
  resources.standardLibrary = lang::standardLibraryLayout(root / "stdlib");
  const lang::driver::AnalysisConfiguration configuration(
      lang::TargetInfo::host(), profile, lang::driver::RuntimeProfile::Hosted);
  const auto request =
      lang::driver::CompilationRequest(
          source, resources.standardLibrary, configuration.target(),
          optimization, lang::CppStandard::Cpp23, {},
          configuration.configurationFlags(), {}, {}, false, {},
          lang::RawPointerAuditMode::Off, {},
          configuration.declaredConfigurationFlags())
          .forExecutable()
          .withDefinitionClosure()
          .withBackend(lang::BackendKind::Llvm);
  const auto identity = lang::driver::currentCompilerBuildIdentity();
  const bool identityMatches =
      lang::driver::compilerSourceStateName(identity.state) ==
          GTI_COMPILER_SOURCE_STATE &&
      identity.commit.value_or("") == GTI_COMPILER_SOURCE_COMMIT;

  // Files never consumed by this no-link request are execution obligations,
  // not prerequisites for observing valid semantic/MIR admission facts.
  std::vector<std::filesystem::path> paths{
      source, resources.standardLibrary.prelude,
      std::filesystem::weakly_canonical(driver), resources.runtimeLibrary};
  // Bound this to the runtime API files, not an arbitrary configured include
  // tree (which may be a system SDK). The public validator owns availability.
  for (const auto *header :
       {"runtime.hpp", "runtime.h", "c_abi.h", "runtime_failure.h",
        "runtime_thread.h", "runtime_thread.hpp"})
    paths.push_back(resources.runtimeInclude / "gti" / header);
  std::vector<std::optional<std::string>> before;
  for (const auto &path : paths)
    before.push_back(hashFile(path));
  const bool setupReady =
      identityMatches && before[0] && before[1] && before[2];
  const bool executionResourcesPresent =
      !lang::driver::validateToolchainLayout(resources);
  lang::driver::CompilationResult result;
  if (setupReady) {
    AdmissionObserver observer;
    result = lang::driver::compileWithBackend(request, observer);
  }
  std::vector<std::optional<std::string>> after;
  for (const auto &path : paths)
    after.push_back(hashFile(path));
  bool loadedStable = true;
  auto names = result.sources.names();
  std::sort(names.begin(), names.end());
  for (const auto &name : names)
    if (const auto *bytes = result.sources.find(name))
      loadedStable &= hashFile(name) == hashBytes(*bytes);
  const bool requiredStable =
      loadedStable &&
      std::equal(before.begin(), before.begin() + 3, after.begin());
  const bool executionStable =
      std::equal(before.begin() + 3, before.end(), after.begin() + 3);
  std::string_view status = "unresolved-setup";
  if (setupReady && requiredStable) {
    switch (result.status) {
    case lang::driver::CompilationStatus::Success:
      status = "observed";
      break;
    case lang::driver::CompilationStatus::FrontendFailure:
      status = "frontend-failure";
      break;
    case lang::driver::CompilationStatus::MirVerificationFailure:
      status = "mir-failure";
      break;
    case lang::driver::CompilationStatus::BackendFailure:
      status = "observer-failure";
      break;
    }
  }
  std::cout << "{\"schema\":\"gti.llvm.canonical-host-admission/v1\",";
  writeField(std::cout, "mode", "canonical-repository-actual-host/v1");
  std::cout << ",\"compiler\":{";
  writeField(std::cout, "commit", GTI_COMPILER_SOURCE_COMMIT);
  std::cout << ',';
  writeField(std::cout, "state", GTI_COMPILER_SOURCE_STATE);
  std::cout << "},\"build\":{";
  writeField(std::cout, "configuration", GTI_COVERAGE_BUILD_CONFIG);
  std::cout << ',';
  writeField(std::cout, "system", GTI_COVERAGE_SYSTEM_NAME);
  std::cout << ',';
  writeField(std::cout, "processor", GTI_COVERAGE_SYSTEM_PROCESSOR);
  std::cout << ',';
  writeField(std::cout, "compiler", GTI_COVERAGE_COMPILER);
  std::cout << ',';
  writeField(std::cout, "llvm", GTI_COVERAGE_LLVM_VERSION);
  std::cout << '}';
  std::cout << ',';
  writeField(std::cout, "status", status);
  std::cout << ",\"driver_identity_matches\":"
            << (identityMatches ? "true" : "false")
            << ",\"required_inputs_stable\":"
            << (requiredStable ? "true" : "false")
            << ",\"resources_stable\":" << (before == after ? "true" : "false")
            << ",\"request\":{";
  writeField(std::cout, "entry", source.string());
  std::cout << ',';
  writeField(std::cout, "purpose", "executable-admission-only");
  std::cout << ',';
  writeField(std::cout, "optimization", optimizationName);
  std::cout << ",\"backend\":\"llvm\",\"cpp_standard\":\"c++23\","
               "\"runtime_profile\":\"hosted\",\"entry_required\":"
            << (request.requiresEntryPoint() ? "true" : "false")
            << ",\"definition_closure_required\":"
            << (request.requiresFunctionDefinitions() ? "true" : "false")
            << ",\"debug_info\":false,\"configuration_flags\":[";
  bool first = true;
  for (const auto &flag : request.configurationFlags()) {
    if (!first)
      std::cout << ',';
    first = false;
    writeJsonString(std::cout, flag);
  }
  std::cout << "],\"declared_configuration_flags\":[],\"target\":";
  writeTarget(std::cout, request.target());
  std::cout << "},\"standard_library\":{";
  writeField(std::cout, "policy", "explicit-checked-out-repository");
  std::cout << ',';
  writeField(std::cout, "root", resources.standardLibrary.root.string());
  std::cout << ',';
  writeField(std::cout, "prelude", resources.standardLibrary.prelude.string());
  std::cout << "},\"execution_resources\":{";
  writeField(std::cout, "runtime_include", resources.runtimeInclude.string());
  std::cout << ',';
  writeField(std::cout, "runtime_library", resources.runtimeLibrary.string());
  std::cout << ",\"layout_valid\":"
            << (executionResourcesPresent ? "true" : "false")
            << ",\"identity_stable\":" << (executionStable ? "true" : "false")
            << ",\"consumed\":false,\"execution\":\"unrun\"},\"resource_"
               "snapshots\":[";
  for (std::size_t i = 0; i < paths.size(); ++i) {
    if (i)
      std::cout << ',';
    std::cout << '{';
    writeField(std::cout, "path", paths[i].string());
    std::cout << ",\"before_sha256\":";
    writeHash(std::cout, before[i]);
    std::cout << ",\"after_sha256\":";
    writeHash(std::cout, after[i]);
    std::cout << '}';
  }
  std::cout << "],\"loaded_sources\":[";
  first = true;
  for (const auto &name : names) {
    const auto *bytes = result.sources.find(name);
    if (!bytes)
      continue;
    if (!first)
      std::cout << ',';
    first = false;
    std::cout << '{';
    writeField(std::cout, "path", name);
    std::cout << ',';
    writeField(std::cout, "sha256", hashBytes(*bytes));
    std::cout << ",\"bytes\":" << bytes->size() << '}';
  }
  std::cout << "],\"diagnostics\":[";
  first = true;
  for (const auto &diagnostic : result.diagnostics) {
    if (!first)
      std::cout << ',';
    first = false;
    std::cout << '{';
    writeField(std::cout, "code", diagnostic.code);
    std::cout << ',';
    writeField(std::cout, "message", diagnostic.message);
    std::cout << ',';
    writeField(std::cout, "source", diagnostic.primary.source);
    std::cout << ",\"start\":" << diagnostic.primary.start
              << ",\"end\":" << diagnostic.primary.end
              << ",\"phase\":" << static_cast<unsigned>(diagnostic.phase)
              << ",\"severity\":" << static_cast<unsigned>(diagnostic.severity)
              << '}';
  }
  std::cout << "],\"mir_error_count\":" << result.mirErrors.size()
            << ",\"observer_error\":";
  if (result.backendFailureDetails)
    writeJsonString(std::cout, *result.backendFailureDetails);
  else
    std::cout << "null";
  std::cout << ",\"native_inputs_linked\":false,\"code_emission\":\"unrun\","
               "\"execution\":\"unrun\",\"uses\":";
  if (result.succeeded() && status == "observed")
    std::cout << result.artifact->contents;
  else
    std::cout << "null";
  std::cout << "}\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--canonical-host-target") {
    const auto profile = lang::parseExecutionProfile(argv[2]);
    if (!profile)
      return 2;
    writeTarget(std::cout, lang::driver::AnalysisConfiguration(
                               lang::TargetInfo::host(), *profile,
                               lang::driver::RuntimeProfile::Hosted)
                               .target());
    std::cout << '\n';
    return 0;
  }
  if (argc == 6 && std::string_view(argv[1]) == "--observe-canonical-host") {
    const auto profile = lang::parseExecutionProfile(argv[4]);
    const std::string_view level = argv[5];
    if (!profile || (level != "O0" && level != "O3")) {
      std::cerr << "canonical-host observation requires a valid profile and O0 "
                   "or O3\n";
      return 2;
    }
    return observeCanonicalHost(
        argv[0], std::filesystem::weakly_canonical(argv[2]),
        std::filesystem::weakly_canonical(argv[3]), *profile,
        level == "O0" ? lang::OptimizationLevel::O0
                      : lang::OptimizationLevel::O3,
        level);
  }
  if (argc == 2 && std::string_view(argv[1]) == "--ledger")
    return writeLedger();
  if (argc == 4 && std::string_view(argv[1]) == "--probe")
    return probeSource(std::filesystem::weakly_canonical(argv[2]),
                       std::filesystem::weakly_canonical(argv[3]));
  if (argc == 5 && std::string_view(argv[1]) == "--probe-uses") {
    const auto profile = lang::parseExecutionProfile(argv[4]);
    if (!profile) {
      std::cerr << "invalid execution profile: " << argv[4] << '\n';
      return 2;
    }
    return probeUses(std::filesystem::weakly_canonical(argv[2]),
                     std::filesystem::weakly_canonical(argv[3]), *profile);
  }
  std::cerr << "usage: gti_llvm_coverage_evidence --ledger | --probe "
               "<repo-root> <source> | --probe-uses <repo-root> <source> "
               "<single-threaded|concurrent> | --observe-canonical-host "
               "<repo-root> <source> <single-threaded|concurrent> <O0|O3> | "
               "--canonical-host-target <single-threaded|concurrent>\n";
  return 2;
}
