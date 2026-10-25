#include "gti/build_identity_config.h"
#include "gti/frontend.h"
#include "gti/llvm_admission.h"
#include "gti/lowered_program_builder.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
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

int probeUses(const std::filesystem::path &root,
              const std::filesystem::path &source,
              lang::ExecutionProfile profile) {
  auto verified = lowerSource(root, source, profile);
  if (!verified)
    return 1;
  const lang::LlvmAdmissionPlan plan =
      lang::planLlvmAdmission(std::move(*verified));
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

  std::cout << "{\"schema\":\"gti.llvm.required-uses/v1\",\"compiler\":{";
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
  std::cout << "},\"target\":";
  writeTarget(std::cout, plan.program().target());
  std::cout << ',';
  writeField(std::cout, "closure", closureName(plan.closure()));
  std::cout << ",\"admitted\":" << (plan.admitted() ? "true" : "false")
            << ",\"first_unsupported\":";
  if (unsupportedIndex)
    std::cout << *unsupportedIndex;
  else
    std::cout << "null";
  std::cout << ",\"use_count\":" << requiredCount << ",\"uses\":[";
  bool first = true;
  for (const lang::LlvmCoverageUse &use : requirements) {
    if (!use.required)
      continue;
    if (!first)
      std::cout << ',';
    first = false;
    std::cout << "{\"base\":";
    writeCoverageEntry(std::cout, *use.entry);
    std::cout << ",\"witness\":";
    if (use.implementedShape)
      writeCoverageEntry(std::cout, *use.implementedShape);
    else
      std::cout << "null";
    std::cout << ",\"executable\":" << (use.executable ? "true" : "false")
              << ",\"body\":";
    if (use.body) {
      std::cout << "{\"kind\":" << static_cast<unsigned>(use.body->kind)
                << ",\"owner\":" << use.body->owner << '}';
    } else {
      std::cout << "null";
    }
    std::cout << ",\"block\":" << use.block
              << ",\"instruction\":" << use.instruction
              << ",\"occurrences\":" << use.occurrences << ",\"source\":{";
    writeField(std::cout, "path", use.source.source);
    std::cout << ",\"start\":" << use.source.start
              << ",\"end\":" << use.source.end
              << ",\"line\":" << use.source.line << "}}";
  }
  std::cout << "]}\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
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
               "<single-threaded|concurrent>\n";
  return 2;
}
