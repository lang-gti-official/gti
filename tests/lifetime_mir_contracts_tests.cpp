#include "../src/compiler/mir_lifetimes.h"
#include "../src/compiler/mir_verification_queries.h"
#include "gti/frontend.h"
#include "gti/callable_lifetimes.h"
#include "gti/mir_verifier.h"
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
          if (mutation == 2) descriptor.lifetimeArguments.push_back(Lifetime::staticLifetime());
          expect(!verifyMirProgram(broken).valid(),
                 "callable descriptors independently reject missing application, retagged input and invented explicit region arguments");
        }
      }
  expect(appliedDescriptor, "item fixture publishes a concrete callable invocation lifetime application");
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
        for (int mutation = 0; mutation != 3; ++mutation) {
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
          expect(!verifyMirProgram(forged).valid(),
                 "pack stages preserve exact lane, source region and stored external authority");
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
int main() {
  mut int number = 3;
  unsafe { restore_mut<int>(erase_mut(number)) = 7; }
  Borrowed view{number};
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
}

bool checkRawOwnerLifetimeFacts() {
  const auto checked = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      "raw-owner-lifetime-facts.gti", R"gti([[borrowed_from(owner)]] unsafe T& borrow<T, Owner>(const T* address, Owner& owner) { unsafe { return *address; } }
[[borrowed_from(owner)]] unsafe mut T& borrow_mut<T, Owner>(T* address, mut Owner& owner) { unsafe { return *address; } }
struct Storage { mut int value = 7; Storage() {} };
int rewrite(mut int& value) { value = 5; return value; }
int main() {
  mut Storage storage{};
  rewrite(storage.value);
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

} // namespace

int main() {
  const bool rawOwnerFacts = checkRawOwnerLifetimeFacts();
  const bool ownerSuppliers = checkPhysicalOwnerSuppliers();
  const bool referenceInitializers = checkCompleteReferenceInitializers();
  const bool consumingResults = checkConsumingExternalBorrowResults();
  expect(rawOwnerFacts && ownerSuppliers && referenceInitializers && consumingResults,
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
  checkReferenceErasureLifetimeAuthority();
  checkScopedFunctionItemIdentity();
  checkScalarLoadDependencyDomain();
  checkForwardedCarrierPackLifetimeStage();
  checkPatternedResultScope();
  checkProgramBasePathAuthority();
  checkNestedBaseLifetimeViews();
  checkBorrowedBasePublication();
  std::cout << "lifetime MIR contracts passed\n";
}
