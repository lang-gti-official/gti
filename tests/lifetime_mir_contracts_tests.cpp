#include "../src/compiler/mir_lifetimes.h"
#include "../src/compiler/mir_storage_effects.h"
#include "../src/compiler/mir_verification_queries.h"
#include "gti/frontend.h"
#include "gti/callable_lifetimes.h"
#include "gti/borrow_conditions.h"
#include "gti/mir_verifier.h"
#include "gti/mir_lowering.h"
#include "gti/mir_editor.h"
#include "gti/mir_printer.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>

using namespace lang;
namespace {
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
MirInstruction *firstLifetimeCall(MirBody &body) {
  for (auto &block : body.blocks)
    for (auto &instruction : block.instructions)
      if (instruction.kind == MirInstructionKind::Call &&
          instruction.functionTarget &&
          !instruction.lifetimeApplication.bindings.empty())
        return &instruction;
  return nullptr;
}
void checkScopedCallableFieldTransport() {
  const auto fields = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "closed-signature-field.gti", R"gti(
struct Badge<S> { int value; Badge(int input) : value(input) {} };
struct Envelope<S> {
  Badge<S> value;
  Envelope(int input) : value(Badge<S>(input)) {}
  Badge<S> get() { return this.value; }
};
using Bound = for<lifetime a> int&<a>(int&<a>);
int main() { Envelope<Bound> envelope{7}; auto value = envelope.get(); return value.value - 7; }
)gti");
  expect(fields.canGenerateCode(),
         "unrelated nominal fields preserve closed callable scopes through construction and selection");
  bool catalogChecked = false;
  bool constructorChecked = false;
  for (const auto &owner : fields.mir.classInstances()) {
    if (owner.type.arguments.size() != 1 ||
        owner.type.arguments.front().quantifiedLifetimes.size() != 1)
      continue;
    auto renamed = owner.type;
    auto &signature = renamed.arguments.front();
    const auto before = Lifetime::parameter(signature.quantifiedLifetimes.front());
    const auto after = Lifetime::parameter(900041);
    signature.quantifiedLifetimes.front() = after.id;
    for (auto &argument : signature.arguments)
      if (argument.referenceLifetime == before)
        argument.referenceLifetime = after;
    expect(mirTypeRelocationKind(fields.mir, renamed) == owner.traits.relocation &&
               owner.traits.relocation != RelocationKind::Unavailable,
           "relocation selects a unique alpha-equivalent closed nominal type");
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
      auto wrong = renamed;
      auto &changed = wrong.arguments.front();
      if (mutation == 0) changed.quantifiedLifetimes.clear();
      if (mutation == 1) changed.arguments.front().referenceLifetime = Lifetime::staticLifetime();
      if (mutation == 2) changed.arguments.back().referenceLifetime = Lifetime::parameter(900042);
      expect(mirTypeRelocationKind(fields.mir, wrong) == RelocationKind::Unavailable,
             "relocation does not match open, fixed or foreign callable regions");
    }
    catalogChecked = true;
  }
  for (const auto &constructor : fields.mir.constructorInstances())
    for (std::size_t index = 0; index < constructor.initializers.size(); ++index) {
      const auto &selected = constructor.initializers[index];
      if (selected.targetType.arguments.size() != 1 ||
          selected.targetType.arguments.front().quantifiedLifetimes.size() != 1)
        continue;
      for (unsigned mutation = 0; mutation < 3; ++mutation) {
        auto forged = fields.mir;
        auto &target = const_cast<MirConstructorInstance *>(
            forged.findConstructorInstance(constructor.id))->initializers[index].targetType;
        auto &signature = target.arguments.front();
        if (mutation == 0) signature.quantifiedLifetimes.clear();
        if (mutation == 1) signature.arguments.front().referenceLifetime = Lifetime::staticLifetime();
        if (mutation == 2) signature.arguments.back().referenceLifetime = Lifetime::parameter(900042);
        expect(!verifyMirProgram(forged).valid(),
               "selected fields cannot substitute a free, fixed or uncorrelated callable scope");
      }
      constructorChecked = true;
    }
  expect(catalogChecked && constructorChecked,
         "closed-field controls exercise both relocation lookup and selected constructor types");

  const auto library = std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto path = std::filesystem::weakly_canonical(library / "scoped-carrier-stage.gti");
  const std::string source = R"gti(
import std.detail_function_pointer_impl;
using Selection = for<lifetime a> int&<a>(int&<a>, int&<a>, bool);
int&<a> select<lifetime a>(int&<a> left, int&<a> right, bool first) {
  if (first) { return left; } return right;
}
class Dispatch<R&<region>(Args...)> {
  gti_internal::function_pointer<R&<region>(Args...)> target;
public:
  Dispatch<F>(F function)
      : target(gti_internal::function_pointer_from_item<R&<region>(Args...)>(function)) {}
  R&<region> choose(Args... args) {
    return gti_internal::function_pointer_invoke(this.target, args...);
  }
};
int main() { Dispatch<Selection> dispatch{select}; int left = 3; int right = 8;
  return dispatch.choose(left, right, false) - 8; }
)gti";
  const PackageSourceRoot modules{
      .identity = "toolchain:std", .name = "std", .packageRoot = library, .sourceRoot = library,
      .modules = {{"meta", library / "std/meta.gti"},
                  {"detail_function_pointer_impl", library / "std/detail/function_pointer.gti"}},
      .kind = SourceOwnerKind::Toolchain, .implicitAlias = "std", .defaultModule = "meta"};
  const auto pointer = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      path, source, {library / "prelude.gti", path}, {{path.string(), source}}, {library}, {modules});
  if (!pointer.canGenerateCode())
    for (const auto &diagnostic : pointer.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(pointer.canGenerateCode(),
         "an evaluated higher-ranked pointer field lends its carrier only to one checked invocation");
  bool stageChecked = false;
  for (const auto address : enumerateMirBodyAddresses(pointer.mir)) {
    const auto *body = findMirBody(pointer.mir, address);
    for (const auto &block : body->blocks)
      for (const auto &call : block.instructions) {
        if (call.intrinsic != IntrinsicKind::FunctionPointerInvoke || call.operands.empty() ||
            call.operands.front().kind != MirOperandKind::Value)
          continue;
        const auto *stage = mir_detail::definitionFor(*body, call.operands.front().value);
        expect(stage && stage->kind == MirInstructionKind::CallInput && stage->result &&
                   stage->callInputIndex == 0 && !stage->info.type.referenceLifetime.specified(),
               "the private operation shell stays separate from signature-bound inputs");
        for (unsigned mutation = 0; mutation < 12; ++mutation) {
          auto forged = pointer.mir;
          auto *changed = findMirBody(forged, address);
          auto *input = const_cast<MirInstruction *>(mir_detail::definitionFor(*changed, *stage->result));
          auto *invocation = [&]() -> MirInstruction * {
            for (auto &changedBlock : changed->blocks)
              for (auto &instruction : changedBlock.instructions)
                if (instruction.id == call.id) return &instruction;
            return nullptr;
          }();
          expect(invocation != nullptr, "carrier mutation retains its exact consumer");
          if (mutation == 0) input->callInputIndex = 1;
          if (mutation == 1) input->callInputRole = MirCallInputRole::Receiver;
          if (mutation == 2) ++input->callSite;
          if (mutation == 3) invocation->intrinsic = IntrinsicKind::None;
          if (mutation == 4) invocation->lifetimeApplication.callableSignature.reset();
          if (mutation == 5) { invocation->borrowOrigin = BorrowOriginKind::Argument; invocation->borrowArgument = 0; }
          if (mutation == 6) input->info.type.referenceAccess = AccessMode::Mutable;
          if (mutation >= 7 && mutation <= 9) {
            auto &signature = input->info.type.arguments.front().arguments.front();
            if (mutation == 7) signature.quantifiedLifetimes.clear();
            if (mutation == 8) signature.arguments.front().referenceLifetime = Lifetime::staticLifetime();
            if (mutation == 9) signature.arguments[1].referenceLifetime = Lifetime::parameter(900042);
            invocation->parameterTypes.front() = input->info.type;
            invocation->operands.front().type = input->info.type;
            changed->values[*input->result - 1].info = input->info;
          }
          if (mutation == 10) input->operands.front().place = 0;
          if (mutation == 11) {
            bool escaped = false;
            for (auto &exit : changed->blocks)
              if (exit.terminator.kind == MirTerminatorKind::Return) {
                exit.terminator.value = MirOperand{.kind = MirOperandKind::Value,
                    .value = *input->result, .type = input->info.type};
                escaped = true;
                break;
              }
            expect(escaped && rebuildMirValueUses(*changed),
                   "the escaped operation shell has an independently rebuilt second use");
            const auto lifetimes = verifyMirLifetimeApplications(forged, *changed, address.owner);
            expect(std::any_of(lifetimes.errors.begin(), lifetimes.errors.end(), [&](const auto &error) {
                     return error.instruction == input->id && error.message.find("original source lifetime") != std::string::npos;
                   }), "the carrier stage itself rejects an added non-invocation escape");
          }
          expect(!verifyMirProgram(forged).valid(),
                 "carrier stages reject forged scope, role, endpoint, consumer, access and physical source");
        }
        stageChecked = true;
      }
  }
  expect(stageChecked, "closed callable field controls include the actual carrier input stage");
}

void checkClosedProjectedDefaultContract() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "closed-projected-default.gti", R"gti(
class Projected<R&<result>(R&<result>)> { public: Projected() = default; };
using Selection = for<lifetime a> int&<a>(int&<a>);
int main() { Projected<Selection> value{}; return 0; }
)gti");
  if (!checked.canGenerateCode())
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(checked.canGenerateCode(), "closed projected default construction has no invocation application");
  bool examined = false;
  for (const auto &function : checked.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::Construct || instruction.constructorTarget ||
            instruction.info.type.arguments.size() != 1 ||
            instruction.info.type.arguments.front().quantifiedLifetimes.empty())
          continue;
        examined = true;
        expect(instruction.lifetimeApplication.bindings.empty() &&
                   instruction.lifetimeApplication.variables.empty() &&
                   instruction.lifetimeApplication.obligations.empty() &&
                   instruction.lifetimeApplication.typeObligations.empty(),
               "a nested invocation binder remains in its owner signature rather than becoming a constructor argument");
        for (unsigned mutation = 0; mutation < 7; ++mutation) {
          auto forged = checked.mir;
          auto *owner = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
          auto *construct = const_cast<MirInstruction *>(mir_detail::definitionFor(owner->body, *instruction.result));
          expect(construct != nullptr, "closed default mutation retains its exact producer");
          auto &application = construct->lifetimeApplication;
          const auto variable = Lifetime::inference(900001);
          if (mutation == 0) {
            application.bindings.push_back({Lifetime::parameter(900002), variable});
            application.variables.push_back(variable);
            owner->body.lifetimeBody.variables.push_back(variable);
          } else if (mutation == 1) {
            application.variables.push_back(variable);
            owner->body.lifetimeBody.variables.push_back(variable);
          } else if (mutation == 2) {
            application.obligations.push_back({Lifetime::staticLifetime(), Lifetime::staticLifetime()});
          } else if (mutation == 3) {
            application.typeObligations.push_back({SemanticType::Int32, Lifetime::staticLifetime()});
          } else {
            auto &signature = construct->info.type.arguments.front();
            if (mutation == 4)
              signature.quantifiedLifetimes.clear();
            if (mutation == 5) {
              signature.quantifiedLifetimes.clear();
              signature.arguments[0].referenceLifetime = Lifetime::staticLifetime();
              signature.arguments[1].referenceLifetime = Lifetime::staticLifetime();
            }
            if (mutation == 6) {
              signature.quantifiedLifetimes.push_back(900003);
              signature.arguments[1].referenceLifetime = Lifetime::parameter(900003);
            }
            owner->body.values[*construct->result - 1].info = construct->info;
          }
          expect(!verifyMirProgram(forged).valid(),
                 "targetless defaults reject invented application facts, opened/fixed scopes and broken binder correlations");
        }
      }
  expect(examined, "the closed projected default reaches an independently verified structural Construct");
}
void checkReferenceContentsOpeningShape() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "reference-contents-shape.gti", R"gti(
struct View<lifetime a, lifetime b> {
  int&<a> first; int&<b> second;
  View(int&<a> x, int&<b> y) : first(x), second(y) {}
};
struct Wrapper<lifetime a, lifetime b, lifetime shell> {
  View<a,b>&<shell> nested;
  Wrapper(View<a,b>&<shell> value) : nested(value) {}
};
int read<lifetime a, lifetime b>(View<a,b>& input) {
  return input.first + input.second;
}
int holder(Wrapper<static,static,static>& input) { input; return 0; }
View<a,b>&<shell> identity<lifetime a, lifetime b, lifetime shell>(
    View<a,b>&<shell> input) { return input; }
int relay<lifetime a, lifetime b, lifetime shell>(View<a,b>&<shell> input) {
  [[discard]] identity(input); return 0;
}
int main() {
  int x = 2; int y = 5; View view{x,y};
  [[discard]] identity(view);
  [[discard]] relay(view);
  return read(view) - 7;
}
)gti");
  if (!checked.canGenerateCode())
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(checked.canGenerateCode(), "opening-shape fixture starts from verified ordinary references");
  const auto functionId = [&](std::string_view name) {
    const auto found = std::find_if(checked.hir.functionInstances().begin(),
        checked.hir.functionInstances().end(), [&](const auto &function) {
          return function.source && function.source->name().lexeme == name;
        });
    expect(found != checked.hir.functionInstances().end(), "opening fixture retains its exact function");
    return found->id;
  };
  struct StagedOpening {
    MirProgram program;
    HirFunctionInstanceId function = 0;
    MirLoanId loan = 0;
    MirPlaceId address = 0;
    MirInstructionId anchor = 0;
  };
  const auto stage = [&](std::string_view name, bool field, bool returned) {
    StagedOpening result{checked.mir, functionId(name)};
    auto &function = *const_cast<MirFunctionInstance *>(result.program.findFunctionInstance(result.function));
    auto &body = function.body;
    SemanticType reference;
    MirLoanId read = 0;
    HirValueId source = 0;
    MirPlace address;
    for (const auto &place : body.places) address.id = std::max(address.id, place.id);
    ++address.id;
    if (returned) {
      for (const auto &block : body.blocks)
        for (const auto &call : block.instructions) {
          if (call.kind != MirInstructionKind::Call || !call.loan) continue;
          const auto signature = instantiateMirLifetimeTarget(result.program, call);
          const auto *loan = body.findLoan(*call.loan);
          if (!signature || signature->returnType.kind != SemanticType::Reference ||
              !loan || loan->kind != MirLoanKind::CallResult) continue;
          expect(!read, "returned opening has one exact reference-producing call");
          reference = signature->returnType;
          read = loan->id;
          source = call.hirValue;
          address.root = MirPlaceRootKind::Loan;
          address.loan = read;
          address.type = reference.arguments.front();
          address.access = reference.referenceAccess;
        }
    } else {
      expect(function.parameterBindings.size() == 1, "named opening has one exact formal binding");
      const auto root = std::find_if(body.places.begin(), body.places.end(), [&](const auto &place) {
        return place.root == MirPlaceRootKind::Binding &&
               place.binding == function.parameterBindings.front() && place.projections.empty();
      });
      expect(root != body.places.end() && root->type.kind == SemanticType::Reference,
             "named opening starts from its full physical reference");
      const auto id = address.id;
      address = *root;
      address.id = id;
      BorrowPath path;
      if (field) {
        const auto outer = mirClassLifetimeView(result.program, root->type.arguments.front());
        expect(outer && outer->instance->declaredFields.size() == 1,
               "field opening has one independently instantiated reference field");
        path.push_back(fieldProjection(outer->instance->declaredFields.front().symbol));
      }
      const auto view = mir_detail::walkBorrowPath(result.program, root->type, root->access,
          path, {}, mir_detail::MirBorrowPathRoot::PhysicalReference);
      expect(view && !view->referenceCrossings.empty(), "opening address has an exact typed reference crossing");
      reference = view->referenceCrossings.back().reference;
      address.projections = view->projections;
      address.type = view->type;
      address.access = view->referenceAccessLimit;
      if (address.key) address.key->projections.insert(address.key->projections.end(), path.begin(), path.end());
      for (const auto &loan : body.loans)
        if (loan.entry && loan.kind == MirLoanKind::Parameter && loan.source == root->id) {
          expect(!read, "named opening has one exact physical entry loan");
          read = loan.id;
        }
      const auto *hir = checked.hir.findFunctionInstance(result.function);
      const auto binding = std::find_if(hir->body.bindings.begin(), hir->body.bindings.end(),
          [&](const auto &candidate) { return candidate.id == function.parameterBindings.front(); });
      expect(binding != hir->body.bindings.end(), "opening formal keeps its HIR binding identity");
      for (const auto &value : hir->body.values)
        if (!source && value.kind == HirValueKind::Variable && value.symbol == binding->info.symbol)
          source = value.id;
    }
    expect(read && source && reference.kind == SemanticType::Reference,
           "opening retains its evaluated address and separate read ancestry");
    result.address = address.id;
    body.places.push_back(address);
    const auto inventory = mir_detail::referenceSupplierPaths(result.program, reference.arguments.front());
    expect(inventory.status == ReferenceSupplierQueryStatus::Complete && inventory.paths.size() == 2 &&
               std::all_of(inventory.paths.begin(), inventory.paths.end(), [](const auto &path) {
                 return path.crossings.empty() && path.conditions.empty();
               }), "opening has exactly two unconditional direct shared reference fields");
    MirLoan opening;
    for (const auto &loan : body.loans) opening.id = std::max(opening.id, loan.id);
    ++opening.id;
    opening.kind = MirLoanKind::Stored;
    opening.producedBy = source;
    opening.designatedFields = inventory.paths.front().fields;
    opening.referenceContents = MirReferenceContentsSource{opening.id, opening.designatedFields};
    opening.contentsOpening = MirReferenceContentsOpening{
        {.kind = MirOperandKind::BorrowRead, .place = address.id, .type = address.type}, reference, {read}};
    opening.additionalDependencies.push_back({
        .referenceContents = MirReferenceContentsSource{opening.id, inventory.paths.back().fields},
        .designatedFields = inventory.paths.back().fields});
    result.loan = opening.id;
    MirInstruction anchor;
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions) anchor.id = std::max(anchor.id, instruction.id);
    ++anchor.id;
    anchor.kind = MirInstructionKind::Borrow;
    anchor.hirValue = source;
    anchor.loan = opening.id;
    anchor.operands = {opening.contentsOpening->reference};
    anchor.info.type = address.type;
    anchor.info.category = ValueCategory::Place;
    result.anchor = anchor.id;
    body.loans.push_back(std::move(opening));
    // Deliberately stage structure without claiming dominance, active loans or
    // executable admission. The full verifier must still reject the marker.
    body.blocks.front().instructions.push_back(std::move(anchor));
    return result;
  };
  const auto shape = [](const StagedOpening &staged) {
    const auto &body = staged.program.findFunctionInstance(staged.function)->body;
    return mir_detail::referenceContentsOpeningShape(staged.program, body, *body.findLoan(staged.loan));
  };
  for (const auto &staged : {stage("read", false, false), stage("holder", true, false), stage("main", false, true)}) {
    const auto inventory = shape(staged);
    expect(inventory && inventory->fields.size() == 2,
           "named, field and exact returned addresses preserve the direct contents shape");
    expect(!verifyMirProgram(staged.program).valid(), "shape success never opens the staged contents admission gate");
    for (int mutation = 0; mutation != 16; ++mutation) {
      auto forged = staged;
      auto &body = const_cast<MirFunctionInstance *>(forged.program.findFunctionInstance(forged.function))->body;
      auto &loan = *const_cast<MirLoan *>(body.findLoan(forged.loan));
      auto &opening = *loan.contentsOpening;
      auto &anchor = *std::find_if(body.blocks.front().instructions.begin(), body.blocks.front().instructions.end(),
          [&](const auto &instruction) { return instruction.id == forged.anchor; });
      if (mutation == 0) loan.source = forged.address;
      if (mutation == 1) loan.referenceContents.reset();
      if (mutation == 2) loan.referenceContents->openingLoan = opening.readLoans.front();
      if (mutation == 3) loan.additionalDependencies.front().referenceContents->fields = loan.designatedFields;
      if (mutation == 4) loan.additionalDependencies.front() = {
          .referenceContents = loan.referenceContents, .designatedFields = loan.designatedFields};
      if (mutation == 5) loan.additionalDependencies.clear();
      if (mutation == 6) opening.referenceType.referenceLifetime = Lifetime::parameter(UINT64_MAX);
      if (mutation == 7) { opening.reference.type = SemanticType::Int32; anchor.operands.front() = opening.reference; }
      if (mutation == 8) opening.readLoans = {forged.loan + 99};
      if (mutation == 9) opening.readLoans.push_back(opening.readLoans.front());
      if (mutation == 10) opening.readLoans = {forged.loan};
      if (mutation == 11) {
        auto &read = *const_cast<MirLoan *>(body.findLoan(opening.readLoans.front()));
        read.parent = read.id;
      }
      if (mutation == 12) loan.additionalDependencies.front().access = AccessMode::Mutable;
      if (mutation == 13) anchor.tombstone = true;
      if (mutation == 14) body.blocks.front().instructions.push_back(anchor);
      if (mutation == 15) {
        auto &address = *const_cast<MirPlace *>(body.findPlace(forged.address));
        address.projections.push_back({.kind = MirProjectionKind::Field, .field = UINT64_MAX});
      }
      expect(!shape(forged), "opening shape rejects coordinated field/type/origin/read/anchor forgery");
    }
  }
  const auto returned = stage("main", false, true);
  for (int mutation = 0; mutation != 6; ++mutation) {
    auto forged = returned;
    auto &body = const_cast<MirFunctionInstance *>(forged.program.findFunctionInstance(forged.function))->body;
    const auto read = body.findLoan(forged.loan)->contentsOpening->readLoans.front();
    MirInstruction *producer = nullptr;
    for (auto &block : body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.kind == MirInstructionKind::Call && instruction.loan == read) producer = &instruction;
    expect(producer && producer->result, "returned opening retains an exact call producer");
    auto &value = *const_cast<MirValue *>(body.findValue(*producer->result));
    if (mutation == 0) value.definitionBlock = 0;
    if (mutation == 1) value.definition = forged.anchor;
    if (mutation == 2) body.values.push_back(value);
    if (mutation == 3) body.blocks.front().instructions.push_back(*producer);
    if (mutation == 4) producer->lifetimeApplication.bindings.clear();
    if (mutation == 5) producer->hirValue = 0;
    expect(!shape(forged), "returned opening cannot fabricate its exact applied call/result identity");
  }
  const auto checkFormation = [&](StagedOpening staged, bool nested, bool returned,
                                  bool fromFormal = false) {
    auto &body = const_cast<MirFunctionInstance *>(
        staged.program.findFunctionInstance(staged.function))->body;
    auto &opening = *body.loans[staged.loan - 1].contentsOpening;
    const auto read = opening.readLoans.front();
    auto &entry = body.blocks.front().instructions;
    const auto old = std::find_if(entry.begin(), entry.end(),
        [&](const auto &instruction) { return instruction.id == staged.anchor; });
    expect(old != entry.end(), "formation retains its exact staged anchor");
    const auto anchor = *old;
    entry.erase(old);
    bool placed = false;
    for (auto &block : body.blocks) {
      if (!returned && placed) break;
      auto position = returned
          ? std::find_if(block.instructions.begin(), block.instructions.end(),
                [&](const auto &instruction) {
                  return instruction.kind == MirInstructionKind::Call && instruction.loan == read;
                })
          : block.instructions.begin();
      if (returned && position == block.instructions.end()) continue;
      if (returned) ++position;
      const auto index = std::distance(block.instructions.begin(), position);
      block.instructions.insert(block.instructions.begin() + index, anchor);
      block.instructions.insert(block.instructions.begin() + index + 1,
          {.id = anchor.id + 1, .kind = MirInstructionKind::EndBorrow, .loan = staged.loan});
      placed = true;
      break;
    }
    expect(placed, "formation executes while the evaluated read is live");
    const auto flow = [&] {
      return verifyMirPersistentStorageLoanFlow(
          body, staged.function, {}, nullptr, nullptr, {}, &staged.program);
    };
    if (nested) {
      expect(!flow().valid(), "a nested reference read also requires external-field authority");
      const auto external = body.loans[read - 1].storedDependency;
      expect(external != 0, "nested fixture has independent external-field authority");
      opening.readLoans.push_back(external);
    }
    expect(flow().valid(), "named, nested and returned formation prove every actual read capability");
    expect(!verifyMirProgram(staged.program).valid(),
           "formation-only proof does not bypass the closed contents publication gate");
    const auto complete = opening.readLoans;
    for (std::size_t index = 0; index < complete.size(); ++index) {
      opening.readLoans = complete;
      opening.readLoans.erase(opening.readLoans.begin() + index);
      expect(!flow().valid(), "omitting any required formation capability is rejected");
    }
    opening.readLoans = complete;
    if (returned) {
      const auto &result = body.loans[read - 1];
      if (fromFormal) {
        expect(result.parent && result.parent != result.storedDependency &&
                   result.additionalDependencies.size() == 2,
               "a returned formal reference preserves physical and external parent roles");
        opening.readLoans = {result.parent, result.storedDependency};
        expect(flow().valid(),
               "a frontier may decompose when every origin has independently authorized parents");
        const auto parents = opening.readLoans;
        for (std::size_t index = 0; index < parents.size(); ++index) {
          opening.readLoans = parents;
          opening.readLoans.erase(opening.readLoans.begin() + index);
          expect(!flow().valid(), "returned formal decomposition retains every physical origin");
        }
        return;
      }
      expect(result.source && !result.parent && result.storedDependency &&
                 !result.additionalDependencies.empty(),
             "returned owned-shell reference has separate external lifetime witnesses");
      opening.readLoans = {result.storedDependency};
      const auto rejected = flow();
      expect(!rejected.valid() && std::any_of(rejected.errors.begin(), rejected.errors.end(),
          [](const auto &issue) {
            return issue.message ==
                "reference contents read omits part of its physical capability frontier";
          }), "external dependency parents cannot replace an unparented physical-shell capability");
    }
  };
  checkFormation(stage("read", false, false), false, false);
  checkFormation(stage("holder", true, false), true, false);
  checkFormation(stage("main", false, true), false, true);
  checkFormation(stage("relay", false, true), false, true, true);

}

void checkReferenceReceiverProducer() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "reference-receiver-producer.gti", R"gti(
struct View<lifetime storage> {
  int&<storage> value;
  View(int&<storage> input) : value(input) {}
  int read() { return this.value; }
};
T&<a> forward_reference<T, lifetime a>(T&<a> input) requires outlives<T,a> { return input; }
int relay<lifetime storage, lifetime shell>(View<storage>&<shell> left,
                                           View<storage>&<shell> right) {
  return forward_reference(left).read() + right.read();
}
int main() { int x=5; int y=6; View left{x}; View right{y}; return relay(left,right)-11; }
)gti");
  if (!checked.canGenerateCode())
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(checked.canGenerateCode(), "ordinary reference-result receivers retain their exact Call producer");
  bool examined = false;
  for (const auto &function : checked.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &call : block.instructions) {
        if (call.kind != MirInstructionKind::Call || !call.loan || !call.result ||
            call.info.category != ValueCategory::Place || call.parameterTypes.size() != 1 ||
            call.parameterTypes.front().kind != SemanticType::Reference ||
            call.info.type.kind != SemanticType::Class) continue;
        const auto *published = function.body.findLoan(*call.loan);
        const auto *parent = published ? function.body.findLoan(published->parent) : nullptr;
        const auto *root = parent ? function.body.findPlace(parent->source) : nullptr;
        if (!parent || !parent->entry || parent->kind != MirLoanKind::Parameter || !root) continue;
        const MirLoan *other = nullptr;
        const MirPlace *otherAddress = nullptr;
        for (const auto &loan : function.body.loans) {
          const auto *source = function.body.findPlace(loan.source);
          if (!loan.entry || loan.kind != MirLoanKind::Parameter || loan.id == parent->id ||
              !source || source->type != root->type || source->binding == root->binding) continue;
          for (const auto &place : function.body.places)
            if (place.id != source->id &&
                mir_detail::wholeEntryPlaceMatches(function.body, *source, place)) {
              other = &loan;
              otherAddress = &place;
            }
        }
        if (!other || !otherAddress) continue;
        const auto *stage = call.operands.size() == 1 && call.operands.front().kind == MirOperandKind::Value
            ? mir_detail::definitionFor(function.body, call.operands.front().value) : nullptr;
        const auto *value = function.body.findValue(*call.result);
        expect(stage && stage->kind == MirInstructionKind::CallInput && stage->operands.size() == 1 &&
                   stage->operands.front().kind == MirOperandKind::Loan &&
                   stage->operands.front().loan == parent->id &&
                   stage->operands.front().place != root->id && published->source == root->id &&
                   published->storedDependency == parent->storedDependency &&
                   published->storedDependency && published->additionalDependencies.size() == 1 &&
                   published->additionalDependencies.front().parent == published->storedDependency &&
                   value && value->definitionBlock == block.id && value->definition == call.id,
               "entry root and evaluated whole referent retain one call and both independent suppliers");
        examined = true;
        for (int mutation = 0; mutation != 7; ++mutation) {
          auto forged = checked.mir;
          auto &body = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id))->body;
          auto &changed = *const_cast<MirLoan *>(body.findLoan(published->id));
          auto &input = const_cast<MirInstruction *>(mir_detail::definitionFor(body, call.operands.front().value))->operands.front();
          auto &facts = *const_cast<MirValue *>(body.findValue(*call.result))->borrowDependencies;
          const auto replacePrimary = [&](MirPlaceId source, MirLoanId nextParent) {
            for (auto &fact : facts.dependencies)
              if (fact.source == published->source && fact.parent == published->parent) {
                fact.source = source;
                fact.parent = nextParent;
              }
            changed.source = source;
            changed.parent = nextParent;
          };
          if (mutation == 0) input.loan = other->id;
          if (mutation == 1) input.place = otherAddress->id;
          if (mutation == 2) replacePrimary(published->source, other->id);
          if (mutation == 3) replacePrimary(other->source, published->parent);
          if (mutation == 4)
            const_cast<MirPlace *>(body.findPlace(input.place))->projections.push_back(
                {.kind = MirProjectionKind::Field, .field = UINT64_MAX});
          if (mutation == 5) const_cast<MirLoan *>(body.findLoan(parent->id))->access = AccessMode::Mutable;
          if (mutation == 6) {
            auto &instructions = const_cast<MirBlock *>(body.findBlock(block.id))->instructions;
            instructions.erase(std::find_if(instructions.begin(), instructions.end(),
                [&](const auto &instruction) { return instruction.id == call.id; }));
          }
          expect(!verifyMirProgram(forged).valid(),
                 "reference receiver cannot substitute another formal, ancestry, projection or producer");
        }
      }
  expect(examined, "receiver producer control compares two independent, exactly same-typed formals");
  const auto getter = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "reference-receiver-contents-staged.gti", R"gti(
struct View<lifetime storage> {
  int&<storage> value;
  View(int&<storage> input) : value(input) {}
  int&<storage> get() { return this.value; }
};
T&<a> forward_reference<T, lifetime a>(T&<a> input) requires outlives<T,a> { return input; }
int main() { int x=7; View view{x}; return forward_reference(view).get()-7; }
)gti");
  expect(getter.semanticValid && getter.hirValid && !getter.canGenerateCode() &&
             std::any_of(getter.diagnostics.begin(), getter.diagnostics.end(), [](const auto &diagnostic) {
               return diagnostic.code == "GTI-B0001" &&
                   diagnostic.message.find("complete") != std::string::npos;
             }), "a scalar receiver read does not invent missing borrowed-result contents authority");
}

void checkScopedReceiverOwner() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "scoped-receiver-owner.gti", R"gti(
class Holder<Signature> {
public:
  Holder() = default;
  int read() { return 3; }
};
using Signature = for<lifetime a, lifetime b> int&<a>(int&<a>, int&<b>);
int main() { Holder<Signature> holder{}; return holder.read() - 3; }
)gti");
  if (!checked.canGenerateCode())
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(checked.canGenerateCode(), "nested closed invocation scopes retain exact ordinary receiver identity");
  bool renamed = false;
  for (const auto &function : checked.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (!instruction.receiver || !instruction.functionTarget) continue;
        const auto *target = checked.mir.findFunctionInstance(*instruction.functionTarget);
        const auto expected = target ? instantiateMirCallableOwnerType(
            checked.mir, *target, instruction.lifetimeApplication) : std::nullopt;
        if (!expected || instruction.receiver->type == *expected) continue;
        const auto &ownerType = instruction.receiver->type;
        expect(equivalentLifetimeTypes(ownerType, *expected) && ownerType.arguments.size() == 1 &&
                   ownerType.arguments.front().kind == SemanticType::FunctionSignature,
               "receiver scope adaptation changes only closed invocation binder names");
        renamed = true;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto forged = checked.mir;
          auto &body = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id))->body;
          auto &call = const_cast<MirBlock *>(body.findBlock(block.id))->instructions;
          auto found = std::find_if(call.begin(), call.end(), [&](const auto &value) {
            return value.id == instruction.id;
          });
          auto &signature = found->receiver->type.arguments.front();
          if (mutation == 0) signature.quantifiedLifetimes.clear();
          if (mutation == 1) signature.arguments.front().referenceLifetime = Lifetime::staticLifetime();
          if (mutation == 2)
            signature.arguments.back().referenceLifetime = signature.arguments[1].referenceLifetime;
          expect(!verifyMirProgram(forged).valid(),
                 "receiver alpha equivalence preserves binder inventory, fixed regions and correlations");
        }
      }
  expect(renamed, "scoped receiver regression exercises the canonical-to-actual binder boundary");
}
void checkAppliedBorrowFieldSummary() {
  const std::vector<std::pair<const char *, const char *>> fixtures = {
      {"compact-global", R"gti(
int global = 7;
int&<static> map(int ignored) { return global; }
int observe<F>(F& callback) { return callback(0); }
int main() { return observe(map) - 7; }
)gti"},
      {"instantiated-payload", R"gti(
enum Link<lifetime a> { value(int&<a> item) };
int&<a> select<lifetime a>(Link<a>& link) {
  switch (link) { case Link<a>::value(auto& payload): return payload; }
}
int main() { int value = 7; auto link = Link<_>::value(value); return select(link) - 7; }
)gti"}};
  for (const auto &[name, source] : fixtures) {
    const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
        std::string(name) + ".gti", source);
    if (!checked.canGenerateCode())
      for (const auto &diagnostic : checked.diagnostics)
        std::cerr << name << ": " << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(checked.canGenerateCode(), "compact global and applied conditional suppliers preserve exact target contracts");
    bool examined = false;
    for (const auto &function : checked.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (const auto &call : block.instructions) {
          const auto *target = call.functionTarget
              ? checked.mir.findFunctionInstance(*call.functionTarget) : nullptr;
          if (call.kind != MirInstructionKind::Call || !target) continue;
          const bool global = call.borrowOrigin == BorrowOriginKind::Global && call.borrowPlace;
          const bool conditional = !call.borrowFields.empty() &&
              !call.borrowFields.front().sourceConditions.empty();
          if (!global && !conditional) continue;
          if (global)
            expect(call.borrowFields.empty() && call.borrowPlace == target->returnBorrowPlace,
                   "a sole global dependency keeps the target's exact compact storage identity");
          if (conditional) {
            const auto applied = instantiateBorrowSummaryLifetimes(
                {.dependencies = target->returnBorrowFields}, call.lifetimeApplication.bindings);
            expect(applied && applied->dependencies == call.borrowFields &&
                       call.borrowFields != target->returnBorrowFields,
                   "conditional call suppliers use the independently instantiated guard scope");
          }
          examined = true;
          for (int mutation = 0; mutation != (conditional ? 3 : 1); ++mutation) {
            auto forged = checked.mir;
            auto &body = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id))->body;
            auto &instructions = const_cast<MirBlock *>(body.findBlock(block.id))->instructions;
            auto changed = std::find_if(instructions.begin(), instructions.end(),
                [&](const auto &instruction) { return instruction.id == call.id; });
            if (global) ++changed->borrowPlace->root;
            else if (mutation == 0)
              changed->borrowFields.front().sourceConditions.front().enumeration.lifetimeArguments.front() =
                  Lifetime::staticLifetime();
            else if (mutation == 1) changed->borrowFields = target->returnBorrowFields;
            else changed->lifetimeApplication.bindings.clear();
            expect(!verifyMirProgram(forged).valid(),
                   "summary replay rejects another global, strengthened/stale guard scope and missing application");
          }
        }
    expect(examined, "supplier regression reaches the intended concrete call boundary");
  }
}
bool checkConsumingExternalBorrowResults() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "consuming-external-results.gti", R"gti(
int shared = 7;
struct Parcel<lifetime a> {
  int&<a> value;
  Parcel(int&<a> input) : value(input) {}
};
enum Optional<lifetime a> { value(int&<a> input), empty };
struct External<lifetime a> {
  int&<a> value;
  int owned;
  External(int&<a> input) : value(input), owned(1) {}
  int&<a> release() && { return this.value; }
  int& immediate() && { return this.value; }
  Parcel<a> relay() && { return Parcel<a>(this.value); }
};
struct Independent {
  Independent() {}
  int&<a> argument<lifetime a>(int&<a> input) && { return input; }
  int&<static> global() && { return shared; }
  Optional<static> empty() && { return Optional<static>::empty; }
};
struct Exclusive<lifetime a> {
  mut int&<a> value;
  Exclusive(mut int&<a> input) : value(input) {}
  mut int&<a> release() && { return this.value; }
};
External<a> makeExternal<lifetime a>(int&<a> input) { return External<a>(input); }
int&<a> release<lifetime a>(int&<a> input) { return makeExternal(input).release(); }
Parcel<a> relay<lifetime a>(int&<a> input) { return External<a>(input).relay(); }
int main() {
  int value = 3;
  mut int exclusive = 5;
  Parcel parcel = relay(value);
  Optional<static> empty = Independent().empty();
  mut int& selected = Exclusive(exclusive).release();
  selected = 9;
  return release(value) + parcel.value + Independent().argument(value) +
         Independent().global() + External(value).immediate() + exclusive - 28;
}
)gti");
  if (!checked.canGenerateCode()) {
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << " at "
                << diagnostic.primary.start << '-' << diagnostic.primary.end << '\n';
    for (const auto &error : verifyMirProgram(checked.mir).errors)
      std::cerr << error.message << '\n';
    std::cerr << "consuming methods retain only independently checked external result suppliers\n";
    return false;
  }
  bool receiver = false, argument = false, global = false, empty = false, mutableResult = false;
  for (const auto &function : checked.mir.functionInstances()) {
    if (function.receiverMutability != ReceiverMutability::Consuming ||
        !function.completeReturnBorrow)
      continue;
    const auto &summary = *function.completeReturnBorrow;
    empty |= function.returnType.kind == SemanticType::Enum && summary.dependencies.empty();
    for (const auto &dependency : summary.dependencies) {
      argument |= dependency.origin == BorrowOriginKind::Argument;
      global |= dependency.origin == BorrowOriginKind::Global;
    }
    if (function.returnBorrowOrigin == BorrowOriginKind::Global) {
      expect(function.owner && function.returnBorrowPlace &&
                 summary.dependencies.size() == 1 &&
                 summary.dependencies.front().origin == BorrowOriginKind::Global,
             "a consuming global supplier remains an exact independent static place");
      for (int mutation = 0; mutation != 2; ++mutation) {
        auto forged = checked.mir;
        auto *target = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
        if (mutation == 0) {
          target->returnBorrowPlace.reset();
        } else {
          ++target->returnBorrowPlace->root;
          target->completeReturnBorrow->dependencies.front().globalRoot = target->returnBorrowPlace->root;
          for (auto &mapping : target->returnBorrowFields)
            if (mapping.origin == BorrowOriginKind::Global)
              mapping.globalRoot = target->returnBorrowPlace->root;
        }
        expect(!verifyMirProgram(forged).valid(),
               "member global returns still require the exact executable static root");
      }
    }
    if (function.returnType.kind != SemanticType::Reference || summary.dependencies.size() != 1 ||
        summary.dependencies.front().origin != BorrowOriginKind::Receiver)
      continue;
    expect(!summary.dependencies.front().sourceFields.empty(),
           "a consuming receiver's scalar contract never adds a whole-shell supplier");
    mutableResult |= function.returnType.referenceAccess == AccessMode::Mutable;
    if (function.returnType.referenceAccess == AccessMode::Mutable)
      continue;
    receiver = true;
    const auto *owner = checked.mir.findClassInstance(*function.owner);
    const auto owned = std::find_if(owner->declaredFields.begin(), owner->declaredFields.end(),
        [](const auto &field) { return field.type.kind == SemanticType::Int32; });
    expect(owned != owner->declaredFields.end(), "receiver fixture retains a distinct owned scalar field");
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto forged = checked.mir;
      auto *target = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
      if (mutation == 3) {
        target->completeReturnBorrow.reset();
      } else {
        auto invalid = target->completeReturnBorrow->dependencies.front();
        invalid.sourceFields = mutation == 0 ? BorrowPath{} : BorrowPath{fieldProjection(owned->symbol)};
        if (mutation == 2) {
          target->completeReturnBorrow->dependencies.push_back(invalid);
          target->returnBorrowFields.push_back(invalid);
        } else {
          target->completeReturnBorrow->dependencies = {invalid};
          target->returnBorrowFields = {invalid};
        }
      }
      expect(!verifyMirProgram(forged).valid(),
             "consuming result proof rejects shell, owned field, mixed suppliers and missing complete evidence");
    }
  }
  expect(receiver && argument && global && empty && mutableResult,
         "consuming result fixture covers stored readonly/mutable, argument, global and checked empty suppliers");
  bool carrierRelay = false;
  for (const auto &function : checked.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &call : block.instructions) {
        const auto *result = call.loan ? function.body.findLoan(*call.loan) : nullptr;
        const auto *stage = call.receiver && call.receiver->kind == MirOperandKind::Value
            ? mir_detail::definitionFor(function.body, call.receiver->value) : nullptr;
        const auto *shell = stage && stage->loan ? function.body.findLoan(*stage->loan) : nullptr;
        const auto *stored = shell ? function.body.findLoan(shell->storedDependency) : nullptr;
        if (call.kind != MirInstructionKind::Call || !call.result || !result || result->kind != MirLoanKind::Stored ||
            !result->parent || !stage || stage->callInputRole != MirCallInputRole::Receiver ||
            !shell || shell->kind != MirLoanKind::CallArgument || !stored ||
            stored->parent != result->parent || call.borrowFields.size() != 1)
          continue;
        carrierRelay = true;
        for (int mutation = 0; mutation != 7; ++mutation) {
          auto forged = checked.mir;
          auto *owner = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
          auto &body = owner->body;
          auto *changedCall = const_cast<MirInstruction *>(mir_detail::definitionFor(body, *call.result));
          auto *changedStage = const_cast<MirInstruction *>(mir_detail::definitionFor(body, call.receiver->value));
          expect(changedCall && changedStage, "carrier relay control keeps exact executed call and receiver input");
          if (mutation == 0) body.loans[shell->id - 1].storedDependency = 0;
          if (mutation == 1) body.loans[stored->id - 1].parent = 0;
          if (mutation == 2) body.loans[result->id - 1].designatedFields.clear();
          if (mutation == 3) body.loans[result->id - 1].source = shell->source;
          if (mutation == 4) changedStage->callInputRole = MirCallInputRole::Argument;
          if (mutation == 5) body.loans[result->id - 1].access = AccessMode::Mutable;
          if (mutation == 6) changedCall->borrowFields.front().sourceFields.clear();
          expect(!verifyMirProgram(forged).valid(),
                 "fresh consuming carrier results retain exact staged shell, external parent, field map and permission");
        }
      }
  expect(carrierRelay, "consuming fixture includes a fresh receiver's checked carrier result");
  return true;
}

bool checkCompleteReferenceInitializers() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "complete-reference-initializers.gti", R"gti(
int&<a> choose<lifetime a>(int&<a> left, int&<a> right, bool first) {
  if (first) { return left; } return right;
}
struct Selected<lifetime a> {
  int&<a> value;
  Selected(int&<a> left, int&<a> right, bool first) : value(choose(left, right, first)) {}
};
struct Direct<lifetime a> {
  int&<a> value;
  Direct(int&<a> input) : value(input) {}
};
struct Exclusive<lifetime a> {
  mut int&<a> value;
  Exclusive(mut int&<a> input) : value(input) {}
};
int main() {
  int left = 3; int right = 8; mut int third = 9;
  Selected selected{left, right, false}; Direct direct{right}; Exclusive exclusive{third};
  return selected.value + direct.value + exclusive.value - 25;
}
)gti");
  if (!checked.canGenerateCode()) {
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto &error : verifyMirProgram(checked.mir).errors)
      std::cerr << error.message << '\n';
    for (const auto &constructor : checked.mir.constructorInstances()) {
      if (std::none_of(constructor.parameterTypes.begin(), constructor.parameterTypes.end(),
          [](const auto &type) { return type.kind == SemanticType::Reference &&
                                      type.referenceAccess == AccessMode::Mutable; }))
        continue;
      std::cerr << "exclusive constructor=" << constructor.id << " parameters=";
      for (const auto binding : constructor.parameterBindings) std::cerr << binding << ',';
      std::cerr << '\n';
      for (const auto &initializer : constructor.initializers) {
        std::cerr << "  field=" << initializer.field << " arguments=";
        for (const auto value : initializer.arguments) std::cerr << value << ',';
        std::cerr << " complete=" << initializer.completeBorrow.has_value();
        if (initializer.completeBorrow)
          for (const auto &dependency : initializer.completeBorrow->dependencies)
            std::cerr << " [source=" << dependency.source << " parent=" << dependency.parent
                      << " access=" << static_cast<int>(dependency.access)
                      << " fields=" << dependency.designatedFields.size()
                      << " conditions=" << dependency.conditions.size() << ']';
        std::cerr << '\n';
      }
      const auto dump = MirPrinter{}.print(constructor.body);
      std::cerr << dump.substr(0, 10000);
      if (dump.size() > 10000) std::cerr << "[bounded constructor dump: " << dump.size() << " bytes]\n";
    }
    std::cerr << "reference-field constructors independently publish scalar, selected and exclusive inputs\n";
    return false;
  }
  bool selected = false;
  bool exclusive = false;
  for (const auto &constructor : checked.mir.constructorInstances()) {
    if (!constructor.completeBorrow || constructor.initializers.size() != 1 ||
        !constructor.initializers.front().storesReference)
      continue;
    const auto publication = std::find_if(constructor.body.loans.begin(), constructor.body.loans.end(),
        [&](const auto &loan) { return loan.escapes && loan.kind == MirLoanKind::Stored &&
            loan.storedPath == BorrowPath{fieldProjection(constructor.initializers.front().field)}; });
    expect(publication != constructor.body.loans.end(), "reference initializer has its exact publication loan");
    if (constructor.completeBorrow->dependencies.size() == 2) {
      selected = true;
      expect(publication->additionalDependencies.size() == 1,
             "a selected scalar publication preserves both permitted origins");
      for (int mutation = 0; mutation != 7; ++mutation) {
        auto forged = checked.mir;
        auto *owner = const_cast<MirConstructorInstance *>(forged.findConstructorInstance(constructor.id));
        auto *loan = const_cast<MirLoan *>(owner->body.findLoan(publication->id));
        if (mutation == 0) loan->additionalDependencies.clear();
        if (mutation == 1) loan->source = loan->additionalDependencies.front().source;
        if (mutation == 2) owner->initializers.front().completeBorrow->dependencies.pop_back();
        if (mutation == 3) owner->initializers.front().targetType.referenceLifetime = Lifetime::staticLifetime();
        if (mutation == 4)
          for (auto &block : owner->body.blocks)
            for (auto &instruction : block.instructions)
              if (instruction.kind == MirInstructionKind::Borrow && instruction.loan == loan->id)
                instruction.tombstone = true;
        if (mutation == 5) loan->access = AccessMode::Mutable;
        if (mutation == 6) owner->completeBorrow->dependencies.pop_back();
        expect(!verifyMirProgram(forged).valid(),
               "reference constructor replay rejects lost suppliers, forged origin/access/region and skipped publication");
      }
    }
    if (publication->entry) {
      exclusive = true;
      expect(publication->access == AccessMode::Mutable,
             "exclusive constructor reference transfers its unique incoming capability");
      expect(constructor.initializers.front().completeBorrow &&
                 constructor.initializers.front().completeBorrow->dependencies.size() == 1 &&
                 constructor.initializers.front().completeBorrow->dependencies.front().parent == publication->id,
             "the initializer snapshot preserves the exact incoming exclusive capability");
      for (int mutation = 0; mutation != 8; ++mutation) {
        auto forged = checked.mir;
        auto *owner = const_cast<MirConstructorInstance *>(forged.findConstructorInstance(constructor.id));
        auto *entry = const_cast<MirLoan *>(owner->body.findLoan(publication->id));
        auto &initializer = owner->initializers.front();
        auto &captured = initializer.completeBorrow->dependencies.front();
        if (mutation == 0) entry->entry = false;
        if (mutation == 1) captured.parent = 0;
        if (mutation == 2) captured.parent = entry->id + 1;
        if (mutation == 3) captured.source = 0;
        if (mutation == 4) captured.access = AccessMode::ReadOnly;
        if (mutation == 5) entry->parent = entry->id;
        if (mutation == 6) initializer.arguments.front() = 0;
        if (mutation == 7)
          for (auto &place : owner->body.places)
            if (place.sourceValue == initializer.arguments.front()) place.sourceValue = 0;
        expect(!verifyMirProgram(forged).valid(),
               "entry transfer requires its incoming parent, exact source/access, addressed initializer and parentless publication");
      }
    }
  }
  expect(selected && exclusive, "constructor fixture exercises both selected suppliers and entry transfer");
  return true;
}

void checkScopedFunctionItemIdentity() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "scoped-function-item-identity.gti", R"gti(
int global = 7;
int&<static> fixed(int ignored) { return global; }
int&<a> choose<lifetime a, lifetime b>(int&<a> first, int&<b> second) { return first; }
int observeFixed<F>(F& callback) { return callback(0); }
int observePair<F>(F& callback) { int first = 3; int second = 8; return callback(first, second); }
int main() { return observeFixed(fixed) + observePair(choose) - 10; }
)gti");
  if (!checked.canGenerateCode()) {
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "fresh function items retain exact fixed regions and declared invocation binders");
  }
  bool fixed = false;
  bool quantified = false;
  for (const auto &function : checked.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.operation != MirOperation::FunctionItem || !instruction.functionTarget || !instruction.result)
          continue;
        const auto *target = checked.mir.findFunctionInstance(*instruction.functionTarget);
        const auto expected = target ? functionItemLifetimeType(target->declaration, target->returnType,
            target->parameterTypes, target->lifetimeContract) : std::nullopt;
        expect(expected && equivalentLifetimeTypes(instruction.info.type, *expected),
               "each executable item independently matches its target's complete scoped type");
        if (instruction.info.type.arguments.front().kind != SemanticType::Reference) continue;
        const bool isFixed = instruction.info.type.arguments.front().referenceLifetime == Lifetime::staticLifetime();
        const bool isQuantified = instruction.info.type.quantifiedLifetimes.size() == 2;
        if (!isFixed && !isQuantified) continue;
        fixed |= isFixed;
        quantified |= isQuantified;
        for (int mutation = 0; mutation != (isQuantified ? 2 : 1); ++mutation) {
          auto forged = checked.mir;
          auto *changed = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
          auto *item = const_cast<MirInstruction *>(mir_detail::definitionFor(changed->body, *instruction.result));
          expect(item != nullptr, "function item mutation keeps its exact executable producer");
          if (isFixed) {
            item->info.type.quantifiedLifetimes.push_back(900001);
            item->info.type.arguments.front().referenceLifetime = Lifetime::parameter(900001);
          } else if (mutation == 0) {
            item->info.type.quantifiedLifetimes.pop_back();
          } else {
            item->info.type.arguments.back().referenceLifetime = item->info.type.arguments.front().referenceLifetime;
          }
          changed->body.values[*instruction.result - 1].info = item->info;
          expect(!equivalentLifetimeTypes(item->info.type, *expected) && !verifyMirProgram(forged).valid(),
                 "function items cannot turn fixed storage into a binder, erase a binder or change repeated-region relationships");
        }
      }
  expect(fixed && quantified, "function item fixture contrasts fixed storage and two independent invocation slots");
  bool temporary = false;
  bool renamed = false;
  for (const auto &function : checked.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &input : block.instructions) {
        if (input.kind != MirInstructionKind::CallInput ||
            input.callInputKind != HirCallInputKind::ReadTemporaryBorrow ||
            input.operands.size() != 1 || input.info.type.arguments.size() != 1)
          continue;
        const auto *place = function.body.findPlace(input.operands.front().place);
        if (!place || place->type.kind != SemanticType::FunctionItem) continue;
        const auto *loan = input.loan ? function.body.findLoan(*input.loan) : nullptr;
        expect(loan && loan->kind == MirLoanKind::CallArgument && loan->source == place->id &&
                   equivalentLifetimeTypes(place->type, input.info.type.arguments.front()),
               "a fresh scoped item publishes its exact materialized argument loan");
        temporary = true;
        auto missingLoan = checked.mir;
        auto *changed = const_cast<MirFunctionInstance *>(missingLoan.findFunctionInstance(function.id));
        for (auto &changedBlock : changed->body.blocks)
          for (auto &instruction : changedBlock.instructions)
            if (instruction.id == input.id) instruction.loan.reset();
        expect(!verifyMirProgram(missingLoan).valid(),
               "bound scope matching cannot omit a temporary item's argument loan");
        if (place->type != input.info.type.arguments.front()) {
          auto changedCleanup = checked.mir;
          auto *changedOwner = const_cast<MirFunctionInstance *>(changedCleanup.findFunctionInstance(function.id));
          for (auto &drop : changedOwner->body.dropObligations)
            if (drop.place == place->id) {
              expect(drop.dropType.type == place->type,
                     "an alpha-renamed parameter keeps the producer's exact cleanup type");
              drop.dropType.type = input.info.type.arguments.front();
              renamed = true;
            }
          expect(!verifyMirProgram(changedCleanup).valid(),
                 "parameter scope renaming cannot retag materialized item cleanup storage");
        }
      }
  expect(temporary && renamed, "item fixture exercises fresh materialization across a renamed bound scope");
  bool appliedDescriptor = false;
  for (const auto &function : checked.mir.functionInstances())
    for (std::size_t parameter = 0; parameter != function.callableParameters.size(); ++parameter)
      for (std::size_t index = 0; index != function.callableParameters[parameter].signatures.size(); ++index) {
        const auto &signature = function.callableParameters[parameter].signatures[index];
        if (signature.lifetimeApplication.bindings.empty() || signature.parameterTypes.empty() ||
            signature.parameterTypes.front().kind != SemanticType::Reference) continue;
        appliedDescriptor = true;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto broken = checked.mir;
          auto *changed = const_cast<MirFunctionInstance *>(broken.findFunctionInstance(function.id));
          auto &descriptor = changed->callableParameters[parameter].signatures[index];
          if (mutation == 0) descriptor.lifetimeApplication.complete = false;
          if (mutation == 1) descriptor.parameterTypes.front().referenceLifetime = Lifetime::staticLifetime();
          if (mutation == 2) descriptor.lifetimeApplication.bindings.push_back(
              {Lifetime::parameter(UINT64_MAX - 500), Lifetime::staticLifetime()});
          expect(!verifyMirProgram(broken).valid(),
                 "callable descriptors independently reject missing application, retagged input and invented region bindings");
        }
      }
  expect(appliedDescriptor, "item fixture publishes a concrete callable invocation lifetime application");
}

void checkStatelessItemTemporaryBorrow() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "stateless-item-temporary-borrow.gti", R"gti(
int identity(int input) { return input; }
int&<a> referenceIdentity<lifetime a>(int&<a> input) { return input; }
int global = 9;
int&<static> fixedReference(int ignored) { return global; }
F duplicate<F>(F& value) { return value; }
int main() {
  auto value = duplicate(identity); auto reference = duplicate(referenceIdentity);
  auto fixed = duplicate(fixedReference); int&<static> globalReference = fixed(0);
  int first = 3; int second = 7;
  return value(2) + reference(first) + reference(second) + globalReference - 21;
}
)gti");
  if (!checked.canGenerateCode())
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(checked.canGenerateCode(),
         "ordinary temporary borrowing copies exact stateless item values without a confined invocation contract");
  std::size_t copied = 0;
  for (const auto &function : checked.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &call : block.instructions) {
        if (call.kind != MirInstructionKind::Call || !call.functionTarget ||
            call.operands.size() != 1 || call.operands.front().kind != MirOperandKind::Value)
          continue;
        const auto *target = checked.mir.findFunctionInstance(*call.functionTarget);
        const auto *input = mir_detail::definitionFor(function.body, call.operands.front().value);
        if (!target || target->returnType.kind != SemanticType::FunctionItem ||
            !target->callableParameters.empty() || !input ||
            input->kind != MirInstructionKind::CallInput ||
            input->callInputKind != HirCallInputKind::ReadTemporaryBorrow ||
            input->operands.size() != 1 || !input->loan)
          continue;
        const auto *place = function.body.findPlace(input->operands.front().place);
        const auto materialized = place
            ? findMirTemporaryMaterialization(function.body, *place, input->hirValue)
            : MirTemporaryMaterialization{};
        expect(place && place->type.kind == SemanticType::FunctionItem && materialized &&
                   materialized.producer->operation == MirOperation::FunctionItem &&
                   call.lifetimeApplication.complete,
               "stateless copying retains the exact materialized item, temporary loan and applied target");
        ++copied;
        for (int mutation = 0; mutation != 8; ++mutation) {
          auto forged = checked.mir;
          auto *owner = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
          auto *callee = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(*call.functionTarget));
          const auto instruction = [&](MirInstructionId id) -> MirInstruction * {
            for (auto &changedBlock : owner->body.blocks)
              for (auto &candidate : changedBlock.instructions)
                if (candidate.id == id) return &candidate;
            return nullptr;
          };
          auto *changedInput = instruction(input->id);
          auto *changedCall = instruction(call.id);
          auto *producer = instruction(materialized.producer->id);
          expect(changedInput && changedCall && producer,
                 "stateless copy mutations retain their exact executable identities");
          if (mutation == 0) producer->functionTarget = call.functionTarget;
          if (mutation == 1) changedInput->loan.reset();
          if (mutation == 2) {
            changedInput->info.type.referenceAccess = AccessMode::Mutable;
            owner->body.values[*changedInput->result - 1].info = changedInput->info;
            changedCall->operands.front().type = changedInput->info.type;
          }
          if (mutation == 3) callee->definitionKind = MirDefinitionKind::Declaration;
          if (mutation == 4) callee->requiresUnsafe = true;
          if (mutation == 5) changedCall->lifetimeApplication.complete = false;
          if (mutation == 6) callee->returnType = SemanticType::referenceTo(callee->returnType);
          if (mutation == 7) callee->returnType = SemanticType::rawPointerTo(callee->returnType);
          expect(!verifyMirProgram(forged).valid(),
                 "stateless temporary copying cannot forge item identity, authority, access, target safety, application or escaping address results");
        }
      }
  expect(copied == 3, "stateless copy proof covers ordinary, quantified and fixed-region item types");
}

void checkFreshInputReferenceResult() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir,
                                 .lowerReachableOnly = true,
                                 .lowerLibraryReachableOnly = true,
                                 .trustedBodies = TrustedBodyAnalysis::Reachable}).analyze(
      "fresh-input-reference-result.gti", R"gti(
import std.meta;
int global = 7;
int&<static> mapped(int ignored) { return global; }
std::meta::callable_return_type<F> relay<F>(F& callback) { return callback(0); }
int&<a> select<lifetime a>(int& temporary, int&<a> source) { return source; }
int main() {
  int owner = 9;
  int&<static> first = relay(mapped);
  int& second = select(1, owner);
  return first + second - 16;
}
)gti", {root / "prelude.gti"}, {}, {root});
  if (!checked.canGenerateCode())
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(checked.canGenerateCode(),
         "an exact fresh input can return an independently owned checked reference");
  std::size_t proved = 0;
  for (const auto &function : checked.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &call : block.instructions) {
        if (call.kind != MirInstructionKind::Call || !call.functionTarget ||
            call.operands.empty() || call.operands.front().kind != MirOperandKind::Value)
          continue;
        const auto *target = checked.mir.findFunctionInstance(*call.functionTarget);
        const auto *input = mir_detail::definitionFor(function.body, call.operands.front().value);
        if (!target || target->returnType.kind != SemanticType::Reference ||
            !input || input->kind != MirInstructionKind::CallInput ||
            input->callInputKind != HirCallInputKind::ReadTemporaryBorrow)
          continue;
        expect(call.info.category == ValueCategory::Place &&
                   call.info.type == target->returnType.arguments.front() &&
                   call.info.access == target->returnType.referenceAccess &&
                   call.lifetimeApplication.complete &&
                   !call.lifetimeApplication.bindings.empty(),
               "the full applied reference result is represented by its exact place-valued referent");
        ++proved;
        for (int mutation = 0; mutation != 8; ++mutation) {
          auto forged = checked.mir;
          auto *caller = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
          auto *callee = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(*call.functionTarget));
          MirInstruction *changed = nullptr;
          MirInstruction *stage = nullptr;
          for (auto &changedBlock : caller->body.blocks)
            for (auto &instruction : changedBlock.instructions) {
              if (instruction.id == call.id) changed = &instruction;
              if (instruction.id == input->id) stage = &instruction;
            }
          expect(changed && stage, "fresh-reference mutations retain exact call and stage identities");
          if (mutation == 0) changed->info.category = ValueCategory::Value;
          if (mutation == 1) changed->info.access = AccessMode::Mutable;
          if (mutation == 2) changed->info.type = SemanticType::Bool;
          if (mutation < 3 && changed->result)
            caller->body.values[*changed->result - 1].info = changed->info;
          if (mutation == 3) stage->callInputIndex = 1;
          if (mutation == 4) changed->lifetimeApplication.complete = false;
          if (mutation == 5) changed->lifetimeApplication.bindings.front().second = Lifetime::staticLifetime();
          if (mutation == 6) callee->definitionKind = MirDefinitionKind::Declaration;
          if (mutation == 7) {
            callee->returnBorrowOrigin = BorrowOriginKind::Argument;
            callee->returnBorrowParameter = 0;
            callee->returnBorrowPlace.reset();
            if (callee->completeReturnBorrow)
              for (auto &supplier : callee->completeReturnBorrow->dependencies) {
                supplier.origin = BorrowOriginKind::Argument;
                supplier.parameter = 0;
                supplier.globalRoot = 0;
              }
            changed->borrowOrigin = BorrowOriginKind::Argument;
            changed->borrowArgument = 0;
            changed->borrowPlace.reset();
          }
          expect(!verifyMirProgram(forged).valid(),
                 "fresh inputs cannot forge reference result shape, applied regions, source target or temporary escape suppliers");
        }
      }
  expect(proved == 2,
         "fresh-reference proof covers static mapper results and a different caller-owned reference input");
}

void checkScalarLoadDependencyDomain() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "scalar-load-dependency-domain.gti", R"gti(
mut int cleanup_trace = 0;
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  int read() { return this.value; }
  int&<a> get() { return this.value; }
};
struct Cleanup<lifetime a> {
  int&<a> value;
  Cleanup(int&<a> input) : value(input) {}
  ~Cleanup() { cleanup_trace += this.value; }
};
void observe<lifetime a>(int&<a> input) { Cleanup<a> cleanup{input}; }
int main() {
  int input = 7;
  View view{input};
  View copy = view;
  observe(input);
  return copy.read() + copy.get() + cleanup_trace - 21;
}
)gti");
  if (!checked.canGenerateCode()) {
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "scalar reads of stored references preserve their address loans without storing them in scalar values");
  }
  bool scalar = false;
  bool carrier = false;
  for (const auto &function : checked.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &load : block.instructions) {
        if (load.kind != MirInstructionKind::Load || !load.result || load.operands.size() != 1 ||
            load.operands.front().kind != MirOperandKind::Copy)
          continue;
        const auto *value = function.body.findValue(*load.result);
        const auto *source = function.body.findPlace(load.operands.front().place);
        if (!value || !source) continue;
        if (isPassiveScalarValueType(load.info.type) &&
            std::any_of(source->projections.begin(), source->projections.end(), [](const auto &step) {
              return step.kind == MirProjectionKind::Dereference;
            })) {
          scalar = true;
          expect(!value->borrowDependencies,
                 "copied primitive contents do not retain the source reference's scalar address fact");
          auto forged = checked.mir;
          auto *changed = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
          changed->body.values[value->id - 1].borrowDependencies = MirBorrowDependencySet{
              .dependencies = {{.source = source->id, .access = source->access}}};
          expect(!verifyMirProgram(forged).valid(),
                 "a scalar Load cannot claim that it stores the source address capability");
        }
        const auto carrierView = load.info.type.kind == SemanticType::Class
            ? mirClassLifetimeView(checked.mir, load.info.type) : std::nullopt;
        if (carrierView && std::any_of(carrierView->instance->declaredFields.begin(),
                                      carrierView->instance->declaredFields.end(), [](const auto &field) {
              return field.type.kind == SemanticType::Reference;
            })) {
          carrier = true;
          auto forged = checked.mir;
          auto *changed = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
          changed->body.values[value->id - 1].borrowDependencies = MirBorrowDependencySet{};
          expect(!verifyMirProgram(forged).valid(),
                 "a carrier Load must retain its complete stored-reference contents");
        }
      }
  expect(scalar && carrier, "load-domain fixture contrasts primitive reads and copied borrowed carriers");
}

void checkForwardedCarrierPackLifetimeStage() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "forwarded-carrier-pack-stage.gti", R"gti(
struct View<lifetime a> { int&<a> value; View(int&<a> input) : value(input) {} };
int&<b> select<lifetime a, lifetime b, lifetime shell>(int&<a> ignored, View<b>&<shell> chosen) {
  return chosen.value;
}
class Relay<R(Args...)> {
public:
  Relay() {}
  R invoke(Args... values) { return select(values...); }
};
using Choice = for<lifetime a, lifetime b, lifetime shell> int&<b>(int&<a>, View<b>&<shell>);
int main() { int first = 3; int second = 8; View view{second}; Relay<Choice> relay{}; return relay.invoke(first, view) - 8; }
)gti");
  if (!checked.canGenerateCode()) {
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "forwarded referenced-carrier pack lanes reach independently checked input stages");
  }
  bool examined = false;
  for (const auto &function : checked.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &stage : block.instructions) {
        const auto *place = stage.operands.size() == 1
            ? function.body.findPlace(stage.operands.front().place) : nullptr;
        if (stage.kind != MirInstructionKind::CallInput || !stage.result || !place ||
            stage.info.type.kind != SemanticType::Reference || stage.info.type.arguments.size() != 1 ||
            stage.info.type.arguments.front().kind != SemanticType::Class ||
            place->projections.size() != 1 || place->projections.front().kind != MirProjectionKind::PackElement)
          continue;
        examined = true;
        expect(place->projections.front().constantIndex == 1,
               "referenced carrier uses its second grouped formal lane");
        for (int mutation = 0; mutation != 5; ++mutation) {
          auto forged = checked.mir;
          auto *owner = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
          auto *input = const_cast<MirInstruction *>(mir_detail::definitionFor(owner->body, *stage.result));
          expect(input != nullptr, "pack stage control retains the exact SSA producer");
          if (mutation == 0) {
            input->info.type.arguments.front().lifetimeArguments.front() = Lifetime::staticLifetime();
            input->operands.front().type = input->info.type;
            owner->body.values[*stage.result - 1].info = input->info;
            expect(!verifyMirLifetimeApplications(forged, owner->body, owner->id).valid(),
                   "matching forged stage tags cannot strengthen the original grouped carrier region");
          }
          if (mutation == 1)
            owner->body.places[place->id - 1].projections.front().constantIndex = 0;
          if (mutation == 2) {
            bool removed = false;
            for (auto &loan : owner->body.loans)
              if (loan.entry && loan.kind == MirLoanKind::Parameter && loan.storedDependency &&
                  loan.source == place->id) {
                loan.storedDependency = 0; removed = true;
              }
            expect(removed, "pack lane control removes a real shell-to-external-group link");
          }
          if (mutation == 3 || mutation == 4) {
            bool changed = false;
            for (auto &loan : owner->body.loans)
              if (loan.entry && loan.kind == MirLoanKind::Parameter && loan.storedDependency &&
                  loan.source == place->id) {
                if (mutation == 3) loan.source = place->id - 1;
                else loan.entry = false;
                changed = true;
              }
            expect(changed, "pack entry control changes the exact physical reference authority");
          }
          expect(!verifyMirProgram(forged).valid(),
                 "pack stages preserve exact lane, source region, live entry and stored external authority");
        }
      }
  expect(examined, "carrier pack fixture reaches an exact referenced-carrier input stage");
}

void checkPatternedResultScope() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "patterned-result-scope.gti", R"gti(
struct Environment<R&<environment>(Args...), lifetime environment> {
  R&<environment> value;
  Environment(R&<environment> input) : value(input) {}
};
int read<lifetime a>(int&<a> input) {
  Environment<int&<a>(int&), a> value{input};
  return value.value;
}
int main() { int value = 7; return read(value) - 7; }
)gti");
  if (!checked.canGenerateCode()) {
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "closed callable result scopes preserve fixed environments through exact constructor replay");
  }
  bool examined = false;
  for (const auto &function : checked.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::Construct ||
            instruction.info.type.arguments.size() != 1 ||
            instruction.info.type.arguments.front().kind != SemanticType::FunctionSignature)
          continue;
        const auto &signature = instruction.info.type.arguments.front();
        expect(signature.quantifiedLifetimes.size() == 1 && signature.arguments.size() == 2,
               "patterned result fixture has one invocation binder and an independent fixed result region");
        examined = true;
        for (int mutation = 0; mutation != 4; ++mutation) {
          auto forged = checked.mir;
          auto *owner = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
          auto *call = const_cast<MirInstruction *>(mir_detail::definitionFor(owner->body, *instruction.result));
          expect(call != nullptr, "patterned constructor control uses the exact SSA producer");
          auto &changed = call->info.type.arguments.front();
          if (mutation == 0) changed.arguments.front().referenceLifetime = Lifetime::staticLifetime();
          if (mutation == 1) changed.arguments[1].referenceLifetime = changed.arguments.front().referenceLifetime;
          if (mutation == 2) changed.quantifiedLifetimes.clear();
          if (mutation == 3) changed.quantifiedLifetimes.push_back(900001);
          expect(!verifyMirLifetimeApplications(forged, owner->body, owner->id).valid(),
                 "alpha comparison preserves fixed regions, invocation correlation and the full binder inventory");
        }
      }
  expect(examined, "patterned scope fixture reaches the independent constructor result gate");
}

void checkProgramBasePathAuthority() {
  const auto source = Frontend({.stopAfter = FrontendPhase::Mir}).analyze("base-path-authority.gti", R"gti(
interface Marker { int mark(); };
struct Base { int value; Base(int input) : value(input) {} };
struct Middle : public Base { Middle(int input) : Base(input) {} };
struct Derived : public Marker, public Middle {
  Derived(int input) : Middle(input) {}
  int mark() override { return this.value; }
};
int&<a> inherited<lifetime a>(Derived&<a> input) { return input.value; }
int main() { Derived value{7}; value.value = 11; return inherited(value) - 11; }
)gti");
  if (!source.semanticValid || !source.mir.valid()) {
    for (const auto &diagnostic : source.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto &error : verifyMirProgram(source.mir).errors)
      std::cerr << error.message << '\n';
    expect(false, "ordinary inherited physical storage must reach the exact program-aware MIR gate");
  }
  std::optional<MirBodyAddress> selected;
  MirPlaceId placeId = 0;
  for (const auto address : enumerateMirBodyAddresses(source.mir)) {
    const auto *body = findMirBody(source.mir, address);
    if (!body) continue;
    for (const auto &place : body->places)
      if (place.key && std::any_of(place.key->projections.begin(), place.key->projections.end(),
            [](const auto &step) { return step.kind == PlaceProjectionKind::Base; })) {
        selected = address; placeId = place.id; break;
      }
    if (selected) break;
  }
  expect(selected.has_value(), "fixture must preserve an exact inherited typed place");
  for (int mutation = 0; mutation != 4; ++mutation) {
    auto broken = source.mir;
    auto *body = findMirBody(broken, *selected);
    auto &place = body->places[placeId - 1];
    if (mutation == 0) {
      for (auto &step : place.key->projections)
        if (step.kind == PlaceProjectionKind::Base) { step.baseOwner += 1000; break; }
    } else if (mutation == 3) {
      std::erase_if(place.projections, [](const auto &step) {
        return step.kind == MirProjectionKind::Base;
      });
    } else {
      for (auto &step : place.projections)
        if (step.kind == MirProjectionKind::Base) {
          if (mutation == 1) step.constantIndex = 999;
          else step.selection += 1000;
          break;
        }
    }
    expect(verifyMirBodyStructure(*body, selected->owner).valid(),
           "Base owner/ordinal/instance mutations remain provisional structural shapes");
    expect(!verifyMirBody(*body, selected->owner).valid() && !verifyMirProgram(broken).valid(),
           "neither context-free nor final program verification accepts a forged Base authority");
  }
}
void checkNestedBaseLifetimeViews() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "nested-base-lifetime-views.gti", R"gti(
struct Root<T, lifetime a> { T&<a> value; Root(T&<a> input) : value(input) {} };
struct Middle<T, lifetime a> : public Root<T, a> {
  Middle(T&<a> input) : Root<T, a>(input) {}
};
struct Leaf<lifetime a> : public Middle<int, a> {
  Leaf(int&<a> input) : Middle<int, a>(input) {}
  int&<a> get() { return value; }
};
int&<a> read<lifetime a>(Leaf<a>& input) { return input.value; }
int main() { int input = 7; Leaf value{input}; return read(value) + value.get() - 14; }
)gti");
  if (!checked.canGenerateCode()) {
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "nested Base instances preserve their exact actual owner lifetime views");
  }
  bool examined = false;
  for (const auto &owner : checked.mir.classInstances()) {
    if (owner.structuralBases.empty()) continue;
    examined = true;
    for (int mutation = 0; mutation != 3; ++mutation) {
      auto forged = checked.mir;
      auto *changed = const_cast<MirClassInstance *>(forged.findClassInstance(owner.id));
      auto &base = changed->structuralBases.front();
      if (mutation == 0) base.instance = owner.id;
      if (mutation == 1) base.type.lifetimeArguments.front() = Lifetime::staticLifetime();
      if (mutation == 2) base.interface = true;
      changed->bases.front() = base;
      expect(!verifyMirProgram(forged).valid(),
             "exact Base views preserve selected instance, declaration-region recipe and storage kind");
    }
  }
  expect(examined, "nested Base fixture retains a structural base inventory");
  bool ordinaryPublication = false;
  for (const auto &constructor : checked.mir.constructorInstances())
    for (const auto &loan : constructor.body.loans)
      if (mir_detail::baseConstructorPublicationStage(constructor.body, loan)) {
        ordinaryPublication = true;
        expect(!loan.lifecycleBound,
               "a generated base cleanup does not invent destructor-bound borrow retention");
        auto forged = checked.mir;
        auto *changed = const_cast<MirConstructorInstance *>(forged.findConstructorInstance(constructor.id));
        changed->body.loans[loan.id - 1].lifecycleBound = true;
        expect(!verifyMirProgram(forged).valid(),
               "base publication must retain its exact destructor-observation policy");
      }
  expect(ordinaryPublication, "nested Base fixture retains non-cleanup-bound borrowed publication");
  bool returned = false;
  for (const auto &function : checked.mir.functionInstances()) {
    if (!function.completeReturnBorrow || function.completeReturnBorrow->dependencies.size() != 1)
      continue;
    const auto &path = function.completeReturnBorrow->dependencies.front().sourceFields;
    if (path.size() != 3 || path.front().kind != PlaceProjectionKind::Base ||
        path[1].kind != PlaceProjectionKind::Base || path.back().kind != PlaceProjectionKind::Field)
      continue;
    returned = true;
    auto forged = checked.mir;
    auto *changed = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
    auto &dependency = changed->completeReturnBorrow->dependencies.front();
    dependency.sourceFields.erase(dependency.sourceFields.begin() + 1);
    changed->returnBorrowFields = changed->completeReturnBorrow->dependencies;
    expect(!verifyMirProgram(forged).valid(),
           "complete inherited returns cannot skip a typed intermediate Base owner");
  }
  expect(returned, "nested Base fixture independently publishes the full inherited external supplier path");
  bool produced = false;
  for (const auto &function : checked.mir.functionInstances())
    for (const auto &loan : function.body.loans) {
      const auto *place = function.body.findPlace(loan.source);
      if (loan.entry || loan.kind != MirLoanKind::Return || !place ||
          place->projections.empty() || place->projections.back().kind != MirProjectionKind::Dereference ||
          std::count_if(place->projections.begin(), place->projections.end(), [](const auto &step) {
            return step.kind == MirProjectionKind::Base;
          }) != 2)
        continue;
      produced = true;
      for (int mutation = 0; mutation != 2; ++mutation) {
        auto forged = checked.mir;
        auto *changed = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
        auto &source = changed->body.places[place->id - 1];
        if (mutation == 0)
          source.projections.push_back({.kind = MirProjectionKind::Dereference});
        else {
          source.access = AccessMode::Mutable;
          changed->body.loans[loan.id - 1].access = AccessMode::Mutable;
        }
        expect(!verifyMirProgram(forged).valid(),
               "inherited return replay cannot invent an extra reference crossing or mutable capability");
      }
    }
  expect(produced, "nested Base fixture has an independently replayed stored-reference return producer");
}

void checkLocalStoredReferencePublication() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "local-stored-reference-publication.gti", R"gti(
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
};
struct Pair<lifetime a, lifetime b> : public Base<a> {
  int&<b> other;
  Pair(int&<a> first, int&<b> second) : Base<a>(first), other(second) {}
};
int&<a> external<lifetime a>(int&<a> input) {
  int local = 9;
  Pair pair{input, local};
  return pair.value;
}
int main() { int value = 7; return external(value) - 7; }
)gti");
  if (!checked.canGenerateCode())
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(checked.canGenerateCode(),
         "a local carrier publishes one caller-owned field without its unrelated local reference");
  HirFunctionInstanceId owner = 0;
  MirInstructionId publication = 0;
  for (const auto &function : checked.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *result = instruction.loan
            ? function.body.findLoan(*instruction.loan) : nullptr;
        const auto *carrier = instruction.operands.size() == 1
            ? function.body.findLoan(instruction.operands.front().loan) : nullptr;
        if (instruction.kind == MirInstructionKind::Borrow && result && carrier &&
            result->kind == MirLoanKind::Return && carrier->kind == MirLoanKind::Stored &&
            !carrier->entry && instruction.operands.front().place) {
          expect(owner == 0, "fixture needs one selected local-carrier reference publication");
          owner = function.id;
          publication = instruction.id;
          expect(block.terminator.kind == MirTerminatorKind::Return &&
                     block.terminator.value &&
                     block.terminator.value->kind == MirOperandKind::Loan &&
                     block.terminator.value->loan == result->id &&
                     !block.terminator.value->place,
                 "return must consume the captured pointer after carrier cleanup");
          expect(result->additionalDependencies.empty() && result->designatedFields.empty() &&
                     carrier->additionalDependencies.size() == 1,
                 "selected reference and containing dependency inventory remain distinct");
        }
      }
  expect(owner && publication, "fixture must retain the exact field address independently of its owner");
  for (int mutation = 0; mutation != 7; ++mutation) {
    auto broken = checked.mir;
    auto *function = const_cast<MirFunctionInstance *>(broken.findFunctionInstance(owner));
    MirInstruction *publish = nullptr;
    for (auto &block : function->body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == publication) publish = &instruction;
    expect(publish && publish->loan, "fixture must retain its exact publication");
    auto &result = function->body.loans[*publish->loan - 1];
    const auto &carrier = function->body.loans[publish->operands.front().loan - 1];
    const auto &unrelated = carrier.additionalDependencies.front();
    if (mutation == 0) result.source = unrelated.source;
    if (mutation == 1) publish->operands.front().place = 0;
    if (mutation == 2)
      result.additionalDependencies.push_back({.source = unrelated.source,
                                               .access = AccessMode::ReadOnly});
    if (mutation == 3) result.access = AccessMode::Mutable;
    if (mutation == 4) {
      auto &runtime = function->body.places[publish->operands.front().place - 1];
      expect(unrelated.designatedFields.size() == 1 &&
                 unrelated.designatedFields.front().kind == PlaceProjectionKind::Field,
             "fixture needs an independent direct field for the coordinated forgery");
      runtime.projections = {
          {.kind = MirProjectionKind::Field, .field = unrelated.designatedFields.front().field},
          {.kind = MirProjectionKind::Dereference}};
      runtime.key = function->body.findPlace(unrelated.source)->key;
      result.source = unrelated.source;
    }
    if (mutation == 5) {
      for (auto &block : function->body.blocks)
        if (block.terminator.returnLoan == result.id && block.terminator.value)
          block.terminator.value->place = publish->operands.front().place;
    }
    if (mutation == 6) {
      for (auto &block : function->body.blocks) {
        const auto found = std::find_if(block.instructions.begin(), block.instructions.end(),
            [&](const auto &item) { return item.id == publication; });
        if (found != block.instructions.end()) {
          const auto moved = *found;
          block.instructions.erase(found);
          block.instructions.push_back(moved);
          break;
        }
      }
    }
    expect(!verifyMirProgram(broken).valid(),
           "selected reference rejects forged owners, access, local escape and reads after carrier cleanup");
  }
}

void checkFailedBasePublicationRelease() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "failed-base-publication-release.gti", R"gti(
mut int observed = 0;
interface Marker { int mark(); };
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
  ~Base() { observed = observed + value; }
};
struct Derived<lifetime a> : public Marker, public Base<a> {
  Derived(int&<a> input) : Base<a>(input) {}
  int mark() override { return 0; }
  int&<a> get() { return value; }
};
int&<a> read<lifetime a>(Derived<a>& view) { return view.value; }
int main() {
  int value = 7;
  { Derived view{value}; if (read(view) + view.get() != 14) { return 1; } }
  return observed - 7;
}
)gti");
  if (!checked.canGenerateCode())
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(checked.canGenerateCode(),
         "a failed base construction releases its unpublished dependency without dropping an unconstructed base");
  HirConstructorInstanceId owner = 0;
  MirBlockId failureBlock = 0;
  MirBlockId successBlock = 0;
  MirLoanId publication = 0;
  for (const auto &constructor : checked.mir.constructorInstances())
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *loan = instruction.loan
            ? constructor.body.findLoan(*instruction.loan) : nullptr;
        if (instruction.kind == MirInstructionKind::InitializeBase && loan &&
            loan->lifecycleBound) {
          expect(owner == 0 && block.terminator.kind == MirTerminatorKind::Invoke,
                 "fixture needs one exact fallible cleanup-bound base publication");
          owner = constructor.id;
          publication = loan->id;
          failureBlock = block.terminator.elseTarget;
          successBlock = block.terminator.target;
        }
      }
  expect(owner && failureBlock && successBlock,
         "fixture must distinguish the failed initialization from its successful publication");
  for (int mutation = 0; mutation != 2; ++mutation) {
    auto broken = checked.mir;
    auto *constructor = const_cast<MirConstructorInstance *>(
        broken.findConstructorInstance(owner));
    auto *failed = const_cast<MirBlock *>(constructor->body.findBlock(failureBlock));
    auto *succeeded = const_cast<MirBlock *>(constructor->body.findBlock(successBlock));
    const auto end = std::find_if(failed->instructions.begin(), failed->instructions.end(),
        [&](const auto &instruction) {
          return instruction.kind == MirInstructionKind::EndBorrow &&
                 instruction.loan == publication;
        });
    expect(end != failed->instructions.end(),
           "failed initialization must retire its own unpublished dependency");
    if (mutation == 0)
      succeeded->instructions.insert(succeeded->instructions.begin(), *end);
    failed->instructions.erase(end);
    expect(!verifyMirProgram(broken).valid(),
           "base dependency release must neither occur on success nor disappear on failure");
  }
}

void checkBorrowedBasePublication() {
  const auto source = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "borrowed-base-publication.gti", R"gti(
interface Marker { int mark(); };
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input) : value(input) {}
  ~Base() { int observed = this.value; }
};
int later(int divisor) { return 1 / divisor; }
struct Derived<lifetime a> : public Marker, public Base<a> {
  int after;
  Derived(int&<a> input, int divisor) : Base<a>(input), after(later(divisor)) {}
  int mark() override { return 0; }
};
int&<a> inherited<lifetime a>(Derived<a>& input) { return input.value; }
int main() { int number = 7; Derived view{number, 1}; return inherited(view) - 7; }
)gti");
  if (!source.semanticValid || !source.mir.valid()) {
    for (const auto &diagnostic : source.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto &error : verifyMirProgram(source.mir).errors)
      std::cerr << error.message << '\n';
    expect(false, "a borrowed Base after an interface retains exact rollback publication");
  }
  HirConstructorInstanceId owner = 0;
  MirLoanId publication = 0;
  MirInstructionId stageId = 0;
  for (const auto &constructor : source.mir.constructorInstances())
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *loan = instruction.loan ? constructor.body.findLoan(*instruction.loan) : nullptr;
        if (instruction.kind == MirInstructionKind::InitializeBase && loan &&
            loan->lifecycleBound && loan->storedPath.size() == 1 &&
            loan->storedPath.front().kind == PlaceProjectionKind::Base &&
            loan->storedPath.front().index == 1) {
          expect(owner == 0, "fixture must have one borrowed state-bearing Base publication");
          owner = constructor.id; publication = loan->id; stageId = instruction.id;
        }
      }
  expect(owner != 0, "fixture must exercise a destructor-bound Base at the full direct-base ordinal");
  for (int mutation = 0; mutation != 8; ++mutation) {
    auto broken = source.mir;
    auto *constructor = const_cast<MirConstructorInstance *>(broken.findConstructorInstance(owner));
    auto &loan = constructor->body.loans[publication - 1];
    MirInstruction *stage = nullptr;
    for (auto &block : constructor->body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == stageId) stage = &instruction;
    expect(stage && stage->successResultDrop && stage->constructorInitializer,
           "base publication mutation must retain its exact stage and rollback witness");
    if (mutation == 0) loan.lifecycleBound = false;
    if (mutation == 1) loan.source = 0;
    if (mutation == 2)
      constructor->initializers[stage->constructorInitializer - 1].completeBorrow.reset();
    if (mutation == 3)
      constructor->body.dropObligations[*stage->successResultDrop - 1].dropType.type =
          constructor->ownerType;
    if (mutation == 4) {
      bool moved = false;
      for (auto &block : constructor->body.blocks) {
        const auto end = std::find_if(block.instructions.begin(), block.instructions.end(),
            [&](const auto &instruction) {
              return instruction.kind == MirInstructionKind::EndBorrow && instruction.loan == publication;
            });
        const auto drop = std::find_if(block.instructions.begin(), block.instructions.end(),
            [&](const auto &instruction) {
              return instruction.kind == MirInstructionKind::Drop &&
                     instruction.destination == stage->destination;
            });
        if (end != block.instructions.end() && drop != block.instructions.end() && drop < end) {
          std::rotate(drop, end, end + 1); moved = true; break;
        }
      }
      expect(moved, "rollback mutation must move a real retained-dependency end before its exact drop");
    }
    if (mutation == 5) stage->kind = MirInstructionKind::Compute;
    if (mutation == 6) stage->loan.reset();
    if (mutation == 7) {
      auto duplicate = *stage;
      duplicate.id = constructor->body.instructionCount() + 1;
      constructor->body.blocks.back().instructions.push_back(std::move(duplicate));
    }
    if (mutation >= 5)
      expect(mir_detail::baseConstructorPublicationStage(constructor->body, loan) == nullptr,
             "Base loan production requires one exact InitializeBase stage");
    expect(!verifyMirProgram(broken).valid(),
           "Base publication rejects missing retention, origin, complete evidence, rollback type, early release and producer forgery");
  }
  bool inheritedReturn = false;
  for (const auto &function : source.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (std::size_t index = 0; index + 2 < block.instructions.size(); ++index) {
        const auto &publication = block.instructions[index];
        const auto &retirement = block.instructions[index + 1];
        if (publication.kind != MirInstructionKind::Borrow || !publication.loan ||
            publication.operands.size() != 1 ||
            retirement.kind != MirInstructionKind::EndBorrow || !retirement.loan)
          continue;
        const auto *shell = function.body.findLoan(*retirement.loan);
        const auto *entry = function.body.findLoan(publication.operands.front().loan);
        const auto *returned = function.body.findLoan(*publication.loan);
        if (!shell || shell->kind != MirLoanKind::Parameter || !entry ||
            entry->kind != MirLoanKind::Stored || !returned ||
            returned->kind != MirLoanKind::Return)
          continue;
        expect(!inheritedReturn && shell->storedDependency == entry->id &&
                   block.instructions[index + 2].kind == MirInstructionKind::EndBorrow &&
                   block.instructions[index + 2].loan == entry->id,
               "inherited external return retires its distinct reference shell and stored entry");
        inheritedReturn = true;
        for (int mutation = 0; mutation != 10; ++mutation) {
          auto broken = source.mir;
          auto *owner = const_cast<MirFunctionInstance *>(broken.findFunctionInstance(function.id));
          auto &body = owner->body;
          auto &changed = body.blocks[block.id - 1];
          auto &sourceShell = body.loans[shell->id - 1];
          auto &sourceEntry = body.loans[entry->id - 1];
          auto &runtime = body.places[publication.operands.front().place - 1];
          if (mutation == 0) sourceShell.storedDependency = 0;
          if (mutation == 1) sourceShell.source = 0;
          if (mutation == 2) sourceShell.parent = sourceEntry.id;
          if (mutation == 3) sourceShell.access = AccessMode::Mutable;
          if (mutation == 4) sourceEntry.access = AccessMode::Mutable;
          if (mutation == 5) {
            bool changedBase = false;
            for (auto &step : runtime.projections)
              if (step.kind == MirProjectionKind::Base) {
                step.constantIndex = 0; changedBase = true; break;
              }
            expect(changedBase, "inherited return mutation uses an exact full-ordinal Base projection");
          }
          if (mutation == 6)
            for (auto &step : runtime.projections)
              if (step.kind == MirProjectionKind::Base) { step.selection += 1000; break; }
          if (mutation == 7)
            body.loans[returned->id - 1].source = sourceEntry.source;
          if (mutation == 8)
            changed.instructions.erase(changed.instructions.begin() + index + 1);
          if (mutation == 9)
            std::swap(changed.instructions[index + 1], changed.instructions[index + 2]);
          expect(!verifyMirProgram(broken).valid(),
                 "inherited return rejects foreign shell, entry, permission, typed path, address and retirement");
        }
      }
  expect(inheritedReturn, "borrowed Base fixture exercises referenced-carrier external publication");
}
void checkReferenceErasureLifetimeAuthority() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto prelude = std::filesystem::weakly_canonical(root / "prelude.gti");
  const auto capability = std::filesystem::weakly_canonical(root / "std/detail/memory_capabilities.gti");
  const auto entry = std::filesystem::weakly_canonical(root / "__trusted_tests__/lifetime-erasure-mir.gti");
  const std::string declarations = R"gti(
export module detail.memory_capabilities;
namespace gti_internal {
export class erased_object {};
export erased_object& erase_reference<T>(T& object);
export mut erased_object& erase_reference_mut<T>(mut T& object);
export unsafe T&<a> restore_reference<T, lifetime a>(erased_object&<a> object) requires outlives<T, a>;
export unsafe mut T&<a> restore_reference_mut<T, lifetime a>(mut erased_object&<a> object) requires outlives<T, a>;
}
)gti";
  const std::string source = R"gti(
import "../std/detail/memory_capabilities.gti";
using Opaque = gti_internal::erased_object;
Opaque& erase<T>(T& source) { return gti_internal::erase_reference(source); }
mut Opaque& erase_mut<T>(mut T& source) { return gti_internal::erase_reference_mut(source); }
unsafe T&<a> restore<T, lifetime a>(Opaque&<a> source) requires outlives<T, a> {
  unsafe { return gti_internal::restore_reference<T>(source); }
}
unsafe mut T&<a> restore_mut<T, lifetime a>(mut Opaque&<a> source) requires outlives<T, a> {
  unsafe { return gti_internal::restore_reference_mut<T>(source); }
}
struct Borrowed<lifetime storage> {
  int&<storage> value;
  Borrowed(int&<storage> source) : value(source) {}
};
unsafe Borrowed<storage>&<shell> recover<lifetime storage, lifetime shell>(Opaque&<shell> source)
    requires outlives<storage, shell> {
  unsafe { return restore<Borrowed<storage>, shell>(source); }
}
int inspect<lifetime storage, lifetime shell>(Borrowed<storage>&<shell> source) {
  unsafe { return recover<storage, shell>(erase(source)).value; }
}
void inspect_unused<lifetime storage, lifetime shell>(Borrowed<storage>&<shell> source) {
  unsafe { [[discard]] recover<storage, shell>(erase(source)); }
}
int main() {
  mut int number = 3;
  unsafe { restore_mut<int>(erase_mut(number)) = 7; }
  Borrowed view{number};
  inspect_unused(view);
  unsafe { return restore<int>(erase(number)) + inspect(view) - 14; }
}
)gti";
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      entry, source, {prelude, entry},
      {{prelude.string(), ""}, {capability.string(), declarations}, {entry.string(), source}}, {root});
  if (!checked.canGenerateCode())
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << " at " << diagnostic.primary.source << ':'
                << diagnostic.primary.start << '-' << diagnostic.primary.end << ": "
                << diagnostic.message << '\n';
  expect(checked.canGenerateCode(),
         "trusted erased views preserve read/mutable origins and independently prove restored borrowed types");
  struct Site { MirBodyAddress body; MirInstructionId instruction; IntrinsicKind intrinsic; };
  std::vector<Site> sites;
  std::vector<IntrinsicKind> kinds;
  for (const auto address : enumerateMirBodyAddresses(checked.mir))
    if (const auto *body = findMirBody(checked.mir, address))
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.intrinsic == IntrinsicKind::EraseReference ||
              instruction.intrinsic == IntrinsicKind::EraseReferenceMut ||
              instruction.intrinsic == IntrinsicKind::RestoreReference ||
              instruction.intrinsic == IntrinsicKind::RestoreReferenceMut) {
            expect(checkedMirCallableLifetimeSignature(checked.mir, instruction) &&
                       instantiateMirLifetimeTarget(checked.mir, instruction),
                   "every erased-view operation has a rederived trusted lifetime rule");
            sites.push_back({address, instruction.id, instruction.intrinsic});
            if (std::find(kinds.begin(), kinds.end(), instruction.intrinsic) == kinds.end())
              kinds.push_back(instruction.intrinsic);
          }
  expect(kinds.size() == 4, "erased-view fixture reaches all four trusted operations");
  const auto instructionAt = [](MirBody &body, MirInstructionId id) -> MirInstruction * {
    for (auto &block : body.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == id) return &instruction;
    return nullptr;
  };
  bool borrowedRestoration = false;
  for (const auto &site : sites) {
    const bool restoring = site.intrinsic == IntrinsicKind::RestoreReference ||
                           site.intrinsic == IntrinsicKind::RestoreReferenceMut;
    for (int mutation = 0; mutation != 10; ++mutation) {
      auto broken = checked.mir;
      auto *body = findMirBody(broken, site.body);
      auto *instruction = instructionAt(*body, site.instruction);
      auto &application = instruction->lifetimeApplication;
      auto &witness = *application.callableSignature;
      const auto outer = witness.signature.arguments.front().referenceLifetime;
      switch (mutation) {
      case 0: application.callableSignature.reset(); break;
      case 1: witness.signatureBindings.push_back({outer, Lifetime::staticLifetime()}); break;
      case 2: witness.contract.receiver = outer; break;
      case 3: witness.contract.bounds.push_back({Lifetime::staticLifetime(), outer}); break;
      case 4: witness.signature.arguments[1].referenceLifetime = Lifetime::staticLifetime(); break;
      case 5: witness.signature.arguments[restoring ? 1 : 0].arguments.front().classId += 100000; break;
      case 6:
        instruction->intrinsic = restoring
            ? (site.intrinsic == IntrinsicKind::RestoreReference ? IntrinsicKind::RestoreReferenceMut
                                                               : IntrinsicKind::RestoreReference)
            : (site.intrinsic == IntrinsicKind::EraseReference ? IntrinsicKind::EraseReferenceMut
                                                             : IntrinsicKind::EraseReference);
        break;
      case 7: application.bindings.clear(); break;
      case 8: application.complete = false; break;
      case 9:
        witness.contract.requirements.nodes.push_back(
            {.kind = LifetimeRequirementNode::Kind::SemanticPredicate, .predicate = 1, .semanticTruth = true});
        witness.contract.requirements.root = witness.contract.requirements.nodes.size() - 1;
        break;
      }
      expect(!verifyMirLifetimeApplications(broken, *body, site.body.owner).valid(),
             "erased-view witnesses cannot invent scope, access, capability, receiver facts or requirements");
    }
    if (!restoring) continue;
    auto missing = checked.mir;
    auto *body = findMirBody(missing, site.body);
    auto *instruction = instructionAt(*body, site.instruction);
    const auto signature = instantiateMirLifetimeTarget(missing, *instruction);
    expect(signature.has_value(), "restoration mutation starts from the exact applied signature");
    const LifetimeTypeBound required{signature->returnType.arguments.front(),
                                     signature->returnType.referenceLifetime};
    const auto removed = std::erase(instruction->lifetimeApplication.typeObligations, required);
    std::erase(body->lifetimeBody.typeObligations, required);
    expect(removed != 0 && !verifyMirLifetimeApplications(missing, *body, site.body.owner).valid(),
           "restored referent validity cannot disappear from both application and body facts");
    if (required.type.kind != SemanticType::Class || required.type.lifetimeArguments.empty())
      continue;
    borrowedRestoration = true;
    auto stronger = checked.mir;
    body = findMirBody(stronger, site.body);
    instruction = instructionAt(*body, site.instruction);
    // A forged extra obligation is not an assumption. Even with complete
    // application/body metadata, the independent stored region is not static.
    const LifetimeTypeBound forged{required.type, Lifetime::staticLifetime()};
    instruction->lifetimeApplication.typeObligations.push_back(forged);
    body->lifetimeBody.typeObligations.push_back(forged);
    expect(!verifyMirLifetimeApplications(stronger, *body, site.body.owner).valid(),
           "unsafe opaque restoration cannot manufacture static validity for a nested borrowed type");
  }
  expect(borrowedRestoration, "restoration fixture exercises a concrete borrowed type obligation");
  bool opaqueWitness = false, convertedInput = false, repeatedReferent = false;
  bool reborrowedAdditionalInput = false, unusedPublication = false;
  for (const auto address : enumerateMirBodyAddresses(checked.mir)) {
    const auto *body = findMirBody(checked.mir, address);
    if (!body) continue;
    for (const auto &place : body->places)
      for (const auto &other : body->places) {
        if (place.id >= other.id || place.root != MirPlaceRootKind::Binding ||
            place.projections.empty() || place.sourceValue || other.sourceValue ||
            place.root != other.root || place.binding != other.binding ||
            place.symbol != other.symbol || place.loan != other.loan ||
            place.projections != other.projections || place.type != other.type ||
            place.access != other.access || !place.initiallyAvailable || !other.initiallyAvailable)
          continue;
        const auto source = std::find_if(body->places.begin(), body->places.end(),
            [](const auto &candidate) { return candidate.sourceValue != 0; });
        if (source == body->places.end()) continue;
        repeatedReferent = true;
        auto forged = checked.mir;
        auto *changedBody = findMirBody(forged, address);
        changedBody->places[other.id - 1].sourceValue = source->sourceValue;
        expect(!verifyMirProgram(forged).valid(),
               "duplicate physical projections cannot claim conflicting evaluated source authority");
      }
    for (const auto &block : body->blocks)
      for (const auto &call : block.instructions) {
        if (!call.restoresReferenceResult || call.restoredBorrowFields.empty() ||
            !call.loan || call.operands.size() != 1 ||
            call.operands.front().kind != MirOperandKind::Value)
          continue;
        const auto *loan = body->findLoan(*call.loan);
        const auto *stage = mir_detail::definitionFor(*body, call.operands.front().value);
        if (!loan || !stage || stage->kind != MirInstructionKind::CallInput ||
            stage->operands.size() != 1 || stage->operands.front().kind != MirOperandKind::Loan)
          continue;
        const auto *witness = body->findLoan(stage->operands.front().loan);
        expect(witness && loan->storedDependency == witness->id &&
                   mir_detail::restoredBorrowAuthority(*body, *loan) == &call,
               "restored fields retain the exact evaluated opaque input capability");
        opaqueWitness = true;
        for (const auto &inputSource : witness->additionalDependencies) {
          if (!inputSource.designatedFields.empty() || !inputSource.conditions.empty())
            continue;
          const auto outputSource = std::find_if(loan->additionalDependencies.begin(),
              loan->additionalDependencies.end(), [&](const auto &candidate) {
                return candidate.source == inputSource.source &&
                       candidate.designatedFields.empty() && candidate.conditions.empty();
              });
          if (outputSource == loan->additionalDependencies.end() ||
              inputSource.parent == witness->id)
            continue;
          expect(outputSource->parent == witness->id,
                 "each scalar input supplier retains the exact evaluated input loan as its parent");
          reborrowedAdditionalInput = true;
          unusedPublication |= call.result && body->usesOf(*call.result).empty();
          const auto index = static_cast<std::size_t>(
              outputSource - loan->additionalDependencies.begin());
          for (int mutation = 0; mutation != 4; ++mutation) {
            auto forged = checked.mir;
            auto *changedBody = findMirBody(forged, address);
            auto &changed = changedBody->loans[loan->id - 1].additionalDependencies[index];
            if (mutation == 0) changed.parent = inputSource.parent;
            if (mutation == 1) changed.source = stage->operands.front().place;
            if (mutation == 2) changed.access = AccessMode::Mutable;
            if (mutation == 3) {
              changed.parent = inputSource.parent;
              changedBody->values[*call.result - 1].borrowDependencies.reset();
            }
            const bool rejected = !verifyMirProgram(forged).valid();
            if (!rejected)
              std::cerr << "additional restored supplier mutation " << mutation
                        << " accepted at owner " << address.owner << " loan "
                        << loan->id << " input " << witness->id << '\n';
            expect(rejected,
                   "an additional restored supplier cannot bypass its input loan or forge source/access");
          }
        }
        for (int mutation = 0; mutation != 5; ++mutation) {
          auto forged = checked.mir;
          auto *changedBody = findMirBody(forged, address);
          auto *changedStage = instructionAt(*changedBody, stage->id);
          auto &changedLoan = changedBody->loans[loan->id - 1];
          if (mutation == 0) changedLoan.parent = 0;
          if (mutation == 1) changedLoan.source = changedStage->operands.front().place;
          if (mutation == 2) changedLoan.access = AccessMode::Mutable;
          if (mutation == 3) {
            auto other = *witness;
            other.id = changedBody->loans.size() + 1;
            changedBody->loans.push_back(other);
            changedStage->operands.front().loan = other.id;
          }
          if (mutation == 4) {
            changedStage->operands.front().place = loan->source;
            expect(!verifyMirProgram(forged).valid(),
                   "restoration input cannot replace its addressed opaque place with a lifetime supplier");
            continue;
          }
          expect(!mir_detail::restoredBorrowAuthority(*changedBody, changedBody->loans[loan->id - 1]) &&
                     !verifyMirProgram(forged).valid(),
                 "restoration cannot substitute parent, source, access or another same-source input loan");
        }
      }
    for (const auto &block : body->blocks)
      for (const auto &stage : block.instructions) {
        if (stage.kind != MirInstructionKind::CallInput || stage.operands.size() != 1 ||
            stage.operands.front().kind != MirOperandKind::Loan ||
            stage.info.type.kind != SemanticType::Reference || stage.info.type.arguments.size() != 1)
          continue;
        const auto &operand = stage.operands.front();
        const auto *place = body->findPlace(operand.place);
        if (!place || place->type.kind != SemanticType::Class ||
            place->type == stage.info.type.arguments.front())
          continue;
        expect(operand.type.arguments.size() == 1 && operand.type.arguments.front() == place->type,
               "a reference input retains original nested regions before the independently checked conversion stage");
        convertedInput = true;
        auto forged = checked.mir;
        auto *changedBody = findMirBody(forged, address);
        auto *changedStage = instructionAt(*changedBody, stage.id);
        changedStage->operands.front().type = stage.info.type;
        expect(!verifyMirProgram(forged).valid(),
               "retagging the physical input cannot replace the checked lifetime conversion");
      }
  }
  expect(opaqueWitness && convertedInput && repeatedReferent &&
             reborrowedAdditionalInput && unusedPublication,
         "erasure controls cover opaque identity, used/unused suppliers, conversion types and repeated exact referents");
}

void checkReferenceErasureAddressIdentity() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto prelude = std::filesystem::weakly_canonical(root / "prelude.gti");
  const auto capability = std::filesystem::weakly_canonical(root / "std/detail/memory_capabilities.gti");
  const auto entry = std::filesystem::weakly_canonical(root / "__trusted_tests__/erasure-address-identity.gti");
  const std::string declarations = R"gti(export module detail.memory_capabilities;
namespace gti_internal {
export class erased_object {};
export erased_object& erase_reference<T>(T& input);
export mut erased_object& erase_reference_mut<T>(mut T& input);
export unsafe T&<a> restore_reference<T, lifetime a>(erased_object&<a> input) requires outlives<T, a>;
}
)gti";
  const std::string source = R"gti(import "../std/detail/memory_capabilities.gti";
using Opaque = gti_internal::erased_object;
Opaque& leaf(int& input) { return gti_internal::erase_reference(input); }
mut Opaque& mutable_leaf(mut int& input) { return gti_internal::erase_reference_mut(input); }
Opaque&<a> through<lifetime a>(int&<a> first, int&<a> second, bool choice) {
  if (choice && second == 7) { return leaf(first); }
  return leaf(first);
}
Opaque& forward(Opaque& input) { return input; }
[[borrowed_from(owner)]] unsafe Opaque& raw_forward(const Opaque* address, Opaque& owner) {
  unsafe { return *address; }
}
int main() {
  mut int first = 3;
  mut int exclusive = 11;
  int second = 7;
  Opaque& erased = forward(through(first, second, first == 3));
  unsafe {
    [[discard]] raw_forward(&erased, erased);
    [[discard]] gti_internal::restore_reference<int>(mutable_leaf(exclusive));
    return gti_internal::restore_reference<int>(erased) - 3;
  }
}
)gti";
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      entry, source, {prelude, entry},
      {{prelude.string(), ""}, {capability.string(), declarations}, {entry.string(), source}}, {root});
  if (!checked.canGenerateCode())
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(checked.canGenerateCode(), "erasure address proof fixture reaches verified MIR");
  const auto functionId = [&](std::string_view name) {
    for (const auto &function : checked.hir.functionInstances())
      if (function.source && function.source->name().lexeme == name)
        return function.id;
    return HirFunctionInstanceId{0};
  };
  const auto throughId = functionId("through");
  const auto leafId = functionId("leaf");
  const auto forwardId = functionId("forward");
  const auto rawId = functionId("raw_forward");
  const auto mutableId = functionId("mutable_leaf");
  const auto mainId = functionId("main");
  expect(throughId && leafId && forwardId && rawId && mutableId && mainId,
         "address proof covers static helpers, mutable erasure and raw owner witnesses");
  const auto *main = checked.mir.findFunctionInstance(mainId);
  const MirInstruction *throughCall = nullptr;
  bool forwarded = false, mutableInput = false, rejectedRaw = false;
  for (const auto &block : main->body.blocks)
    for (const auto &call : block.instructions) {
      if (call.kind != MirInstructionKind::Call || !call.functionTarget || !call.loan)
        continue;
      const auto target = *call.functionTarget;
      if (target != throughId && target != forwardId && target != rawId && target != mutableId)
        continue;
      const auto applied = instantiateMirLifetimeTarget(checked.mir, call);
      expect(applied.has_value(), "selected erasure fixture call has its exact lifetime application");
      const MirOperand output{.kind = MirOperandKind::Loan, .loan = *call.loan,
                              .type = applied->returnType};
      const auto identity = mir_detail::verifiedReferenceErasureSource(checked.mir, main->body, output);
      if (target == rawId) {
        expect(!identity, "raw owner witnesses are not whole-formal erasure identity");
        rejectedRaw |= target == rawId;
        continue;
      }
      expect(identity.has_value(), "finite erasing and forwarding helper chains retain actual address identity");
      if (target == throughId) {
        throughCall = &call;
        expect(identity->operand == call.operands.front() &&
                   identity->referenceType == call.parameterTypes.front(),
               "the query preserves the original caller CallInput operand and applied reference type");
      } else if (target == mutableId) {
        mutableInput = identity->referenceType.referenceAccess == AccessMode::Mutable &&
                       identity->operand == call.operands.front();
      } else {
        forwarded = identity->referenceType.arguments.front() == SemanticType::Int32;
      }
    }
  expect(throughCall && forwarded && mutableInput && rejectedRaw,
         "address queries exercise all intended source shapes");
  // Subobject publication preserves the physical formal's capability without
  // turning that subobject address into whole-formal pointer identity.
  const std::string projectedSource = R"gti(import "../std/detail/memory_capabilities.gti";
using Opaque = gti_internal::erased_object;
struct Pair { int first; int second; Pair(int input) : first(input), second(input + 1) {} };
Opaque&<a> field<lifetime a>(Pair&<a> input, Pair&<a> other) {
  [[discard]] gti_internal::erase_reference(input.second);
  [[discard]] gti_internal::erase_reference(other.first);
  return gti_internal::erase_reference(input.first);
}
int main() {
  Pair pair{3}; Pair other{7};
  unsafe { return gti_internal::restore_reference<int>(field(pair, other)) - 3; }
}
)gti";
  const auto projected = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      entry, projectedSource, {prelude, entry},
      {{prelude.string(), ""}, {capability.string(), declarations},
       {entry.string(), projectedSource}}, {root});
  if (!projected.canGenerateCode())
    for (const auto &diagnostic : projected.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(projected.canGenerateCode(),
         "projected-field erasure retains its exact physical reference parent through verified MIR");
  HirFunctionInstanceId fieldId = 0;
  for (const auto &function : projected.hir.functionInstances())
    if (function.source && function.source->name().lexeme == "field") fieldId = function.id;
  const auto *field = projected.mir.findFunctionInstance(fieldId);
  expect(field && std::any_of(field->body.places.begin(), field->body.places.end(),
             [](const auto &place) {
               return std::any_of(place.projections.begin(), place.projections.end(),
                   [](const auto &step) { return step.kind == MirProjectionKind::Field; });
             }), "projected-field control retains its real typed field address");
  bool rejectedField = false;
  for (const auto &function : projected.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &call : block.instructions)
        if (call.functionTarget == fieldId && call.loan) {
          const auto applied = instantiateMirLifetimeTarget(projected.mir, call);
          expect(applied.has_value(), "field control keeps the selected exact call contract");
          expect(!mir_detail::verifiedReferenceErasureSource(projected.mir, function.body,
                     {.kind = MirOperandKind::Loan, .loan = *call.loan,
                      .type = applied->returnType}),
                 "a helper field projection is not its whole formal pointer");
          rejectedField = true;
        }
  expect(rejectedField, "field identity negative reaches its actual helper call");
  std::vector<const MirInstruction *> projections;
  for (const auto &block : field->body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == MirInstructionKind::Call &&
          instruction.intrinsic == IntrinsicKind::EraseReference)
        projections.push_back(&instruction);
  expect(projections.size() == 3, "field controls retain three independently evaluated subobjects");
  const auto &selected = *projections.back();
  const auto *published = selected.loan ? field->body.findLoan(*selected.loan) : nullptr;
  const auto *sameOwner = projections[0]->loan ? field->body.findLoan(*projections[0]->loan) : nullptr;
  const auto *otherOwner = projections[1]->loan ? field->body.findLoan(*projections[1]->loan) : nullptr;
  const auto *parent = published ? field->body.findLoan(published->parent) : nullptr;
  expect(published && sameOwner && otherOwner && parent && parent->entry &&
             parent->kind == MirLoanKind::Parameter &&
             published->parent == sameOwner->parent && published->parent != otherOwner->parent &&
             published->source != sameOwner->source && published->source != otherOwner->source &&
             selected.result && field->body.findValue(*selected.result)->borrowDependencies,
         "subobject controls distinguish the same owner's sibling field from another formal");
  for (int mutation = 0; mutation != 6; ++mutation) {
    auto forged = projected.mir;
    auto &body = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(fieldId))->body;
    auto &claim = body.loans[published->id - 1];
    auto &facts = *body.values[*selected.result - 1].borrowDependencies;
    expect(facts.dependencies.size() == 1 && facts.dependencies.front().parent == parent->id,
           "the scalar publication carries its exact formal parent");
    auto &fact = facts.dependencies.front();
    if (mutation == 0) claim.parent = fact.parent = 0;
    if (mutation == 1) claim.parent = fact.parent = otherOwner->parent;
    if (mutation == 2) claim.source = fact.source = sameOwner->source;
    if (mutation == 3) claim.source = fact.source = otherOwner->source;
    if (mutation == 4) claim.access = fact.access = AccessMode::Mutable;
    if (mutation == 5) {
      bool reordered = false;
      for (auto &block : body.blocks) {
        const auto call = std::find_if(block.instructions.begin(), block.instructions.end(),
            [&](const auto &item) { return item.id == selected.id; });
        if (call == block.instructions.end()) continue;
        const auto endpoint = std::find_if(call, block.instructions.end(),
            [&](const auto &item) {
              return item.kind == MirInstructionKind::EndBorrow && item.loan == parent->id;
            });
        if (endpoint == block.instructions.end()) continue;
        std::rotate(call, endpoint, std::next(endpoint));
        reordered = true;
      }
      expect(reordered, "the live formal capability has a concrete endpoint after publication");
    }
    const bool rejected = !verifyMirProgram(forged).valid();
    if (!rejected) std::cerr << "physical subobject mutation " << mutation << " accepted\n";
    expect(rejected,
           "coordinated publication facts cannot erase or replace their exact subobject parent, source, access or endpoint");
  }
  const auto query = [&](const MirProgram &program) {
    const auto *caller = program.findFunctionInstance(mainId);
    const auto applied = instantiateMirLifetimeTarget(program, *throughCall);
    return applied ? mir_detail::verifiedReferenceErasureSource(program, caller->body,
        {.kind = MirOperandKind::Loan, .loan = *throughCall->loan,
         .type = applied->returnType}) : std::nullopt;
  };
  for (int mutation = 0; mutation != 8; ++mutation) {
    auto forged = checked.mir;
    auto &helper = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(throughId))->body;
    auto &leaf = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(leafId))->body;
    bool changed = false;
    if (mutation == 0) {
      // Same type, same region, same lifetime summary: change only the actual
      // staged pointer on one normal-return path to the other formal.
      const auto &target = *forged.findFunctionInstance(throughId);
      const auto inputs = mir_detail::concreteFormalInputs(helper, target.parameterTypes,
                                                          target.parameterBindings);
      expect(inputs && inputs->size() == 3, "two equal reference formals retain distinct ordinals");
      const auto second = std::find_if(helper.loans.begin(), helper.loans.end(), [&](const auto &loan) {
        return loan.entry && loan.kind == MirLoanKind::Parameter &&
               loan.carriers == std::vector<HirBindingId>{(*inputs)[1].binding};
      });
      const auto place = std::find_if(helper.places.begin(), helper.places.end(), [&](const auto &value) {
        return value.root == MirPlaceRootKind::Binding && value.binding == (*inputs)[1].binding &&
               value.type == SemanticType::Int32 && value.projections.size() == 1 &&
               value.projections.front().kind == MirProjectionKind::Dereference;
      });
      expect(second != helper.loans.end() && place != helper.places.end(),
             "the wrong-formal control has an exact independently typed entry address");
      for (auto &block : helper.blocks)
        for (auto &call : block.instructions)
          if (!changed && call.functionTarget == leafId) {
            auto *stage = call.operands.front().kind == MirOperandKind::Value
                ? const_cast<MirInstruction *>(mir_detail::definitionFor(helper, call.operands.front().value))
                : nullptr;
            auto &input = stage ? stage->operands.front() : call.operands.front();
            input.kind = MirOperandKind::Loan;
            input.value = 0;
            input.loan = second->id;
            input.place = place->id;
            changed = true;
          }
    } else if (mutation == 1) {
      for (auto &block : helper.blocks)
        if (!changed && block.terminator.kind == MirTerminatorKind::Return &&
            block.terminator.value) {
          block.terminator.value->loan = 0;
          changed = true;
        }
    } else if (mutation == 2) {
      for (auto &block : helper.blocks)
        for (const auto &instruction : block.instructions)
          if (!changed && instruction.kind == MirInstructionKind::Borrow && instruction.loan) {
            auto duplicate = instruction;
            duplicate.id += 100000;
            block.instructions.push_back(std::move(duplicate));
            changed = true;
            break;
          }
    } else {
      for (auto &block : leaf.blocks)
        for (auto &call : block.instructions)
          if (!changed && call.intrinsic == IntrinsicKind::EraseReference) {
            if (mutation == 3) call.intrinsic = IntrinsicKind::RawBorrow;
            if (mutation == 4) call.lifetimeApplication.bindings.push_back(
                {Lifetime::parameter(987654), Lifetime::staticLifetime()});
            if (mutation == 5) call.tombstone = true;
            if (mutation == 6) {
              call.intrinsic = IntrinsicKind::None;
              call.functionTarget = leafId;
              const auto *target = forged.findFunctionInstance(leafId);
              expect(target->lifetimeContract.parameters.size() == 1,
                     "recursive pointer helper has one exact invocation binder");
              call.lifetimeApplication.callableSignature.reset();
              call.lifetimeApplication.bindings = {{
                  target->lifetimeContract.parameters.front(),
                  call.parameterTypes.front().referenceLifetime}};
              expect(instantiateMirLifetimeTarget(forged, call).has_value(),
                     "recursive negative preserves the applied signature before the cycle check");
            }
            if (mutation == 7) call.operands.front().place = leaf.places.size() + 100;
            changed = true;
          }
    }
    expect(changed, "each erasure identity mutation reaches an actual proof edge");
    expect(!query(forged),
           "all-return address proof rejects wrong formal, return pointer, ambiguity, raw witness, application, tombstone, recursion and missing actual address");
  }
}

void checkCarrierReferenceShellInputs() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "carrier-reference-shell-inputs.gti", R"gti(int shared = 7;
int independent = 9;
struct Borrowed<lifetime storage> {
  int&<storage> value;
  Borrowed(int&<storage> input) : value(input) {}
};
Borrowed<storage>&<shell> relay<lifetime storage, lifetime shell>(
    Borrowed<storage>&<shell> input) { return input; }
int main() {
  Borrowed<static> first{shared};
  Borrowed<static> second{shared};
  Borrowed<static> third{independent};
  Borrowed<static>& firstRef = relay(first);
  Borrowed<static>& secondRef = relay(second);
  Borrowed<static>& thirdRef = relay(third);
  return firstRef.value + secondRef.value + thirdRef.value - 23;
}
)gti");
  if (!checked.canGenerateCode())
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(checked.canGenerateCode(),
         "reference results retain the actual carrier shell and its separate stored owners");
  bool sameOwner = false, differentOwner = false;
  for (const auto address : enumerateMirBodyAddresses(checked.mir)) {
    const auto *body = findMirBody(checked.mir, address);
    if (!body) continue;
    for (const auto &block : body->blocks)
      for (const auto &call : block.instructions) {
        const auto *loan = call.loan ? body->findLoan(*call.loan) : nullptr;
        if (call.kind != MirInstructionKind::Call || !call.functionTarget ||
            call.info.type.kind != SemanticType::Class || call.operands.size() != 1 ||
            call.operands.front().kind != MirOperandKind::Value ||
            !loan || loan->kind != MirLoanKind::CallResult || !loan->storedDependency)
          continue;
        const auto *group = body->findLoan(loan->storedDependency);
        const auto *stage = mir_detail::definitionFor(*body, call.operands.front().value);
        expect(group && group->kind == MirLoanKind::Stored && stage &&
                   stage->kind == MirInstructionKind::CallInput &&
                   stage->operands.size() == 1 &&
                   stage->operands.front().kind == MirOperandKind::BorrowRead,
               "carrier shell result has one exact read-only prepared input and stored group");
        const auto *input = body->findPlace(stage->operands.front().place);
        expect(input && mirLifetimeValueConversion(checked.mir, *body, address.owner,
                                                  input->type, call.info.type) &&
                   loan->source == input->id &&
                   std::find(group->carriers.begin(), group->carriers.end(), input->binding) !=
                       group->carriers.end() && loan->parent != group->id &&
                   std::any_of(loan->additionalDependencies.begin(), loan->additionalDependencies.end(),
                       [&](const auto &dependency) {
                         return dependency.source == group->source && dependency.parent == group->id;
                       }),
               "the primary input shell and every original external parent retain distinct roles");
        bool retainedAddress = false;
        for (const auto &consumerBlock : body->blocks)
          for (const auto &consumer : consumerBlock.instructions) {
            if (consumer.kind != MirInstructionKind::Initialize ||
                consumer.operands.size() != 1 ||
                consumer.operands.front().loan != loan->id)
              continue;
            const auto *place = body->findPlace(consumer.operands.front().place);
            retainedAddress |= place && place->root == MirPlaceRootKind::Loan &&
                               place->loan == loan->id && place->projections.empty();
          }
        expect(retainedAddress,
               "named reference binding retains the exact emitted call-result address");
        for (const auto &other : body->loans) {
          if (other.id == group->id || other.kind != MirLoanKind::Stored ||
              other.carriers.size() != 1 || other.designatedFields != group->designatedFields)
            continue;
          const auto unrelated = std::find_if(body->places.begin(), body->places.end(),
              [&](const auto &place) {
                return place.root == MirPlaceRootKind::Binding &&
                       place.binding == other.carriers.front() && place.projections.empty() &&
                       place.type == input->type && place.binding != input->binding;
              });
          if (unrelated == body->places.end()) continue;
          const auto *owner = body->findPlace(group->source);
          const auto *otherOwner = body->findPlace(other.source);
          expect(owner && otherOwner, "stored groups retain concrete owner identities");
          const bool sharesOwner = owner->root == otherOwner->root &&
              owner->symbol == otherOwner->symbol && owner->binding == otherOwner->binding &&
              owner->projections == otherOwner->projections;
          sameOwner |= sharesOwner;
          differentOwner |= !sharesOwner;
          for (int mutation = 0; mutation != 5; ++mutation) {
            auto forged = checked.mir;
            auto *changed = findMirBody(forged, address);
            if (mutation == 0) changed->loans[loan->id - 1].storedDependency = other.id;
            if (mutation == 1)
              for (auto &changedBlock : changed->blocks)
                for (auto &instruction : changedBlock.instructions)
                  if (instruction.id == stage->id)
                    instruction.operands.front().place = unrelated->id;
            if (mutation == 2) changed->loans[loan->id - 1].source = unrelated->id;
            if (mutation == 3)
              changed->loans[loan->id - 1].additionalDependencies.front().parent = other.id;
            if (mutation == 4)
              for (auto &changedBlock : changed->blocks)
                for (auto &instruction : changedBlock.instructions)
                  if (instruction.kind == MirInstructionKind::Initialize &&
                      instruction.operands.size() == 1 &&
                      instruction.operands.front().loan == loan->id)
                    instruction.operands.front().place = unrelated->id;
            expect(!verifyMirProgram(forged).valid(),
                   "same-typed unrelated carriers cannot replace input address, shell source or stored group");
          }
        }
      }
  }
  expect(sameOwner && differentOwner,
         "shell mutations distinguish different carriers sharing an owner from different external owners");
  bool retainedSources = false;
  for (const auto &function : checked.hir.functionInstances()) {
    if (!function.source || function.source->name().lexeme != "main") continue;
    for (const auto &loan : function.body.loans) {
      if (loan.kind != SemanticLoanKind::Reference || loan.entry ||
          loan.additionalDependencies.size() != 1 || !loan.parent)
        continue;
      const auto peer = std::find_if(function.body.loans.begin(), function.body.loans.end(),
          [&](const auto &other) {
            return other.kind == SemanticLoanKind::Reference && !other.entry &&
                   other.additionalDependencies.size() == 1 && other.parent &&
                   other.additionalDependencies.front().place != loan.additionalDependencies.front().place;
          });
      expect(peer != function.body.loans.end(), "retained source controls have another exact carrier");
      retainedSources = true;
      for (int mutation = 0; mutation != 4; ++mutation) {
        auto forged = checked.hir;
        auto &body = const_cast<HirFunctionInstance *>(forged.findFunctionInstance(function.id))->body;
        auto &changed = *std::find_if(body.loans.begin(), body.loans.end(),
            [&](const auto &candidate) { return candidate.semanticLoan == loan.semanticLoan; });
        if (mutation == 0) changed.parent = 0;
        if (mutation == 1) changed.additionalDependencies.front().place = peer->additionalDependencies.front().place;
        if (mutation == 2) changed.additionalDependencies.front().access = AccessMode::Mutable;
        if (mutation == 3) changed.additionalDependencies.push_back(changed.additionalDependencies.front());
        expect(!MirLowerer{}.lower(forged, checked.failureMetadata).valid(),
               "retention rejects a changed semantic parent, carrier, access or complete supplier count");
      }
    }
  }
  expect(retainedSources, "retention controls reach the complete semantic reference supplier set");
}

bool checkRawOwnerLifetimeFacts() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "raw-owner-lifetime-facts.gti", R"gti([[borrowed_from(owner)]] unsafe T& borrow<T, Owner>(const T* address, Owner& owner) { unsafe { return *address; } }
[[borrowed_from(owner)]] unsafe mut T& borrow_mut<T, Owner>(T* address, mut Owner& owner) { unsafe { return *address; } }
struct Storage { mut int value = 7; Storage() {} };
int rewrite(mut int& value) { value = 5; return value; }
int main() {
  mut Storage storage{};
  if (rewrite(storage.value) != 5) { return 1; }
  unsafe {
    borrow_mut(&storage.value, storage) = 11;
    return borrow(&storage.value, storage) - 11;
  }
}
)gti");
  if (!checked.canGenerateCode()) {
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return false;
  }
  expect(verifyMirProgram(checked.mir).valid(),
         "shared and exclusive raw-reference producers replay their checked owner");
  bool shared = false, exclusive = false;
  for (const auto &function : checked.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *loan = mir_detail::rawBorrowResultLoan(function.body, instruction);
        if (!loan)
          continue;
        shared |= instruction.intrinsic == IntrinsicKind::RawBorrow;
        exclusive |= instruction.intrinsic == IntrinsicKind::RawBorrowMut;
        expect(instruction.result && loan->parent &&
                   function.body.values[*instruction.result - 1].borrowDependencies,
               "raw-reference controls require an explicit checked owner fact");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto forged = checked.mir;
          auto *target = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
          auto &changed = *std::find_if(target->body.blocks[block.id - 1].instructions.begin(),
                                       target->body.blocks[block.id - 1].instructions.end(),
                                       [&](const auto &candidate) { return candidate.id == instruction.id; });
          if (mutation == 0)
            changed.unsafeOperation = UnsafeOperationKind::None;
          else if (mutation == 1)
            target->body.loans[loan->id - 1].parent = 0;
          else
            target->body.values[*instruction.result - 1].borrowDependencies->dependencies.clear();
          expect(!verifyMirProgram(forged).valid(),
                 "raw-reference replay rejects missing unsafe authority, owner ancestry or lifetime supplier");
        }
      }
  bool rejectedWriteOmission = false;
  for (const auto &function : checked.mir.functionInstances()) {
    if (function.owner || function.parameterTypes.size() != 1 ||
        function.returnType != SemanticType::Int32 ||
        function.parameterTypes.front().kind != SemanticType::Reference ||
        function.parameterTypes.front().referenceAccess != AccessMode::Mutable ||
        !function.effectSummary.traits.writesPlace)
      continue;
    auto forged = checked.mir;
    auto *target = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
    target->effectSummary.traits.writesPlace = false;
    target->effectSummary.traits.writesUnknownMemory = false;
    const auto proof = verifyMirProgram(forged);
    expect(!proof.valid() && std::any_of(proof.errors.begin(), proof.errors.end(), [](const auto &error) {
      return error.message.find("effect") != std::string::npos;
    }), "preserving carrier facts requires independently verified non-writing effects");
    rejectedWriteOmission = true;
  }
  expect(rejectedWriteOmission, "effect control must reach a mutable operation that writes");
  expect(shared && exclusive, "raw-reference controls must cover both access modes");
  return true;
}

bool checkCompletePhysicalReferenceSources() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "complete-physical-reference-sources.gti", R"gti(
[[borrowed_from(owner)]] unsafe T& borrow<T, Owner>(const T* address, Owner& owner) {
  unsafe { return *address; }
}
struct View<lifetime a> {
  int&<a> external; int local = 7;
  View(int&<a> input) : external(input) {}
  int& read() { unsafe { return borrow(&this.local, this); } }
};
int main() { int value = 3; View view{value}; return view.read() - 7; }
)gti");
  if (!checked.canGenerateCode()) {
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return false;
  }
  bool exercised = false;
  for (const auto &function : checked.mir.functionInstances()) {
    if (function.owner || !function.parameterTypes.empty())
      continue;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *loan = instruction.loan ? function.body.findLoan(*instruction.loan) : nullptr;
        const auto *value = instruction.result ? function.body.findValue(*instruction.result) : nullptr;
        if (instruction.kind != MirInstructionKind::Call || !instruction.functionTarget ||
            !loan || loan->kind != MirLoanKind::CallResult || loan->additionalDependencies.size() != 1 ||
            !value || !value->borrowDependencies || value->borrowDependencies->dependencies.size() != 2)
          continue;
        expect(loan->source != loan->additionalDependencies.front().source,
               "complete reference result separates carrier storage and its stored external source");
        for (int mutation = 0; mutation != 4; ++mutation) {
          auto forged = checked.mir;
          auto *changed = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
          auto &group = changed->body.loans[loan->id - 1];
          auto &facts = changed->body.values[value->id - 1].borrowDependencies->dependencies;
          if (mutation == 0) {
            const auto omitted = group.additionalDependencies.front().source;
            group.additionalDependencies.clear();
            std::erase_if(facts, [&](const auto &fact) { return fact.source == omitted; });
          } else if (mutation == 1) {
            const auto old = group.source;
            group.source = group.additionalDependencies.front().source;
            for (auto &fact : facts)
              if (fact.source == old) fact.source = group.source;
          } else if (mutation == 2) {
            group.access = AccessMode::Mutable;
            for (auto &dependency : group.additionalDependencies)
              dependency.access = AccessMode::Mutable;
            for (auto &fact : facts)
              fact.access = AccessMode::Mutable;
          } else {
            auto &call = *std::find_if(changed->body.blocks[block.id - 1].instructions.begin(),
                changed->body.blocks[block.id - 1].instructions.end(),
                [&](const auto &candidate) { return candidate.id == instruction.id; });
            call.borrowFields.clear();
          }
          expect(!verifyMirProgram(forged).valid(),
                 "complete replay rejects coordinated missing, collapsed, upgraded and unsummarized sources");
        }
        exercised = true;
      }
  }
  expect(exercised, "physical-reference regression exercises both independently required sources");
  return true;
}

bool checkReferencedCarrierInitializer() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "referenced-carrier-initializer.gti", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
struct Holder<lifetime a, lifetime owner> {
  View<a>&<owner> value;
  Holder(View<a>&<owner> first, View<a>&<owner> second) : value(first) {}
};
int main() {
  int first = 7; int second = 9;
  View left{first}; View right{second};
  Holder holder{left, right};
  return 0;
}
)gti");
  if (!checked.canGenerateCode()) {
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return false;
  }
  bool exercised = false;
  for (const auto &constructor : checked.mir.constructorInstances()) {
    if (constructor.parameterTypes.size() != 2 || constructor.initializers.size() != 1)
      continue;
    const auto &initial = constructor.initializers.front();
    if (!initial.completeBorrow || initial.completeBorrow->dependencies.size() != 1)
      continue;
    const auto &incoming = initial.completeBorrow->dependencies.front();
    const auto *entry = constructor.body.findLoan(incoming.parent);
    if (!entry || !entry->entry || entry->kind != MirLoanKind::Parameter ||
        !entry->storedDependency || entry->access != AccessMode::ReadOnly)
      continue;
    const MirLoan *other = nullptr;
    for (const auto &loan : constructor.body.loans)
      if (loan.entry && loan.kind == MirLoanKind::Parameter && loan.id != entry->id)
        other = &loan;
    expect(other && other->source != entry->source,
           "reference initializer controls require two independent carrier-reference inputs");
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto forged = checked.mir;
      auto *changed = const_cast<MirConstructorInstance *>(
          forged.findConstructorInstance(constructor.id));
      auto &captured = changed->initializers.front().completeBorrow->dependencies.front();
      if (mutation == 0)
        captured.parent = other->id;
      else if (mutation == 1)
        captured.parent = entry->storedDependency;
      else if (mutation == 2)
        captured.source = other->source;
      else
        changed->body.loans[entry->id - 1].access = AccessMode::Mutable;
      expect(!verifyMirProgram(forged).valid(),
             "shared formal publication preserves exact origin, physical entry and read access");
    }
    exercised = true;
  }
  expect(exercised, "reference-to-carrier initializer retains its incoming physical entry snapshot");
  return true;
}

bool checkPhysicalOwnerSuppliers() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "physical-owner-suppliers.gti", R"gti([[borrowed_from(owner)]] unsafe T& borrow<T, Owner>(const T* address, Owner& owner) { unsafe { return *address; } }
struct View<lifetime a> {
  int&<a> external; int local = 7;
  View(int&<a> source) : external(source) {}
  int& read() { unsafe { return borrow(&this.local, this); } }
};
struct Wrapper<lifetime a, lifetime owner> {
  View<a>&<owner> value;
  Wrapper(View<a>&<owner> input) : value(input) {}
  int& read() { return this.value.read(); }
};
int&<shell> read<lifetime a, lifetime shell>(View<a>&<shell> first, View<a>&<shell> second) {
  return first.read();
}
int copied<lifetime a, lifetime shell>(View<a>&<shell> first, View<a>&<shell> second) {
  return first.read();
}
int main() {
  int value = 3;
  View first{value}; View second{value};
  Wrapper wrapper{first};
  return read(first, second) + wrapper.read() + copied(first, second) - 21;
}
)gti");
  if (!checked.canGenerateCode()) {
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return false;
  }
  expect(verifyMirProgram(checked.mir).valid(),
         "owner-relative references require independently verified typed producers");
  bool rejectedMissingMapping = false;
  for (const auto &function : checked.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *loan = instruction.loan ? function.body.findLoan(*instruction.loan) : nullptr;
        const auto *parent = loan ? function.body.findLoan(loan->parent) : nullptr;
        if (instruction.kind != MirInstructionKind::Call || !instruction.functionTarget ||
            instruction.borrowFields.size() < 2 || !loan ||
            loan->kind != MirLoanKind::CallResult || !parent || parent->kind != MirLoanKind::Stored ||
            std::none_of(instruction.borrowFields.begin(), instruction.borrowFields.end(),
                         [](const auto &mapping) { return !mapping.sourceFields.empty(); }))
          continue;
        auto forged = checked.mir;
        auto *target = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
        auto &changed = *std::find_if(target->body.blocks[block.id - 1].instructions.begin(),
                                     target->body.blocks[block.id - 1].instructions.end(),
                                     [&](const auto &candidate) { return candidate.id == instruction.id; });
        changed.borrowFields.clear();
        const auto proof = verifyMirBody(forged, target->body, function.id);
        expect(!proof.valid() && std::any_of(proof.errors.begin(), proof.errors.end(), [](const auto &error) {
          return error.message.find("declared parent") != std::string::npos;
        }), "a mapped primary supplier needs its exact staged input path even beside other suppliers");
        rejectedMissingMapping = true;
      }
  expect(rejectedMissingMapping, "owner controls must reach a primary stored-reference supplier beside physical storage");
  bool sameTypeOwners = false;
  for (const auto &function : checked.mir.functionInstances()) {
    if (function.owner || function.parameterTypes.size() != 2 ||
        function.parameterTypes.front().kind != SemanticType::Reference ||
        function.parameterTypes.front() != function.parameterTypes.back() ||
        !function.completeReturnBorrow || function.completeReturnBorrow->dependencies.size() < 2)
      continue;
    const auto mapping = std::find_if(
        function.completeReturnBorrow->dependencies.begin(),
        function.completeReturnBorrow->dependencies.end(), [](const auto &source) {
          return source.origin == BorrowOriginKind::Argument && source.parameter == 0 &&
                 source.sourceFields.empty();
        });
    if (mapping == function.completeReturnBorrow->dependencies.end())
      continue;
    sameTypeOwners = true;
    for (int mutation = 0; mutation != 3; ++mutation) {
      auto forged = checked.mir;
      auto *target = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
      if (mutation == 0) {
        std::erase_if(target->completeReturnBorrow->dependencies, [](const auto &source) {
          return source.origin == BorrowOriginKind::Argument && source.parameter == 0;
        });
      } else if (mutation == 1) {
        target->returnType.referenceLifetime = Lifetime::staticLifetime();
      } else {
        target->completeReturnBorrow->dependencies.front().access = AccessMode::Mutable;
      }
      expect(!verifyMirProgram(forged).valid(),
             "owner supplier proof retains every permitted same-typed input and rejects static strengthening and exclusive access");
    }
  }
  expect(sameTypeOwners, "owner supplier controls must reach two identical checked owner inputs");
  bool rejectedOtherOwner = false;
  for (const auto &function : checked.mir.functionInstances()) {
    if (function.owner || function.returnType.kind != SemanticType::Int32 ||
        function.parameterTypes.size() != 2 || function.parameterBindings.size() != 2 ||
        function.parameterTypes.front() != function.parameterTypes.back())
      continue;
    const auto entry = [&](std::size_t index) -> const MirLoan * {
      for (const auto &loan : function.body.loans) {
        const auto *place = function.body.findPlace(loan.source);
        if (loan.entry && loan.kind == MirLoanKind::Parameter && place &&
            place->root == MirPlaceRootKind::Binding && place->projections.empty() &&
            place->binding == function.parameterBindings[index])
          return &loan;
      }
      return nullptr;
    };
    const auto *first = entry(0);
    const auto *second = entry(1);
    if (!first || !second) continue;
    for (const auto &loan : function.body.loans) {
      if (loan.kind != MirLoanKind::CallResult || loan.source != first->source ||
          loan.parent != first->id)
        continue;
      auto forged = checked.mir;
      auto *target = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
      auto &changed = target->body.loans[loan.id - 1];
      changed.source = second->source;
      changed.parent = second->id;
      changed.storedDependency = second->storedDependency;
      expect(!verifyMirProgram(forged).valid(),
             "a call result cannot replace its evaluated owner with another identically typed input");
      rejectedOtherOwner = true;
    }
  }
  expect(rejectedOtherOwner, "owner producer mutation must reach two exact checked input capabilities");
  SymbolId externalField = 0;
  for (const auto &owner : checked.mir.classInstances())
    for (const auto &field : owner.declaredFields)
      if (field.type.kind == SemanticType::Reference && field.type.arguments.size() == 1 &&
          field.type.arguments.front().kind == SemanticType::Int32)
        externalField = field.symbol;
  bool rejectedCrossing = false;
  for (const auto &function : checked.mir.functionInstances()) {
    if (!function.owner || !function.completeReturnBorrow || !externalField)
      continue;
    const auto *owner = checked.mir.findClassInstance(*function.owner);
    if (!owner) continue;
    const auto mapping = std::find_if(
        function.completeReturnBorrow->dependencies.begin(),
        function.completeReturnBorrow->dependencies.end(), [&](const auto &source) {
          return source.origin == BorrowOriginKind::Receiver && source.sourceFields.size() == 1 &&
                 std::any_of(owner->declaredFields.begin(), owner->declaredFields.end(),
                             [&](const auto &field) {
                   return field.symbol == source.sourceFields.front().field &&
                          field.type.kind == SemanticType::Reference &&
                          field.type.arguments.size() == 1 &&
                          field.type.arguments.front().kind == SemanticType::Class;
                 });
        });
    if (mapping == function.completeReturnBorrow->dependencies.end()) continue;
    const auto index = static_cast<std::size_t>(mapping - function.completeReturnBorrow->dependencies.begin());
    auto forged = checked.mir;
    auto *target = const_cast<MirFunctionInstance *>(forged.findFunctionInstance(function.id));
    target->completeReturnBorrow->dependencies[index].sourceFields.push_back(fieldProjection(externalField));
    expect(!verifyMirProgram(forged).valid(),
           "an owner-relative result cannot acquire an extra stored-reference crossing");
    rejectedCrossing = true;
  }
  expect(rejectedCrossing, "owner supplier fixture retains an exact external owner field path");
  return true;
}

void checkEvaluatedConstantIndexReturn() {
  const auto checked = Frontend().analyze("evaluated-constant-index-return.gti", R"gti(
struct Samples {
  int values[3];
  Samples(int input[3]) : values(input) {}
  int& first() { return this.values[0]; }
};
int main() { int values[3]={3,4,5}; Samples samples{values}; return samples.first()-3; }
)gti");
  if (!checked.canGenerateCode())
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(checked.canGenerateCode(),
         "a physical constant-index reference return retains its exact evaluated index and owner");
  if (!checked.canGenerateCode())
    return;
  bool observed = false;
  for (const auto &function : checked.mir.functionInstances()) {
    if (function.returnType.kind != SemanticType::Reference)
      continue;
    for (const auto &place : function.body.places) {
      if (place.root != MirPlaceRootKind::This || place.projections.empty())
        continue;
      const auto &projection = place.projections.back();
      if (projection.kind != MirProjectionKind::Index ||
          !projection.index || !projection.constantIndex)
        continue;
      const auto *value = function.body.findValue(projection.index);
      expect(value != nullptr, "constant-index return retains its SSA definition");
      if (!value)
        continue;
      for (int mutation = 0; mutation != 8; ++mutation) {
        auto forged = checked.mir;
        auto *changed = const_cast<MirFunctionInstance *>(
            forged.findFunctionInstance(function.id));
        auto *source = const_cast<MirPlace *>(changed->body.findPlace(place.id));
        auto *index = const_cast<MirValue *>(changed->body.findValue(value->id));
        auto *block = const_cast<MirBlock *>(changed->body.findBlock(value->definitionBlock));
        auto definition = std::find_if(block->instructions.begin(), block->instructions.end(),
            [&](const auto &instruction) { return instruction.id == value->definition; });
        expect(definition != block->instructions.end(), "constant index is executable");
        if (definition == block->instructions.end())
          continue;
        if (mutation == 0) source->projections.back().constantIndex = 1;
        if (mutation == 1) definition->literal = std::uint64_t{1};
        if (mutation == 2) {
          definition->operation = MirOperation::Identity;
          definition->literal.reset();
        }
        if (mutation == 3) {
          auto duplicate = *definition;
          duplicate.id = changed->body.instructionCount() + 1;
          block->instructions.push_back(std::move(duplicate));
        }
        if (mutation == 4) index->definitionBlock = 900001;
        if (mutation == 5) {
          index->info.type = SemanticType::Double;
          definition->info = index->info;
        }
        if (mutation == 6) {
          source->access = AccessMode::Mutable;
          changed->returnType.referenceAccess = AccessMode::Mutable;
          changed->body.returnType = changed->returnType;
          for (auto &loan : changed->body.loans)
            if (loan.kind == MirLoanKind::Return && loan.source == source->id)
              loan.access = AccessMode::Mutable;
          for (auto &returnBlock : changed->body.blocks)
            for (auto &instruction : returnBlock.instructions)
              if (instruction.kind == MirInstructionKind::Borrow &&
                  instruction.operands.size() == 1 &&
                  instruction.operands.front().place == source->id)
                instruction.operands.front().kind = MirOperandKind::BorrowWrite;
        }
        if (mutation == 7) source->projections.back().selection = 900002;
        const auto proof = verifyMirProgram(forged);
        if (proof.valid())
          std::cerr << "accepted constant-index mutation " << mutation << '\n';
        expect(!proof.valid(),
               "constant-index return rejects different/nonconstant/ambiguous producers, "
               "foreign definitions, wrong types, access or guard metadata");
      }
      observed = true;
      break;
    }
    if (observed) break;
  }
  expect(observed, "constant-index fixture exercises both structural and SSA index identities");
}

void checkLifetimeConvertedCarrierReturn() {
  const auto checked = Frontend().analyze("lifetime-converted-carrier-return.gti", R"gti(
struct Cell { int value; Cell(int input) : value(input) {} };
struct Slice {
  Cell& cell;
  Slice(Cell& input) : cell(input) {}
  int read() { return this.cell.value; }
};
struct Owner {
  Cell cell;
  Owner(int input) : cell(Cell(input)) {}
  Slice view() { return Slice(this.cell); }
};
int main() { Owner owner{7}; auto view = owner.view(); return view.read() - 7; }
)gti");
  if (!checked.canGenerateCode())
    for (const auto &diagnostic : checked.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(checked.canGenerateCode() && verifyMirProgram(checked.mir).valid(),
         "a carrier return preserves its applied type and proves the declared lifetime view");
  bool observed = false;
  for (const auto &function : checked.mir.functionInstances()) {
    if (function.returnType.kind != SemanticType::Class ||
        function.returnType.lifetimeArguments.size() != 1)
      continue;
    for (const auto &block : function.body.blocks) {
      if (block.terminator.kind != MirTerminatorKind::Return ||
          !block.terminator.value ||
          block.terminator.value->type == function.returnType)
        continue;
      const auto actual = block.terminator.value->type;
      expect(mirLifetimeValueConversion(checked.mir, function.body, function.id,
                                       actual, function.returnType),
             "the exact body independently proves the applied-to-declared result relation");
      auto missing = function.body;
      missing.lifetimeBody.obligations.clear();
      expect(!mirLifetimeValueConversion(checked.mir, missing, function.id,
                                        actual, function.returnType),
             "a nominal shape alone cannot replace the return lifetime relation");
      auto stronger = function.returnType;
      stronger.lifetimeArguments[0] = Lifetime::staticLifetime();
      expect(!mirLifetimeValueConversion(checked.mir, function.body, function.id,
                                        actual, stronger),
             "the checked carrier cannot acquire a static owner region");
      for (int mutation = 0; mutation != 4; ++mutation) {
        auto forged = checked.mir;
        auto *changed = const_cast<MirFunctionInstance *>(
            forged.findFunctionInstance(function.id));
        if (mutation == 0)
          changed->body.lifetimeBody.obligations.clear();
        if (mutation == 1) {
          changed->returnType = stronger;
          changed->body.returnType = stronger;
        }
        if (mutation == 2) {
          changed->returnType = stronger;
          changed->body.returnType = stronger;
          changed->body.lifetimeBody.obligations.push_back(
              {function.returnType.lifetimeArguments[0], Lifetime::staticLifetime()});
        }
        if (mutation == 3) {
          auto *returned = const_cast<MirBlock *>(changed->body.findBlock(block.id));
          returned->terminator.value->type.classId = changed->ownerType.classId;
        }
        expect(!verifyMirProgram(forged).valid(),
               "carrier return replay rejects missing/forged relations, static export and wrong nominal identity");
      }
      observed = true;
      break;
    }
    if (observed) break;
  }
  expect(observed, "carrier fixture must retain distinct applied and declared lifetime domains");
}

void checkLocalInlineStorageBounds() {
  // This source-free body deliberately exceeds the optional containment
  // budget with unrelated, independently mapped local storage origins.
  MirProgram empty;
  MirBody manyLocals;
  manyLocals.placeDomain = {1, 2, 0};
  manyLocals.lifetimeBody.complete = true;
  constexpr std::size_t localCount = 4097;
  for (std::size_t index = 1; index <= localCount; ++index) {
    MirPlace place;
    place.id = index;
    place.root = MirPlaceRootKind::Binding;
    place.binding = index;
    place.type = SemanticType::Int32;
    place.key = PlaceKey{.domain = manyLocals.placeDomain, .root = index};
    manyLocals.places.push_back(place);
    manyLocals.lifetimeBody.origins.push_back(
        {.region = Lifetime::local(index), .place = index, .key = *place.key});
  }
  LifetimeSignatureContract noRequirements{.complete = true};
  expect(verifyMirLifetimeBodyFacts(empty, manyLocals, noRequirements).valid(),
         "unrelated local origins cannot exhaust an optional storage proof when no type fact is requested");
  const auto opaque = SemanticType::typeParameter(910001);
  manyLocals.places.back().type = opaque;
  manyLocals.lifetimeBody.typeObligations = {{opaque, Lifetime::local(localCount)}};
  expect(verifyMirLifetimeBodyFacts(empty, manyLocals, noRequirements).valid(),
         "a late direct local storage fact survives every unrelated origin");
  manyLocals.lifetimeBody.typeObligations = {{opaque, Lifetime::local(1)}};
  expect(!verifyMirLifetimeBodyFacts(empty, manyLocals, noRequirements).valid(),
         "retaining direct facts does not invent a relationship to another local storage region");

  const auto frontend = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "local-inline-storage-bounds.gti", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
struct Phantom<T> {
  Phantom() = default;
  int touch() { return 0; }
};
struct ReferenceHolder<T, lifetime a> {
  T&<a> value;
  ReferenceHolder(T&<a> input) : value(input) {}
  int touch() { return 0; }
};
int inspect<lifetime a, lifetime b>(View<a>&<b> input) {
  Phantom<View<a>>* phantom = nullptr;
  ReferenceHolder<View<a>, b>* external = nullptr;
  return 0;
}
int main() { int value = 7; View view{value}; return inspect(view); }
)gti");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "local storage-bound controls must reach independently checked MIR");
  }
  bool checkedPhantom = false, checkedExternal = false;
  // The catalog is source-checked, but these are independent local type-fact
  // queries. Avoid depending on constructing or borrowing either wrapper.
  for (const auto &instance : frontend.mir.classInstances()) {
    const auto &type = instance.type;
    if (type.arguments.size() != 1) continue;
    const auto *declaration = frontend.semantics.findClassType(type.classId);
    if (!declaration || (declaration->qualifiedName != "Phantom" &&
                         declaration->qualifiedName != "ReferenceHolder")) continue;
    MirBody local;
    local.placeDomain = {1, 2, 0};
    local.lifetimeBody.complete = true;
    MirPlace place;
    place.id = 1;
    place.root = MirPlaceRootKind::Binding;
    place.binding = 1;
    place.type = type;
    place.key = PlaceKey{.domain = local.placeDomain, .root = 1};
    local.places.push_back(place);
    local.lifetimeBody.origins.push_back(
        {.region = Lifetime::local(1), .place = 1, .key = *place.key});
    LifetimeSignatureContract scope{.complete = true};
    for (const auto region : freeTypeLifetimes(type)) {
      expect(region.kind == Lifetime::Kind::Parameter,
             "the source-checked local inventory retains a canonical region scope");
      scope.parameters.push_back(region);
    }
    expect(verifyMirLifetimeBodyFacts(frontend.mir, local, scope).valid(),
           "an exact mapped origin of the source-checked wrapper is independently valid");
    local.lifetimeBody.typeObligations.push_back({type.arguments.front(), Lifetime::local(1)});
    expect(!verifyMirLifetimeBodyFacts(frontend.mir, local, scope).valid(),
           "a local object cannot prove a phantom argument or an external reference target outlives its shell");
    if (declaration->qualifiedName == "Phantom") checkedPhantom = true;
    else {
      checkedExternal = true;
      local.lifetimeBody.typeObligations = {{type, Lifetime::staticLifetime()}};
      expect(!verifyMirLifetimeBodyFacts(frontend.mir, local, scope).valid(),
             "local inline storage facts cannot be retagged as a static lifetime guarantee");
    }
  }
  expect(checkedPhantom && checkedExternal,
         "local storage bounds must exercise both non-stored arguments and stored-reference boundaries");
}

} // namespace

int main() {
  const bool physicalReferences = checkCompletePhysicalReferenceSources();
  const bool referencedInitializer = checkReferencedCarrierInitializer();
  const bool rawOwnerFacts = checkRawOwnerLifetimeFacts();
  const bool ownerSuppliers = checkPhysicalOwnerSuppliers();
  const bool referenceInitializers = checkCompleteReferenceInitializers();
  const bool consumingResults = checkConsumingExternalBorrowResults();
  expect(physicalReferences && referencedInitializer && rawOwnerFacts && ownerSuppliers && referenceInitializers && consumingResults,
         "reference publication and consuming result fixtures must both reach independently verified MIR");
  const auto frontend = Frontend({.stopAfter = FrontendPhase::Mir})
                            .analyze("lifetime-mir-contracts.gti", R"gti(
int&<a> identity<lifetime a>(int&<a> value) { return value; }
int main() {
  int value = 17;
  int& alias = identity(value);
  return identity(alias) - 17;
}
)gti");
  if (!frontend.semanticValid || !frontend.mir.valid()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto &error : verifyMirProgram(frontend.mir).errors)
      std::cerr << error.message << '\n';
    expect(
        false,
        "named lifetime calls and reference aliases must reach verified MIR");
  }
  expect(verifyMirProgram(frontend.mir).valid(),
         "independent MIR verification must accept the source fixture");
  std::optional<MirBodyAddress> selected;
  for (const auto address : enumerateMirBodyAddresses(frontend.mir)) {
    const auto *body = findMirBody(frontend.mir, address);
    if (!body)
      continue;
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == MirInstructionKind::Call &&
            instruction.functionTarget &&
            !instruction.lifetimeApplication.bindings.empty())
          selected = address;
  }
  expect(selected.has_value(),
         "fixture must retain an applied named lifetime call");
  auto missingBinding = frontend.mir;
  auto *bindingBody = findMirBody(missingBinding, *selected);
  firstLifetimeCall(*bindingBody)->lifetimeApplication.bindings.clear();
  expect(!verifyMirLifetimeApplications(missingBinding, *bindingBody,
                                        selected->owner)
              .valid(),
         "MIR must reject a named target with a missing universal binding");
  auto missingEdge = frontend.mir;
  auto *edgeBody = findMirBody(missingEdge, *selected);
  auto *call = firstLifetimeCall(*edgeBody);
  expect(!call->lifetimeApplication.obligations.empty(),
         "borrowed actual arguments must contribute region obligations");
  for (const auto bound : call->lifetimeApplication.obligations)
    std::erase(edgeBody->lifetimeBody.obligations, bound);
  call->lifetimeApplication.obligations.clear();
  expect(!verifyMirLifetimeApplications(missingEdge, *edgeBody, selected->owner)
              .valid(),
         "MIR must reconstruct actual argument edges even when both "
         "application and body records omit them");
  auto missingInventory = frontend.mir;
  auto *inventoryBody = findMirBody(missingInventory, *selected);
  inventoryBody->lifetimeBody.variables.clear();
  expect(!verifyMirLifetimeApplications(missingInventory, *inventoryBody,
                                        selected->owner)
              .valid(),
         "MIR cannot trust an inferred region number without its allocation "
         "inventory");
  const auto receiver = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "receiver-lifetime-contract.gti", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  int read() { return this.value; }
};
int inspect<lifetime a>(int&<a> input) {
  View<a> view{input};
  return view.read();
}
int main() { int value = 7; return inspect(value) - 7; }
)gti");
  if (!receiver.canGenerateCode()) {
    for (const auto &diagnostic : receiver.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    if (const auto *method = receiver.mir.findFunctionInstance(3))
      std::cerr << MirPrinter{}.print(method->body);
    expect(false, "method calls must apply the owner's lifetime universe");
  }
  for (const auto &function : receiver.mir.functionInstances())
    if (function.returnType.kind == SemanticType::Int32)
      for (const auto &block : function.body.blocks)
        if (block.terminator.kind == MirTerminatorKind::Return)
          expect(!block.terminator.returnLoan,
                 "loading a scalar through a stored reference must not export an address loan");
  auto forgedReceiver = receiver.mir;
  bool receiverMutationChecked = false;
  for (const auto &function : forgedReceiver.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (!receiverMutationChecked && instruction.kind == MirInstructionKind::Call &&
            instruction.functionTarget && instruction.receiver) {
          auto *target = const_cast<MirFunctionInstance *>(
              forgedReceiver.findFunctionInstance(*instruction.functionTarget));
          if (!target || !target->owner || target->ownerType.lifetimeArguments.empty())
            continue;
          target->ownerType.lifetimeArguments.front() = Lifetime::staticLifetime();
          const auto invalid = verifyMirLifetimeApplications(
              forgedReceiver, function.body, function.id);
          expect(!invalid.valid() && std::any_of(invalid.errors.begin(), invalid.errors.end(),
                     [](const auto &error) {
                       return error.message.find("receiver type does not prove its nested") != std::string::npos;
                     }),
                 "receiver storage validity must not authorize stronger nested owner lifetimes");
          receiverMutationChecked = true;
        }
  expect(receiverMutationChecked, "receiver fixture must contain an applied nominal owner");
  const auto published = Frontend({.stopAfter = FrontendPhase::Mir,
                                    .retainApplicationBodies = true}).analyze(
      "published-unused-lifetime-supplier.gti", R"gti(
export module published_unused_lifetime_supplier;
export int&<static> first(int&<static> left, int&<static> right) { return left; }
int main() { return 0; }
)gti");
  if (!published.canGenerateCode()) {
    for (const auto &diagnostic : published.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "an uncalled published reference contract must reach verified MIR");
  }
  const auto publishedFunction = std::find_if(
      published.mir.functionInstances().begin(), published.mir.functionInstances().end(),
      [](const auto &function) {
        return function.returnType.kind == SemanticType::Reference &&
               function.parameterTypes.size() == 2;
      });
  expect(publishedFunction != published.mir.functionInstances().end() &&
             publishedFunction->completeReturnBorrow &&
             publishedFunction->completeReturnBorrow->dependencies.size() == 2,
         "the published result retains both permitted suppliers even when only first is returned");
  for (const bool removeEntireSummary : {false, true}) {
    auto narrowed = published.mir;
    auto &target = const_cast<MirFunctionInstance &>(
        *narrowed.findFunctionInstance(publishedFunction->id));
    std::erase_if(target.completeReturnBorrow->dependencies, [](const auto &supplier) {
      return supplier.origin == BorrowOriginKind::Argument && supplier.parameter == 1;
    });
    std::erase_if(target.returnBorrowFields, [](const auto &supplier) {
      return supplier.origin == BorrowOriginKind::Argument && supplier.parameter == 1;
    });
    std::erase(target.returnBorrowParameters, std::size_t{1});
    if (removeEntireSummary) {
      target.completeReturnBorrow.reset();
      target.returnBorrowFields.clear();
      target.returnBorrowParameters.clear();
    }
    const auto invalid = verifyMirProgram(narrowed);
    expect(!invalid.valid() && std::any_of(invalid.errors.begin(), invalid.errors.end(),
               [](const auto &error) {
                 return error.message.find("complete permitted supplier inventory") != std::string::npos;
               }),
           "independent MIR proof must reject narrowing an unused declared supplier");
  }
  const auto lambda = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "lambda-lifetime-contract.gti", R"gti(
int main() {
  auto choose = [](int& left, int& right, bool first) -> int& {
    if (first) { return left; }
    return right;
  };
  int left = 5; int right = 11;
  return choose(left, right, false) - 11;
}
)gti");
  if (!lambda.canGenerateCode()) {
    for (const auto &diagnostic : lambda.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto &error : verifyMirProgram(lambda.mir).errors)
      std::cerr << error.message << '\n';
    for (const auto &closure : lambda.mir.lambdaInstances())
      std::cerr << MirPrinter{}.print(closure.body);
    expect(false, "a local lambda must preserve both inferred result suppliers in MIR");
  }
  expect(lambda.mir.lambdaInstances().size() == 1 &&
             lambda.mir.lambdaInstances().front().completeReturnBorrow &&
             lambda.mir.lambdaInstances().front().completeReturnBorrow->dependencies.size() == 2,
         "lambda result contract must contain both input suppliers");
  for (const auto &loan : lambda.mir.lambdaInstances().front().body.loans)
    expect(!(loan.entry && loan.kind == MirLoanKind::Return),
           "returning either input must preserve the reference parameter's entry loan");
  auto missingLambdaSupplier = lambda.mir;
  auto &brokenLambda = const_cast<MirLambdaInstance &>(
      missingLambdaSupplier.lambdaInstances().front());
  brokenLambda.completeReturnBorrow->dependencies.pop_back();
  expect(!verifyMirProgram(missingLambdaSupplier).valid(),
         "independent lambda proof must reject an omitted reachable supplier");
  auto missingLambdaTarget = lambda.mir;
  bool removedLambdaTarget = false;
  for (const auto address : enumerateMirBodyAddresses(missingLambdaTarget))
    if (auto *body = findMirBody(missingLambdaTarget, address))
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions)
          if (instruction.kind == MirInstructionKind::Call &&
              instruction.lambdaTarget && !instruction.borrowFields.empty()) {
            instruction.lambdaTarget.reset();
            removedLambdaTarget = true;
          }
  expect(removedLambdaTarget && !verifyMirProgram(missingLambdaTarget).valid(),
         "a complete supplier set cannot authorize a call without its typed target");
  const auto copiedCarrier = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "copied-carrier-parent.gti", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
int&<a> get<lifetime a>(View<a> view) { return view.value; }
int probe<lifetime a>(View<a> first, View<a> second) { return get(first) - 3; }
int main() {
  int left = 3; int right = 7;
  View first{left}; View second{right};
  return probe(first, second);
}
)gti");
  if (!copiedCarrier.canGenerateCode()) {
    for (const auto &diagnostic : copiedCarrier.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    if (const auto *probe = copiedCarrier.hir.findFunctionInstance(2)) {
      for (const auto &loan : probe->body.loans)
        std::cerr << "probe HIR loan=" << loan.semanticLoan << " entry=" << loan.entry
                  << " parent=" << loan.parent << " fields=" << loan.designatedFields.size()
                  << " suppliers=" << loan.additionalDependencies.size() << '\n';
      for (const auto &value : probe->body.values)
        if (value.functionTarget)
          std::cerr << "probe HIR call=" << value.id << " target=" << *value.functionTarget
                    << " origin=" << static_cast<int>(value.borrowOrigin)
                    << " fields=" << value.borrowFields.size()
                    << " complete=" << value.borrowDependencies.has_value()
                    << " dependencies=" << (value.borrowDependencies ? value.borrowDependencies->dependencies.size() : 0)
                    << '\n';
    }
    if (const auto *probe = copiedCarrier.mir.findFunctionInstance(2)) {
      for (const auto &loan : probe->body.loans)
        std::cerr << "probe MIR loan=" << loan.id << " kind=" << static_cast<int>(loan.kind)
                  << " entry=" << loan.entry << " source=" << loan.source
                  << " parent=" << loan.parent << " fields=" << loan.designatedFields.size() << '\n';
      for (const auto &block : probe->body.blocks)
        for (const auto &instruction : block.instructions) {
          std::cerr << "probe MIR instruction=" << instruction.id
                    << " kind=" << static_cast<int>(instruction.kind)
                    << " inputkind=" << static_cast<int>(instruction.callInputKind)
                    << " result=" << instruction.result.value_or(0)
                    << " site=" << instruction.callSite << '\n';
          for (const auto &operand : instruction.operands)
            std::cerr << "  operand kind=" << static_cast<int>(operand.kind)
                      << " place=" << operand.place << " value=" << operand.value
                      << " same_type=" << (operand.type == instruction.info.type) << '\n';
          if (instruction.functionTarget)
            std::cerr << "probe MIR call=" << instruction.id << " target=" << *instruction.functionTarget
                      << " loan=" << instruction.loan.value_or(0)
                      << " origin=" << static_cast<int>(instruction.borrowOrigin)
                      << " fields=" << instruction.borrowFields.size() << '\n';
        }
    }
  }
  expect(copiedCarrier.canGenerateCode(),
         "a copied readonly carrier must retain its exact stored parent authority");
  bool checkedCopiedValueRegion = false;
  for (const auto &function : copiedCarrier.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::Call ||
            instruction.operands.empty())
          continue;
        const auto signature = instantiateMirLifetimeTarget(copiedCarrier.mir, instruction);
        if (!signature || signature->parameterTypes.empty() ||
            signature->parameterTypes.front().kind != SemanticType::Class ||
            signature->parameterTypes.front().lifetimeArguments.empty() ||
            instruction.operands.front().type.kind != SemanticType::Class)
          continue;
        const auto &actual = instruction.operands.front().type;
        const auto &expected = signature->parameterTypes.front();
        expect(mirLifetimeValueConversion(copiedCarrier.mir, function.body,
                                          function.id, actual, expected),
               "a copied carrier's applied regions follow its checked body graph");
        auto stronger = expected;
        stronger.lifetimeArguments.front() = Lifetime::staticLifetime();
        expect(!mirLifetimeValueConversion(copiedCarrier.mir, function.body,
                                           function.id, actual, stronger),
               "value copying cannot manufacture a static stored reference");
        checkedCopiedValueRegion = true;
      }
  expect(checkedCopiedValueRegion,
         "copied carrier fixture must exercise an applied nominal value conversion");
  const auto localCopiedCarrier = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "local-copied-carrier-parent.gti", R"gti(
struct View<lifetime a> {
  int&<a> first; int&<a> second;
  View(int&<a> left, int&<a> right) : first(left), second(right) {}
};
int&<a> choose<lifetime a>(View<a> view, bool first) {
  if (first) { return view.first; }
  return view.second;
}
int main() {
  int left = 3; int right = 7;
  View view{left, right};
  return choose(view, false) - 7;
}
)gti");
  expect(localCopiedCarrier.canGenerateCode(),
         "a reference extracted from a local carrier copy retains its stored group");
  auto missingCopiedParent = localCopiedCarrier.mir;
  bool removedCopiedParent = false;
  for (const auto &immutable : missingCopiedParent.functionInstances()) {
    auto &function = const_cast<MirFunctionInstance &>(immutable);
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == MirInstructionKind::Call && instruction.result) {
          const auto signature = instantiateMirLifetimeTarget(localCopiedCarrier.mir,
                                                               instruction);
          if (!signature || signature->returnType.kind != SemanticType::Reference)
            continue;
          auto *value = const_cast<MirValue *>(function.body.findValue(*instruction.result));
          if (!value || !value->borrowDependencies ||
              value->borrowDependencies->dependencies.size() != 2)
            continue;
          auto &dependencies = value->borrowDependencies->dependencies;
          const auto *parent = function.body.findLoan(dependencies.front().parent);
          expect(parent && parent->kind == MirLoanKind::Stored && !parent->entry &&
                     dependencies.back().parent == parent->id,
                 "every possible result source must name the same retained local carrier");
          dependencies.back().parent = 0;
          removedCopiedParent = true;
        }
  }
  expect(removedCopiedParent,
         "copied result parent mutation must find an applied reference-returning call");
  expect(!verifyMirProgram(missingCopiedParent).valid(),
         "a copied carrier result cannot drop one retained permission parent");
  const auto retainedSelection = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "retained-selection-carrier.gti", R"gti(
int&<a> choose<lifetime a>(int&<a> left, int&<a> right, bool first) {
  if (first) { return left; }
  return right;
}
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  int&<a> get() { return this.value; }
};
int main() {
  int left = 3; int right = 9;
  int& selected = choose(left, right, false);
  View view{selected};
  return view.get() - 9;
}
)gti");
  if (!retainedSelection.canGenerateCode()) {
    for (const auto &diagnostic : retainedSelection.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto &function : retainedSelection.mir.functionInstances()) {
      std::cerr << "selection function=" << function.id << '\n';
      for (const auto &loan : function.body.loans) {
        std::cerr << " loan=" << loan.id << " kind=" << static_cast<int>(loan.kind)
                  << " source=" << loan.source << " parent=" << loan.parent
                  << " carriers=" << loan.carriers.size() << " fields=" << loan.designatedFields.size()
                  << " entry=" << loan.entry << '\n';
        for (const auto &dependency : loan.additionalDependencies)
          std::cerr << "  additional source=" << dependency.source
                    << " parent=" << dependency.parent
                    << " fields=" << dependency.designatedFields.size() << '\n';
      }
      for (const auto &place : function.body.places) {
        std::cerr << " place=" << place.id << " root=" << static_cast<int>(place.root)
                  << " binding=" << place.binding << " value=" << place.value << '\n';
        for (const auto &projection : place.projections)
          std::cerr << "  projection=" << static_cast<int>(projection.kind)
                    << " field=" << projection.field << '\n';
      }
    }
  }
  expect(retainedSelection.canGenerateCode(),
         "a stored reference must retain every supplier of an intermediate selected reference");
  bool checkedSelectedStorage = false;
  for (const auto &function : retainedSelection.mir.functionInstances())
    for (const auto &loan : function.body.loans)
      if (loan.kind == MirLoanKind::Stored && !loan.entry &&
          loan.additionalDependencies.size() == 1) {
        expect(loan.designatedFields.size() == 1 &&
                   loan.additionalDependencies.front().designatedFields == loan.designatedFields,
               "both selected origins must be published at the constructor's exact field");
        checkedSelectedStorage = true;
      }
  expect(checkedSelectedStorage, "selected reference fixture must publish a composed stored loan");
  auto selfOrigin = retainedSelection.mir;
  bool changedProducerParent = false;
  for (const auto &immutable : selfOrigin.functionInstances()) {
    auto &function = const_cast<MirFunctionInstance &>(immutable);
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == MirInstructionKind::Construct &&
            instruction.loan && instruction.result) {
          auto *value = const_cast<MirValue *>(function.body.findValue(*instruction.result));
          if (!value || !value->borrowDependencies || value->borrowDependencies->dependencies.empty())
            continue;
          expect(std::none_of(value->borrowDependencies->dependencies.begin(),
                              value->borrowDependencies->dependencies.end(),
                              [&](const auto &dependency) { return dependency.parent == *instruction.loan; }),
                 "a constructor's source dependency set cannot name its newly published group as an origin");
          value->borrowDependencies->dependencies.front().parent = *instruction.loan;
          changedProducerParent = true;
        }
  }
  expect(changedProducerParent && !verifyMirProgram(selfOrigin).valid(),
         "the constructor producer cannot replace an external source parent with its own stored group");
  for (const bool removeSupplier : {false, true}) {
    auto brokenSelection = retainedSelection.mir;
    bool changed = false;
    for (const auto &immutable : brokenSelection.functionInstances()) {
      auto &function = const_cast<MirFunctionInstance &>(immutable);
      for (auto &loan : function.body.loans)
        if (loan.kind == MirLoanKind::Stored && !loan.entry &&
            loan.additionalDependencies.size() == 1) {
          if (removeSupplier)
            loan.additionalDependencies.clear();
          else
            loan.designatedFields.clear();
          changed = true;
        }
    }
    expect(changed && !verifyMirProgram(brokenSelection).valid(),
           "a stored selection cannot lose its field mapping or one permitted supplier");
  }
  const auto retainedSelectionPair = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "retained-selection-pair.gti", R"gti(
int&<a> choose<lifetime a>(int&<a> left, int&<a> right, bool first) {
  if (first) { return left; }
  return right;
}
struct Pair<lifetime a> {
  int&<a> first; int&<a> second;
  Pair(int&<a> left, int&<a> right) : first(left), second(right) {}
};
int main() {
  int left = 3; int right = 9;
  int& selected = choose(left, right, false);
  Pair pair{selected, selected};
  return pair.first + pair.second - 18;
}
)gti");
  expect(retainedSelectionPair.canGenerateCode(),
         "each constructed field must preserve every supplier of its selected input");
  bool checkedSelectedPair = false;
  for (const auto &function : retainedSelectionPair.mir.functionInstances())
    for (const auto &loan : function.body.loans)
      if (loan.kind == MirLoanKind::Stored && !loan.entry &&
          loan.additionalDependencies.size() == 3) {
        const auto firstField = loan.designatedFields;
        expect(firstField.size() == 1,
               "the first selected supplier must identify its result field");
        std::size_t sameField = 1;
        for (const auto &dependency : loan.additionalDependencies)
          if (dependency.designatedFields == firstField)
            ++sameField;
        expect(sameField == 2,
               "two possible origins must be retained separately for each of two fields");
        checkedSelectedPair = true;
      }
  expect(checkedSelectedPair,
         "the two-field fixture must publish all four field/source alternatives");
  auto wrongCopiedCarrier = copiedCarrier.mir;
  bool changedCopySource = false;
  for (const auto &immutable : wrongCopiedCarrier.functionInstances()) {
    if (immutable.parameterTypes.size() != 2 || immutable.parameterBindings.size() != 2 ||
        immutable.parameterTypes[0].kind != SemanticType::Class ||
        immutable.parameterTypes[0] != immutable.parameterTypes[1])
      continue;
    auto &function = const_cast<MirFunctionInstance &>(immutable);
    const auto other = std::find_if(function.body.places.begin(), function.body.places.end(),
        [&](const auto &place) {
          return place.root == MirPlaceRootKind::Binding &&
                 place.binding == function.parameterBindings[1] && place.projections.empty() &&
                 place.type == function.parameterTypes[1];
        });
    if (other == function.body.places.end()) continue;
    for (auto &block : function.body.blocks)
      for (auto &instruction : block.instructions) {
        const bool sourceCopy = instruction.kind == MirInstructionKind::Load ||
            (instruction.kind == MirInstructionKind::CallInput &&
             instruction.callInputRole == MirCallInputRole::Argument &&
             instruction.callInputIndex == 0 &&
             (instruction.callInputKind == HirCallInputKind::Value ||
              instruction.callInputKind == HirCallInputKind::CopyValue));
        if (!sourceCopy || instruction.operands.size() != 1 ||
            instruction.operands.front().kind != MirOperandKind::Copy)
          continue;
        auto &operand = instruction.operands.front();
        const auto *source = function.body.findPlace(operand.place);
        if (!source || source->root != MirPlaceRootKind::Binding ||
            source->binding != function.parameterBindings[0] || !source->projections.empty() ||
            source->type != other->type)
          continue;
        operand.place = other->id;
        changedCopySource = true;
      }
  }
  expect(changedCopySource,
         "copied carrier mutation must replace the evaluated source in a load or input stage");
  expect(!verifyMirProgram(wrongCopiedCarrier).valid(),
         "a same-type copy of another carrier cannot use the original carrier's parent loan");
  auto missingStoredAddress = copiedCarrier.mir;
  bool removedStoredAddress = false;
  for (const auto address : enumerateMirBodyAddresses(missingStoredAddress))
    if (auto *body = findMirBody(missingStoredAddress, address))
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions) {
          if (instruction.kind != MirInstructionKind::Borrow || !instruction.loan ||
              instruction.operands.size() != 1)
            continue;
          auto &operand = instruction.operands.front();
          const auto *published = body->findLoan(*instruction.loan);
          const auto *entry = body->findLoan(operand.loan);
          if (published && published->kind == MirLoanKind::Return && entry && entry->entry &&
              entry->kind == MirLoanKind::Stored && operand.kind == MirOperandKind::Loan && operand.place) {
            operand.place = 0;
            removedStoredAddress = true;
          }
        }
  expect(removedStoredAddress && !verifyMirProgram(missingStoredAddress).valid(),
         "stored-reference publication must preserve the exact evaluated field address");
  const auto laterStoredField = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "later-stored-field-return.gti", R"gti(
struct Pair<lifetime a> {
  int&<a> left;
  int&<a> right;
  Pair(int&<a> first, int&<a> second) : left(first), right(second) {}
};
int&<a> choose<lifetime a>(Pair<a> pair, bool left) {
  if (left) { return pair.left; }
  return pair.right;
}
int main() { int left = 3; int right = 7; Pair pair{left, right}; return choose(pair, false) - 7; }
)gti");
  if (!laterStoredField.canGenerateCode())
    for (const auto &diagnostic : laterStoredField.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(laterStoredField.canGenerateCode(),
         "later abstract entry fields must publish their exact returned address");
  auto missingLaterField = laterStoredField.mir;
  bool removedLaterField = false;
  for (const auto address : enumerateMirBodyAddresses(missingLaterField))
    if (auto *body = findMirBody(missingLaterField, address))
      for (auto &loan : body->loans)
        if (loan.entry && loan.kind == MirLoanKind::Stored &&
            !loan.additionalDependencies.empty()) {
          loan.additionalDependencies.front().designatedFields.clear();
          removedLaterField = true;
        }
  expect(removedLaterField && !verifyMirProgram(missingLaterField).valid(),
         "later field publication cannot invent an absent abstract entry path");
  const auto constructedCarrier = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "constructed-stored-reference-publication.gti", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  int&<a> get() { return this.value; }
};
int&<a> use<lifetime a>(int&<a> input, int&<a> other, bool choose) {
  View<a> view{input};
  if (choose) { return other; }
  return view.get();
}
int main() { int first = 7; int second = 9; return use(first, second, false) - 7; }
)gti");
  if (!constructedCarrier.canGenerateCode())
    for (const auto &diagnostic : constructedCarrier.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(constructedCarrier.canGenerateCode(),
         "a locally constructed carrier must preserve its shared formal's complete authority");
  std::optional<MirBodyAddress> constructionBody;
  MirLoanId constructionLoan = 0;
  MirLoanId incomingLoan = 0;
  MirPlaceId otherFormal = 0;
  for (const auto &function : constructedCarrier.mir.functionInstances()) {
    if (function.parameterBindings.size() != 3 ||
        function.parameterTypes[0].kind != SemanticType::Reference ||
        function.parameterTypes[0] != function.parameterTypes[1])
      continue;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::Construct || !instruction.loan)
          continue;
        const auto *stored = function.body.findLoan(*instruction.loan);
        if (!stored || stored->kind != MirLoanKind::Stored || stored->entry)
          continue;
        const auto fromInput = [&](MirPlaceId id) {
          const auto *source = function.body.findPlace(id);
          return source && source->root == MirPlaceRootKind::Binding &&
                 source->binding == function.parameterBindings.front() &&
                 std::all_of(source->projections.begin(), source->projections.end(),
                             [](const auto &projection) {
                               return projection.kind == MirProjectionKind::Dereference;
                             });
        };
        if (!fromInput(stored->source))
          continue;
        const auto *source = function.body.findPlace(stored->source);
        const auto other = std::find_if(function.body.places.begin(), function.body.places.end(),
            [&](const auto &place) {
              return place.root == MirPlaceRootKind::Binding &&
                     place.binding == function.parameterBindings[1] &&
                     place.projections == source->projections && place.type == source->type;
            });
        if (other == function.body.places.end())
          continue;
        const auto entry = std::find_if(function.body.loans.begin(), function.body.loans.end(),
            [&](const auto &loan) {
              return loan.entry && loan.kind == MirLoanKind::Parameter &&
                     fromInput(loan.source) && loan.access == AccessMode::ReadOnly;
            });
        if (entry == function.body.loans.end())
          continue;
        constructionBody = MirBodyAddress{MirBodyKind::Function, function.id};
        constructionLoan = stored->id;
        incomingLoan = entry->id;
        otherFormal = other->id;
      }
  }
  if (!constructionBody)
    for (const auto &function : constructedCarrier.mir.functionInstances()) {
      std::cerr << "constructor source fixture function=" << function.id
                << " parameters=" << function.parameterBindings.size() << '\n';
      for (const auto &loan : function.body.loans) {
        const auto *source = function.body.findPlace(loan.source);
        std::cerr << "  loan=" << loan.id << " kind=" << static_cast<int>(loan.kind)
                  << " parent=" << loan.parent << " entry=" << loan.entry
                  << " source=" << loan.source << " binding="
                  << (source ? source->binding : 0) << '\n';
      }
    }
  expect(constructionBody && constructionLoan && incomingLoan && otherFormal,
         "fixture must exercise a fresh Stored group from a shared reference entry");
  bool constructorMutationsRejected = true;
  for (int mutation = 0; mutation != 4; ++mutation) {
    auto invalid = constructedCarrier.mir;
    auto *body = findMirBody(invalid, *constructionBody);
    auto &stored = body->loans[constructionLoan - 1];
    auto &entry = body->loans[incomingLoan - 1];
    if (mutation == 0)
      stored.source = otherFormal;
    else if (mutation == 1)
      entry.entry = false;
    else {
      entry.access = AccessMode::Mutable;
      if (mutation == 3)
        body->places[entry.source - 1].type.referenceAccess = AccessMode::Mutable;
    }
    const auto rejected = verifyMirProgram(invalid);
    if (mutation == 2)
      expect(!rejected.valid() && std::any_of(rejected.errors.begin(), rejected.errors.end(),
                 [](const auto &error) {
                   return error.message.find("formal reference access") != std::string::npos;
                 }),
             "whole reference entry loans must retain their formal's exact access");
    if (rejected.valid())
      std::cerr << "accepted constructor publication mutation=" << mutation
                << " body=" << constructionBody->owner << " stored=" << constructionLoan
                << " input=" << incomingLoan << " other=" << otherFormal << '\n';
    constructorMutationsRejected = !rejected.valid() && constructorMutationsRejected;
  }
  expect(constructorMutationsRejected,
         "constructor publication cannot forge its source or erase a local/exclusive parent");
  const auto physical = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "physical-payload-contract.gti", R"gti(
enum State<T> { value(T item) };
T&<a> get<T, lifetime a>(State<T>&<a> state) {
  switch (state) { case State<T>::value(auto& payload): return payload; }
}
int main() { State<int> state = State<int>::value(7); return get(state) - 7; }
)gti");
  if (!physical.canGenerateCode()) {
    for (const auto &diagnostic : physical.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "physical payload reference must reach independently verified MIR");
  }
  std::optional<MirBodyAddress> physicalBody;
  MirLoanId physicalLoan = 0;
  for (const auto address : enumerateMirBodyAddresses(physical.mir))
    if (const auto *body = findMirBody(physical.mir, address))
      for (const auto &loan : body->loans)
        if (loan.referenceSelection && loan.referenceSelection->kind ==
                                           ReferenceSelectionKind::PhysicalPayload) {
          physicalBody = address;
          physicalLoan = loan.id;
        }
  expect(physicalBody && physicalLoan, "physical payload must preserve its selection role");
  for (int mutation = 0; mutation != 4; ++mutation) {
    auto invalid = physical.mir;
    auto *body = findMirBody(invalid, *physicalBody);
    auto &loan = body->loans[physicalLoan - 1];
    auto &selection = *loan.referenceSelection;
    if (mutation == 0)
      selection.kind = ReferenceSelectionKind::StoredExternalReference;
    else if (mutation == 1)
      selection.fields.front() = fieldProjection(0);
    else if (mutation == 2)
      selection.transferredFrom = loan.id;
    else
      loan.source = selection.carrier;
    expect(!verifyMirProgram(invalid).valid(),
           "physical payload cannot forge external authority, selector, transfer or storage");
  }
  if (physicalBody && physicalLoan) {
    auto missingPatternRegion = physical.mir;
    auto *body = findMirBody(missingPatternRegion, *physicalBody);
    const auto *loan = body->findLoan(physicalLoan);
    const auto region = loan->referenceSelection->reference.referenceLifetime;
    expect(region.kind == Lifetime::Kind::Inference,
           "physical reference patterns form an inferred binding region");
    const auto removed = std::erase_if(body->lifetimeBody.obligations,
                                      [&](const auto &edge) { return edge.shorter == region; });
    expect(removed != 0, "the payload binding must prove its source-to-binding region");
    expect(!verifyMirProgram(missingPatternRegion).valid(),
           "MIR cannot trust a payload reference without its binding lifetime relationship");
  }
  const auto projectedPayload = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "physical-payload-stored-field.gti", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
enum Box<T> { item(T value) };
int read<lifetime a>(Box<View<a>>& box) {
  switch (box) { case Box<View<a>>::item(auto& payload): return payload.value; }
}
int main() {
  int number = 7;
  View view{number};
  auto box = Box<View<_>>::item(view);
  return read(box) - 7;
}
)gti");
  expect(projectedPayload.canGenerateCode(),
         "a physical payload's stored reference field uses its exact ordinary subobject path");
  bool mutatedProjectedPayload = false;
  auto fakePhysicalOrigins = projectedPayload.mir;
  for (const auto address : enumerateMirBodyAddresses(fakePhysicalOrigins))
    if (auto *body = findMirBody(fakePhysicalOrigins, address))
      for (auto &loan : body->loans)
        if (loan.referenceSelection && loan.referenceSelection->kind ==
                                           ReferenceSelectionKind::PhysicalPayload) {
          loan.additionalDependencies.push_back(
              {.source = loan.source, .access = loan.access});
          mutatedProjectedPayload = true;
        }
  expect(mutatedProjectedPayload && !verifyMirProgram(fakePhysicalOrigins).valid(),
         "a physical payload cannot hide multiple external origins behind its selection role");
  const auto standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  // These tests exercise compiler-private capability protocols themselves.
  // Give only these fixtures explicit trusted source identity; application
  // acceptance is covered separately by the public native programs.
  const auto trustedCapability = [&](const char *name, const std::string &source,
                                    const std::vector<PackageSourceRoot> &modules = {}) {
    const auto path = std::filesystem::weakly_canonical(standardLibrary / name);
    return Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
        path, source, {standardLibrary / "prelude.gti", path},
        {{path.string(), source}}, {standardLibrary}, modules);
  };
  const auto native = trustedCapability("lifetime-native-callable.gti", R"gti(
using Count = (std::string_view) -> int32_t;
int32_t count<lifetime a>(std::string_view<a> value) { return 7; }
int32_t invoke<lifetime a>(Count callback, std::string_view<a> value) {
  unsafe { return callback(value); }
}
int main() { Count callback = count; return invoke(callback, "text") - 7; }
)gti");
  if (!native.canGenerateCode())
    for (const auto &diagnostic : native.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(native.canGenerateCode(), "native borrowed inputs retain invocation scopes through verified MIR");
  struct NativeSite { MirBodyAddress body; MirInstructionId instruction; bool forming; };
  std::vector<NativeSite> nativeSites;
  bool nativeForming = false;
  bool nativeInvoke = false;
  for (const auto address : enumerateMirBodyAddresses(native.mir))
    if (const auto *body = findMirBody(native.mir, address))
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.operation == MirOperation::NativeCallback || instruction.nativeCalleeType) {
            const bool forming = instruction.operation == MirOperation::NativeCallback;
            const auto *signature = checkedMirCallableLifetimeSignature(native.mir, instruction);
            expect(signature && signature->signature.hasNativeFunctionShape() &&
                       !signature->signature.quantifiedLifetimes.empty(),
                   "native forming and invoking preserve the actual quantified carrier");
            nativeForming |= forming;
            nativeInvoke |= !forming;
            nativeSites.push_back({address, instruction.id, forming});
          }
  expect(nativeForming && nativeInvoke, "native fixture reaches both source-free boundaries");
  for (const auto site : nativeSites)
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto broken = native.mir;
      auto *body = findMirBody(broken, site.body);
      MirInstruction *selectedNative = nullptr;
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions)
          if (instruction.id == site.instruction)
            selectedNative = &instruction;
      auto &application = selectedNative->lifetimeApplication;
      switch (mutation) {
      case 0: application.callableSignature.reset(); break;
      case 1: application.callableSignature->signature.quantifiedLifetimes.clear(); break;
      case 2:
        if (site.forming) {
          auto &target = const_cast<MirFunctionInstance &>(*broken.findFunctionInstance(
              broken.findNativeCallbackAdapter(*selectedNative->nativeCallbackAdapter)->target));
          target.parameterTypes.front().lifetimeArguments = {Lifetime::staticLifetime()};
        } else {
          application.bindings.clear();
        }
        break;
      case 3: application.complete = false; break;
      }
      expect(!verifyMirLifetimeApplications(broken, *body, site.body.owner).valid(),
             "native MIR rejects missing scope, universal binding, proof or a stronger callback target");
    }
  const auto panic = trustedCapability("lifetime-panic-contract.gti", R"gti(
void report_failure(std::string_view message) { gti_internal::panic(message); }
int main() { return 0; }
)gti");
  if (!panic.canGenerateCode()) {
    for (const auto &diagnostic : panic.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(panic.canGenerateCode(),
         "terminal failure must preserve the message storage lifetime");
  std::optional<MirBodyAddress> panicBody;
  MirInstructionId panicCall = 0;
  for (const auto address : enumerateMirBodyAddresses(panic.mir))
    if (const auto *body = findMirBody(panic.mir, address))
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.intrinsic == IntrinsicKind::Panic &&
              instruction.parameterTypes.size() == 1 &&
              instruction.parameterTypes.front().lifetimeArguments.size() == 1 &&
              instruction.parameterTypes.front().lifetimeArguments.front().specified()) {
            panicBody = address;
            panicCall = instruction.id;
          }
  expect(panicBody.has_value(), "panic regression must exercise a region-bearing message");
  for (int mutation = 0; mutation != 2; ++mutation) {
    auto broken = panic.mir;
    auto *body = findMirBody(broken, *panicBody);
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == panicCall) {
          if (mutation == 0)
            instruction.parameterTypes.front().lifetimeArguments.clear();
          else {
            instruction.parameterTypes.front().arguments.push_back(SemanticType::Int32);
            instruction.operands.front().type = instruction.parameterTypes.front();
          }
        }
    expect(!verifyMirProgram(broken).valid(),
           "panic cannot discard a message region or forge non-region type facts");
  }
  const PackageSourceRoot pointerModules{
      .identity = "toolchain:std", .name = "std",
      .packageRoot = standardLibrary, .sourceRoot = standardLibrary,
      .modules = {{"meta", standardLibrary / "std/meta.gti"},
                  {"detail_function_pointer_impl", standardLibrary / "std/detail/function_pointer.gti"}},
      .kind = SourceOwnerKind::Toolchain, .implicitAlias = "std",
      .defaultModule = "meta"};
  const auto pointer = trustedCapability("lifetime-pointer-contracts.gti", R"gti(
import std.detail_function_pointer_impl;
using Selection = for<lifetime region> int&<region>(int&<region>, int&<region>, bool);
int&<shorter> select<lifetime longer, lifetime shorter>(
    int&<longer> left, int&<shorter> right, bool first)
  requires outlives<longer, shorter> {
  if (first) { return left; }
  return right;
}
int main() {
  auto pointer = gti_internal::function_pointer_from_item<Selection>(select);
  int left = 5;
  int right = 11;
  int& selected = gti_internal::function_pointer_invoke(pointer, left, right, false);
  return selected - 11;
}
)gti", {pointerModules});
  if (!pointer.semanticValid || !pointer.mir.valid()) {
    for (const auto &diagnostic : pointer.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto &error : verifyMirProgram(pointer.mir).errors)
      std::cerr << error.message << '\n';
    expect(false, "a higher-ranked invocation pointer must preserve both permitted suppliers");
  }
  std::optional<MirBodyAddress> pointerBody;
  MirInstructionId invocation = 0;
  for (const auto address : enumerateMirBodyAddresses(pointer.mir))
    if (const auto *body = findMirBody(pointer.mir, address))
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.intrinsic == IntrinsicKind::FunctionPointerInvoke) {
            pointerBody = address;
            invocation = instruction.id;
            expect(instruction.lifetimeApplication.callableSignature.has_value() &&
                       instruction.borrowArguments.size() == 2,
                   "dynamic invocation must retain its signature and conservative suppliers");
          }
  expect(pointerBody.has_value(), "fixture must contain a dynamic invocation");
  auto corruptPointer = pointer.mir;
  auto *dynamicBody = findMirBody(corruptPointer, *pointerBody);
  for (auto &block : dynamicBody->blocks)
    for (auto &instruction : block.instructions)
      if (instruction.id == invocation)
        instruction.lifetimeApplication.callableSignature->contract.typeBounds.clear();
  expect(!verifyMirProgram(corruptPointer).valid(),
         "MIR must derive callable obligations independently from the trusted carrier");
  for (const auto qualifier : {ReceiverMutability::Mutable, ReceiverMutability::Consuming}) {
    bool checkedForming = false;
    bool checkedInvocation = false;
    for (const auto address : enumerateMirBodyAddresses(pointer.mir))
      if (const auto *body = findMirBody(pointer.mir, address))
        for (const auto &block : body->blocks)
          for (const auto &original : block.instructions) {
            const bool forming = original.intrinsic == IntrinsicKind::FunctionPointerFromItem;
            if (!forming && original.intrinsic != IntrinsicKind::FunctionPointerInvoke)
              continue;
            auto forged = pointer.mir;
            for (const auto &immutable : forged.classInstances())
              if (immutable.compilerCapability == CompilerCapabilityTypeKind::FunctionPointer)
                const_cast<MirClassInstance &>(immutable).type.arguments.front().signatureReceiver = qualifier;
            auto instruction = original;
            auto &carrier = forming ? instruction.info.type
                                    : instruction.parameterTypes.front().arguments.front();
            carrier.arguments.front().signatureReceiver = qualifier;
            auto &callable = *instruction.lifetimeApplication.callableSignature;
            callable.signature.signatureReceiver = qualifier;
            expect(mirClassLifetimeView(forged, carrier).has_value() &&
                       callableLifetimeCarrierMatches(callable, carrier.arguments.front()),
                   "qualifier forgery remains a well-formed exact carrier for the pointer-policy control");
            expect(checkedMirCallableLifetimeSignature(forged, instruction) == nullptr,
                   "MIR pointer forming and invocation reject mutable or consuming interface forgery");
            checkedForming |= forming;
            checkedInvocation |= !forming;
          }
    expect(checkedForming && checkedInvocation,
           "pointer qualifier policy is checked independently at forming and invocation");
  }
  const auto nominalPointer = trustedCapability("lifetime-pointer-nominal.gti", R"gti(
import std.detail_function_pointer_impl;
struct Holder<T, lifetime a> {
  T&<a> value;
  Holder(T&<a> input) : value(input) {}
};
using Read = for<lifetime a> int(Holder<int, a>&<a>);
int read<lifetime a>(Holder<int, a>&<a> holder) { return holder.value; }
int main() {
  auto pointer = gti_internal::function_pointer_from_item<Read>(read);
  int number = 7;
  Holder<int, _> holder{number};
  return gti_internal::function_pointer_invoke(pointer, holder) - 7;
}
)gti", {pointerModules});
  if (!nominalPointer.canGenerateCode())
    for (const auto &diagnostic : nominalPointer.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(nominalPointer.canGenerateCode(),
         "higher-ranked nominal input formation must reach independently verified MIR");
  for (int mutation = 0; mutation != 3; ++mutation) {
    auto forged = nominalPointer.mir;
    bool changedArgument = false;
    bool rejectedArgument = false;
    for (const auto address : enumerateMirBodyAddresses(forged))
      if (auto *body = findMirBody(forged, address))
        for (auto &block : body->blocks)
          for (auto &instruction : block.instructions) {
            if (instruction.intrinsic != IntrinsicKind::FunctionPointerInvoke ||
                instruction.parameterTypes.size() != 2 || instruction.operands.size() != 2)
              continue;
            auto &parameter = instruction.parameterTypes.back();
            if (parameter.kind != SemanticType::Reference || parameter.arguments.size() != 1 ||
                parameter.arguments.front().kind != SemanticType::Class ||
                parameter.arguments.front().lifetimeArguments.size() != 1)
              continue;
            expect(verifyMirLifetimeApplications(forged, *body, address.owner).valid(),
                   "nominal pointer arguments have independent source and region proof");
            if (mutation == 0) {
              body->lifetimeBody.obligations.clear();
              for (auto &currentBlock : body->blocks)
                for (auto &current : currentBlock.instructions)
                  current.lifetimeApplication.obligations.clear();
            } else if (mutation == 1) {
              parameter.arguments.front().lifetimeArguments.front() = Lifetime::parameter(UINT64_MAX);
              instruction.operands.back().type = parameter;
            } else {
              parameter.referenceAccess = AccessMode::Mutable;
              instruction.operands.back().type = parameter;
            }
            changedArgument = true;
            rejectedArgument = !verifyMirLifetimeApplications(forged, *body, address.owner).valid();
          }
    expect(changedArgument && rejectedArgument && !verifyMirProgram(forged).valid(),
           "direct nominal arguments cannot bypass missing edges, region scope or exact parameter access");
  }
  auto omittedNominalRequirement = nominalPointer.mir;
  bool rejectedNominalRequirement = false;
  for (const auto address : enumerateMirBodyAddresses(omittedNominalRequirement))
    if (auto *body = findMirBody(omittedNominalRequirement, address))
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions)
          if (instruction.intrinsic == IntrinsicKind::FunctionPointerInvoke &&
              instruction.lifetimeApplication.callableSignature) {
            auto &contract = instruction.lifetimeApplication.callableSignature->contract;
            expect(!contract.requirements.nodes.empty(),
                   "nominal callable formation must retain its exact input requirement formula");
            contract.requirements = {};
            rejectedNominalRequirement =
                checkedMirCallableLifetimeSignature(omittedNominalRequirement, instruction) == nullptr;
          }
  expect(rejectedNominalRequirement && !verifyMirProgram(omittedNominalRequirement).valid(),
         "dynamic callable metadata cannot omit its nominal formation obligations");
  auto foreignNominalRegion = nominalPointer.mir;
  bool changedNominalRegion = false;
  for (const auto &immutable : foreignNominalRegion.classInstances())
    if (!immutable.lifetimeContract.typeBounds.empty()) {
      auto &instance = const_cast<MirClassInstance &>(immutable);
      instance.lifetimeContract.typeBounds.front().region = Lifetime::parameter(UINT64_MAX);
      changedNominalRegion = true;
      break;
    }
  expect(changedNominalRegion && !verifyMirProgram(foreignNominalRegion).valid(),
         "a nominal formation contract cannot supply a foreign scoped region");
  const auto rejectedPath = std::filesystem::weakly_canonical(
      standardLibrary / "lifetime-pointer-requires.gti");
  const std::string rejectedSource = R"gti(
import std.detail_function_pointer_impl;
using Independent = for<lifetime x, lifetime y> int&<y>(int&<x>, int&<y>, bool);
int&<shorter> constrained<lifetime longer, lifetime shorter>(
    int&<longer> left, int&<shorter> right, bool first)
  requires outlives<longer, shorter> {
  if (first) { return left; }
  return right;
}
int main() {
  auto pointer = gti_internal::function_pointer_from_item<Independent>(constrained);
  return 0;
}
)gti";
  const auto rejected = trustedCapability("lifetime-pointer-requires.gti", rejectedSource,
                                          {pointerModules});
  const auto failure = std::find_if(rejected.diagnostics.begin(), rejected.diagnostics.end(),
      [](const auto &diagnostic) { return diagnostic.code == "GTI-S2093"; });
  const auto targetOffset = rejectedSource.find(">(constrained)") + 2;
  expect(!rejected.semanticValid && !rejected.canGenerateCode() &&
             failure != rejected.diagnostics.end() &&
             failure->message.find("exact-signature") != std::string::npos &&
             failure->primary.source == rejectedPath.string() &&
             failure->primary.start == targetOffset &&
             failure->primary.end == targetOffset + std::string("constrained").size() &&
             failure->fixes.empty(),
         "an invocation pointer cannot discard a target outlives requirement or invent a repair");
  checkScopedCallableFieldTransport();
  checkClosedProjectedDefaultContract();
  checkReferenceContentsOpeningShape();
  checkReferenceReceiverProducer();
  checkLocalInlineStorageBounds();
  checkEvaluatedConstantIndexReturn();
  checkLifetimeConvertedCarrierReturn();
  checkReferenceErasureLifetimeAuthority();
  checkReferenceErasureAddressIdentity();
  checkCarrierReferenceShellInputs();
  checkScopedReceiverOwner();
  checkAppliedBorrowFieldSummary();
  checkScopedFunctionItemIdentity();
  checkStatelessItemTemporaryBorrow();
  checkFreshInputReferenceResult();
  checkScalarLoadDependencyDomain();
  checkForwardedCarrierPackLifetimeStage();
  checkPatternedResultScope();
  checkProgramBasePathAuthority();
  checkNestedBaseLifetimeViews();
  checkLocalStoredReferencePublication();
  checkFailedBasePublicationRelease();
  checkBorrowedBasePublication();
  std::cout << "lifetime MIR contracts passed\n";
}
