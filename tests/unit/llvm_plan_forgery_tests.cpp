// LLVM backend planning over forged MIR, with no source-level form: the
// concrete parameter-pack, closure-environment, constructor-field and
// stored-field publication plans and the coverage ledger decline
// inconsistent inputs, and the MIR verifier rejects forged restoration
// relays, raw-field witnesses and concrete pack owners before the backend
// sees them. (Restored from the deleted llvm_admission_tests.cpp, minimally
// adapted; its GTI sources became tests/slow/llvm/admission cases.)
#include "../../src/compiler/llvm/closure_environment.h"
#include "../../src/compiler/llvm/constructor_field_publication.h"
#include "../../src/compiler/llvm/coverage.h"
#include "../../src/compiler/llvm/parameter_packs.h"
#include "../../src/compiler/llvm/placement.h"
#include "../../src/compiler/mir_verification_queries.h"
#include "gti/frontend.h"
#include "gti/llvm_admission.h"
#include "gti/llvm_backend.h"
#include "gti/lowered_program_builder.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"
#include "unit_test.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace lang {
struct MirProgramTestAccess {
  static MirProgram
  storageInventory(std::vector<MirClassInstance> classes,
                   std::vector<MirPayloadEnum> payloadEnums = {},
                   std::vector<MirIntegralEnum> integralEnums = {},
                   std::vector<MirLambdaInstance> lambdas = {},
                   std::optional<MirBody> module = std::nullopt) {
    MirProgram program;
    program.classes = std::move(classes);
    program.payloadEnums_ = std::move(payloadEnums);
    program.integralEnums_ = std::move(integralEnums);
    program.lambdas = std::move(lambdas);
    if (module)
      program.moduleBody = std::move(*module);
    return program;
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

void testConcreteParameterPackPlan() {
  using lang::SemanticType;
  SemanticType pack = SemanticType::Unknown;
  pack.kind = SemanticType::TypePack;
  pack.concretePack = true;
  pack.arguments = {SemanticType::Bool, SemanticType::Double};
  const auto prefix = [&](std::vector<SemanticType> parameters,
                          std::vector<lang::HirBindingId> bindings,
                          lang::HirBindingId binding,
                          const SemanticType &type) {
    return lang::native_llvm::concretePackPrefix(parameters, bindings, binding,
                                                 type);
  };

  expect(prefix({SemanticType::Int32, pack}, {1, 3}, 3, pack) == 1,
         "source parameter packs retain one exact trailing ABI boundary");
  expect(prefix({SemanticType::Int32, SemanticType::Bool, SemanticType::Double},
                {1, 3}, 3, pack) == 1,
         "expanded parameter types can retain one source-level pack binding");
  expect(
      !prefix({SemanticType::Int32, SemanticType::Bool, SemanticType::Double},
              {1, 3, 0}, 3, pack),
      "zero-valued pack continuations are not a verified owner binding form");

  SemanticType empty = pack;
  empty.arguments.clear();
  expect(prefix({SemanticType::Int32}, {1, 3}, 3, empty) == 1,
         "an empty pack keeps its source binding without adding an ABI lane");
  expect(
      !prefix({SemanticType::Int32, SemanticType::Double, SemanticType::Bool},
              {1, 3}, 3, pack),
      "pack mapping rejects reordered concrete lanes");
  expect(!prefix({pack, SemanticType::Int32}, {3, 1}, 3, pack),
         "pack mapping rejects a non-trailing source pack");
  SemanticType symbolic = pack;
  symbolic.concretePack = false;
  expect(!prefix({SemanticType::Int32, symbolic}, {1, 3}, 3, symbolic),
         "pack mapping rejects symbolic packs");
  SemanticType nested = pack;
  nested.arguments = {pack};
  expect(!prefix({pack}, {3}, 3, nested),
         "pack mapping rejects nested concrete packs");
  expect(!prefix({SemanticType::Int32}, {1, 0}, 3, empty),
         "empty pack mapping rejects an inconsistent zero-lane binding");
  expect(!prefix({SemanticType::Int32}, {1}, 3, empty),
         "empty packs require their exact trailing source binding");
}

void testConcreteParameterAliases() {
  using namespace lang;
  SemanticType pack = SemanticType::Unknown;
  pack.kind = SemanticType::TypePack;
  pack.concretePack = true;
  pack.arguments = {SemanticType::Bool, SemanticType::Double};
  const PlaceKey packKey{.domain = {.snapshot = 1, .body = 2}, .root = 30};
  PlaceKey elementKey = packKey;
  elementKey.projections.push_back(
      {.kind = PlaceProjectionKind::ConstantIndex, .index = 0});
  const MirPlace root{.id = 1,
                      .root = MirPlaceRootKind::Binding,
                      .binding = 3,
                      .symbol = 30,
                      .type = pack,
                      .key = packKey,
                      .initiallyAvailable = true};
  const MirPlace element{
      .id = 2,
      .root = MirPlaceRootKind::Binding,
      .binding = 3,
      .symbol = 30,
      .projections = {{.kind = MirProjectionKind::PackElement,
                       .constantIndex = 0}},
      .type = SemanticType::Bool,
      .key = elementKey,
      .initiallyAvailable = true};
  MirBody aliases;
  aliases.places = {root, root, element, element};
  aliases.places[1].id = 3;
  aliases.places[3].id = 4;
  const std::vector<SemanticType> parameters{
      SemanticType::Int32, SemanticType::Bool, SemanticType::Double};
  const std::vector<HirBindingId> bindings{1, 3};
  const auto plan =
      native_llvm::planConcreteParameters(parameters, bindings, aliases);
  expect(plan && plan->packs.size() == 1 && plan->lanes.size() == 3 &&
             plan->lanes[1].packElement == 0 && plan->lanes[1].materialized &&
             plan->lanes[2].packElement == 1 && !plan->lanes[2].materialized,
         "same-key pack aliases coalesce into one physical lane");

  MirBody keyless = aliases;
  keyless.places[2].key.reset();
  keyless.places[3].key.reset();
  const auto keylessPlan =
      native_llvm::planConcreteParameters(parameters, bindings, keyless);
  expect(keylessPlan && keylessPlan->lanes.size() == 3 &&
             keylessPlan->lanes[1].packElement == 0 &&
             keylessPlan->lanes[1].materialized,
         "compatible keyless pack views share their verified physical lane");

  MirBody divergent = aliases;
  divergent.places.back().key->projections.front().index = 1;
  expect(!native_llvm::planConcreteParameters(parameters, bindings, divergent),
         "different-key projected aliases fail closed");
  divergent = keyless;
  divergent.places[2].key = elementKey;
  divergent.places[2].key->projections.front().index = 1;
  expect(!native_llvm::planConcreteParameters(parameters, bindings, divergent),
         "a present wrong pack-view key still fails closed");
  divergent = aliases;
  divergent.places[1].key->root = 31;
  expect(!native_llvm::planConcreteParameters(parameters, bindings, divergent),
         "different-key pack roots fail closed");
}

lang::VerifiedLoweredProgram lower(std::string_view source,
                                   bool withStandardLibrary = false,
                                   lang::FrontendOptions options = {}) {
  const auto root = std::filesystem::path(__FILE__)
                        .parent_path()
                        .parent_path()
                        .parent_path() /
                    "stdlib";
  const auto frontend = lang::Frontend(options).analyze(
      "llvm-admission.gti", std::string(source),
      withStandardLibrary
          ? std::vector<std::filesystem::path>{root / "prelude.gti"}
          : std::vector<std::filesystem::path>{},
      {},
      withStandardLibrary ? std::vector<std::filesystem::path>{root}
                          : std::vector<std::filesystem::path>{});
  if (!frontend.canGenerateCode()) {
    throw std::runtime_error(frontend.diagnostics.empty()
                                 ? "fixture did not pass frontend"
                                 : frontend.diagnostics.front().message);
  }
  auto build = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host());
  if (!build.valid())
    throw std::runtime_error(build.issues.front().detail);
  std::vector<lang::LoweredProgramIssue> issues;
  auto verified =
      lang::verifiedLoweredProgram(std::move(*build.program), issues);
  if (!verified)
    throw std::runtime_error(issues.front().detail);
  return *verified;
}

void testVerifiedConcretePackOwners() {
  using namespace lang;
  const auto verified = lower(R"(
enum class Tag { value };
void receive<Args...>(int prefix, Args... args) {}
struct Box {
  Box<Args...>(Tag tag, Args... args) {}
public:
  Box() = default;
};
int invoke<Args...>(Args... args) {
  auto callback = [](int prefix, Args... forwarded) -> int { return prefix; };
  return callback(7, args...);
}
int main() {
  receive(0);
  receive(0, 1, true);
  Box value{Tag::value};
  Box full{Tag::value, 1, true};
  return invoke() + invoke(1, true) - 14;
}
)");
  const auto &mir = verified.program().mir();
  const auto exactEmptyPack = [](const auto &owner) {
    if (owner.parameterBindings.size() != owner.parameterTypes.size() + 1)
      return false;
    const HirBindingId binding = owner.parameterBindings.back();
    const auto root = std::find_if(
        owner.body.places.begin(), owner.body.places.end(),
        [&](const MirPlace &place) {
          return place.root == MirPlaceRootKind::Binding &&
                 place.binding == binding && place.projections.empty() &&
                 place.type.kind == SemanticType::TypePack &&
                 place.type.concretePack && place.type.arguments.empty() &&
                 place.initiallyAvailable;
        });
    if (root == owner.body.places.end())
      return false;
    const auto plan = native_llvm::planConcreteParameters(
        owner.parameterTypes, owner.parameterBindings, owner.body);
    return plan && plan->packs.size() == 1 &&
           plan->packs.front().binding == binding &&
           plan->lanes.size() == owner.parameterTypes.size();
  };
  const auto function = std::find_if(
      mir.functionInstances().begin(), mir.functionInstances().end(),
      [&](const auto &owner) {
        return !owner.parameterTypes.empty() && exactEmptyPack(owner);
      });
  const auto constructor =
      std::find_if(mir.constructorInstances().begin(),
                   mir.constructorInstances().end(), exactEmptyPack);
  const auto lambda = std::find_if(mir.lambdaInstances().begin(),
                                   mir.lambdaInstances().end(), exactEmptyPack);
  expect(function != mir.functionInstances().end(),
         "verified functions retain the source identity of an empty pack");
  expect(constructor != mir.constructorInstances().end(),
         "verified constructors retain the source identity of an empty pack");
  expect(lambda != mir.lambdaInstances().end(),
         "verified lambdas retain the source identity of an empty pack");

  const auto exactTwoElementPack = [](const auto &owner) {
    if (owner.parameterTypes.size() != 3 || owner.parameterBindings.size() != 2)
      return false;
    const HirBindingId binding = owner.parameterBindings.back();
    const auto root = std::find_if(
        owner.body.places.begin(), owner.body.places.end(),
        [&](const MirPlace &place) {
          return place.root == MirPlaceRootKind::Binding &&
                 place.binding == binding && place.projections.empty() &&
                 place.type.kind == SemanticType::TypePack &&
                 place.type.concretePack && place.type.arguments.size() == 2;
        });
    const auto plan = native_llvm::planConcreteParameters(
        owner.parameterTypes, owner.parameterBindings, owner.body);
    return root != owner.body.places.end() && plan && plan->lanes.size() == 3 &&
           plan->packs.size() == 1 && plan->lanes[1].packElement == 0 &&
           plan->lanes[2].packElement == 1;
  };
  expect(std::any_of(mir.functionInstances().begin(),
                     mir.functionInstances().end(), exactTwoElementPack),
         "verified functions retain fixed-prefix heterogeneous pack lanes");
  expect(std::any_of(mir.constructorInstances().begin(),
                     mir.constructorInstances().end(), exactTwoElementPack),
         "verified constructors retain fixed-prefix heterogeneous pack lanes");
  expect(std::any_of(mir.lambdaInstances().begin(), mir.lambdaInstances().end(),
                     exactTwoElementPack),
         "verified lambdas retain fixed-prefix heterogeneous pack lanes");

  enum class Mutation { Missing, Wrong, Foreign, Extra, Conflicting };
  const auto rejectsMutation = [&](MirBodyKind kind, std::size_t owner,
                                   Mutation mutation) {
    MirProgram malformed = mir;
    std::vector<HirBindingId> *bindings = nullptr;
    MirBody *body = nullptr;
    if (kind == MirBodyKind::Function)
      if (auto &instance = const_cast<MirFunctionInstance &>(
              *malformed.findFunctionInstance(owner));
          true) {
        bindings = &instance.parameterBindings;
        body = &instance.body;
      }
    if (kind == MirBodyKind::Constructor)
      if (auto &instance = const_cast<MirConstructorInstance &>(
              *malformed.findConstructorInstance(owner));
          true) {
        bindings = &instance.parameterBindings;
        body = &instance.body;
      }
    if (kind == MirBodyKind::Lambda)
      if (auto &instance =
              const_cast<MirLambdaInstance &>(*malformed.findLambda(owner));
          true) {
        bindings = &instance.parameterBindings;
        body = &instance.body;
      }
    if (!bindings || !body || bindings->empty())
      return false;
    if (mutation == Mutation::Missing)
      bindings->pop_back();
    else if (mutation == Mutation::Wrong)
      bindings->back() = bindings->front();
    else if (mutation == Mutation::Foreign)
      bindings->back() = 999999;
    else if (mutation == Mutation::Extra)
      bindings->push_back(bindings->back());
    else {
      const auto root = std::find_if(
          body->places.begin(), body->places.end(), [&](const MirPlace &place) {
            return place.root == MirPlaceRootKind::Binding &&
                   place.binding == bindings->back() &&
                   place.projections.empty() &&
                   place.type.kind == SemanticType::TypePack;
          });
      if (root == body->places.end() || !root->key)
        return false;
      MirPlace alias = *root;
      alias.id = std::max_element(body->places.begin(), body->places.end(),
                                  [](const auto &left, const auto &right) {
                                    return left.id < right.id;
                                  })
                     ->id +
                 1;
      ++alias.key->root;
      body->places.push_back(std::move(alias));
    }
    return !verifyMirProgram(malformed).valid();
  };
  const auto checkMutations = [&](MirBodyKind kind, std::size_t owner,
                                  std::string_view name) {
    for (const auto mutation :
         {Mutation::Missing, Mutation::Wrong, Mutation::Foreign,
          Mutation::Extra, Mutation::Conflicting})
      expect(rejectsMutation(kind, owner, mutation),
             std::string(name) +
                 " empty packs reject malformed formal/root identity");
  };
  if (function != mir.functionInstances().end())
    checkMutations(MirBodyKind::Function, function->id, "function");
  if (constructor != mir.constructorInstances().end())
    checkMutations(MirBodyKind::Constructor, constructor->id, "constructor");
  if (lambda != mir.lambdaInstances().end())
    checkMutations(MirBodyKind::Lambda, lambda->id, "lambda");
}

void testVerifiedRestorationRelay() {
  using namespace lang;

  const auto verified = lower(R"(
import std.memory;
class Cell {
  mut int& value;
public:
  Cell(mut int& value) : value(value) {}
  int read() { return this.value; }
  void add(int amount) mut { this.value += amount; }
};
unsafe Cell& relay_read(std::erased_object& object) {
  unsafe { Cell& cell = std::restore_ref<Cell>(object); return cell; }
}
unsafe mut Cell& relay_mut(mut std::erased_object& object) {
  unsafe { mut Cell& cell = std::restore_mut<Cell>(object); return cell; }
}
int main() {
  mut int value = 3;
  mut Cell cell{value};
  { std::erased_object& object = std::erase_ref(cell);
    unsafe { if (relay_read(object).read() != 3) return 1; } }
  { mut std::erased_object& object = std::erase_mut(cell);
    unsafe { relay_mut(object).add(2); } }
  return cell.read() == 5 ? 0 : 2;
}
)",
                              true);
  const auto &mir = verified.program().mir();
  struct Witness {
    HirFunctionInstanceId owner = 0;
    MirInstructionId instruction = 0;
    AccessMode access = AccessMode::ReadOnly;
  };
  std::vector<Witness> witnesses;
  for (const auto &function : mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.functionTarget && instruction.restoresReferenceResult &&
            mir_detail::verifiedReferenceRestoreCall(mir, function.body,
                                                     instruction))
          witnesses.push_back(
              {function.id, instruction.id, instruction.borrowAccess});
  expect(std::any_of(witnesses.begin(), witnesses.end(),
                     [](const auto &entry) {
                       return entry.access == AccessMode::ReadOnly;
                     }) &&
             std::any_of(witnesses.begin(), witnesses.end(),
                         [](const auto &entry) {
                           return entry.access == AccessMode::Mutable;
                         }),
         "unrelated read and mutable source helpers retain one verified finite "
         "restoration chain");
  const auto found =
      std::find_if(witnesses.begin(), witnesses.end(), [](const auto &entry) {
        return entry.access == AccessMode::ReadOnly;
      });
  if (found == witnesses.end())
    return;
  const Witness witness = *found;

  const auto functionAt =
      [](MirProgram &program,
         HirFunctionInstanceId id) -> MirFunctionInstance & {
    auto &functions = const_cast<std::vector<MirFunctionInstance> &>(
        program.functionInstances());
    return functions.at(id - 1);
  };
  const auto instructionAt = [](MirBody &body,
                                MirInstructionId id) -> MirInstruction & {
    for (auto &block : body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == id)
          return instruction;
    throw std::runtime_error("restoration witness instruction disappeared");
  };
  const auto rejects = [&](MirProgram program, const auto &mutation) -> bool {
    auto &function = functionAt(program, witness.owner);
    auto &call = instructionAt(function.body, witness.instruction);
    mutation(program, function, call);
    return !mir_detail::verifiedReferenceRestoreCall(program, function.body,
                                                     call);
  };

  expect(rejects(mir,
                 [&](MirProgram &program, MirFunctionInstance &,
                     MirInstruction &call) {
                   functionAt(program, *call.functionTarget)
                       .restoresReferenceResult = false;
                 }),
         "a restoration helper summary is not authority without its exact "
         "verified target summary");
  expect(rejects(mir,
                 [](MirProgram &, MirFunctionInstance &, MirInstruction &call) {
                   call.restoredBorrowFields.clear();
                 }),
         "restoration rejects incomplete restored-field inventory");
  expect(rejects(mir,
                 [](MirProgram &, MirFunctionInstance &, MirInstruction &call) {
                   call.borrowOrigin = BorrowOriginKind::Receiver;
                 }),
         "restoration rejects a changed argument-origin contract");
  expect(rejects(mir,
                 [](MirProgram &, MirFunctionInstance &, MirInstruction &call) {
                   call.borrowAccess = AccessMode::Mutable;
                 }),
         "restoration rejects access stronger than its opaque witness");
  expect(rejects(mir,
                 [&](MirProgram &program, MirFunctionInstance &,
                     MirInstruction &call) {
                   auto &target = functionAt(program, *call.functionTarget);
                   for (auto &block : target.body.blocks)
                     for (auto &producer : block.instructions)
                       if (producer.restoresReferenceResult) {
                         producer.intrinsic = IntrinsicKind::None;
                         producer.functionTarget = target.id;
                       }
                 }),
         "a cyclic helper summary without an intrinsic endpoint rejects");
  expect(rejects(mir,
                 [&](MirProgram &program, MirFunctionInstance &,
                     MirInstruction &call) {
                   auto &target = functionAt(program, *call.functionTarget);
                   for (auto &block : target.body.blocks)
                     for (auto &producer : block.instructions)
                       if (producer.restoresReferenceResult)
                         producer.tombstone = true;
                 }),
         "a tombstoned restore producer cannot witness executable authority");
}

void testClosureEnvironmentPlan() {
  using namespace lang;
  using native_llvm::closureCleanupInstance;
  using native_llvm::closureEnvironmentLayout;

  const auto verified = lower(R"(
int invoke<F>(F callable) { return callable(); }
class Field {
  int value = invoke([]() -> int { return 17; });
public:
  int read() { return this.value; }
public:
  Field() = default;
};
int main() { Field value{}; return value.read() - 17; }
)");
  const auto &mir = verified.program().mir();
  const MirLambdaInstance *first = nullptr;
  const MirLambdaInstance *second = nullptr;
  for (const auto &candidate : mir.lambdaInstances()) {
    if (first == nullptr)
      first = &candidate;
    else if (candidate.type == first->type) {
      second = &candidate;
      break;
    }
  }
  expect(first != nullptr && second != nullptr,
         "field initializers retain duplicate exact lambda bodies");
  if (first == nullptr || second == nullptr)
    return;
  expect(closureEnvironmentLayout(mir, first->type).has_value(),
         "compatible exact lambda bodies share one physical environment");

  MirLambdaInstance ownedBorrowed;
  ownedBorrowed.captureTypes = {
      SemanticType::classType(900),
      SemanticType::referenceTo(SemanticType::Int32, AccessMode::Mutable)};
  ownedBorrowed.captureModes = {LambdaCaptureMode::Move,
                                LambdaCaptureMode::BorrowMut};
  ownedBorrowed.captureRequiresActiveCleanup = {true, false};
  MirLambdaInstance compatible = ownedBorrowed;
  compatible.id = 2;
  compatible.parameterBindings = {42};
  expect(closureEnvironmentLayout(ownedBorrowed) ==
             closureEnvironmentLayout(compatible),
         "owned and borrowed captures compare only physical environment "
         "facts, not body-local identities");
  compatible.captureModes[1] = LambdaCaptureMode::BorrowRead;
  expect(closureEnvironmentLayout(ownedBorrowed) !=
             closureEnvironmentLayout(compatible),
         "capture access modes cannot share a closure environment plan");
  compatible = ownedBorrowed;
  compatible.captureRequiresActiveCleanup[0] = false;
  expect(closureEnvironmentLayout(ownedBorrowed) !=
             closureEnvironmentLayout(compatible),
         "capture cleanup-state requirements cannot share an environment");
  compatible = ownedBorrowed;
  compatible.captureTypes[0] = SemanticType::Int32;
  expect(closureEnvironmentLayout(ownedBorrowed) !=
             closureEnvironmentLayout(compatible),
         "different ordered capture storage cannot share an environment");

  MirProgram incompatibleMode = mir;
  auto &modeInstances = const_cast<std::vector<MirLambdaInstance> &>(
      incompatibleMode.lambdaInstances());
  modeInstances[second->id - 1].captureModes.push_back(LambdaCaptureMode::Copy);
  expect(!closureEnvironmentLayout(incompatibleMode, first->type),
         "incompatible duplicate capture modes fail closed");
  MirProgram incompatibleCleanup = mir;
  auto &cleanupInstances = const_cast<std::vector<MirLambdaInstance> &>(
      incompatibleCleanup.lambdaInstances());
  cleanupInstances[second->id - 1].captureRequiresActiveCleanup.push_back(
      false);
  expect(!closureEnvironmentLayout(incompatibleCleanup, first->type),
         "incompatible duplicate cleanup state fails closed");
  MirProgram incompatibleType = mir;
  auto &typeInstances = const_cast<std::vector<MirLambdaInstance> &>(
      incompatibleType.lambdaInstances());
  typeInstances[second->id - 1].captureTypes.push_back(SemanticType::Int32);
  expect(!closureEnvironmentLayout(incompatibleType, first->type),
         "incompatible duplicate capture storage fails closed");

  MirDropType cleanup{.type = first->type, .lambdaInstance = first->id};
  expect(closureCleanupInstance(mir, cleanup) == first,
         "closure cleanup resolves its exact recorded lambda instance");
  cleanup.lambdaInstance = mir.lambdaInstances().size() + 1;
  expect(closureCleanupInstance(mir, cleanup) == nullptr,
         "closure cleanup rejects a missing exact instance");
  cleanup.lambdaInstance = first->id;
  cleanup.type = SemanticType::Int32;
  expect(closureCleanupInstance(mir, cleanup) == nullptr,
         "closure cleanup rejects an identity/type mismatch");
}

void testUnstagedConstructorFieldPublicationPlan() {
  using namespace lang;
  using native_llvm::UnstagedConstructorFieldPublicationTransport;

  const auto verified = lower(R"(
class Holder<F> {
  F callable;
public:
  Holder(F input) : callable(input) {}
  int invoke() { return this.callable(); }
};
int run<F>(F callable) {
  Holder<F> holder{callable};
  return holder.invoke();
}
int main() {
  return run([]() -> int { return 7; }) - 7;
}
)");
  const auto &mir = verified.program().mir();
  const auto admission = planLlvmAdmission(verified);
  const auto publicationUse = std::find_if(
      admission.requirements().begin(), admission.requirements().end(),
      [](const auto &use) {
        return use.entry->family == "Shape" &&
               use.entry->name == "UnstagedConstructorFieldPublication";
      });
  expect(admission.admitted() &&
             publicationUse != admission.requirements().end() &&
             publicationUse->entry->state == LlvmCoverageState::Pending &&
             publicationUse->implementedShape &&
             publicationUse->implementedShape->name == "NativeCallableStorage",
         "an exact callable record publication witnesses the pending shared "
         "constructor requirement");

  const MirConstructorInstance *publicationConstructor = nullptr;
  const MirInstruction *publicationInstruction = nullptr;
  native_llvm::UnstagedConstructorFieldPublicationPlan publication;
  for (const auto &constructor : mir.constructorInstances())
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions) {
        auto candidate = native_llvm::unstagedConstructorFieldPublication(
            mir, constructor.body, constructor.id, instruction);
        if (candidate.matched() &&
            candidate.sourceType.kind == SemanticType::Lambda) {
          publicationConstructor = &constructor;
          publicationInstruction = &instruction;
          publication = candidate;
        }
      }
  expect(publicationConstructor && publicationInstruction &&
             publication.exact && publication.value != 0 &&
             publication.sourceType == publication.targetType &&
             publication.transport ==
                 UnstagedConstructorFieldPublicationTransport::ExactValue,
         "the callable field initializer has one exact owner, field and "
         "result publication plan");
  if (!publicationConstructor || !publicationInstruction)
    return;

  const auto constructorId = publicationConstructor->id;
  const auto instructionId = publicationInstruction->id;
  const auto initializerIndex =
      static_cast<std::size_t>(publication.initializer -
                               publicationConstructor->initializers.data()) +
      1;
  const auto mutateConstructor =
      [&](MirProgram &program) -> MirConstructorInstance & {
    auto &constructors = const_cast<std::vector<MirConstructorInstance> &>(
        program.constructorInstances());
    return constructors.at(constructorId - 1);
  };
  const auto findInstruction = [&](MirBody &body) -> MirInstruction * {
    for (auto &block : body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == instructionId)
          return &instruction;
    return nullptr;
  };

  MirProgram wrongResult = mir;
  auto &wrongBody = mutateConstructor(wrongResult).body;
  auto *wrongInstruction = findInstruction(wrongBody);
  expect(wrongInstruction != nullptr,
         "publication mutation retains its exact instruction");
  if (wrongInstruction) {
    wrongBody.values.at(*wrongInstruction->result - 1).definition = 0;
    const auto wrong = native_llvm::unstagedConstructorFieldPublication(
        wrongResult, wrongBody, constructorId, *wrongInstruction);
    expect(wrong.matched() && !wrong.exact,
           "a record whose SSA result has the wrong producer fails "
           "publication correspondence");
  }

  MirProgram stagedSibling = mir;
  auto &stagedBody = mutateConstructor(stagedSibling).body;
  MirInstruction staged;
  staged.id = stagedBody.instructionCount() + 1;
  staged.kind = MirInstructionKind::Initialize;
  staged.constructorInitializer = initializerIndex;
  stagedBody.blocks.front().instructions.push_back(staged);
  auto *stagedOriginal = findInstruction(stagedBody);
  expect(stagedOriginal &&
             !native_llvm::unstagedConstructorFieldPublication(
                  stagedSibling, stagedBody, constructorId, *stagedOriginal)
                  .matched(),
         "an explicit Initialize sibling owns publication instead of the "
         "record-only hook");

  MirProgram duplicate = mir;
  auto &duplicateBody = mutateConstructor(duplicate).body;
  auto *original = findInstruction(duplicateBody);
  expect(original != nullptr,
         "duplicate-publication mutation retains its source record");
  if (original) {
    MirInstruction sibling = *original;
    sibling.id = duplicateBody.instructionCount() + 1;
    sibling.result = duplicateBody.values.size() + 1;
    MirValue value = duplicateBody.values.at(*original->result - 1);
    value.id = *sibling.result;
    value.definition = sibling.id;
    duplicateBody.values.push_back(value);
    duplicateBody.valueUses.emplace_back();
    duplicateBody.blocks.front().instructions.push_back(sibling);
    original = findInstruction(duplicateBody);
    const auto ambiguous = native_llvm::unstagedConstructorFieldPublication(
        duplicate, duplicateBody, constructorId, *original);
    expect(ambiguous.matched() && !ambiguous.exact,
           "two record-only producers cannot claim one constructor field");
  }

  const auto hierarchyField = lower(R"(
class P {
  int value;
public:
  P(int input) : value(input) {}
  virtual int read() { return this.value; }
};
class Holder<F> {
public:
  F callable;
  Holder(F input) : callable(input) {}
};
int store<F>(F callable) {
  Holder<F> holder{callable};
  return 0;
}
int main() {
  P value{7};
  return store([value]() -> int { return value.read(); });
}
)");
  const auto &hierarchyMir = hierarchyField.program().mir();
  bool hierarchyPublication = false;
  for (const auto &constructor : hierarchyMir.constructorInstances())
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto candidate = native_llvm::unstagedConstructorFieldPublication(
            hierarchyMir, constructor.body, constructor.id, instruction);
        if (!candidate.matched() ||
            candidate.transport !=
                UnstagedConstructorFieldPublicationTransport::ExactValue ||
            !native_llvm::containsHierarchyStorage(hierarchyMir,
                                                   candidate.targetType))
          continue;
        hierarchyPublication |=
            candidate.exact && candidate.sourceType == candidate.targetType;
      }
  const auto hierarchyAdmission = planLlvmAdmission(hierarchyField);
  expect(hierarchyPublication,
         "the hierarchy field fixture retains an exact record-only "
         "publication plan");
  expect(!hierarchyAdmission.admitted(),
         "a nested hierarchy field publication remains outside public LLVM "
         "admission");
  expect(std::any_of(hierarchyAdmission.requirements().begin(),
                     hierarchyAdmission.requirements().end(),
                     [](const auto &use) {
                       return use.required &&
                              use.entry->key() ==
                                  "Shape.UnstagedConstructorFieldPublication" &&
                              !use.implementedShape;
                     }),
         "the nested hierarchy field keeps an explicit unsupported "
         "constructor-publication requirement");

  const auto emptyOwner = lower(R"(
import std.memory;
int main() {
  std::unique_ptr<int> owner = nullptr;
  return owner == nullptr ? 0 : 1;
}
)",
                                true);
  const auto &ownerMir = emptyOwner.program().mir();
  const MirConstructorInstance *ownerConstructor = nullptr;
  const MirInstruction *ownerInstruction = nullptr;
  native_llvm::UnstagedConstructorFieldPublicationPlan ownerPublication;
  for (const auto &constructor : ownerMir.constructorInstances())
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions) {
        auto candidate = native_llvm::unstagedConstructorFieldPublication(
            ownerMir, constructor.body, constructor.id, instruction);
        if (candidate.matched() &&
            candidate.transport ==
                UnstagedConstructorFieldPublicationTransport::NullPointer) {
          ownerConstructor = &constructor;
          ownerInstruction = &instruction;
          ownerPublication = candidate;
        }
      }
  expect(ownerConstructor && ownerInstruction && ownerPublication.exact &&
             ownerPublication.value != 0 &&
             ownerPublication.sourceType == SemanticType::NullPtr &&
             ownerPublication.targetType.kind == SemanticType::UniqueOwner &&
             ownerPublication.targetType.arguments.size() == 1 &&
             ownerPublication.targetType.arguments.front() ==
                 SemanticType::Int32,
         "a source null literal has one exact typed unique-owner field "
         "publication transport");
  if (!ownerConstructor || !ownerInstruction)
    return;

  const auto ownerAdmission = planLlvmAdmission(emptyOwner);
  const auto ownerUse = std::find_if(
      ownerAdmission.requirements().begin(),
      ownerAdmission.requirements().end(), [&](const auto &use) {
        return use.entry->family == "Shape" &&
               use.entry->name == "UnstagedConstructorFieldPublication" &&
               use.body && use.body->kind == MirBodyKind::Constructor &&
               use.body->owner == ownerConstructor->id &&
               use.instruction == ownerInstruction->id;
      });
  expect(ownerUse != ownerAdmission.requirements().end(),
         "null-owner publication retains its exact admission witness");
  if (ownerUse != ownerAdmission.requirements().end()) {
    expect(!ownerUse->implementedShape ||
               ownerUse->implementedShape->name == "NativeUniqueOwnership",
           "null-owner publication accepts only the O-LLVM-08 native unique "
           "ownership shape");
    if (!ownerUse->implementedShape)
      expect(!ownerAdmission.admitted(),
             "the O-LLVM-09 component fails closed until O-LLVM-08 supplies "
             "native unique ownership");
  }

  const auto ownerConstructorId = ownerConstructor->id;
  const auto ownerInstructionId = ownerInstruction->id;
  const auto ownerInitializerIndex = static_cast<std::size_t>(
      ownerPublication.initializer - ownerConstructor->initializers.data());
  const auto mutateOwnerConstructor =
      [&](MirProgram &program) -> MirConstructorInstance & {
    auto &constructors = const_cast<std::vector<MirConstructorInstance> &>(
        program.constructorInstances());
    const auto selected = std::find_if(
        constructors.begin(), constructors.end(), [&](const auto &candidate) {
          return candidate.id == ownerConstructorId;
        });
    return *selected;
  };
  const auto findOwnerInstruction = [&](MirBody &body) -> MirInstruction * {
    for (auto &block : body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == ownerInstructionId)
          return &instruction;
    return nullptr;
  };

  MirProgram nonnullOwner = ownerMir;
  auto &nonnullBody = mutateOwnerConstructor(nonnullOwner).body;
  auto *nonnullInstruction = findOwnerInstruction(nonnullBody);
  expect(nonnullInstruction != nullptr,
         "nonnull mutation retains its publication instruction");
  if (nonnullInstruction) {
    nonnullInstruction->literal = std::uint64_t{1};
    const auto rejected = native_llvm::unstagedConstructorFieldPublication(
        nonnullOwner, nonnullBody, ownerConstructorId, *nonnullInstruction);
    expect(rejected.matched() && !rejected.exact,
           "a nonnull literal cannot acquire the null-owner transport");
  }

  MirProgram wrongSource = ownerMir;
  auto &wrongSourceBody = mutateOwnerConstructor(wrongSource).body;
  auto *wrongSourceInstruction = findOwnerInstruction(wrongSourceBody);
  expect(wrongSourceInstruction != nullptr,
         "wrong-source mutation retains its publication instruction");
  if (wrongSourceInstruction) {
    wrongSourceInstruction->info.type =
        SemanticType::rawPointerTo(SemanticType::Int32, AccessMode::ReadOnly);
    wrongSourceBody.values.at(*wrongSourceInstruction->result - 1).info.type =
        wrongSourceInstruction->info.type;
    const auto rejected = native_llvm::unstagedConstructorFieldPublication(
        wrongSource, wrongSourceBody, ownerConstructorId,
        *wrongSourceInstruction);
    expect(rejected.matched() && !rejected.exact,
           "a differently typed pointer cannot acquire the null-owner "
           "transport");
  }

  MirProgram effectfulOwner = ownerMir;
  auto &effectfulBody = mutateOwnerConstructor(effectfulOwner).body;
  auto *effectfulInstruction = findOwnerInstruction(effectfulBody);
  expect(effectfulInstruction != nullptr,
         "effect mutation retains its publication instruction");
  if (effectfulInstruction) {
    effectfulInstruction->definedFailure.propagation =
        FailurePropagationKind::DirectCall;
    const auto rejected = native_llvm::unstagedConstructorFieldPublication(
        effectfulOwner, effectfulBody, ownerConstructorId,
        *effectfulInstruction);
    expect(rejected.matched() && !rejected.exact,
           "an effectful instruction cannot acquire the source-null owner "
           "transport");
  }

  MirProgram unsupportedPointee = ownerMir;
  auto &unsupportedConstructor = mutateOwnerConstructor(unsupportedPointee);
  auto &unsupportedInitializer =
      unsupportedConstructor.initializers.at(ownerInitializerIndex);
  unsupportedInitializer.targetType =
      SemanticType::uniqueOwnerOf(SemanticType::Unknown);
  auto &ownerClasses = const_cast<std::vector<MirClassInstance> &>(
      unsupportedPointee.classInstances());
  const auto unsupportedClass = std::find_if(
      ownerClasses.begin(), ownerClasses.end(), [&](const auto &candidate) {
        return candidate.id == unsupportedConstructor.owner;
      });
  expect(unsupportedClass != ownerClasses.end(),
         "unsupported-pointee mutation retains its owner class");
  if (unsupportedClass != ownerClasses.end()) {
    const auto field = std::find_if(
        unsupportedClass->declaredFields.begin(),
        unsupportedClass->declaredFields.end(), [&](const auto &candidate) {
          return candidate.symbol == unsupportedInitializer.field;
        });
    expect(field != unsupportedClass->declaredFields.end(),
           "unsupported-pointee mutation retains its owner field");
    if (field != unsupportedClass->declaredFields.end())
      field->type = unsupportedInitializer.targetType;
  }
  auto *unsupportedInstruction =
      findOwnerInstruction(unsupportedConstructor.body);
  if (unsupportedInstruction) {
    const auto rejected = native_llvm::unstagedConstructorFieldPublication(
        unsupportedPointee, unsupportedConstructor.body, ownerConstructorId,
        *unsupportedInstruction);
    expect(rejected.matched() && !rejected.exact,
           "an incomplete unique pointee cannot acquire native owner "
           "publication");
  }

  MirProgram duplicateOwner = ownerMir;
  auto &duplicateOwnerBody = mutateOwnerConstructor(duplicateOwner).body;
  auto *duplicateOriginal = findOwnerInstruction(duplicateOwnerBody);
  if (duplicateOriginal) {
    MirInstruction sibling = *duplicateOriginal;
    sibling.id = duplicateOwnerBody.instructionCount() + 1;
    sibling.result = duplicateOwnerBody.values.size() + 1;
    MirValue value =
        duplicateOwnerBody.values.at(*duplicateOriginal->result - 1);
    value.id = *sibling.result;
    value.definition = sibling.id;
    duplicateOwnerBody.values.push_back(value);
    duplicateOwnerBody.valueUses.emplace_back();
    duplicateOwnerBody.blocks.front().instructions.push_back(sibling);
    duplicateOriginal = findOwnerInstruction(duplicateOwnerBody);
    const auto rejected = native_llvm::unstagedConstructorFieldPublication(
        duplicateOwner, duplicateOwnerBody, ownerConstructorId,
        *duplicateOriginal);
    expect(rejected.matched() && !rejected.exact,
           "duplicate null producers cannot claim one unique-owner field");
  }

  MirProgram stagedOwner = ownerMir;
  auto &stagedOwnerBody = mutateOwnerConstructor(stagedOwner).body;
  MirInstruction ownerStage;
  ownerStage.id = stagedOwnerBody.instructionCount() + 1;
  ownerStage.kind = MirInstructionKind::Initialize;
  ownerStage.constructorInitializer = ownerInitializerIndex + 1;
  stagedOwnerBody.blocks.front().instructions.push_back(ownerStage);
  auto *stagedOwnerInstruction = findOwnerInstruction(stagedOwnerBody);
  expect(stagedOwnerInstruction &&
             !native_llvm::unstagedConstructorFieldPublication(
                  stagedOwner, stagedOwnerBody, ownerConstructorId,
                  *stagedOwnerInstruction)
                  .matched(),
         "an explicit null-owner Initialize stage excludes the record-only "
         "publication hook");
}

void testIntegralEnumReferences() {
  using namespace lang;
  constexpr std::string_view source = R"(
enum class E { first, second };
E& read(E& value) { return value; }
mut E& edit(mut E& value) { return value; }
int main() {
  mut E value = E::first;
  edit(value) = E::second;
  return read(value) == E::second ? 0 : 1;
}
)";
  const auto plan = planLlvmAdmission(lower(source));
  expect(plan.admitted(), "sealed integral enum references are represented");
  unsigned borrows = 0;
  for (const auto &use : plan.requirements())
    if (use.required && use.entry->family == "MirInstructionKind" &&
        use.entry->name == "Borrow") {
      ++borrows;
      expect(use.implementedShape &&
                 use.implementedShape->name == "PassiveReference",
             "enum borrows retain the selected-address witness");
    }
  expect(borrows != 0, "integral enum fixture exercises actual MIR borrows");

  const auto payload = planLlvmAdmission(lower(R"(
enum class E { first, second };
enum Message { item(E value) };
E& read(Message& message) {
  switch (message) { case Message::item(auto& value): return value; }
}
int main() { Message message = Message::item(E::first); return read(message) == E::first ? 0 : 1; }
)"));
  unsigned enumFields = 0;
  for (const auto &use : payload.requirements()) {
    if (!use.required || use.entry->family != "MirInstructionKind" ||
        use.entry->name != "Borrow" || !use.body ||
        use.body->kind != MirBodyKind::Function)
      continue;
    const auto *owner =
        payload.program().mir().findFunctionInstance(use.body->owner);
    const auto *block = owner ? owner->body.findBlock(use.block) : nullptr;
    if (!block)
      continue;
    for (const auto &stage : block->instructions) {
      if (stage.id != use.instruction)
        continue;
      if (payload.program().mir().findIntegralEnum(stage.info.type)) {
        ++enumFields;
        expect(use.implementedShape &&
                   use.implementedShape->name == "PassiveReference",
               "integral enum payload fields have the same address witness");
      }
      if (payload.program().mir().findPayloadEnum(stage.info.type))
        expect(!use.implementedShape ||
                   use.implementedShape->name != "PassiveReference",
               "payload enum itself cannot borrow the scalar enum witness");
    }
  }
  expect(enumFields != 0, "payload regression reaches integral enum borrowing");
  // This asserts only the borrow's witness. O07 payload storage/execution is
  // still independently required by the complete combined execution fixture.

  // The enum's identity must be represented in the sealed integral inventory.
  // Test copies are corrupted before sealing, never accepted mutable inputs.
  const auto frontend =
      Frontend().analyze("enum-reference.gti", std::string(source));
  for (const bool missing : {false, true}) {
    auto mir = frontend.mir;
    auto &enums =
        const_cast<std::vector<MirIntegralEnum> &>(mir.integralEnums());
    expect(enums.size() == 1, "enum identity control has exactly one record");
    if (enums.empty())
      continue;
    if (missing)
      enums.clear();
    else
      enums.front().underlyingType = SemanticType::Unknown;
    auto build = LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir, mir, mir, {},
        TargetInfo::host());
    bool rejected = !build.valid();
    if (build.valid()) {
      std::vector<LoweredProgramIssue> issues;
      auto verified = verifiedLoweredProgram(std::move(*build.program), issues);
      rejected = !verified;
      if (verified)
        rejected = !planLlvmAdmission(*verified).admitted();
    }
    expect(
        rejected,
        "missing or malformed integral enum metadata cannot grant admission");
  }

  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
  FrontendOptions options;
  options.requireEntryPoint = true;
  options.lowerReachableOnly = true;
  options.lowerLibraryReachableOnly = true;
  const auto rawFrontend = Frontend(options).analyze(
      root / "tests/fixtures/llvm_enum_raw_borrow.gti", std::nullopt,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  expect(rawFrontend.canGenerateCode(),
         "raw enum control is valid source, not a semantic rejection");
  auto rawMir = rawFrontend.mir;
  unsigned marked = 0;
  // Current source staging does not mark restored-loan relays as raw reads.
  // Exercise the latent raw classifier with a conservative effect annotation
  // on an owned MIR copy, then require the complete program to verify and seal.
  // No type, permission, origin, producer or escape fact is changed.
  for (auto &owner : const_cast<std::vector<MirFunctionInstance> &>(
           rawMir.functionInstances()))
    for (auto &block : owner.body.blocks)
      for (auto &stage : block.instructions)
        if (stage.kind == MirInstructionKind::Borrow &&
            stage.operands.size() == 1 &&
            stage.operands[0].kind == MirOperandKind::Loan &&
            rawMir.findIntegralEnum(stage.info.type)) {
          stage.rawMemoryAccess = true;
          ++marked;
        }
  expect(marked != 0, "raw control retains an exact enum Borrow/Loan relay");
  const auto summaries = deriveMirProgramEffectSummaries(rawMir);
  for (std::size_t index = 0; index < summaries.functions.size(); ++index)
    const_cast<MirFunctionInstance &>(rawMir.functionInstances()[index])
        .effectSummary = summaries.functions[index];
  expect(verifyMirProgram(rawMir).valid(),
         "conservative relay effect does not change verified loan authority");
  auto rawBuild = LoweredProgramBuilder().build(
      rawFrontend.program, rawFrontend.semantics, rawFrontend.hir, rawMir,
      rawMir, {}, TargetInfo::host());
  if (!rawBuild.valid())
    throw std::runtime_error(rawBuild.issues.front().detail);
  std::vector<LoweredProgramIssue> issues;
  auto rawVerified =
      verifiedLoweredProgram(std::move(*rawBuild.program), issues);
  if (!rawVerified)
    throw std::runtime_error(issues.front().detail);
  const auto raw = planLlvmAdmission(*rawVerified);
  expect(!raw.admitted(),
         "selected enum references do not grant raw enum access");
  unsigned rawLoans = 0;
  for (const auto &use : raw.requirements()) {
    if (!use.required || use.entry->family != "Shape" ||
        use.entry->name != "RawMemory" || !use.body ||
        use.body->kind != MirBodyKind::Function)
      continue;
    const auto *owner =
        raw.program().mir().findFunctionInstance(use.body->owner);
    const auto *block = owner ? owner->body.findBlock(use.block) : nullptr;
    if (!block)
      continue;
    for (const auto &stage : block->instructions)
      if (stage.id == use.instruction &&
          stage.kind == MirInstructionKind::Borrow &&
          stage.operands.size() == 1 &&
          stage.operands[0].kind == MirOperandKind::Loan) {
        ++rawLoans;
        expect(!use.implementedShape,
               "raw enum Borrow/Loan keeps its independently pending witness");
      }
  }
  expect(rawLoans != 0, "raw enum control exercises Borrow/Loan admission");
}

void testLedger() {
  using namespace lang;
  using llvm_detail::Shape;
  std::set<std::string> keys;
  std::size_t pending = 0;
  std::size_t nonexecuting = 0;
  for (const auto &entry : llvmCoverageLedger()) {
    expect(keys.insert(entry.key()).second, "coverage identities are unique");
    expect(entry.milestone >= 3 && entry.milestone <= 15,
           "each entry has a concrete implementation owner");
    expect(!entry.predicate.empty(),
           "each entry states its admission boundary");
    pending += entry.state == LlvmCoverageState::Pending;
    nonexecuting += entry.state == LlvmCoverageState::VerifiedNonExecuting;
    if (entry.state == LlvmCoverageState::Implemented) {
      const bool hasEvidence = !entry.positiveTest.empty() &&
                               !entry.positiveAssertion.empty() &&
                               entry.positiveTest != "llvm_admission" &&
                               entry.positiveTest != "gti_unit_tests";
      const bool hasGap = !entry.positiveEvidenceGap.empty();
      expect(hasEvidence != hasGap,
             "implemented rows name execution evidence or one explicit gap");
    } else if (entry.state == LlvmCoverageState::Pending) {
      expect(entry.positiveTest.empty() && entry.positiveAssertion.empty() &&
                 entry.positiveEvidenceGap.empty(),
             "pending rows cannot masquerade as accepted execution");
    } else {
      expect(entry.positiveTest == "gti_unit_tests" &&
                 entry.positiveAssertion == "llvm_admission_ledger" &&
                 entry.positiveEvidenceGap.empty() &&
                 entry.nonExecuting != LlvmNonExecutingRule::None,
             "nonexecuting rows retain structural evidence");
    }
    expect(!entry.negativeTest.empty() && !entry.negativeAssertion.empty(),
           "each row names its negative drift control");
    if (entry.key() == "MirOperation.None")
      expect(entry.state == LlvmCoverageState::VerifiedNonExecuting &&
                 entry.nonExecuting == LlvmNonExecutingRule::VerifiedMetadata,
             "the no-operation marker is structural metadata");
    if (entry.key() == "IntrinsicKind.ExactTypeParameterConstruction")
      expect(entry.state == LlvmCoverageState::VerifiedNonExecuting &&
                 entry.nonExecuting == LlvmNonExecutingRule::MustBeAbsent,
             "the HIR-only exact pack construction cannot reach executable "
             "MIR");
    if (entry.family == "Shape" &&
        (entry.name == "NativeStaticDispatchStorage" ||
         entry.name == "NativeBorrowedDispatchView" ||
         entry.name == "NativeVirtualInvocation"))
      expect(entry.state == LlvmCoverageState::Implemented &&
                 entry.positiveTest == "llvm_dispatch_execution" &&
                 !entry.positiveAssertion.empty(),
             "existing dispatch families bind explicit execution profiles");
  }
  expect(pending > 250 && nonexecuting > 20,
         "the full input vocabulary is inventoried");

#define LLVM_COVERAGE_BEGIN(Type)                                              \
  {                                                                            \
    using Enum = Type;
#define LLVM_COVERAGE_ENTRY(Name, Milestone, State, Rule, Predicate)           \
  expect(llvm_detail::coverage(Enum::Name).name == #Name,                      \
         "each declared enumerator has a typed positive classification");
#define LLVM_COVERAGE_SENTINEL(Name)                                           \
  try {                                                                        \
    static_cast<void>(llvm_detail::coverage(Enum::Name));                      \
    expect(false, "Count cannot become an executable ledger entry");           \
  } catch (const std::logic_error &) {                                         \
  }
// The unscoped type-kind enum has no valid out-of-range cast; its exhaustive
// switch is the negative gate. Scoped enums have a fixed underlying type.
#define LLVM_COVERAGE_END()                                                    \
  if constexpr (!std::is_convertible_v<Enum, int>) {                           \
    try {                                                                      \
      static_cast<void>(llvm_detail::coverage(static_cast<Enum>(-1)));         \
      expect(false, "unknown vocabulary cannot silently gain coverage");       \
    } catch (const std::logic_error &) {                                       \
    }                                                                          \
  }                                                                            \
  }
#include "../src/compiler/llvm/coverage.def"
#undef LLVM_COVERAGE_BEGIN
#undef LLVM_COVERAGE_ENTRY
#undef LLVM_COVERAGE_SENTINEL
#undef LLVM_COVERAGE_END
}

void testRawFieldWitnesses() {
  const auto borrowedField = lang::planLlvmAdmission(lower(R"(
struct Cell { mut int value = 7; Cell() = default; };
int read(int& value) { return value; }
void change(mut int& value) { value = 9; }
struct Snapshot { int value; Snapshot(int& source) : value(source) {} };
int main() {
  mut Cell object{};
  unsafe {
    Cell* pointer = &object;
    Snapshot snapshot{pointer->value};
    change(pointer->value);
    return read(pointer->value) + snapshot.value - 16;
  }
}
)"));
  expect(
      borrowedField.admitted(),
      "raw fields bind exact read/mutable call and constructor parameters: " +
          borrowedField.limitation());
  for (const auto &source :
       {"struct Cell { mut int value = 7; Cell() = default; }; "
        "int read(int& value) { return value; } "
        "int main() { Cell object{}; const Cell* p = &object; return "
        "read(p->value); }",
        "struct Cell { mut int value = 7; Cell() = default; }; "
        "void change(mut int& value) { value = 9; } "
        "int main() { Cell object{}; const Cell* p = &object; unsafe { "
        "change(p->value); } return 0; }"})
    expect(
        !lang::Frontend()
             .analyze("raw-borrow-rejection.gti", source)
             .canGenerateCode(),
        "raw argument borrows preserve unsafe and mutable access requirements");
  for (const auto qualifier : {"const ", "mut "}) {
    const auto pointerInput = lang::planLlvmAdmission(lower(std::string(R"(
struct Slot { mut int value = 7; Slot() = default; };
unsafe int consume(const Slot* pointer) { unsafe { return pointer->value; } }
int main() {
  mut Slot object{};
  unsafe {
)") + qualifier + R"(Slot* pointer = &object;
    return consume(pointer + 0) - 7;
  }
}
)"));
    expect(
        pointerInput.admitted(),
        "native pointer arithmetic arguments transport their exact producer, "
        "including mutable-to-readonly pointer coercion");
  }
  expect(!lang::Frontend()
              .analyze("unsafe-pointer-input.gti", R"(
struct Slot { mut int value = 7;  public: Slot() = default; };
unsafe int consume(const Slot* pointer) { unsafe { return pointer->value; } }
int main() {
  mut Slot object{};
  const Slot* pointer = &object;
  return consume(pointer + 0);
}
)")
              .canGenerateCode(),
         "transport admission does not authorize arithmetic outside unsafe");
  const std::string source = R"(
class Slot { public: mut int value = 0; int read() { return this.value; }  public: Slot() = default; };
int main() {
  mut Slot object{};
  unsafe { Slot* pointer = &object; pointer->value = 7; return pointer->value - 7; }
}
)";
  const auto plan = lang::planLlvmAdmission(lower(source));
  bool witnessed = false;
  for (const auto &use : plan.requirements())
    if (use.required && use.entry->family == "UnsafeOperationKind" &&
        use.entry->name == "RawMember") {
      witnessed = true;
      expect(use.implementedShape &&
                 use.implementedShape->name == "NativeClassRawAccess",
             "raw field loads/stores require the complete typed field witness");
    }
  expect(plan.admitted() && witnessed,
         "ordinary mutable raw field access is admitted");
  for (const auto &valid :
       {"class Readonly { public: int value = 7;  public: Readonly() = "
        "default; }; int main() { Readonly "
        "object{}; unsafe { const Readonly* pointer = &object; return "
        "pointer->value - 7; } }",
        "class Padded { public: int8_t tag = int8_t(1); double tail = "
        "double(42.5);  public: Padded() = default; }; int main() { Padded "
        "object{}; unsafe { const Padded* "
        "pointer = &object; return pointer->tail == double(42.5) ? 0 : 1; } }",
        "class Nested { public: mut int values[3] = {1, 2, 3};  public: "
        "Nested() = default; }; int main() { "
        "mut Nested object{}; unsafe { Nested* pointer = &object; "
        "pointer->values[1] = 9; return pointer->values[1] - 9; } }",
        "class Parent { public: int value = 7;  public: Parent() = default; }; "
        "class Child : public Parent "
        "{ public: int extra = 2;  public: Child() = default; }; int main() { "
        "Child object{}; unsafe { "
        "const Child* pointer = &object; return pointer->value - 7; } }"}) {
    const auto frontend =
        lang::Frontend().analyze("valid-raw-field.gti", valid);
    expect(frontend.canGenerateCode() &&
               lang::verifyMirProgram(frontend.mir).valid(),
           "raw projection type verification preserves readonly, padded, "
           "nested-array and base fields");
  }
  const auto method = lang::planLlvmAdmission(lower(R"(
class Slot { public: int value = 7; int read() { return this.value; }  public: Slot() = default; };
int main() { Slot object{}; unsafe { const Slot* pointer = &object; return pointer->read(); } }
)"));
  bool methodWitnessed = false;
  for (const auto &use : method.requirements())
    if (use.required && use.entry->family == "UnsafeOperationKind" &&
        use.entry->name == "RawMember") {
      methodWitnessed = true;
      expect(use.implementedShape &&
                 use.implementedShape->name == "NativeClassRawAccess",
             "a raw method receiver requires the dereferenced place witness");
    }
  expect(method.admitted() && methodWitnessed,
         "a method reached through a raw pointer to a native class is the "
         "same raw member access as a field");
  const auto virtualMethod = lang::planLlvmAdmission(lower(R"(
class Dynamic { public: int value = 7; virtual int read() { return this.value; }  public: Dynamic() = default; };
int main() { Dynamic object{}; unsafe { const Dynamic* pointer = &object; return pointer->read(); } }
)"));
  // The pointee of a raw class pointer is a checked dispatch view under the
  // program's unsafe obligation; the virtual call keeps its own witness.
  bool rawVirtual = false;
  for (const auto &use : virtualMethod.requirements())
    rawVirtual |= use.required && use.implementedShape &&
                  use.implementedShape->name == "NativeVirtualInvocation";
  if (!virtualMethod.admitted())
    std::cerr << "raw virtual admission: " << virtualMethod.limitation()
              << '\n';
  expect(virtualMethod.admitted() && rawVirtual,
         "a virtual method reached through a raw pointer dispatches through "
         "the pointee's checked view");
  const auto unsupported = lang::planLlvmAdmission(lower(R"(
class Dynamic { public: int value = 7; virtual int read() { return this.value; }  public: Dynamic() = default; };
int main() { Dynamic object{}; unsafe { const Dynamic* pointer = &object; return pointer->value; } }
)"));
  // Exact hierarchy metadata now supplies the static field layout of the
  // pointee; only the virtual call above still needs a dispatch view.
  bool fieldWitnessed = false;
  for (const auto &use : unsupported.requirements())
    fieldWitnessed |= use.required &&
                      use.entry->family == "UnsafeOperationKind" &&
                      use.entry->name == "RawMember" && use.implementedShape &&
                      use.implementedShape->name == "NativeClassRawAccess";
  expect(unsupported.admitted() && fieldWitnessed,
         "a raw field read of hierarchy storage uses its exact metadata "
         "layout: " +
             unsupported.limitation());
  for (const auto &invalid :
       {"class Slot { public: mut int value = 0;  public: Slot() = default; }; "
        "int main() { Slot "
        "object{}; unsafe { const Slot* pointer = &object; pointer->value = 1; "
        "} return 0; }",
        "class Slot { public: int value = 0;  public: Slot() = default; }; int "
        "main() { Slot object{}; "
        "unsafe { const Slot* pointer = &object; return pointer->missing; } }"})
    expect(
        !lang::Frontend()
             .analyze("invalid-raw-field.gti", invalid)
             .canGenerateCode(),
        "read-only writes and nonexistent fields reject at the semantic owner");
  const auto frontend = lang::Frontend().analyze("raw-field.gti", source);
  const auto output =
      std::filesystem::temp_directory_path() /
      ("gti-malformed-field-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  struct RemoveOutput {
    std::filesystem::path path;
    ~RemoveOutput() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  } removeOutput{output};
  for (unsigned mutation = 0; mutation < 3; ++mutation) {
    {
      std::ofstream sentinel(output);
      sentinel << "preserved";
    }
    auto mir = frontend.mir;
    bool changed = false;
    std::string location;
    for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
             mir.functionInstances()))
      for (auto &place : function.body.places)
        if (!changed &&
            std::any_of(place.projections.begin(), place.projections.end(),
                        [](const auto &projection) {
                          return projection.kind ==
                                 lang::MirProjectionKind::RawDereference;
                        }))
          for (auto &projection : place.projections)
            if (!changed && projection.kind == lang::MirProjectionKind::Field) {
              location = "Function/" + std::to_string(function.id) +
                         " place=" + std::to_string(place.id) +
                         " raw-root=" + std::to_string(place.value) +
                         " field=" + std::to_string(projection.field);
              if (mutation == 0)
                projection.field = 0;
              if (mutation == 1)
                place.type = lang::SemanticType::Int64;
              if (mutation == 2)
                place.value = 0;
              changed = true;
            }
    const auto verification = lang::verifyMirProgram(mir);
    bool rejected = !verification.valid();
    expect(changed && rejected,
           "malformed raw class field is rejected by program verification");
    if (mutation == 1)
      expect(std::any_of(verification.errors.begin(), verification.errors.end(),
                         [](const auto &error) {
                           return error.bodyKind ==
                                      lang::MirBodyKind::Function &&
                                  error.owner != 0 && error.instruction != 0 &&
                                  error.message.find("place 3") !=
                                      std::string::npos &&
                                  error.message.find(
                                      "exact raw field projection type") !=
                                      std::string::npos;
                         }),
             "field-type integrity error retains its body, root producer and "
             "place");
    const auto mirrored = lang::LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir, mir, mir, {},
        lang::TargetInfo::host());
    expect(
        !mirrored.valid(),
        "mirrored malformed snapshots cannot enter verified lowered sealing");
    std::string rejectedBy = "MIR verifier";
    if (!rejected) {
      auto build = lang::LoweredProgramBuilder().build(
          frontend.program, frontend.semantics, frontend.hir, mir, mir, {},
          lang::TargetInfo::host());
      rejected = !build.valid();
      rejectedBy = "LoweredProgramBuilder";
      if (!build.valid() && !build.issues.empty())
        rejectedBy += "/" + build.issues[0].detail.substr(0, 300);
      if (build.valid()) {
        std::vector<lang::LoweredProgramIssue> issues;
        auto verified =
            lang::verifiedLoweredProgram(std::move(*build.program), issues);
        rejected = !verified;
        rejectedBy = "lowered verifier";
        if (verified) {
          const auto admission = lang::planLlvmAdmission(*verified);
          rejected = !admission.admitted();
          rejectedBy = "native admission";
          if (const auto *unsupported = admission.firstUnsupported())
            rejectedBy += "/" + unsupported->entry->key();
          bool generated = false;
          try {
            const auto artifact =
                lang::LlvmBackend().generate(verified->program());
            std::ofstream published(output, std::ios::binary);
            published << artifact.contents;
            generated = true;
          } catch (const lang::BackendUnsupportedError &) {
          }
          expect(!generated,
                 "malformed projected type cannot produce native object bytes");
        }
      }
    }
    if (mutation == 1)
      std::cout << "raw field type control: " << location
                << " int32_t->int64_t; rejected by " << rejectedBy << '\n';
    std::ifstream sentinel(output);
    std::string contents{std::istreambuf_iterator<char>(sentinel), {}};
    expect(contents == "preserved",
           "malformed field preflight preserves the existing artifact");
    expect(changed && rejected, "malformed raw field must fail no later than "
                                "native artifact preflight: " +
                                    std::to_string(mutation));
  }
}

} // namespace

GTI_UNIT_TEST(llvm_admission_concrete_parameter_pack_plan) {
  testConcreteParameterPackPlan();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(llvm_admission_concrete_parameter_aliases) {
  testConcreteParameterAliases();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(llvm_admission_verified_concrete_pack_owners) {
  testVerifiedConcretePackOwners();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(llvm_admission_closure_environment_plan) {
  testClosureEnvironmentPlan();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(llvm_admission_verified_restoration_relay) {
  testVerifiedRestorationRelay();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(llvm_admission_unstaged_constructor_field_publication_plan) {
  testUnstagedConstructorFieldPublicationPlan();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(llvm_admission_ledger) {
  testLedger();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(llvm_admission_raw_field_witnesses) {
  testRawFieldWitnesses();
  return failures == 0 ? 0 : 1;
}

GTI_UNIT_TEST(llvm_admission_integral_enum_references) {
  testIntegralEnumReferences();
  return failures == 0 ? 0 : 1;
}
