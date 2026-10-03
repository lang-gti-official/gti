#include "compiler_test_fixture.h"

namespace {

const std::string declarations = R"gti(
struct Counter { mut int value = 4; };
class Service {
  mut Counter& counter;
public:
  Service(mut Counter& source) : counter(source) {}
  int read() { return this.counter.value; }
  void advance(int amount) mut { this.counter.value += amount; }
};
Service service(mut Counter& source) { return Service(source); }
int amount(int divisor) { return 4 / divisor; }
mut Counter global_owner{};
class Trace {
  int divisor;
public:
  Trace(int value) : divisor(value) {}
  ~Trace() { int result = 1 / this.divisor; }
};
class FallibleService {
  mut Counter& counter;
  Trace lifetime;
public:
  FallibleService(mut Counter& source, int divisor)
      : counter(source), lifetime(Trace(1)) { int result = 1 / divisor; }
  int read() { return this.counter.value; }
};
int conditional(mut Counter& owner, bool take) {
  int result = take ? service(owner).read() : 0;
  return result;
}
expected<int, int> maybe_amount(bool fail) {
  if (fail) { return unexpected(7); }
  return 2;
}
expected<int, int> expected_call(mut Counter& owner, bool fail) {
  service(owner).advance(?maybe_amount(fail));
  return owner.value;
}
)gti";

lang::FrontendResult analyze(const std::string &body) {
  return lang::Frontend().analyze(
      "ephemeral-carrier-mir.gti",
      declarations + "\nint main() {\nmut Counter decoy{}; decoy.value = 1;\n" +
          body + "\n}\n",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
}

void checkSchedules() {
  for (const std::string &program :
       {"mut Counter owner{}; service(owner).advance(2); return owner.value - "
        "6;",
        "mut Counter owner{}; int value = Service(owner).read(); return value "
        "- 4;",
        "mut Counter owner{}; service(owner).advance(amount(1)); return "
        "owner.value - 8;",
        "mut Counter owner{}; service(owner).advance(amount(0)); return 0;",
        "mut Counter owner{}; service(owner).advance(2147483647); return 0;",
        "Service(global_owner).advance(1); return global_owner.value - 5;",
        "mut Counter owner{}; bool value = service(owner).read() == 4 && "
        "amount(1) > 0; return value ? 0 : 1;",
        "mut Counter owner{}; bool value = amount(1) > 0 && "
        "service(owner).read() == 4; return value ? 0 : 1;",
        "mut Counter owner{}; bool value = amount(-1) > 0 && "
        "service(owner).read() == 4; return value ? 1 : 0;",
        "mut Counter owner{}; int value = amount(1) > 0 ? "
        "service(owner).read() : 0; return value - 4;",
        "mut Counter owner{}; int value = amount(1) > 0 ? "
        "service(owner).read() : Service(owner).read(); return value - 4;",
        "mut Counter owner{}; int result = FallibleService(owner, 1).read(); "
        "return result - 4;",
        "mut Counter owner{}; int result = FallibleService(owner, 0).read(); "
        "return result - 4;",
        "mut Counter owner{}; int result = conditional(owner, true); return "
        "result - 4;",
        "mut Counter owner{}; auto result = expected_call(owner, false); "
        "return result && result.value() == 6 ? 0 : 1;",
        "mut Counter owner{}; auto result = expected_call(owner, true); "
        "return !result && result.error() == 7 ? 0 : 1;"}) {
    const auto frontend = analyze(program);
    expect(frontend.canGenerateCode(),
           "ephemeral carrier has verified MIR: " + program);
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      for (const auto &function : frontend.mir.functionInstances()) {
        if (!lang::verifyMirBody(function.body).valid())
          std::cerr << lang::MirPrinter().print(function.body);
      }
      continue;
    }
    bool found = false;
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &loan : function.body.loans) {
        if (loan.kind != lang::MirLoanKind::CallArgument ||
            loan.storedDependency == 0)
          continue;
        const auto *place = function.body.findPlace(loan.source);
        const auto carrier = place == nullptr
                                 ? lang::MirTemporaryCarrierDependency{}
                                 : lang::findMirTemporaryCarrierDependency(
                                       function.body, *place);
        expect(carrier && carrier.loan->id == loan.storedDependency &&
                   carrier.drop->fullExpression != 0 && loan.parent == 0,
               "shell and external owner use distinct exact loan identities");
        found = true;
        if (!carrier)
          continue;
        for (int mutation = 0; mutation < 5; ++mutation) {
          auto malformed = frontend.mir;
          auto *instance = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto &changed = instance->body.loans[loan.id - 1];
          switch (mutation) {
          case 0:
            changed.storedDependency = 0;
            break;
          case 1:
            changed.source = carrier.loan->source;
            break;
          case 2:
            changed.parent = changed.storedDependency;
            break;
          case 3:
            changed.producedBy = carrier.loan->producedBy;
            break;
          case 4:
            changed.access = changed.access == lang::AccessMode::Mutable
                                 ? lang::AccessMode::ReadOnly
                                 : lang::AccessMode::Mutable;
            break;
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "independent proof rejects forged temporary shell authority");
        }
        const auto *owner = function.body.findPlace(carrier.loan->source);
        if (owner != nullptr) {
          for (const auto &other : function.body.places) {
            if (other.id == owner->id || other.type != owner->type ||
                other.root != lang::MirPlaceRootKind::Binding ||
                !other.projections.empty() || other.access != owner->access)
              continue;
            auto malformed = frontend.mir;
            auto *instance = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            instance->body.loans[carrier.loan->id - 1].source = other.id;
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "stored source must match the exact producer's resolved "
                   "origin");
            break;
          }
        }
        for (const auto &block : function.body.blocks) {
          for (const auto &instruction : block.instructions) {
            const bool shell =
                instruction.kind == lang::MirInstructionKind::CallInput &&
                instruction.loan == loan.id;
            const bool drop =
                instruction.kind == lang::MirInstructionKind::Drop &&
                instruction.destination == carrier.drop->place;
            if (!shell && !drop)
              continue;
            auto malformed = frontend.mir;
            auto *instance = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            auto &instructions =
                instance->body.blocks[block.id - 1].instructions;
            auto &changed = *std::find_if(
                instructions.begin(), instructions.end(),
                [&](const auto &value) { return value.id == instruction.id; });
            if (shell) {
              changed.loan.reset();
            } else {
              changed.kind = lang::MirInstructionKind::Lifecycle;
              changed.destination.reset();
              changed.lifecycle.clear();
              changed.info = {};
              changed.definedFailure = {};
            }
            expect(
                !lang::verifyMirProgram(malformed).valid(),
                shell
                    ? "receiver cannot erase its shell loan"
                    : "normal and conditional carrier drops cannot disappear");
          }
          for (std::size_t index = 1; index < block.instructions.size();
               ++index) {
            const auto &instruction = block.instructions[index];
            if (instruction.kind != lang::MirInstructionKind::EndBorrow ||
                instruction.loan != carrier.loan->id ||
                block.instructions[index - 1].kind !=
                    lang::MirInstructionKind::Drop)
              continue;
            auto malformed = frontend.mir;
            auto *instance = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            auto &instructions =
                instance->body.blocks[block.id - 1].instructions;
            std::swap(instructions[index], instructions[index - 1]);
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "dependency cannot end before the carrier drop on any path");
          }
        }
      }
    }
    expect(found,
           "fixture exercises an explicit ephemeral shell receiver loan");
  }
}

void checkConstructorRollbackMutations() {
  const auto frontend = analyze(
      "mut Counter owner{}; int result = FallibleService(owner, 0).read(); "
      "return result;");
  expect(frontend.canGenerateCode(),
         "constructor rollback mutation seed verifies");
  if (!frontend.canGenerateCode())
    return;
  bool checkedMissing = false;
  bool checkedEarly = false;
  bool checkedSuccess = false;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto &body = constructor.body;
    for (const auto &loan : body.loans) {
      if (loan.kind != lang::MirLoanKind::Stored || !loan.escapes ||
          loan.storedField == 0)
        continue;
      for (const auto &block : body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::EndBorrow ||
              instruction.loan != loan.id)
            continue;
          auto missing = body;
          auto &instructions = missing.blocks[block.id - 1].instructions;
          std::erase_if(instructions, [&](const auto &value) {
            return value.id == instruction.id;
          });
          expect(!lang::verifyMirBody(missing).valid(),
                 "failed construction cannot export a stored dependency");
          checkedMissing = true;
          for (const auto &targetBlock : body.blocks) {
            if (targetBlock.activeFailure != 0) {
              const auto rollback = std::find_if(
                  targetBlock.instructions.begin(),
                  targetBlock.instructions.end(), [&](const auto &value) {
                    const auto *drop = value.lifecycle.size() == 1
                                           ? body.findDropObligation(
                                                 value.lifecycle.front().source)
                                           : nullptr;
                    return value.kind == lang::MirInstructionKind::Drop &&
                           drop != nullptr &&
                           drop->kind == lang::MirDropObligationKind::
                                             ConstructionRollback;
                  });
              if (rollback == targetBlock.instructions.end())
                continue;
              auto early = missing;
              auto &earlyInstructions =
                  early.blocks[targetBlock.id - 1].instructions;
              const auto position = std::find_if(
                  earlyInstructions.begin(), earlyInstructions.end(),
                  [&](const auto &value) { return value.id == rollback->id; });
              earlyInstructions.insert(position, instruction);
              expect(
                  !lang::verifyMirBody(early).valid(),
                  "stored dependency survives every armed subobject rollback");
              checkedEarly = true;
            } else if (targetBlock.terminator.kind ==
                       lang::MirTerminatorKind::Return) {
              auto success = missing;
              success.blocks[targetBlock.id - 1].instructions.push_back(
                  instruction);
              expect(
                  !lang::verifyMirBody(success).valid(),
                  "successful constructor must export its stored dependency");
              checkedSuccess = true;
            }
          }
        }
      }
    }
  }
  expect(checkedMissing && checkedEarly && checkedSuccess,
         "constructor lifetime mutations cover failure, rollback, and "
         "publication");
}

void checkForbiddenResultMutation() {
  const auto frontend =
      lang::Frontend().analyze("ephemeral-carrier-result.gti", R"gti(
struct Owner { int value = 4; };
class View {
  Owner& owner;
public:
  View(Owner& source) : owner(source) {}
  int read() { return this.owner.value; }
  int* pointer() { return nullptr; }
};
int main() {
  Owner owner{};
  { View named{owner}; [[discard]] named.pointer(); }
  [[discard]] View(owner).read();
  return 0;
}
)gti");
  expect(frontend.canGenerateCode(),
         "named carrier raw-result mutation seed verifies");
  if (!frontend.canGenerateCode())
    return;
  const auto rawTarget = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.owner &&
               function.returnType.kind == lang::SemanticType::RawPointer;
      });
  expect(rawTarget != frontend.mir.functionInstances().end(),
         "mutation seed retains a valid named-only raw-result target");
  if (rawTarget == frontend.mir.functionInstances().end())
    return;
  bool checked = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call || !call.receiver ||
            call.receiver->kind != lang::MirOperandKind::Value || !call.result)
          continue;
        const auto *value = function.body.findValue(call.receiver->value);
        if (value == nullptr)
          continue;
        const auto *stageBlock =
            function.body.findBlock(value->definitionBlock);
        const auto stage = std::find_if(
            stageBlock->instructions.begin(), stageBlock->instructions.end(),
            [&](const auto &instruction) {
              return instruction.id == value->definition &&
                     instruction.kind == lang::MirInstructionKind::CallInput &&
                     instruction.callInputRole ==
                         lang::MirCallInputRole::Receiver &&
                     instruction.callInputKind ==
                         lang::HirCallInputKind::ReadTemporaryBorrow &&
                     instruction.info.traits.containsBorrowedState;
            });
        if (stage == stageBlock->instructions.end())
          continue;
        auto malformed = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &instructions = instance->body.blocks[block.id - 1].instructions;
        auto &changed = *std::find_if(
            instructions.begin(), instructions.end(),
            [&](const auto &instruction) { return instruction.id == call.id; });
        changed.functionTarget = rawTarget->id;
        changed.info.type = rawTarget->returnType;
        changed.info.traits = lang::semanticTraits(rawTarget->returnType);
        instance->body.values[*changed.result - 1].info = changed.info;
        expect(lang::verifyMirBody(instance->body).valid(),
               "forged raw-result call remains body-local well typed");
        const auto verification = lang::verifyMirProgram(malformed);
        expect(
            std::any_of(
                verification.errors.begin(), verification.errors.end(),
                [&](const auto &error) {
                  return error.owner == function.id &&
                         error.instruction == call.id &&
                         error.message.find(
                             "temporary receiver input does not match") !=
                             std::string::npos;
                }),
            "program verification rejects a raw result from an ephemeral "
            "borrowed receiver even when the exact target is otherwise valid");
        checked = true;
      }
    }
  }
  expect(checked, "raw-result mutation exercises one ephemeral receiver call");
}

void checkLegacyBorrowedOperatorSchedules() {
  const std::string source = R"gti(
struct Counter { int value = 4; };
class Cursor {
  Counter& counter;
public:
  Cursor(Counter& source) : counter(source) {}
  int& operator*() { return this.counter.value; }
};
Cursor cursor(Counter& owner) { return Cursor(owner); }
int main() {
  Counter owner{};
)gti";
  for (const auto *expression :
       {"false || *cursor(owner) == 4", "true || *cursor(owner) == 4",
        "true && *cursor(owner) == 4", "false && *cursor(owner) == 4",
        "true ? *cursor(owner) == 4 : false",
        "false ? false : *cursor(owner) == 4"}) {
    const auto frontend = lang::Frontend().analyze(
        "legacy-borrowed-operator.gti",
        source + "bool result = " + expression + "; return result ? 0 : 1; }");
    expect(frontend.canGenerateCode(),
           std::string("borrowed-result operator keeps its original loan ") +
               "schedule in conditional expressions: " + expression);
    if (!frontend.canGenerateCode())
      continue;
    bool foundValueReceiver = false;
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &block : function.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::CallInput ||
              instruction.callInputRole != lang::MirCallInputRole::Receiver ||
              !instruction.info.traits.containsBorrowedState)
            continue;
          expect(instruction.callInputKind == lang::HirCallInputKind::Value &&
                     !instruction.loan,
                 "borrowed-result operator is not an ephemeral shell receiver");
          foundValueReceiver = true;
        }
      }
    }
    expect(foundValueReceiver,
           "regression exercises the distinct borrowed-result operator family");
  }
}

void checkReservedSiblingRetirement() {
  const auto frontend =
      lang::Frontend().analyze("reserved-sibling-retirement.gti", R"gti(
class Slot {
  mut int value = 3;
public:
  int& operator[](int index) { return this.value; }
  mut int& operator[](int index) mut { return this.value; }
};
void bump(mut Slot& slot) { slot[0] = slot[0] + 1; }
int main() {
  mut Slot slot{};
  bump(slot);
  return slot[0] == 4 ? 0 : 1;
}
)gti");
  expect(frontend.canGenerateCode(),
         "ending a read sibling restores the parent despite an unactivated "
         "two-phase reservation");
  if (!frontend.canGenerateCode())
    return;
  bool checked = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &reservation : function.body.loans) {
      if (!reservation.twoPhase || reservation.parent == 0)
        continue;
      for (const auto &reader : function.body.loans) {
        if (reader.id == reservation.id ||
            reader.parent != reservation.parent ||
            reader.kind != lang::MirLoanKind::CallResult ||
            reader.access != lang::AccessMode::ReadOnly)
          continue;
        bool readerEnds = false;
        bool reservationActivates = false;
        for (const auto &block : function.body.blocks) {
          for (const auto &instruction : block.instructions) {
            readerEnds =
                readerEnds ||
                (instruction.kind == lang::MirInstructionKind::EndBorrow &&
                 instruction.loan == reader.id);
            reservationActivates =
                reservationActivates ||
                (instruction.kind == lang::MirInstructionKind::ActivateBorrow &&
                 instruction.loan == reservation.id);
          }
        }
        checked = checked || (readerEnds && reservationActivates);
      }
    }
  }
  expect(checked,
         "Slot regression exercises read retirement and reservation activation "
         "under the same mutable parent");
}

void checkConditionalParentReborrowMutation() {
  const auto frontend =
      lang::Frontend().analyze("conditional-parent-reborrow.gti", R"gti(
struct Owner { mut int first = 4; mut int second = 7; };
class View {
  mut int& value;
public:
  View(mut int& source) : value(source) {}
  int read() { return this.value; }
};
int choose(mut Owner& owner, bool take) {
  int result = take ? View(owner.first).read() : 0;
  return result + owner.second;
}
int main() {
  mut Owner owner{};
  return choose(owner, true) - 11;
}
)gti");
  expect(frontend.canGenerateCode(),
         "conditional-parent mutation seed has independently verified MIR");
  if (!frontend.canGenerateCode())
    return;
  bool checked = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto &body = function.body;
    for (const auto &block : body.blocks) {
      if (block.activeFailure != 0)
        continue;
      for (const auto &drop : block.instructions) {
        if (drop.kind != lang::MirInstructionKind::Drop || !drop.destination ||
            drop.lifecycle.size() != 1 || !drop.lifecycle.front().conditional)
          continue;
        const auto *place = body.findPlace(*drop.destination);
        const auto carrier =
            place == nullptr
                ? lang::MirTemporaryCarrierDependency{}
                : lang::findMirTemporaryCarrierDependency(body, *place);
        const auto *parent =
            carrier ? body.findLoan(carrier.loan->parent) : nullptr;
        const auto *source =
            carrier ? body.findPlace(carrier.loan->source) : nullptr;
        if (parent == nullptr || !parent->entry ||
            parent->access != lang::AccessMode::Mutable || source == nullptr ||
            source->projections.empty() ||
            source->projections.back().kind != lang::MirProjectionKind::Field)
          continue;
        // Borrow a disjoint sibling field: the exact Active and Suspended
        // predecessor states both permit it. Only their conditional union at
        // the merge must reject the new executable authority.
        const auto sibling = std::find_if(
            body.places.begin(), body.places.end(), [&](const auto &candidate) {
              return candidate.root == source->root &&
                     candidate.binding == source->binding &&
                     candidate.symbol == source->symbol &&
                     candidate.loan == source->loan &&
                     candidate.type == source->type &&
                     candidate.projections.size() ==
                         source->projections.size() &&
                     candidate.projections.back().kind ==
                         lang::MirProjectionKind::Field &&
                     candidate.projections.back().field !=
                         source->projections.back().field &&
                     std::equal(source->projections.begin(),
                                source->projections.end() - 1,
                                candidate.projections.begin());
            });
        if (sibling == body.places.end())
          continue;
        auto malformed = body;
        const lang::MirLoanId child = malformed.loans.size() + 1;
        malformed.loans.push_back({.id = child,
                                   .parent = parent->id,
                                   .kind = lang::MirLoanKind::Local,
                                   .source = sibling->id,
                                   .access = lang::AccessMode::Mutable});
        lang::MirInstructionId next = 1;
        for (const auto &candidateBlock : body.blocks)
          for (const auto &instruction : candidateBlock.instructions)
            next = std::max(next, instruction.id + 1);
        const lang::MirInstruction borrow{
            .id = next,
            .kind = lang::MirInstructionKind::Borrow,
            .operands = {{.kind = lang::MirOperandKind::Loan,
                          .loan = parent->id,
                          .type = body.findPlace(parent->source)->type}},
            .loan = child};
        const lang::MirInstruction end{.id = next + 1,
                                       .kind =
                                           lang::MirInstructionKind::EndBorrow,
                                       .loan = child};
        auto &instructions = malformed.blocks[block.id - 1].instructions;
        const auto position = std::find_if(
            instructions.begin(), instructions.end(),
            [&](const auto &value) { return value.id == drop.id; });
        instructions.insert(position, {borrow, end});
        const auto verification = lang::verifyMirBody(malformed, function.id);
        const bool exactFailure = std::any_of(
            verification.errors.begin(), verification.errors.end(),
            [&](const auto &error) {
              return error.owner == function.id && error.block == block.id &&
                     error.instruction == borrow.id &&
                     error.message ==
                         "conditionally live temporary dependency cannot "
                         "authorize "
                         "an executable loan use in direct child reborrow";
            });
        expect(exactFailure,
               "direct Borrow cannot turn a conditional parent into definite "
               "authority at the CFG merge");
        if (!exactFailure)
          for (const auto &error : verification.errors)
            std::cerr << error.message << '\n';
        checked = true;
      }
    }
  }
  expect(checked,
         "conditional-parent mutation exercises a disjoint direct child borrow "
         "before conditional carrier retirement");
}

} // namespace

int main() {
  checkSchedules();
  checkConstructorRollbackMutations();
  checkForbiddenResultMutation();
  checkLegacyBorrowedOperatorSchedules();
  checkReservedSiblingRetirement();
  checkConditionalParentReborrowMutation();
  return failures == 0 ? 0 : 1;
}
