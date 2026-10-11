#include "compiler_test_fixture.h"

namespace {

// Blocks one failure's primary cleanup runs after `from`, through ordinary
// edges, fallible-drop continuations and shared cleanup continuations.
bool failurePathReaches(const lang::MirBody &body, lang::MirBlockId from,
                        lang::MirBlockId to) {
  const lang::MirBlock *cursor = body.findBlock(from);
  for (std::size_t steps = 0; cursor != nullptr && steps <= body.blocks.size();
       ++steps) {
    if (cursor->id == to)
      return true;
    const auto &terminator = cursor->terminator;
    if (terminator.kind != lang::MirTerminatorKind::Goto &&
        terminator.kind != lang::MirTerminatorKind::Invoke &&
        terminator.kind != lang::MirTerminatorKind::ContinueFailure)
      return false;
    cursor = body.findBlock(terminator.target);
  }
  return false;
}

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
class Group {
  Counter& input;
  mut Counter& left;
  mut Counter& right;
public:
  Group(Counter& source, mut Counter& a, mut Counter& b)
      : input(source), left(a), right(b) {}
  void advance(int amount) mut {
    this.left.value += amount;
    this.right.value += this.input.value;
  }
};
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
  for (const std::string program :
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
        "return !result && result.error() == 7 ? 0 : 1;",
        "mut Counter owner{}; mut Counter other{}; "
        "Group(decoy, owner, other).advance(2); return owner.value + "
        "other.value - 11;",
        "mut Counter owner{}; mut Counter other{}; "
        "Group(decoy, owner, other).advance(amount(0)); return 0;"}) {
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
        if (!carrier.loan->additionalDependencies.empty()) {
          for (bool erase : {false, true}) {
            auto malformed = frontend.mir;
            auto *instance = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            auto &dependencies = instance->body.loans[carrier.loan->id - 1]
                                     .additionalDependencies;
            if (erase)
              dependencies.pop_back();
            else
              dependencies.back().access = lang::AccessMode::ReadOnly;
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "temporary receiver preserves every stored permission and "
                   "origin");
          }
        }
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
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
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

void checkTemporaryOperatorPhysicalStorage() {
  const auto declarations = [](bool cleanup) {
    return std::string(R"gti(
class Cursor {
  int& owner;
  int inline_value = 7;
public:
  Cursor(int& source) : owner(source) {}
  // The checked addition keeps a failure edge, so the temporary also has a
  // failure-cleanup endpoint after its interior result loan.
  int& operator*() { int checked = this.inline_value + 0; return this.inline_value; }
  int& ref() { return this.inline_value; }
)gti") + (cleanup ? "  ~Cursor() {}\n" : "") +
           R"gti(
};
Cursor cursor(int& source) { return Cursor(source); }
)gti";
  };
  for (const bool cleanup : {false, true}) {
    for (const auto *expression :
         {"*cursor(owner) + *cursor(other) == 14",
          "false || *cursor(owner) == 7", "true && *cursor(owner) == 7",
          "true || *cursor(owner) == 7", "false && *cursor(owner) == 7",
          "true ? *cursor(owner) == 7 : *cursor(other) == 7"}) {
      const auto frontend = lang::Frontend().analyze(
          "temporary-operator-physical-storage.gti",
          declarations(cleanup) +
              "int main() { int owner = 4; int other = 6; "
              "return (" +
              expression + ") ? 0 : 1; }");
      expect(frontend.canGenerateCode(),
             std::string(
                 "temporary borrowed operator retains physical storage: ") +
                 expression + (cleanup ? " (cleanup-bearing)" : " (passive)"));
      if (!frontend.canGenerateCode()) {
        for (const auto &diagnostic : frontend.diagnostics)
          std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        continue;
      }
      bool found = false;
      bool checkedEndpoint = false;
      for (const auto &function : frontend.mir.functionInstances()) {
        const auto &body = function.body;
        for (const auto &loan : body.loans) {
          if (loan.kind != lang::MirLoanKind::CallResult ||
              loan.storedDependency == 0)
            continue;
          const auto *place = body.findPlace(loan.source);
          const auto temporary =
              place == nullptr
                  ? lang::MirTemporaryCarrierDependency{}
                  : lang::findMirTemporaryCarrierDependency(body, *place);
          if (!temporary)
            continue;
          found = true;
          expect(loan.parent == 0 &&
                     loan.storedDependency == temporary.loan->id &&
                     loan.source != temporary.loan->source &&
                     temporary.drop->fullExpression != 0 &&
                     temporary.loan->lifecycleBound == cleanup,
                 "inline result protects the exact temporary and its separate "
                 "external dependency");
          for (int mutation = 0; mutation < 3; ++mutation) {
            auto malformed = frontend.mir;
            auto *instance = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            auto &changed = instance->body.loans[loan.id - 1];
            if (mutation == 0)
              changed.storedDependency = 0;
            else if (mutation == 1)
              changed.source = temporary.loan->source;
            else
              instance->body.dropObligations[temporary.drop->id - 1].value = 0;
            expect(
                !lang::verifyMirProgram(malformed).valid(),
                "temporary inline result cannot erase its dependency or "
                "producer drop identity, or substitute the external referent");
          }
          for (const auto &other : body.places) {
            if (other.id == loan.source || other.type != place->type ||
                !lang::findMirTemporaryCarrierDependency(body, other))
              continue;
            auto malformed = frontend.mir;
            auto *instance = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            instance->body.loans[loan.id - 1].source = other.id;
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "same-typed temporary cannot replace the selected receiver");
          }
          for (const auto &block : body.blocks) {
            const auto end = std::find_if(
                block.instructions.begin(), block.instructions.end(),
                [&](const auto &instruction) {
                  return instruction.kind ==
                             lang::MirInstructionKind::EndBorrow &&
                         instruction.loan == loan.id;
                });
            if (end == block.instructions.end())
              continue;
            // The temporary's drop follows the loan end in the same block or,
            // on a failure path, in a later block of the same cleanup, which
            // may be a shared continuation.
            for (const auto &later : body.blocks) {
              const auto drop = std::find_if(
                  later.instructions.begin(), later.instructions.end(),
                  [&](const auto &instruction) {
                    return instruction.kind == lang::MirInstructionKind::Drop &&
                           instruction.destination == temporary.drop->place;
                  });
              if (drop == later.instructions.end() ||
                  (later.id == block.id
                       ? end >= drop
                       : block.activeFailure == 0 ||
                             !failurePathReaches(body, block.id, later.id)))
                continue;
              auto malformed = frontend.mir;
              auto *instance = const_cast<lang::MirFunctionInstance *>(
                  malformed.findFunctionInstance(function.id));
              const auto saved = *drop;
              auto &from = instance->body.blocks[later.id - 1].instructions;
              from.erase(from.begin() + (drop - later.instructions.begin()));
              auto &into = instance->body.blocks[block.id - 1].instructions;
              into.insert(into.begin() + (end - block.instructions.begin()),
                          saved);
              expect(!lang::verifyMirProgram(malformed).valid(),
                     "temporary cannot be destroyed before its interior "
                     "result loan ends on success or failure");
              checkedEndpoint = true;
            }
          }
        }
      }
      expect(found && checkedEndpoint,
             "temporary operator regression covers physical result and cleanup "
             "endpoints");
    }
  }
  for (const auto *body :
       {"int& bad(int& owner) { return *cursor(owner); }",
        "int main() { int owner = 4; int& escaped = *cursor(owner); "
        "return escaped; }"}) {
    const auto program = declarations(false) + body;
    const auto start = program.rfind("*cursor(owner)");
    const auto frontend =
        lang::Frontend().analyze("temporary-operator-escape.gti", program);
    expect(!frontend.canGenerateCode() &&
               std::any_of(
                   frontend.diagnostics.begin(), frontend.diagnostics.end(),
                   [&](const auto &diagnostic) {
                     return diagnostic.code == "GTI-S2017" &&
                            diagnostic.phase ==
                                lang::DiagnosticPhase::Semantics &&
                            diagnostic.severity ==
                                lang::DiagnosticSeverity::Error &&
                            std::filesystem::path(diagnostic.primary.source)
                                    .filename() ==
                                "temporary-operator-escape.gti" &&
                            diagnostic.primary.start == start &&
                            diagnostic.primary.end == start + 1 &&
                            diagnostic.message.find("temporary") !=
                                std::string::npos &&
                            diagnostic.fixes.empty();
                   }) &&
               std::none_of(frontend.diagnostics.begin(),
                            frontend.diagnostics.end(),
                            [](const auto &diagnostic) {
                              return diagnostic.code == "GTI-B0001";
                            }),
           "temporary inline reference cannot escape its full expression");
  }
  for (const auto *body :
       {"int& bad(int& owner) { return cursor(owner).ref(); }",
        "int main() { int owner = 4; int& escaped = cursor(owner).ref(); "
        "return escaped; }"}) {
    const auto frontend = lang::Frontend().analyze(
        "temporary-member-escape.gti", declarations(false) + body);
    expect(!frontend.semanticValid &&
               std::any_of(
                   frontend.diagnostics.begin(), frontend.diagnostics.end(),
                   [](const auto &diagnostic) {
                     return diagnostic.code == "GTI-S2081" &&
                            diagnostic.phase ==
                                lang::DiagnosticPhase::Semantics &&
                            diagnostic.message.find("temporary receiver") !=
                                std::string::npos;
                   }) &&
               std::none_of(frontend.diagnostics.begin(),
                            frontend.diagnostics.end(),
                            [](const auto &diagnostic) {
                              return diagnostic.code == "GTI-B0001";
                            }),
           "ordinary temporary methods preserve their existing borrowed-result "
           "rejection at both return and binding boundaries");
  }
  for (const auto *result : {"local.ref()", "*local"}) {
    for (const unsigned int relay : {0U, 1U, 2U}) {
      const std::string body =
          "int& identity(int& source) { return source; } "
          "int& bad(int& owner) { Cursor local = cursor(owner); " +
          (relay == 1   ? "int& selected = " + std::string(result) +
                              "; return selected; }"
           : relay == 2 ? "return identity(" + std::string(result) + "); }"
                        : "return " + std::string(result) + "; }");
      const auto frontend = lang::Frontend().analyze(
          "local-receiver-escape.gti", declarations(false) + body);
      expect(!frontend.semanticValid &&
                 std::any_of(frontend.diagnostics.begin(),
                             frontend.diagnostics.end(),
                             [](const auto &diagnostic) {
                               return diagnostic.code == "GTI-S2017" &&
                                      diagnostic.phase ==
                                          lang::DiagnosticPhase::Semantics &&
                                      diagnostic.message.find("local") !=
                                          std::string::npos;
                             }) &&
                 std::none_of(frontend.diagnostics.begin(),
                              frontend.diagnostics.end(),
                              [](const auto &diagnostic) {
                                return diagnostic.code == "GTI-B0001";
                              }),
             std::string(
                 "a local carrier's external owner cannot authorize its ") +
                 "inline " + (relay != 0 ? "relayed " : "") + result +
                 " return");
    }
  }
  const auto referenceReceiver = lang::Frontend().analyze(
      "reference-receiver-return.gti", declarations(false) + R"gti(
int& relay(Cursor& owner) { return owner.ref(); }
int& dereference(Cursor& owner) { return *owner; }
int& plain_alias(int& owner) { int& first = owner; int& second = first; return second; }
int& alias_relay(Cursor& owner) {
  int decoy = 11;
  int& unrelated = decoy;
  int observed = unrelated;
  int& selected = owner.ref();
  return selected;
}
int main() {
  int owner = 3;
  Cursor local = cursor(owner);
  return relay(local) + dereference(local) + alias_relay(local) + plain_alias(owner) - 24;
}
)gti");
  expect(
      referenceReceiver.canGenerateCode(),
      "physical storage supplied by a reference parameter remains returnable");
  if (!referenceReceiver.canGenerateCode())
    for (const auto &diagnostic : referenceReceiver.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  bool checkedAliasPublication = false;
  if (referenceReceiver.canGenerateCode())
    for (const auto &function : referenceReceiver.mir.functionInstances())
      for (const auto &block : function.body.blocks)
        for (std::size_t index = 0; index < block.instructions.size();
             ++index) {
          const auto &publication = block.instructions[index];
          const auto *published =
              publication.loan ? function.body.findLoan(*publication.loan)
                               : nullptr;
          if (publication.kind != lang::MirInstructionKind::Borrow ||
              published == nullptr ||
              published->kind != lang::MirLoanKind::Return ||
              publication.operands.size() != 1)
            continue;
          const auto &input = publication.operands.front();
          const auto *retained = function.body.findLoan(input.loan);
          const auto *runtime = function.body.findPlace(input.place);
          if (retained == nullptr ||
              retained->kind != lang::MirLoanKind::Parameter ||
              retained->storedDependency == 0 || runtime == nullptr ||
              runtime->root != lang::MirPlaceRootKind::Binding)
            continue;
          const auto unrelated = std::find_if(
              function.body.places.begin(), function.body.places.end(),
              [&](const auto &place) {
                return place.root == lang::MirPlaceRootKind::Binding &&
                       place.binding != runtime->binding &&
                       place.type == runtime->type &&
                       place.access == runtime->access &&
                       place.projections == runtime->projections;
              });
          expect(unrelated != function.body.places.end(),
                 "alias return mutation has an unrelated same-typed runtime "
                 "address");
          if (unrelated == function.body.places.end())
            continue;
          checkedAliasPublication = true;
          for (int mutation = 0; mutation < 4; ++mutation) {
            auto malformed = referenceReceiver.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            auto &operand = changed->body.blocks[block.id - 1]
                                .instructions[index]
                                .operands.front();
            if (mutation == 0)
              operand.place = 0;
            else if (mutation == 1) {
              operand.place = unrelated->id;
              // Even forged carrier membership cannot replace executable
              // initialization from the selected parameter authority.
              changed->body.loans[retained->id - 1].carriers.push_back(
                  unrelated->binding);
            } else if (mutation == 2)
              operand.loan = retained->storedDependency;
            else
              changed->body.loans[published->id - 1].storedDependency = 0;
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "alias publication cannot erase its runtime "
                   "address/dependency, "
                   "substitute another address, or replace its physical "
                   "authority");
          }
        }
  expect(checkedAliasPublication,
         "reference-parameter subobject relay preserves explicit runtime "
         "address evidence");
  const auto external =
      lang::Frontend().analyze("temporary-external-operator-result.gti", R"gti(
class Cursor {
  int& owner;
public:
  Cursor(int& source) : owner(source) {}
  int& operator*() { return this.owner; }
};
Cursor cursor(int& source) { return Cursor(source); }
int& relay(int& owner) { return *cursor(owner); }
int main() {
  int owner = 7;
  int& extracted = *cursor(owner);
  return relay(extracted) - 7;
}
)gti");
  expect(external.canGenerateCode(),
         "an exact external-field operator result does not depend on the "
         "temporary cursor's physical lifetime");
  if (!external.canGenerateCode())
    for (const auto &diagnostic : external.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  const auto fallible = lang::Frontend().analyze(
      "temporary-fallible-borrow-extraction.gti", R"gti(
#include <std/utility>
enum class Error { absent };
struct Owner { int value = 7; };
expected<Owner&, Error> lookup(Owner& source) { return source; }
Owner& relay(Owner& source) { return lookup(source).expect("owner required"); }
int main() {
  Owner owner{};
  Owner& temporary = lookup(owner).expect("owner required");
  auto pending = lookup(owner);
  Owner& consumed = std::move(pending).expect("owner still required");
  return temporary.value + consumed.value + relay(owner).value - 21;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(fallible.canGenerateCode(),
         "compiler-owned fallible extraction preserves its external owner, "
         "not the temporary expected carrier's shell");
  if (!fallible.canGenerateCode())
    for (const auto &diagnostic : fallible.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
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
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
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

void checkTemporaryCursorRetainedOwnerDependencies() {
  const auto frontend = lang::Frontend().analyze(
      "temporary-multiowner-cursor-comparison.gti", R"gti(
struct Finish {};
class OwnerPair {
  int& first;
  int& second;
public:
  OwnerPair(int& left, int& right) : first(left), second(right) {}
  int total() { return this.first + this.second; }
  ReadCursor begin() { return ReadCursor(this); }
  Finish end() { return Finish(); }
};
class ReadCursor {
  OwnerPair& owner;
public:
  ReadCursor(OwnerPair& source) : owner(source) {}
  bool operator!=(Finish other) { return this.owner.total() != 0; }
  bool operator==(Finish other) { return this.owner.total() == 0; }
};
int main() {
  int first = 0;
  int second = 0;
  int third = 1;
  int fourth = 2;
  auto left = OwnerPair(first, second);
  auto right = OwnerPair(third, fourth);
  if (left.begin() != left.end()) { return 1; }
  if (!(left.begin() == left.end())) { return 2; }
  if (!(right.begin() != right.end())) { return 3; }
  return 0;
}
)gti");
  expect(frontend.canGenerateCode(),
         "temporary cursor comparison retains a two-owner carrier's shell "
         "and external dependencies");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  bool checked = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto &body = function.body;
    for (const auto &shell : body.loans) {
      if (shell.kind != lang::MirLoanKind::CallArgument ||
          shell.storedDependency == 0)
        continue;
      const auto *place = body.findPlace(shell.source);
      const auto temporary =
          place == nullptr
              ? lang::MirTemporaryCarrierDependency{}
              : lang::findMirTemporaryCarrierDependency(body, *place);
      const auto *retained =
          temporary ? body.findLoan(temporary.loan->storedDependency) : nullptr;
      if (!temporary || retained == nullptr)
        continue;
      expect(temporary.loan->source != temporary.drop->place &&
                 retained->kind == lang::MirLoanKind::Stored &&
                 retained->additionalDependencies.size() == 1 &&
                 shell.storedDependency == temporary.loan->id,
             "temporary shell, physical receiver and two external owners "
             "remain distinct exact identities");
      const auto other = std::find_if(
          body.loans.begin(), body.loans.end(), [&](const auto &candidate) {
            return candidate.kind == lang::MirLoanKind::Stored &&
                   candidate.id != temporary.loan->id &&
                   candidate.storedDependency != 0 &&
                   candidate.storedDependency != retained->id &&
                   candidate.producedBy != 0 &&
                   candidate.source != temporary.loan->source;
          });
      if (other == body.loans.end())
        continue;
      for (int mutation = 0; mutation < 6; ++mutation) {
        auto malformed = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &changed = instance->body.loans[temporary.loan->id - 1];
        switch (mutation) {
        case 0:
          changed.storedDependency = 0;
          break;
        case 1:
          changed.storedDependency = other->storedDependency;
          break;
        case 2:
          changed.source = other->source;
          break;
        case 3:
          instance->body.loans[retained->id - 1].additionalDependencies.clear();
          break;
        case 4:
          instance->body.loans[shell.id - 1].storedDependency = other->id;
          break;
        case 5:
          changed.storedDependency = changed.id;
          break;
        }
        expect(!lang::verifyMirProgram(malformed).valid(),
               "temporary cursor cannot erase, substitute or forge physical "
               "shell and external-owner dependency authority");
      }
      checked = true;
    }
  }
  expect(checked,
         "temporary cursor regression exercises independently verified "
         "multiowner dependency mutations");
}

void checkTemporaryCursorCleanupProviderLifetime() {
  const auto frontend =
      lang::Frontend().analyze("temporary-cursor-cleanup-provider.gti", R"gti(
struct Provider { int value = 0; };
struct Finish {};
class Binding {
  Provider& provider;
public:
  Binding(Provider& source) : provider(source) {}
  int read() { return this.provider.value; }
};
class Owner {
  Binding binding;
public:
  Owner(Binding source) : binding(source) {}
  ~Owner() { int observed = this.binding.read(); }
  int read() { return this.binding.read(); }
  Cursor begin() { return Cursor(this); }
  Finish end(int divisor) { int checked = 1 / divisor; return Finish(); }
};
class Cursor {
  Owner& owner;
public:
  Cursor(Owner& source) : owner(source) {}
  bool operator!=(Finish other) { return this.owner.read() != 0; }
};
int main() {
  Provider provider{};
  auto binding = Binding(provider);
  Owner owner{binding};
  int divisor = 0;
  if (owner.begin() != owner.end(divisor)) { return 1; }
  return 0;
}
)gti");
  expect(frontend.canGenerateCode(),
         "temporary cursor cleanup keeps an independent cleanup-bound "
         "owner's provider alive on a later argument failure");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  bool checked = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto &body = function.body;
    for (const auto &loan : body.loans) {
      if (!loan.lifecycleBound || loan.parent == 0)
        continue;
      for (const auto &block : body.blocks) {
        const auto childEnd = std::find_if(
            block.instructions.begin(), block.instructions.end(),
            [&](const auto &instruction) {
              return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                     instruction.loan == loan.id;
            });
        const auto providerEnd = std::find_if(
            block.instructions.begin(), block.instructions.end(),
            [&](const auto &instruction) {
              return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                     instruction.loan == loan.parent;
            });
        if (childEnd == block.instructions.end() ||
            providerEnd == block.instructions.end() || childEnd >= providerEnd)
          continue;
        auto malformed = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &instructions = instance->body.blocks[block.id - 1].instructions;
        const auto saved = *providerEnd;
        const auto childIndex = childEnd - block.instructions.begin();
        instructions.erase(instructions.begin() +
                           (providerEnd - block.instructions.begin()));
        instructions.insert(instructions.begin() + childIndex, saved);
        expect(!lang::verifyMirProgram(malformed).valid(),
               "an independent cleanup owner's provider cannot end before "
               "its child cleanup dependency");
        checked = true;
      }
    }
  }
  expect(checked,
         "cleanup-provider regression exercises early ancestor retirement");
}

void checkInlineRangeProxyPhysicalLifetime() {
  const auto frontend = lang::Frontend().analyze(
      "inline-range-proxy-mir.gti", R"gti(
class End {
public:
  End() {}
};
class Projection {
  Buffer& source;
public:
  Projection(Buffer& source) : source(source) {}
  int& read() { return this.source.read(); }
};
class Cursor {
  mut Projection projection;
  mut int position = 0;
public:
  Cursor(Buffer& source) : projection(Projection(source)) {}
  Projection& operator*() { return this.projection; }
  void operator++() mut { this.position++; }
  bool operator!=(End end) { return this.position < 2; }
};
class MutableProjection {
  mut Buffer& source;
public:
  MutableProjection(mut Buffer& source) : source(source) {}
  int& read() { return this.source.read(); }
  mut int& write() mut { return this.source.write(); }
};
class MutableCursor {
  mut MutableProjection projection;
  mut int position = 0;
public:
  MutableCursor(mut Buffer& source) : projection(MutableProjection(source)) {}
  MutableProjection& operator*() { return this.projection; }
  mut MutableProjection& operator*() mut { return this.projection; }
  void operator++() mut { this.position++; }
  bool operator!=(End end) { return this.position < 2; }
};
class Buffer {
  mut int number = 4;
public:
  Buffer() {}
  int& read() { return this.number; }
  mut int& write() mut { return this.number; }
  Cursor begin() { return Cursor(this); }
  MutableCursor begin_mut() mut { return MutableCursor(this); }
  End end() { int checked = this.number + 0; return End(); }
};
int inspect(Buffer& source) {
  mut int total = 0;
  for (auto& element : source) {
    for (auto& inner : source) { total += element.read(); }
  }
  return total;
}
int main() {
  mut Buffer buffer{};
  int original = inspect(buffer);
  for (mut auto& element : buffer) { element.write() += 1; }
  return original + buffer.read() - 22;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "inline readonly/exclusive proxies retain ordinary owning ranges "
         "and borrowed helper parameters");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  std::size_t checked = 0;
  std::size_t siblingCursorChecks = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto &body = function.body;
    for (const auto &range : body.rangeIterations) {
      const auto *element = body.findLoan(range.elementLoan);
      const auto *cursor = element == nullptr
                               ? nullptr
                               : body.findLoan(element->storedDependency);
      expect(element != nullptr && cursor != nullptr &&
                 element->kind == lang::MirLoanKind::CallResult &&
                 element->source == range.iterator && element->parent == 0 &&
                 cursor->kind == lang::MirLoanKind::Stored &&
                 cursor->parent == range.rangeLoan,
             "proxy referent, physical cursor and traversal authority are "
             "separate checked identities");
      if (element == nullptr || cursor == nullptr)
        continue;
      ++checked;
      const auto reject = [&](const lang::MirProgram &malformed,
                              const std::string &reason) {
        const auto *instance = malformed.findFunctionInstance(function.id);
        expect(instance != nullptr &&
                   !lang::verifyMirBody(instance->body, function.id).valid(),
               reason);
        expect(!lang::verifyMirProgram(malformed).valid(), reason);
      };
      for (int mutation = 0; mutation < 8; ++mutation) {
        auto malformed = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &changed = instance->body;
        switch (mutation) {
        case 0:
          changed.loans[element->id - 1].storedDependency = 0;
          break;
        case 1:
          changed.loans[element->id - 1].source = range.rangeCarrier;
          break;
        case 2:
          changed.loans[cursor->id - 1].parent = 0;
          break;
        case 3:
          changed.loans[element->id - 1].escapes = true;
          break;
        case 4:
          changed.loans[cursor->id - 1].carriers.clear();
          break;
        case 5:
          changed.loans[element->id - 1].access =
              range.elementAccess == lang::AccessMode::ReadOnly
                  ? lang::AccessMode::Mutable
                  : lang::AccessMode::ReadOnly;
          break;
        case 6:
          changed.loans[element->id - 1].storedDependency = range.rangeLoan;
          break;
        case 7:
          changed.loans[cursor->id - 1].carriers.clear();
          changed.loans[element->id - 1].storedDependency = range.rangeLoan;
          break;
        }
        reject(malformed,
               "forged proxy range origins, authority and escape fail closed");
      }
      for (const auto &other : body.rangeIterations) {
        if (other.id == range.id || other.elementAccess != range.elementAccess)
          continue;
        const auto *otherElement = body.findLoan(other.elementLoan);
        const auto *otherCursor =
            otherElement == nullptr
                ? nullptr
                : body.findLoan(otherElement->storedDependency);
        if (otherCursor == nullptr)
          continue;
        auto wrongCursor = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            wrongCursor.findFunctionInstance(function.id));
        instance->body.loans[element->id - 1].storedDependency =
            otherCursor->id;
        reject(wrongCursor,
               "a proxy cannot substitute another live same-type cursor shell");
        auto wrongProducer = frontend.mir;
        instance = const_cast<lang::MirFunctionInstance *>(
            wrongProducer.findFunctionInstance(function.id));
        instance->body.loans[element->id - 1].producedBy =
            otherElement->producedBy;
        reject(wrongProducer,
               "a proxy cannot substitute another same-type element producer");
        ++siblingCursorChecks;
      }
      bool earlyEndChecked = false;
      for (const auto &block : body.blocks) {
        const auto selected = std::find_if(
            block.instructions.begin(), block.instructions.end(),
            [&](const auto &instruction) {
              return instruction.kind == lang::MirInstructionKind::Call &&
                     instruction.loan == element->id;
            });
        if (selected == block.instructions.end())
          continue;
        auto malformed = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &changed = instance->body;
        lang::MirInstructionId last = 0;
        for (const auto &candidate : changed.blocks)
          for (const auto &instruction : candidate.instructions)
            last = std::max(last, instruction.id);
        auto &instructions = changed.blocks[block.id - 1].instructions;
        instructions.insert(instructions.begin() +
                                (selected - block.instructions.begin()),
                            {.id = last + 1,
                             .kind = lang::MirInstructionKind::EndBorrow,
                             .loan = cursor->id});
        reject(malformed,
               "a physical cursor cannot retire before proxy publication");
        earlyEndChecked = true;
      }
      expect(earlyEndChecked,
             "proxy regression exercises premature physical cursor retirement");
      if (range.endpointParking) {
        const auto &parking = *range.endpointParking;
        expect(!parking.failureRestores.empty(),
               "fallible endpoint restores the parked cursor on its primary "
               "failure edge before shared cleanup");
        for (int mutation = 0; mutation < 12; ++mutation) {
          auto malformed = frontend.mir;
          auto *instance = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto &changed = instance->body;
          auto &inventory =
              *changed.rangeIterations[range.id - 1].endpointParking;
          const auto instruction =
              [&](lang::MirInstructionId id) -> lang::MirInstruction & {
            for (auto &block : changed.blocks)
              for (auto &candidate : block.instructions)
                if (candidate.id == id)
                  return candidate;
            std::abort();
          };
          switch (mutation) {
          case 0:
            instruction(parking.park).kind =
                lang::MirInstructionKind::RestoreRangeCursor;
            break;
          case 1:
            instruction(parking.restore).kind =
                lang::MirInstructionKind::ParkRangeCursor;
            break;
          case 2:
            instruction(parking.park).loan = range.rangeLoan;
            break;
          case 3:
            inventory.cursor = range.rangeLoan;
            instruction(parking.park).loan = range.rangeLoan;
            instruction(parking.restore).loan = range.rangeLoan;
            for (auto id : parking.failureRestores)
              instruction(id).loan = range.rangeLoan;
            break;
          case 4:
            instruction(parking.park).hirValue = parking.endValue + 1;
            break;
          case 5:
            instruction(parking.restore).hirStatement = range.coreLoop;
            break;
          case 6:
            inventory.beginCall = parking.endCall;
            break;
          case 7:
            inventory.endReceiver = parking.beginCall;
            break;
          case 8:
            instruction(parking.endReceiver).callInputKind =
                lang::HirCallInputKind::MutableBorrow;
            instruction(parking.endReceiver).operands.front().kind =
                lang::MirOperandKind::BorrowWrite;
            break;
          case 9:
            instruction(parking.endCall).borrowOrigin =
                lang::BorrowOriginKind::Receiver;
            break;
          case 10:
            instruction(parking.park).rawMemoryAccess = true;
            break;
          case 11:
            inventory.restore = parking.park;
            break;
          }
          reject(malformed, "forged range parking authority, endpoint identity "
                            "and restoration fail closed");
        }
        for (auto failedRestore : parking.failureRestores) {
          auto malformed = frontend.mir;
          auto *instance = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          for (auto &block : instance->body.blocks)
            for (auto &instruction : block.instructions)
              if (instruction.id == failedRestore)
                instruction.kind = lang::MirInstructionKind::EndBorrow;
          reject(malformed, "failed endpoint restores its cursor before any "
                            "cleanup or retirement");
          const lang::MirBlock *failurePrefix = nullptr;
          for (const auto &block : body.blocks)
            if (!block.instructions.empty() &&
                block.instructions.front().id == failedRestore)
              failurePrefix = &block;
          expect(failurePrefix != nullptr &&
                     failurePrefix->failureParameter != 0 &&
                     body.findFailureRecord(failurePrefix->failureParameter) !=
                         nullptr &&
                     body.findFailureRecord(failurePrefix->failureParameter)
                             ->kind == lang::MirFailureRecordKind::Producer,
                 "endpoint Restore stays in the exact primary failure "
                 "prefix, never a shareable continuation parameter");
          if (failurePrefix != nullptr &&
              failurePrefix->terminator.kind ==
                  lang::MirTerminatorKind::ContinueFailure) {
            const auto *suffix =
                body.findBlock(failurePrefix->terminator.target);
            expect(
                suffix != nullptr &&
                    std::none_of(
                        suffix->instructions.begin(),
                        suffix->instructions.end(),
                        [](const auto &step) {
                          return step.kind ==
                                 lang::MirInstructionKind::RestoreRangeCursor;
                        }),
                "shared cleanup suffix never replays a primary endpoint "
                "permission transition");
            auto delayed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                delayed.findFunctionInstance(function.id));
            auto &prefixBlock = changed->body.blocks[failurePrefix->id - 1];
            const auto restore = prefixBlock.instructions.front();
            prefixBlock.instructions.erase(prefixBlock.instructions.begin());
            auto &suffixBlock = changed->body.blocks[suffix->id - 1];
            suffixBlock.instructions.insert(suffixBlock.instructions.begin(),
                                            restore);
            reject(delayed, "endpoint Restore cannot be delayed across a "
                            "shared failure continuation");
          }
        }
        auto earlyCursorUse = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            earlyCursorUse.findFunctionInstance(function.id));
        for (auto &block : instance->body.blocks) {
          const auto receiver =
              std::find_if(block.instructions.begin(), block.instructions.end(),
                           [&](const auto &instruction) {
                             return instruction.id == parking.endReceiver;
                           });
          if (receiver == block.instructions.end())
            continue;
          lang::MirInstructionId last = 0;
          for (const auto &candidate : instance->body.blocks)
            for (const auto &instruction : candidate.instructions)
              last = std::max(last, instruction.id);
          block.instructions.insert(
              receiver, {.id = last + 1,
                         .kind = lang::MirInstructionKind::EndBorrow,
                         .loan = parking.cursor});
          break;
        }
        reject(earlyCursorUse, "parked cursor cannot be used or destroyed "
                               "before the endpoint completes");
      }
    }
  }
  expect(
      checked == 3,
      "owning inline-proxy fixture exercises shared and exclusive range loans");
  expect(siblingCursorChecks == 2,
         "nested shared traversal exercises exact same-type cursor identities");
}

void checkInlineProxyCannotBecomeAbstractStoredRange() {
  const std::string source = R"gti(
class End { public: End() {} };
class Projection {
  Buffer& source;
public:
  Projection(Buffer& source) : source(source) {}
  int& read() { return this.source.read(); }
};
class Cursor {
  mut Projection projection;
  mut int position = 0;
public:
  Cursor(Buffer& source) : projection(Projection(source)) {}
  Projection& operator*() { return this.projection; }
  void operator++() mut { this.position++; }
  bool operator!=(End end) { return this.position < 2; }
};
class Buffer {
  mut int number = 4;
public:
  Buffer() {}
  int& read() { return this.number; }
  Cursor begin() { return Cursor(this); }
  End end() { return End(); }
};
expected<Buffer&, int> lookup(Buffer& source) { return source; }
int main() {
  Buffer buffer{};
  auto borrowed = lookup(buffer);
  auto& stable = borrowed.value();
  mut int total = 0;
  for (auto& element : stable) { total += element.read(); }
  return total - 8;
}
)gti";
  const auto frontend = lang::Frontend().analyze(
      "inline-proxy-stored-parent.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "an inline proxy over a borrowed carrier keeps a separate physical "
         "cursor despite its Stored parent");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  std::size_t checked = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &range : function.body.rangeIterations) {
      const auto *element = function.body.findLoan(range.elementLoan);
      const auto *parent = function.body.findLoan(range.rangeLoan);
      const auto *cursor =
          element == nullptr
              ? nullptr
              : function.body.findLoan(element->storedDependency);
      const auto *iterator = function.body.findPlace(range.iterator);
      if (!element || !parent || !cursor || !iterator)
        continue;
      expect(
          parent->kind == lang::MirLoanKind::Stored &&
              cursor->id != parent->id &&
              cursor->kind == lang::MirLoanKind::Stored &&
              cursor->parent == parent->id,
          "physical inline proxy and abstract parent have distinct identities");
      for (int mutation = 0; mutation < 6; ++mutation) {
        auto malformed = frontend.mir;
        auto &body = const_cast<lang::MirFunctionInstance *>(
                         malformed.findFunctionInstance(function.id))
                         ->body;
        if (mutation <= 3) {
          body.loans[cursor->id - 1].carriers.clear();
          body.loans[element->id - 1].storedDependency = parent->id;
          if (mutation >= 1)
            body.loans[parent->id - 1].carriers.push_back(iterator->binding);
        }
        for (auto &block : body.blocks)
          for (auto &instruction : block.instructions)
            if (instruction.kind == lang::MirInstructionKind::Call &&
                instruction.loan == element->id) {
              expect(instruction.info.traits.containsBorrowedState,
                     "selected inline proxy has authoritative nominal borrowed "
                     "traits");
              if (mutation == 2 || mutation == 3 || mutation == 4)
                instruction.info.traits.containsBorrowedState = false;
              if (mutation == 3)
                body.rangeIterations[range.id - 1].elementAccessSource = 0;
              if (mutation == 5)
                instruction.functionTarget = function.id;
            }
        const auto result = lang::verifyMirProgram(malformed);
        expect(!result.valid(),
               "a physical cursor cannot be forged into abstract stored "
               "authority: " +
                   std::to_string(mutation));
        if (mutation == 2 || mutation == 3 || mutation == 4)
          expect(
              std::any_of(
                  result.errors.begin(), result.errors.end(),
                  [](const auto &error) {
                    return error.message ==
                           "reference iteration element has no exact nominal "
                           "traits and selected dereference declaration";
                  }),
              "the program catalog independently rejects falsified nominal "
              "proxy traits even when cursor authority is also rewired");
      }
      ++checked;
    }
  }
  expect(checked == 1,
         "Stored-parent proxy forgery fixture exercises one range");
  auto virtualSource = source;
  virtualSource.insert(virtualSource.find("Projection& operator*()"),
                       "virtual ");
  const auto virtualFrontend = lang::Frontend().analyze(
      "virtual-inline-proxy-stored-parent.gti", virtualSource,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(virtualFrontend.canGenerateCode(),
         "nominal element validation preserves virtual checked dereference");
  bool virtualProducer = false;
  for (const auto &function : virtualFrontend.mir.functionInstances())
    for (const auto &range : function.body.rangeIterations)
      for (const auto &block : function.body.blocks)
        for (const auto &instruction : block.instructions)
          virtualProducer |=
              instruction.kind == lang::MirInstructionKind::Call &&
              instruction.loan == range.elementLoan &&
              instruction.dispatch == lang::CallDispatch::Virtual;
  expect(virtualProducer,
         "virtual reference-yield fixture exercises the selected dynamic call");
}

void checkRangeEndpointSemanticContracts() {
  const std::string prefix = R"gti(
class Endpoint {
  int count;
public:
  Endpoint(int count) : count(count) {}
  int length() { return this.count; }
};
class EntryProjection {
  mut EndpointRange& owner;
public:
  EntryProjection(mut EndpointRange& owner) : owner(owner) {}
  mut int& value() mut { return this.owner.value(); }
};
class EntryCursor {
  mut EntryProjection projection;
  mut int position = 0;
public:
  EntryCursor(mut EndpointRange& owner) : projection(EntryProjection(owner)) {}
  mut EntryProjection& operator*() mut { return this.projection; }
  bool operator!=(Endpoint endpoint) { return this.position < endpoint.length(); }
  void operator++() mut { this.position++; }
};
class EndpointRange {
  mut int number = 1;
  Endpoint terminal{1};
public:
  EndpointRange() {}
  mut int& value() mut { return this.number; }
  EntryCursor begin_mut() mut { return EntryCursor(this); }
)gti";
  const std::string suffix = R"gti(
};
int main() {
  mut EndpointRange source{};
  for (mut auto& entry : source) { entry.value() += 1; }
  return source.value() - 2;
}
)gti";
  for (const auto *endpoint :
       {"Endpoint end() { return Endpoint(1); }",
        "Endpoint end<Args...>(Args... args) { return Endpoint(1); }"}) {
    const auto frontend = lang::Frontend().analyze("range-endpoint-valid.gti",
                                                   prefix + endpoint + suffix);
    expect(frontend.canGenerateCode(),
           "concrete zero-argument readonly endpoint supports an exclusive "
           "inline proxy, including an empty variadic instantiation");
    if (!frontend.canGenerateCode())
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  for (const auto *endpoint :
       {"Endpoint end(int ignored = 0) { return Endpoint(1); }",
        "virtual Endpoint end() { return Endpoint(1); }",
        "Endpoint end() mut { return Endpoint(1); }",
        "[[noreturn]] Endpoint end() { while (true) {} }",
        "Endpoint& end() { return this.terminal; }"}) {
    const std::string source = prefix + endpoint + suffix;
    const auto colon = source.find(": source)");
    const auto declaration = source.find("end", prefix.size());
    const auto frontend =
        lang::Frontend().analyze("range-endpoint-invalid.gti", source);
    expect(
        !frontend.semanticValid &&
            std::any_of(
                frontend.diagnostics.begin(), frontend.diagnostics.end(),
                [&](const auto &diagnostic) {
                  return diagnostic.code == "GTI-S2083" &&
                         diagnostic.phase == lang::DiagnosticPhase::Semantics &&
                         diagnostic.severity ==
                             lang::DiagnosticSeverity::Error &&
                         std::filesystem::path(diagnostic.primary.source)
                                 .filename() == "range-endpoint-invalid.gti" &&
                         diagnostic.primary.start == colon &&
                         diagnostic.primary.end == colon + 1 &&
                         diagnostic.message.find("inline-proxy") !=
                             std::string::npos &&
                         diagnostic.related.size() == 1 &&
                         diagnostic.related.front().span.start == declaration &&
                         diagnostic.related.front().span.end ==
                             declaration + 3 &&
                         !diagnostic.hints.empty() &&
                         diagnostic.hints.front().find("independent") !=
                             std::string::npos &&
                         diagnostic.fixes.empty();
                }) &&
            std::none_of(frontend.diagnostics.begin(),
                         frontend.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-B0001";
                         }),
        "unsupported exclusive proxy endpoint receives actionable source "
        "diagnostic, not invalid HIR/MIR");
  }

  // These endpoint restrictions belong to the exclusive physical proxy, not
  // all source ranges or all methods with default arguments/borrowed results.
  const std::string readPrefix = R"gti(
class Endpoint { public: Endpoint() {} };
class ReadCursor {
  int& number;
  mut int position = 0;
public:
  ReadCursor(int& number) : number(number) {}
  int& operator*() { return this.number; }
  bool operator!=(Endpoint endpoint) { return this.position < 1; }
  void operator++() mut { this.position++; }
};
class ReadRange {
  int number = 1;
  Endpoint terminal{};
public:
  ReadRange() {}
  ReadCursor begin() { return ReadCursor(this.number); }
)gti";
  for (const auto *endpoint :
       {"Endpoint end(int ignored = 0) { return Endpoint(); }",
        "virtual Endpoint end() { return Endpoint(); }",
        "Endpoint& end() { return this.terminal; }"}) {
    const auto frontend = lang::Frontend().analyze(
        "range-endpoint-readonly.gti",
        readPrefix + endpoint +
            "}; int main() { ReadRange source{}; mut int total = 0; "
            "for (auto& number : source) { total += number; } "
            "return total - 1; }");
    expect(frontend.canGenerateCode(),
           "ordinary readonly scalar ranges preserve default, virtual and "
           "borrowed-reference endpoints");
    if (!frontend.canGenerateCode())
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

void checkReadonlyLocalCleanupGuard() {
  const std::string prefix = R"gti(
class Guard {
  char& value;
public:
  Guard(char& source) : value(source) {}
  ~Guard() { int check = 1 / (this.value == 'a' ? 1 : 0); }
};
class Finish {};
class Cursor {
public:
  char operator*() { return 'a'; }
  void operator++() mut {}
  bool operator!=(Finish finish) { return true; }
};
class Sequence {
public:
  Cursor begin() { return Cursor(); }
  Finish end() { return Finish(); }
};
int main() {
  char unrelated = 'b';
  char& decoy = unrelated;
)gti";
  for (const bool panicDestructor : {false, true})
    for (const auto *body :
         {"char owner = 'a'; char& value = owner; Guard guard{value}; return "
          "0;",
          "Sequence sequence{}; for (char& value : sequence) { "
          "Guard guard{value}; break; } return 0;"}) {
      std::string source = prefix + body + "}";
      if (panicDestructor) {
        const std::string arithmetic =
            "~Guard() { int check = 1 / (this.value == 'a' ? 1 : 0); }";
        source.replace(source.find(arithmetic), arithmetic.size(),
                       "~Guard() { if (this.value != 'a') "
                       "std::panic(\"bad referent\"); }");
        source.insert(0, "#include <std/panic>\n");
      }
      const auto frontend = lang::Frontend().analyze(
          "readonly-local-cleanup-guard.gti", source,
          panicDestructor
              ? std::vector<std::filesystem::path>{standardLibraryPrelude()}
              : std::vector<std::filesystem::path>{},
          {}, {standardLibraryRoot()});
      expect(frontend.canGenerateCode(),
             "cleanup guard retains an ordinary or iteration-local readonly "
             "reference through its exact checked parent");
      if (!frontend.canGenerateCode()) {
        for (const auto &diagnostic : frontend.diagnostics)
          std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        continue;
      }
      bool checked = false;
      for (const auto &function : frontend.mir.functionInstances()) {
        if (function.entryKind == lang::ProgramEntryKind::None)
          continue;
        for (const auto &guard : function.body.loans) {
          const auto *parent = function.body.findLoan(guard.parent);
          if (guard.kind != lang::MirLoanKind::Stored ||
              !guard.lifecycleBound || !parent ||
              parent->kind != lang::MirLoanKind::Local)
            continue;
          expect(guard.access == lang::AccessMode::ReadOnly &&
                     parent->access == lang::AccessMode::ReadOnly &&
                     guard.storedDependency == parent->storedDependency &&
                     !guard.entry && !guard.escapes,
                 "cleanup guard preserves exact readonly ancestry and "
                 "independent external dependency");
          const auto decoy =
              std::find_if(function.body.loans.begin(),
                           function.body.loans.end(), [&](const auto &loan) {
                             return loan.kind == lang::MirLoanKind::Local &&
                                    loan.id != parent->id &&
                                    loan.access == lang::AccessMode::ReadOnly &&
                                    loan.parent == 0 &&
                                    loan.source != parent->source;
                           });
          expect(decoy != function.body.loans.end(),
                 "cleanup guard proof has an unrelated same-typed reference");
          if (decoy == function.body.loans.end())
            continue;
          bool foundProducer = false;
          for (const auto &block : function.body.blocks) {
            for (const auto &instruction : block.instructions) {
              if (instruction.kind != lang::MirInstructionKind::Construct ||
                  instruction.loan != guard.id)
                continue;
              foundProducer = true;
              for (int mutation = 0; mutation < 4; ++mutation) {
                auto malformed = frontend.mir;
                auto *changed = const_cast<lang::MirFunctionInstance *>(
                    malformed.findFunctionInstance(function.id));
                auto &loan = changed->body.loans[guard.id - 1];
                if (mutation == 0) {
                  loan.parent = decoy->id;
                } else if (mutation == 1) {
                  loan.source = decoy->source;
                } else if (mutation == 2) {
                  loan.access = lang::AccessMode::Mutable;
                } else {
                  auto &instructions =
                      changed->body.blocks[block.id - 1].instructions;
                  const auto producer =
                      std::find_if(instructions.begin(), instructions.end(),
                                   [&](const auto &candidate) {
                                     return candidate.id == instruction.id;
                                   });
                  lang::MirInstructionId freshId = 1;
                  for (const auto &candidateBlock : changed->body.blocks)
                    for (const auto &candidate : candidateBlock.instructions)
                      freshId = std::max(freshId, candidate.id + 1);
                  instructions.insert(
                      producer, {.id = freshId,
                                 .kind = lang::MirInstructionKind::EndBorrow,
                                 .loan = parent->id});
                }
                expect(!lang::verifyMirProgram(malformed).valid(),
                       "cleanup guard cannot substitute its selected parent or "
                       "source, escalate access, or retire its parent early");
              }
            }
          }
          expect(foundProducer,
                 "cleanup guard's checked constructor owns its lifecycle loan");
          checked = true;
        }
      }
      expect(checked, "cleanup guard regression exercises exact parent proof");
    }
}

void checkMovedExclusiveParameterEntry() {
  const auto frontend = lang::Frontend().analyze(
      "exclusive-parameter-entry.gti", R"gti(
#include <std/iterator>
class UserPosition {
  mut int& number;
public:
  UserPosition(mut int& number) : number(number) {}
  UserPosition(UserPosition& other) = delete;
  UserPosition(UserPosition&& other) = default;
  int& operator*() { return this.number; }
  mut int& operator*() mut { return this.number; }
  void operator++() mut { this.number++; }
  void operator--() mut { this.number--; }
  void operator+=(std::ptrdiff_t amount) mut { this.number += int(amount); }
  void operator-=(std::ptrdiff_t amount) mut { this.number -= int(amount); }
};
int transport<Position>(mut Position initial)
    requires std::random_access_cursor<Position> && std::writes_to<Position, int> {
  std::advance(initial, std::ptrdiff_t(1));
  mut auto later = std::next(std::move(initial), std::ptrdiff_t(1));
  mut auto final = std::prev(std::move(later));
  *final = 42;
  return *final;
}
int main() {
  mut int number = 0;
  int result = transport(UserPosition(number));
  return number + result - 84;
}
)gti",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(),
         "noncopyable exclusive parameters retain their entry origin across "
         "generic movement and local transport without lexical cleanup");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "unrelated exclusive position transport verifies independently");
  std::size_t checked = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *hir = frontend.hir.findFunctionInstance(function.id);
    if (hir == nullptr || hir->source == nullptr ||
        hir->source->name().lexeme != "transport")
      continue;
    for (const auto &loan : function.body.loans) {
      if (!loan.entry || loan.kind != lang::MirLoanKind::Stored ||
          loan.lifecycleBound)
        continue;
      const auto *origin = function.body.findPlace(loan.source);
      expect(origin != nullptr &&
                 origin->root == lang::MirPlaceRootKind::Binding &&
                 origin->projections.empty() &&
                 origin->type.kind == lang::SemanticType::Class &&
                 std::find(function.parameterBindings.begin(),
                           function.parameterBindings.end(), origin->binding) !=
                     function.parameterBindings.end() &&
                 loan.carriers ==
                     std::vector<lang::HirBindingId>{origin->binding},
             "the moved formal remains immutable entry provenance, not a "
             "second live exclusive value");
      if (origin == nullptr)
        continue;
      ++checked;
      for (int mutation = 0; mutation < 5; ++mutation) {
        auto malformed = frontend.mir;
        auto *instance = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &changed = instance->body;
        auto &entry = changed.loans[loan.id - 1];
        if (mutation == 0) {
          std::erase(entry.carriers, origin->binding);
        } else if (mutation == 1) {
          entry.designatedFields.clear();
        } else if (mutation == 2) {
          entry.access = lang::AccessMode::ReadOnly;
        } else if (mutation == 3) {
          bool found = false;
          for (auto &result : changed.loans) {
            if (result.entry || result.kind != lang::MirLoanKind::Stored)
              continue;
            result.parent = entry.id;
            found = true;
            break;
          }
          expect(found, "exclusive relay publishes a concrete Stored result");
        } else {
          bool found = false;
          for (auto &block : changed.blocks) {
            if (block.activeFailure != 0 || block.failureParameter != 0)
              continue;
            const auto retirement = std::find_if(
                block.instructions.begin(), block.instructions.end(),
                [&](const auto &instruction) {
                  return instruction.kind ==
                             lang::MirInstructionKind::EndBorrow &&
                         instruction.loan == entry.id;
                });
            if (retirement == block.instructions.end())
              continue;
            block.instructions.erase(retirement);
            found = true;
            break;
          }
          expect(found,
                 "consuming relay retires the former exclusive authority");
        }
        expect(!lang::verifyMirProgram(malformed).valid(),
               "exclusive formal provenance, field path, access, ancestry and "
               "retirement cannot be forged after a consuming call");
        bool backendRejected = false;
        try {
          (void)gti_test::lowerProgram(frontend, malformed, malformed);
        } catch (const std::exception &) {
          backendRejected = true;
        }
        expect(backendRejected,
               "lowered backend input rejects forged exclusive parameter "
               "entry");
      }
    }
  }
  expect(checked == 1,
         "exclusive transport exercises one non-lifecycle Stored entry loan");
}

void checkMultiDependencyRelayIdentity() {
  for (const bool sharedFirst : {false, true}) {
    const std::string prefix = sharedFirst ? "int&" : "mut int&";
    const auto frontend = lang::Frontend().analyze(
        "multi-dependency-relay.gti",
        "#include <std/utility>\nclass Owners {\n" + prefix + R"gti( first;
  mut int& second;
public:
  Owners()gti" +
            prefix + R"gti( first, mut int& second)
      : first(first), second(second) {}
  void advance() mut { this.second += this.first; }
};
T relay<T>(T owners) { return std::move(owners); }
int main() {
  mut int left = 3;
  mut int right = 0;
  {
    Owners input{left, right};
    mut auto result = relay(std::move(input));
    result.advance();
    mut auto final = relay(std::move(result));
    final.advance();
  }
  return left + right - 9;
}
)gti",
        {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
    expect(frontend.canGenerateCode(),
           "generic relay preserves distinct multi-dependency endpoints");
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    const auto lowered =
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
    expect(lang::verifyMirProgram(frontend.mir).valid() &&
               lang::verifyLoweredProgram(lowered).empty(),
           "mixed and exclusive owner relay admits verified backend input");
    std::size_t checked = 0;
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &loan : function.body.loans) {
        if (loan.entry || loan.kind != lang::MirLoanKind::Stored ||
            loan.additionalDependencies.empty() || loan.producedBy == 0)
          continue;
        const auto *hir = frontend.hir.findFunctionInstance(function.id);
        const auto *producer =
            hir ? hir->body.findValue(loan.producedBy) : nullptr;
        if (!producer || !producer->functionTarget)
          continue;
        ++checked;
        for (int mutation = 0; mutation < 2; ++mutation) {
          auto malformed = frontend.mir;
          auto *instance = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          if (mutation == 0) {
            instance->body.loans[loan.id - 1].additionalDependencies.clear();
          } else {
            bool changed = false;
            for (auto &block : instance->body.blocks) {
              if (changed || block.activeFailure || block.failureParameter)
                continue;
              for (auto &instruction : block.instructions) {
                if (instruction.kind == lang::MirInstructionKind::EndBorrow &&
                    instruction.loan && *instruction.loan < loan.id) {
                  instruction.loan = loan.id;
                  changed = true;
                  break;
                }
              }
            }
            expect(changed, "a consuming relay retires its input first");
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "relay cannot omit an owner or use the input's endpoint for "
                 "the result");
          bool rejected = false;
          try {
            (void)gti_test::lowerProgram(frontend, malformed, malformed);
          } catch (const std::exception &) {
            rejected = true;
          }
          expect(rejected, "backend admission rejects forged relay endpoints");
        }
      }
    }
    expect(checked == 2,
           "both multi-dependency relay publications are checked");
  }
}

} // namespace

int main() {
  checkSchedules();
  checkConstructorRollbackMutations();
  checkForbiddenResultMutation();
  checkLegacyBorrowedOperatorSchedules();
  checkReservedSiblingRetirement();
  checkTemporaryOperatorPhysicalStorage();
  checkConditionalParentReborrowMutation();
  checkTemporaryCursorRetainedOwnerDependencies();
  checkTemporaryCursorCleanupProviderLifetime();
  checkInlineRangeProxyPhysicalLifetime();
  checkInlineProxyCannotBecomeAbstractStoredRange();
  checkRangeEndpointSemanticContracts();
  checkReadonlyLocalCleanupGuard();
  checkMovedExclusiveParameterEntry();
  checkMultiDependencyRelayIdentity();
  return failures == 0 ? 0 : 1;
}
