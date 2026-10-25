#include "gti/frontend.h"
#include "gti/llvm_admission.h"
#include "gti/lowered_program_builder.h"
#include "gti/mir_verifier.h"

#include "../src/compiler/llvm/closure_environment.h"
#include "../src/compiler/llvm/constructor_field_publication.h"
#include "../src/compiler/llvm/coverage.h"
#include "../src/compiler/llvm/parameter_packs.h"
#include "../src/compiler/llvm/placement.h"
#include "../src/compiler/mir_verification_queries.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

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

  MirBody divergent = aliases;
  divergent.places.back().key->projections.front().index = 1;
  expect(!native_llvm::planConcreteParameters(parameters, bindings, divergent),
         "different-key projected aliases fail closed");
  divergent = aliases;
  divergent.places[1].key->root = 31;
  expect(!native_llvm::planConcreteParameters(parameters, bindings, divergent),
         "different-key pack roots fail closed");
}

void testPlacementPlan() {
  using namespace lang;
  using native_llvm::Placement;
  // Component inputs exercise representation planning only. Public admission
  // and execution below still require a sealed, verified source program.
  SemanticType object = SemanticType::Unknown;
  object.kind = SemanticType::Class;
  SemanticType array = SemanticType::Unknown;
  array.kind = SemanticType::Array;
  array.arguments = {object};
  array.arrayLength = 1;
  MirBody body;
  body.returnType = object;
  body.places = {
      {.id = 1, .type = array},
      {.id = 2, .type = object},
      {.id = 3,
       .type = SemanticType::referenceTo(object, AccessMode::ReadOnly)}};
  MirBlock block;
  block.id = 1;
  block.instructions = {
      {.id = 1,
       .kind = MirInstructionKind::Construct,
       .result = 1,
       .info = {.type = object}},
      {.id = 2,
       .kind = MirInstructionKind::CallInput,
       .result = 2,
       .operands = {{.kind = MirOperandKind::Value,
                     .value = 1,
                     .type = object}},
       .info = {.type = object}},
      {.id = 3,
       .kind = MirInstructionKind::Compute,
       .result = 3,
       .operands = {{.kind = MirOperandKind::Value,
                     .value = 2,
                     .type = object}},
       .operation = MirOperation::Aggregate,
       .info = {.type = array}},
      {.id = 4,
       .kind = MirInstructionKind::Initialize,
       .destination = 1,
       .operands = {{.kind = MirOperandKind::Value,
                     .value = 3,
                     .type = array}}},
      {.id = 5,
       .kind = MirInstructionKind::Load,
       .result = 5,
       .operands = {{.kind = MirOperandKind::Copy, .place = 1, .type = array}},
       .info = {.type = array}},
      {.id = 6,
       .kind = MirInstructionKind::Call,
       .result = 6,
       .info = {.type = object}},
      {.id = 8,
       .kind = MirInstructionKind::Call,
       .result = 8,
       .intrinsic = IntrinsicKind::Move,
       .info = {.type = object}},
      {.id = 9,
       .kind = MirInstructionKind::Call,
       .result = 9,
       .loan = 1,
       .info = {.type = object}}};
  block.terminator = {.kind = MirTerminatorKind::Return,
                      .value = MirOperand{.kind = MirOperandKind::Value,
                                          .value = 6,
                                          .type = object}};
  body.blocks.push_back(block);
  const MirProgram program;
  const Placement placement(program, body);
  expect(placement.target(1) && placement.target(1)->aggregate == 3 &&
             placement.target(1)->element == 0 && placement.target(3) &&
             placement.target(3)->place == 1,
         "fresh argument staging preserves exact aggregate destination paths");
  expect(placement.target(6) && placement.target(6)->returned,
         "fresh returns use the caller's result destination");
  expect(!placement.fresh(5) && !placement.target(5),
         "explicit array copies never become fresh destination aliases");
  expect(!placement.fresh(8) && !placement.fresh(9),
         "intrinsic transfers and borrowed call results are not fresh objects");
  body.blocks[0].instructions.push_back(
      {.id = 10,
       .kind = MirInstructionKind::Initialize,
       .destination = 3,
       .operands = {
           {.kind = MirOperandKind::Value, .value = 1, .type = object}}});
  const Placement withReference(program, body);
  expect(withReference.target(1) && withReference.target(1)->aggregate == 3,
         "referent publication cannot replace object storage with a reference "
         "slot");
  body.blocks[0].instructions.push_back(
      {.id = 7,
       .kind = MirInstructionKind::Initialize,
       .destination = 2,
       .operands = {
           {.kind = MirOperandKind::Value, .value = 1, .type = object}}});
  bool rejected = false;
  try {
    const Placement invalid(program, body);
  } catch (const BackendUnsupportedError &) {
    rejected = true;
  }
  expect(rejected, "conflicting physical destinations fail closed");
}

lang::VerifiedLoweredProgram lower(std::string_view source,
                                   bool withStandardLibrary = false) {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto frontend = lang::Frontend().analyze(
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
      frontend.mir, {}, lang::TargetInfo::host(), true);
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
#include <std/memory>
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
            candidate.type.kind == SemanticType::Lambda) {
          publicationConstructor = &constructor;
          publicationInstruction = &instruction;
          publication = candidate;
        }
      }
  expect(publicationConstructor && publicationInstruction &&
             publication.exact && publication.value != 0,
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
}

void testStoredFieldPublicationPlan() {
  using namespace lang;
  using native_llvm::StoredFieldPublicationKind;

  const auto verified = lower(R"(
#include <std/memory>
class Holder<F> {
  mut F callable;
public:
  Holder(F callable) : callable(std::move(callable)) {}
  int run() mut { return this.callable(); }
};
Holder<F> store<F>(F callable) { return Holder<F>(std::move(callable)); }
class View {
  mut int& value;
public:
  View(mut int& value) : value(value) {}
  int read() { return this.value; }
};
int main() {
  mut int value = 4;
  mut auto update = [mut &value]() -> int { value += 1; return value; };
  mut auto stored = store(std::move(update));
  mut int reference_value = 9;
  View view{reference_value};
  return stored.run() == 5 && view.read() == 9 ? 0 : 1;
}
)",
                              true);
  const auto &mir = verified.program().mir();
  struct Witness {
    HirConstructorInstanceId constructor = 0;
    MirLoanId loan = 0;
    HirClassInstanceId owner = 0;
  } carrier, reference;
  for (const auto &constructor : mir.constructorInstances())
    for (const auto &loan : constructor.body.loans) {
      const auto plan = native_llvm::storedFieldPublication(
          mir, constructor.body, constructor.id, loan);
      if (!plan.exact)
        continue;
      if (plan.kind == StoredFieldPublicationKind::ProvenanceOnly &&
          plan.field && plan.field->type.kind == SemanticType::Lambda)
        carrier = {constructor.id, loan.id, constructor.owner};
      if (plan.kind == StoredFieldPublicationKind::ReferenceAddress)
        reference = {constructor.id, loan.id, constructor.owner};
    }
  expect(carrier.constructor && carrier.loan,
         "a borrowed callable field has one exact provenance-only stored "
         "publication");
  expect(reference.constructor && reference.loan,
         "an unrelated reference field retains exact physical address "
         "publication");
  if (!carrier.constructor || !carrier.loan)
    return;

  const auto constructorAt =
      [](MirProgram &program,
         HirConstructorInstanceId id) -> MirConstructorInstance & {
    auto &constructors = const_cast<std::vector<MirConstructorInstance> &>(
        program.constructorInstances());
    const auto found =
        std::find_if(constructors.begin(), constructors.end(),
                     [&](const auto &candidate) { return candidate.id == id; });
    if (found == constructors.end())
      throw std::runtime_error("stored publication constructor disappeared");
    return *found;
  };
  const auto loanAt = [](MirBody &body, MirLoanId id) -> MirLoan & {
    const auto found =
        std::find_if(body.loans.begin(), body.loans.end(),
                     [&](const auto &loan) { return loan.id == id; });
    if (found == body.loans.end())
      throw std::runtime_error("stored publication loan disappeared");
    return *found;
  };
  const auto ownerAt = [](MirProgram &program,
                          HirClassInstanceId id) -> MirClassInstance & {
    auto &owners =
        const_cast<std::vector<MirClassInstance> &>(program.classInstances());
    const auto found =
        std::find_if(owners.begin(), owners.end(),
                     [&](const auto &candidate) { return candidate.id == id; });
    if (found == owners.end())
      throw std::runtime_error("stored publication owner disappeared");
    return *found;
  };

  MirProgram wrongField = mir;
  auto &wrongFieldConstructor = constructorAt(wrongField, carrier.constructor);
  auto &wrongFieldLoan = loanAt(wrongFieldConstructor.body, carrier.loan);
  ++wrongFieldLoan.storedField;
  const auto missing = native_llvm::storedFieldPublication(
      wrongField, wrongFieldConstructor.body, carrier.constructor,
      wrongFieldLoan);
  expect(missing.matched() && !missing.exact,
         "stored publication rejects a missing declared field identity");

  MirProgram wrongType = mir;
  auto &wrongTypeConstructor = constructorAt(wrongType, carrier.constructor);
  auto &wrongTypeLoan = loanAt(wrongTypeConstructor.body, carrier.loan);
  auto &wrongTypeOwner = ownerAt(wrongType, carrier.owner);
  const auto field = std::find_if(
      wrongTypeOwner.declaredFields.begin(),
      wrongTypeOwner.declaredFields.end(), [&](const auto &candidate) {
        return candidate.symbol == wrongTypeLoan.storedField;
      });
  expect(field != wrongTypeOwner.declaredFields.end(),
         "stored publication mutation retains its declared field");
  if (field != wrongTypeOwner.declaredFields.end()) {
    field->type = SemanticType::referenceTo(SemanticType::Int32);
    const auto mismatched = native_llvm::storedFieldPublication(
        wrongType, wrongTypeConstructor.body, carrier.constructor,
        wrongTypeLoan);
    expect(mismatched.matched() && !mismatched.exact,
           "stored publication rejects a declared field/type mismatch");
  }
}

bool has(const lang::LlvmAdmissionPlan &plan, std::string_view family,
         std::string_view name) {
  return std::any_of(plan.requirements().begin(), plan.requirements().end(),
                     [&](const auto &use) {
                       return use.entry->family == family &&
                              use.entry->name == name;
                     });
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
    if (entry.state == LlvmCoverageState::Implemented)
      expect(entry.positiveTest ==
                 (entry.milestone == 9   ? "llvm_callable_execution"
                  : entry.milestone == 7 ? "llvm_payload_execution"
                  : entry.milestone == 6 ? "llvm_class_execution"
                  : entry.milestone == 5 ? "llvm_place_execution"
                                         : "llvm_scalar_execution"),
             "implemented families name real execution evidence");
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

void testSnapshotAndWholeProgram() {
  auto verified = lower(R"(
int dormant(int value) { return value * 3; }
class Owned { public: Owned() {} ~Owned() {} };
int main() { Owned value{}; int data[1] = {1}; return data == data ? 0 : 1; }
)");
  const auto *identity = &verified.program();
  const auto plan = lang::planLlvmAdmission(
      verified, {.optimization = lang::LlvmCodegenOptimization::Aggressive,
                 .debugInfo = true});
  expect(&plan.program() == identity,
         "admission retains the exact immutable verified snapshot");
  expect(plan.options().optimization ==
                 lang::LlvmCodegenOptimization::Aggressive &&
             plan.options().debugInfo &&
             plan.program().target() == verified.program().target(),
         "coverage retains target and selected optimization/debug identity");
  expect(!plan.admitted() && plan.firstUnsupported() != nullptr,
         "unsupported reachable storage remains a classified limitation");
  expect(plan.closure() == lang::LlvmAdmissionClosure::SealedEntryDependencies,
         "sealed roots and dependencies determine required execution");
  const auto expected = std::count_if(
      plan.program().bodies().begin(), plan.program().bodies().end(),
      [](const auto &body) {
        return body.role == lang::LoweredBodyRole::SourceExecutable;
      });
  expect(plan.requiredBodies().size() < static_cast<std::size_t>(expected),
         "dormant helpers remain inventoried without requiring execution");
  const auto *first = plan.firstUnsupported();
  expect(first && first->source.source.ends_with("llvm-admission.gti") &&
             first->source.end > first->source.start,
         "unsupported input carries original half-open source provenance");
  const auto repeated = lang::planLlvmAdmission(verified, plan.options());
  expect(repeated.limitation() == plan.limitation() &&
             repeated.requirements().size() == plan.requirements().size(),
         "inventory and first limitation are deterministic");
  expect(plan.limitation().find("O-LLVM-") != std::string::npos && first &&
             plan.limitation().find(first->entry->key()) != std::string::npos,
         "the limitation identifies the exact requirement and its owner");
  lang::LlvmBackend backend(plan.options());
  try {
    testPlacementPlan();
    static_cast<void>(backend.generate(plan.program()));
    expect(false, "unsupported admission cannot produce an artifact");
  } catch (const lang::BackendUnsupportedError &error) {
    expect(error.location() && *error.location() == first->source,
           "generation preserves the admission diagnostic span");
  }
}

void testShapeWitnesses() {
  const auto directConsuming = lang::planLlvmAdmission(lower(R"(
class Value { public: int read() && { return 42; } };
int main() { return Value().read() - 42; }
)"));
  expect(
      directConsuming.admitted() &&
          directConsuming.closure() ==
              lang::LlvmAdmissionClosure::SealedEntryDependencies,
      "an exact consuming method retains its sealed direct dependency closure");
  const auto indirect = lang::planLlvmAdmission(lower(R"(
int dormant(int value) { return value * 3; }
int main() { auto function = [](int value) -> int { return value; }; return function(0); }
)"));
  expect(indirect.closure() ==
                 lang::LlvmAdmissionClosure::SealedEntryDependencies &&
             indirect.admitted(),
         "sealed lambda dispatch retains its exact executable body dependency");
  const auto arrays = lang::planLlvmAdmission(lower(R"(
int main() { mut int values[2] = {1, 2}; return values[0] - 1; }
)"));
  expect(has(arrays, "Shape", "FixedArray") &&
             has(arrays, "MirProjectionKind", "Index"),
         "arrays retain both type and checked operation requirements");
  expect(arrays.admitted(), "passive arrays satisfy a structural refinement "
                            "without promoting the broad aggregate family");
  const auto arrayEquality = lang::planLlvmAdmission(lower(R"(
int main() { int left[2] = {1, 2}; int right[2] = {1, 2}; return left == right ? 0 : 1; }
)"));
  expect(!arrayEquality.admitted() && arrayEquality.firstUnsupported() &&
             arrayEquality.firstUnsupported()->entry->key() ==
                 "Shape.ArrayValueComparison",
         "scalar equality admission cannot hide an unimplemented array "
         "comparison");
  for (const auto &use : arrays.requirements())
    if (use.entry->family == "MirOperation" && use.entry->name == "Aggregate")
      expect(use.entry->state == lang::LlvmCoverageState::Pending &&
                 use.implementedShape &&
                 use.implementedShape->name == "PassiveArrayValue",
             "a passive-array proof does not claim class aggregate lowering");
  const auto references = lang::planLlvmAdmission(lower(R"(
int& identity(int& value) { return value; }
int main() { int value = 3; int& alias = identity(value); return alias - 3; }
)"));
  expect(has(references, "Shape", "BorrowReturn") &&
             has(references, "MirLoanKind", "Return"),
         "reference results retain address and loan requirements");
  const auto stored = lang::planLlvmAdmission(lower(R"(
mut int visits = 0;
mut int& selected(mut int& input) { visits += 1; return input; }
class View {
  mut int& value;
public:
  View(mut int& input) : value(selected(input)) {}
  void write() mut { this.value = 42; }
};
int main() {
  mut int value = 1;
  { mut View view{value}; view.write(); }
  return value - 42;
}
)"));
  expect(stored.admitted() && has(stored, "MirLoanKind", "Stored"),
         "evaluated stored-reference addresses are admitted through the sealed "
         "loan stage");
  const auto lifecycle = lang::planLlvmAdmission(lower(R"(
class Value { public: Value() {} ~Value() {} int read() { return 1; } };
int main() { Value value{}; return value.read() - 1; }
)"));
  expect(
      has(lifecycle, "Shape", "UserDestructor") &&
          has(lifecycle, "MirInstructionKind", "Construct"),
      "class construction and selected cleanup are independently inventoried");
  expect(lifecycle.admitted(),
         "exact native class witnesses admit selected lifecycle execution");
  expect(std::any_of(lifecycle.requiredBodies().begin(),
                     lifecycle.requiredBodies().end(),
                     [](auto body) {
                       return body.kind == lang::MirBodyKind::Destructor;
                     }),
         "cleanup recipes retain their exact selected destructor body");
  const auto unsafeScalar = lang::planLlvmAdmission(lower(R"(
unsafe int identity(int value) { return value; }
int main() { unsafe { return identity(0); } }
)"));
  expect(unsafeScalar.admitted(),
         "exact unsafe source calls keep ordinary body admission");
  const auto unsafeOwner = lang::planLlvmAdmission(lower(R"(
class Owner { public: virtual int read() { return 0; } };
unsafe int create() { Owner value{}; return 0; }
int main() { unsafe { return create(); } }
)"));
  expect(!unsafeOwner.admitted(),
         "caller permission cannot admit an unsupported source callee body");
  const auto classArray = lang::planLlvmAdmission(lower(R"(
class Owner { public: Owner() {} ~Owner() {} };
int main() { Owner values[2] = {Owner(), Owner()}; return 0; }
)"));
  expect(classArray.admitted(),
         "native class arrays have explicit lifecycle admission");
  for (const auto &use : classArray.requirements())
    if (use.entry->family == "SemanticType::Kind" && use.entry->name == "Array")
      expect(use.implementedShape &&
                 use.implementedShape->name == "NativeClassStorage",
             "class arrays require the class, not primitive-array witness");
  const auto native = lang::planLlvmAdmission(lower(R"(
extern "C" { int foreign_call(int value); }
int main() { return foreign_call(0); }
)"));
  expect(has(native, "Shape", "NativeCall") &&
             has(native, "Shape", "LibraryDeclaration"),
         "ABI declarations are distinguished from source definitions");
  const auto tls = lang::planLlvmAdmission(lower(R"(
thread_local mut int visits = 0;
int main() { visits += 1; return visits - 1; }
)"));
  expect(has(tls, "Shape", "ThreadLocalPassive") &&
             has(tls, "MirInstructionKind", "BeginThreadLocals"),
         "new thread-local storage and lifetime operations cannot drift behind "
         "coverage");
  expect(tls.admitted(), "passive TLS is explicitly admitted");
  const auto pinned = lang::planLlvmAdmission(lower(R"(
class Pinned {
public:
  Pinned() {}
  Pinned(Pinned&) = delete;
  Pinned(Pinned&&) = delete;
};
int main() { Pinned value{}; return 0; }
)"));
  expect(pinned.admitted(), "immovable ordinary objects use final destination "
                            "placement rather than byte transport");
}

} // namespace

int main() {
  try {
    testConcreteParameterPackPlan();
    testConcreteParameterAliases();
    testVerifiedConcretePackOwners();
    testClosureEnvironmentPlan();
    testVerifiedRestorationRelay();
    testUnstagedConstructorFieldPublicationPlan();
    testStoredFieldPublicationPlan();
    testLedger();
    testSnapshotAndWholeProgram();
    testShapeWitnesses();
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    ++failures;
  }
  if (failures)
    return 1;
  std::cout << "LLVM coverage and admission tests passed\n";
}
