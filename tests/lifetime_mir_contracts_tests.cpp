#include "../src/compiler/mir_lifetimes.h"
#include "gti/frontend.h"
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
} // namespace

int main() {
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
        if (instruction.kind != MirInstructionKind::Load || instruction.operands.size() != 1 ||
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
  expect(changedCopySource && !verifyMirProgram(wrongCopiedCarrier).valid(),
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
    const auto other = std::find_if(function.body.places.begin(), function.body.places.end(),
        [&](const auto &place) {
          return place.root == MirPlaceRootKind::Binding && place.projections.empty() &&
                 place.binding == function.parameterBindings[1] &&
                 place.type == function.parameterTypes[1];
        });
    if (other == function.body.places.end())
      continue;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::Construct || !instruction.loan)
          continue;
        const auto *stored = function.body.findLoan(*instruction.loan);
        if (!stored || stored->kind != MirLoanKind::Stored || stored->entry || stored->parent)
          continue;
        const auto entry = std::find_if(function.body.loans.begin(), function.body.loans.end(),
            [&](const auto &loan) {
              return loan.entry && loan.kind == MirLoanKind::Parameter &&
                     loan.source == stored->source && loan.access == AccessMode::ReadOnly;
            });
        if (entry == function.body.loans.end())
          continue;
        constructionBody = MirBodyAddress{MirBodyKind::Function, function.id};
        constructionLoan = stored->id;
        incomingLoan = entry->id;
        otherFormal = other->id;
      }
  }
  expect(constructionBody && constructionLoan && incomingLoan && otherFormal,
         "fixture must exercise a fresh Stored group from a shared reference entry");
  for (int mutation = 0; mutation != 3; ++mutation) {
    auto invalid = constructedCarrier.mir;
    auto *body = findMirBody(invalid, *constructionBody);
    auto &stored = body->loans[constructionLoan - 1];
    auto &entry = body->loans[incomingLoan - 1];
    if (mutation == 0)
      stored.source = otherFormal;
    else if (mutation == 1)
      entry.entry = false;
    else
      entry.access = AccessMode::Mutable;
    const auto rejected = verifyMirProgram(invalid);
    expect(!rejected.valid(),
           "constructor publication cannot forge its source or erase a local/exclusive parent");
  }
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
      selection.fields.front() = 0;
    else if (mutation == 2)
      selection.transferredFrom = loan.id;
    else
      loan.source = selection.carrier;
    expect(!verifyMirProgram(invalid).valid(),
           "physical payload cannot forge external authority, selector, transfer or storage");
  }
  const auto standardLibrary =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto panic = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      standardLibrary.parent_path() / "tests/lifetime-panic-contract.gti", R"gti(
void report_failure(std::string_view message) { gti_internal::panic(message); }
int main() { return 0; }
)gti", {standardLibrary / "prelude.gti"}, {}, {standardLibrary});
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
  const auto pointer = Frontend({.stopAfter = FrontendPhase::Mir})
                           .analyze(standardLibrary.parent_path() / "tests/lifetime-pointer-contracts.gti", R"gti(
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
)gti", {standardLibrary / "prelude.gti"}, {}, {standardLibrary}, {pointerModules});
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
  const auto nominalPointer = Frontend({.stopAfter = FrontendPhase::Mir}).analyze(
      standardLibrary.parent_path() / "tests/lifetime-pointer-nominal.gti", R"gti(
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
)gti", {standardLibrary / "prelude.gti"}, {}, {standardLibrary}, {pointerModules});
  if (!nominalPointer.canGenerateCode())
    for (const auto &diagnostic : nominalPointer.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(nominalPointer.canGenerateCode(),
         "higher-ranked nominal input formation must reach independently verified MIR");
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
  const auto rejectedPath = standardLibrary.parent_path() / "tests/lifetime-pointer-requires.gti";
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
  const auto rejected = Frontend({.stopAfter = FrontendPhase::Mir})
      .analyze(rejectedPath, rejectedSource, {standardLibrary / "prelude.gti"}, {},
               {standardLibrary}, {pointerModules});
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
  std::cout << "lifetime MIR contracts passed\n";
}
