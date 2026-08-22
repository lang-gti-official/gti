#include "../src/compiler/cpp_mir_representation_snapshot.h"
#include "../src/compiler/cpp_representation.h"

#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool hasBuildIssue(const lang::CppMirRepresentationSnapshotBuild &build,
                   lang::CppMirRepresentationSnapshotIssueKind kind) {
  return std::any_of(
      build.issues.begin(), build.issues.end(),
      [kind](const lang::CppMirRepresentationSnapshotIssue &issue) {
        return issue.kind == kind;
      });
}

bool hasPlanIssue(const lang::CppMirProgramPlan &plan,
                  lang::CppMirPlanIssueKind kind) {
  return std::any_of(plan.issues.begin(), plan.issues.end(),
                     [kind](const lang::CppMirPlanIssue &issue) {
                       return issue.kind == kind;
                     });
}

std::size_t dataCount(const lang::CppMirRepresentationSnapshot &snapshot,
                      lang::CppMirDataKind kind) {
  return static_cast<std::size_t>(
      std::count_if(snapshot.data.begin(), snapshot.data.end(),
                    [kind](const lang::CppMirDataRepresentation &data) {
                      return data.identity.kind == kind;
                    }));
}

std::size_t thunkCount(const lang::CppMirRepresentationSnapshot &snapshot,
                       lang::CppMirThunkKind kind) {
  return static_cast<std::size_t>(
      std::count_if(snapshot.thunks.begin(), snapshot.thunks.end(),
                    [kind](const lang::CppMirGeneratedThunk &thunk) {
                      return thunk.identity.kind == kind;
                    }));
}

std::size_t
programInitializationBodyCallCount(const lang::MirProgram &program) {
  const lang::MirBody *startup = program.hostedStartup();
  if (startup == nullptr) {
    return 0;
  }
  std::size_t result = 0;
  for (const lang::MirBlock &block : startup->blocks) {
    result += static_cast<std::size_t>(std::count_if(
        block.instructions.begin(), block.instructions.end(),
        [](const lang::MirInstruction &instruction) {
          return instruction.kind == lang::MirInstructionKind::CallBody &&
                 instruction.bodyTarget ==
                     lang::MirBodyAddress{.kind = lang::MirBodyKind::Module,
                                          .owner = 0};
        }));
  }
  return result;
}

lang::FrontendResult analyzeRichProgram() {
  return lang::Frontend().analyze("cpp-mir-representation-snapshot.gti", R"(
enum class ExitCode : int32_t { success = 0, failure = 1 };

[[c_abi]]
struct NativeValue {
  int32_t value;
};

[[c_opaque]] struct UnusedHandle;

class UnusedTemplate<T> {
};

class Constants {
public:
  static constexpr int32_t answer = 42;
};

constexpr int32_t seed = 1;
mut int32_t state = seed;

extern "C" {
  int32_t native_identity(int32_t value);
}

int32_t helper(int32_t value) { return value; }

int main() {
  auto selected = []() -> int32_t { return Constants::answer; };
  return helper(selected()) - 42;
}
)");
}

lang::CppMirRepresentationSnapshotBuild
buildSnapshot(const lang::FrontendResult &frontend) {
  return lang::buildCppMirRepresentationSnapshot(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      lang::TargetInfo::host());
}

void expectLoweredRowsMatch(const lang::FrontendResult &frontend,
                            std::string_view fixture) {
  lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O1,
                                        .target = lang::TargetInfo::host()});
  expect(optimized.valid(),
         std::string(fixture) + " should produce verified optimized MIR");
  if (!optimized.valid()) {
    return;
  }
  lang::LoweredProgramBuild lowered = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
      optimized.mir, lang::TargetInfo::host());
  expect(lowered.valid(),
         std::string(fixture) + " should produce a lowered program");
  if (!lowered.valid()) {
    for (const lang::LoweredProgramIssue &issue : lowered.issues) {
      std::cerr << "  lowered issue: " << issue.detail << '\n';
    }
    return;
  }
  const lang::CppMirBodyEmissionMapRows semanticRows =
      lang::buildCppMirBodyEmissionMapRows(frontend.semantics, optimized.mir,
                                           lang::CppStandard::Cpp23);
  const lang::CppMirBodyEmissionMapRows loweredRows =
      lang::buildCppMirBodyEmissionMapRows(*lowered.program,
                                           lang::CppStandard::Cpp23);
  expect(semanticRows.types == loweredRows.types &&
             semanticRows.bodies == loweredRows.bodies &&
             semanticRows.symbols == loweredRows.symbols &&
             semanticRows.enums == loweredRows.enums &&
             semanticRows.capabilities == loweredRows.capabilities,
         std::string(fixture) +
             " should build byte-identical C++ rows from LoweredProgram");

  const lang::CppMirRepresentationSnapshotBuild frontendSnapshot =
      lang::buildCppMirRepresentationSnapshot(
          frontend.program, frontend.semantics, frontend.hir, optimized.mir,
          lang::TargetInfo::host(), lang::CppStandard::Cpp23,
          &optimized.sourceMir);
  const lang::CppMirRepresentationSnapshotBuild loweredSnapshot =
      lang::buildCppMirRepresentationSnapshot(*lowered.program,
                                              lang::CppStandard::Cpp23);
  expect(frontendSnapshot.valid() && loweredSnapshot.valid(),
         std::string(fixture) +
             " should build valid frontend and lowered planning snapshots");
  if (!frontendSnapshot.valid() || !loweredSnapshot.valid()) {
    for (const lang::CppMirRepresentationSnapshotIssue &issue :
         loweredSnapshot.issues) {
      std::cerr << "  lowered snapshot issue: " << issue.detail << '\n';
    }
    return;
  }
  expect(
      frontendSnapshot.snapshot->mir == loweredSnapshot.snapshot->mir &&
          frontendSnapshot.snapshot->bodies ==
              loweredSnapshot.snapshot->bodies &&
          frontendSnapshot.snapshot->data == loweredSnapshot.snapshot->data &&
          frontendSnapshot.snapshot->declarationRoots ==
              loweredSnapshot.snapshot->declarationRoots &&
          frontendSnapshot.snapshot->thunks == loweredSnapshot.snapshot->thunks,
      std::string(fixture) +
          " should build an exact C++ plan inventory from LoweredProgram");

  const lang::CppMirProgramPlan frontendPlan =
      lang::planCppMirProgram(optimized.mir, *frontendSnapshot.snapshot);
  const lang::CppMirProgramPlan loweredPlan =
      lang::planCppMirProgram(optimized.mir, *loweredSnapshot.snapshot);
  expect(frontendPlan.status == loweredPlan.status &&
             frontendPlan.bodies == loweredPlan.bodies &&
             frontendPlan.data == loweredPlan.data &&
             frontendPlan.declarationRoots == loweredPlan.declarationRoots &&
             frontendPlan.thunks == loweredPlan.thunks &&
             frontendPlan.issues.size() == loweredPlan.issues.size() &&
             frontendPlan.unsupported.size() == loweredPlan.unsupported.size(),
         std::string(fixture) +
             " should produce the same sealed C++ whole-program plan");
}

void testExhaustiveSealedInventory() {
  const lang::FrontendResult frontend = analyzeRichProgram();
  expect(frontend.canGenerateCode(),
         "the representation-snapshot fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return;
  }

  expectLoweredRowsMatch(frontend, "the rich representation fixture");

  lang::CppMirRepresentationSnapshotBuild build = buildSnapshot(frontend);
  expect(build.valid(),
         "the sealed builder should accept one exact frontend snapshot");
  if (!build.valid()) {
    for (const lang::CppMirRepresentationSnapshotIssue &issue : build.issues) {
      std::cerr << "  snapshot issue: " << issue.detail << '\n';
    }
    return;
  }

  const std::vector<lang::MirBodyAddress> addresses =
      lang::enumerateMirBodyAddresses(frontend.mir);
  std::vector<lang::MirBodyAddress> represented;
  for (const lang::CppMirBodyRepresentation &body : build.snapshot->bodies) {
    represented.push_back(body.identity.address);
    if (body.role == lang::CppMirBodyRole::SourceExecutable) {
      expect(body.family == lang::CppMirExecutionFamily::GeneralV1,
             "the sealed builder should publish only bodies that passed the "
             "complete generic-emitter preflight");
    } else {
      expect(body.family == lang::CppMirExecutionFamily::None,
             "non-executable body roles should not claim an execution "
             "family");
    }
  }
  expect(represented == addresses,
         "the builder should inventory every core MIR body exactly once and "
         "in core order");

  const bool sawDeclaration =
      std::any_of(build.snapshot->bodies.begin(), build.snapshot->bodies.end(),
                  [](const lang::CppMirBodyRepresentation &body) {
                    return body.identity.definition ==
                               lang::CppMirBodyDefinitionKind::Declaration &&
                           body.role == lang::CppMirBodyRole::AbiDeclaration;
                  });
  expect(sawDeclaration,
         "bodyless native declarations should be exact ABI-declaration rows");
  expect(
      dataCount(*build.snapshot, lang::CppMirDataKind::EnumDefinition) == 1 &&
          dataCount(*build.snapshot,
                    lang::CppMirDataKind::AbiTypeDeclaration) == 2 &&
          dataCount(*build.snapshot, lang::CppMirDataKind::ConstexprBinding) ==
              2 &&
          dataCount(*build.snapshot,
                    lang::CppMirDataKind::ClassTemplateDeclaration) == 1,
      "the builder should exhaustively copy enum, ABI-type, namespace "
      "constexpr, class constexpr, unused opaque, and unused generic "
      "template data facts");
  expect(
      dataCount(*build.snapshot, lang::CppMirDataKind::ClassDeclaration) == 4 &&
          dataCount(*build.snapshot,
                    lang::CppMirDataKind::CallableDeclaration) == 3 &&
          dataCount(*build.snapshot,
                    lang::CppMirDataKind::StorageDeclaration) == 4 &&
          dataCount(*build.snapshot, lang::CppMirDataKind::AccessDeclaration) ==
              1 &&
          dataCount(*build.snapshot,
                    lang::CppMirDataKind::LanguageLinkageDeclaration) == 1,
      "the builder should retain each class, callable, global/static/field "
      "storage, access, and language-linkage declaration surface even when "
      "MIR owns an associated executable body");
  expect(std::all_of(build.snapshot->data.begin(), build.snapshot->data.end(),
                     [](const lang::CppMirDataRepresentation &data) {
                       return data.support ==
                              lang::CppMirSurfaceSupport::Supported;
                     }),
         "the sealed declaration inventory should be supported by the "
         "whole-program representation emitter");

  expect(thunkCount(*build.snapshot, lang::CppMirThunkKind::HostedEntry) == 1 &&
             thunkCount(*build.snapshot,
                        lang::CppMirThunkKind::ProgramInitialization) == 1,
         "the builder should derive exactly one hosted-entry and one "
         "Module/0 initialization thunk");
  const auto hosted = std::find_if(
      build.snapshot->thunks.begin(), build.snapshot->thunks.end(),
      [](const lang::CppMirGeneratedThunk &thunk) {
        return thunk.identity.kind == lang::CppMirThunkKind::HostedEntry;
      });
  expect(hosted != build.snapshot->thunks.end() &&
             hosted->identity.ordinal == 0 &&
             hosted->sourceBody.kind == lang::MirBodyKind::HostedStartup &&
             hosted->sourceBody.owner == hosted->identity.owner &&
             hosted->dependencies.size() == 1 &&
             hosted->dependencies.front().kind ==
                 lang::CppMirThunkKind::ProgramInitialization,
         "the hosted thunk should retain exact owner/source/ordinal and its "
         "initialization-before-entry dependency");

  const lang::CppMirProgramPlan plan =
      lang::planCppMirProgram(frontend.mir, std::move(*build.snapshot));
  expect(plan.status == lang::CppMirProgramPlanStatus::Complete &&
             plan.issues.empty() && plan.unsupported.empty(),
         "the rich sealed inventory should prove complete verified-MIR "
         "emission before backend construction");
}

void testNativeCallbackGeneratedItemRows() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("cpp-mir-representation-native-callback.gti", R"(
using Callback = (int32_t) -> int32_t;
int32_t increment(int32_t value) { return value + 1; }
int32_t decrement(int32_t value) { return value - 1; }
int main() {
  Callback up = increment;
  Callback down = decrement;
  return up == nullptr || down == nullptr ? 1 : 0;
}
)");
  expect(frontend.canGenerateCode() &&
             frontend.mir.nativeCallbackAdapters().size() == 2,
         "the callback snapshot fixture should retain two MIR adapters");
  if (!frontend.canGenerateCode() ||
      frontend.mir.nativeCallbackAdapters().size() != 2) {
    return;
  }

  expectLoweredRowsMatch(frontend, "the native callback fixture");

  lang::CppMirRepresentationSnapshotBuild build = buildSnapshot(frontend);
  expect(build.valid() &&
             thunkCount(*build.snapshot,
                        lang::CppMirThunkKind::NativeInteropAdapter) == 2,
         "the sealed builder should produce one native generated-item row per "
         "verified callback adapter");
  if (!build.valid()) {
    return;
  }
  for (const lang::MirNativeCallbackAdapter &adapter :
       frontend.mir.nativeCallbackAdapters()) {
    const lang::CppMirThunkIdentity identity{
        .kind = lang::CppMirThunkKind::NativeInteropAdapter,
        .owner = adapter.id};
    const auto thunk = std::find_if(
        build.snapshot->thunks.begin(), build.snapshot->thunks.end(),
        [&](const lang::CppMirGeneratedThunk &candidate) {
          return candidate.identity == identity;
        });
    const auto *payload =
        thunk == build.snapshot->thunks.end()
            ? nullptr
            : std::get_if<lang::CppMirNativeCallbackThunk>(&thunk->payload);
    const std::size_t roots = static_cast<std::size_t>(std::count_if(
        build.snapshot->bodies.begin(), build.snapshot->bodies.end(),
        [&](const lang::CppMirBodyRepresentation &body) {
          return std::find(body.requiredThunks.begin(),
                           body.requiredThunks.end(),
                           identity) != body.requiredThunks.end();
        }));
    expect(payload != nullptr && payload->adapter == adapter &&
               thunk->sourceBody ==
                   lang::MirBodyAddress{.kind = lang::MirBodyKind::Function,
                                        .owner = adapter.target} &&
               roots == 1,
           "each callback generated item should retain its exact payload, "
           "source function, and MIR-operation body root");
  }

  lang::MirProgram drifted = frontend.mir;
  auto &adapters = const_cast<std::vector<lang::MirNativeCallbackAdapter> &>(
      drifted.nativeCallbackAdapters());
  std::swap(adapters[0].target, adapters[1].target);
  for (lang::MirNativeCallbackAdapter &adapter : adapters) {
    const lang::MirFunctionInstance *target =
        drifted.findFunctionInstance(adapter.target);
    adapter.targetMayRaiseDefinedFailure =
        target == nullptr ? adapter.targetMayRaiseDefinedFailure
                          : target->mayRaiseDefinedFailure;
  }
  expect(lang::verifyMirProgram(drifted).valid(),
         "the callback drift mutation should remain independently valid MIR");
  const lang::CppMirRepresentationSnapshotBuild driftedBuild =
      lang::buildCppMirRepresentationSnapshot(
          frontend.program, frontend.semantics, frontend.hir, drifted,
          lang::TargetInfo::host());
  expect(
      !driftedBuild.valid() &&
          hasBuildIssue(
              driftedBuild,
              lang::CppMirRepresentationSnapshotIssueKind::CrossPhaseMismatch),
      "the snapshot boundary should reject valid MIR callback rows that "
      "drift from their exact HIR adapter identities");
}

void testUnusedSourceTemplatesRemainInventorySurface() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "cpp-mir-representation-unused-templates.gti", R"(
namespace types {
using Word = int32_t;
}
namespace t = types;
using Count = int32_t;
;

T unused_free<T>(T value) { return value; }

class UnusedOperators<T> {
public:
  bool operator==(UnusedOperators<T>& other) { return true; }
};

class UnusedMembers {
  int32_t value;
public:
  UnusedMembers<uint64_t N>(int32_t values[N]) : value(0) {}
  T echo<T>(T value) { return value; }
};

int main() { return 0; }
)");
  expect(frontend.canGenerateCode(),
         "unused generic declaration surfaces should pass the frontend");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return;
  }

  const auto isUnusedFunction = [](const lang::FunctionDecl *source) {
    return source != nullptr &&
           (!source->genericParameters().empty() || source->operatorName());
  };
  const bool loweredUnusedFunction =
      std::any_of(frontend.hir.functionInstances().begin(),
                  frontend.hir.functionInstances().end(),
                  [&](const lang::HirFunctionInstance &instance) {
                    return isUnusedFunction(instance.source);
                  });
  const bool loweredUnusedConstructor =
      std::any_of(frontend.hir.constructorInstances().begin(),
                  frontend.hir.constructorInstances().end(),
                  [](const lang::HirConstructorInstance &instance) {
                    return instance.source != nullptr &&
                           instance.source->name().lexeme == "UnusedMembers";
                  });
  expect(!loweredUnusedFunction && !loweredUnusedConstructor,
         "unused source templates should have no concrete HIR/MIR body "
         "instance in this fixture");

  const lang::CppMirRepresentationSnapshotBuild build = buildSnapshot(frontend);
  expect(build.valid(),
         "unused source templates should still produce a coherent sealed "
         "snapshot");
  if (!build.valid()) {
    for (const lang::CppMirRepresentationSnapshotIssue &issue : build.issues) {
      std::cerr << "  snapshot issue: " << issue.detail << '\n';
    }
    return;
  }

  expect(
      dataCount(*build.snapshot,
                lang::CppMirDataKind::CallableTemplateDeclaration) == 3 &&
          dataCount(*build.snapshot,
                    lang::CppMirDataKind::ClassTemplateDeclaration) == 1 &&
          dataCount(*build.snapshot,
                    lang::CppMirDataKind::CallableDeclaration) == 5,
      "an unused generic free function, generic member, generic constructor, "
      "and class-template operator should remain explicit unsupported "
      "declaration inventory");
  expect(dataCount(*build.snapshot,
                   lang::CppMirDataKind::NamespaceDeclaration) == 1 &&
             dataCount(*build.snapshot,
                       lang::CppMirDataKind::NamespaceAliasDeclaration) == 1 &&
             dataCount(*build.snapshot,
                       lang::CppMirDataKind::TypeAliasDeclaration) == 2 &&
             dataCount(*build.snapshot,
                       lang::CppMirDataKind::ClassDeclaration) == 2 &&
             dataCount(*build.snapshot,
                       lang::CppMirDataKind::StorageDeclaration) == 1 &&
             dataCount(*build.snapshot,
                       lang::CppMirDataKind::AccessDeclaration) == 2 &&
             dataCount(*build.snapshot,
                       lang::CppMirDataKind::EmptyDeclaration) == 1,
         "passive namespace, alias, class, storage, and access declarations "
         "must not disappear merely because they have no executable MIR body");
  expect(std::all_of(build.snapshot->data.begin(), build.snapshot->data.end(),
                     [](const lang::CppMirDataRepresentation &data) {
                       return data.identity.declaration != 0 &&
                              data.support ==
                                  lang::CppMirSurfaceSupport::Supported;
                     }),
         "every copied declaration row should have a nonzero derived identity "
         "and complete representation support");
  const lang::CppMirProgramPlan plan =
      lang::planCppMirProgram(frontend.mir, *build.snapshot);
  expect(plan.status == lang::CppMirProgramPlanStatus::Complete &&
             plan.issues.empty(),
         "passive and uninstantiated source declarations should remain on the "
         "complete whole-program representation route");
}

void testExactProgramAndTargetAnalysisSeal() {
  constexpr std::string_view passiveSource = R"(
namespace alpha {}
namespace selected = alpha;
)";
  const lang::FrontendResult analyzed = lang::Frontend().analyze(
      "cpp-mir-representation-passive-a.gti", std::string(passiveSource));
  const lang::FrontendResult otherProgram = lang::Frontend().analyze(
      "cpp-mir-representation-passive-b.gti", std::string(passiveSource));
  expect(analyzed.canGenerateCode() && otherProgram.canGenerateCode(),
         "byte-identical passive Program fixtures should pass the frontend");
  if (!analyzed.canGenerateCode() || !otherProgram.canGenerateCode()) {
    return;
  }

  const lang::CppMirRepresentationSnapshotBuild mixedProgram =
      lang::buildCppMirRepresentationSnapshot(
          otherProgram.program, analyzed.semantics, analyzed.hir, analyzed.mir,
          lang::TargetInfo::host());
  expect(!mixedProgram.valid() &&
             hasBuildIssue(mixedProgram,
                           lang::CppMirRepresentationSnapshotIssueKind::
                               MissingProgramDeclaration),
         "a passive-only Program cannot borrow semantic/HIR/MIR ownership "
         "from a byte-identical but separately parsed Program");

  lang::HirProgram differentGraph = analyzed.hir;
  auto &differentGraphSeal =
      const_cast<lang::SemanticAnalysisSeal &>(differentGraph.analysisSeal());
  differentGraphSeal.sourceGraph.preludeRoots.push_back(999);
  expect(differentGraphSeal.programSnapshot ==
                 analyzed.semantics.analysisSeal().programSnapshot &&
             differentGraphSeal.matchesTarget(lang::TargetInfo::host()),
         "the graph-drift mutation should preserve exact Program and target "
         "identity");
  const lang::CppMirRepresentationSnapshotBuild mixedGraph =
      lang::buildCppMirRepresentationSnapshot(
          analyzed.program, analyzed.semantics, differentGraph, analyzed.mir,
          lang::TargetInfo::host());
  expect(
      !mixedGraph.valid() &&
          hasBuildIssue(
              mixedGraph,
              lang::CppMirRepresentationSnapshotIssueKind::CrossPhaseMismatch),
      "the same Program and target must still reject a HIR snapshot sealed "
      "for different source-graph/prelude provenance");

  const lang::FrontendResult conditional =
      lang::Frontend().analyze("cpp-mir-representation-passive-target.gti", R"(
#if target.os == "never"
namespace selected_never {}
using Selected = uint64_t;
#else
namespace selected_host {}
using Selected = int32_t;
#endif
)");
  expect(conditional.canGenerateCode(),
         "the target-conditional passive fixture should pass the frontend");
  if (!conditional.canGenerateCode()) {
    return;
  }
  lang::TargetInfo otherProfile = lang::TargetInfo::host();
  otherProfile.executionProfile = lang::ExecutionProfile::Concurrent;
  const lang::CppMirRepresentationSnapshotBuild mixedProfile =
      lang::buildCppMirRepresentationSnapshot(
          conditional.program, conditional.semantics, conditional.hir,
          conditional.mir, otherProfile);
  expect(
      !mixedProfile.valid() &&
          hasBuildIssue(
              mixedProfile,
              lang::CppMirRepresentationSnapshotIssueKind::CrossPhaseMismatch),
      "the full backend TargetInfo seal must reject a changed execution "
      "profile even when the active passive branch is unchanged");

  lang::TargetInfo otherTarget = lang::TargetInfo::host();
  otherTarget.os = "never";
  const lang::CppMirRepresentationSnapshotBuild mixedTarget =
      lang::buildCppMirRepresentationSnapshot(
          conditional.program, conditional.semantics, conditional.hir,
          conditional.mir, otherTarget);
  expect(!mixedTarget.valid() &&
             hasBuildIssue(mixedTarget,
                           lang::CppMirRepresentationSnapshotIssueKind::
                               CrossPhaseMismatch) &&
             hasBuildIssue(mixedTarget,
                           lang::CppMirRepresentationSnapshotIssueKind::
                               MissingProgramDeclaration),
         "the builder must reject a different full backend target and the "
         "different passive branch that target would activate");
}

void testPrivateInventorySealAndContractedThunkClosure() {
  const lang::FrontendResult passive =
      lang::Frontend().analyze("cpp-mir-representation-seal-passive.gti", R"(
namespace alpha {}
namespace selected = alpha;
)");
  expect(passive.canGenerateCode(),
         "the passive inventory-seal fixture should pass the frontend");
  if (!passive.canGenerateCode()) {
    return;
  }
  const lang::CppMirRepresentationSnapshotBuild passiveBuild =
      buildSnapshot(passive);
  expect(passiveBuild.valid() && !passiveBuild.snapshot->data.empty(),
         "the passive inventory-seal fixture should produce data rows");
  if (!passiveBuild.valid() || passiveBuild.snapshot->data.empty()) {
    return;
  }

  lang::CppMirRepresentationSnapshot omittedData = *passiveBuild.snapshot;
  omittedData.data.clear();
  const lang::CppMirProgramPlan omittedDataPlan =
      lang::planCppMirProgram(passive.mir, std::move(omittedData));
  expect(omittedDataPlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
             hasPlanIssue(omittedDataPlan,
                          lang::CppMirPlanIssueKind::InvalidInventorySeal),
         "clearing all passive rows must break the private full-copy seal");

  lang::CppMirRepresentationSnapshot staleData = *passiveBuild.snapshot;
  staleData.data.front().identity.declaration += 1000;
  const lang::CppMirProgramPlan staleDataPlan =
      lang::planCppMirProgram(passive.mir, std::move(staleData));
  expect(staleDataPlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
             hasPlanIssue(staleDataPlan,
                          lang::CppMirPlanIssueKind::InvalidInventorySeal),
         "staling a nonzero passive declaration identity must break the "
         "private full-copy seal");

  const lang::FrontendResult inertEntry = lang::Frontend().analyze(
      "cpp-mir-representation-seal-entry.gti", "int main() { return 0; }");
  expect(inertEntry.canGenerateCode(),
         "the hosted-entry inventory-seal fixture should pass the frontend");
  if (!inertEntry.canGenerateCode()) {
    return;
  }
  const lang::CppMirRepresentationSnapshotBuild inertBuild =
      buildSnapshot(inertEntry);
  expect(inertBuild.valid(),
         "the hosted-entry inventory-seal seed should build");
  if (!inertBuild.valid()) {
    return;
  }

  lang::CppMirRepresentationSnapshot omittedHosted = *inertBuild.snapshot;
  omittedHosted.thunks.erase(
      std::remove_if(omittedHosted.thunks.begin(), omittedHosted.thunks.end(),
                     [](const lang::CppMirGeneratedThunk &thunk) {
                       return thunk.identity.kind ==
                              lang::CppMirThunkKind::HostedEntry;
                     }),
      omittedHosted.thunks.end());
  for (lang::CppMirBodyRepresentation &body : omittedHosted.bodies) {
    body.requiredThunks.erase(
        std::remove_if(body.requiredThunks.begin(), body.requiredThunks.end(),
                       [](const lang::CppMirThunkIdentity &identity) {
                         return identity.kind ==
                                lang::CppMirThunkKind::HostedEntry;
                       }),
        body.requiredThunks.end());
  }
  const lang::CppMirProgramPlan omittedHostedPlan =
      lang::planCppMirProgram(inertEntry.mir, std::move(omittedHosted));
  expect(omittedHostedPlan.status ==
                 lang::CppMirProgramPlanStatus::Incoherent &&
             hasPlanIssue(omittedHostedPlan,
                          lang::CppMirPlanIssueKind::InvalidInventorySeal) &&
             hasPlanIssue(omittedHostedPlan,
                          lang::CppMirPlanIssueKind::MissingContractedThunk),
         "coordinated hosted-entry row/root omission must fail both the seal "
         "and the independently derived thunk closure");

  lang::CppMirRepresentationSnapshot injectedInitialization =
      *inertBuild.snapshot;
  const lang::CppMirThunkIdentity initialization{
      .kind = lang::CppMirThunkKind::ProgramInitialization};
  injectedInitialization.thunks.push_back(
      {.identity = initialization,
       .sourceBody = {.kind = lang::MirBodyKind::Module, .owner = 0}});
  const auto hosted = std::find_if(injectedInitialization.thunks.begin(),
                                   injectedInitialization.thunks.end(),
                                   [](const lang::CppMirGeneratedThunk &thunk) {
                                     return thunk.identity.kind ==
                                            lang::CppMirThunkKind::HostedEntry;
                                   });
  if (hosted != injectedInitialization.thunks.end()) {
    hosted->dependencies = {initialization};
  }
  const lang::CppMirProgramPlan injectedInitializationPlan =
      lang::planCppMirProgram(inertEntry.mir,
                              std::move(injectedInitialization));
  expect(injectedInitializationPlan.status ==
                 lang::CppMirProgramPlanStatus::Incoherent &&
             hasPlanIssue(injectedInitializationPlan,
                          lang::CppMirPlanIssueKind::InvalidInventorySeal) &&
             hasPlanIssue(injectedInitializationPlan,
                          lang::CppMirPlanIssueKind::UnexpectedContractedThunk),
         "an injected initialization node/dependency in an inert program "
         "must fail both the seal and independently derived thunk closure");

  const lang::FrontendResult dynamic = lang::Frontend().analyze(
      "cpp-mir-representation-seal-initialization.gti", R"(
int32_t initial_state() { return 1; }
mut int32_t state = initial_state();
int main() { return state - 1; }
)");
  expect(dynamic.canGenerateCode(),
         "the dynamic-initialization inventory-seal fixture should pass the "
         "frontend");
  if (!dynamic.canGenerateCode()) {
    return;
  }
  const lang::CppMirRepresentationSnapshotBuild dynamicBuild =
      buildSnapshot(dynamic);
  expect(dynamicBuild.valid(),
         "the dynamic-initialization inventory-seal seed should build");
  if (!dynamicBuild.valid()) {
    return;
  }
  lang::CppMirRepresentationSnapshot omittedInitialization =
      *dynamicBuild.snapshot;
  omittedInitialization.thunks.erase(
      std::remove_if(omittedInitialization.thunks.begin(),
                     omittedInitialization.thunks.end(),
                     [](const lang::CppMirGeneratedThunk &thunk) {
                       return thunk.identity.kind ==
                              lang::CppMirThunkKind::ProgramInitialization;
                     }),
      omittedInitialization.thunks.end());
  for (lang::CppMirBodyRepresentation &body : omittedInitialization.bodies) {
    body.requiredThunks.erase(
        std::remove_if(body.requiredThunks.begin(), body.requiredThunks.end(),
                       [](const lang::CppMirThunkIdentity &identity) {
                         return identity.kind ==
                                lang::CppMirThunkKind::ProgramInitialization;
                       }),
        body.requiredThunks.end());
  }
  for (lang::CppMirGeneratedThunk &thunk : omittedInitialization.thunks) {
    if (thunk.identity.kind == lang::CppMirThunkKind::HostedEntry) {
      thunk.dependencies.clear();
    }
  }
  const lang::CppMirProgramPlan omittedInitializationPlan =
      lang::planCppMirProgram(dynamic.mir, std::move(omittedInitialization));
  expect(omittedInitializationPlan.status ==
                 lang::CppMirProgramPlanStatus::Incoherent &&
             hasPlanIssue(omittedInitializationPlan,
                          lang::CppMirPlanIssueKind::InvalidInventorySeal) &&
             hasPlanIssue(omittedInitializationPlan,
                          lang::CppMirPlanIssueKind::MissingContractedThunk),
         "coordinated initialization row/root/dependency omission must fail "
         "both the seal and independently derived thunk closure");
}

void testPureDataOnlyProgramInitializationPlan() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "cpp-mir-representation-data-only-initialization.gti", R"(
constexpr int32_t seed = 7;
mut int32_t zeroed;
int main() { return seed - 7 + zeroed; }
)");
  expect(frontend.canGenerateCode(),
         "the pure data-only initialization fixture should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  expectLoweredRowsMatch(frontend, "the data-only representation fixture");
  expect(frontend.hir.module().roots.empty() &&
             !frontend.mir.programInitializationPlan().steps.empty() &&
             !frontend.mir.module().places.empty() &&
             !frontend.mir.module().blocks.empty(),
         "data-only program storage should have no HIR execution roots but "
         "retain explicit MIR storage stages");

  lang::CppMirRepresentationSnapshotBuild build = buildSnapshot(frontend);
  expect(build.valid(),
         "the sealed snapshot should accept noncanonical data-only Module "
         "storage blocks");
  if (!build.valid()) {
    return;
  }
  const auto module = std::find_if(
      build.snapshot->bodies.begin(), build.snapshot->bodies.end(),
      [](const lang::CppMirBodyRepresentation &body) {
        return body.identity.address ==
               lang::MirBodyAddress{.kind = lang::MirBodyKind::Module};
      });
  const auto hosted = std::find_if(
      build.snapshot->thunks.begin(), build.snapshot->thunks.end(),
      [](const lang::CppMirGeneratedThunk &thunk) {
        return thunk.identity.kind == lang::CppMirThunkKind::HostedEntry;
      });
  expect(module != build.snapshot->bodies.end() &&
             module->role == lang::CppMirBodyRole::DataOnly &&
             module->requiredThunks.empty() &&
             thunkCount(*build.snapshot,
                        lang::CppMirThunkKind::ProgramInitialization) == 0 &&
             hosted != build.snapshot->thunks.end() &&
             hosted->dependencies.empty() &&
             programInitializationBodyCallCount(frontend.mir) == 0,
         "pure data-only storage must not create, root, or inject a "
         "program-initialization thunk or hosted body call");

  const lang::CppMirProgramPlan plan =
      lang::planCppMirProgram(frontend.mir, std::move(*build.snapshot));
  expect(plan.status == lang::CppMirProgramPlanStatus::Complete &&
             plan.issues.empty(),
         "pure data-only storage should remain complete without an executable "
         "fallback route");
}

void testNamespacedProgramStorageRows() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "cpp-mir-representation-namespaced-storage.gti", R"(
int32_t initial_value() { return 1; }
namespace outer {
namespace values {
constexpr char marker = 'G';
mut int32_t dynamic = initial_value();
}
}
int main() { return outer::values::dynamic - 1; }
)");
  expect(frontend.canGenerateCode(),
         "namespaced program storage should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::CppMirBodyEmissionMapRows rows =
      lang::buildCppMirBodyEmissionMapRows(frontend.semantics, frontend.mir,
                                           lang::CppStandard::Cpp23);
  const auto hasStorage = [&](std::string_view qualified,
                              std::string_view spelling) {
    const auto step = std::find_if(
        frontend.mir.programInitializationPlan().steps.begin(),
        frontend.mir.programInitializationPlan().steps.end(),
        [&](const lang::MirProgramInitializationStep &candidate) {
          const lang::SymbolRecord *record =
              frontend.semantics.database().findSymbol(candidate.symbol);
          return record != nullptr && record->qualifiedName == qualified;
        });
    return step != frontend.mir.programInitializationPlan().steps.end() &&
           std::any_of(
               rows.symbols.begin(), rows.symbols.end(),
               [&](const lang::CppMirSymbolRepresentation &row) {
                 return row.kind ==
                            lang::CppMirSymbolRepresentationKind::Storage &&
                        row.owner == 0 && row.symbol == step->symbol &&
                        row.spelling == spelling;
               });
  };

  expect(hasStorage("outer::values::marker",
                    "::__gti_program::outer::values::marker") &&
             hasStorage("outer::values::dynamic",
                        "::__gti_program::outer::values::dynamic"),
         "data-only and executable Module steps should carry exact nested "
         "namespace storage spellings");
}

void testStaticOnlyProgramInitializationFact() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("cpp-mir-representation-static-only.gti", R"(
int32_t initial_value() { return 1; }
class StaticOnly {
public:
  static int32_t value = initial_value();
};
int main() { return StaticOnly::value - 1; }
)");
  expect(frontend.canGenerateCode(),
         "the static-only initialization fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : frontend.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return;
  }
  lang::CppMirRepresentationSnapshotBuild build = buildSnapshot(frontend);
  expect(build.valid(),
         "static-only initialization should produce a coherent sealed "
         "snapshot");
  if (!build.valid()) {
    return;
  }
  const auto module = std::find_if(
      build.snapshot->bodies.begin(), build.snapshot->bodies.end(),
      [](const lang::CppMirBodyRepresentation &body) {
        return body.identity.address ==
               lang::MirBodyAddress{.kind = lang::MirBodyKind::Module};
      });
  const auto staticInitializers =
      std::find_if(build.snapshot->bodies.begin(), build.snapshot->bodies.end(),
                   [](const lang::CppMirBodyRepresentation &body) {
                     return body.identity.address.kind ==
                            lang::MirBodyKind::StaticFieldInitializers;
                   });
  const auto initialization =
      std::find_if(build.snapshot->thunks.begin(), build.snapshot->thunks.end(),
                   [](const lang::CppMirGeneratedThunk &thunk) {
                     return thunk.identity.kind ==
                            lang::CppMirThunkKind::ProgramInitialization;
                   });
  expect(module != build.snapshot->bodies.end() &&
             module->role == lang::CppMirBodyRole::SourceExecutable &&
             module->requiredThunks.size() == 1 &&
             staticInitializers != build.snapshot->bodies.end() &&
             staticInitializers->role == lang::CppMirBodyRole::DataOnly &&
             staticInitializers->requiredThunks.empty() &&
             initialization != build.snapshot->thunks.end() &&
             initialization->sourceBody ==
                 lang::MirBodyAddress{.kind = lang::MirBodyKind::Module} &&
             programInitializationBodyCallCount(frontend.mir) == 1,
         "the executable Module schedule should solely root the exact "
         "Module/0 program-initialization owner marker while the migrated "
         "non-generic class-static body remains canonical data-only");
  const lang::CppMirProgramPlan plan =
      lang::planCppMirProgram(frontend.mir, std::move(*build.snapshot));
  expect(plan.status == lang::CppMirProgramPlanStatus::Complete &&
             plan.issues.empty(),
         "the static-only program-initialization marker should plan "
         "coherently through verified MIR");

  lang::CppMirRepresentationSnapshotBuild wrongRoot = buildSnapshot(frontend);
  expect(wrongRoot.valid(), "the wrong-root mutation seed should build");
  if (!wrongRoot.valid()) {
    return;
  }
  const lang::CppMirThunkIdentity initializationIdentity{
      .kind = lang::CppMirThunkKind::ProgramInitialization};
  for (lang::CppMirBodyRepresentation &body : wrongRoot.snapshot->bodies) {
    body.requiredThunks.erase(std::remove(body.requiredThunks.begin(),
                                          body.requiredThunks.end(),
                                          initializationIdentity),
                              body.requiredThunks.end());
  }
  const auto entry = std::find_if(
      wrongRoot.snapshot->bodies.begin(), wrongRoot.snapshot->bodies.end(),
      [](const lang::CppMirBodyRepresentation &body) {
        return body.identity.address.kind == lang::MirBodyKind::Function &&
               body.role == lang::CppMirBodyRole::SourceExecutable;
      });
  expect(entry != wrongRoot.snapshot->bodies.end(),
         "the wrong-root fixture should retain its entry body");
  if (entry != wrongRoot.snapshot->bodies.end()) {
    entry->requiredThunks.push_back(initializationIdentity);
  }
  const lang::CppMirProgramPlan wrongRootPlan =
      lang::planCppMirProgram(frontend.mir, std::move(*wrongRoot.snapshot));
  expect(
      wrongRootPlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
          hasPlanIssue(wrongRootPlan,
                       lang::CppMirPlanIssueKind::InvalidInventorySeal) &&
          hasPlanIssue(wrongRootPlan,
                       lang::CppMirPlanIssueKind::InvalidContractedThunkGraph),
      "an unrelated executable body must not root the program-"
      "initialization owner marker");
}

void testCheckedDynamicInitializationIsComplete() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "cpp-mir-representation-checked-initialization.gti", R"(
constexpr int32_t seed = 1;
mut int32_t dynamic = seed;
mut int32_t checked = dynamic + 1;
int main() { return checked - 2; }
)");
  expect(frontend.canGenerateCode(),
         "the checked dynamic initialization fixture should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const bool checkedFailure = std::any_of(
      frontend.mir.module().blocks.begin(), frontend.mir.module().blocks.end(),
      [](const lang::MirBlock &block) {
        return std::any_of(block.instructions.begin(), block.instructions.end(),
                           [](const lang::MirInstruction &instruction) {
                             return !instruction.definedFailure.empty();
                           });
      });
  expect(checkedFailure && lang::verifyMirProgram(frontend.mir).valid(),
         "the checked initializer should retain verified Module failure "
         "detection and propagation intent");
  lang::CppMirRepresentationSnapshotBuild build = buildSnapshot(frontend);
  expect(build.valid(),
         "checked Module failure control flow should remain snapshot-"
         "coherent");
  if (!build.valid()) {
    return;
  }
  const auto module = std::find_if(
      build.snapshot->bodies.begin(), build.snapshot->bodies.end(),
      [](const lang::CppMirBodyRepresentation &body) {
        return body.identity.address ==
               lang::MirBodyAddress{.kind = lang::MirBodyKind::Module};
      });
  expect(module != build.snapshot->bodies.end() &&
             module->role == lang::CppMirBodyRole::SourceExecutable &&
             module->requiredThunks.size() == 1 &&
             std::all_of(
                 build.snapshot->bodies.begin(), build.snapshot->bodies.end(),
                 [&](const lang::CppMirBodyRepresentation &body) {
                   return body.identity.address.kind ==
                              lang::MirBodyKind::Module ||
                          std::none_of(body.requiredThunks.begin(),
                                       body.requiredThunks.end(),
                                       [](const auto &thunk) {
                                         return thunk.kind ==
                                                lang::CppMirThunkKind::
                                                    ProgramInitialization;
                                       });
                 }),
         "only executable Module/0 should root checked program "
         "initialization");
  const lang::CppMirProgramPlan plan =
      lang::planCppMirProgram(frontend.mir, std::move(*build.snapshot));
  expect(plan.status == lang::CppMirProgramPlanStatus::Complete &&
             plan.issues.empty(),
         "checked dynamic Module execution should pass complete failure-form "
         "MIR text preflight");
}

void testMissingAndStaleFactsFailClosed() {
  const lang::FrontendResult frontend = analyzeRichProgram();
  if (!frontend.canGenerateCode()) {
    expect(false, "the missing/stale fixture should pass the frontend");
    return;
  }

  lang::FrontendResult staleProgram = analyzeRichProgram();
  expect(staleProgram.canGenerateCode(),
         "the independently owned stale Program should also be valid");
  const lang::CppMirRepresentationSnapshotBuild staleProgramBuild =
      lang::buildCppMirRepresentationSnapshot(
          staleProgram.program, frontend.semantics, frontend.hir, frontend.mir,
          lang::TargetInfo::host());
  expect(!staleProgramBuild.valid() &&
             hasBuildIssue(staleProgramBuild,
                           lang::CppMirRepresentationSnapshotIssueKind::
                               MissingProgramDeclaration),
         "byte-identical declarations from another Program must not satisfy "
         "the sealed source-identity inventory");

  lang::HirProgram missingEnumHir = frontend.hir;
  auto &enums = const_cast<std::vector<lang::HirEnum> &>(
      missingEnumHir.enumDeclarations());
  expect(!enums.empty(), "the data fixture should retain one HIR enum");
  if (!enums.empty()) {
    enums.clear();
  }
  const lang::CppMirRepresentationSnapshotBuild missingDataBuild =
      lang::buildCppMirRepresentationSnapshot(
          frontend.program, frontend.semantics, missingEnumHir, frontend.mir,
          lang::TargetInfo::host());
  expect(!missingDataBuild.valid() &&
             hasBuildIssue(missingDataBuild,
                           lang::CppMirRepresentationSnapshotIssueKind::
                               MissingHirDeclaration),
         "an omitted semantic data declaration should fail the builder seal");

  lang::HirProgram staleInitializationPlan = frontend.hir;
  auto &initializationPlan = const_cast<lang::HirProgramInitializationPlan &>(
      staleInitializationPlan.programInitializationPlan());
  expect(!initializationPlan.unitOrder.empty(),
         "the plan-drift fixture should retain source-unit order");
  if (!initializationPlan.unitOrder.empty()) {
    initializationPlan.unitOrder.push_back(
        initializationPlan.unitOrder.front());
  }
  expect(frontend.semantics.analysisSeal() ==
                 staleInitializationPlan.analysisSeal() &&
             !lang::verifyHirProgramPlans(frontend.semantics,
                                          staleInitializationPlan)
                  .valid(),
         "the plan-drift mutation should preserve the frontend seal while "
         "breaking semantic/HIR plan coherence");
  std::string planMismatch;
  expect(!lang::cppMirFrontendSnapshotsMatch(frontend.semantics,
                                             staleInitializationPlan,
                                             frontend.mir, &planMismatch) &&
             planMismatch.find("program plans differ") != std::string::npos,
         "the shared backend frontend-snapshot gate should report HIR plan "
         "verification drift before representation inventory");
  const lang::CppMirRepresentationSnapshotBuild stalePlanBuild =
      lang::buildCppMirRepresentationSnapshot(
          frontend.program, frontend.semantics, staleInitializationPlan,
          frontend.mir, lang::TargetInfo::host());
  expect(
      !stalePlanBuild.valid() &&
          hasBuildIssue(
              stalePlanBuild,
              lang::CppMirRepresentationSnapshotIssueKind::CrossPhaseMismatch),
      "the snapshot builder must reject coordinated HIR plan drift even "
      "when Program, target, and analysis seal still match");

  lang::MirProgram staleDataConstant = frontend.mir;
  auto &constantSteps = const_cast<lang::MirProgramInitializationPlan &>(
                            staleDataConstant.programInitializationPlan())
                            .steps;
  const auto constantStep = std::find_if(
      constantSteps.begin(), constantSteps.end(),
      [](const lang::MirProgramInitializationStep &step) {
        return step.dataInitialization ==
                   lang::MirProgramDataInitializationKind::Constant &&
               step.dataConstant.has_value();
      });
  expect(constantStep != constantSteps.end(),
         "the cross-phase constant fixture should retain a data constant");
  if (constantStep != constantSteps.end()) {
    if (auto *integer =
            std::get_if<lang::ConstantInteger>(&*constantStep->dataConstant)) {
      ++integer->magnitude;
    }
  }
  expect(constantStep != constantSteps.end() &&
             lang::verifyMirProgram(staleDataConstant).valid(),
         "a same-type program data-constant drift should remain generically "
         "valid MIR");
  std::string constantMismatch;
  expect(!lang::cppMirFrontendSnapshotsMatch(frontend.semantics, frontend.hir,
                                             staleDataConstant,
                                             &constantMismatch) &&
             constantMismatch.find("data-only initialization facts") !=
                 std::string::npos,
         "the frontend snapshot gate must reject a verifier-valid stale data "
         "constant");
  const lang::CppMirRepresentationSnapshotBuild staleConstantBuild =
      lang::buildCppMirRepresentationSnapshot(
          frontend.program, frontend.semantics, frontend.hir, staleDataConstant,
          lang::TargetInfo::host());
  expect(
      !staleConstantBuild.valid() &&
          hasBuildIssue(
              staleConstantBuild,
              lang::CppMirRepresentationSnapshotIssueKind::CrossPhaseMismatch),
      "the sealed builder must reject verifier-valid stale data "
      "provenance");

  lang::MirProgram staleStaticOwner = frontend.mir;
  auto &ownerSteps = const_cast<lang::MirProgramInitializationPlan &>(
                         staleStaticOwner.programInitializationPlan())
                         .steps;
  const auto staticStep = std::find_if(
      ownerSteps.begin(), ownerSteps.end(),
      [](const lang::MirProgramInitializationStep &step) {
        return step.storageKind == lang::ProgramStorageKind::StaticField;
      });
  const auto alternateOwner = std::find_if(
      frontend.mir.classInstances().begin(),
      frontend.mir.classInstances().end(), [&](const auto &candidate) {
        return staticStep != ownerSteps.end() &&
               candidate.id != staticStep->ownerClass;
      });
  expect(staticStep != ownerSteps.end() &&
             alternateOwner != frontend.mir.classInstances().end(),
         "the cross-phase owner fixture should retain an alternate class");
  if (staticStep != ownerSteps.end() &&
      alternateOwner != frontend.mir.classInstances().end()) {
    staticStep->ownerClass = alternateOwner->id;
  }
  expect(staticStep != ownerSteps.end() &&
             alternateOwner != frontend.mir.classInstances().end() &&
             lang::verifyMirProgram(staleStaticOwner).valid(),
         "an alternate valid static class owner should remain generically "
         "valid MIR");
  std::string ownerMismatch;
  expect(!lang::cppMirFrontendSnapshotsMatch(frontend.semantics, frontend.hir,
                                             staleStaticOwner,
                                             &ownerMismatch) &&
             ownerMismatch.find("step identity differs") != std::string::npos,
         "the frontend snapshot gate must reject a verifier-valid stale "
         "static owner");
  const lang::CppMirRepresentationSnapshotBuild staleOwnerBuild =
      lang::buildCppMirRepresentationSnapshot(
          frontend.program, frontend.semantics, frontend.hir, staleStaticOwner,
          lang::TargetInfo::host());
  expect(
      !staleOwnerBuild.valid() &&
          hasBuildIssue(
              staleOwnerBuild,
              lang::CppMirRepresentationSnapshotIssueKind::CrossPhaseMismatch),
      "the sealed builder must reject verifier-valid stale class "
      "ownership");

  lang::MirProgram staleHostedAnchor = frontend.mir;
  auto &hostedPlan = const_cast<std::optional<lang::MirHostedStartupPlan> &>(
      staleHostedAnchor.hostedStartupPlan());
  expect(hostedPlan.has_value() &&
             hostedPlan->sourceAnchor.end > hostedPlan->sourceAnchor.start + 1,
         "the hosted-anchor fixture should retain a nonempty main span");
  if (hostedPlan &&
      hostedPlan->sourceAnchor.end > hostedPlan->sourceAnchor.start + 1) {
    ++hostedPlan->sourceAnchor.start;
  }
  expect(hostedPlan.has_value() &&
             lang::verifyMirProgram(staleHostedAnchor).valid(),
         "a different nonempty hosted source anchor should remain generically "
         "valid MIR");
  std::string hostedAnchorMismatch;
  expect(!lang::cppMirFrontendSnapshotsMatch(frontend.semantics, frontend.hir,
                                             staleHostedAnchor,
                                             &hostedAnchorMismatch) &&
             hostedAnchorMismatch.find("hosted-startup identity differs") !=
                 std::string::npos,
         "the frontend snapshot gate must reject verifier-valid hosted main "
         "anchor drift");
  const lang::CppMirRepresentationSnapshotBuild staleHostedAnchorBuild =
      lang::buildCppMirRepresentationSnapshot(
          frontend.program, frontend.semantics, frontend.hir, staleHostedAnchor,
          lang::TargetInfo::host());
  expect(
      !staleHostedAnchorBuild.valid() &&
          hasBuildIssue(
              staleHostedAnchorBuild,
              lang::CppMirRepresentationSnapshotIssueKind::CrossPhaseMismatch),
      "the sealed builder must reject verifier-valid hosted source "
      "provenance drift");

  lang::MirProgram missingBodyIdentity = frontend.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      missingBodyIdentity.functionInstances());
  expect(!functions.empty(), "the body fixture should retain MIR functions");
  if (!functions.empty()) {
    functions.front().id = functions.size() + 100;
  }
  const lang::CppMirRepresentationSnapshotBuild missingBodyBuild =
      lang::buildCppMirRepresentationSnapshot(
          frontend.program, frontend.semantics, frontend.hir,
          missingBodyIdentity, lang::TargetInfo::host());
  expect(!missingBodyBuild.valid() &&
             hasBuildIssue(missingBodyBuild,
                           lang::CppMirRepresentationSnapshotIssueKind::
                               MissingMirBodyIdentity),
         "a missing/stale core MIR body identity should fail closed");

  lang::HirProgram staleEntryHir = frontend.hir;
  auto &hirFunctions = const_cast<std::vector<lang::HirFunctionInstance> &>(
      staleEntryHir.functionInstances());
  const auto entry =
      std::find_if(hirFunctions.begin(), hirFunctions.end(),
                   [](const lang::HirFunctionInstance &function) {
                     return function.entryKind != lang::ProgramEntryKind::None;
                   });
  expect(entry != hirFunctions.end(),
         "the thunk fixture should retain a hosted entry");
  if (entry != hirFunctions.end()) {
    entry->entryKind = lang::ProgramEntryKind::None;
  }
  const lang::CppMirRepresentationSnapshotBuild staleThunkBuild =
      lang::buildCppMirRepresentationSnapshot(
          frontend.program, frontend.semantics, staleEntryHir, frontend.mir,
          lang::TargetInfo::host());
  expect(
      !staleThunkBuild.valid() &&
          hasBuildIssue(
              staleThunkBuild,
              lang::CppMirRepresentationSnapshotIssueKind::InvalidHostedEntry),
      "stale hosted-entry facts should fail before planning");

  lang::CppMirRepresentationSnapshotBuild exact = buildSnapshot(frontend);
  expect(exact.valid(), "the exact mutation seed should build");
  if (!exact.valid()) {
    return;
  }
  const auto module = std::find_if(
      exact.snapshot->bodies.begin(), exact.snapshot->bodies.end(),
      [](const lang::CppMirBodyRepresentation &body) {
        return body.identity.address ==
               lang::MirBodyAddress{.kind = lang::MirBodyKind::Module};
      });
  expect(module != exact.snapshot->bodies.end() &&
             !module->requiredThunks.empty(),
         "the exact mutation seed should root its initialization thunk");
  exact.snapshot->thunks.erase(
      std::remove_if(exact.snapshot->thunks.begin(),
                     exact.snapshot->thunks.end(),
                     [](const lang::CppMirGeneratedThunk &thunk) {
                       return thunk.identity.kind ==
                              lang::CppMirThunkKind::ProgramInitialization;
                     }),
      exact.snapshot->thunks.end());
  const lang::CppMirProgramPlan missingThunkPlan =
      lang::planCppMirProgram(frontend.mir, std::move(*exact.snapshot));
  expect(missingThunkPlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
             hasPlanIssue(missingThunkPlan,
                          lang::CppMirPlanIssueKind::InvalidInventorySeal) &&
             hasPlanIssue(missingThunkPlan,
                          lang::CppMirPlanIssueKind::MissingThunkDependency),
         "a removed builder-derived thunk should make the complete plan "
         "incoherent, never partially executable");
}

void testRuntimeBindingRole() {
  const std::string source = R"(
namespace gti_internal {
namespace runtime {
@runtime("stdin.read_byte")
int32_t read_stdin_byte();
}
}
int main() { return 0; }
)";
  const std::filesystem::path entry = std::filesystem::temp_directory_path() /
                                      "gti-cpp-mir-representation-runtime.gti";
  const std::string key = std::filesystem::weakly_canonical(entry).string();
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(entry, source, {entry}, {{key, source}});
  expect(frontend.canGenerateCode(),
         "the internal runtime-role fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::CppMirRepresentationSnapshotBuild build = buildSnapshot(frontend);
  expect(build.valid(), "the runtime-role snapshot should build coherently");
  if (!build.valid()) {
    return;
  }
  const auto runtime =
      std::find_if(build.snapshot->bodies.begin(), build.snapshot->bodies.end(),
                   [](const lang::CppMirBodyRepresentation &body) {
                     return body.identity.definition ==
                            lang::CppMirBodyDefinitionKind::RuntimeBinding;
                   });
  expect(runtime != build.snapshot->bodies.end() &&
             runtime->role == lang::CppMirBodyRole::AbiDeclaration &&
             runtime->family == lang::CppMirExecutionFamily::None,
         "a runtime binding should be copied only as an ABI declaration, "
         "never executable MIR support");
}

void testDeducedCallableOverlayPreflight() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("snapshot-callable-overlay.gti", R"(
int invoke<Operation>(Operation operation) { return operation(); }

int main() {
  auto answer = []() -> int { return 7; };
  return invoke(answer) - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the deduced-callable overlay fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *callable = nullptr;
  const lang::FunctionInfo *declaration = nullptr;
  for (const lang::MirFunctionInstance &instance :
       frontend.mir.functionInstances()) {
    if (std::any_of(instance.parameterTypes.begin(),
                    instance.parameterTypes.end(),
                    [](const lang::SemanticType &type) {
                      return type.kind == lang::SemanticType::Lambda;
                    })) {
      callable = &instance;
      declaration = frontend.semantics.findFunction(instance.declaration);
      break;
    }
  }
  expect(callable != nullptr && declaration != nullptr,
         "the fixture should retain one concrete lambda-parameter instance");
  if (callable == nullptr || declaration == nullptr) {
    return;
  }

  lang::CppMirBodyEmissionMapRows rows = lang::buildCppMirBodyEmissionMapRows(
      frontend.semantics, frontend.mir, lang::CppStandard::Cpp23);
  const lang::CppMirBodyEmissionMap baseMap(rows);
  const lang::CppMirProgramEmissionAnalysis base =
      lang::CppMirBodyEmitter(frontend.mir, baseMap).analyzeProgram();
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = callable->id};
  const auto body =
      std::find_if(base.bodies.begin(), base.bodies.end(),
                   [&](const lang::CppMirBodyEmissionAnalysis &candidate) {
                     return candidate.body == address;
                   });
  expect(body != base.bodies.end() && !body->ready() &&
             std::any_of(body->issues.begin(), body->issues.end(),
                         [](const lang::CppMirBodyEmissionIssue &issue) {
                           return issue.kind ==
                                  lang::CppMirBodyEmissionIssueKind::
                                      UnsupportedTextVocabulary;
                         }),
         "the base spelling map should identify the unnameable closure as a "
         "specialized text surface");

  const std::optional<std::size_t> overlays =
      lang::cppMirApplyCallableTemplateTypeOverlays(rows, frontend.semantics,
                                                    lang::CppStandard::Cpp23,
                                                    *declaration, *callable);
  expect(overlays && *overlays > 0,
         "the callable instance should derive an exact declaration-level type "
         "overlay");
  if (!overlays || *overlays == 0) {
    return;
  }
  const lang::CppMirBodyEmissionMap overlayMap(std::move(rows));
  const lang::CppMirBodyEmitter overlayEmitter(frontend.mir, overlayMap);
  expect(overlayEmitter.analyze(address).ready() &&
             (overlayEmitter.supportsBodyText(address) ||
              overlayEmitter.supportsFailureBodyText(address)),
         "the exact overlay should prove complete MIR text for the callable "
         "instance");

  lang::CppMirRepresentationSnapshotBuild build = buildSnapshot(frontend);
  expect(build.valid(),
         "whole-program preflight should accept a callable body only after its "
         "exact overlay proof");
  if (!build.valid()) {
    return;
  }
  const lang::CppMirProgramPlan plan =
      lang::planCppMirProgram(frontend.mir, std::move(*build.snapshot));
  expect(plan.status == lang::CppMirProgramPlanStatus::Complete,
         "the overlay-proven callable program should produce a complete plan");
}

void testAtomicBackendRouteAndIncoherentRejection() {
  const lang::FrontendResult frontend = analyzeRichProgram();
  if (!frontend.canGenerateCode()) {
    expect(false, "the backend-route fixture should pass the frontend");
    return;
  }
  lang::CppMirRepresentationSnapshotBuild build = buildSnapshot(frontend);
  expect(build.valid(), "the backend-route snapshot should build");
  if (!build.valid()) {
    return;
  }
  const lang::CppMirProgramPlan plan =
      lang::planCppMirProgram(frontend.mir, std::move(*build.snapshot));
  expect(plan.status == lang::CppMirProgramPlanStatus::Complete &&
             lang::selectCppMirBackendProgramRoute(plan) ==
                 lang::CppMirBackendProgramRoute::VerifiedMir,
         "a complete sealed plan should select the verified-MIR route");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .sourceMir = &frontend.mir,
                                   .optimizations = optimizations});
  expect(artifact.contents.find("// Generated by GTI.") != std::string::npos &&
             artifact.contents.find("native_identity") != std::string::npos &&
             artifact.contents.find("helper") != std::string::npos &&
             artifact.contents.find("int main()") != std::string::npos,
         "verified-MIR emission should publish one complete program artifact, "
         "not a partial body artifact");

  lang::CppMirProgramPlan incoherent;
  incoherent.status = lang::CppMirProgramPlanStatus::Incoherent;
  incoherent.issues.push_back(
      {.kind = lang::CppMirPlanIssueKind::MissingBodyRow,
       .detail = "adversarial missing row"});
  bool rejected = false;
  try {
    (void)lang::selectCppMirBackendProgramRoute(incoherent);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected,
         "the backend route selector should always reject an incoherent "
         "whole-program plan");

  lang::CppMirProgramPlan unsupported;
  unsupported.status = lang::CppMirProgramPlanStatus::UnsupportedSurface;
  unsupported.unsupported.push_back(
      {.kind = lang::CppMirUnsupportedSurfaceKind::Body,
       .body = lang::MirBodyAddress{.kind = lang::MirBodyKind::Function,
                                    .owner = 1}});
  rejected = false;
  try {
    (void)lang::selectCppMirBackendProgramRoute(unsupported);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected,
         "the backend route selector should reject an unsupported plan rather "
         "than selecting a fallback emitter");

  const lang::FrontendResult stale = analyzeRichProgram();
  expect(stale.canGenerateCode(),
         "the stale backend Program should independently pass the frontend");
  rejected = false;
  try {
    (void)lang::CppBackend().generate({.program = stale.program,
                                       .semantics = frontend.semantics,
                                       .hir = frontend.hir,
                                       .mir = frontend.mir,
                                       .sourceMir = &frontend.mir,
                                       .optimizations = optimizations});
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected,
         "CppBackend should reject an incoherent Program/representation "
         "snapshot before publishing bytes");
}

// ADR 016 phase-4 agreement gate: every extracted representation authority
// must spell exactly the bytes the compatibility emitter writes, so table
// rows can never drift from emitted output while both paths coexist.
void testRepresentationSpellingAuthorities() {
  const lang::FrontendResult frontend = analyzeRichProgram();
  expect(frontend.canGenerateCode(),
         "the spelling-authority fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::FunctionDecl *helper = nullptr;
  const lang::EnumDecl *exitCode = nullptr;
  for (const lang::StmtPtr &declaration : frontend.program.declarations()) {
    if (const auto *function =
            dynamic_cast<const lang::FunctionDecl *>(declaration.get());
        function != nullptr && function->name().lexeme == "helper") {
      helper = function;
    }
    if (const auto *enumeration =
            dynamic_cast<const lang::EnumDecl *>(declaration.get());
        enumeration != nullptr && enumeration->name().lexeme == "ExitCode") {
      exitCode = enumeration;
    }
  }
  expect(helper != nullptr && exitCode != nullptr,
         "the fixture should declare the helper function and ExitCode enum");
  if (helper == nullptr || exitCode == nullptr) {
    return;
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      lang::CppBackend().generate({.program = frontend.program,
                                   .semantics = frontend.semantics,
                                   .hir = frontend.hir,
                                   .mir = frontend.mir,
                                   .sourceMir = &frontend.mir,
                                   .optimizations = optimizations});

  const std::string helperSpelling =
      lang::cppFunctionSpelling(frontend.semantics, *helper);
  const lang::FunctionInfo *helperInfo =
      frontend.semantics.findFunction(*helper);
  expect(helperInfo != nullptr &&
             helperSpelling ==
                 "__gti_fn_" + std::to_string(helperInfo->id) + "_helper" &&
             artifact.contents.find(helperSpelling + "(") != std::string::npos,
         "the function-spelling authority should produce the exact emitted "
         "helper definition name");

  lang::SemanticType int32Type;
  int32Type.kind = lang::SemanticType::Int32;
  const lang::EnumTypeInfo *exitCodeInfo =
      frontend.semantics.findEnumType(*exitCode);
  lang::SemanticType enumType;
  enumType.kind = lang::SemanticType::Enum;
  enumType.enumId = exitCodeInfo == nullptr ? 0 : exitCodeInfo->id;
  const std::string enumSpelling = lang::cppSemanticTypeSpelling(
      frontend.semantics, lang::CppStandard::Cpp20, enumType);
  expect(lang::cppSemanticTypeSpelling(frontend.semantics,
                                       lang::CppStandard::Cpp20,
                                       int32Type) == "std::int32_t" &&
             artifact.contents.find("std::int32_t") != std::string::npos &&
             exitCodeInfo != nullptr &&
             enumSpelling == "::__gti_program::ExitCode",
         "the type-spelling authority should produce the exact emitted "
         "scalar and enum spellings");

  expect(lang::cppStaticStorageBaseSpelling(7, "x") == "__gti_static_7_x" &&
             lang::cppStaticStorageValueSpelling(7, "x") ==
                 "__gti_static_7_x::value",
         "the static-storage spelling authority should match the emitted "
         "holder and value forms");
}

} // namespace

// The closure port's row contract: the builder names the inline-lambda and
// deduced-callable capabilities, one never-called body row per lambda
// instance, and one Capture name row per capture, so Closure sites can
// prove every spelling they fuse before any text emits.
void testClosureAndCallableRows() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("snapshot-closure-rows.gti", R"(
int main() {
  int offset = 3;
  auto add_offset = [offset](int value) -> int {
    return offset + value;
  };
  return add_offset(4) - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the closure-rows fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  expectLoweredRowsMatch(frontend, "the closure representation fixture");
  const lang::CppMirBodyEmissionMapRows rows =
      lang::buildCppMirBodyEmissionMapRows(frontend.semantics, frontend.mir,
                                           lang::CppStandard::Cpp23);
  const auto capability = [&](lang::CppMirEmissionCapabilityKind kind,
                              std::string_view spelling) {
    return std::any_of(
        rows.capabilities.begin(), rows.capabilities.end(),
        [&](const lang::CppMirEmissionCapabilityRepresentation &row) {
          return row.kind == kind && row.spelling == spelling;
        });
  };
  expect(capability(lang::CppMirEmissionCapabilityKind::Closure,
                    "cpp_inline_lambda_v1") &&
             capability(lang::CppMirEmissionCapabilityKind::CallableDispatch,
                        "cpp_deduced_callable_v1"),
         "the builder should name the inline-lambda and deduced-callable "
         "capabilities");
  expect(!frontend.mir.lambdaInstances().empty(),
         "the fixture should lower one lambda instance");
  for (const lang::MirLambdaInstance &lambda : frontend.mir.lambdaInstances()) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Lambda,
                                       .owner = lambda.id};
    expect(std::any_of(rows.bodies.begin(), rows.bodies.end(),
                       [&](const lang::CppMirBodyNameRepresentation &row) {
                         return row.address == address &&
                                row.spelling == "__gti_inline_lambda_" +
                                                    std::to_string(lambda.id);
                       }),
           "each lambda instance should carry its never-called body row");
    for (std::size_t index = 0; index < lambda.captureSymbols.size(); ++index) {
      const lang::SymbolId symbol = lambda.captureSymbols[index];
      expect(std::any_of(
                 rows.symbols.begin(), rows.symbols.end(),
                 [&](const lang::CppMirSymbolRepresentation &row) {
                   return row.kind ==
                              lang::CppMirSymbolRepresentationKind::Capture &&
                          row.owner == lambda.id && row.symbol == symbol &&
                          row.ordinal == index + 1 && !row.spelling.empty();
                 }),
             "each capture should carry its exact source-named row");
    }
  }
}

void testDeclarationAdapterGeneratedItemRows() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "cpp-mir-representation-declaration-adapters.gti", R"(
class Cursor {
  mut int32_t current = 0;
public:
  int32_t& operator*() { return this.current; }
  void operator++() mut { this.current++; }
  bool operator!=(Cursor& other) { return this.current != other.current; }
};

class Increment {
public:
  int32_t operator()(int32_t value) { return value + 1; }
};

int main() {
  Increment increment = Increment();
  return increment(0);
}
)");
  expect(frontend.canGenerateCode(),
         "the declaration-adapter fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O1,
                                        .target = lang::TargetInfo::host()});
  lang::LoweredProgramBuild lowered = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
      optimized.mir, lang::TargetInfo::host());
  expect(optimized.valid() && lowered.valid(),
         "the declaration-adapter fixture should produce LoweredProgram");
  if (!optimized.valid() || !lowered.valid()) {
    return;
  }
  lang::CppMirRepresentationSnapshotBuild build =
      lang::buildCppMirRepresentationSnapshot(*lowered.program);
  expect(build.valid(),
         "the lowered declaration adapters should build a sealed C++ plan");
  if (!build.valid()) {
    return;
  }
  expect(thunkCount(*build.snapshot,
                    lang::CppMirThunkKind::StructuralOperatorAdapter) == 3 &&
             thunkCount(*build.snapshot,
                        lang::CppMirThunkKind::CallableAdapter) == 1 &&
             build.snapshot->declarationRoots.size() == 4,
         "each eligible function declaration should own one exact generated "
         "adapter row and root");
  const bool exactSources = std::all_of(
      build.snapshot->thunks.begin(), build.snapshot->thunks.end(),
      [&](const lang::CppMirGeneratedThunk &thunk) {
        if (thunk.identity.kind !=
                lang::CppMirThunkKind::StructuralOperatorAdapter &&
            thunk.identity.kind != lang::CppMirThunkKind::CallableAdapter) {
          return true;
        }
        const auto roots =
            std::find_if(build.snapshot->declarationRoots.begin(),
                         build.snapshot->declarationRoots.end(),
                         [&](const lang::CppMirDeclarationThunkRoots &row) {
                           return row.declaration == thunk.sourceDeclaration;
                         });
        return thunk.sourceKind ==
                   lang::CppMirGeneratedThunkSourceKind::Declaration &&
               thunk.sourceDeclaration != 0 &&
               roots != build.snapshot->declarationRoots.end() &&
               std::find(roots->requiredThunks.begin(),
                         roots->requiredThunks.end(),
                         thunk.identity) != roots->requiredThunks.end();
      });
  expect(exactSources,
         "adapter rows should retain their exact declaration provenance");
  const lang::CppMirProgramPlan plan =
      lang::planCppMirProgram(optimized.mir, std::move(*build.snapshot));
  expect(plan.complete() && plan.issues.empty() && plan.unsupported.empty(),
         "structural and callable adapters should be contracted production "
         "families rather than unsupported placeholders");
}

int main() {
  testExhaustiveSealedInventory();
  testNativeCallbackGeneratedItemRows();
  testUnusedSourceTemplatesRemainInventorySurface();
  testExactProgramAndTargetAnalysisSeal();
  testPrivateInventorySealAndContractedThunkClosure();
  testPureDataOnlyProgramInitializationPlan();
  testNamespacedProgramStorageRows();
  testStaticOnlyProgramInitializationFact();
  testCheckedDynamicInitializationIsComplete();
  testMissingAndStaleFactsFailClosed();
  testRuntimeBindingRole();
  testDeducedCallableOverlayPreflight();
  testAtomicBackendRouteAndIncoherentRejection();
  testRepresentationSpellingAuthorities();
  testClosureAndCallableRows();
  testDeclarationAdapterGeneratedItemRows();

  if (failures != 0) {
    std::cerr << failures
              << " C++ MIR representation-snapshot test(s) failed\n";
    return 1;
  }
  std::cout << "C++ MIR representation-snapshot tests passed\n";
  return 0;
}
