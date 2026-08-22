#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
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

lang::BackendArtifact emit(const lang::FrontendResult &frontend,
                           const lang::MirProgram &mir,
                           const lang::OptimizationResult &compatibility,
                           lang::CppStandard standard) {
  static_cast<void>(compatibility);
  return gti_test::emitCpp(frontend, frontend.mir, mir, standard);
}

std::string expectedByteArray(std::string_view name,
                              const std::vector<std::uint8_t> &bytes) {
  std::ostringstream output;
  output << "[[maybe_unused]] static constexpr std::uint8_t " << name
         << "[] = {";
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    output << (index % 12 == 0 ? "\n  " : " ")
           << static_cast<unsigned int>(bytes[index]);
    if (index + 1 != bytes.size()) {
      output << ',';
    }
  }
  output << "\n};";
  return output.str();
}

std::string expectedSourceArray(const lang::FailureSiteDescriptor &site) {
  std::ostringstream output;
  output << "[[maybe_unused]] static constexpr char __gti_failure_source_"
         << site.id << "_v1[] = {";
  for (std::size_t index = 0; index < site.logicalSource.size(); ++index) {
    output << (index % 6 == 0 ? "\n  " : " ") << "static_cast<char>("
           << static_cast<unsigned int>(
                  static_cast<unsigned char>(site.logicalSource[index]))
           << ')';
    if (index + 1 != site.logicalSource.size()) {
      output << ',';
    }
  }
  output << "\n};";
  return output.str();
}

std::string expectedSite(const lang::FailureSiteDescriptor &site) {
  std::ostringstream output;
  output << "  {{__gti_failure_source_" << site.id << "_v1, UINT64_C("
         << site.logicalSource.size() << ")}, UINT64_C(" << site.line
         << "), UINT64_C(" << site.start << "), UINT64_C(" << site.end
         << "), __gti_failure_outcomes_" << site.id << "_v1, UINT32_C("
         << site.outcomes.size() << "), UINT32_C(0)},";
  return output.str();
}

std::string expectedIdentity(const lang::FailureArtifactIdentity &identity) {
  std::ostringstream output;
  output << "        {";
  for (std::size_t index = 0; index < identity.bytes.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    output << static_cast<unsigned int>(identity.bytes[index]);
  }
  output << "},";
  return output.str();
}

std::string_view descriptorRegion(std::string_view generated) {
  std::size_t anchor = generated.find("__gti_failure_source_1_v1");
  if (anchor == std::string_view::npos) {
    anchor = generated.find("__gti_failure_canonical_descriptor_v1");
  }
  if (anchor == std::string_view::npos) {
    return {};
  }
  const std::size_t begin =
      generated.rfind("namespace gti_internal::backend {", anchor);
  const std::size_t close =
      generated.find("} // namespace gti_internal::backend", anchor);
  if (begin == std::string_view::npos || close == std::string_view::npos) {
    return {};
  }
  return generated.substr(
      begin,
      close + std::string_view{"} // namespace gti_internal::backend"}.size() -
          begin);
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
  expect(metadata.sites().size() == 2,
         "the focused fixture should contain exactly two canonical sites");

  const lang::OptimizationPipeline pipeline;
  const lang::OptimizationResult compatibility =
      pipeline.run(frontend.hir, lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      emit(frontend, frontend.mir, compatibility, lang::CppStandard::Cpp23);
  const std::string &generated = artifact.contents;
  expect(generated.find("#include <gti/runtime_failure.h>") !=
             std::string::npos,
         "production C++ should include the versioned failure ABI header");
  expect(generated.find(expectedByteArray(
             "__gti_failure_canonical_descriptor_v1",
             metadata.descriptorBytes())) != std::string::npos,
         "generated C++ should retain every canonical descriptor byte");
  expect(generated.find(expectedIdentity(metadata.artifactIdentity())) !=
             std::string::npos,
         "generated C++ should retain the exact SHA-256 artifact identity");

  for (const lang::FailureSiteDescriptor &site : metadata.sites()) {
    expect(generated.find(expectedSourceArray(site)) != std::string::npos,
           "a site should retain its exact counted logical-source bytes");
    expect(generated.find(expectedSite(site)) != std::string::npos,
           "a site should retain uint64 line/span facts and outcome count");
  }
  expect(generated.find("{GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1, "
                        "GTI_FAILURE_DETAIL_ADDITION_V1, UINT32_C(0)}") !=
                 std::string::npos &&
             generated.find("{GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1, "
                            "GTI_FAILURE_DETAIL_DIVISION_V1, UINT32_C(0)}") !=
                 std::string::npos &&
             generated.find(
                 "{GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1, "
                 "GTI_FAILURE_DETAIL_INTEGER_DIVISION_V1, UINT32_C(0)}") !=
                 std::string::npos,
         "generated sites should preserve their canonical ABI-v1 outcomes");
  expect(generated.find("__gti_failure_sites_v1,\n        UINT32_C(2),") !=
                 std::string::npos &&
             generated.find("UINT64_C(" +
                            std::to_string(metadata.descriptorBytes().size()) +
                            ")};") != std::string::npos,
         "the artifact descriptor should point at the exact site and byte "
         "tables");

  const std::string_view baseline = descriptorRegion(generated);
  expect(!baseline.empty(),
         "the focused artifact should expose one bounded descriptor region");
  for (const lang::OptimizationLevel level :
       {lang::OptimizationLevel::O1, lang::OptimizationLevel::O3}) {
    const lang::OptimizationResult levelCompatibility =
        pipeline.run(frontend.hir, level);
    const lang::OptimizedProgram optimized =
        pipeline.run({.hir = frontend.hir,
                      .mir = frontend.mir,
                      .level = level,
                      .compatibility = &levelCompatibility});
    expect(optimized.valid(),
           "optimized descriptor snapshots should remain verified");
    if (optimized.valid()) {
      expect(descriptorRegion(emit(frontend, optimized.mir, levelCompatibility,
                                   lang::CppStandard::Cpp23)
                                  .contents) == baseline,
             "O0/O1/O3 must emit byte-identical failure descriptors");
    }
  }
  expect(descriptorRegion(emit(frontend, frontend.mir, compatibility,
                               lang::CppStandard::Cpp20)
                              .contents) == baseline,
         "C++20 and C++23 must emit byte-identical failure descriptors");
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
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const std::string generated =
      emit(frontend, frontend.mir, compatibility, lang::CppStandard::Cpp23)
          .contents;
  expect(generated.find("        nullptr,\n        UINT32_C(0),") !=
                 std::string::npos &&
             generated.find("__gti_failure_sites_v1[]") == std::string::npos &&
             generated.find(expectedByteArray(
                 "__gti_failure_canonical_descriptor_v1",
                 frontend.failureMetadata.descriptorBytes())) !=
                 std::string::npos,
         "an empty artifact should retain canonical bytes with a null site "
         "table");
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

  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  bool rejected = false;
  std::string message;
  try {
    (void)emit(frontend, drifted, compatibility, lang::CppStandard::Cpp23);
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
