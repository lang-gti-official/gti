#include "compiler_test_fixture.h"

namespace {
void checkFoldLifetimeApplication(const lang::FrontendResult &result) {
  using namespace lang;
  bool checked = false;
  for (const auto &function : result.hir.functionInstances()) {
    for (const auto &fold : function.body.values) {
      if (fold.kind != HirValueKind::PackFold ||
          fold.packFoldElements.size() != 2)
        continue;
      const auto *original = function.body.findValue(fold.packFoldElements[0].call);
      if (!original || original->lifetimeApplication.bindings.empty() ||
          !original->callPlan || original->callPlan->arguments.size() != 1 ||
          original->callableArguments.size() != 1)
        continue;
      for (int mutation = 0; mutation != 14; ++mutation) {
        auto forged = result.hir;
        auto *changed = const_cast<HirFunctionInstance *>(
            forged.findFunctionInstance(function.id));
        auto *changedFold = const_cast<HirValue *>(changed->body.findValue(fold.id));
        auto &element = changedFold->packFoldElements[0];
        auto *call = const_cast<HirValue *>(changed->body.findValue(element.call));
        auto &application = call->lifetimeApplication;
        if (mutation == 0) application.complete = false;
        if (mutation == 1) application.bindings.clear();
        if (mutation == 2)
          application.bindings.push_back({Lifetime::parameter(900001),
                                          application.bindings[0].second});
        if (mutation == 3)
          application.bindings[0].first = Lifetime::parameter(900002);
        if (mutation == 4)
          application.bindings[0].second = Lifetime::staticLifetime();
        if (mutation == 5) {
          element.parameterTypes[0].referenceLifetime = Lifetime::staticLifetime();
          call->parameterTypes = element.parameterTypes;
          call->callPlan->arguments[0].parameterType = element.parameterTypes[0];
        }
        if (mutation == 6) {
          auto *input = const_cast<HirValue *>(
              changed->body.findValue(call->operands[0]));
          input->packElement = 1;
        }
        if (mutation == 7)
          changedFold->packFoldElements[1].call = element.call;
        if (mutation == 8) call->callableArguments.clear();
        if (mutation == 9)
          call->callableArguments[0].boundary = CallableBoundary::Owned;
        if (mutation == 10) call->callableArguments[0].parameterIndex = 1;
        if (mutation == 11)
          call->callableArguments.push_back(call->callableArguments[0]);
        if (mutation == 12 || mutation == 13) {
          auto *target = const_cast<HirFunctionInstance *>(
              forged.findFunctionInstance(element.functionTarget));
          if (mutation == 12)
            target->typeArguments[0] = SemanticType::Unknown;
          else if (!target->typeArguments[0].lifetimeArguments.empty())
            target->typeArguments[0].lifetimeArguments[0] = Lifetime::staticLifetime();
          else
            target->typeArguments[0].classId = 900003;
        }
        const auto proof = verifyHirProgramPlans(result.semantics, forged);
        expect(!proof.valid() && std::any_of(proof.errors.begin(), proof.errors.end(),
                   [](const auto &error) {
                     return error.find("HIR fold lost its exact ordered call") !=
                            std::string::npos;
                   }),
               "fold verification rejects incomplete/foreign applications, retagged "
               "applied parameters, wrong equal-typed lanes, duplicate calls and "
               "missing or forged callable boundaries and generic owner types");
      }
      checked = true;
      break;
    }
    if (checked) break;
  }
  expect(checked, "reference fold fixture exercises a complete selected lifetime application");
}
} // namespace

int main() {
  const auto result = lang::Frontend().analyze(
      "reference-pack-loans.gti", R"gti(
class Ticket {
public:
  Ticket() = default;
  Ticket(Ticket& other) = delete;
  Ticket(Ticket&& other) = default;
};
void read<T>(T& value) {}
void write<T>(mut T& value) {}
void observe<Args...>(Args&... args) { (read(args), ...); }
void update<Args...>(mut Args&... args) {
  (write(args), ...);
  observe(args...);
}
int main() { mut Ticket first{}; mut Ticket second{}; update(first, second); return 0; }
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "reference pack calls and folds retain verified per-element loans");
  if (result.hirValid)
    checkFoldLifetimeApplication(result);
  if (!result.canGenerateCode())
    return 1;
  bool checkedEntry = false;
  bool checkedInput = false;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &loan : function.body.loans) {
      const auto *place = function.body.findPlace(loan.source);
      if (!loan.entry || loan.access != lang::AccessMode::Mutable || !place ||
          place->projections.empty() ||
          place->projections.front().kind !=
              lang::MirProjectionKind::PackElement)
        continue;
      auto forged = result.mir;
      auto *instance = const_cast<lang::MirFunctionInstance *>(
          forged.findFunctionInstance(function.id));
      instance->body.loans[loan.id - 1].access = lang::AccessMode::ReadOnly;
      expect(!lang::verifyMirProgram(forged).valid(),
             "a mutable pack entry cannot be forged into read-only authority");
      checkedEntry = true;
      break;
    }
    for (const auto &block : function.body.blocks) {
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const auto &input = block.instructions[index];
        if (input.kind != lang::MirInstructionKind::CallInput ||
            input.callInputRole != lang::MirCallInputRole::Argument ||
            input.callInputKind != lang::HirCallInputKind::MutableBorrow)
          continue;
        auto forged = result.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            forged.findFunctionInstance(function.id));
        instance->body.blocks[block.id - 1].instructions[index].callInputKind =
            lang::HirCallInputKind::MoveValue;
        expect(
            !lang::verifyMirProgram(forged).valid(),
            "a reference-pack call cannot transfer ownership of its referent");
        checkedInput = true;
        break;
      }
      if (checkedInput)
        break;
    }
    if (checkedEntry && checkedInput)
      break;
  }
  expect(checkedEntry && checkedInput,
         "reference packs publish entry and call authority");
  const auto carriers = lang::Frontend().analyze("reference-pack-carrier-origins.gti", R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
void inspect<T>(T& value) {}
void relay<Args...>(Args&... values) { (inspect(values), ...); }
int main() { int first = 3; int second = 8; View left{first}; View right{second}; relay(left, right); return 0; }
)gti");
  if (!carriers.canGenerateCode())
    for (const auto &diagnostic : carriers.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(carriers.canGenerateCode() && lang::verifyMirProgram(carriers.mir).valid(),
         "reference-to-carrier pack leaves retain separate physical shells and stored external groups");
  if (carriers.hirValid)
    checkFoldLifetimeApplication(carriers);
  bool checkedGroups = false;
  for (const auto &function : carriers.mir.functionInstances()) {
    const lang::MirLoan *first = nullptr;
    const lang::MirLoan *second = nullptr;
    for (const auto &loan : function.body.loans) {
      const auto *place = function.body.findPlace(loan.source);
      if (!loan.entry || loan.kind != lang::MirLoanKind::Parameter || !loan.storedDependency ||
          !place || place->projections.empty() ||
          place->projections.front().kind != lang::MirProjectionKind::PackElement)
        continue;
      if (!first) first = &loan;
      else if (place->binding == function.body.findPlace(first->source)->binding) { second = &loan; break; }
    }
    if (!first || !second)
      continue;
    expect(first->storedDependency != second->storedDependency,
           "equal-shaped carrier leaves have distinct exact incoming owner groups");
    for (int mutation = 0; mutation != 3; ++mutation) {
      auto forged = carriers.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(forged.findFunctionInstance(function.id));
      auto &shell = changed->body.loans[first->id - 1];
      auto &group = changed->body.loans[first->storedDependency - 1];
      if (mutation == 0) shell.storedDependency = second->storedDependency;
      if (mutation == 1) group.source = changed->body.loans[second->storedDependency - 1].source;
      if (mutation == 2) group.entry = false;
      expect(!lang::verifyMirProgram(forged).valid(),
             "pack proof rejects a sibling's owners, wrong exact source prefix, or retired incoming group");
    }
    checkedGroups = true;
    break;
  }
  expect(checkedGroups, "carrier pack fixture must exercise two independent stored input groups");
  return failures ? 1 : 0;
}
