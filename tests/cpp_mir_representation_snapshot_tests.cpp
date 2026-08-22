#include "../src/compiler/cpp_mir_representation_snapshot.h"
#include "../src/compiler/cpp_representation.h"

#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

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
  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  return lang::buildCppMirRepresentationSnapshot(lowered);
}

lang::CppMirBodyEmissionMapRows
buildRows(const lang::FrontendResult &frontend,
          lang::CppStandard standard = lang::CppStandard::Cpp23) {
  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  return lang::buildCppMirBodyEmissionMapRows(lowered, standard);
}

void expectLoweredRowsDeterministic(const lang::FrontendResult &frontend,
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
  const lang::CppMirBodyEmissionMapRows firstRows =
      lang::buildCppMirBodyEmissionMapRows(*lowered.program,
                                           lang::CppStandard::Cpp23);
  const lang::CppMirBodyEmissionMapRows secondRows =
      lang::buildCppMirBodyEmissionMapRows(*lowered.program,
                                           lang::CppStandard::Cpp23);
  expect(firstRows.types == secondRows.types &&
             firstRows.bodies == secondRows.bodies &&
             firstRows.symbols == secondRows.symbols &&
             firstRows.enums == secondRows.enums &&
             firstRows.capabilities == secondRows.capabilities,
         std::string(fixture) +
             " should build deterministic C++ rows from LoweredProgram");

  const lang::CppMirRepresentationSnapshotBuild firstSnapshot =
      lang::buildCppMirRepresentationSnapshot(*lowered.program,
                                              lang::CppStandard::Cpp23);
  const lang::CppMirRepresentationSnapshotBuild secondSnapshot =
      lang::buildCppMirRepresentationSnapshot(*lowered.program,
                                              lang::CppStandard::Cpp23);
  expect(firstSnapshot.valid() && secondSnapshot.valid(),
         std::string(fixture) + " should build valid lowered snapshots");
  if (!firstSnapshot.valid() || !secondSnapshot.valid()) {
    for (const lang::CppMirRepresentationSnapshotIssue &issue :
         firstSnapshot.issues) {
      std::cerr << "  lowered snapshot issue: " << issue.detail << '\n';
    }
    return;
  }
  expect(firstSnapshot.snapshot->mir == secondSnapshot.snapshot->mir &&
             firstSnapshot.snapshot->bodies ==
                 secondSnapshot.snapshot->bodies &&
             firstSnapshot.snapshot->data == secondSnapshot.snapshot->data &&
             firstSnapshot.snapshot->declarationRoots ==
                 secondSnapshot.snapshot->declarationRoots &&
             firstSnapshot.snapshot->thunks == secondSnapshot.snapshot->thunks,
         std::string(fixture) +
             " should build a deterministic C++ plan inventory");

  const lang::CppMirProgramPlan plan =
      lang::planCppMirProgram(optimized.mir, *firstSnapshot.snapshot);
  expect(plan.complete() && plan.issues.empty() && plan.unsupported.empty(),
         std::string(fixture) + " should produce a complete C++ plan");
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

  expectLoweredRowsDeterministic(frontend, "the rich representation fixture");

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

  expectLoweredRowsDeterministic(frontend, "the native callback fixture");

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
  const lang::LoweredProgramBuild driftedBuild =
      lang::LoweredProgramBuilder().build(frontend.program, frontend.semantics,
                                          frontend.hir, frontend.mir, drifted,
                                          lang::TargetInfo::host());
  expect(!driftedBuild.valid(),
         "LoweredProgram construction should reject valid MIR callback rows "
         "that drift from their exact HIR adapter identities");
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

void testLoweredProgramConstructionSeal() {
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

  const lang::LoweredProgramBuild mixedProgram =
      lang::LoweredProgramBuilder().build(
          otherProgram.program, analyzed.semantics, analyzed.hir, analyzed.mir,
          analyzed.mir, lang::TargetInfo::host());
  expect(!mixedProgram.valid(),
         "LoweredProgram construction should reject separately owned "
         "frontend snapshots even when their source bytes match");

  lang::HirProgram differentGraph = analyzed.hir;
  auto &differentGraphSeal =
      const_cast<lang::SemanticAnalysisSeal &>(differentGraph.analysisSeal());
  differentGraphSeal.sourceGraph.preludeRoots.push_back(999);
  const lang::LoweredProgramBuild mixedGraph =
      lang::LoweredProgramBuilder().build(
          analyzed.program, analyzed.semantics, differentGraph, analyzed.mir,
          analyzed.mir, lang::TargetInfo::host());
  expect(!mixedGraph.valid(),
         "LoweredProgram construction should reject source-graph provenance "
         "drift before a backend can observe it");

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
  const lang::LoweredProgramBuild mixedProfile =
      lang::LoweredProgramBuilder().build(
          conditional.program, conditional.semantics, conditional.hir,
          conditional.mir, conditional.mir, otherProfile);
  expect(!mixedProfile.valid(),
         "LoweredProgram construction should reject a backend target that "
         "differs from the analyzed target seal");
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
  expectLoweredRowsDeterministic(frontend,
                                 "the data-only representation fixture");
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

  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  const lang::CppMirBodyEmissionMapRows rows =
      lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp23);
  const auto hasStorage = [&](std::string_view qualified,
                              std::string_view spelling) {
    const auto step = std::find_if(
        frontend.mir.programInitializationPlan().steps.begin(),
        frontend.mir.programInitializationPlan().steps.end(),
        [&](const lang::MirProgramInitializationStep &candidate) {
          const lang::LoweredSymbol *record =
              lowered.findSymbol(candidate.symbol);
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

  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  const lang::MirFunctionInstance *callable = nullptr;
  for (const lang::MirFunctionInstance &instance :
       lowered.mir().functionInstances()) {
    if (std::any_of(instance.parameterTypes.begin(),
                    instance.parameterTypes.end(),
                    [](const lang::SemanticType &type) {
                      return type.kind == lang::SemanticType::Lambda;
                    })) {
      callable = &instance;
      break;
    }
  }
  const lang::LoweredFunctionDeclaration *declaration =
      callable == nullptr
          ? nullptr
          : lowered.findFunctionDeclaration(callable->declaration);
  expect(callable != nullptr && declaration != nullptr,
         "the fixture should retain one concrete lambda-parameter instance");
  if (callable == nullptr || declaration == nullptr) {
    return;
  }

  lang::CppMirBodyEmissionMapRows rows =
      lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp23);
  const lang::CppMirBodyEmissionMap baseMap(rows);
  const lang::CppMirProgramEmissionAnalysis base =
      lang::CppMirBodyEmitter(lowered.mir(), baseMap).analyzeProgram();
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
      lang::cppMirApplyCallableTemplateTypeOverlays(
          rows, lowered, lang::CppStandard::Cpp23, *declaration, *callable);
  expect(overlays && *overlays > 0,
         "the callable instance should derive an exact declaration-level type "
         "overlay");
  if (!overlays || *overlays == 0) {
    return;
  }
  const lang::CppMirBodyEmissionMap overlayMap(std::move(rows));
  const lang::CppMirBodyEmitter overlayEmitter(lowered.mir(), overlayMap);
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
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
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
         "the stale Program should independently pass the frontend");
  const lang::LoweredProgramBuild staleBuild =
      lang::LoweredProgramBuilder().build(
          stale.program, frontend.semantics, frontend.hir, frontend.mir,
          frontend.mir, lang::TargetInfo::host());
  expect(!staleBuild.valid(),
         "LoweredProgram construction should reject an incoherent frontend "
         "snapshot before a backend can observe it");
}

// C++ spellings are derived only from the sealed lowered program and must
// match the bytes the backend emits.
void testRepresentationSpellingAuthorities() {
  const lang::FrontendResult frontend = analyzeRichProgram();
  expect(frontend.canGenerateCode(),
         "the spelling-authority fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::LoweredProgram lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  const lang::LoweredFunctionDeclaration *helper = nullptr;
  const lang::LoweredEnumDeclaration *exitCode = nullptr;
  for (const lang::LoweredDeclaration &declaration : lowered.declarations()) {
    if (declaration.name == "helper") {
      helper =
          std::get_if<lang::LoweredFunctionDeclaration>(&declaration.payload);
    } else if (declaration.name == "ExitCode") {
      exitCode =
          std::get_if<lang::LoweredEnumDeclaration>(&declaration.payload);
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
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);

  const std::string helperSpelling =
      lang::cppFunctionSpelling(*helper, "helper");
  expect(helperSpelling ==
                 "__gti_fn_" + std::to_string(helper->id) + "_helper" &&
             artifact.contents.find(helperSpelling + "(") != std::string::npos,
         "the function-spelling authority should produce the exact emitted "
         "helper definition name");

  lang::SemanticType int32Type;
  int32Type.kind = lang::SemanticType::Int32;
  lang::SemanticType enumType;
  enumType.kind = lang::SemanticType::Enum;
  enumType.enumId = exitCode->id;
  const std::string enumSpelling = lang::cppSemanticTypeSpelling(
      lowered, lang::CppStandard::Cpp20, enumType);
  expect(lang::cppSemanticTypeSpelling(lowered, lang::CppStandard::Cpp20,
                                       int32Type) == "std::int32_t" &&
             artifact.contents.find("std::int32_t") != std::string::npos &&
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
  expectLoweredRowsDeterministic(frontend,
                                 "the closure representation fixture");
  const lang::CppMirBodyEmissionMapRows rows = buildRows(frontend);
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

void testLifecycleCleanupGeneratedItemRows() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "cpp-mir-representation-lifecycle-cleanup.gti", R"(
class OrdinaryCleanup {
  mut int32_t value = 1;
public:
  ~OrdinaryCleanup() { this.value = 0; }
};

class GenericCleanup<T> {
  mut int32_t value = 1;
public:
  ~GenericCleanup() { this.value = 0; }
};

int main() {
  OrdinaryCleanup ordinary = OrdinaryCleanup();
  GenericCleanup<int32_t> generic = GenericCleanup<int32_t>();
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the lifecycle-cleanup fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  expectLoweredRowsDeterministic(frontend, "the lifecycle-cleanup fixture");

  lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O1,
                                        .target = lang::TargetInfo::host()});
  lang::LoweredProgramBuild lowered = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
      optimized.mir, lang::TargetInfo::host());
  expect(optimized.valid() && lowered.valid(),
         "the lifecycle-cleanup fixture should produce LoweredProgram");
  if (!optimized.valid() || !lowered.valid()) {
    return;
  }
  lang::CppMirRepresentationSnapshotBuild build =
      lang::buildCppMirRepresentationSnapshot(*lowered.program);
  expect(build.valid() &&
             thunkCount(*build.snapshot,
                        lang::CppMirThunkKind::LifecycleCleanup) == 2,
         "ordinary and concrete generic destructors should each own one "
         "lifecycle cleanup row");
  if (!build.valid()) {
    return;
  }

  std::size_t ordinary = 0;
  std::size_t specialization = 0;
  bool exact = true;
  for (const lang::CppMirGeneratedThunk &thunk : build.snapshot->thunks) {
    if (thunk.identity.kind != lang::CppMirThunkKind::LifecycleCleanup) {
      continue;
    }
    const auto *payload =
        std::get_if<lang::CppMirLifecycleCleanupThunk>(&thunk.payload);
    const lang::MirDestructorInstance *destructor =
        payload == nullptr
            ? nullptr
            : optimized.mir.findDestructorInstance(payload->destructorInstance);
    const lang::MirClassInstance *owner =
        destructor == nullptr
            ? nullptr
            : optimized.mir.findClassInstance(destructor->owner);
    exact =
        exact && payload != nullptr && destructor != nullptr &&
        owner != nullptr &&
        thunk.sourceKind == lang::CppMirGeneratedThunkSourceKind::Declaration &&
        thunk.sourceDeclaration != 0 &&
        thunk.identity.owner == payload->destructorInstance &&
        payload->classInstance == owner->id &&
        payload->ownerClass == owner->declaration &&
        payload->mayRaiseDefinedFailure == destructor->mayRaiseDefinedFailure;
    ordinary +=
        payload != nullptr &&
        payload->form == lang::CppMirLifecycleCleanupForm::OrdinaryClass;
    specialization +=
        payload != nullptr &&
        payload->form ==
            lang::CppMirLifecycleCleanupForm::ConcreteSpecialization;
  }
  expect(exact && ordinary == 1 && specialization == 1,
         "lifecycle rows should preserve exact destructor provenance and "
         "ordinary-versus-specialization form");

  const lang::CppMirProgramPlan plan =
      lang::planCppMirProgram(optimized.mir, std::move(*build.snapshot));
  expect(plan.complete() && plan.issues.empty() && plan.unsupported.empty(),
         "lifecycle cleanup should be a contracted production family");
}

void testConcreteInstanceGeneratedItemRows() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "cpp-mir-representation-concrete-instances.gti", R"(
class Box<T> {
  T value;
public:
  Box(T initial) : value(initial) {}
  T read() { return this.value; }
};

T identity<T>(T value) { return value; }

int main() {
  Box<int32_t> box = Box<int32_t>(1);
  return identity<int32_t>(box.read()) - 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the concrete-instance fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  expectLoweredRowsDeterministic(frontend, "the concrete-instance fixture");

  lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O1,
                                        .target = lang::TargetInfo::host()});
  lang::LoweredProgramBuild lowered = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
      optimized.mir, lang::TargetInfo::host());
  expect(optimized.valid() && lowered.valid(),
         "the concrete-instance fixture should produce LoweredProgram");
  if (!optimized.valid() || !lowered.valid()) {
    return;
  }
  lang::CppMirRepresentationSnapshotBuild build =
      lang::buildCppMirRepresentationSnapshot(*lowered.program);
  expect(build.valid() &&
             thunkCount(*build.snapshot,
                        lang::CppMirThunkKind::ConcreteInstanceAdapter) == 3,
         "the generic class constructor/member and free function should each "
         "own one concrete-instance row");
  if (!build.valid()) {
    return;
  }

  std::size_t functions = 0;
  std::size_t constructors = 0;
  bool exact = true;
  for (const lang::CppMirGeneratedThunk &thunk : build.snapshot->thunks) {
    if (thunk.identity.kind != lang::CppMirThunkKind::ConcreteInstanceAdapter) {
      continue;
    }
    const auto *payload =
        std::get_if<lang::CppMirConcreteInstanceThunk>(&thunk.payload);
    const auto roots =
        std::find_if(build.snapshot->declarationRoots.begin(),
                     build.snapshot->declarationRoots.end(),
                     [&](const lang::CppMirDeclarationThunkRoots &row) {
                       return row.declaration == thunk.sourceDeclaration;
                     });
    exact =
        exact && payload != nullptr &&
        thunk.sourceKind == lang::CppMirGeneratedThunkSourceKind::Declaration &&
        thunk.sourceDeclaration != 0 &&
        roots != build.snapshot->declarationRoots.end() &&
        std::find(roots->requiredThunks.begin(), roots->requiredThunks.end(),
                  thunk.identity) != roots->requiredThunks.end() &&
        payload->body.owner == thunk.identity.owner;
    if (payload == nullptr) {
      continue;
    }
    if (payload->kind == lang::CppMirConcreteInstanceAdapterKind::Function) {
      const lang::MirFunctionInstance *instance =
          optimized.mir.findFunctionInstance(payload->body.owner);
      exact =
          exact && instance != nullptr &&
          payload->body.kind == lang::MirBodyKind::Function &&
          payload->declaration == instance->declaration &&
          payload->ownerClassInstance == instance->owner.value_or(0) &&
          payload->mayRaiseDefinedFailure == instance->mayRaiseDefinedFailure;
      ++functions;
    } else if (payload->kind ==
               lang::CppMirConcreteInstanceAdapterKind::Constructor) {
      const lang::MirConstructorInstance *instance =
          optimized.mir.findConstructorInstance(payload->body.owner);
      exact =
          exact && instance != nullptr &&
          payload->body.kind == lang::MirBodyKind::Constructor &&
          payload->declaration != 0 &&
          payload->ownerClassInstance == instance->owner &&
          payload->mayRaiseDefinedFailure == instance->mayRaiseDefinedFailure;
      ++constructors;
    } else {
      exact = false;
    }
  }
  expect(exact && functions == 2 && constructors == 1,
         "concrete-instance rows should preserve exact generic declaration, "
         "owner, MIR body, roots, and failure effects");

  const lang::CppMirProgramPlan plan =
      lang::planCppMirProgram(optimized.mir, std::move(*build.snapshot));
  expect(plan.complete() && plan.issues.empty() && plan.unsupported.empty(),
         "concrete instances should be a contracted production family");
}

int main() {
  testExhaustiveSealedInventory();
  testNativeCallbackGeneratedItemRows();
  testUnusedSourceTemplatesRemainInventorySurface();
  testLoweredProgramConstructionSeal();
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
  testLifecycleCleanupGeneratedItemRows();
  testConcreteInstanceGeneratedItemRows();

  if (failures != 0) {
    std::cerr << failures
              << " C++ MIR representation-snapshot test(s) failed\n";
    return 1;
  }
  std::cout << "C++ MIR representation-snapshot tests passed\n";
  return 0;
}
