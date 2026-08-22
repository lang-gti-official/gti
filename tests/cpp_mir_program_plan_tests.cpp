#include "../src/compiler/cpp_mir_program_plan.h"

#include "gti/frontend.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

// Planner unit tests deliberately exercise malformed hand-authored rows. This
// named private peer is the only non-production route that can reseal those
// rows, keeping the production builder as the sole snapshot authority.
struct CppMirRepresentationSnapshotTestAccess {
  static void seal(CppMirRepresentationSnapshot &snapshot) {
    snapshot.inventorySeal_ = CppMirRepresentationSnapshot::InventorySeal{
        .mir = snapshot.mir,
        .bodies = snapshot.bodies,
        .data = snapshot.data,
        .declarationRoots = snapshot.declarationRoots,
        .thunks = snapshot.thunks};
  }
};

} // namespace lang

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool hasIssue(const lang::CppMirProgramPlan &plan,
              lang::CppMirPlanIssueKind kind) {
  return std::any_of(plan.issues.begin(), plan.issues.end(),
                     [kind](const lang::CppMirPlanIssue &issue) {
                       return issue.kind == kind;
                     });
}

bool hasVerificationError(const lang::MirVerificationResult &verification,
                          std::string_view needle) {
  return std::any_of(verification.errors.begin(), verification.errors.end(),
                     [needle](const lang::MirVerificationError &error) {
                       return error.message.find(needle) != std::string::npos;
                     });
}

lang::CppMirProgramPlan
planSnapshotForTesting(const lang::MirProgram &program,
                       lang::CppMirRepresentationSnapshot snapshot) {
  lang::CppMirRepresentationSnapshotTestAccess::seal(snapshot);
  return lang::planCppMirProgram(program, std::move(snapshot));
}

std::size_t unsupportedCount(const lang::CppMirProgramPlan &plan,
                             lang::CppMirUnsupportedSurfaceKind kind) {
  return static_cast<std::size_t>(
      std::count_if(plan.unsupported.begin(), plan.unsupported.end(),
                    [kind](const lang::CppMirUnsupportedSurface &surface) {
                      return surface.kind == kind;
                    }));
}

void printUnexpectedPlan(const lang::CppMirProgramPlan &plan) {
  std::cerr << "  plan status=" << static_cast<int>(plan.status)
            << " bodies=" << plan.bodies.size()
            << " unsupported=" << plan.unsupported.size() << '\n';
  for (const lang::CppMirPlanIssue &issue : plan.issues) {
    std::cerr << "  issue " << static_cast<int>(issue.kind) << ": "
              << issue.detail << '\n';
  }
}

bool isCanonicalNoExecutionInitializer(const lang::MirBody &body) {
  const bool initializer =
      body.kind == lang::MirBodyKind::Module ||
      body.kind == lang::MirBodyKind::FieldInitializers ||
      body.kind == lang::MirBodyKind::StaticFieldInitializers;
  if (!initializer || body.returnType != lang::SemanticType::Void ||
      body.entry != 1 || body.blocks.size() != 1 || !body.places.empty() ||
      !body.loans.empty() || !body.fullExpressions.empty() ||
      !body.cleanupBoundaries.empty() || !body.dropObligations.empty() ||
      !body.failureRecords.empty() || !body.values.empty() ||
      !body.valueUses.empty()) {
    return false;
  }
  lang::MirBlock expected;
  expected.id = 1;
  expected.terminator.kind = lang::MirTerminatorKind::Exit;
  expected.reachable = true;
  return body.blocks.front() == expected;
}

bool hasExecutableProgramInitialization(const lang::MirProgram &program) {
  return std::any_of(
      program.programInitializationPlan().steps.begin(),
      program.programInitializationPlan().steps.end(), [](const auto &step) {
        return step.role == lang::ProgramInitializationStepRole::Initializer;
      });
}

bool hostedStartupCallsProgramInitialization(const lang::MirProgram &program) {
  const lang::MirBody *startup = program.hostedStartup();
  return startup != nullptr &&
         std::any_of(startup->blocks.begin(), startup->blocks.end(),
                     [](const lang::MirBlock &block) {
                       return std::any_of(
                           block.instructions.begin(), block.instructions.end(),
                           [](const lang::MirInstruction &instruction) {
                             return instruction.kind ==
                                        lang::MirInstructionKind::CallBody &&
                                    instruction.bodyTarget ==
                                        lang::MirBodyAddress{
                                            .kind = lang::MirBodyKind::Module,
                                            .owner = 0};
                           });
                     });
}

lang::CppMirBodyRepresentation *
findBody(lang::CppMirRepresentationSnapshot &snapshot,
         lang::MirBodyAddress address);

lang::CppMirRepresentationSnapshot
makeSnapshot(const lang::MirProgram &program) {
  lang::CppMirRepresentationSnapshot snapshot;
  snapshot.mir = program;
  for (const lang::MirBodyAddress address :
       lang::enumerateMirBodyAddresses(program)) {
    const std::optional<lang::CppMirBodyIdentity> identity =
        lang::captureCppMirBodyIdentity(program, address);
    expect(identity.has_value(),
           "every verified MIR body should be capturable");
    if (!identity) {
      continue;
    }

    lang::CppMirBodyRepresentation body{.identity = *identity};
    const lang::MirBody *mirBody = lang::findMirBody(program, address);
    switch (identity->definition) {
    case lang::CppMirBodyDefinitionKind::ImplicitSource:
      if (address.kind == lang::MirBodyKind::Module) {
        const bool executable = hasExecutableProgramInitialization(program);
        body.role = executable ? lang::CppMirBodyRole::SourceExecutable
                               : lang::CppMirBodyRole::DataOnly;
        body.family = executable ? lang::CppMirExecutionFamily::Unsupported
                                 : lang::CppMirExecutionFamily::None;
      } else if (mirBody != nullptr &&
                 isCanonicalNoExecutionInitializer(*mirBody)) {
        body.role = lang::CppMirBodyRole::DataOnly;
        body.family = lang::CppMirExecutionFamily::None;
      } else {
        body.role = lang::CppMirBodyRole::SourceExecutable;
        body.family = lang::CppMirExecutionFamily::Unsupported;
      }
      break;
    case lang::CppMirBodyDefinitionKind::Source:
      body.role = lang::CppMirBodyRole::SourceExecutable;
      body.family = lang::CppMirExecutionFamily::Unsupported;
      break;
    case lang::CppMirBodyDefinitionKind::CompilerGenerated:
      body.role = lang::CppMirBodyRole::SourceExecutable;
      body.family = lang::CppMirExecutionFamily::Unsupported;
      break;
    case lang::CppMirBodyDefinitionKind::RuntimeBinding:
    case lang::CppMirBodyDefinitionKind::Declaration:
      body.role = lang::CppMirBodyRole::AbiDeclaration;
      body.family = lang::CppMirExecutionFamily::None;
      break;
    case lang::CppMirBodyDefinitionKind::Count:
      break;
    }
    snapshot.bodies.push_back(std::move(body));
  }

  const lang::CppMirThunkIdentity initialization{
      .kind = lang::CppMirThunkKind::ProgramInitialization};
  const bool hasInitialization = hasExecutableProgramInitialization(program);
  if (hasInitialization) {
    if (lang::CppMirBodyRepresentation *body =
            findBody(snapshot, {.kind = lang::MirBodyKind::Module})) {
      body->requiredThunks.push_back(initialization);
    }
    snapshot.thunks.push_back(
        {.identity = initialization,
         .sourceBody = {.kind = lang::MirBodyKind::Module, .owner = 0}});
  }

  for (const lang::MirFunctionInstance &function :
       program.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None) {
      continue;
    }
    const lang::CppMirThunkIdentity hosted{
        .kind = lang::CppMirThunkKind::HostedEntry, .owner = function.id};
    if (lang::CppMirBodyRepresentation *body =
            findBody(snapshot, {.kind = lang::MirBodyKind::HostedStartup,
                                .owner = function.id})) {
      body->requiredThunks.push_back(hosted);
    }
    lang::CppMirGeneratedThunk thunk{
        .identity = hosted,
        .sourceBody = {.kind = lang::MirBodyKind::HostedStartup,
                       .owner = function.id}};
    if (hostedStartupCallsProgramInitialization(program)) {
      thunk.dependencies.push_back(initialization);
    }
    snapshot.thunks.push_back(std::move(thunk));
  }
  for (const lang::MirNativeCallbackAdapter &adapter :
       program.nativeCallbackAdapters()) {
    const lang::CppMirThunkIdentity identity{
        .kind = lang::CppMirThunkKind::NativeInteropAdapter,
        .owner = adapter.id};
    lang::CppMirGeneratedThunk thunk{
        .identity = identity,
        .sourceBody = {.kind = lang::MirBodyKind::Function,
                       .owner = adapter.target}};
    thunk.payload = lang::CppMirNativeCallbackThunk{.adapter = adapter};
    snapshot.thunks.push_back(std::move(thunk));

    for (lang::CppMirBodyRepresentation &body : snapshot.bodies) {
      const lang::MirBody *mirBody =
          lang::findMirBody(program, body.identity.address);
      if (mirBody == nullptr) {
        continue;
      }
      const bool used = std::any_of(
          mirBody->blocks.begin(), mirBody->blocks.end(),
          [&](const lang::MirBlock &block) {
            return std::any_of(
                block.instructions.begin(), block.instructions.end(),
                [&](const lang::MirInstruction &instruction) {
                  return instruction.operation ==
                             lang::MirOperation::NativeCallback &&
                         instruction.nativeCallbackAdapter == adapter.id;
                });
          });
      if (used) {
        body.requiredThunks.push_back(identity);
      }
    }
  }
  return snapshot;
}

lang::CppMirBodyRepresentation *
findBody(lang::CppMirRepresentationSnapshot &snapshot,
         lang::MirBodyAddress address) {
  const auto found =
      std::find_if(snapshot.bodies.begin(), snapshot.bodies.end(),
                   [&](const lang::CppMirBodyRepresentation &body) {
                     return body.identity.address == address;
                   });
  return found == snapshot.bodies.end() ? nullptr : &*found;
}

lang::CppMirGeneratedThunk *
findThunk(lang::CppMirRepresentationSnapshot &snapshot,
          lang::CppMirThunkKind kind) {
  const auto found =
      std::find_if(snapshot.thunks.begin(), snapshot.thunks.end(),
                   [kind](const lang::CppMirGeneratedThunk &thunk) {
                     return thunk.identity.kind == kind;
                   });
  return found == snapshot.thunks.end() ? nullptr : &*found;
}

const lang::MirFunctionInstance *findEntry(const lang::MirProgram &program) {
  const auto found = std::find_if(
      program.functionInstances().begin(), program.functionInstances().end(),
      [](const lang::MirFunctionInstance &function) {
        return function.entryKind != lang::ProgramEntryKind::None;
      });
  return found == program.functionInstances().end() ? nullptr : &*found;
}

const lang::MirFunctionInstance *
findFirstNonEntrySourceFunction(const lang::MirProgram &program) {
  const auto found = std::find_if(
      program.functionInstances().begin(), program.functionInstances().end(),
      [](const lang::MirFunctionInstance &function) {
        return function.entryKind == lang::ProgramEntryKind::None &&
               function.definitionKind == lang::MirDefinitionKind::Source;
      });
  return found == program.functionInstances().end() ? nullptr : &*found;
}

lang::FrontendResult analyzeSimpleProgram() {
  return lang::Frontend().analyze("cpp-mir-plan-simple.gti", R"(
extern "C" {
  int32_t native_identity(int32_t value);
  void native_sink(int32_t value);
}

int32_t identity(int32_t value) { return value; }

int main() { return identity(0); }
)");
}

void addClosedThunkGraph(lang::CppMirRepresentationSnapshot &snapshot,
                         const lang::MirProgram &program) {
  const lang::MirFunctionInstance *entry = findEntry(program);
  const lang::MirFunctionInstance *helper =
      findFirstNonEntrySourceFunction(program);
  expect(entry != nullptr && helper != nullptr,
         "the simple fixture should have entry and helper functions");
  if (entry == nullptr || helper == nullptr) {
    return;
  }

  const lang::CppMirThunkIdentity lifecycle{
      .kind = lang::CppMirThunkKind::LifecycleCleanup, .owner = helper->id};
  const lang::CppMirThunkIdentity concrete{
      .kind = lang::CppMirThunkKind::ConcreteInstanceAdapter,
      .owner = helper->id};
  const lang::CppMirThunkIdentity hosted{
      .kind = lang::CppMirThunkKind::HostedEntry, .owner = entry->id};
  snapshot.thunks = {
      {.identity = hosted,
       .sourceBody = {.kind = lang::MirBodyKind::HostedStartup,
                      .owner = entry->id}},
      {.identity = concrete,
       .sourceBody = {.kind = lang::MirBodyKind::Function, .owner = helper->id},
       .dependencies = {lifecycle}},
      {.identity = lifecycle,
       .sourceBody = {.kind = lang::MirBodyKind::Function,
                      .owner = helper->id}}};
  if (lang::CppMirBodyRepresentation *body =
          findBody(snapshot, {.kind = lang::MirBodyKind::Function,
                              .owner = helper->id})) {
    body->requiredThunks = {concrete};
  }
  if (lang::CppMirBodyRepresentation *body =
          findBody(snapshot, {.kind = lang::MirBodyKind::HostedStartup,
                              .owner = entry->id})) {
    body->requiredThunks = {hosted};
  }
}

void testExactInventoryAndBodyRoles() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("cpp-mir-plan-inventory.gti", R"(
extern "C" {
  int32_t native_identity(int32_t value);
}

class Box {
public:
  Box(int32_t value) {}
  ~Box() {}
  int32_t read() { return 1; }
};

int main() {
  Box box = Box(1);
  auto answer = []() -> int32_t { return 42; };
  return answer() + box.read() - 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the whole-body inventory fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  lang::CppMirRepresentationSnapshot snapshot = makeSnapshot(frontend.mir);
  snapshot.data = {{.identity = {.kind = lang::CppMirDataKind::EnumDefinition,
                                 .declaration = 17}},
                   {.identity = {.kind = lang::CppMirDataKind::ConstexprBinding,
                                 .declaration = 3}}};
  const lang::CppMirProgramPlan plan =
      planSnapshotForTesting(frontend.mir, std::move(snapshot));

  std::vector<lang::MirBodyAddress> plannedAddresses;
  for (const lang::CppMirBodyRepresentation &body : plan.bodies) {
    plannedAddresses.push_back(body.identity.address);
  }
  const std::vector<lang::MirBodyAddress> expectedAddresses =
      lang::enumerateMirBodyAddresses(frontend.mir);
  const auto hasKind = [&](lang::MirBodyKind kind) {
    return std::any_of(
        expectedAddresses.begin(), expectedAddresses.end(),
        [kind](lang::MirBodyAddress address) { return address.kind == kind; });
  };
  if (plannedAddresses != expectedAddresses ||
      plan.status != lang::CppMirProgramPlanStatus::UnsupportedSurface) {
    printUnexpectedPlan(plan);
  }
  expect(plannedAddresses == expectedAddresses &&
             hasKind(lang::MirBodyKind::Module) &&
             hasKind(lang::MirBodyKind::FieldInitializers) &&
             hasKind(lang::MirBodyKind::StaticFieldInitializers) &&
             hasKind(lang::MirBodyKind::Function) &&
             hasKind(lang::MirBodyKind::Constructor) &&
             hasKind(lang::MirBodyKind::Destructor) &&
             hasKind(lang::MirBodyKind::Lambda) &&
             hasKind(lang::MirBodyKind::HostedStartup),
         "the plan should retain every core MIR body in exact inventory order");
  expect(plan.status == lang::CppMirProgramPlanStatus::UnsupportedSurface &&
             plan.issues.empty(),
         "a valid unsupported lambda should atomically demote the rich plan");
  expect(plan.data.size() == 2 &&
             plan.data.front().identity.kind ==
                 lang::CppMirDataKind::ConstexprBinding &&
             plan.data.back().identity.kind ==
                 lang::CppMirDataKind::EnumDefinition,
         "data-only constexpr and enum facts should have deterministic order");

  bool sawDeclaration = false;
  bool sawSourceBody = false;
  bool sawDataOnly = false;
  for (const lang::CppMirBodyRepresentation &body : plan.bodies) {
    sawDeclaration =
        sawDeclaration || body.role == lang::CppMirBodyRole::AbiDeclaration;
    sawSourceBody =
        sawSourceBody || body.role == lang::CppMirBodyRole::SourceExecutable;
    sawDataOnly = sawDataOnly || body.role == lang::CppMirBodyRole::DataOnly;
  }
  expect(sawDeclaration && sawSourceBody && sawDataOnly,
         "the inventory should distinguish ABI declarations, executable "
         "source bodies, and data-only initializer roots");
}

void testCoherentInventoryAndThunkClosure() {
  const lang::FrontendResult frontend = analyzeSimpleProgram();
  expect(frontend.canGenerateCode(),
         "the complete-plan fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  lang::CppMirRepresentationSnapshot snapshot = makeSnapshot(frontend.mir);
  addClosedThunkGraph(snapshot, frontend.mir);
  const lang::CppMirProgramPlan plan =
      planSnapshotForTesting(frontend.mir, std::move(snapshot));
  if (plan.status != lang::CppMirProgramPlanStatus::UnsupportedSurface) {
    printUnexpectedPlan(plan);
  }
  expect(plan.status == lang::CppMirProgramPlanStatus::UnsupportedSurface &&
             plan.issues.empty() &&
             unsupportedCount(plan, lang::CppMirUnsupportedSurfaceKind::Body) ==
                 3 &&
             unsupportedCount(plan,
                              lang::CppMirUnsupportedSurfaceKind::Thunk) == 2,
         "transitional family labels and uncontracted thunk kinds should "
         "remain inventory-only unsupported surface");
  expect(plan.thunks.size() == 3 &&
             plan.thunks[0].identity.kind ==
                 lang::CppMirThunkKind::HostedEntry &&
             plan.thunks[1].identity.kind ==
                 lang::CppMirThunkKind::LifecycleCleanup &&
             plan.thunks[2].identity.kind ==
                 lang::CppMirThunkKind::ConcreteInstanceAdapter,
         "generated thunks should be canonicalized dependency before user");
}

void testDataInventoryBelongsToRepresentationSnapshot() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("cpp-mir-plan-data-boundary.gti", R"(
enum class ExitCode : int32_t { success = 0 };
)");
  expect(frontend.canGenerateCode(),
         "the data-inventory boundary fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  lang::CppMirRepresentationSnapshot unsealed = makeSnapshot(frontend.mir);
  const lang::CppMirProgramPlan unsealedPlan =
      lang::planCppMirProgram(frontend.mir, std::move(unsealed));
  expect(unsealedPlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(unsealedPlan,
                      lang::CppMirPlanIssueKind::InvalidInventorySeal),
         "a hand-authored production snapshot must not bypass the private "
         "builder seal");

  // Core MIR cannot reconstruct declaration-only enum/constexpr facts. This
  // private planner test deliberately reseals an empty hand-authored list to
  // exercise that boundary; production snapshots can be sealed only after
  // the representation builder supplies its exhaustive Program inventory.
  lang::CppMirRepresentationSnapshot omitted = makeSnapshot(frontend.mir);
  const lang::CppMirProgramPlan omittedDataPlan =
      planSnapshotForTesting(frontend.mir, std::move(omitted));
  if (!omittedDataPlan.complete()) {
    printUnexpectedPlan(omittedDataPlan);
  }
  expect(omittedDataPlan.complete() && omittedDataPlan.data.empty(),
         "the planner should not pretend MIR can detect an omitted semantic "
         "data row");

  lang::CppMirRepresentationSnapshot supplied = makeSnapshot(frontend.mir);
  supplied.data = {{.identity = {.kind = lang::CppMirDataKind::EnumDefinition,
                                 .declaration = 4}},
                   {.identity = {.kind = lang::CppMirDataKind::ConstexprBinding,
                                 .declaration = 9}}};
  const lang::CppMirProgramPlan suppliedDataPlan =
      planSnapshotForTesting(frontend.mir, std::move(supplied));
  if (!suppliedDataPlan.complete()) {
    printUnexpectedPlan(suppliedDataPlan);
  }
  expect(suppliedDataPlan.complete() && suppliedDataPlan.data.size() == 2,
         "the planner should retain and validate every data row supplied by "
         "the representation snapshot builder");
}

void testExecutableInitializerCannotClaimDataOnly() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("cpp-mir-plan-executable-initializer.gti", R"(
int32_t initial_state() { return 1; }
mut int32_t state = initial_state();
int main() { return state - 1; }
)");
  expect(frontend.canGenerateCode(),
         "the executable-initializer fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  lang::CppMirRepresentationSnapshot snapshot = makeSnapshot(frontend.mir);
  lang::CppMirBodyRepresentation *module =
      findBody(snapshot, {.kind = lang::MirBodyKind::Module});
  expect(module != nullptr &&
             module->role == lang::CppMirBodyRole::SourceExecutable,
         "a represented global initializer should be an executable MIR root");
  if (module != nullptr) {
    module->role = lang::CppMirBodyRole::DataOnly;
    module->family = lang::CppMirExecutionFamily::None;
  }
  const lang::CppMirProgramPlan plan =
      planSnapshotForTesting(frontend.mir, std::move(snapshot));
  expect(plan.status == lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(plan, lang::CppMirPlanIssueKind::InvalidBodyRole),
         "an executable initializer schedule must fail closed when labeled "
         "DataOnly");
}

void testDataOnlyModuleUsesPlanRole() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("cpp-mir-plan-data-only-module.gti", R"(
constexpr int32_t seed = 3;
mut int32_t state;
int main() { return seed - 3 + state; }
)");
  expect(frontend.canGenerateCode(),
         "the data-only Module fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  lang::CppMirRepresentationSnapshot snapshot = makeSnapshot(frontend.mir);
  const lang::CppMirBodyRepresentation *module =
      findBody(snapshot, {.kind = lang::MirBodyKind::Module});
  expect(module != nullptr && !frontend.mir.module().places.empty() &&
             module->role == lang::CppMirBodyRole::DataOnly &&
             module->requiredThunks.empty() &&
             findThunk(snapshot,
                       lang::CppMirThunkKind::ProgramInitialization) == nullptr,
         "tagged implicit-zero/constant storage blocks should remain "
         "DataOnly and create no runtime initialization thunk");
  const lang::CppMirProgramPlan plan =
      planSnapshotForTesting(frontend.mir, std::move(snapshot));
  expect(plan.status == lang::CppMirProgramPlanStatus::UnsupportedSurface &&
             plan.issues.empty(),
         "the planner should accept a noncanonical data-only Module role");
}

void testLegacyStaticExecutionDoesNotInferMergedThunk() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("cpp-mir-plan-legacy-static-row.gti", R"(
class Registry {
public:
  static constexpr int32_t value = 1;
};
int main() { return Registry::value - 1; }
)");
  expect(frontend.canGenerateCode(),
         "the legacy-static row fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  lang::CppMirRepresentationSnapshot snapshot = makeSnapshot(frontend.mir);
  const auto staticBody =
      std::find_if(snapshot.bodies.begin(), snapshot.bodies.end(),
                   [](const lang::CppMirBodyRepresentation &body) {
                     return body.identity.address.kind ==
                            lang::MirBodyKind::StaticFieldInitializers;
                   });
  expect(staticBody != snapshot.bodies.end() &&
             staticBody->role == lang::CppMirBodyRole::DataOnly,
         "the seed fixture should retain one migrated empty static body");
  if (staticBody != snapshot.bodies.end()) {
    // Private planner adversary for a future/legacy generic-static body. Such
    // a row remains executable unsupported inventory, but it cannot infer the
    // distinct merged Module program-initialization contract.
    staticBody->role = lang::CppMirBodyRole::SourceExecutable;
    staticBody->family = lang::CppMirExecutionFamily::Unsupported;
  }
  expect(findThunk(snapshot, lang::CppMirThunkKind::ProgramInitialization) ==
                 nullptr &&
             std::none_of(
                 snapshot.bodies.begin(), snapshot.bodies.end(),
                 [](const lang::CppMirBodyRepresentation &body) {
                   return std::any_of(
                       body.requiredThunks.begin(), body.requiredThunks.end(),
                       [](const auto &id) {
                         return id.kind ==
                                lang::CppMirThunkKind::ProgramInitialization;
                       });
                 }),
         "an executable legacy static row must not create or root the merged "
         "initialization thunk");
  const lang::CppMirProgramPlan plan =
      planSnapshotForTesting(frontend.mir, std::move(snapshot));
  expect(plan.status == lang::CppMirProgramPlanStatus::UnsupportedSurface &&
             plan.issues.empty(),
         "a legacy executable static body should remain atomic unsupported "
         "inventory without corrupting the merged thunk graph");
}

void testMissingDuplicateAndStaleBodyIdentities() {
  const lang::FrontendResult frontend = analyzeSimpleProgram();
  expect(frontend.canGenerateCode(),
         "the body-identity fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  lang::CppMirRepresentationSnapshot missing = makeSnapshot(frontend.mir);
  missing.bodies.pop_back();
  const lang::CppMirProgramPlan missingPlan =
      planSnapshotForTesting(frontend.mir, std::move(missing));
  expect(missingPlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(missingPlan, lang::CppMirPlanIssueKind::MissingBodyRow),
         "a missing body row should fail closed as Incoherent");

  lang::CppMirRepresentationSnapshot duplicate = makeSnapshot(frontend.mir);
  duplicate.bodies.push_back(duplicate.bodies.back());
  const lang::CppMirProgramPlan duplicatePlan =
      planSnapshotForTesting(frontend.mir, std::move(duplicate));
  expect(
      duplicatePlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
          hasIssue(duplicatePlan, lang::CppMirPlanIssueKind::DuplicateBodyRow),
      "a duplicate body row should fail closed as Incoherent");

  lang::CppMirRepresentationSnapshot staleProgram = makeSnapshot(frontend.mir);
  auto &staleFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      staleProgram.mir->functionInstances());
  expect(!staleFunctions.empty(),
         "the copied MIR snapshot should retain function headers");
  if (!staleFunctions.empty()) {
    // constexprFunction is deliberately not a complete MirPrinter identity;
    // exact MirProgram comparison must still observe this drift.
    staleFunctions.front().constexprFunction =
        !staleFunctions.front().constexprFunction;
  }
  const lang::CppMirProgramPlan staleProgramPlan =
      planSnapshotForTesting(frontend.mir, std::move(staleProgram));
  expect(staleProgramPlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(staleProgramPlan,
                      lang::CppMirPlanIssueKind::StaleMirIdentity),
         "a representation snapshot from stale MIR should fail closed");

  lang::CppMirRepresentationSnapshot missingMir = makeSnapshot(frontend.mir);
  missingMir.mir.reset();
  const lang::CppMirProgramPlan missingMirPlan =
      planSnapshotForTesting(frontend.mir, std::move(missingMir));
  expect(
      missingMirPlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
          hasIssue(missingMirPlan, lang::CppMirPlanIssueKind::StaleMirIdentity),
      "an omitted canonical MIR snapshot should fail closed");

  lang::CppMirRepresentationSnapshot staleBody = makeSnapshot(frontend.mir);
  staleBody.bodies.back().identity.placeDomain.revision += 1;
  const lang::CppMirProgramPlan staleBodyPlan =
      planSnapshotForTesting(frontend.mir, std::move(staleBody));
  expect(
      staleBodyPlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
          hasIssue(staleBodyPlan, lang::CppMirPlanIssueKind::StaleBodyIdentity),
      "a stale per-body identity should fail closed");
}

void testDeclarationAndRuntimeCannotClaimExecution() {
  const lang::FrontendResult declarationFrontend = analyzeSimpleProgram();
  expect(declarationFrontend.canGenerateCode(),
         "the declaration-role fixture should pass the frontend");
  if (!declarationFrontend.canGenerateCode()) {
    return;
  }
  lang::CppMirRepresentationSnapshot declaration =
      makeSnapshot(declarationFrontend.mir);
  const auto declarationRow =
      std::find_if(declaration.bodies.begin(), declaration.bodies.end(),
                   [](const lang::CppMirBodyRepresentation &body) {
                     return body.identity.definition ==
                            lang::CppMirBodyDefinitionKind::Declaration;
                   });
  expect(declarationRow != declaration.bodies.end(),
         "the fixture should retain an ABI declaration body row");
  if (declarationRow != declaration.bodies.end()) {
    declarationRow->role = lang::CppMirBodyRole::SourceExecutable;
    declarationRow->family = lang::CppMirExecutionFamily::GeneralV1;
  }
  const lang::CppMirProgramPlan declarationPlan =
      planSnapshotForTesting(declarationFrontend.mir, std::move(declaration));
  expect(
      declarationPlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
          hasIssue(declarationPlan, lang::CppMirPlanIssueKind::InvalidBodyRole),
      "an ABI declaration must not claim executable MIR support");

  lang::MirProgram invalidVoidDeclaration = declarationFrontend.mir;
  auto &voidFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      invalidVoidDeclaration.functionInstances());
  const auto voidDeclaration =
      std::find_if(voidFunctions.begin(), voidFunctions.end(),
                   [](const lang::MirFunctionInstance &function) {
                     return function.definitionKind ==
                                lang::MirDefinitionKind::Declaration &&
                            function.returnType == lang::SemanticType::Void;
                   });
  expect(voidDeclaration != voidFunctions.end(),
         "the fixture should retain a void declaration body");
  if (voidDeclaration != voidFunctions.end()) {
    voidDeclaration->body.blocks.front().terminator.kind =
        lang::MirTerminatorKind::Unreachable;
  }
  const lang::MirVerificationResult invalidVoidVerification =
      lang::verifyMirProgram(invalidVoidDeclaration);
  expect(!invalidVoidVerification.valid() &&
             hasVerificationError(invalidVoidVerification,
                                  "bodyless function declaration"),
         "a void declaration must retain exactly one empty Return block");

  lang::MirProgram invalidNonvoidDeclaration = declarationFrontend.mir;
  auto &nonvoidFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      invalidNonvoidDeclaration.functionInstances());
  const auto nonvoidDeclaration =
      std::find_if(nonvoidFunctions.begin(), nonvoidFunctions.end(),
                   [](const lang::MirFunctionInstance &function) {
                     return function.definitionKind ==
                                lang::MirDefinitionKind::Declaration &&
                            function.returnType != lang::SemanticType::Void;
                   });
  expect(nonvoidDeclaration != nonvoidFunctions.end(),
         "the fixture should retain a nonvoid declaration body");
  if (nonvoidDeclaration != nonvoidFunctions.end()) {
    nonvoidDeclaration->body.blocks.front().terminator.kind =
        lang::MirTerminatorKind::Return;
  }
  const lang::MirVerificationResult invalidNonvoidVerification =
      lang::verifyMirProgram(invalidNonvoidDeclaration);
  expect(!invalidNonvoidVerification.valid() &&
             hasVerificationError(invalidNonvoidVerification,
                                  "bodyless function declaration"),
         "a nonvoid declaration must retain exactly one empty Unreachable "
         "block");

  lang::MirProgram invalidParameterMetadata = declarationFrontend.mir;
  auto &parameterFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          invalidParameterMetadata.functionInstances());
  const auto parameterDeclaration =
      std::find_if(parameterFunctions.begin(), parameterFunctions.end(),
                   [](const lang::MirFunctionInstance &function) {
                     return function.definitionKind ==
                                lang::MirDefinitionKind::Declaration &&
                            !function.parameterTypes.empty();
                   });
  expect(parameterDeclaration != parameterFunctions.end() &&
             !parameterDeclaration->body.places.empty(),
         "the fixture should retain admitted declaration parameter metadata");
  if (parameterDeclaration != parameterFunctions.end() &&
      !parameterDeclaration->body.places.empty()) {
    parameterDeclaration->body.places.front().initiallyAvailable = false;
  }
  const lang::MirVerificationResult invalidParameterVerification =
      lang::verifyMirProgram(invalidParameterMetadata);
  expect(!invalidParameterVerification.valid() &&
             hasVerificationError(invalidParameterVerification,
                                  "bodyless function declaration"),
         "a bodyless declaration must retain only exact admitted parameter "
         "place metadata");

  const std::string runtimeSource = R"(
namespace gti_internal {
namespace runtime {
@runtime("stdin.read_byte")
int32_t read_stdin_byte();
}
}
int main() { return 0; }
)";
  const std::filesystem::path entry = std::filesystem::temp_directory_path() /
                                      "gti-cpp-mir-plan-runtime-prelude.gti";
  const std::string entryKey =
      std::filesystem::weakly_canonical(entry).string();
  const lang::FrontendResult runtimeFrontend = lang::Frontend().analyze(
      entry, runtimeSource, {entry}, {{entryKey, runtimeSource}});
  expect(runtimeFrontend.canGenerateCode(),
         "the trusted runtime-role fixture should pass the frontend");
  if (!runtimeFrontend.canGenerateCode()) {
    return;
  }
  lang::CppMirRepresentationSnapshot runtime =
      makeSnapshot(runtimeFrontend.mir);
  const auto runtimeRow =
      std::find_if(runtime.bodies.begin(), runtime.bodies.end(),
                   [](const lang::CppMirBodyRepresentation &body) {
                     return body.identity.definition ==
                            lang::CppMirBodyDefinitionKind::RuntimeBinding;
                   });
  expect(runtimeRow != runtime.bodies.end(),
         "the trusted fixture should retain a runtime declaration row");
  if (runtimeRow != runtime.bodies.end()) {
    runtimeRow->role = lang::CppMirBodyRole::SourceExecutable;
    runtimeRow->family = lang::CppMirExecutionFamily::GeneralV1;
  }
  const lang::CppMirProgramPlan runtimePlan =
      planSnapshotForTesting(runtimeFrontend.mir, std::move(runtime));
  expect(runtimePlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(runtimePlan, lang::CppMirPlanIssueKind::InvalidBodyRole),
         "a runtime binding must not claim executable MIR support");

  lang::MirProgram invalidRuntime = runtimeFrontend.mir;
  auto &runtimeFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      invalidRuntime.functionInstances());
  const auto runtimeBinding =
      std::find_if(runtimeFunctions.begin(), runtimeFunctions.end(),
                   [](const lang::MirFunctionInstance &function) {
                     return function.definitionKind ==
                            lang::MirDefinitionKind::RuntimeBinding;
                   });
  expect(runtimeBinding != runtimeFunctions.end(),
         "the fixture should retain the runtime binding MIR body");
  if (runtimeBinding != runtimeFunctions.end()) {
    runtimeBinding->body.blocks.front().terminator.kind =
        lang::MirTerminatorKind::Return;
  }
  const lang::MirVerificationResult invalidRuntimeVerification =
      lang::verifyMirProgram(invalidRuntime);
  expect(
      !invalidRuntimeVerification.valid() &&
          hasVerificationError(invalidRuntimeVerification, "runtime binding"),
      "a nonvoid runtime binding must retain its canonical empty "
      "Unreachable body");
}

void testThunkIntegrityFailures() {
  const lang::FrontendResult frontend = analyzeSimpleProgram();
  expect(frontend.canGenerateCode(),
         "the thunk-integrity fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  lang::CppMirRepresentationSnapshot duplicateIdentity =
      makeSnapshot(frontend.mir);
  addClosedThunkGraph(duplicateIdentity, frontend.mir);
  duplicateIdentity.thunks.push_back(duplicateIdentity.thunks.back());
  const lang::CppMirProgramPlan duplicateIdentityPlan =
      planSnapshotForTesting(frontend.mir, std::move(duplicateIdentity));
  expect(duplicateIdentityPlan.status ==
                 lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(duplicateIdentityPlan,
                      lang::CppMirPlanIssueKind::DuplicateThunkIdentity),
         "duplicate generated-thunk IDs should fail closed");

  lang::CppMirRepresentationSnapshot duplicateDependency =
      makeSnapshot(frontend.mir);
  addClosedThunkGraph(duplicateDependency, frontend.mir);
  lang::CppMirGeneratedThunk *duplicateCallable = findThunk(
      duplicateDependency, lang::CppMirThunkKind::ConcreteInstanceAdapter);
  expect(duplicateCallable != nullptr &&
             !duplicateCallable->dependencies.empty(),
         "the closed thunk fixture should retain a callable dependency");
  if (duplicateCallable != nullptr &&
      !duplicateCallable->dependencies.empty()) {
    duplicateCallable->dependencies.push_back(
        duplicateCallable->dependencies.front());
  }
  const lang::CppMirProgramPlan duplicateDependencyPlan =
      planSnapshotForTesting(frontend.mir, std::move(duplicateDependency));
  expect(duplicateDependencyPlan.status ==
                 lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(duplicateDependencyPlan,
                      lang::CppMirPlanIssueKind::DuplicateThunkDependency),
         "duplicate generated-thunk dependencies should fail closed");

  lang::CppMirRepresentationSnapshot missingDependency =
      makeSnapshot(frontend.mir);
  addClosedThunkGraph(missingDependency, frontend.mir);
  if (lang::CppMirGeneratedThunk *callable = findThunk(
          missingDependency, lang::CppMirThunkKind::ConcreteInstanceAdapter)) {
    callable->dependencies = {
        {.kind = lang::CppMirThunkKind::NativeInteropAdapter, .owner = 999}};
  }
  const lang::CppMirProgramPlan missingDependencyPlan =
      planSnapshotForTesting(frontend.mir, std::move(missingDependency));
  expect(missingDependencyPlan.status ==
                 lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(missingDependencyPlan,
                      lang::CppMirPlanIssueKind::MissingThunkDependency),
         "a missing generated-thunk dependency should fail closed");

  lang::CppMirRepresentationSnapshot cyclic = makeSnapshot(frontend.mir);
  addClosedThunkGraph(cyclic, frontend.mir);
  expect(cyclic.thunks.size() == 3,
         "the closed thunk fixture should contain three thunks");
  if (lang::CppMirGeneratedThunk *structural =
          findThunk(cyclic, lang::CppMirThunkKind::LifecycleCleanup)) {
    structural->dependencies = {
        {.kind = lang::CppMirThunkKind::ConcreteInstanceAdapter,
         .owner = structural->identity.owner}};
  }
  const lang::CppMirProgramPlan cyclicPlan =
      planSnapshotForTesting(frontend.mir, std::move(cyclic));
  expect(cyclicPlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(cyclicPlan,
                      lang::CppMirPlanIssueKind::CyclicThunkDependency),
         "a generated-thunk dependency cycle should fail closed");
}

void testExactContractedThunkProvenance() {
  const lang::FrontendResult frontend = analyzeSimpleProgram();
  expect(frontend.canGenerateCode(),
         "the contracted-thunk fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirFunctionInstance *entry = findEntry(frontend.mir);
  const lang::MirFunctionInstance *helper =
      findFirstNonEntrySourceFunction(frontend.mir);
  expect(entry != nullptr && helper != nullptr,
         "the contracted-thunk fixture should retain entry and helper bodies");
  if (entry == nullptr || helper == nullptr) {
    return;
  }

  lang::CppMirRepresentationSnapshot badHostedOrdinal =
      makeSnapshot(frontend.mir);
  addClosedThunkGraph(badHostedOrdinal, frontend.mir);
  for (lang::CppMirGeneratedThunk &thunk : badHostedOrdinal.thunks) {
    if (thunk.identity.kind == lang::CppMirThunkKind::HostedEntry) {
      thunk.identity.ordinal = 1;
    }
  }
  for (lang::CppMirBodyRepresentation &body : badHostedOrdinal.bodies) {
    for (lang::CppMirThunkIdentity &required : body.requiredThunks) {
      if (required.kind == lang::CppMirThunkKind::HostedEntry) {
        required.ordinal = 1;
      }
    }
  }
  const lang::CppMirProgramPlan badHostedOrdinalPlan =
      planSnapshotForTesting(frontend.mir, std::move(badHostedOrdinal));
  expect(badHostedOrdinalPlan.status ==
                 lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(badHostedOrdinalPlan,
                      lang::CppMirPlanIssueKind::InvalidThunkIdentity),
         "a hosted-entry thunk must use exact ordinal zero");

  lang::CppMirRepresentationSnapshot wrongHostedOwner =
      makeSnapshot(frontend.mir);
  addClosedThunkGraph(wrongHostedOwner, frontend.mir);
  for (lang::CppMirGeneratedThunk &thunk : wrongHostedOwner.thunks) {
    if (thunk.identity.kind == lang::CppMirThunkKind::HostedEntry) {
      thunk.sourceBody = {.kind = lang::MirBodyKind::Function,
                          .owner = entry->id};
    }
  }
  const lang::CppMirProgramPlan wrongHostedOwnerPlan =
      planSnapshotForTesting(frontend.mir, std::move(wrongHostedOwner));
  expect(wrongHostedOwnerPlan.status ==
                 lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(wrongHostedOwnerPlan,
                      lang::CppMirPlanIssueKind::InvalidThunkSource),
         "a hosted-entry thunk source must be exact HostedStartup/<entry>, "
         "not the source entry Function");

  lang::CppMirRepresentationSnapshot nonEntryHosted =
      makeSnapshot(frontend.mir);
  addClosedThunkGraph(nonEntryHosted, frontend.mir);
  for (lang::CppMirGeneratedThunk &thunk : nonEntryHosted.thunks) {
    if (thunk.identity.kind == lang::CppMirThunkKind::HostedEntry) {
      thunk.identity.owner = helper->id;
      thunk.sourceBody = {.kind = lang::MirBodyKind::HostedStartup,
                          .owner = helper->id};
    }
  }
  for (lang::CppMirBodyRepresentation &body : nonEntryHosted.bodies) {
    for (lang::CppMirThunkIdentity &required : body.requiredThunks) {
      if (required.kind == lang::CppMirThunkKind::HostedEntry) {
        required.owner = helper->id;
      }
    }
  }
  const lang::CppMirProgramPlan nonEntryHostedPlan =
      planSnapshotForTesting(frontend.mir, std::move(nonEntryHosted));
  expect(nonEntryHostedPlan.status ==
                 lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(nonEntryHostedPlan,
                      lang::CppMirPlanIssueKind::InvalidThunkSource),
         "a hosted-entry thunk owner must carry a program entry kind");

  const lang::FrontendResult initializationFrontend =
      lang::Frontend().analyze("cpp-mir-plan-program-initialization.gti", R"(
int32_t initial_state() { return 1; }
mut int32_t state = initial_state();
int main() { return state - 1; }
)");
  expect(initializationFrontend.canGenerateCode(),
         "the program-initialization thunk fixture should pass the frontend");
  if (!initializationFrontend.canGenerateCode()) {
    return;
  }
  const lang::CppMirThunkIdentity initialization{
      .kind = lang::CppMirThunkKind::ProgramInitialization};
  const auto makeInitializationSnapshot = [&] {
    return makeSnapshot(initializationFrontend.mir);
  };

  const lang::CppMirProgramPlan initializationPlan = planSnapshotForTesting(
      initializationFrontend.mir, makeInitializationSnapshot());
  expect(initializationPlan.status ==
                 lang::CppMirProgramPlanStatus::UnsupportedSurface &&
             initializationPlan.issues.empty() &&
             unsupportedCount(initializationPlan,
                              lang::CppMirUnsupportedSurfaceKind::Thunk) == 0,
         "the exact Module/0 program-initialization thunk should be a "
         "coherent contracted inventory row");

  lang::CppMirRepresentationSnapshot badInitializationIdentity =
      makeInitializationSnapshot();
  badInitializationIdentity.thunks.front().identity.ordinal = 1;
  if (lang::CppMirBodyRepresentation *module = findBody(
          badInitializationIdentity, {.kind = lang::MirBodyKind::Module})) {
    module->requiredThunks.front().ordinal = 1;
  }
  const lang::CppMirProgramPlan badInitializationIdentityPlan =
      planSnapshotForTesting(initializationFrontend.mir,
                             std::move(badInitializationIdentity));
  expect(badInitializationIdentityPlan.status ==
                 lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(badInitializationIdentityPlan,
                      lang::CppMirPlanIssueKind::InvalidThunkIdentity),
         "a program-initialization thunk must use exact owner and ordinal "
         "zero");

  lang::CppMirRepresentationSnapshot wrongInitializationSource =
      makeInitializationSnapshot();
  const lang::MirFunctionInstance *initializationEntry =
      findEntry(initializationFrontend.mir);
  expect(initializationEntry != nullptr,
         "the initialization fixture should retain an entry body");
  if (initializationEntry != nullptr) {
    wrongInitializationSource.thunks.front().sourceBody = {
        .kind = lang::MirBodyKind::Function, .owner = initializationEntry->id};
  }
  const lang::CppMirProgramPlan wrongInitializationSourcePlan =
      planSnapshotForTesting(initializationFrontend.mir,
                             std::move(wrongInitializationSource));
  expect(wrongInitializationSourcePlan.status ==
                 lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(wrongInitializationSourcePlan,
                      lang::CppMirPlanIssueKind::InvalidThunkSource),
         "a program-initialization thunk source must be exact Module/0");

  lang::CppMirRepresentationSnapshot omittedHosted = makeSnapshot(frontend.mir);
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
      planSnapshotForTesting(frontend.mir, std::move(omittedHosted));
  expect(
      omittedHostedPlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
          hasIssue(omittedHostedPlan,
                   lang::CppMirPlanIssueKind::MissingContractedThunk) &&
          hasIssue(omittedHostedPlan,
                   lang::CppMirPlanIssueKind::InvalidContractedThunkGraph),
      "coordinated hosted-entry row/root omission must fail the independently "
      "derived contract");

  lang::CppMirRepresentationSnapshot injectedInitialization =
      makeSnapshot(frontend.mir);
  injectedInitialization.thunks.push_back(
      {.identity = initialization,
       .sourceBody = {.kind = lang::MirBodyKind::Module, .owner = 0}});
  if (lang::CppMirGeneratedThunk *hosted = findThunk(
          injectedInitialization, lang::CppMirThunkKind::HostedEntry)) {
    hosted->dependencies = {initialization};
  }
  const lang::CppMirProgramPlan injectedInitializationPlan =
      planSnapshotForTesting(frontend.mir, std::move(injectedInitialization));
  expect(injectedInitializationPlan.status ==
                 lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(injectedInitializationPlan,
                      lang::CppMirPlanIssueKind::UnexpectedContractedThunk) &&
             hasIssue(injectedInitializationPlan,
                      lang::CppMirPlanIssueKind::InvalidContractedThunkGraph),
         "an injected initialization node/dependency without executable "
         "initializers must fail the independently derived contract");

  lang::CppMirRepresentationSnapshot omittedInitialization =
      makeInitializationSnapshot();
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
  if (lang::CppMirGeneratedThunk *hosted = findThunk(
          omittedInitialization, lang::CppMirThunkKind::HostedEntry)) {
    hosted->dependencies.clear();
  }
  const lang::CppMirProgramPlan omittedInitializationPlan =
      planSnapshotForTesting(initializationFrontend.mir,
                             std::move(omittedInitialization));
  expect(omittedInitializationPlan.status ==
                 lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(omittedInitializationPlan,
                      lang::CppMirPlanIssueKind::MissingContractedThunk) &&
             hasIssue(omittedInitializationPlan,
                      lang::CppMirPlanIssueKind::InvalidContractedThunkGraph),
         "coordinated initialization row/root/dependency omission must fail "
         "the independently derived contract");
}

void testNativeCallbackGeneratedItemInventory() {
  const lang::FrontendResult frontend =
      lang::Frontend().analyze("cpp-mir-plan-native-callback.gti", R"(
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
         "the native callback inventory fixture should retain two exact MIR "
         "adapters");
  if (!frontend.canGenerateCode() ||
      frontend.mir.nativeCallbackAdapters().size() != 2) {
    return;
  }

  const auto makeCallbackSnapshot = [&] { return makeSnapshot(frontend.mir); };
  const lang::CppMirProgramPlan exactPlan =
      planSnapshotForTesting(frontend.mir, makeCallbackSnapshot());
  const std::size_t callbackRows = static_cast<std::size_t>(
      std::count_if(exactPlan.thunks.begin(), exactPlan.thunks.end(),
                    [](const lang::CppMirGeneratedThunk &thunk) {
                      return thunk.identity.kind ==
                             lang::CppMirThunkKind::NativeInteropAdapter;
                    }));
  const bool exactPayloads = std::all_of(
      frontend.mir.nativeCallbackAdapters().begin(),
      frontend.mir.nativeCallbackAdapters().end(),
      [&](const lang::MirNativeCallbackAdapter &adapter) {
        const auto found = std::find_if(
            exactPlan.thunks.begin(), exactPlan.thunks.end(),
            [&](const lang::CppMirGeneratedThunk &thunk) {
              return thunk.identity ==
                     lang::CppMirThunkIdentity{
                         .kind = lang::CppMirThunkKind::NativeInteropAdapter,
                         .owner = adapter.id};
            });
        const auto *payload =
            found == exactPlan.thunks.end()
                ? nullptr
                : std::get_if<lang::CppMirNativeCallbackThunk>(&found->payload);
        return payload != nullptr && payload->adapter == adapter &&
               found->sourceBody ==
                   lang::MirBodyAddress{.kind = lang::MirBodyKind::Function,
                                        .owner = adapter.target};
      });
  expect(exactPlan.coherent() && exactPlan.issues.empty() &&
             callbackRows == 2 && exactPayloads &&
             unsupportedCount(exactPlan,
                              lang::CppMirUnsupportedSurfaceKind::Thunk) == 0,
         "native callback rows should be exact contracted generated items, "
         "not unsupported backend surface");

  lang::CppMirRepresentationSnapshot omitted = makeCallbackSnapshot();
  const lang::CppMirThunkIdentity omittedIdentity{
      .kind = lang::CppMirThunkKind::NativeInteropAdapter, .owner = 1};
  omitted.thunks.erase(
      std::remove_if(omitted.thunks.begin(), omitted.thunks.end(),
                     [&](const lang::CppMirGeneratedThunk &thunk) {
                       return thunk.identity == omittedIdentity;
                     }),
      omitted.thunks.end());
  for (lang::CppMirBodyRepresentation &body : omitted.bodies) {
    body.requiredThunks.erase(std::remove(body.requiredThunks.begin(),
                                          body.requiredThunks.end(),
                                          omittedIdentity),
                              body.requiredThunks.end());
  }
  const lang::CppMirProgramPlan omittedPlan =
      planSnapshotForTesting(frontend.mir, std::move(omitted));
  expect(omittedPlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(omittedPlan,
                      lang::CppMirPlanIssueKind::MissingContractedThunk) &&
             hasIssue(omittedPlan,
                      lang::CppMirPlanIssueKind::InvalidContractedThunkGraph),
         "coordinated callback row/root omission should fail the independently "
         "derived MIR census");

  lang::CppMirRepresentationSnapshot stalePayload = makeCallbackSnapshot();
  const auto stale =
      std::find_if(stalePayload.thunks.begin(), stalePayload.thunks.end(),
                   [](const lang::CppMirGeneratedThunk &thunk) {
                     return thunk.identity.kind ==
                            lang::CppMirThunkKind::NativeInteropAdapter;
                   });
  if (stale != stalePayload.thunks.end()) {
    if (auto *payload =
            std::get_if<lang::CppMirNativeCallbackThunk>(&stale->payload)) {
      payload->adapter = frontend.mir.nativeCallbackAdapters().back();
    }
  }
  const lang::CppMirProgramPlan stalePayloadPlan =
      planSnapshotForTesting(frontend.mir, std::move(stalePayload));
  expect(stalePayloadPlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(stalePayloadPlan,
                      lang::CppMirPlanIssueKind::InvalidThunkPayload),
         "a callback row with stale target or policy facts should fail closed");

  lang::CppMirRepresentationSnapshot reordered = makeCallbackSnapshot();
  std::vector<std::size_t> callbackIndices;
  for (std::size_t index = 0; index < reordered.thunks.size(); ++index) {
    if (reordered.thunks[index].identity.kind ==
        lang::CppMirThunkKind::NativeInteropAdapter) {
      callbackIndices.push_back(index);
    }
  }
  if (callbackIndices.size() == 2) {
    std::swap(reordered.thunks[callbackIndices[0]],
              reordered.thunks[callbackIndices[1]]);
  }
  const lang::CppMirProgramPlan reorderedPlan =
      planSnapshotForTesting(frontend.mir, std::move(reordered));
  expect(reorderedPlan.status == lang::CppMirProgramPlanStatus::Incoherent &&
             hasIssue(reorderedPlan,
                      lang::CppMirPlanIssueKind::InvalidContractedThunkOrder),
         "reordered callback inventory rows should fail the sealed MIR order");
}

void testOneUnsupportedBodyDemotesAtomically() {
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      "cpp-mir-plan-atomic-unsupported.gti", "int helper() { return 0; }");
  expect(frontend.canGenerateCode(),
         "the all-or-nothing fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  lang::CppMirRepresentationSnapshot snapshot = makeSnapshot(frontend.mir);
  const auto supported =
      std::find_if(snapshot.bodies.begin(), snapshot.bodies.end(),
                   [](const lang::CppMirBodyRepresentation &body) {
                     return body.role == lang::CppMirBodyRole::SourceExecutable;
                   });
  expect(supported != snapshot.bodies.end(),
         "the fixture should contain a supported source body");
  if (supported != snapshot.bodies.end()) {
    supported->family = lang::CppMirExecutionFamily::Unsupported;
  }

  const std::size_t expectedBodies = snapshot.bodies.size();
  const lang::CppMirProgramPlan plan =
      planSnapshotForTesting(frontend.mir, std::move(snapshot));
  if (plan.status != lang::CppMirProgramPlanStatus::UnsupportedSurface) {
    printUnexpectedPlan(plan);
  }
  expect(plan.status == lang::CppMirProgramPlanStatus::UnsupportedSurface &&
             plan.issues.empty() && plan.unsupported.size() == 1 &&
             plan.unsupported.front().kind ==
                 lang::CppMirUnsupportedSurfaceKind::Body &&
             plan.bodies.size() == expectedBodies,
         "one valid unsupported body should demote the complete program "
         "without producing a partial plan");
}

} // namespace

int main() {
  testExactInventoryAndBodyRoles();
  testCoherentInventoryAndThunkClosure();
  testDataInventoryBelongsToRepresentationSnapshot();
  testExecutableInitializerCannotClaimDataOnly();
  testDataOnlyModuleUsesPlanRole();
  testLegacyStaticExecutionDoesNotInferMergedThunk();
  testMissingDuplicateAndStaleBodyIdentities();
  testDeclarationAndRuntimeCannotClaimExecution();
  testThunkIntegrityFailures();
  testExactContractedThunkProvenance();
  testNativeCallbackGeneratedItemInventory();
  testOneUnsupportedBodyDemotesAtomically();

  if (failures != 0) {
    std::cerr << failures << " C++ MIR program-plan test(s) failed\n";
    return 1;
  }
  std::cout << "C++ MIR program-plan tests passed\n";
  return 0;
}
