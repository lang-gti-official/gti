#include "gti/failure_metadata.h"
#include "gti/frontend.h"
#include "gti/mir_printer.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

bool passed = true;

void expect(bool condition, const std::string &message) {
  if (condition) {
    return;
  }
  passed = false;
  std::cerr << "FAIL: " << message << '\n';
}

void printDiagnostics(const lang::FrontendResult &frontend) {
  for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
    std::cerr << "  " << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

template <typename Callback>
void forEachInstruction(const lang::MirProgram &program, Callback callback) {
  const auto body = [&](const lang::MirBody &value) {
    for (const lang::MirBlock &block : value.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        callback(instruction);
      }
    }
  };
  body(program.module());
  for (const lang::MirClassInstance &instance : program.classInstances()) {
    body(instance.fieldInitializers);
    body(instance.staticFieldInitializers);
  }
  for (const lang::MirFunctionInstance &instance :
       program.functionInstances()) {
    body(instance.body);
  }
  for (const lang::MirConstructorInstance &instance :
       program.constructorInstances()) {
    body(instance.body);
  }
  for (const lang::MirDestructorInstance &instance :
       program.destructorInstances()) {
    body(instance.body);
  }
  for (const lang::MirLambdaInstance &instance : program.lambdaInstances()) {
    body(instance.body);
  }
}

lang::MirInstruction *firstFailureInstruction(lang::MirProgram &program) {
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      program.functionInstances());
  for (lang::MirFunctionInstance &function : functions) {
    for (lang::MirBlock &block : function.body.blocks) {
      const auto found = std::find_if(
          block.instructions.begin(), block.instructions.end(),
          [](const lang::MirInstruction &instruction) {
            return !instruction.definedFailure.localOrigins.empty();
          });
      if (found != block.instructions.end()) {
        return &*found;
      }
    }
  }
  return nullptr;
}

void testCanonicalMetadataAndMirSites() {
  const std::string source = R"(
int32_t increment<T>(T marker, int32_t value) {
  return value + 1;
}

int main() {
  int32_t left = increment<int32_t>(1, 1);
  int32_t right = increment<int64_t>(int64_t(2), 2);
  mut int32_t values[2] = {left, right};
  return values[0] / right;
}
)";
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("/tmp/gti-failure-a/main.gti", source);
  const lang::FrontendResult relocated =
      lang::Frontend().analyze("/tmp/gti-failure-b/main.gti", source);

  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "the failure metadata fixture should reach verified MIR");
  expect(relocated.canGenerateCode() && relocated.diagnostics.empty(),
         "the relocated failure metadata fixture should compile");
  if (!frontend.canGenerateCode() || !relocated.canGenerateCode()) {
    printDiagnostics(frontend);
    printDiagnostics(relocated);
    return;
  }

  const lang::FailureMetadata &metadata = frontend.failureMetadata;
  expect(lang::verifyFailureMetadata(metadata).valid(),
         "the frontend should publish structurally valid failure metadata");
  expect(!metadata.sites().empty() && !metadata.artifactIdentity().isZero() &&
             metadata.artifactIdentity().hex().size() == 64,
         "a checked artifact should have one-based sites and a SHA-256 ID");
  expect(metadata.descriptorBytes() ==
                 relocated.failureMetadata.descriptorBytes() &&
             metadata.artifactIdentity() ==
                 relocated.failureMetadata.artifactIdentity() &&
             metadata.sites() == relocated.failureMetadata.sites(),
         "relocating identical direct compilation inputs must not change the "
         "failure descriptor");
  const bool directNames =
      std::all_of(metadata.sourceUnits().begin(), metadata.sourceUnits().end(),
                  [](const lang::FailureSourceDescriptor &unit) {
                    return unit.logicalName == "main.gti" &&
                           unit.logicalName.find("/tmp/") == std::string::npos;
                  });
  if (!directNames) {
    for (const lang::FailureSourceDescriptor &unit : metadata.sourceUnits()) {
      std::cerr << "  failure source " << unit.sourceUnit << ": "
                << unit.logicalName << '\n';
    }
  }
  expect(directNames,
         "direct compilation metadata should retain only entry-relative "
         "logical names");

  std::size_t localOrigins = 0;
  std::size_t propagationOperations = 0;
  bool exactSites = true;
  forEachInstruction(
      frontend.mir, [&](const lang::MirInstruction &instruction) {
        localOrigins += instruction.definedFailure.localOrigins.size();
        propagationOperations += instruction.definedFailure.propagation !=
                                 lang::FailurePropagationKind::None;
        exactSites =
            exactSites && instruction.localFailureSites.size() ==
                              instruction.definedFailure.localOrigins.size();
        for (std::size_t index = 0;
             index < instruction.definedFailure.localOrigins.size(); ++index) {
          const std::optional<lang::FailureSiteId> expected =
              metadata.siteFor(instruction.definedFailure.localOrigins[index]);
          exactSites = exactSites && expected &&
                       instruction.localFailureSites[index] == *expected;
        }
        if (instruction.definedFailure.localOrigins.empty()) {
          exactSites = exactSites && instruction.localFailureSites.empty();
        }
      });
  if (localOrigins <= metadata.sites().size()) {
    std::cerr << "  local origins: " << localOrigins
              << ", canonical sites: " << metadata.sites().size() << '\n';
  }
  expect(localOrigins > metadata.sites().size(),
         "generic instances should coalesce at their shared definition site");
  expect(propagationOperations != 0 && exactSites &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "MIR detectors should retain exact sites while propagation remains "
         "un-sited");

  const std::string snapshot = lang::MirPrinter().print(frontend.mir);
  const bool validSnapshot =
      snapshot.starts_with("mir-v16 ") &&
      snapshot.find("failure-metadata artifact=" +
                    metadata.artifactIdentity().hex()) != std::string::npos &&
      snapshot.find("failure-site @1 source=8:main.gti") != std::string::npos &&
      snapshot.find(" failure-sites=[") != std::string::npos &&
      snapshot.find("/tmp/gti-failure") == std::string::npos;
  if (!validSnapshot) {
    std::cerr << snapshot;
  }
  expect(validSnapshot,
         "MIR snapshots should expose deterministic artifact sites without "
         "absolute paths");
}

void testEmptyDescriptorContract() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "/tmp/gti-empty/main.gti", "int main() { return 0; }");
  expect(frontend.canGenerateCode() && frontend.failureMetadata.sites().empty(),
         "a failure-free artifact should still publish an empty descriptor");
  expect(frontend.failureMetadata.artifactIdentity().hex() ==
             "4ca6c6a620c8410ce1f804123e28329ae1e10a2fca4c6d975e502246a213a721",
         "the empty descriptor must retain the normative prefix digest");
}

void testExternalSourceRouteIdentity() {
  const std::filesystem::path fixtureRoot =
      std::filesystem::weakly_canonical(
          std::filesystem::temp_directory_path()) /
      "gti-route";
  const std::string entrySource = R"(
#include "../shared.gti"
int main() { return checked(1); }
)";
  const std::string externalSource =
      "int checked(int value) { return value + 1; }";
  const auto analyzeAt = [&](const std::filesystem::path &root) {
    const std::filesystem::path entry = root / "app/main.gti";
    const std::filesystem::path external = root / "shared.gti";
    return lang::Frontend().analyze(entry, entrySource, {},
                                    {{external.string(), externalSource}});
  };
  const lang::FrontendResult frontend = analyzeAt(fixtureRoot);
  const lang::FrontendResult relocated =
      analyzeAt(fixtureRoot.parent_path() / "gti-route-relocated");

  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "the external source route fixture should compile");
  expect(relocated.canGenerateCode() && relocated.diagnostics.empty(),
         "the relocated external source route fixture should compile");
  if (!frontend.canGenerateCode() || !relocated.canGenerateCode()) {
    printDiagnostics(frontend);
    printDiagnostics(relocated);
    return;
  }
  const auto externalUnit =
      std::find_if(frontend.failureMetadata.sourceUnits().begin(),
                   frontend.failureMetadata.sourceUnits().end(),
                   [](const lang::FailureSourceDescriptor &unit) {
                     return unit.logicalName.starts_with("<external>/");
                   });
  const auto edge = std::find_if(frontend.sourceGraph.dependencyEdges().begin(),
                                 frontend.sourceGraph.dependencyEdges().end(),
                                 [](const lang::SourceDependency &dependency) {
                                   return dependency.kind ==
                                          lang::SourceDependencyKind::Include;
                                 });
  expect(externalUnit != frontend.failureMetadata.sourceUnits().end() &&
             externalUnit->logicalName.ends_with("/shared.gti") &&
             externalUnit->logicalName.find("/tmp/") == std::string::npos,
         "an out-of-root unit should use a content-and-route external name");
  expect(frontend.failureMetadata.descriptorBytes() ==
                 relocated.failureMetadata.descriptorBytes() &&
             frontend.failureMetadata.artifactIdentity() ==
                 relocated.failureMetadata.artifactIdentity(),
         "relocating an external include graph must preserve its descriptor "
         "and artifact identity");
  expect(edge != frontend.sourceGraph.dependencyEdges().end() &&
             edge->includeSpelling == "\"../shared.gti\"" &&
             edge->includeOccurrence == 0,
         "the source graph should retain exact non-path include-route facts");
}

void testMetadataAndSiteVerifierMutations() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "/tmp/gti-forged/main.gti", "int checked(int value) { return value + 1; }"
                                  "int main() { return checked(1); }");
  expect(frontend.canGenerateCode(),
         "the verifier mutation fixture should compile");
  if (!frontend.canGenerateCode()) {
    return;
  }

  lang::MirProgram wrongSite = frontend.mir;
  lang::MirInstruction *instruction = firstFailureInstruction(wrongSite);
  if (instruction != nullptr && !instruction->localFailureSites.empty()) {
    instruction->localFailureSites.front() =
        instruction->localFailureSites.front() == 1 ? 2 : 1;
  }
  const lang::MirVerificationResult wrongSiteResult =
      lang::verifyMirProgram(wrongSite);
  expect(instruction != nullptr && !wrongSiteResult.valid() &&
             std::any_of(wrongSiteResult.errors.begin(),
                         wrongSiteResult.errors.end(),
                         [](const lang::MirVerificationError &error) {
                           return error.message.find("artifact-local site") !=
                                  std::string::npos;
                         }),
         "MIR verification should reject a forged detector site");

  lang::FailureMetadata malformed = frontend.failureMetadata;
  auto &bytes =
      const_cast<std::vector<std::uint8_t> &>(malformed.descriptorBytes());
  if (!bytes.empty()) {
    bytes.front() ^= 0xFFU;
  }
  expect(!lang::verifyFailureMetadata(malformed).valid(),
         "failure metadata verification should reject descriptor drift");

  lang::FailureMetadata wrongAssignment = frontend.failureMetadata;
  auto &assignments = const_cast<std::vector<lang::FailureOriginAssignment> &>(
      wrongAssignment.assignments());
  if (!assignments.empty()) {
    assignments.front().site = 0;
  }
  expect(!lang::verifyFailureMetadata(wrongAssignment).valid(),
         "failure metadata verification should reject a forged assignment");
}

} // namespace

int main() {
  testCanonicalMetadataAndMirSites();
  testEmptyDescriptorContract();
  testExternalSourceRouteIdentity();
  testMetadataAndSiteVerifierMutations();
  return passed ? 0 : 1;
}
