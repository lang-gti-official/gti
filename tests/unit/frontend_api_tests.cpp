// Frontend library API checks with no source-level observable: executable
// and standard-library discovery, stop phases, target triples, support
// facilities and test-suite analysis options. (Moved unchanged from
// compiler_frontend_tests.cpp; the GTI-source cases became tests/ui files.)
#include "../temporary_directory.h"
#include "gti/executable_path.h"
#include "gti/frontend.h"
#include "gti/standard_library.h"
#include "gti/support.h"
#include "unit_test.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

[[maybe_unused]] bool
hasDiagnostic(const std::vector<lang::Diagnostic> &diagnostics,
              const std::string &text) {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.message.find(text) !=
                              std::string::npos;
                     });
}

[[maybe_unused]] bool
hasDiagnosticCode(const std::vector<lang::Diagnostic> &diagnostics,
                  const std::string &code) {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [&](const lang::Diagnostic &diagnostic) {
                       return diagnostic.code == code;
                     });
}

[[maybe_unused]] const lang::Diagnostic *
findDiagnosticByCode(const std::vector<lang::Diagnostic> &diagnostics,
                     const std::string &code) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code)
      return &diagnostic;
  }
  return nullptr;
}

[[maybe_unused]] std::size_t
countDiagnosticCode(const std::vector<lang::Diagnostic> &diagnostics,
                    const std::string &code) {
  return static_cast<std::size_t>(
      std::count_if(diagnostics.begin(), diagnostics.end(),
                    [&](const lang::Diagnostic &diagnostic) {
                      return diagnostic.code == code;
                    }));
}

[[maybe_unused]] bool
hasRelatedDiagnostic(const std::vector<lang::Diagnostic> &diagnostics,
                     const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    for (const lang::RelatedDiagnostic &related : diagnostic.related) {
      if (related.message.find(text) != std::string::npos)
        return true;
    }
  }
  return false;
}

[[maybe_unused]] bool
hasDiagnosticHint(const std::vector<lang::Diagnostic> &diagnostics,
                  const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    for (const std::string &hint : diagnostic.hints) {
      if (hint.find(text) != std::string::npos)
        return true;
    }
  }
  return false;
}

[[maybe_unused]] std::filesystem::path standardLibraryPrelude() {
  return std::filesystem::path(__FILE__)
             .parent_path()
             .parent_path()
             .parent_path() /
         "stdlib/prelude.gti";
}

[[maybe_unused]] std::filesystem::path standardLibraryRoot() {
  return standardLibraryPrelude().parent_path();
}

void testExecutablePathDiscovery() {
  const std::filesystem::path executable =
      lang::executablePath("not-the-running-test-binary");
  std::error_code error;
  expect(executable.is_absolute() &&
             std::filesystem::is_regular_file(executable, error),
         "native executable discovery should not depend on argv[0]");

  const lang::StandardLibraryLayout rootLayout =
      lang::standardLibraryLayout("/toolchain/share/gti/stdlib");
  const lang::StandardLibraryLayout legacyLayout =
      lang::standardLibraryLayout("/custom/prelude.gti");
  expect(rootLayout.root == "/toolchain/share/gti/stdlib" &&
             rootLayout.prelude == "/toolchain/share/gti/stdlib/prelude.gti" &&
             legacyLayout.root == "/custom" &&
             legacyLayout.prelude == "/custom/prelude.gti",
         "standard-library discovery should accept roots and legacy prelude "
         "paths");
}

void testFrontendStopPhase() {
  const std::string source = "int main() { return 0; }\n";

  lang::FrontendOptions semanticsOnly;
  semanticsOnly.stopAfter = lang::FrontendPhase::Semantics;
  const lang::FrontendResult stopped =
      lang::Frontend(semanticsOnly).analyze("stop-semantics.gti", source);
  expect(stopped.sourceValid && stopped.syntaxValid && stopped.semanticValid,
         "a semantics-only analysis should validate the frontend phases it "
         "runs");
  expect(stopped.semantics.expressionCount() > 0,
         "a semantics-only analysis should retain the semantic model");
  expect(!stopped.hirValid && !stopped.mirValid,
         "a semantics-only analysis must not mark HIR or MIR valid");
  expect(stopped.hir.functionInstances().empty(),
         "a semantics-only analysis must not lower HIR instances");
  expect(!stopped.canGenerateCode(),
         "a semantics-only analysis must leave code generation disabled");

  lang::FrontendOptions hirOnly;
  hirOnly.stopAfter = lang::FrontendPhase::Hir;
  const lang::FrontendResult hirStopped =
      lang::Frontend(hirOnly).analyze("stop-hir.gti", source);
  expect(hirStopped.hirValid && !hirStopped.mirValid &&
             !hirStopped.canGenerateCode(),
         "a HIR-stopped analysis should validate HIR but not MIR");

  const lang::FrontendResult full =
      lang::Frontend().analyze("stop-full.gti", source);
  expect(full.canGenerateCode(),
         "the default stop phase must keep the full pipeline");
}

void testTargetTripleParsing() {
  const lang::TargetInfo host = lang::TargetInfo::host();
  expect(host.dataLayout.supported() &&
             host.dataLayout.pointerWidthBits() == 64 &&
             host.dataLayout.littleEndian() &&
             host.executionProfile == lang::ExecutionProfile::SingleThreaded,
         "the host target should commit to the supported 64-bit "
         "little-endian layout and default single-threaded profile");
  expect(lang::parseExecutionProfile("single-threaded") ==
                 lang::ExecutionProfile::SingleThreaded &&
             lang::parseExecutionProfile("concurrent") ==
                 lang::ExecutionProfile::Concurrent &&
             !lang::parseExecutionProfile("threads") &&
             lang::executionProfileName(lang::ExecutionProfile::Concurrent) ==
                 "concurrent",
         "execution profiles should use one exact compiler-owned vocabulary");
  expect(lang::parseTargetProperty("os") == lang::TargetProperty::Os &&
             lang::parseTargetProperty("vendor") ==
                 lang::TargetProperty::Vendor &&
             lang::parseTargetProperty("arch") == lang::TargetProperty::Arch &&
             !lang::parseTargetProperty("platform") &&
             lang::targetPropertyName(lang::TargetProperty::Arch) == "arch" &&
             lang::targetEndiannessName(lang::TargetEndianness::Little) ==
                 "little" &&
             lang::targetScalarKindName(lang::TargetScalarKind::Float32) ==
                 "float" &&
             lang::targetScalarKindName(lang::TargetScalarKind::Float64) ==
                 "double",
         "target conditions and scalar facts should use one exact compiler-"
         "owned vocabulary");

  if (!lang::targetTripleParsingAvailable()) {
    expect(!lang::parseTargetTriple("arm64-apple-macos").has_value(),
           "triple parsing must decline cleanly when unavailable");
    return;
  }

  const std::optional<lang::TargetInfo> mac =
      lang::parseTargetTriple("aarch64-apple-darwin");
  expect(mac && mac->arch == "arm64" && mac->vendor == "apple" &&
             mac->os == "macos" && mac->dataLayout.supported() &&
             mac->dataLayout.pointerWidthBits() == 64 &&
             mac->dataLayout.endianness() == lang::TargetEndianness::Little,
         "an aarch64 darwin triple should normalize into GTI's vocabulary");
  const std::optional<lang::TargetInfo> macAlias =
      lang::parseTargetTriple("arm64-apple-macos");
  expect(macAlias && macAlias->arch == "arm64" && macAlias->os == "macos",
         "arm64 and aarch64 spellings should normalize identically");
  const std::optional<lang::TargetInfo> linux64 =
      lang::parseTargetTriple("x86_64-unknown-linux-gnu");
  expect(linux64 && linux64->arch == "x86_64" && linux64->os == "linux" &&
             linux64->vendor == "unknown",
         "a linux gnu triple should map to GTI's linux target");
  const std::optional<lang::TargetInfo> windows =
      lang::parseTargetTriple("x86_64-pc-windows-msvc");
  expect(windows && windows->os == "windows" && windows->vendor == "pc" &&
             windows->dataLayout == mac->dataLayout,
         "a windows msvc triple should map to GTI's windows target");

  const auto scalar = [](const lang::TargetInfo &target,
                         lang::TargetScalarKind kind, std::uint32_t size,
                         std::uint32_t alignment) {
    const std::optional<lang::TargetTypeLayout> layout =
        target.dataLayout.scalarLayout(kind);
    return layout && layout->sizeBytes == size &&
           layout->abiAlignmentBytes == alignment &&
           layout->preferredAlignmentBytes == alignment;
  };
  expect(scalar(*mac, lang::TargetScalarKind::Bool, 1, 1) &&
             scalar(*mac, lang::TargetScalarKind::Char, 1, 1) &&
             scalar(*mac, lang::TargetScalarKind::Int16, 2, 2) &&
             scalar(*mac, lang::TargetScalarKind::UInt32, 4, 4) &&
             scalar(*mac, lang::TargetScalarKind::Int64, 8, 8) &&
             scalar(*mac, lang::TargetScalarKind::Float32, 4, 4) &&
             scalar(*mac, lang::TargetScalarKind::Float64, 8, 8) &&
             scalar(*mac, lang::TargetScalarKind::Pointer, 8, 8),
         "supported triples should expose deterministic GTI-owned scalar "
         "layout facts");

  const lang::TargetTripleParseResult narrow =
      lang::parseTargetTripleResult("i386-pc-windows-msvc");
  const lang::TargetTripleParseResult bigEndian =
      lang::parseTargetTripleResult("aarch64_be-unknown-linux-gnu");
  const lang::TargetTripleParseResult unsupportedOs =
      lang::parseTargetTripleResult("x86_64-unknown-freebsd");
  const lang::TargetTripleParseResult malformed =
      lang::parseTargetTripleResult("banana");
  expect(!narrow.succeeded() &&
             narrow.error == lang::TargetTripleError::UnsupportedArchitecture &&
             !bigEndian.succeeded() &&
             bigEndian.error ==
                 lang::TargetTripleError::UnsupportedEndianness &&
             !unsupportedOs.succeeded() &&
             unsupportedOs.error ==
                 lang::TargetTripleError::UnsupportedOperatingSystem &&
             !malformed.succeeded() &&
             malformed.error == lang::TargetTripleError::Malformed,
         "triple parsing should distinguish malformed and unsupported target "
         "properties");
  expect(
      lang::targetTripleErrorMessage(bigEndian.error).find("little-endian") !=
          std::string_view::npos,
      "triple rejection should expose an actionable stable reason");

  lang::FrontendOptions unsupportedOptions;
  unsupportedOptions.target = {"unsupported", "unknown", "unknown",
                               lang::TargetDataLayout::unsupported()};
  const lang::FrontendResult unsupported =
      lang::Frontend(unsupportedOptions)
          .analyze("unsupported-target.gti", "int main() { return 0; }");
  const lang::Diagnostic *layoutDiagnostic =
      findDiagnosticByCode(unsupported.diagnostics, "GTI-S2062");
  expect(unsupported.sourceValid && !unsupported.canGenerateCode() &&
             layoutDiagnostic != nullptr &&
             layoutDiagnostic->phase == lang::DiagnosticPhase::Semantics &&
             layoutDiagnostic->primary.source.ends_with(
                 "unsupported-target.gti") &&
             layoutDiagnostic->primary.start == 0 &&
             layoutDiagnostic->primary.end == 0 &&
             layoutDiagnostic->message.find("no supported GTI data layout") !=
                 std::string::npos &&
             layoutDiagnostic->hints.size() == 1 &&
             layoutDiagnostic->hints.front().find("64-bit little-endian") !=
                 std::string::npos,
         "an unsupported selected layout should fail before parsing or code "
         "generation with one source-facing configuration diagnostic");
}

void testSupportFacilities() {
  std::filesystem::path retired;
  const gti_test::TemporaryDirectory survivor("gti-same-prefix");
  std::ofstream(survivor.root() / "fixture") << "survivor";
  {
    const gti_test::TemporaryDirectory scoped("gti-same-prefix");
    retired = scoped.root();
    expect(retired != survivor.root() && std::filesystem::is_directory(retired),
           "same-prefix fixture directories have separate ownership");
    std::ofstream(retired / "fixture") << "scoped";
  }
  expect(!std::filesystem::exists(retired) &&
             std::filesystem::exists(survivor.root() / "fixture"),
         "fixture cleanup removes only its own directory");
  try {
    const gti_test::TemporaryDirectory unwound("gti-unwind");
    retired = unwound.root();
    std::ofstream(retired / "fixture") << "unwound";
    throw 1;
  } catch (int) {
  }
  expect(!std::filesystem::exists(retired),
         "fixture directories are removed during exception unwinding");

  bool ran = false;
  expect(lang::runGuarded([&ran] { ran = true; }) && ran,
         "guarded execution should run work and report success");
  expect(!lang::endTimeTrace("unused.json"),
         "ending a time trace that was never started must fail cleanly");
  expect(lang::timeTraceAvailable() == lang::targetTripleParsingAvailable(),
         "LLVM-backed facilities should agree on availability");

  // Recursion far deeper than any thread stack continues on fresh,
  // guard-paged segments of the same thread.
  struct Deep {
    static std::size_t descend(std::size_t remaining) {
      volatile char frame[512] = {};
      frame[0] = static_cast<char>(remaining & 1);
      if (remaining == 0)
        return 0;
      return lang::withSufficientStack(
                 [remaining] { return descend(remaining - 1); }) +
             static_cast<std::size_t>(static_cast<std::size_t>(frame[0]) ==
                                      (remaining & 1));
    }
  };
  expect(Deep::descend(100000) == 100000,
         "boundary-guarded recursion completes far beyond one thread stack");
#if defined(__unix__) || defined(__APPLE__)
  // The calling thread's stack depends on the launcher (CTest raises the
  // soft stack limit to the hard one, 64 MiB on macOS, which this recursion
  // fits), so prove the switch on a thread whose stack is explicitly small.
  struct Probe {
    std::size_t result = 0;
    std::size_t segments = 0;
  };
  Probe probe;
  pthread_attr_t attributes;
  pthread_t thread;
  const bool threaded =
      pthread_attr_init(&attributes) == 0 &&
      pthread_attr_setstacksize(&attributes, std::size_t{2} << 20) == 0 &&
      pthread_create(
          &thread, &attributes,
          [](void *raw) -> void * {
            auto &observed = *static_cast<Probe *>(raw);
            const std::size_t before = lang::nativeStackSegmentsEntered();
            observed.result = Deep::descend(100000);
            observed.segments = lang::nativeStackSegmentsEntered() - before;
            return nullptr;
          },
          &probe) == 0 &&
      pthread_join(thread, nullptr) == 0;
  pthread_attr_destroy(&attributes);
  expect(threaded && probe.result == 100000 && probe.segments > 0,
         "recursion beyond a 2 MiB thread stack really ran on additional "
         "stack segments");
#endif
}

void testAnnotatedSuiteAnalysis() {
  const std::string source = "namespace app { [[test]] void addition() {} }\n";
  auto cache = std::make_shared<lang::FrontendCache>();
  for (const bool synthesize : {false, true, false, true}) {
    lang::FrontendOptions options;
    options.program.testSuite = true;
    options.stopAfter = lang::FrontendPhase::Semantics;
    options.requireEntryPoint = true;
    options.synthesizeTestEntry = synthesize;
    options.cache = cache;
    const auto result = lang::Frontend(options).analyze(
        "suite-analysis.gti", source,
        {standardLibraryPrelude(), standardLibraryRoot() / "std/string.gti",
         standardLibraryRoot() / "std/vector.gti"},
        {}, {standardLibraryRoot()});
    expect(result.semanticValid && result.diagnostics.empty() &&
               result.testCases == std::vector<std::string>{"app::addition"},
           "source-only and generated test analysis retain validated cases "
           "when alternating with a shared syntax cache");
    const auto *entry =
        result.sourceGraph.findUnit(result.sourceGraph.entryUnit());
    expect(entry != nullptr, "test analysis retains its source entry unit");
    if (entry == nullptr) {
      continue;
    }
    const auto *text = result.sources.find(entry->path.string());
    expect(text != nullptr && ((*text == source) != synthesize),
           "source-only test analysis preserves the exact written text");
    expect(result.semantics
                   .executableEntryDiagnostic(entry->id, entry->path.string())
                   .has_value() != synthesize,
           "only artifact requests synthesize a hosted main");
  }
  lang::FrontendOptions executable;
  executable.stopAfter = lang::FrontendPhase::Semantics;
  executable.requireEntryPoint = true;
  executable.synthesizeTestEntry = false;
  const auto missing =
      lang::Frontend(executable).analyze("missing-main.gti", source);
  expect(
      !missing.semanticValid &&
          hasDiagnosticCode(missing.diagnostics, "GTI-S2094"),
      "source-only test dispatch policy never exempts an ordinary executable");
}

} // namespace

GTI_UNIT_TEST(frontend_api) {
  testExecutablePathDiscovery();
  testFrontendStopPhase();
  testTargetTripleParsing();
  testSupportFacilities();
  testAnnotatedSuiteAnalysis();
  return failures == 0 ? 0 : 1;
}
