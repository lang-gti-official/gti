#include "gti/frontend.h"
#include "gti/optimizer.h"

#include "llvm_backend_test_support.h"
#include "lowered_program_test_support.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

bool sameFailureMetadata(const lang::FailureMetadata &left,
                         const lang::FailureMetadata &right) {
  return left.artifactIdentity() == right.artifactIdentity() &&
         left.descriptorBytes() == right.descriptorBytes() &&
         left.sites() == right.sites();
}

bool hasOutcome(const lang::FailureMetadata &metadata,
                lang::DefinedFailureCode code,
                lang::DefinedFailureDetail detail) {
  const lang::DefinedFailureOutcome wanted{.code = code, .detail = detail};
  return std::any_of(
      metadata.sites().begin(), metadata.sites().end(), [&](const auto &site) {
        return std::find(site.outcomes.begin(), site.outcomes.end(), wanted) !=
               site.outcomes.end();
      });
}

void testExactDescriptorEmission(const std::filesystem::path &fixture) {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(fixture.string(), readFile(fixture));
  expect(frontend.canGenerateCode(),
         "the descriptor-emission fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::FailureMetadata &metadata = frontend.mir.failureMetadata();
  expect(lang::verifyFailureMetadata(metadata).valid() &&
             metadata.artifactIdentity() ==
                 frontend.failureMetadata.artifactIdentity() &&
             metadata.descriptorBytes() ==
                 frontend.failureMetadata.descriptorBytes() &&
             metadata.sites() == frontend.failureMetadata.sites(),
         "MIR should retain the exact canonical frontend failure snapshot");
  expect(metadata.sites().size() == 4,
         "the focused fixture should contain four canonical sites");
  expect(!metadata.descriptorBytes().empty() &&
             !metadata.artifactIdentity().isZero(),
         "the canonical snapshot should carry descriptor bytes and a "
         "non-zero artifact identity");

  // Two sites come from one source and two from the imported source; two
  // three sites share the addition outcome shape. Equal facts stay equal and
  // distinct facts stay distinct in the canonical site table.
  std::vector<std::string> sources;
  std::vector<std::vector<lang::DefinedFailureOutcome>> outcomeShapes;
  for (const lang::FailureSiteDescriptor &site : metadata.sites()) {
    expect(site.end > site.start && site.line > 0 && !site.outcomes.empty(),
           "a site should retain line/span facts and its outcomes");
    if (std::find(sources.begin(), sources.end(), site.logicalSource) ==
        sources.end()) {
      sources.push_back(site.logicalSource);
    }
    if (std::find(outcomeShapes.begin(), outcomeShapes.end(), site.outcomes) ==
        outcomeShapes.end()) {
      outcomeShapes.push_back(site.outcomes);
    }
  }
  expect(sources.size() == 2,
         "distinct sources remain distinct; duplicates are equal");
  expect(outcomeShapes.size() == 2,
         "distinct outcome shapes remain distinct; duplicates are equal");
  expect(hasOutcome(metadata, lang::DefinedFailureCode::IntegerOverflow,
                    lang::DefinedFailureDetail::Addition) &&
             hasOutcome(metadata, lang::DefinedFailureCode::IntegerOverflow,
                        lang::DefinedFailureDetail::Division) &&
             hasOutcome(metadata, lang::DefinedFailureCode::DivisionByZero,
                        lang::DefinedFailureDetail::IntegerDivision),
         "canonical sites should preserve their ABI-v1 outcomes");

  // Every optimization level hands the backend the exact same canonical
  // descriptor, and the shipped backend accepts each lowered program.
  for (const lang::OptimizationLevel level :
       {lang::OptimizationLevel::O0, lang::OptimizationLevel::O1,
        lang::OptimizationLevel::O3}) {
    const lang::OptimizedProgram optimized =
        gti_test::optimizeProgram(frontend, level);
    const lang::LoweredProgram lowered = gti_test::lowerProgram(
        frontend, optimized.sourceMir, optimized.mir, lang::TargetInfo::host(),
        optimized.report.transformations);
    expect(sameFailureMetadata(optimized.mir.failureMetadata(), metadata) &&
               sameFailureMetadata(lowered.mir().failureMetadata(), metadata),
           "O0/O1/O3 must lower byte-identical failure descriptors");
    std::string error;
    expect(
        gti_test::llvmEmits(frontend, level, lang::TargetInfo::host(), &error),
        "the shipped backend should emit the descriptor fixture: " + error);
  }
}

void testEmptyDescriptorEmission() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "/tmp/gti-empty-descriptor/main.gti", "int main() { return 0; }");
  expect(frontend.canGenerateCode() &&
             frontend.mir.failureMetadata().sites().empty(),
         "the empty-descriptor fixture should have no failure sites");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::LoweredProgram lowered =
      gti_test::lowerOptimizedProgram(frontend, lang::OptimizationLevel::O0);
  expect(!frontend.failureMetadata.descriptorBytes().empty() &&
             lowered.mir().failureMetadata().sites().empty() &&
             sameFailureMetadata(lowered.mir().failureMetadata(),
                                 frontend.failureMetadata),
         "an empty artifact should retain canonical bytes with no sites");
  std::string error;
  expect(gti_test::llvmEmits(frontend, lang::OptimizationLevel::O0,
                             lang::TargetInfo::host(), &error),
         "the shipped backend should emit an empty descriptor: " + error);
}

void testMetadataDriftIsRejected(const std::filesystem::path &fixture) {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(fixture.string(), readFile(fixture));
  if (!frontend.canGenerateCode()) {
    expect(false, "the metadata mutation fixture should pass the frontend");
    return;
  }
  lang::MirProgram drifted = frontend.mir;
  auto &bytes = const_cast<std::vector<std::uint8_t> &>(
      drifted.failureMetadata().descriptorBytes());
  expect(!bytes.empty(),
         "the mutation fixture should contain canonical descriptor bytes");
  if (bytes.empty()) {
    return;
  }
  bytes.front() ^= UINT8_C(0xff);
  expect(!lang::verifyFailureMetadata(drifted.failureMetadata()).valid(),
         "canonical descriptor drift should fail metadata verification");

  bool rejected = false;
  std::string message;
  try {
    (void)gti_test::lowerProgram(frontend, frontend.mir, drifted);
  } catch (const std::logic_error &error) {
    rejected = true;
    message = error.what();
  }
  expect(rejected &&
             message.find("test fixture did not produce a lowered program") !=
                 std::string::npos &&
             message.find("invalid failure metadata") != std::string::npos,
         "LoweredProgram construction should reject descriptor drift before "
         "backend emission");
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: gti_failure_descriptor_backend_tests <fixture>\n";
    return 2;
  }
  testExactDescriptorEmission(argv[1]);
  testEmptyDescriptorEmission();
  testMetadataDriftIsRejected(argv[1]);
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  return 0;
}
