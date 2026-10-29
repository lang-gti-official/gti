#include "gti/frontend.h"
#include "gti/llvm_admission.h"
#include "gti/llvm_backend.h"
#include "gti/lowered_program_builder.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"

#include "../src/compiler/llvm/closure_environment.h"
#include "../src/compiler/llvm/constructor_field_publication.h"
#include "../src/compiler/llvm/coverage.h"
#include "../src/compiler/llvm/parameter_packs.h"
#include "../src/compiler/llvm/placement.h"
#include "../src/compiler/mir_verification_queries.h"

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
       .info = {.type = object, .category = ValueCategory::Place}},
      {.id = 10,
       .kind = MirInstructionKind::Call,
       .result = 10,
       .loan = 2,
       .info = {.type = object}}};
  block.terminator = {.kind = MirTerminatorKind::Return,
                      .value = MirOperand{.kind = MirOperandKind::Value,
                                          .value = 6,
                                          .type = object}};
  body.blocks.push_back(block);
  const Placement placement(body);
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
  expect(placement.fresh(10),
         "a by-value call result carrying borrowed state is a fresh object");
  body.blocks[0].instructions.push_back(
      {.id = 10,
       .kind = MirInstructionKind::Initialize,
       .destination = 3,
       .operands = {
           {.kind = MirOperandKind::Value, .value = 1, .type = object}}});
  const Placement withReference(body);
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
    const Placement invalid(body);
  } catch (const BackendUnsupportedError &) {
    rejected = true;
  }
  expect(rejected, "conflicting physical destinations fail closed");
}

lang::VerifiedLoweredProgram lower(std::string_view source,
                                   bool withStandardLibrary = false,
                                   lang::FrontendOptions options = {}) {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
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

// An init-capture relocates a moved hierarchy object into its slot of the
// closure environment: the move's only publication is that slot, and the
// generated relocation rebuilds its dispatch state. A capture that does not
// move the object, mutated or written in source, has no typed relocation and
// stays PolymorphicTransfer.
void testHierarchyCapturePlan() {
  using namespace lang;
  const auto verified = lower(R"(
class Root {
public:
  int seed;
  Root(int seed) : seed(seed) {}
  Root(Root& other) = delete;
  Root(Root&& other) = default;
  virtual int value() { return this.seed; }
};
class Leaf : public Root {
public:
  Leaf(int seed) : Root(seed) {}
  int value() override { return this.seed + 1; }
};
int main() {
  mut Leaf leaf = Leaf(4);
  auto reader = [held = std::move(leaf)]() -> int { return held.value(); };
  return reader() - 5;
}
)",
                              true);
  const auto plan = planLlvmAdmission(verified);
  const auto implemented = [&](std::string_view name) {
    return std::any_of(plan.requirements().begin(), plan.requirements().end(),
                       [&](const auto &use) {
                         return use.required && use.implementedShape &&
                                use.implementedShape->name == name;
                       });
  };
  expect(plan.admitted() && implemented("NativeHierarchyCapture") &&
             implemented("NativeFreshHierarchyTransfer"),
         "a moved hierarchy capture relocates into its closure slot: " +
             plan.limitation());

  const auto &mir = verified.program().mir();
  const MirFunctionInstance *entry = nullptr;
  const MirInstruction *moved = nullptr;
  const MirInstruction *closure = nullptr;
  for (const auto &function : mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &stage : block.instructions)
        if (stage.operation == MirOperation::Closure &&
            stage.operands.size() == 1) {
          entry = &function;
          closure = &stage;
        }
  if (entry)
    for (const auto &block : entry->body.blocks)
      for (const auto &stage : block.instructions)
        if (closure && stage.result &&
            stage.result == closure->operands.front().value)
          moved = &stage;
  expect(entry && moved && closure && moved->kind == MirInstructionKind::Move,
         "the capture is the closure's moved operand");
  if (!entry || !moved || !closure)
    return;
  const auto transfer =
      native_llvm::planFreshHierarchyTransfer(mir, entry->body, *moved);
  const native_llvm::Placement placement(mir, entry->body);
  const auto *slot = placement.target(*moved->result);
  expect(transfer.publishes ==
                 native_llvm::FreshHierarchyPublication::Element &&
             transfer.publication == closure && transfer.element == 0 && slot &&
             slot->aggregate == *closure->result && slot->element == 0,
         "the moved object's only publication is its closure slot");

  // A capture recorded as a copy is not relocated into its slot: the move
  // gains no placement authority and its plan fails closed.
  MirProgram copied = mir;
  auto &function = const_cast<MirFunctionInstance &>(
      *copied.findFunctionInstance(entry->id));
  for (auto &block : function.body.blocks)
    for (auto &stage : block.instructions)
      if (stage.operation == MirOperation::Closure &&
          stage.result == closure->result)
        stage.closureCaptureModes.front() = LambdaCaptureMode::Copy;
  const MirInstruction *copiedMove = nullptr;
  for (const auto &block : function.body.blocks)
    for (const auto &stage : block.instructions)
      if (stage.id == moved->id)
        copiedMove = &stage;
  bool refused = false;
  try {
    static_cast<void>(native_llvm::planFreshHierarchyTransfer(
        copied, function.body, *copiedMove));
  } catch (const BackendUnsupportedError &) {
    refused = true;
  }
  const native_llvm::Placement copiedPlacement(copied, function.body);
  expect(refused && !copiedPlacement.target(*moved->result),
         "a capture that is not a move has no typed relocation authority");

  // In source: a by-copy capture of the same hierarchy object, copied with
  // its generated logical-field copy, and a copy of that whole closure.
  const auto byCopy = planLlvmAdmission(lower(R"(
class Root {
public:
  int seed;
  Root(int seed) : seed(seed) {}
  virtual int value() { return this.seed; }
};
class Leaf : public Root {
public:
  Leaf(int seed) : Root(seed) {}
  int value() override { return this.seed + 1; }
};
int main() {
  Leaf leaf = Leaf(4);
  auto reader = [leaf]() -> int { return leaf.value(); };
  auto again = reader;
  return again() - 5;
}
)"));
  expect(byCopy.admitted() &&
             std::none_of(
                 byCopy.requirements().begin(), byCopy.requirements().end(),
                 [](const auto &use) {
                   return use.required &&
                          use.entry->key() == "Shape.PolymorphicTransfer" &&
                          !use.implementedShape;
                 }),
         "a by-copy hierarchy capture and a closure copy are generated "
         "copies: " +
             byCopy.limitation());
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

void testStoredFieldPublicationPlan() {
  using namespace lang;
  using native_llvm::StoredFieldPublicationKind;

  const auto verified = lower(R"(
import std.memory;
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

void testHierarchyStorageScopeReplaysExactOutcomes() {
  using namespace lang;
  using native_llvm::containsHierarchyStorage;
  using native_llvm::HierarchyStorageScope;
  const auto plain = SemanticType::classType(1);
  const auto polymorphic = SemanticType::classType(2);
  const auto repeated = SemanticType::classType(3);
  std::vector<MirClassInstance> classes{
      {.id = 1,
       .type = plain,
       .declaredFields = {{.type = SemanticType::Int32}}},
      {.id = 2, .type = polymorphic, .polymorphic = true},
      {.id = 3,
       .type = repeated,
       .declaredFields = {{.type = plain}, {.type = plain}}}};
  const auto program = MirProgramTestAccess::storageInventory(classes);
  // The same class types denote a recursive graph in another program value.
  classes[0].declaredFields.front().type = repeated;
  const auto cyclic = MirProgramTestAccess::storageInventory(classes);
  const auto rejection = [](const MirProgram &input,
                            const SemanticType &type) -> std::string {
    try {
      static_cast<void>(containsHierarchyStorage(input, type));
      return {};
    } catch (const BackendUnsupportedError &error) {
      return error.what();
    }
  };
  const std::string missing = rejection(program, SemanticType::classType(99));
  const std::string recursive = rejection(cyclic, plain);
  expect(!missing.empty() && !recursive.empty(),
         "unscoped hierarchy queries reject missing and recursive storage");

  {
    const HierarchyStorageScope scope(program);
    for (int repeat = 0; repeat < 2; ++repeat) {
      expect(!containsHierarchyStorage(program, plain) &&
                 !containsHierarchyStorage(program, repeated) &&
                 containsHierarchyStorage(program, polymorphic),
             "a scoped query returns the program's exact answers");
      expect(rejection(program, SemanticType::classType(99)) == missing,
             "a scoped query replays the exact rejection it recorded");
    }
    expect(scope.cachedTypes() == 4,
           "a repeated scoped query reuses its recorded outcome");
    expect(rejection(cyclic, plain) == recursive && scope.cachedTypes() == 4,
           "another program value never reads or fills the open outcomes");
    {
      const HierarchyStorageScope nested(cyclic);
      expect(rejection(cyclic, plain) == recursive && nested.cachedTypes() == 1,
             "a nested scope for another program records its own outcomes");
      expect(!containsHierarchyStorage(program, plain),
             "an inner scope never answers for the outer program");
    }
    {
      const HierarchyStorageScope reused(program);
      expect(!containsHierarchyStorage(program, plain) &&
                 reused.cachedTypes() == 4,
             "a nested scope for the same program reuses its outcomes");
    }
    expect(!containsHierarchyStorage(program, repeated) &&
               scope.cachedTypes() == 4,
           "closing a nested scope restores the enclosing outcomes");
  }
  expect(!containsHierarchyStorage(program, plain) &&
             rejection(cyclic, plain) == recursive,
         "queries after the scope closes compute the same answers directly");
}

void testHierarchyStorageExclusion() {
  using namespace lang;
  using native_llvm::containsHierarchyStorage;
  const auto plain = SemanticType::classType(1);
  const auto polymorphic = SemanticType::classType(2);
  const auto repeated = SemanticType::classType(3);
  const auto wrapper = SemanticType::classType(4);
  const auto derived = SemanticType::classType(5);
  std::vector<MirClassInstance> classes{
      {.id = 1,
       .type = plain,
       .declaredFields = {{.type = SemanticType::Int32}}},
      {.id = 2, .type = polymorphic, .polymorphic = true},
      {.id = 3,
       .type = repeated,
       .declaredFields = {{.type = plain}, {.type = plain}}},
      {.id = 4, .type = wrapper, .declaredFields = {{.type = polymorphic}}},
      {.id = 5,
       .type = derived,
       .bases = {{.instance = 2, .type = polymorphic}}}};
  // Query-only component fixtures never acquire a source seal or become
  // executable backend input; malformed variants below must fail closed.
  const auto program = MirProgramTestAccess::storageInventory(classes);
  expect(!containsHierarchyStorage(program, plain) &&
             !containsHierarchyStorage(program, repeated),
         "repeated acyclic ordinary fields are not recursive hierarchy state");
  expect(containsHierarchyStorage(program, polymorphic) &&
             containsHierarchyStorage(program, wrapper) &&
             containsHierarchyStorage(program, derived),
         "hierarchy exclusion follows inline fields and exact state bases");
  SemanticType array(SemanticType::Array, {wrapper});
  array.arrayLength = 2;
  expect(containsHierarchyStorage(program, array),
         "hierarchy exclusion follows inline array elements");
  expect(!containsHierarchyStorage(
             program,
             SemanticType::referenceTo(polymorphic, AccessMode::ReadOnly)) &&
             !containsHierarchyStorage(program,
                                       SemanticType::rawPointerTo(polymorphic)),
         "borrowed and raw addresses do not copy their referent storage");
  const auto rejects = [&](const MirProgram &input, const SemanticType &type) {
    try {
      static_cast<void>(containsHierarchyStorage(input, type));
      return false;
    } catch (const BackendUnsupportedError &) {
      return true;
    }
  };
  expect(rejects(program, SemanticType::Unknown) &&
             rejects(program, SemanticType::typeParameter(1)) &&
             rejects(program, SemanticType::classType(99)),
         "unknown and missing storage never grant object-byte transport");
  auto wrongBase = classes;
  wrongBase.back().bases.front().type = plain;
  expect(rejects(MirProgramTestAccess::storageInventory(wrongBase), derived),
         "base identity and concrete type must agree even after finding state");
  wrongBase.back().bases.front() = {.instance = 99, .type = polymorphic};
  expect(rejects(MirProgramTestAccess::storageInventory(wrongBase), derived),
         "missing base identities never grant object-byte transport");
  wrongBase.back().bases.front() = {.instance = 5, .type = derived};
  expect(rejects(MirProgramTestAccess::storageInventory(wrongBase), derived),
         "recursive base storage fails closed");
  classes[0].declaredFields.front().type = repeated;
  const auto cyclic = MirProgramTestAccess::storageInventory(classes);
  expect(rejects(cyclic, plain),
         "recursive inline storage rejects without an unbounded walk");
  classes[0].declaredFields.front().type = SemanticType::Unknown;
  const auto unknown = MirProgramTestAccess::storageInventory(classes);
  expect(rejects(unknown, repeated),
         "unknown nested storage cannot be cached as an ordinary sibling");
  const auto sealed = lower(R"(
    struct Plain { int value = 1;  public: Plain() = default; };
    class P { public: virtual int read() { return 1; }  public: P() = default; };
    struct Pair { Plain left = Plain(); Plain right = Plain();  public: Pair() = default; };
    struct Wrapper { P value = P();  public: Wrapper() = default; };
    int main() { Pair pair{}; Wrapper wrapper{}; return 0; }
  )");
  bool foundPair = false, foundWrapper = false;
  for (const auto &owner : sealed.program().mir().classInstances()) {
    if (owner.declaredFields.size() == 2) {
      foundPair = true;
      expect(!containsHierarchyStorage(sealed.program().mir(), owner.type),
             "source-sealed repeated ordinary subobjects remain outside "
             "hierarchy-storage exclusion");
    }
    if (!owner.polymorphic && owner.declaredFields.size() == 1 &&
        owner.declaredFields[0].type.kind == SemanticType::Class &&
        containsHierarchyStorage(sealed.program().mir(), owner.type))
      foundWrapper = true;
  }
  expect(foundPair && foundWrapper,
         "source-sealed field inventory preserves nested hierarchy exclusion");
  expect(!Frontend()
              .analyze("inline-cycle.gti", R"(
             struct Recursive { Recursive value; };
             int main() { Recursive value{}; return 0; }
           )")
              .canGenerateCode(),
         "cyclic inline source cannot acquire a verified executable seal");
}

void testInlineHierarchyFamilyComposition() {
  using namespace lang;
  using native_llvm::containsHierarchyStorage;

  const auto scalar = SemanticType::classType(20);
  const auto polymorphic = SemanticType::classType(21);
  const auto wrapper = SemanticType::classType(22);
  const auto scalarStorage = SemanticType::classType(23, {scalar});
  const auto sharedOwner = SemanticType::classType(24, {wrapper});
  const auto recursiveOwner = SemanticType::classType(25);
  const auto unionType = SemanticType::classType(26);
  const auto hierarchyStorage = SemanticType::classType(27, {wrapper});
  const auto signature = SemanticType::functionSignature(SemanticType::Int32,
                                                         {SemanticType::Int32});
  const auto functionPointer = SemanticType::classType(28, {signature});
  const auto cyclicClass = SemanticType::classType(29);
  const auto scalarPayload = SemanticType::enumType(30);
  const auto hierarchyPayload = SemanticType::enumType(31);
  const auto integral = SemanticType::enumType(32);
  const std::vector<SemanticType> noParameters;
  const std::vector<SemanticType> scalarCapture{scalar};
  const std::vector<SemanticType> hierarchyCapture{wrapper};
  const std::vector<SemanticType> cyclicCapture{cyclicClass};
  const auto scalarClosure = SemanticType::lambdaType(
      40, SemanticType::Int32, noParameters, scalarCapture);
  const auto hierarchyClosure = SemanticType::lambdaType(
      41, SemanticType::Int32, noParameters, hierarchyCapture);
  const auto cyclicClosure = SemanticType::lambdaType(
      42, SemanticType::Int32, noParameters, cyclicCapture);

  std::vector<MirClassInstance> classes{
      {.id = 1,
       .type = scalar,
       .declaredFields = {{.type = SemanticType::Int32}}},
      {.id = 2, .type = polymorphic, .polymorphic = true},
      {.id = 3, .type = wrapper, .declaredFields = {{.type = polymorphic}}},
      {.id = 4,
       .type = scalarStorage,
       .compilerCapability = CompilerCapabilityTypeKind::UninitializedStorage},
      {.id = 5,
       .type = sharedOwner,
       .compilerCapability = CompilerCapabilityTypeKind::SharedOwner},
      {.id = 6,
       .type = recursiveOwner,
       .declaredFields = {{.type =
                               SemanticType::uniqueOwnerOf(recursiveOwner)}}},
      {.id = 7,
       .type = unionType,
       .unionLayout = MirUnionLayout{.sizeBytes = 8, .abiAlignmentBytes = 8},
       .declaredFields = {{.type = scalar}, {.type = wrapper}}},
      {.id = 8,
       .type = hierarchyStorage,
       .compilerCapability = CompilerCapabilityTypeKind::UninitializedStorage},
      {.id = 9,
       .type = functionPointer,
       .compilerCapability = CompilerCapabilityTypeKind::FunctionPointer},
      {.id = 10,
       .type = cyclicClass,
       .declaredFields = {{.type = cyclicClosure}}}};
  std::vector<MirPayloadEnum> payloads{
      {.declaration = 30,
       .type = scalarPayload,
       .variants = {{.variantIndex = 0,
                     .payloadTypes = {SemanticType::Int32},
                     .payloadSymbols = {300},
                     .payloadRequiresActiveCleanup = {false}}}},
      {.declaration = 31,
       .type = hierarchyPayload,
       .variants = {{.variantIndex = 0,
                     .payloadTypes = {wrapper},
                     .payloadSymbols = {310},
                     .payloadRequiresActiveCleanup = {false}}}}};
  std::vector<MirIntegralEnum> integrals{
      {.declaration = 32,
       .type = integral,
       .underlyingType = SemanticType::UInt32}};
  std::vector<MirLambdaInstance> lambdas{
      {.id = 1,
       .declaration = 40,
       .type = scalarClosure,
       .returnType = SemanticType::Int32,
       .captureTypes = scalarCapture,
       .captureModes = {LambdaCaptureMode::Copy},
       .captureSymbols = {4000},
       .captureRequiresActiveCleanup = {false}},
      {.id = 2,
       .declaration = 41,
       .type = hierarchyClosure,
       .returnType = SemanticType::Int32,
       .captureTypes = hierarchyCapture,
       .captureModes = {LambdaCaptureMode::Copy},
       .captureSymbols = {4100},
       .captureRequiresActiveCleanup = {false}},
      {.id = 3,
       .declaration = 41,
       .type = hierarchyClosure,
       .returnType = SemanticType::Int32,
       .captureTypes = hierarchyCapture,
       .captureModes = {LambdaCaptureMode::Copy},
       .captureSymbols = {4110},
       .captureRequiresActiveCleanup = {false}},
      {.id = 4,
       .declaration = 42,
       .type = cyclicClosure,
       .returnType = SemanticType::Int32,
       .captureTypes = cyclicCapture,
       .captureModes = {LambdaCaptureMode::Copy},
       .captureSymbols = {4200},
       .captureRequiresActiveCleanup = {false}}};
  const auto inventory = [&](std::vector<MirClassInstance> selectedClasses,
                             std::vector<MirPayloadEnum> selectedPayloads,
                             std::vector<MirIntegralEnum> selectedIntegrals,
                             std::vector<MirLambdaInstance> selectedLambdas) {
    return MirProgramTestAccess::storageInventory(
        std::move(selectedClasses), std::move(selectedPayloads),
        std::move(selectedIntegrals), std::move(selectedLambdas));
  };
  const auto program = inventory(classes, payloads, integrals, lambdas);
  const auto rejects = [&](const MirProgram &input, const SemanticType &type) {
    try {
      static_cast<void>(containsHierarchyStorage(input, type));
      return false;
    } catch (const BackendUnsupportedError &) {
      return true;
    }
  };

  expect(!containsHierarchyStorage(program, scalarPayload) &&
             !containsHierarchyStorage(program, scalarClosure) &&
             !containsHierarchyStorage(program, scalarStorage) &&
             !containsHierarchyStorage(program, sharedOwner) &&
             !containsHierarchyStorage(program,
                                       SemanticType::uniqueOwnerOf(wrapper)) &&
             !containsHierarchyStorage(program, functionPointer) &&
             !containsHierarchyStorage(program, recursiveOwner) &&
             !containsHierarchyStorage(program, integral),
         "sealed nonhierarchy payload, closure, storage and owner handles "
         "remain outside inline hierarchy storage");
  expect(containsHierarchyStorage(program, hierarchyPayload) &&
             containsHierarchyStorage(program, hierarchyClosure) &&
             containsHierarchyStorage(program, hierarchyStorage) &&
             containsHierarchyStorage(program, unionType),
         "payload, closure, vacant storage and union graphs preserve nested "
         "hierarchy exclusion");
  const auto hierarchyPack =
      SemanticType::concreteTypePack(1, {scalar, wrapper});
  expect(containsHierarchyStorage(program, hierarchyPack),
         "concrete packs inspect every exact inline argument");
  expect(!containsHierarchyStorage(
             program,
             SemanticType::referenceTo(recursiveOwner, AccessMode::ReadOnly)) &&
             !containsHierarchyStorage(
                 program, SemanticType::rawPointerTo(recursiveOwner)),
         "reference and pointer recursion stops at validated indirection");

  auto duplicateClass = classes;
  auto duplicateWrapper = classes[2];
  duplicateWrapper.id = 11;
  duplicateClass.push_back(duplicateWrapper);
  expect(
      rejects(inventory(duplicateClass, payloads, integrals, lambdas), wrapper),
      "duplicate concrete class rows fail closed");
  auto duplicateClassId = classes;
  duplicateClassId.push_back({.id = 1, .type = SemanticType::classType(200)});
  expect(rejects(inventory(duplicateClassId, payloads, integrals, lambdas),
                 scalar),
         "duplicate class row identities fail closed");
  auto duplicatePayload = payloads;
  duplicatePayload.push_back(payloads[1]);
  expect(rejects(inventory(classes, duplicatePayload, integrals, lambdas),
                 hierarchyPayload),
         "duplicate payload rows fail closed");
  auto malformedPayload = payloads;
  malformedPayload[1].variants[0].payloadSymbols.clear();
  expect(rejects(inventory(classes, malformedPayload, integrals, lambdas),
                 hierarchyPayload),
         "payload field arity must retain its exact sealed inventory");

  auto incompatibleTypes = lambdas;
  incompatibleTypes[2].captureTypes[0] = scalar;
  expect(rejects(inventory(classes, payloads, integrals, incompatibleTypes),
                 hierarchyClosure),
         "incompatible closure capture types fail closed");
  auto incompatibleModes = lambdas;
  incompatibleModes[2].captureModes[0] = LambdaCaptureMode::Move;
  expect(rejects(inventory(classes, payloads, integrals, incompatibleModes),
                 hierarchyClosure),
         "incompatible closure capture modes fail closed");
  auto incompatibleCleanup = lambdas;
  incompatibleCleanup[2].captureRequiresActiveCleanup[0] = true;
  expect(rejects(inventory(classes, payloads, integrals, incompatibleCleanup),
                 hierarchyClosure),
         "incompatible closure cleanup layouts fail closed");
  auto duplicateLambdaId = lambdas;
  duplicateLambdaId[2].id = duplicateLambdaId[1].id;
  expect(rejects(inventory(classes, payloads, integrals, duplicateLambdaId),
                 hierarchyClosure),
         "duplicate closure row identities fail closed");
  auto malformedLambda = lambdas;
  malformedLambda[1].captureSymbols.clear();
  expect(rejects(inventory(classes, payloads, integrals, malformedLambda),
                 hierarchyClosure),
         "closure capture vectors require exact parallel arity");
  auto unknownMode = lambdas;
  unknownMode[1].captureModes[0] = static_cast<LambdaCaptureMode>(255);
  expect(rejects(inventory(classes, payloads, integrals, unknownMode),
                 hierarchyClosure),
         "unknown closure storage modes fail closed");
  expect(rejects(program, cyclicClass) && rejects(program, cyclicClosure),
         "mixed class and closure inline cycles fail closed");
  expect(rejects(program, SemanticType::classType(999)) &&
             rejects(program, SemanticType::enumType(999)) &&
             rejects(program,
                     SemanticType::lambdaType(999, SemanticType::Int32,
                                              noParameters, scalarCapture)) &&
             rejects(program, SemanticType::typePack(999)) &&
             rejects(program, SemanticType(SemanticType::SharedPointer)),
         "missing, symbolic and unsupported storage identities never imply "
         "ordinary byte transport");
}

void testHierarchyPlacementOwnership() {
  using namespace lang;
  using native_llvm::Placement;

  const auto verified = lower(R"(
    class P {
      int value;
    public:
      P(int input) : value(input) {}
      virtual int read() { return this.value; }
    };
    int main() {
      P original{7};
      P explicit_copy{original};
      P implicit_copy = original;
      return explicit_copy.read() + implicit_copy.read() - 14;
    }
  )");
  const auto &program = verified.program().mir();
  std::optional<MirBodyAddress> selectedAddress;
  MirBlockId selectedBlock = 0;
  MirInstructionId selectedInstruction = 0;
  MirValueId selectedValue = 0;
  MirPlaceId selectedDestination = 0;
  for (const auto address : enumerateMirBodyAddresses(program)) {
    const auto *body = findMirBody(program, address);
    for (const auto &block : body->blocks)
      for (const auto &stage : block.instructions) {
        if (!native_llvm::isFreshHierarchyTransferProducer(stage) ||
            !native_llvm::containsHierarchyStorage(program, stage.info.type))
          continue;
        try {
          const auto transfer =
              native_llvm::planFreshHierarchyTransfer(program, *body, stage);
          const Placement placement(program, *body);
          const auto *target = placement.target(*stage.result);
          expect(placement.fresh(*stage.result) && target &&
                     target->place == transfer.destination,
                 "program-aware placement binds only the exact proved "
                 "hierarchy publication");
          selectedAddress = address;
          selectedBlock = block.id;
          selectedInstruction = stage.id;
          selectedValue = *stage.result;
          selectedDestination = transfer.destination;
          break;
        } catch (const BackendUnsupportedError &) {
        }
      }
    if (selectedAddress)
      break;
  }
  expect(selectedAddress && selectedValue && selectedDestination,
         "the verified fresh-copy fixture contains an exact private transfer "
         "plan without changing public admission");
  if (!selectedAddress)
    return;

  const auto *ownedBody = findMirBody(program, *selectedAddress);
  MirBody detached = *ownedBody;
  bool foreignRejected = false;
  try {
    const Placement placement(program, detached);
  } catch (const std::invalid_argument &) {
    foreignRejected = true;
  }
  expect(foreignRejected,
         "program-aware placement rejects a detached foreign MIR body");

  MirProgram wrongKind = program;
  auto *wrongBody =
      const_cast<MirBody *>(findMirBody(wrongKind, *selectedAddress));
  wrongBody->kind = wrongBody->kind == MirBodyKind::Function
                        ? MirBodyKind::Constructor
                        : MirBodyKind::Function;
  bool kindRejected = false;
  try {
    const Placement placement(wrongKind, *wrongBody);
  } catch (const std::logic_error &) {
    kindRejected = true;
  }
  expect(kindRejected,
         "program-aware placement rejects a body-kind identity mismatch");

  MirProgram unsupported = program;
  auto *unsupportedBody =
      const_cast<MirBody *>(findMirBody(unsupported, *selectedAddress));
  MirInstruction *unsupportedProducer = nullptr;
  for (auto &block : unsupportedBody->blocks)
    if (block.id == selectedBlock)
      for (auto &stage : block.instructions)
        if (stage.id == selectedInstruction)
          unsupportedProducer = &stage;
  expect(unsupportedProducer,
         "the unsupported-placement mutation retains its exact producer");
  if (!unsupportedProducer)
    return;
  const auto &source = unsupportedProducer->operands.front();
  MirPlaceId sourcePlace = source.place;
  if (!sourcePlace && source.loan) {
    const auto *loan = unsupportedBody->findLoan(source.loan);
    sourcePlace = loan ? loan->source : 0;
  }
  auto *place = const_cast<MirPlace *>(unsupportedBody->findPlace(sourcePlace));
  expect(place, "the exact transfer mutation retains its source place");
  if (!place)
    return;
  place->foreignStorage = true;
  const Placement denied(unsupported, *unsupportedBody);
  expect(!denied.fresh(selectedValue) && !denied.target(selectedValue),
         "an unsupported hierarchy candidate gains no generic placement "
         "authority");
}

bool has(const lang::LlvmAdmissionPlan &plan, std::string_view family,
         std::string_view name) {
  return std::any_of(plan.requirements().begin(), plan.requirements().end(),
                     [&](const auto &use) {
                       return use.entry->family == family &&
                              use.entry->name == name;
                     });
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

  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
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
                               entry.positiveTest != "llvm_admission";
      const bool hasGap = !entry.positiveEvidenceGap.empty();
      expect(hasEvidence != hasGap,
             "implemented rows name execution evidence or one explicit gap");
    } else if (entry.state == LlvmCoverageState::Pending) {
      expect(entry.positiveTest.empty() && entry.positiveAssertion.empty() &&
                 entry.positiveEvidenceGap.empty(),
             "pending rows cannot masquerade as accepted execution");
    } else {
      expect(entry.positiveTest == "llvm_admission" &&
                 entry.positiveAssertion == "ledger.nonexecuting" &&
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

constexpr std::string_view exactPackConstructionSource = R"(
T pack_construct<T, Args...>(Args... values) { return T(values...); }
int main() { return pack_construct<int>(7) - 7; }
)";

void testNonExecutingEvidence() {
  const auto packed = lower(exactPackConstructionSource);
  std::size_t identities = 0;
  std::size_t exactIntrinsics = 0;
  for (const auto &instance : packed.program().mir().functionInstances()) {
    for (const auto &block : instance.body.blocks) {
      for (const auto &instruction : block.instructions) {
        identities += instruction.operation == lang::MirOperation::Identity;
        exactIntrinsics += instruction.intrinsic ==
                           lang::IntrinsicKind::ExactTypeParameterConstruction;
      }
    }
  }
  const auto plan = lang::planLlvmAdmission(packed);
  expect(identities != 0 && exactIntrinsics == 0 &&
             has(plan, "MirOperation", "Identity") &&
             !has(plan, "IntrinsicKind", "ExactTypeParameterConstruction"),
         "exact pack construction lowers to Identity before LLVM admission");

  auto frontend = lang::Frontend().analyze(
      "llvm-exact-pack-forge.gti", std::string(exactPackConstructionSource));
  expect(frontend.canGenerateCode(),
         "exact pack forge fixture must pass the frontend");
  bool forged = false;
  for (const auto &instance : frontend.mir.functionInstances()) {
    auto *body = lang::findMirBody(frontend.mir,
                                   {lang::MirBodyKind::Function, instance.id});
    if (!body)
      continue;
    for (auto &block : body->blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.operation != lang::MirOperation::Identity)
          continue;
        instruction.intrinsic =
            lang::IntrinsicKind::ExactTypeParameterConstruction;
        forged = true;
        break;
      }
      if (forged)
        break;
    }
    if (forged)
      break;
  }
  expect(forged, "fixture must expose one lowered exact Identity operation");
  auto build = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, lang::TargetInfo::host());
  expect(build.valid(),
         "the current core MIR verifier delegates this HIR-only absence "
         "boundary to sealed backend admission");
  if (!build.valid())
    return;
  std::vector<lang::LoweredProgramIssue> issues;
  const auto verified =
      lang::verifiedLoweredProgram(std::move(*build.program), issues);
  expect(verified.has_value(),
         "the forged fixture must still pass the current construction seal");
  if (!verified)
    return;
  const auto rejected = lang::planLlvmAdmission(*verified);
  expect(!rejected.admitted() && rejected.firstUnsupported() &&
             rejected.firstUnsupported()->entry->key() ==
                 "IntrinsicKind.ExactTypeParameterConstruction",
         "LLVM admission rejects a forged executable HIR-only intrinsic");
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
  const auto instructionFor = [](const lang::LlvmAdmissionPlan &plan,
                                 const lang::LlvmCoverageUse &use) {
    const auto *body =
        use.body ? lang::findMirBody(plan.program().mir(), *use.body) : nullptr;
    const auto *block = body ? body->findBlock(use.block) : nullptr;
    if (!block)
      return static_cast<const lang::MirInstruction *>(nullptr);
    const auto found =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [&](const auto &instruction) {
                       return instruction.id == use.instruction;
                     });
    return found == block->instructions.end() ? nullptr : &*found;
  };

  const auto ownedCallable = lang::planLlvmAdmission(lower(R"(
import std.memory;
F retain<F>(F callable) { return std::move(callable); }
int main() {
  auto stored = retain([]() -> int { return 0; });
  return stored();
}
)",
                                                           true));
  std::size_t ownedDescriptors = 0;
  for (const auto &use : ownedCallable.requirements()) {
    if (!use.required || use.entry->family != "CallableBoundary" ||
        use.entry->name != "Owned")
      continue;
    const auto *instruction = instructionFor(ownedCallable, use);
    if (instruction && std::any_of(instruction->callableArguments.begin(),
                                   instruction->callableArguments.end(),
                                   [](const auto &argument) {
                                     return argument.boundary ==
                                            lang::CallableBoundary::Owned;
                                   }))
      ownedDescriptors += use.occurrences;
  }
  expect(ownedCallable.admitted() && ownedDescriptors == 1,
         "an exact owned callable argument descriptor reaches admission");

  constexpr std::string_view confinedCallableSource = R"(
int apply<F>(F& callable, int value) { return callable(value); }
int main() {
  auto callback = [](int value) -> int { return value; };
  return apply(callback, 0);
}
)";
  const auto confinedCallable =
      lang::planLlvmAdmission(lower(confinedCallableSource));
  bool confinedDescriptor = false;
  bool confinedInvocation = false;
  for (const auto &use : confinedCallable.requirements()) {
    if (!use.required || use.entry->family != "CallableBoundary" ||
        use.entry->name != "Confined")
      continue;
    const auto *instruction = instructionFor(confinedCallable, use);
    if (!instruction)
      continue;
    confinedDescriptor |= std::any_of(
        instruction->callableArguments.begin(),
        instruction->callableArguments.end(), [](const auto &argument) {
          return argument.boundary == lang::CallableBoundary::Confined;
        });
    confinedInvocation |=
        instruction->callableBoundary == lang::CallableBoundary::Confined;
  }
  expect(confinedCallable.admitted() && confinedDescriptor &&
             confinedInvocation,
         "confined callable argument and invocation boundaries remain "
         "independent inventory occurrences");

  const auto confinedFrontend = lang::Frontend().analyze(
      "llvm-callable-boundary.gti", std::string(confinedCallableSource));
  expect(confinedFrontend.canGenerateCode(),
         "confined callable descriptor control passes the frontend");
  for (unsigned mutation = 0; mutation < 2; ++mutation) {
    auto malformed = confinedFrontend.mir;
    bool changed = false;
    for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
             malformed.functionInstances()))
      for (auto &block : function.body.blocks)
        for (auto &instruction : block.instructions)
          if (!changed && !instruction.callableArguments.empty()) {
            if (mutation == 0)
              instruction.callableArguments.front().boundary =
                  lang::CallableBoundary::Owned;
            else
              ++instruction.callableArguments.front().parameterIndex;
            changed = true;
          }
    expect(changed && !lang::verifyMirProgram(malformed).valid(),
           "forged callable argument boundary descriptors fail MIR "
           "verification");
  }

  const auto addressConversion = lang::planLlvmAdmission(lower(R"(
struct Reading { int value = 42;  public: Reading() = default; };
using Erased = void*;
using Typed = Reading*;
int main() {
  mut Reading owner{};
  Typed address = &owner;
  Erased erased = Erased(address);
  Typed restored = Typed(erased);
  return restored == address ? 0 : 1;
}
)"));
  std::size_t addressConversions = 0;
  for (const auto &use : addressConversion.requirements())
    if (use.required && use.entry->family == "MirOperation" &&
        use.entry->name == "Convert" && use.implementedShape &&
        use.implementedShape->name == "NativeObjectAddress")
      addressConversions += use.occurrences;
  expect(addressConversion.admitted() && addressConversions == 2,
         "verified typed-to-void-to-same-typed conversion uses the exact "
         "native object-address witness");

  const auto numericConversion = lang::planLlvmAdmission(lower(R"(
int widen(int8_t value) { return int(value); }
int main() { return widen(int8_t(0)); }
)"));
  std::size_t numericConverts = 0;
  bool numericObjectAddress = false;
  for (const auto &use : numericConversion.requirements())
    if (use.required && use.entry->family == "MirOperation" &&
        use.entry->name == "Convert") {
      numericConverts += use.occurrences;
      numericObjectAddress |=
          use.implementedShape &&
          use.implementedShape->name == "NativeObjectAddress";
    }
  expect(numericConversion.admitted() && numericConverts != 0 &&
             !numericObjectAddress,
         "ordinary numeric Convert does not borrow the object-address "
         "witness");

  const auto directConsuming = lang::planLlvmAdmission(lower(R"(
class Value { public: int read() && { return 42; }  public: Value() = default; };
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
  const auto pointerArrays = lang::planLlvmAdmission(lower(R"(
int main() {
  mut int value = 1;
  int* pointers[2][2] = {{&value, nullptr}, {nullptr, &value}};
  int* copy[2][2] = pointers;
  return copy[1][1] == &value ? 0 : 1;
}
)"));
  expect(pointerArrays.admitted(),
         "raw pointer arrays use passive value storage");
  bool pointerAggregate = false;
  for (const auto &use : pointerArrays.requirements())
    if (use.entry->family == "MirOperation" && use.entry->name == "Aggregate") {
      pointerAggregate = true;
      expect(use.entry->state == lang::LlvmCoverageState::Pending &&
                 use.implementedShape &&
                 use.implementedShape->name == "PassiveArrayValue",
             "pointer leaves refine arrays without admitting all aggregates");
    }
  expect(pointerAggregate, "pointer array fixture must construct an aggregate");
  const auto pointerArrayEquality = lang::planLlvmAdmission(lower(R"(
int main() { int* left[2] = {}; int* right[2] = {}; return left == right ? 0 : 1; }
)"));
  expect(!pointerArrayEquality.admitted() &&
             pointerArrayEquality.firstUnsupported()->entry->key() ==
                 "Shape.ArrayValueComparison",
         "pointer array storage does not invent element comparison semantics");
  // An extent query reads no receiver bytes, so a class-element receiver
  // reports the constant-extent witness rather than a passive-array claim.
  const auto classExtents = lang::planLlvmAdmission(lower(R"(
struct Slot { int value; Slot(int value) : value(value) {} };
int main() {
  Slot slots[2] = {Slot(3), Slot(4)};
  mut int total = 0;
  for (auto& slot : slots) { total += slot.value; }
  return total == 7 && slots.size() == uint64_t(2) ? 0 : 1;
}
)"));
  bool extentSize = false, extentBegin = false;
  for (const auto &use : classExtents.requirements())
    if (use.entry->family == "IntrinsicKind" &&
        (use.entry->name == "ArraySize" ||
         use.entry->name == "ArrayRangeBegin")) {
      (use.entry->name == "ArraySize" ? extentSize : extentBegin) = true;
      expect(use.entry->state == lang::LlvmCoverageState::Pending &&
                 use.implementedShape &&
                 use.implementedShape->name == "ArrayExtentQuery",
             "a class-array extent query uses only the constant extent");
    }
  expect(classExtents.admitted() && extentSize && extentBegin,
         "range-for and size() over class elements read no element");
  // A sealed integral enum array is exactly its underlying integer array.
  // Only complete one-dimensional formation is claimed.
  const auto enumArrays = lang::planLlvmAdmission(lower(R"(
enum class Phase : uint8_t { idle, running };
struct Track {
  Phase stops[2];
  Track(Phase initial[2]) : stops(initial) {}
  Phase at(uint64_t index) { return this.stops[index]; }
};
int main() {
  Phase first = Phase::running;
  Track track = Track({first, Phase::idle});
  return track.at(uint64_t(0)) == Phase::running ? 0 : 1;
}
)"));
  bool enumAggregate = false;
  for (const auto &use : enumArrays.requirements())
    if (use.entry->family == "MirOperation" && use.entry->name == "Aggregate") {
      enumAggregate = true;
      expect(use.implementedShape &&
                 use.implementedShape->name == "IntegralEnumArrayValue",
             "an enum array aggregate is integral enum array formation");
    }
  expect(enumArrays.admitted() && enumAggregate,
         "one-dimensional integral enum arrays use underlying integer storage");
  const auto nestedEnumArrays = lang::planLlvmAdmission(lower(R"(
enum class Phase : uint8_t { idle, running };
struct Grid {
  Phase cells[2][2];
  Grid(Phase a, Phase b) : cells({{a, b}, {b, a}}) {}
};
int main() { Grid grid = Grid(Phase::idle, Phase::running); return 0; }
)"));
  expect(!nestedEnumArrays.admitted() &&
             nestedEnumArrays.firstUnsupported()->entry->key() ==
                 "MirOperation.Aggregate",
         "nested integral enum arrays remain outside the enum array witness");
  // Raw payload access strides by the payload layout and transports the
  // address; an element-count difference remains a separate operation.
  const auto payloadDifference = lang::planLlvmAdmission(lower(R"(
enum Plain { none, pair(int first, int second) };
struct Window { mut Plain values[2] = {Plain::none, Plain::pair(1, 2)};  public: Window() = default; };
int main() {
  mut Window window{};
  unsafe {
    Plain* first = &window.values[0];
    Plain* last = first + uint64_t(1);
    return last - first == 1 ? 0 : 1;
  }
}
)"));
  bool payloadAdvance = false;
  for (const auto &use : payloadDifference.requirements())
    if (use.entry->family == "MirOperation" &&
        use.entry->name == "PointerAdd") {
      payloadAdvance = true;
      expect(use.implementedShape &&
                 use.implementedShape->name == "NativePayloadRawAccess",
             "payload pointer advance uses the payload raw-access witness");
    }
  expect(payloadAdvance && !payloadDifference.admitted() &&
             payloadDifference.firstUnsupported()->entry->key() ==
                 "MirOperation.PointerDifference",
         "payload raw access does not claim pointer difference");
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
class Owner { public: virtual int read() { return 0; }  public: Owner() = default; };
class Derived : public Owner { public: Derived() = default; };
unsafe int create(mut Owner& value, Owner& other) { value = other; return 0; }
int main() { mut Derived first{}; Derived second{}; unsafe { return create(first, second); } }
)"));
  expect(!unsafeOwner.admitted() &&
             has(unsafeOwner, "Shape", "PolymorphicTransfer"),
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

void testRawFieldWitnesses() {
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

void testDispatchStateDependencies() {
  using namespace lang;
  const auto verified = lower(R"(
int selected_helper() { return 7; }
int unused_helper() { auto call = []() -> int { return 9; }; return call(); }
class Generated {
public:
  virtual int read() { return selected_helper(); }
  int unused() { return unused_helper(); }
public:
  Generated() = default;
};
class Source {
public:
  Source() {}
  virtual int read() { return 8; }
};
class Base {
public:
  Base() {}
  virtual int phase() { return 1; }
};
class Derived : public Base {
public:
  Derived() : Base() {}
  int phase() override { return 2; }
  ~Derived() {}
};
class Dormant { public: virtual int read() { return unused_helper(); }  public: Dormant() = default; };
class EmptyBase { public: virtual int phase() { return 3; }  public: EmptyBase() = default; };
class EmptyLeaf : public EmptyBase {
public:
  EmptyLeaf() : EmptyBase() {}
  int phase() override { return 4; }
};
void dormant(Dormant& value) {}
int main() {
  Generated generated{};
  Source source{};
  Derived derived{};
  EmptyLeaf empty{};
  return 0;
}
)");
  const auto plan = planLlvmAdmission(verified);
  const auto required = [&](MirBodyAddress body) {
    return std::find(plan.requiredBodies().begin(), plan.requiredBodies().end(),
                     body) != plan.requiredBodies().end();
  };
  expect(plan.closure() == LlvmAdmissionClosure::SealedEntryDependencies,
         "no-call dispatch states retain the exact sealed dependency closure");
  unsigned activeClasses = 0;
  for (const auto &object : verified.program().mir().classInstances()) {
    const auto *declaration =
        verified.program().findClassDeclaration(object.declaration);
    if (!declaration)
      continue;
    if (declaration->qualifiedName == "Dormant") {
      for (const auto &selection : object.virtualDispatch)
        expect(!required({MirBodyKind::Function, selection.target}),
               "an unreachable class does not root its virtual implementation");
    } else if (declaration->qualifiedName == "Generated" ||
               declaration->qualifiedName == "Source" ||
               declaration->qualifiedName == "Base" ||
               declaration->qualifiedName == "Derived" ||
               declaration->qualifiedName == "EmptyBase" ||
               declaration->qualifiedName == "EmptyLeaf") {
      ++activeClasses;
      for (const auto &selection : object.virtualDispatch)
        expect(
            required({MirBodyKind::Function, selection.target}),
            "generated/source construction and static cleanup require each "
            "exact base/derived phase implementation without a virtual call");
    }
  }
  expect(activeClasses == 6,
         "all no-call dispatch-state controls are instantiated");
  bool helper = false;
  for (const auto &function : verified.program().mir().functionInstances()) {
    const auto *declaration =
        verified.program().findFunctionDeclaration(function.declaration);
    if (!declaration)
      continue;
    if (declaration->qualifiedName == "selected_helper") {
      helper = true;
      expect(required({MirBodyKind::Function, function.id}),
             "a selected table target retains its transitive ordinary helper");
    }
    if (declaration->qualifiedName == "unused_helper" ||
        declaration->qualifiedName == "Generated::unused" ||
        declaration->qualifiedName == "dormant")
      expect(!required({MirBodyKind::Function, function.id}),
             "dispatch-state demand does not root unrelated class methods or "
             "helpers");
  }
  expect(helper, "the transitive helper has a sealed source instance");
  const auto repeated = planLlvmAdmission(verified);
  expect(std::equal(plan.requiredBodies().begin(), plan.requiredBodies().end(),
                    repeated.requiredBodies().begin(),
                    repeated.requiredBodies().end()),
         "dispatch-state closure is deterministic in sealed inventory order");
  const auto unsupported = planLlvmAdmission(lower(R"(
int unsupported_helper() { auto call = []() -> int { return 9; }; return call(); }
class Value { public: virtual int read() { return unsupported_helper(); }  public: Value() = default; };
int main() { Value value{}; return 0; }
)"));
  const auto unsupportedRequired = [&](MirBodyAddress address) {
    return std::find(unsupported.requiredBodies().begin(),
                     unsupported.requiredBodies().end(),
                     address) != unsupported.requiredBodies().end();
  };
  bool callableHelper = false, callableVirtualTarget = false;
  for (const auto &function : unsupported.program().mir().functionInstances()) {
    const auto *declaration =
        unsupported.program().findFunctionDeclaration(function.declaration);
    if (!declaration)
      continue;
    if (declaration->qualifiedName == "unsupported_helper")
      callableHelper =
          unsupportedRequired({MirBodyKind::Function, function.id});
    if (declaration->qualifiedName == "Value::read")
      callableVirtualTarget =
          unsupportedRequired({MirBodyKind::Function, function.id});
  }
  expect(
      unsupported.closure() == LlvmAdmissionClosure::SealedEntryDependencies &&
          unsupported.admitted() && callableHelper && callableVirtualTarget &&
          std::any_of(unsupported.requirements().begin(),
                      unsupported.requirements().end(),
                      [](const auto &use) {
                        return use.required &&
                               use.entry->key() ==
                                   "SemanticType::Kind.Lambda" &&
                               use.implementedShape &&
                               use.implementedShape->name ==
                                   "NativeCallableStorage";
                      }),
      "a table target's callable helper retains the exact sealed virtual "
      "target, helper dependency and callable representation witness");
}

void testVirtualCallDependencies() {
  using namespace lang;
  const auto verified = lower(R"(
int alternate_helper() { return 2; }
int unrelated_helper() { return 3; }
class Base { public: virtual int read() { return 1; }  public: Base() = default; };
class Alternative : public Base {
public:
  int read() override { return alternate_helper(); }
public:
  Alternative() = default;
};
class Unrelated { public: virtual int read() { return unrelated_helper(); }  public: Unrelated() = default; };
void retain_alternative_type(Alternative& value) {}
void retain_unrelated_type(Unrelated& value) {}
int invoke(Base& value) { return value.read(); }
int main() { Base value{}; return invoke(value); }
)");
  const auto plan = planLlvmAdmission(verified);
  expect(plan.admitted() &&
             plan.closure() == LlvmAdmissionClosure::SealedEntryDependencies,
         "sealed virtual dispatch retains a bounded dependency closure");
  std::set<std::string> required;
  for (const auto &function : verified.program().mir().functionInstances()) {
    const auto *declaration =
        verified.program().findFunctionDeclaration(function.declaration);
    if (declaration &&
        std::find(plan.requiredBodies().begin(), plan.requiredBodies().end(),
                  MirBodyAddress{MirBodyKind::Function, function.id}) !=
            plan.requiredBodies().end())
      required.insert(declaration->qualifiedName);
  }
  expect(required.contains("Alternative::read") &&
             required.contains("alternate_helper") &&
             required.contains("Base::read"),
         "virtual calls retain all sealed overrides and transitive helpers, "
         "even for a derived type not constructed by this entry");
  expect(!required.contains("Unrelated::read") &&
             !required.contains("unrelated_helper") &&
             !required.contains("retain_alternative_type") &&
             !required.contains("retain_unrelated_type"),
         "a matching spelling is not a dispatch identity or a body root");
  const auto repeated = planLlvmAdmission(verified);
  expect(std::equal(plan.requiredBodies().begin(), plan.requiredBodies().end(),
                    repeated.requiredBodies().begin(),
                    repeated.requiredBodies().end()),
         "virtual dependency order is deterministic in sealed inventory order");
  const auto unsupported = planLlvmAdmission(lower(R"(
class Base { public: virtual int read() { return 1; }  public: Base() = default; };
int unsupported_helper() { Base values[1] = {Base()}; Base copied[1] = values; return copied[0].read(); }
class Alternative : public Base {
public:
  int read() override { return unsupported_helper(); }
public:
  Alternative() = default;
};
void retain_type(Alternative& value) {}
int invoke(Base& value) { return value.read(); }
int main() { Base value{}; return invoke(value); }
)"));
  expect(
      !unsupported.admitted() &&
          std::any_of(unsupported.requirements().begin(),
                      unsupported.requirements().end(),
                      [](const auto &use) {
                        return use.required &&
                               use.entry->key() == "Shape.PolymorphicTransfer";
                      }),
      "a possible override's unsupported transitive helper remains required");
}

// A view returned through a virtual call keeps the loan source the static
// call's view has: its Return loan is borrow metadata either way, and MIR
// verification rejects any forged relay before admission sees it.
void testVirtualTextRelayReturns() {
  using namespace lang;
  const auto relay = [](bool dispatched) {
    return std::string(R"(
import std.string;
class Base {
public:
  std::string text;
  Base() : text(std::string("ann")) {}
  )") + (dispatched ? "virtual " : "") +
           R"(std::string_view name() { return this.text.view(); }
};
class Derived : public Base {
public:
  Derived() : Base() {}
};
std::string_view call(Base& n) { return n.name(); }
int main() {
  Derived p{};
  std::string_view v = call(p);
  return int(v.size()) - 3;
}
)";
  };
  std::vector<std::string> witnesses[2];
  for (const bool dispatched : {false, true}) {
    const auto plan = planLlvmAdmission(lower(relay(dispatched), true));
    expect(plan.admitted(), dispatched
                                ? "a virtual call's returned view is admitted"
                                : "a static call's returned view is admitted");
    for (const auto &use : plan.requirements())
      if (use.required && use.entry->key() == "MirLoanKind.Return")
        witnesses[dispatched].push_back(
            use.implementedShape ? std::string(use.implementedShape->name)
                                 : std::string("<none>"));
    if (!dispatched)
      continue;
    // Forged relays never reach admission: the Return loan must escape, the
    // return must name it, and its Borrow must relay the returned value.
    for (int mutation = 0; mutation != 3; ++mutation) {
      auto changed = plan.program().mir();
      bool forged = false;
      for (const auto address : plan.requiredBodies()) {
        auto &body = *findMirBody(changed, address);
        for (auto &loan : body.loans) {
          if (loan.kind != MirLoanKind::Return || forged)
            continue;
          if (mutation == 0) {
            loan.escapes = false;
            forged = true;
          } else if (mutation == 1) {
            for (auto &block : body.blocks)
              if (block.terminator.returnLoan == loan.id) {
                block.terminator.returnLoan = 999999;
                forged = true;
              }
          } else {
            for (auto &block : body.blocks)
              for (auto &stage : block.instructions)
                if (stage.loan == loan.id &&
                    stage.kind == MirInstructionKind::Borrow &&
                    stage.operands.size() == 1) {
                  stage.operands.front().loan = loan.id;
                  forged = true;
                }
          }
        }
        if (forged)
          break;
      }
      expect(forged && !verifyMirProgram(changed).valid(),
             "forged view relay " + std::to_string(mutation) +
                 " fails program verification");
    }
  }
  expect(!witnesses[1].empty() && witnesses[0] == witnesses[1] &&
             std::all_of(witnesses[1].begin(), witnesses[1].end(),
                         [](const auto &name) {
                           return name == "NativeBorrowMetadata";
                         }),
         "a virtual call's returned view has the static call's witness");
}

// A reference, span or record reference that a call selects out of
// hierarchy storage it borrows is that call's checked result, relayed alike
// by static, virtual and interface calls and through a receiver or an
// argument. A forged relay or selection never grants that admission.
void testSelectedResultRelays() {
  using namespace lang;
  struct Result {
    const char *type;
    const char *member;
    const char *use;
  };
  const Result results[] = {
      {"int32_t&", "return this.value;", "relay(owner) == 5"},
      {"std::span<int32_t>", "return this.values.as_span();",
       "relay(owner).size() == std::size_t(1)"},
      {"Record&", "return this.record;",
       "relay(owner).name.size() == std::size_t(6)"},
  };
  const auto program = [](const Result &result, int call) {
    const bool interface = call == 2;
    const std::string member = std::string(result.type) + " select()" +
                               (interface ? " override" : "") + " { " +
                               result.member + " }";
    return std::string(R"(
import std.span;
import std.string;
import std.vector;
class Record {
public:
  std::string name;
  Record() : name(std::string("record")) {}
};
)") +
           (interface ? std::string("interface Picker {\n  ") + result.type +
                            " select();\n};\n"
                      : std::string()) +
           "class Base" + (interface ? " : public Picker" : "") + R"( {
public:
  int32_t value;
  mut std::vector<int32_t> values;
  Record record;
  Base() : value(5), record(Record()) {}
  virtual void poke() {}
  )" + (call == 1 ? "virtual " : "") +
           member + "\n};\n" + result.type + " relay(" +
           (interface ? "Picker" : "Base") +
           "& item) { return item.select(); }\n" + R"(int main() {
  mut Base owner{};
  owner.values.push_back(1);
  return )" +
           result.use + " ? 0 : 1;\n}\n";
  };
  const std::set<std::string_view> relayed{
      "MirLoanKind.CallResult", "MirLoanKind.Return",
      "MirInstructionKind.Borrow", "MirOperandKind.Loan"};
  for (const auto &result : results) {
    std::vector<std::string> witnesses[3];
    for (const int call : {0, 1, 2}) {
      const auto plan = planLlvmAdmission(lower(program(result, call), true));
      expect(plan.admitted(), std::string("a ") +
                                  (call == 0   ? "static"
                                   : call == 1 ? "virtual"
                                               : "interface") +
                                  " call's selected " + result.type +
                                  " relays on LLVM");
      for (const auto &use : plan.requirements())
        if (use.required && relayed.contains(use.entry->key()))
          witnesses[call].push_back(
              use.entry->key() + "=" +
              (use.implementedShape ? std::string(use.implementedShape->name)
                                    : std::string("<none>")));
      std::sort(witnesses[call].begin(), witnesses[call].end());
    }
    expect(!witnesses[0].empty() && witnesses[0] == witnesses[1],
           std::string("a virtual call's selected ") + result.type +
               " has the static call's witnesses");
  }

  // Forged selections and relays never grant admission, whichever stage
  // refuses them: the relayed loan must be the selecting call's own result,
  // and the selecting input must borrow that result's source.
  const auto source = std::string(R"(
class Base {
public:
  int32_t value;
  Base() : value(5) {}
  virtual int32_t& slot() { return this.value; }
};
int32_t& relay(Base& base) { return base.slot(); }
int main() {
  Base owner{};
  Base other{};
  return relay(owner) == 5 && other.value == 5 ? 0 : 1;
}
)");
  const auto frontend = Frontend().analyze("selected-relay.gti", source);
  expect(frontend.canGenerateCode() &&
             planLlvmAdmission(lower(source)).admitted(),
         "the unforged selected relay is admitted");
  for (int mutation = 0; mutation != 3; ++mutation) {
    auto mir = frontend.mir;
    bool forged = false;
    for (auto &function : const_cast<std::vector<MirFunctionInstance> &>(
             mir.functionInstances())) {
      auto &body = function.body;
      for (auto &block : body.blocks)
        for (auto &stage : block.instructions) {
          if (forged || stage.tombstone)
            continue;
          const auto *loan = stage.loan ? body.findLoan(*stage.loan) : nullptr;
          if (mutation == 0 && stage.kind == MirInstructionKind::Borrow &&
              stage.operands.size() == 1 &&
              stage.operands.front().kind == MirOperandKind::Loan) {
            // Relay the parameter's entry loan instead of the call's result.
            for (const auto &entry : body.loans)
              if (entry.kind == MirLoanKind::Parameter && entry.entry) {
                stage.operands.front().loan = entry.id;
                forged = true;
              }
          } else if (mutation == 1 && loan &&
                     loan->kind == MirLoanKind::CallResult &&
                     stage.kind == MirInstructionKind::Call &&
                     !stage.receiver) {
            // Name a hierarchy local the call never borrowed.
            for (const auto &place : body.places)
              if (place.id != loan->source &&
                  place.type == body.findPlace(loan->source)->type) {
                const_cast<MirLoan *>(loan)->source = place.id;
                forged = true;
                break;
              }
          } else if (mutation == 2 && loan &&
                     loan->kind == MirLoanKind::CallResult &&
                     stage.kind == MirInstructionKind::Call &&
                     !stage.receiver) {
            // A selection with no borrowed input.
            stage.operands.clear();
            stage.parameterTypes.clear();
            forged = true;
          }
        }
    }
    auto build = LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir, mir, mir, {},
        TargetInfo::host());
    bool refused = !build.valid();
    if (build.valid()) {
      std::vector<LoweredProgramIssue> issues;
      auto verified = verifiedLoweredProgram(std::move(*build.program), issues);
      refused = !verified || !planLlvmAdmission(*verified).admitted();
    }
    expect(forged && refused, "forged selection " + std::to_string(mutation) +
                                  " never grants admission");
  }
}

void testHierarchyReferenceReturns() {
  using namespace lang;
  const auto plan = planLlvmAdmission(lower(R"(
interface Reader { int read(); };
class Root { public: virtual int other() { return 0; }  public: Root() = default; };
class Leaf : public Root, public Reader {
public:
  int read() override { return 7; }
public:
  Leaf() = default;
};
struct Record {
  mut Leaf leaf{};
  Reader& view() { return this.leaf; }
public:
  Record() = default;
};
Reader& forward(Reader& value) { return value; }
int main() { Record value{}; return forward(value.view()).read(); }
)"));
  expect(plan.admitted(),
         "ordinary source-defined reference returns admit hierarchy views");
  unsigned returns = 0;
  for (const auto &use : plan.requirements()) {
    if (use.required && use.entry->key() == "Shape.BorrowReturn") {
      ++returns;
      expect(use.implementedShape &&
                 use.implementedShape->name == "NativeBorrowedDispatchView",
             "each hierarchy return retains its own checked address witness");
    }
    expect(!(use.required && use.entry->key() == "Shape.PolymorphicTransfer"),
           "returning a hierarchy loan does not transfer object bytes");
  }
  expect(returns >= 2,
         "both receiver and parameter borrow returns are exercised");
  for (int mutation = 0; mutation != 4; ++mutation) {
    auto changed = plan.program().mir();
    bool forged = false;
    for (const auto address : plan.requiredBodies()) {
      auto &body = *findMirBody(changed, address);
      for (auto &block : body.blocks) {
        if (mutation == 3 && block.terminator.returnLoan) {
          block.terminator.returnLoan = 999999;
          forged = true;
          break;
        }
        if (mutation == 3)
          continue;
        for (auto &stage : block.instructions) {
          if (stage.kind != MirInstructionKind::Call || !stage.loan ||
              stage.info.category != ValueCategory::Place)
            continue;
          auto &loan = body.loans.at(*stage.loan - 1);
          if (mutation == 0)
            loan.producedBy = 999999;
          else if (mutation == 1)
            loan.access = AccessMode::Mutable;
          else
            stage.info.type = SemanticType::Bool;
          forged = true;
          break;
        }
        if (forged)
          break;
      }
      if (forged)
        break;
    }
    const auto verification = verifyMirProgram(changed);
    expect(forged && !verification.valid(),
           "forged hierarchy call producer/access/result/return-loan " +
               std::to_string(mutation) + " fails program verification");
    if (mutation == 0)
      expect(std::any_of(
                 verification.errors.begin(), verification.errors.end(),
                 [](const auto &error) {
                   return error.bodyKind == MirBodyKind::Function &&
                          error.owner && error.block && error.instruction &&
                          error.message.find("call-result loan producer") !=
                              std::string::npos;
                 }),
             "producer integrity failure retains the owning call location");
  }
}

void testConservativeClassWitnesses() {
  using namespace lang;
  const auto classesProved = [](const LlvmAdmissionPlan &plan) {
    bool found = false;
    for (const auto &use : plan.requirements())
      if (use.required && use.entry->family == "ClassKind" &&
          use.entry->name == "Class") {
        found = true;
        if (!use.implementedShape ||
            use.implementedShape->name != "NativeClassStorage" ||
            use.entry->state != LlvmCoverageState::Pending)
          return false;
      }
    return found;
  };
  const auto functionRequired = [](const LlvmAdmissionPlan &plan,
                                   std::string_view name) {
    for (const auto &function : plan.program().mir().functionInstances()) {
      const auto *declaration =
          plan.program().findFunctionDeclaration(function.declaration);
      if (declaration && declaration->qualifiedName == name)
        return std::find(plan.requiredBodies().begin(),
                         plan.requiredBodies().end(),
                         MirBodyAddress{MirBodyKind::Function, function.id}) !=
               plan.requiredBodies().end();
    }
    return false;
  };
  const auto callableWitness = [](const LlvmAdmissionPlan &plan,
                                  std::string_view key) {
    return std::any_of(plan.requirements().begin(), plan.requirements().end(),
                       [&](const auto &use) {
                         return use.required && use.entry->key() == key &&
                                use.implementedShape &&
                                use.implementedShape->name ==
                                    "NativeCallableStorage";
                       });
  };
  const auto ordinary = planLlvmAdmission(lower(R"(
class Value { int data; public: Value(int input):data(input){} int read(){return this.data;} };
int inspect(int input) { Value value{input}; return value.read(); }
)"));
  expect(ordinary.admitted() && classesProved(ordinary) &&
             ordinary.closure() == LlvmAdmissionClosure::AllExecutableBodies,
         "ordinary rootless inventory uses a per-use class proof without "
         "omission or family promotion");
  expect(std::any_of(ordinary.requiredBodies().begin(),
                     ordinary.requiredBodies().end(),
                     [](auto address) {
                       return address.kind == MirBodyKind::FieldInitializers;
                     }),
         "conservative class admission retains field declaration bodies");
  const auto body = planLlvmAdmission(lower(R"(
class Value {
  int data = 1;
public:
  int unsupported() { auto call = [](int value)->int{return value;}; return call(this.data); }
public:
  Value() = default;
};
int inspect(Value& value) { return value.unsupported(); }
)"));
  expect(body.admitted() && classesProved(body) &&
             functionRequired(body, "Value::unsupported") &&
             callableWitness(body, "SemanticType::Kind.Lambda"),
         "representable class storage retains the required method body and "
         "its exact callable-storage witness");
  const auto cleanup = planLlvmAdmission(lower(R"(
int unsupported(int input) { auto call=[](int value)->int{return value;}; return call(input); }
class Value {
public:
  Value() {}
  ~Value() { int observed = unsupported(1); }
};
void inspect() { Value value{}; }
)"));
  expect(cleanup.admitted() && classesProved(cleanup) &&
             std::any_of(cleanup.requiredBodies().begin(),
                         cleanup.requiredBodies().end(),
                         [](auto address) {
                           return address.kind == MirBodyKind::Destructor;
                         }) &&
             functionRequired(cleanup, "unsupported") &&
             callableWitness(cleanup, "SemanticType::Kind.Lambda"),
         "source cleanup and its callable helper dependency stay required "
         "after class storage is proved");
  const auto itemCall = planLlvmAdmission(lower(R"(
int identity(int value) { return value; }
class Value { public: int read(){auto callback=identity; return callback(1);}  public: Value() = default; };
int inspect(Value& value) { return value.read(); }
)"));
  expect(itemCall.admitted() && classesProved(itemCall) &&
             functionRequired(itemCall, "Value::read") &&
             functionRequired(itemCall, "identity") &&
             callableWitness(itemCall, "SemanticType::Kind.FunctionItem"),
         "required function-item creation and its target remain inventoried "
         "with the exact callable-storage witness");
  for (
      const auto source :
      {R"(interface Empty {}; void accept(Empty& value) {})",
       R"(class Base {public: Base(){}}; class Derived:public Base {public: Derived():Base(){}}; void accept(Derived& value){})"}) {
    const auto borrowed = planLlvmAdmission(lower(source));
    expect(borrowed.admitted() &&
               std::any_of(borrowed.requirements().begin(),
                           borrowed.requirements().end(),
                           [](const auto &use) {
                             return use.required && use.implementedShape &&
                                    (use.implementedShape->name ==
                                         "NativeBorrowedDispatchView" ||
                                     use.implementedShape->name ==
                                         "NativeStaticDispatchStorage");
                           }),
           "exact interface and state-base metadata has bounded public "
           "representation witnesses");
  }
  const auto nestedHierarchy = planLlvmAdmission(lower(
      R"(class Poly {public: virtual int read(){return 1;} public: Poly() = default; }; class Outer {public: Poly value; public: Outer() = default; }; void accept(Outer& value){})"));
  expect(nestedHierarchy.admitted() &&
             std::any_of(nestedHierarchy.requirements().begin(),
                         nestedHierarchy.requirements().end(),
                         [](const auto &use) {
                           return use.required && use.implementedShape &&
                                  use.entry->key() == "Shape.ClassFields" &&
                                  use.implementedShape->name ==
                                      "NativeStaticDispatchStorage";
                         }),
         "ordinary hierarchy field metadata and declaration-only placeholders "
         "do not imply object transfer");
  struct NestedNegative {
    std::string_view source;
    std::string_view requirement;
    bool operationBody = false;
  };
  for (
      const auto control :
      {NestedNegative{
           R"(enum class Choice { empty, held(int value) }; class Outer {public: Choice value=Choice::empty; public: Outer() = default; }; void accept(Outer& value){})",
           "Shape.PayloadEnum"},
       NestedNegative{
           R"(union Payload { int value; bool flag; }; void accept(Payload& value){})",
           "ClassKind.Union"}}) {
    const auto excluded = planLlvmAdmission(lower(control.source));
    const auto owningFailure = std::find_if(
        excluded.requirements().begin(), excluded.requirements().end(),
        [&](const auto &use) {
          return use.required && use.entry->key() == control.requirement &&
                 !use.implementedShape &&
                 use.source.source.ends_with("llvm-admission.gti") &&
                 use.source.end > use.source.start &&
                 (!control.operationBody ||
                  (use.body && use.block && use.instruction));
        });
    expect(!excluded.admitted() &&
               owningFailure != excluded.requirements().end(),
           "nested payload and union storage retains its exact "
           "owning pending requirement and source identity");
  }
  const auto nestedForeign = planLlvmAdmission(lower(R"(
[[c_abi]] struct Record { int value; };
class Outer { public: Record value; Outer(Record input) : value(input) {} };
void accept(Outer& value) {}
)"));
  expect(!nestedForeign.admitted() &&
             std::any_of(nestedForeign.requirements().begin(),
                         nestedForeign.requirements().end(),
                         [](const auto &use) {
                           return use.required &&
                                  use.entry->key() == "ClassKind.Struct" &&
                                  !use.implementedShape;
                         }),
         "an ordinary containing class cannot bypass nested foreign-layout "
         "inventory");
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto owned =
      Frontend().analyze(root / "llvm-owned-field-control.gti", R"(
import std.memory;
class Outer {
  std::unique_ptr<int> owner = std::unique_ptr<int>(nullptr);
public:
  Outer() : owner(std::unique_ptr<int>(nullptr)) {}
};
void accept(Outer& value) {}
)",
                         {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  expect(owned.canGenerateCode(),
         "ownership-field exclusion uses valid source-defined policy");
  if (owned.canGenerateCode()) {
    const auto built = LoweredProgramBuilder().build(
        owned.program, owned.semantics, owned.hir, owned.mir, owned.mir, {},
        TargetInfo::host());
    std::vector<LoweredProgramIssue> issues;
    const auto verified = built.valid()
                              ? verifiedLoweredProgram(*built.program, issues)
                              : std::nullopt;
    expect(verified.has_value(),
           "ownership-field control retains its exact source seal");
    if (verified) {
      const auto ownerPlan = planLlvmAdmission(*verified);
      expect(!ownerPlan.admitted() &&
                 std::any_of(ownerPlan.requirements().begin(),
                             ownerPlan.requirements().end(),
                             [](const auto &use) {
                               return use.required &&
                                      use.entry->key() ==
                                          "SemanticType::Kind.UniqueOwner";
                             }),
             "recursive ordinary storage proof cannot admit a private owning "
             "field");
    }
  }
  ProgramRequest request{.kind = ProgramKind::Library};
  request.exports.push_back({.function = "inspect", .symbol = "gti_inspect"});
  const auto frontend =
      Frontend({.program = std::move(request)})
          .analyze("llvm-conservative-library.gti",
                   "int inspect(int value) { return value + 1; }");
  expect(frontend.canGenerateCode(),
         "library negative control has valid semantic export facts");
  if (frontend.canGenerateCode()) {
    const auto built = LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir, frontend.mir,
        frontend.mir, {}, TargetInfo::host());
    std::vector<LoweredProgramIssue> issues;
    const auto verified = built.valid()
                              ? verifiedLoweredProgram(*built.program, issues)
                              : std::nullopt;
    expect(verified.has_value(),
           "library negative retains its exact source seal");
    if (verified) {
      const auto library = planLlvmAdmission(*verified);
      expect(!library.admitted() && has(library, "ProgramKind", "Library") &&
                 has(library, "Shape", "EmbeddingExports"),
             "infallible embedding does not admit a checked-add export's "
             "defined-failure transport");
      try {
        static_cast<void>(LlvmBackend().generate(*built.program));
        expect(false, "pending library/export plans cannot return a public "
                      "object artifact");
      } catch (const BackendUnsupportedError &) {
      }
    }
  }
}

void testDispatchPerUseWitnesses() {
  using namespace lang;
  const auto input = lower(R"(
    class P {
    public:
      mut int value;
      P() : value(1) {}
      virtual int read() { return this.value; }
    };
    class Q : public P { public: Q() = default; };
    void take(P value) {}
    void assign(mut P& target, P& source) { target = source; }
    int main() {
      mut P first{};
      P second{};
      first.value = 5;
      P copied = P(first);
      take(P());
      first = second;
      mut Q derived{};
      assign(derived, second);
      return copied.read();
    }
  )");
  const auto plan = planLlvmAdmission(input);
  bool goodConstruct = false, goodFreshCopy = false, badTransfer = false,
       goodArgument = false, goodReplacement = false;
  for (const auto &use : plan.requirements()) {
    if (!use.required)
      continue;
    if (use.entry->key() == "MirInstructionKind.Construct") {
      goodConstruct |=
          use.implementedShape &&
          use.implementedShape->name == "NativeStaticDispatchStorage";
      goodFreshCopy |=
          use.implementedShape &&
          use.implementedShape->name == "NativeFreshHierarchyTransfer";
      goodArgument |=
          use.implementedShape &&
          use.implementedShape->name == "NativeFreshHierarchyPublication";
    }
    goodReplacement |=
        use.entry->key() == "MirInstructionKind.Assign" &&
        use.implementedShape &&
        use.implementedShape->name == "NativeHierarchyReplacement";
    badTransfer |= use.entry->key() == "Shape.PolymorphicTransfer" &&
                   !use.implementedShape && use.instruction != 0;
  }
  // Replacing the complete local keeps its own witness; replacing an object
  // through a base view could change an enclosing object's dynamic type.
  expect(!plan.admitted() && goodConstruct && goodFreshCopy && goodArgument &&
             goodReplacement && badTransfer,
         "fresh-local copy, a by-value argument published in its prepared "
         "storage, complete-object replacement and base-view assignment in "
         "one program keep distinct witnesses and explicit pending transfer "
         "requirements");
  const auto copyInput = lower(R"(
    class P {
    public:
      int value;
      P(int input) : value(input) {}
      virtual int read() { return this.value; }
    };
    int main() {
      P original{7};
      P explicit_copy{original};
      P implicit_copy = original;
      return original.read() + explicit_copy.read() + implicit_copy.read() - 21;
    }
  )");
  const auto copyPlan = planLlvmAdmission(copyInput);
  bool explicitCopy = false, implicitCopy = false;
  for (const auto &use : copyPlan.requirements())
    if (use.required && use.implementedShape &&
        use.implementedShape->name == "NativeFreshHierarchyTransfer") {
      explicitCopy |= use.entry->key() == "MirInstructionKind.Construct";
      implicitCopy |= use.entry->key() == "MirInstructionKind.Load";
    }
  expect(copyPlan.admitted() && explicitCopy && implicitCopy,
         "exact generated Copy Construct and implicit Copy Load into fresh "
         "whole locals have operation-specific admission: " +
             copyPlan.limitation());
  const auto moveInput =
      lower(R"(
    mut int destroyed = 0;
    class P {
      int value;
    public:
      P(int input) : value(input) {}
      virtual int read() { return this.value; }
      ~P() { destroyed += 1; }
    };
    int main() {
      {
        P original{7};
        P transferred = std::move(original);
        if (transferred.read() != 7) { return 1; }
      }
      return destroyed == 1 ? 0 : 2;
    }
  )",
            true,
            {.requireEntryPoint = true,
             .lowerReachableOnly = true,
             .lowerLibraryReachableOnly = true,
             .trustedBodies = TrustedBodyAnalysis::Reachable});
  const auto movePlan = planLlvmAdmission(moveInput);
  if (!movePlan.admitted()) {
    std::size_t reported = 0;
    for (const auto &use : movePlan.requirements()) {
      if (!use.required || use.implementedShape ||
          use.entry->key() != "SemanticType::Kind.Class" || reported >= 8)
        continue;
      ++reported;
      std::cerr << "fresh-move-unwitnessed-class: body=";
      if (use.body)
        std::cerr << static_cast<int>(use.body->kind) << '/' << use.body->owner;
      else
        std::cerr << "inventory";
      std::cerr << " block=" << use.block << " instruction=" << use.instruction
                << " occurrences=" << use.occurrences
                << " source=" << use.source.source << ':' << use.source.line
                << '@' << use.source.start << ".." << use.source.end << '\n';
      for (const auto &owner : moveInput.program().mir().classInstances()) {
        const auto *row = moveInput.program().findClassInstance(owner.id);
        if (!row || row->source != use.source)
          continue;
        const auto *declaration =
            moveInput.program().findClassDeclaration(owner.declaration);
        std::cerr << "fresh-move-class-row: id=" << owner.id
                  << " declaration=" << owner.declaration << " name="
                  << (declaration ? declaration->qualifiedName : "<missing>")
                  << " kind=" << static_cast<int>(owner.kind)
                  << " fields=" << owner.declaredFields.size()
                  << " bases=" << owner.bases.size()
                  << " structural-bases=" << owner.structuralBases.size()
                  << " borrowed=" << owner.traits.containsBorrowedState
                  << " active=" << owner.requiresActiveCleanup
                  << " own-flag=" << owner.requiresActiveDropState << '\n';
      }
    }
  }
  bool wholeMove = false;
  for (const auto &use : movePlan.requirements())
    wholeMove |= use.required &&
                 use.entry->key() == "MirInstructionKind.Move" &&
                 use.implementedShape &&
                 use.implementedShape->name == "NativeFreshHierarchyTransfer";
  expect(movePlan.admitted() && wholeMove,
         "an exact whole-binding Move into its unique fresh local has bounded "
         "admission without by-value or replacement support: " +
             movePlan.limitation());
  const auto mutations = lower(R"(
    class P {
    public:
      mut int value;
      P() : value(1) {}
      virtual int read() { return this.value; }
    };
    class Q : public P { public: Q() = default; };
    int poke(mut P& view, P& source) {
      mut P first{};
      first.value = 5;
      first = source;
      view = source;
      return first.read() + view.read();
    }
    int main() { mut Q derived{}; P second{}; return poke(derived, second); }
  )");
  const auto mutationPlan = planLlvmAdmission(mutations);
  bool scalarMutation = false, objectMutation = false, objectCopy = false,
       objectReplacement = false;
  for (const auto address : mutationPlan.requiredBodies()) {
    const auto *body = findMirBody(mutations.program().mir(), address);
    std::size_t transfers = 0;
    for (const auto &use : mutationPlan.requirements())
      if (use.required && use.body == address &&
          use.entry->key() == "Shape.PolymorphicTransfer")
        transfers += use.occurrences;
    for (const auto &block : body->blocks)
      for (const auto &stage : block.instructions) {
        const bool rejected = std::any_of(
            mutationPlan.requirements().begin(),
            mutationPlan.requirements().end(), [&](const auto &use) {
              return use.required && use.body == address &&
                     use.block == block.id && use.instruction == stage.id &&
                     use.entry->key() == "Shape.PolymorphicTransfer";
            });
        if (stage.kind == MirInstructionKind::Load &&
            stage.info.type.kind == SemanticType::Class)
          objectCopy |= rejected;
        if (stage.kind != MirInstructionKind::Assign || !stage.destination)
          continue;
        const auto *place = body->findPlace(*stage.destination);
        if (place->type == SemanticType::Int32)
          scalarMutation |= !rejected;
        if (place->type.kind != SemanticType::Class)
          continue;
        const bool replaced = std::any_of(
            mutationPlan.requirements().begin(),
            mutationPlan.requirements().end(), [&](const auto &use) {
              return use.required && use.body == address &&
                     use.block == block.id && use.instruction == stage.id &&
                     use.implementedShape &&
                     use.implementedShape->name == "NativeHierarchyReplacement";
            });
        objectReplacement |= replaced && !rejected;
        objectMutation |= !replaced && transfers == 2;
      }
  }
  expect(scalarMutation && objectCopy && objectMutation && objectReplacement,
         "scalar writes and a complete local's replacement remain distinct "
         "from the base view's coalesced source copy and assignment "
         "exclusions in the same body");
  // Its initialization step constructs a namespace object in place; the
  // receiver borrows that complete object through its symbol.
  const auto global = planLlvmAdmission(lower(R"(
    class P { public: P() {} virtual int read() { return 1; } };
    P value{};
    int main() { return value.read(); }
  )"));
  expect(global.admitted() &&
             std::any_of(
                 global.requirements().begin(), global.requirements().end(),
                 [](const auto &use) {
                   return use.required &&
                          use.entry->key() == "Shape.PolymorphicTransfer" &&
                          use.implementedShape &&
                          use.implementedShape->name ==
                              "NativeProgramHierarchyStorage";
                 }),
         "a namespace hierarchy object constructed in place by its "
         "initialization step carries its program-storage witness: " +
             global.limitation());
  // Each thread's dynamic initializer constructs a thread-local object in
  // that thread's slot; its thread-exit cleanup runs each destructor under
  // its own phase. A copy of program storage is not constructed in place.
  const auto threadLocal = [&](const char *declarations) {
    return planLlvmAdmission(lower(std::string(R"(
    class P { public: P() {} virtual int read() { return 1; } )") +
                                   declarations + R"(
    int main() { return value.read(); }
  )"));
  };
  for (const auto *declarations : {"};\n thread_local P value{};",
                                   "~P() {} };\n thread_local P value{};"}) {
    const auto plan = threadLocal(declarations);
    expect(plan.admitted() &&
               std::any_of(
                   plan.requirements().begin(), plan.requirements().end(),
                   [](const auto &use) {
                     return use.required &&
                            use.entry->key() == "Shape.PolymorphicTransfer" &&
                            use.implementedShape &&
                            use.implementedShape->name ==
                                "NativeThreadLocalHierarchyStorage";
                   }),
           "a thread-local hierarchy object constructed in its thread's slot "
           "carries its thread-storage witness: " +
               plan.limitation());
  }
  // A copy of program storage reads that complete object in place, into a
  // thread's slot as into a local; a copied array element stays refused.
  const auto copiedThreadLocal =
      threadLocal("};\n P global{};\n thread_local P value = global;");
  expect(copiedThreadLocal.admitted(),
         "a thread-local copy of program storage is a generated copy: " +
             copiedThreadLocal.limitation());
  const auto copiedElement = threadLocal(
      "};\n P values[2] = {P(), P()};\n P value = values[1];");
  expect(!copiedElement.admitted() &&
             std::any_of(copiedElement.requirements().begin(),
                         copiedElement.requirements().end(),
                         [](const auto &use) {
                           return use.required &&
                                  use.entry->key() ==
                                      "Shape.PolymorphicTransfer" &&
                                  !use.implementedShape;
                         }),
         "a copied array element keeps an explicit transfer exclusion");
}

} // namespace

int main() {
  try {
    testConcreteParameterPackPlan();
    testConcreteParameterAliases();
    testVerifiedConcretePackOwners();
    testClosureEnvironmentPlan();
    testHierarchyCapturePlan();
    testVerifiedRestorationRelay();
    testUnstagedConstructorFieldPublicationPlan();
    testStoredFieldPublicationPlan();
    testLedger();
    testNonExecutingEvidence();
    testSnapshotAndWholeProgram();
    testShapeWitnesses();
    testIntegralEnumReferences();
    testRawFieldWitnesses();
    testHierarchyStorageExclusion();
    testHierarchyStorageScopeReplaysExactOutcomes();
    testInlineHierarchyFamilyComposition();
    testHierarchyPlacementOwnership();
    testDispatchStateDependencies();
    testVirtualCallDependencies();
    testHierarchyReferenceReturns();
    testVirtualTextRelayReturns();
    testSelectedResultRelays();
    testConservativeClassWitnesses();
    testDispatchPerUseWitnesses();
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    ++failures;
  }
  if (failures)
    return 1;
  std::cout << "LLVM coverage and admission tests passed\n";
}
