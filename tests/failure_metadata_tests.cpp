#include "gti/failure_metadata.h"
#include "gti/frontend.h"
#include "gti/mir_printer.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
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

std::optional<lang::HirFunctionInstanceId>
functionInstance(const lang::FrontendResult &frontend, std::string_view name) {
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source != nullptr && function.source->name().lexeme == name) {
      return function.id;
    }
  }
  return std::nullopt;
}

lang::MirBody *functionBody(lang::MirProgram &program,
                            lang::HirFunctionInstanceId id) {
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      program.functionInstances());
  const auto found =
      std::find_if(functions.begin(), functions.end(),
                   [&](const lang::MirFunctionInstance &function) {
                     return function.id == id;
                   });
  return found == functions.end() ? nullptr : &found->body;
}

const lang::MirBody *functionBody(const lang::FrontendResult &frontend,
                                  std::string_view name) {
  const std::optional<lang::HirFunctionInstanceId> id =
      functionInstance(frontend, name);
  const lang::MirFunctionInstance *function =
      id ? frontend.mir.findFunctionInstance(*id) : nullptr;
  return function == nullptr ? nullptr : &function->body;
}

bool hasVerificationError(const lang::MirVerificationResult &result,
                          std::string_view message) {
  return std::any_of(result.errors.begin(), result.errors.end(),
                     [&](const lang::MirVerificationError &error) {
                       return error.message.find(message) != std::string::npos;
                     });
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
      snapshot.starts_with("mir-v17 ") &&
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

void testInvokeEdgesAndFailureCleanup() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("/tmp/gti-failure-control/main.gti", R"(
class Token {
public:
  int value;
  Token(int input) : value(input) {}
  ~Token() {}
};

int leaf(mut int value) {
  Token outer = Token(1);
  {
    Token inner = Token(2);
    return value + 1;
  }
}

int caller(mut int value) {
  Token local = Token(3);
  return leaf(value);
}

int consume(Token token, int value) { return value; }

int deferred_nested_argument(mut int value) {
  return consume(Token(4), value + 1);
}

int main() { return caller(1); }
)");
  expect(frontend.canGenerateCode() && frontend.diagnostics.empty(),
         "the failure cleanup fixture should reach verified MIR");
  if (!frontend.canGenerateCode()) {
    printDiagnostics(frontend);
    return;
  }

  const lang::MirBody *leaf = functionBody(frontend, "leaf");
  const lang::MirBody *caller = functionBody(frontend, "caller");
  const auto inspect = [](const lang::MirBody *body,
                          std::size_t expectedFailureDrops,
                          lang::FailurePropagationKind propagation) {
    if (body == nullptr || body->failureRecords.size() != 1) {
      return false;
    }
    const lang::MirFailureRecord &record = body->failureRecords.front();
    const lang::MirBlock *producer = body->findBlock(record.producerBlock);
    const lang::MirBlock *cleanup = body->findBlock(record.parameterBlock);
    if (producer == nullptr || cleanup == nullptr ||
        producer->terminator.kind != lang::MirTerminatorKind::Invoke ||
        producer->terminator.failureRecord != record.id ||
        cleanup->failureParameter != record.id ||
        cleanup->terminator.kind != lang::MirTerminatorKind::PropagateFailure ||
        cleanup->terminator.failureRecord != record.id ||
        producer->instructions.empty() ||
        producer->instructions.back().id != record.producerInstruction ||
        producer->instructions.back().definedFailure.propagation !=
            propagation) {
      return false;
    }
    std::vector<std::size_t> constructionOrder;
    for (const lang::MirInstruction &instruction : cleanup->instructions) {
      if (instruction.kind != lang::MirInstructionKind::Drop) {
        continue;
      }
      if (instruction.lifecycle.size() != 1 ||
          !instruction.lifecycle.front().failureCleanup) {
        return false;
      }
      const lang::MirDropObligation *obligation =
          body->findDropObligation(instruction.lifecycle.front().source);
      if (obligation == nullptr) {
        return false;
      }
      constructionOrder.push_back(obligation->constructionOrder);
    }
    return constructionOrder.size() == expectedFailureDrops &&
           std::is_sorted(constructionOrder.begin(), constructionOrder.end(),
                          std::greater<>());
  };
  expect(inspect(leaf, 2, lang::FailurePropagationKind::None),
         "a local scalar failure should receive an exact record and clean "
         "nested locals in reverse construction order");
  expect(inspect(caller, 1, lang::FailurePropagationKind::DirectCall),
         "a propagating scalar call should preserve its record while cleaning "
         "the caller's local state");

  const lang::MirBody *deferred =
      functionBody(frontend, "deferred_nested_argument");
  std::size_t deferredLocalOrigins = 0;
  bool deferredInvokePropagates = false;
  if (deferred != nullptr) {
    for (const lang::MirBlock &block : deferred->blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        deferredLocalOrigins +=
            !instruction.definedFailure.localOrigins.empty();
      }
    }
    if (deferred->failureRecords.size() == 1) {
      const lang::MirFailureRecord &record = deferred->failureRecords.front();
      const lang::MirBlock *producer =
          deferred->findBlock(record.producerBlock);
      const lang::MirInstruction *invocation =
          producer == nullptr || producer->instructions.empty()
              ? nullptr
              : &producer->instructions.back();
      deferredInvokePropagates =
          invocation != nullptr && invocation->definedFailure.propagation ==
                                       lang::FailurePropagationKind::DirectCall;
    }
  }
  expect(deferred != nullptr && deferredLocalOrigins != 0 &&
             deferredInvokePropagates,
         "nested argument detectors should remain identity-only until owned "
         "parameter staging is represented, while the root call propagates");

  const std::string snapshot = lang::MirPrinter().print(frontend.mir);
  expect(snapshot.find("failure-records 1") != std::string::npos &&
             snapshot.find("failure-parameter=fail1") != std::string::npos &&
             snapshot.find("failure-cleanup=1") != std::string::npos,
         "MIR snapshots should expose fixed records and failure cleanup");

  const std::optional<lang::HirFunctionInstanceId> leafId =
      functionInstance(frontend, "leaf");
  if (!leafId) {
    expect(false, "the failure cleanup fixture should retain leaf identity");
    return;
  }

  lang::MirProgram missingInvoke = frontend.mir;
  lang::MirBody *missingInvokeLeaf = functionBody(missingInvoke, *leafId);
  if (missingInvokeLeaf != nullptr &&
      !missingInvokeLeaf->failureRecords.empty()) {
    const lang::MirFailureRecord &record =
        missingInvokeLeaf->failureRecords.front();
    lang::MirBlock &producer =
        missingInvokeLeaf->blocks[record.producerBlock - 1];
    producer.terminator = {.kind = lang::MirTerminatorKind::Goto,
                           .target = producer.terminator.target};
    lang::rebuildMirReachability(*missingInvokeLeaf);
    (void)lang::rebuildMirValueUses(*missingInvokeLeaf);
  }
  const lang::MirVerificationResult missingInvokeResult =
      lang::verifyMirProgram(missingInvoke);
  expect(!missingInvokeResult.valid() &&
             hasVerificationError(missingInvokeResult, "one invoke"),
         "MIR verification should reject a removed failure invoke");

  lang::MirProgram rewrittenRecord = frontend.mir;
  lang::MirBody *rewrittenRecordLeaf = functionBody(rewrittenRecord, *leafId);
  if (rewrittenRecordLeaf != nullptr &&
      !rewrittenRecordLeaf->failureRecords.empty()) {
    const lang::MirFailureRecord &record =
        rewrittenRecordLeaf->failureRecords.front();
    rewrittenRecordLeaf->blocks[record.parameterBlock - 1]
        .terminator.failureRecord = 0;
  }
  const lang::MirVerificationResult rewrittenRecordResult =
      lang::verifyMirProgram(rewrittenRecord);
  expect(!rewrittenRecordResult.valid() &&
             hasVerificationError(rewrittenRecordResult,
                                  "preserve its exact fixed record"),
         "MIR verification should reject a rewritten propagated record");

  lang::MirProgram reorderedCleanup = frontend.mir;
  lang::MirBody *reorderedCleanupLeaf = functionBody(reorderedCleanup, *leafId);
  if (reorderedCleanupLeaf != nullptr &&
      !reorderedCleanupLeaf->failureRecords.empty()) {
    const lang::MirFailureRecord &record =
        reorderedCleanupLeaf->failureRecords.front();
    std::vector<lang::MirInstruction> &instructions =
        reorderedCleanupLeaf->blocks[record.parameterBlock - 1].instructions;
    std::vector<std::size_t> drops;
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      if (instructions[index].kind == lang::MirInstructionKind::Drop) {
        drops.push_back(index);
      }
    }
    if (drops.size() >= 2) {
      std::swap(instructions[drops[0]], instructions[drops[1]]);
    }
    (void)lang::rebuildMirValueUses(*reorderedCleanupLeaf);
  }
  const lang::MirVerificationResult reorderedCleanupResult =
      lang::verifyMirProgram(reorderedCleanup);
  expect(!reorderedCleanupResult.valid() &&
             hasVerificationError(reorderedCleanupResult, "cleanup sequence"),
         "MIR verification should reject reordered failure cleanup");
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
  testInvokeEdgesAndFailureCleanup();
  testEmptyDescriptorContract();
  testExternalSourceRouteIdentity();
  testMetadataAndSiteVerifierMutations();
  return passed ? 0 : 1;
}
