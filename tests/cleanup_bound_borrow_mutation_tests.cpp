#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;
int mutations = 0;

void expect(bool condition, const std::string &name) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << name << '\n';
  }
}

const std::string declarations = R"gti(
#include <std/utility>
mut int observed = 0;
struct Resource {
  mut int value = 7;
  void reset() mut { this.value = 9; }
public:
  Resource() = default;
};
class Guard {
  Resource& resource;
public:
  Guard(Resource& resource) : resource(resource) {}
  ~Guard() { observed = this.resource.value; }
};
class Box<T> {
  T item;
public:
  Box(T item) : item(std::move(item)) {}
};
)gti";

std::optional<lang::MirProgram> analyze(const std::string &name,
                                        const std::string &source) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  auto result = lang::Frontend().analyze(
      root / "cleanup-bound-mutation.gti", declarations + source,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  expect(result.canGenerateCode(), name + " control compiles");
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    if (std::getenv("GTI_TEST_DUMP_MIR") != nullptr)
      for (const auto &error : lang::verifyMirProgram(result.mir).errors) {
        if (error.bodyKind == lang::MirBodyKind::Function) {
          if (const auto *function =
                  result.mir.findFunctionInstance(error.owner))
            std::cerr << lang::MirPrinter().print(function->body);
        } else if (error.bodyKind == lang::MirBodyKind::Constructor) {
          if (const auto *constructor =
                  result.mir.findConstructorInstance(error.owner))
            std::cerr << lang::MirPrinter().print(constructor->body);
        }
      }
    return std::nullopt;
  }
  expect(lang::verifyMirProgram(result.mir).valid(),
         name + " control verifies");
  return std::move(result.mir);
}

void rejected(lang::MirProgram program, const std::string &name,
              std::string_view message = {}) {
  ++mutations;
  // Operand substitutions must be rejected by their semantic proof, not a
  // stale cached SSA use index left behind by the test itself.
  for (const auto &function : program.functionInstances())
    expect(lang::rebuildMirValueUses(
               const_cast<lang::MirFunctionInstance &>(function).body),
           name + " rebuilds function value uses");
  for (const auto &constructor : program.constructorInstances())
    expect(lang::rebuildMirValueUses(
               const_cast<lang::MirConstructorInstance &>(constructor).body),
           name + " rebuilds constructor value uses");
  for (const auto &destructor : program.destructorInstances())
    expect(lang::rebuildMirValueUses(
               const_cast<lang::MirDestructorInstance &>(destructor).body),
           name + " rebuilds destructor value uses");
  const auto result = lang::verifyMirProgram(program);
  expect(!result.valid(), name + " is rejected");
  if (!message.empty())
    expect(std::any_of(result.errors.begin(), result.errors.end(),
                       [&](const auto &error) {
                         return error.message.find(message) !=
                                std::string::npos;
                       }),
           name + " reports " + std::string(message));
  if (std::getenv("GTI_TEST_DUMP_MUTATION_ERRORS") != nullptr)
    for (const auto &error : result.errors)
      std::cerr << name << ": " << error.message << '\n';
}

struct BodyKey {
  lang::MirBodyKind kind;
  std::size_t owner;
};

lang::MirBody &body(lang::MirProgram &program, BodyKey key) {
  if (key.kind == lang::MirBodyKind::Destructor)
    return const_cast<lang::MirDestructorInstance *>(
               program.findDestructorInstance(key.owner))
        ->body;
  if (key.kind == lang::MirBodyKind::Constructor)
    return const_cast<lang::MirConstructorInstance *>(
               program.findConstructorInstance(key.owner))
        ->body;
  return const_cast<lang::MirFunctionInstance *>(
             program.findFunctionInstance(key.owner))
      ->body;
}

lang::MirInstruction *instruction(lang::MirBody &body,
                                  lang::MirInstructionId id) {
  for (auto &block : body.blocks)
    for (auto &candidate : block.instructions)
      if (candidate.id == id)
        return &candidate;
  return nullptr;
}

const lang::MirInstruction *definition(const lang::MirBody &body,
                                       lang::MirValueId value) {
  const auto *record = body.findValue(value);
  if (record == nullptr)
    return nullptr;
  for (const auto &block : body.blocks)
    for (const auto &candidate : block.instructions)
      if (candidate.id == record->definition)
        return &candidate;
  return nullptr;
}

std::optional<lang::MirInstructionId> loanEnd(const lang::MirBody &body,
                                              lang::MirLoanId loan) {
  for (const auto &block : body.blocks)
    if (block.activeFailure == 0)
      for (const auto &candidate : block.instructions)
        if (candidate.kind == lang::MirInstructionKind::EndBorrow &&
            candidate.loan == loan)
          return candidate.id;
  return std::nullopt;
}

// Move only proof instructions, preserving all value/block/obligation IDs.
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

bool moveBefore(lang::MirBody &body, lang::MirInstructionId moved,
                lang::MirInstructionId before) {
  std::optional<lang::MirInstruction> saved;
  for (auto &block : body.blocks) {
    const auto found =
        std::find_if(block.instructions.begin(), block.instructions.end(),
                     [&](const auto &i) { return i.id == moved; });
    if (found != block.instructions.end()) {
      saved = *found;
      block.instructions.erase(found);
      break;
    }
  }
  if (!saved)
    return false;
  for (auto &block : body.blocks) {
    const auto found =
        std::find_if(block.instructions.begin(), block.instructions.end(),
                     [&](const auto &i) { return i.id == before; });
    if (found != block.instructions.end()) {
      block.instructions.insert(found, *saved);
      return true;
    }
  }
  return false;
}

lang::MirInstructionId freshInstructionId(const lang::MirBody &body) {
  lang::MirInstructionId result = 0;
  for (const auto &block : body.blocks)
    for (const auto &candidate : block.instructions)
      result = std::max(result, candidate.id);
  return result + 1;
}

// A closure that moves in a cleanup-bound carrier is itself one: the lambda
// instance records semantics' decision, which MIR re-derives from its
// captures, and the closure formation's loan lives until the closure drops.
void checkClosureCarrier() {
  const auto control = analyze("closure-carrier", R"gti(
int main() {
  mut Resource first{};
  {
    Guard guard = Guard(first);
    auto held = [kept = std::move(guard)]() -> int { return 1; };
    observed = held();
  }
  first.reset(); return 0;
}
)gti");
  if (!control)
    return;
  bool flagged = false;
  for (const auto &lambda : control->lambdaInstances())
    flagged = flagged || lambda.lifecycleBoundBorrowCarrier;
  expect(flagged, "closure holding a cleanup-bound carrier is itself one");
  {
    auto cleared = *control;
    for (const auto &lambda : cleared.lambdaInstances())
      const_cast<lang::MirLambdaInstance &>(lambda)
          .lifecycleBoundBorrowCarrier = false;
    rejected(std::move(cleared), "closure carrier decision cannot be cleared",
             "closure cleanup-bound dependency retention");
  }
  bool checkedEnd = false;
  for (const auto &function : control->functionInstances()) {
    const BodyKey key{lang::MirBodyKind::Function, function.id};
    for (const auto &loan : function.body.loans) {
      if (!loan.lifecycleBound || loan.entry || loan.escapes)
        continue;
      const lang::MirInstruction *formation = nullptr;
      for (const auto &block : function.body.blocks)
        for (const auto &candidate : block.instructions)
          if (candidate.loan == loan.id &&
              candidate.kind == lang::MirInstructionKind::Compute &&
              candidate.operation == lang::MirOperation::Closure)
            formation = &candidate;
      const auto end = loanEnd(function.body, loan.id);
      if (formation == nullptr || !end)
        continue;
      std::optional<lang::MirInstructionId> drop;
      for (const auto &block : function.body.blocks)
        if (block.activeFailure == 0)
          for (const auto &candidate : block.instructions)
            if (candidate.kind == lang::MirInstructionKind::Drop &&
                candidate.info.type == formation->info.type)
              drop = candidate.id;
      if (!drop)
        continue;
      auto early = *control;
      expect(moveBefore(body(early, key), *end, *drop),
             "closure carrier loan end moves before the closure drop");
      rejected(std::move(early),
               "closure carrier loan cannot end before the closure drops",
               "ends before carrier drop");
      checkedEnd = true;
    }
  }
  expect(checkedEnd, "closure carrier fixture exercises its formation loan");
}

void checkOwnedCall() {
  const auto control = analyze("owned-call", R"gti(
void consume(Guard first, Guard second, int divisor) {
  int checked = 1 / divisor;
}
int main() {
  mut Resource first{}; mut Resource second{};
  consume(Guard(first), Guard(second), 1);
  first.reset(); second.reset(); return 0;
}
)gti");
  if (!control)
    return;
  bool checkedCall = false;
  bool checkedEntry = false;
  for (const auto &function : control->functionInstances()) {
    const BodyKey key{lang::MirBodyKind::Function, function.id};
    for (const auto &block : function.body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call ||
            !call.functionTarget || call.operands.size() != 3 ||
            call.parameterTypes.size() != 3 ||
            call.parameterTypes[0].kind != lang::SemanticType::Class ||
            call.parameterTypes[0] != call.parameterTypes[1])
          continue;
        const auto *first = definition(function.body, call.operands[0].value);
        const auto *second = definition(function.body, call.operands[1].value);
        if (first == nullptr || second == nullptr ||
            !first->preparedParameterDrop || !second->preparedParameterDrop)
          continue;
        checkedCall = true;
        for (const auto &loan : function.body.loans) {
          if (!loan.lifecycleBound || loan.entry || loan.escapes)
            continue;
          const auto end = loanEnd(function.body, loan.id);
          if (!end)
            continue;
          for (const auto anchor : {first->id, call.id}) {
            auto malformed = *control;
            expect(moveBefore(body(malformed, key), *end, anchor),
                   "owned-call endpoint mutation is applied");
            rejected(malformed,
                     "owner loan cannot end before " +
                         std::string(anchor == call.id ? "invocation"
                                                       : "preparation"),
                     "lifecycle-bound");
          }
          break;
        }
        const auto source = *first->preparedParameterDrop;
        for (const auto &failureBlock : function.body.blocks) {
          if (failureBlock.activeFailure == 0)
            continue;
          for (const auto &drop : failureBlock.instructions) {
            if (drop.kind != lang::MirInstructionKind::Drop ||
                std::none_of(drop.lifecycle.begin(), drop.lifecycle.end(),
                             [&](const auto &event) {
                               return event.kind ==
                                          lang::MirLifecycleEventKind::Drop &&
                                      event.source == source;
                             }))
              continue;
            for (const auto &loan : function.body.loans) {
              if (!loan.lifecycleBound || loan.producedBy != first->hirValue)
                continue;
              for (const auto &continuation : function.body.blocks) {
                if (continuation.activeFailure != failureBlock.activeFailure)
                  continue;
                for (const auto &end : continuation.instructions) {
                  if (end.kind != lang::MirInstructionKind::EndBorrow ||
                      end.loan != loan.id)
                    continue;
                  auto malformed = *control;
                  expect(moveBefore(body(malformed, key), end.id, drop.id),
                         "prepared failure endpoint mutation is applied");
                  rejected(malformed,
                           "later argument failure must drop prior prepared "
                           "guard before ending its loan",
                           "lifecycle-bound");
                }
              }
            }
          }
          // A later argument's failed construction can finish loan retirement
          // after the prior prepared parameter's fallible cleanup. That
          // continuation must still belong to the same exact failure record.
          if (failureBlock.failureParameter == 0 &&
              std::any_of(failureBlock.instructions.begin(),
                          failureBlock.instructions.end(), [](const auto &i) {
                            return i.kind ==
                                   lang::MirInstructionKind::EndBorrow;
                          })) {
            const auto unrelated = std::find_if(
                function.body.failureRecords.begin(),
                function.body.failureRecords.end(), [&](const auto &record) {
                  return record.id != failureBlock.activeFailure;
                });
            if (unrelated != function.body.failureRecords.end()) {
              auto malformed = *control;
              body(malformed, key).blocks[failureBlock.id - 1].activeFailure =
                  unrelated->id;
              rejected(malformed, "failed producer endpoint retains its exact "
                                  "failure identity");
            }
          }
        }
        for (const std::string mutation : {"wrong-drop", "missing", "duplicate",
                                           "conditional", "failure-cleanup"}) {
          auto malformed = *control;
          auto *changed = instruction(body(malformed, key), call.id);
          const auto event = std::find_if(
              changed->lifecycle.begin(), changed->lifecycle.end(),
              [&](const auto &candidate) {
                return candidate.kind ==
                           lang::MirLifecycleEventKind::TransferOut &&
                       candidate.source == source;
              });
          expect(event != changed->lifecycle.end(),
                 "owned-call has exact transfer event");
          if (event == changed->lifecycle.end())
            continue;
          if (mutation == "wrong-drop")
            event->source = *second->preparedParameterDrop;
          else if (mutation == "missing")
            changed->lifecycle.erase(event);
          else if (mutation == "duplicate") {
            const auto duplicate = *event;
            changed->lifecycle.push_back(duplicate);
          } else if (mutation == "conditional")
            event->conditional = true;
          else
            event->failureCleanup = true;
          rejected(
              malformed, "owned-call rejects " + mutation + " transfer",
              mutation == "conditional" || mutation == "failure-cleanup"
                  ? "invalid lifecycle event"
                  : "prepared owning parameter must transfer exactly once");
        }
        for (const bool wrongSite : {false, true}) {
          auto malformed = *control;
          auto *changed = instruction(body(malformed, key), first->id);
          if (wrongSite)
            ++changed->callSite;
          else
            changed->callInputIndex = 1;
          rejected(malformed,
                   wrongSite ? "prepared input belongs to exact call"
                             : "prepared input belongs to exact index",
                   "call input use does not match its ordered invocation");
        }
      }
    }
    for (const auto &loan : function.body.loans) {
      if (!loan.lifecycleBound || !loan.entry || loan.escapes)
        continue;
      auto missingMarker = *control;
      body(missingMarker, key).loans[loan.id - 1].lifecycleBound = false;
      rejected(missingMarker,
               "owned parameter cannot erase cleanup retention marker");
      const auto end = loanEnd(function.body, loan.id);
      if (!end)
        continue;
      const auto *source = function.body.findPlace(loan.source);
      if (source == nullptr)
        continue;
      for (const auto &block : function.body.blocks)
        for (const auto &drop : block.instructions)
          if (drop.kind == lang::MirInstructionKind::Drop &&
              drop.destination == source->id) {
            auto malformed = *control;
            expect(moveBefore(body(malformed, key), *end, drop.id),
                   "entry endpoint mutation is applied");
            rejected(malformed, "owned parameter retains loan through drop",
                     "lifecycle-bound");
            body(malformed, key).loans[loan.id - 1].lifecycleBound = false;
            rejected(
                malformed,
                "unmarked owned parameter still retains loan through drop");
            checkedEntry = true;
          }
    }
  }
  expect(checkedCall, "owned-call fixture exercises exact prepared transfers");
  expect(checkedEntry, "owned-call fixture exercises callee entry retention");
}

void checkNestedPublication() {
  const auto control = analyze("nested-publication", R"gti(
class PairBox {
  Guard first; Guard second;
public:
  PairBox(Guard first, Guard second)
      : first(std::move(first)), second(std::move(second)) {}
};
int main() {
  mut Resource first{}; mut Resource second{};
  { PairBox box{Guard(first), Guard(second)}; }
  first.reset(); second.reset(); return 0;
}
)gti");
  if (!control)
    return;
  bool checkedPublication = false;
  bool checkedWrongValue = false;
  bool checkedDestroy = false;
  for (const auto &constructor : control->constructorInstances()) {
    const BodyKey key{lang::MirBodyKind::Constructor, constructor.id};
    std::vector<const lang::MirInstruction *> publications;
    for (const auto &block : constructor.body.blocks)
      for (const auto &candidate : block.instructions) {
        const auto *loan = candidate.loan
                               ? constructor.body.findLoan(*candidate.loan)
                               : nullptr;
        if (candidate.kind == lang::MirInstructionKind::Borrow &&
            loan != nullptr && loan->lifecycleBound && loan->escapes &&
            loan->storedField != 0 && candidate.operands.size() == 1 &&
            candidate.operands.front().kind == lang::MirOperandKind::Loan &&
            candidate.operands.front().value != 0)
          publications.push_back(&candidate);
      }
    if (publications.empty())
      continue;
    checkedPublication = true;
    const auto &publication = *publications.front();
    const auto &published = *constructor.body.findLoan(*publication.loan);
    const auto &entry =
        *constructor.body.findLoan(publication.operands.front().loan);
    auto missingMarkers = *control;
    for (auto &loan : body(missingMarkers, key).loans)
      loan.lifecycleBound = false;
    rejected(
        missingMarkers,
        "constructor cannot erase entry and field retention markers together");
    if (const auto end = loanEnd(constructor.body, entry.id)) {
      auto malformed = *control;
      expect(moveBefore(body(malformed, key), *end, publication.id),
             "field entry endpoint mutation is applied");
      rejected(malformed, "entry loan cannot end before field publication");
    } else
      expect(false, "field publication retires its entry dependency");

    auto earlyEscape = *control;
    auto &escapedBody = body(earlyEscape, key);
    bool inserted = false;
    for (auto &block : escapedBody.blocks) {
      if (block.terminator.kind == lang::MirTerminatorKind::Return) {
        block.instructions.push_back(
            {.id = freshInstructionId(escapedBody),
             .kind = lang::MirInstructionKind::EndBorrow,
             .hirValue = published.producedBy,
             .loan = published.id});
        inserted = true;
        break;
      }
    }
    expect(inserted, "constructor has a successful publication exit");
    rejected(earlyEscape, "escaping field loan cannot end on normal return",
             "constructor field dependency cannot retire");
    for (auto &loan : body(earlyEscape, key).loans)
      loan.lifecycleBound = false;
    rejected(earlyEscape,
             "unmarked escaping field dependency cannot end on normal return",
             "constructor field dependency cannot retire");

    auto access = *control;
    body(access, key).loans[published.id - 1].access =
        lang::AccessMode::Mutable;
    rejected(access, "publication cannot upgrade a shared dependency");

    // Match the exact moved value's staged cleanup transfer. A same-typed Drop
    // is a linear discharge too, but cannot implement publication into a field.
    for (const auto &block : constructor.body.blocks)
      for (const auto &candidate : block.instructions) {
        if (candidate.kind != lang::MirInstructionKind::Initialize ||
            candidate.hirValue != publication.hirValue ||
            candidate.lifecycle.size() != 1 ||
            candidate.lifecycle.front().kind !=
                lang::MirLifecycleEventKind::Reparent)
          continue;
        const auto *drop = constructor.body.findDropObligation(
            candidate.lifecycle.front().source);
        if (drop == nullptr || drop->dropType.type != publication.info.type)
          continue;
        auto malformed = *control;
        auto *changed = instruction(body(malformed, key), candidate.id);
        *changed = {.id = candidate.id,
                    .kind = lang::MirInstructionKind::Drop,
                    .hirValue = candidate.hirValue,
                    .destination = drop->place,
                    .info = {.type = drop->dropType.type,
                             .category = lang::ValueCategory::Place,
                             .access = lang::AccessMode::Mutable},
                    .lifecycle = {{.kind = lang::MirLifecycleEventKind::Drop,
                                   .source = drop->id}}};
        rejected(malformed, "field publication cannot destroy its moved value",
                 "cleanup-bound field publication lacks");
        checkedDestroy = true;
      }
    if (publications.size() < 2)
      continue;
    const auto &first = *publications[0];
    const auto &second = *publications[1];
    expect(first.info.type == second.info.type,
           "wrong-value mutation uses the same nominal carrier type");
    auto wrongValue = *control;
    instruction(body(wrongValue, key), second.id)->operands.front().value =
        first.operands.front().value;
    rejected(
        wrongValue, "publication rejects another same-typed moved value",
        "nested carrier publication does not transfer its exact parameter");
    auto wrongEntry = *control;
    instruction(body(wrongEntry, key), second.id)->operands.front().loan =
        first.operands.front().loan;
    rejected(
        wrongEntry, "publication rejects another same-typed parameter loan",
        "nested carrier publication does not transfer its exact parameter");
    auto wrongField = *control;
    body(wrongField, key).loans[*second.loan - 1].storedField =
        constructor.body.findLoan(*first.loan)->storedField;
    rejected(wrongField, "publication rejects another same-typed field");
    checkedWrongValue = true;
  }
  expect(checkedPublication,
         "nested fixture exercises owned field publication");
  expect(checkedWrongValue,
         "nested fixture contains independent same-typed fields");
  expect(checkedDestroy,
         "nested fixture contains exact field ownership transfer");
}

void checkReturnedAndNestedLifetime() {
  const auto control = analyze("returned-and-nested", R"gti(
Guard acquire(Resource& owner) { return Guard(owner); }
int main() {
  mut Resource owner{};
  { Box<Guard> box{acquire(owner)}; }
  owner.reset(); return 0;
}
)gti");
  if (!control)
    return;
  bool checkedReturn = false;
  bool checkedNested = false;
  for (const auto &function : control->functionInstances()) {
    const BodyKey key{lang::MirBodyKind::Function, function.id};
    for (const auto &loan : function.body.loans) {
      if (!loan.lifecycleBound)
        continue;
      if (loan.kind == lang::MirLoanKind::Return) {
        auto missingMarkers = *control;
        for (auto &candidate : body(missingMarkers, key).loans)
          candidate.lifecycleBound = false;
        rejected(missingMarkers, "factory cannot erase source and return "
                                 "retention markers together");
        auto malformed = *control;
        auto &changed = body(malformed, key);
        for (auto &block : changed.blocks)
          if (block.terminator.returnLoan == loan.id) {
            block.instructions.push_back(
                {.id = freshInstructionId(changed),
                 .kind = lang::MirInstructionKind::EndBorrow,
                 .hirValue = loan.producedBy,
                 .loan = loan.id});
            rejected(malformed, "returned carrier loan remains live at return");
            for (auto &candidate : changed.loans)
              candidate.lifecycleBound = false;
            rejected(
                malformed,
                "unmarked returned carrier dependency remains live at return");
            checkedReturn = true;
            break;
          }
        continue;
      }
      if (loan.entry || loan.escapes || loan.carriers.empty())
        continue;
      const auto end = loanEnd(function.body, loan.id);
      if (!end)
        continue;
      for (const auto &block : function.body.blocks)
        for (const auto &candidate : block.instructions) {
          if (candidate.kind != lang::MirInstructionKind::Drop ||
              !candidate.destination)
            continue;
          const auto *place = function.body.findPlace(*candidate.destination);
          if (place == nullptr ||
              std::find(loan.carriers.begin(), loan.carriers.end(),
                        place->binding) == loan.carriers.end())
            continue;
          auto malformed = *control;
          expect(moveBefore(body(malformed, key), *end, candidate.id),
                 "nested carrier endpoint mutation is applied");
          rejected(malformed,
                   "outer carrier retains dependency through nested drop");
          checkedNested = true;
        }
    }
  }
  expect(checkedReturn, "factory fixture exercises escaping result loan");
  expect(checkedNested, "factory fixture exercises nested result retention");
}

void checkCompleteDependencySet() {
  const auto control = analyze("complete-dependency-set", R"gti(
class Both {
  Resource& first; Resource& second;
public:
  Both(Resource& first, Resource& second) : first(first), second(second) {}
  ~Both() { observed = this.first.value; observed = this.second.value; }
};
Both acquireBoth(Resource& first, Resource& second) { return Both(first, second); }
int main() {
  Resource first{}; Resource second{};
  Both both = acquireBoth(first, second); return 0;
}
)gti");
  if (!control)
    return;
  bool checkedStored = false;
  bool checkedReturn = false;
  for (const auto &function : control->functionInstances()) {
    const BodyKey key{lang::MirBodyKind::Function, function.id};
    for (const auto &loan : function.body.loans) {
      if (!loan.lifecycleBound || loan.additionalDependencies.empty())
        continue;
      for (const std::string mutation :
           {"missing", "wrong-source", "upgraded", "wrong-path",
            "missing-primary-path"}) {
        auto malformed = *control;
        auto &changed = body(malformed, key).loans[loan.id - 1];
        if (mutation == "missing")
          changed.additionalDependencies.clear();
        else if (mutation == "wrong-source")
          changed.additionalDependencies.front().source = changed.source;
        else if (mutation == "upgraded")
          changed.additionalDependencies.front().access =
              lang::AccessMode::Mutable;
        else if (mutation == "wrong-path")
          changed.additionalDependencies.front().designatedFields =
              changed.designatedFields;
        else
          changed.designatedFields.clear();
        rejected(malformed, std::string(loan.kind == lang::MirLoanKind::Return
                                            ? "returned"
                                            : "stored") +
                                " dependency set rejects " + mutation);
      }
      checkedReturn = checkedReturn || loan.kind == lang::MirLoanKind::Return;
      checkedStored = checkedStored || loan.kind == lang::MirLoanKind::Stored;
    }
  }
  expect(checkedStored,
         "multiowner fixture retains complete stored dependency set");
  expect(checkedReturn,
         "multiowner factory retains complete returned dependency set");
}

void checkMandatoryLocalRetention() {
  const auto control = analyze("mandatory-local-retention", R"gti(
int main() {
  mut Resource owner{};
  { Guard guard{owner}; }
  owner.reset(); return 0;
}
)gti");
  if (!control)
    return;
  bool checked = false;
  bool checkedEarly = false;
  bool checkedAuthority = false;
  for (const auto &function : control->functionInstances()) {
    const BodyKey key{lang::MirBodyKind::Function, function.id};
    for (const auto &loan : function.body.loans) {
      if (!loan.lifecycleBound || loan.entry || loan.escapes ||
          loan.carriers.empty())
        continue;
      auto missingMarker = *control;
      body(missingMarker, key).loans[loan.id - 1].lifecycleBound = false;
      rejected(missingMarker,
               "direct local guard cannot erase cleanup retention marker");
      checked = true;
      const auto end = loanEnd(function.body, loan.id);
      for (const auto &block : function.body.blocks) {
        for (const auto &candidate : block.instructions) {
          if (candidate.kind == lang::MirInstructionKind::Construct &&
              candidate.loan == loan.id) {
            for (const auto &carrier : control->classInstances()) {
              if (carrier.type != candidate.info.type)
                continue;
              auto forgedAuthority = *control;
              auto *changed = const_cast<lang::MirClassInstance *>(
                  forgedAuthority.findClassInstance(carrier.id));
              changed->mutexGuardBorrowCarrier = true;
              rejected(forgedAuthority,
                       "ordinary guard cannot forge mutex authority",
                       "same-unit mutex guard storage/token owner");
              checkedAuthority = true;
            }
          }
          if (!end || block.activeFailure != 0 ||
              candidate.kind != lang::MirInstructionKind::Drop ||
              !candidate.destination)
            continue;
          const auto *place = function.body.findPlace(*candidate.destination);
          if (place == nullptr ||
              std::find(loan.carriers.begin(), loan.carriers.end(),
                        place->binding) == loan.carriers.end())
            continue;
          auto early = missingMarker;
          auto &changed = body(early, key);
          expect(moveBefore(changed, *end, candidate.id),
                 "unmarked direct local endpoint mutation is applied");
          // Retire once before the destructor, not once on each Invoke
          // successor. Leave the constructor's independent failure exit intact.
          if (block.terminator.kind == lang::MirTerminatorKind::Invoke &&
              block.terminator.invokeInstruction == candidate.id) {
            for (const auto successor :
                 {block.terminator.target, block.terminator.elseTarget}) {
              auto &successorBlock = changed.blocks[successor - 1];
              std::erase_if(successorBlock.instructions, [&](const auto &i) {
                return i.kind == lang::MirInstructionKind::EndBorrow &&
                       i.loan == loan.id;
              });
            }
          }
          rejected(
              early,
              "unmarked direct local guard cannot retire before destructor");
          checkedEarly = true;
        }
      }
    }
  }
  expect(checked && checkedEarly && checkedAuthority,
         "local guard exercises marker, endpoint and mutex authority proofs");
}

void checkTransitiveCleanupDependency() {
  const auto control = analyze("transitive-cleanup", R"gti(
class View {
  Resource& resource;
public:
  View(Resource& resource) : resource(resource) {}
  int read() { return this.resource.value; }
};
class Adapter {
  View& view;
public:
  Adapter(View& view) : view(view) {}
  ~Adapter() { observed = this.view.read(); }
};
int main() {
  Resource resource{};
  View view{resource};
  Adapter adapter{view};
  int divisor = resource.value;
  return 7 / divisor;
}
)gti");
  if (!control)
    return;
  bool checkedNormal = false;
  bool checkedFailure = false;
  for (const auto &function : control->functionInstances()) {
    const BodyKey key{lang::MirBodyKind::Function, function.id};
    for (const auto &loan : function.body.loans) {
      if (!loan.lifecycleBound || loan.storedDependency == 0 ||
          loan.carriers.empty())
        continue;
      for (const auto &block : function.body.blocks) {
        for (const auto &drop : block.instructions) {
          const auto *place = drop.destination
                                  ? function.body.findPlace(*drop.destination)
                                  : nullptr;
          if (drop.kind != lang::MirInstructionKind::Drop || place == nullptr ||
              std::find(loan.carriers.begin(), loan.carriers.end(),
                        place->binding) == loan.carriers.end())
            continue;
          for (const auto &continuation : function.body.blocks) {
            // A failure's cleanup may continue into a shared suffix that
            // runs under its own continuation record.
            if (continuation.activeFailure != block.activeFailure &&
                (block.activeFailure == 0 ||
                 !failurePathReaches(function.body, block.id, continuation.id)))
              continue;
            for (const auto &end : continuation.instructions) {
              if (end.kind != lang::MirInstructionKind::EndBorrow ||
                  end.loan != loan.storedDependency)
                continue;
              auto early = *control;
              expect(moveBefore(body(early, key), end.id, drop.id),
                     "transitive ancestor endpoint mutation is applied");
              rejected(early,
                       "ancestor loan cannot end before retaining "
                       "carrier cleanup",
                       "before its carrier-shell loan");
              (block.activeFailure == 0 ? checkedNormal : checkedFailure) =
                  true;
            }
          }
        }
      }
    }
  }
  expect(checkedNormal && checkedFailure,
         "transitive cleanup fixture covers normal and failure endpoints");
}

void checkConstructedFieldPublication() {
  const auto control = analyze("constructed field publication", R"gti(
class PairGuard {
  Resource& left; Resource& right;
public:
  PairGuard(Resource& left, Resource& right) : left(left), right(right) {}
  ~PairGuard() { observed += this.left.value + this.right.value; }
};
class Reversed {
  PairGuard pair;
public:
  Reversed(Resource& first, Resource& second) : pair(PairGuard(second, first)) {}
};
int main() { Resource first{}; Resource second{}; Reversed owner{first,second}; return 0; }
)gti");
  if (!control)
    return;
  bool checked = false;
  for (const auto &constructor : control->constructorInstances()) {
    const BodyKey key{lang::MirBodyKind::Constructor, constructor.id};
    for (const auto &block : constructor.body.blocks)
      for (const auto &publication : block.instructions) {
        if (publication.kind != lang::MirInstructionKind::Borrow ||
            !publication.loan || publication.operands.size() != 1 ||
            publication.operands.front().kind != lang::MirOperandKind::Loan)
          continue;
        const auto &operand = publication.operands.front();
        const auto *producer = definition(constructor.body, operand.value);
        const auto *retained = constructor.body.findLoan(operand.loan);
        if (!producer ||
            producer->kind != lang::MirInstructionKind::Construct ||
            !retained || retained->entry || producer->borrowFields.size() != 2)
          continue;
        checked = true;
        auto wrongMap = *control;
        auto *forged = instruction(body(wrongMap, key), producer->id);
        std::swap(forged->borrowFields[0].parameter,
                  forged->borrowFields[1].parameter);
        rejected(wrongMap,
                 "nested producer cannot forge reordered formal mapping");

        auto missingOwner = *control;
        body(missingOwner, key)
            .loans[*publication.loan - 1]
            .additionalDependencies.clear();
        rejected(missingOwner,
                 "nested field publication retains every child dependency");

        const auto end = loanEnd(constructor.body, retained->id);
        expect(end.has_value(),
               "constructed carrier has an explicit transfer endpoint");
        if (end) {
          auto early = *control;
          expect(moveBefore(body(early, key), *end, publication.id),
                 "constructed child early endpoint mutation is applied");
          rejected(early,
                   "constructed child cannot retire before field publication");
        }
        for (const auto &stageBlock : constructor.body.blocks)
          for (const auto &stage : stageBlock.instructions)
            for (std::size_t i = 0; i < stage.lifecycle.size(); ++i)
              if (stage.kind == lang::MirInstructionKind::Initialize &&
                  stage.lifecycle[i].kind ==
                      lang::MirLifecycleEventKind::Reparent &&
                  stage.operands.size() == 1 &&
                  stage.operands.front().value == operand.value) {
                auto unarmed = *control;
                auto *changed = instruction(body(unarmed, key), stage.id);
                changed->lifecycle[i].kind =
                    lang::MirLifecycleEventKind::TransferOut;
                changed->lifecycle[i].target = 0;
                rejected(unarmed,
                         "constructed field cannot lose rollback authority");
              }
      }
  }
  expect(checked, "nested constructor fixture exercises child publication");
}

void checkOwnedFieldReference() {
  const auto control = analyze("owned field reference", R"gti(
class Mixed {
  Resource& resource;
  mut int local;
public:
  Mixed(Resource& resource, int local) : resource(resource), local(local) {}
  int& get() { return this.local; }
  Resource& external() { return this.resource; }
  void reset() mut { this.local = 9; }
  ~Mixed() { observed += this.resource.value; }
};
int main() {
  Resource resource{};
  mut Mixed first{resource, 3};
  Mixed second{resource, 5};
  int& item = first.get();
  int value = item;
  first.reset();
  auto moved = std::move(first);
  int& later = moved.get();
  int result = value + later;
  auto final_owner = std::move(moved);
  return result + final_owner.get() - 21;
}
)gti");
  if (!control)
    return;
  bool checked = false;
  for (const auto &function : control->functionInstances()) {
    const BodyKey key{lang::MirBodyKind::Function, function.id};
    for (const auto &block : function.body.blocks) {
      for (const auto &call : block.instructions) {
        const auto *target =
            call.functionTarget
                ? control->findFunctionInstance(*call.functionTarget)
                : nullptr;
        if (call.kind != lang::MirInstructionKind::Call || !call.loan ||
            target == nullptr ||
            target->returnType.kind != lang::SemanticType::Reference ||
            target->returnType.arguments !=
                std::vector<lang::SemanticType>{lang::SemanticType::Int32} ||
            call.borrowOrigin != lang::BorrowOriginKind::Receiver ||
            !call.borrowFields.empty())
          continue;
        const auto *loan = function.body.findLoan(*call.loan);
        const auto *dependency =
            loan ? function.body.findLoan(loan->storedDependency) : nullptr;
        const auto *source =
            loan ? function.body.findPlace(loan->source) : nullptr;
        expect(loan && dependency && source,
               "owned field retains physical and external authorities");
        if (!loan || !dependency || !source)
          continue;
        checked = true;
        auto erased = *control;
        body(erased, key).loans[loan->id - 1].storedDependency = 0;
        rejected(erased,
                 "owned field reference cannot erase its stored dependency");

        auto flattened = *control;
        body(flattened, key).loans[loan->id - 1].source = dependency->source;
        rejected(
            flattened,
            "owned field reference cannot substitute the external resource");

        bool changedOwner = false;
        for (const auto &place : function.body.places) {
          if (place.root != lang::MirPlaceRootKind::Binding ||
              place.type != source->type || !place.projections.empty() ||
              place.binding == source->binding)
            continue;
          for (const auto &other : function.body.loans) {
            if (other.kind != lang::MirLoanKind::Stored ||
                std::find(other.carriers.begin(), other.carriers.end(),
                          place.binding) == other.carriers.end())
              continue;
            auto forged = *control;
            auto &changed = body(forged, key).loans[loan->id - 1];
            changed.source = place.id;
            changed.storedDependency = other.id;
            rejected(
                forged,
                "owned field reference cannot forge a coherent sibling owner");
            changedOwner = true;
            break;
          }
          if (changedOwner)
            break;
        }
        expect(changedOwner,
               "owned field fixture has an independently bound sibling");
        const auto end = loanEnd(function.body, dependency->id);
        expect(end.has_value(), "owned field resource has a checked endpoint");
        if (end) {
          auto early = *control;
          expect(moveBefore(body(early, key), *end, call.id),
                 "owned field ancestor endpoint mutation is applied");
          rejected(
              early,
              "owned field dependency cannot end before reference extraction");
        }
      }
    }
  }
  expect(checked,
         "owned field fixture exercises a scalar receiver-storage result");
}

void checkNestedViewCopy() {
  const auto control = analyze("nested view copy", R"gti(
class View {
  Resource& owner;
public:
  View(Resource& owner) : owner(owner) {}
  Resource& get() { return this.owner; }
};
class Wrapper {
  View view;
  mut int count;
public:
  Wrapper(Resource& owner) : view(View(owner)), count(0) {}
  View binding() { return this.view; }
  void touch() mut { this.count++; }
  int inspect() mut { auto copy = this.binding(); this.touch(); return copy.get().value; }
};
int main() { Resource owner{}; mut Wrapper wrapper{owner}; return wrapper.inspect() - 7; }
)gti");
  if (!control)
    return;
  bool checked = false;
  for (const auto &function : control->functionInstances()) {
    if (function.returnType.kind != lang::SemanticType::Class ||
        function.returnBorrowFields.size() != 1 ||
        function.returnBorrowFields.front().sourceFields.size() != 2 ||
        function.returnBorrowFields.front().resultFields.size() != 1)
      continue;
    const BodyKey key{lang::MirBodyKind::Function, function.id};
    for (const auto &block : function.body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Return ||
          !block.terminator.returnLoan)
        continue;
      checked = true;
      const auto returned = *block.terminator.returnLoan;
      auto wrongPath = *control;
      body(wrongPath, key).loans[returned - 1].designatedFields =
          function.returnBorrowFields.front().sourceFields;
      rejected(wrongPath,
               "nested returned copy must use result-relative field paths");

      auto wrongMap = *control;
      auto &changed = *const_cast<lang::MirFunctionInstance *>(
          wrongMap.findFunctionInstance(function.id));
      changed.returnBorrowFields.front().sourceFields =
          changed.returnBorrowFields.front().resultFields;
      // Keep caller and callee metadata consistent: executable Load evidence
      // must still reject the fabricated source path independently.
      for (const auto &caller : wrongMap.functionInstances())
        for (auto &callerBlock :
             const_cast<lang::MirFunctionInstance &>(caller).body.blocks)
          for (auto &call : callerBlock.instructions)
            if (call.functionTarget == function.id)
              call.borrowFields = changed.returnBorrowFields;
      rejected(wrongMap, "nested source origin is independently proved from "
                         "the returned copy");

      for (const auto &transferBlock : function.body.blocks)
        for (const auto &transfer : transferBlock.instructions) {
          if (transfer.kind != lang::MirInstructionKind::Borrow ||
              transfer.loan != returned)
            continue;
          auto erased = *control;
          instruction(body(erased, key), transfer.id)->operands.front().value =
              0;
          rejected(erased,
                   "nested copy cannot erase its runtime value evidence");
          const auto *producer =
              definition(function.body, transfer.operands.front().value);
          expect(producer && producer->kind == lang::MirInstructionKind::Load,
                 "nested copy has a concrete load producer");
          if (producer) {
            auto forged = *control;
            auto &place =
                body(forged, key).places[producer->operands.front().place - 1];
            place.projections.clear();
            rejected(forged,
                     "nested copy cannot claim an unprojected source shell");
          }
        }
    }
  }
  expect(checked,
         "nested view fixture exercises exact member-copy publication");
}

void checkMappedAliasParent() {
  const auto control = analyze("mapped alias parent", R"gti(
class Projection {
  Resource& resource;
public:
  Projection(Resource& resource) : resource(resource) {}
  int& get() { return this.resource.value; }
};
int main() {
  Resource first{}; Resource second{};
  Projection left{first}; Projection right{second};
  Projection& left_alias = left;
  Projection& right_alias = right;
  int& a = left_alias.get();
  int& b = right_alias.get();
  return a + b - 14;
}
)gti");
  if (!control)
    return;
  bool checked = false;
  for (const auto &function : control->functionInstances()) {
    const BodyKey key{lang::MirBodyKind::Function, function.id};
    for (const auto &block : function.body.blocks)
      for (const auto &call : block.instructions) {
        const auto *child =
            call.loan ? function.body.findLoan(*call.loan) : nullptr;
        if (call.kind != lang::MirInstructionKind::Call ||
            call.borrowFields.size() != 1 || child == nullptr ||
            child->kind != lang::MirLoanKind::CallResult || child->parent == 0)
          continue;
        const auto *parent = function.body.findLoan(child->parent);
        const auto alias =
            std::find_if(function.body.loans.begin(), function.body.loans.end(),
                         [&](const auto &loan) {
                           return loan.storedDependency == child->parent;
                         });
        const auto other =
            std::find_if(function.body.loans.begin(), function.body.loans.end(),
                         [&](const auto &loan) {
                           return parent != nullptr &&
                                  loan.kind == lang::MirLoanKind::Stored &&
                                  loan.id != parent->id &&
                                  loan.source != parent->source &&
                                  loan.access == parent->access &&
                                  !loan.entry && !loan.escapes;
                         });
        if (parent == nullptr || alias == function.body.loans.end() ||
            other == function.body.loans.end())
          continue;
        checked = true;
        auto wrongParent = *control;
        body(wrongParent, key).loans[child->id - 1].parent = other->id;
        rejected(wrongParent,
                 "mapped alias cannot substitute another owner's parent");

        auto wrongDependency = *control;
        body(wrongDependency, key).loans[alias->id - 1].storedDependency =
            other->id;
        rejected(wrongDependency,
                 "mapped alias must retain its exact input dependency");

        auto missingMap = *control;
        instruction(body(missingMap, key), call.id)->borrowFields.clear();
        expect(!lang::verifyMirBody(body(missingMap, key)).valid(),
               "body verifier requires a field map before bypassing alias "
               "shell parent");
        rejected(missingMap,
                 "mapped alias cannot erase its external field contract");

        auto wrongField = *control;
        instruction(body(wrongField, key), call.id)
            ->borrowFields.front()
            .sourceFields.pop_back();
        rejected(wrongField,
                 "mapped alias cannot discard its exact projected field");

        auto wrongAccess = *control;
        instruction(body(wrongAccess, key), call.id)
            ->borrowFields.front()
            .access = lang::AccessMode::Mutable;
        rejected(wrongAccess,
                 "mapped alias cannot upgrade shared field access");

        if (const auto end = loanEnd(function.body, parent->id)) {
          auto early = *control;
          expect(moveBefore(body(early, key), *end, call.id),
                 "mapped alias early parent retirement mutation applies");
          rejected(
              early,
              "mapped alias requires an active selected stored dependency");
        }
        break;
      }
    if (checked)
      break;
  }
  expect(checked,
         "mapped alias fixture exercises two distinct same-typed owners");
}

void checkPassiveCleanupParent() {
  const auto control = analyze("passive cleanup parent", R"gti(
struct View {
  Resource& resource;
  View(Resource& resource) : resource(resource) {}
};
class Holder {
  View view;
public:
  Holder(View view, int divisor) : view(view) { int checked = 1 / divisor; }
  ~Holder() { observed += 1; }
};
int main() {
  Resource first{}; Resource second{};
  View left{first}; View right{second};
  Holder held{left, 1};
  return right.resource.value - 7;
}
)gti");
  if (!control)
    return;
  bool checked = false;
  for (const auto &function : control->functionInstances()) {
    const BodyKey key{lang::MirBodyKind::Function, function.id};
    for (const auto &child : function.body.loans) {
      if (!child.lifecycleBound || child.kind != lang::MirLoanKind::Stored ||
          child.parent == 0 || child.entry || child.escapes)
        continue;
      const auto *parent = function.body.findLoan(child.parent);
      if (parent == nullptr || parent->kind != lang::MirLoanKind::Stored ||
          parent->lifecycleBound ||
          parent->access != lang::AccessMode::ReadOnly)
        continue;
      const auto other = std::find_if(
          function.body.loans.begin(), function.body.loans.end(),
          [&](const auto &loan) {
            return loan.kind == lang::MirLoanKind::Stored &&
                   loan.id != parent->id && loan.source != parent->source &&
                   !loan.lifecycleBound && !loan.entry && !loan.escapes;
          });
      if (other == function.body.loans.end())
        continue;
      checked = true;
      auto wrongParent = *control;
      body(wrongParent, key).loans[child.id - 1].parent = other->id;
      rejected(wrongParent,
               "cleanup-bearing construction cannot substitute passive parent");
      auto wrongAccess = *control;
      body(wrongAccess, key).loans[child.id - 1].access =
          lang::AccessMode::Mutable;
      rejected(wrongAccess,
               "cleanup-bearing child cannot upgrade passive shared parent");
      for (const auto &block : function.body.blocks)
        for (const auto &producer : block.instructions)
          if (producer.loan == child.id &&
              producer.kind == lang::MirInstructionKind::Construct)
            if (const auto end = loanEnd(function.body, parent->id)) {
              auto early = *control;
              expect(moveBefore(body(early, key), *end, producer.id),
                     "passive parent endpoint mutation applies");
              rejected(early, "cleanup child cannot outlive an already retired "
                              "passive parent");
            }
      break;
    }
    if (checked)
      break;
  }
  expect(checked, "cleanup child has an independent local passive parent");
}

void checkRangeOverCleanupOwner() {
  const auto control = analyze("range over cleanup owner", R"gti(
class Cell {
  Resource& resource;
  int value = 7;
public:
  Cell(Resource& resource) : resource(resource) {}
  int& get() { return this.value; }
  ~Cell() { observed += this.resource.value; }
};
struct Finish {
  int position;
  Finish(int position) : position(position) {}
};
struct Cursor {
  Cell& cell;
  mut int position;
  Cursor(Cell& cell, int position) : cell(cell), position(position) {}
  int& operator*() { return this.cell.get(); }
  void operator++() mut { this.position++; }
  bool operator!=(Finish& end) { return this.position != end.position; }
};
class Collection {
  Cell cell;
public:
  Collection(Resource& resource) : cell(Cell(resource)) {}
  Cursor begin() { return Cursor(this.cell, 0); }
  Finish end() { return Finish(1); }
};
struct ResourceView {
  Resource& source;
  ResourceView(Resource& source) : source(source) {}
  Resource& get() { return this.source; }
};
int traverse(ResourceView resource) {
  Collection collection{resource.get()};
  mut int result = 0;
  for (auto& value : collection) { result += value; }
  return result - 7;
}
int main() {
  Resource resource{};
  return traverse(ResourceView(resource));
}
)gti");
  if (!control)
    return;
  bool checkedReferenceField = false;
  for (const auto &constructor : control->constructorInstances()) {
    for (const auto &loan : constructor.body.loans) {
      if (loan.kind != lang::MirLoanKind::Stored || !loan.escapes ||
          loan.storedField == 0 || loan.storedDependency == 0)
        continue;
      checkedReferenceField = true;
      expect(!loan.lifecycleBound,
             "a stored reference does not own the referenced Cell's cleanup");
      auto forgedOwner = *control;
      body(forgedOwner, {lang::MirBodyKind::Constructor, constructor.id})
          .loans[loan.id - 1]
          .lifecycleBound = true;
      rejected(forgedOwner, "reference publication cannot steal cleanup",
               "stored reference field cannot acquire");
    }
  }
  bool checkedAdapter = false;
  bool checkedRange = false;
  for (const auto &function : control->functionInstances()) {
    const BodyKey key{lang::MirBodyKind::Function, function.id};
    for (const auto &loan : function.body.loans) {
      if (loan.kind == lang::MirLoanKind::Stored && !loan.entry &&
          !loan.escapes && loan.storedDependency != 0 && loan.parent == 0) {
        checkedAdapter = true;
        auto missingDependency = *control;
        body(missingDependency, key).loans[loan.id - 1].storedDependency = 0;
        rejected(missingDependency,
                 "returned adapter must retain nested owner's dependency",
                 "carrier shell loan does not preserve");
      }
      if (loan.kind == lang::MirLoanKind::Return && loan.escapes &&
          loan.storedDependency != 0) {
        auto missingReturnDependency = *control;
        body(missingReturnDependency, key).loans[loan.id - 1].storedDependency =
            0;
        rejected(missingReturnDependency,
                 "returned adapter cannot discard its storage dependency");
      }
      const auto *parent = function.body.findLoan(loan.parent);
      const auto *resource =
          parent == nullptr ? nullptr
                            : function.body.findLoan(parent->storedDependency);
      if (loan.kind == lang::MirLoanKind::CallResult && parent != nullptr &&
          parent->kind == lang::MirLoanKind::Local && resource != nullptr &&
          loan.source != resource->source) {
        checkedRange = true;
        auto wrongOwner = *control;
        auto &changed = body(wrongOwner, key).loans[loan.id - 1];
        changed.source = resource->source;
        changed.parent = resource->id;
        rejected(
            wrongOwner,
            "range element cannot replace its storage owner with resource");
      }
    }
  }
  expect(checkedReferenceField, "range cursor stores a checked Cell reference");
  expect(checkedAdapter, "begin publishes an adapter over a nested owner");
  expect(checkedRange, "range element retains physical container storage");
}

void checkExternalReceiverAdapterReturn() {
  const auto control = analyze("external receiver adapter return", R"gti(
class Cursor<T> {
  T& value;
public:
  Cursor(T& value) : value(value) {}
  T& operator*() { return this.value; }
};
class SingleRange<T> {
  T value;
public:
  SingleRange(T value) : value(value) {}
  Cursor<T> begin() { return Cursor<T>(this.value); }
};
class WrappedRange<T> {
  SingleRange<T>& internal;
public:
  WrappedRange(SingleRange<T>& internal) : internal(internal) {}
  Cursor<T> begin() { return this.internal.begin(); }
};
int main() {
  SingleRange<int> range{7};
  WrappedRange<int> wrapper{range};
  auto cursor = wrapper.begin();
  return *cursor - 7;
}
)gti");
  if (!control)
    return;
  bool checked = false;
  for (const auto &function : control->functionInstances()) {
    if (function.returnType.kind != lang::SemanticType::Class ||
        function.returnBorrowFields.size() != 1)
      continue;
    const auto &mapping = function.returnBorrowFields.front();
    if (mapping.origin != lang::BorrowOriginKind::Receiver ||
        mapping.sourceFields.size() != 1 || mapping.resultFields.size() != 1 ||
        mapping.sourceFields == mapping.resultFields)
      continue;
    const BodyKey key{lang::MirBodyKind::Function, function.id};
    const auto shell =
        std::find_if(function.body.places.begin(), function.body.places.end(),
                     [](const auto &place) {
                       return place.root == lang::MirPlaceRootKind::This &&
                              place.projections.empty();
                     });
    expect(shell != function.body.places.end(),
           "external adapter control has a distinct wrapper receiver shell");
    for (const auto &block : function.body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Return ||
          !block.terminator.returnLoan)
        continue;
      const auto returned = *block.terminator.returnLoan;
      const auto *loan = function.body.findLoan(returned);
      expect(loan != nullptr && loan->designatedFields == mapping.resultFields,
             "external adapter publication labels the returned field, not "
             "source field");
      if (loan == nullptr)
        continue;
      checked = true;
      auto missing = *control;
      body(missing, key).loans[returned - 1].designatedFields.clear();
      rejected(missing,
               "external adapter return cannot erase result field identity");

      auto wrongField = *control;
      body(wrongField, key).loans[returned - 1].designatedFields =
          mapping.sourceFields;
      rejected(wrongField, "external adapter return cannot replace result "
                           "field with source field");

      if (shell != function.body.places.end()) {
        expect(loan->source != shell->id,
               "external adapter source is not the wrapper shell");
        auto wrongOwner = *control;
        body(wrongOwner, key).loans[returned - 1].source = shell->id;
        rejected(wrongOwner, "external adapter return cannot replace external "
                             "range with wrapper shell");

        auto forgedMap = wrongOwner;
        auto &changed = *const_cast<lang::MirFunctionInstance *>(
            forgedMap.findFunctionInstance(function.id));
        changed.returnBorrowFields.front().sourceFields.clear();
        for (const auto &caller : forgedMap.functionInstances())
          for (auto &callerBlock :
               const_cast<lang::MirFunctionInstance &>(caller).body.blocks)
            for (auto &call : callerBlock.instructions)
              if (call.functionTarget == function.id)
                call.borrowFields = changed.returnBorrowFields;
        rejected(forgedMap, "external adapter return source is independently "
                            "checked against its producer");
      }
    }
  }
  expect(
      checked,
      "external receiver adapter publishes its exact resolved field mapping");
}

void checkFactoryCarrierAndDestructor() {
  const auto control = analyze("factory carrier and destructor receiver", R"gti(
class Cell<T> {
  Resource& resource;
  T tag;
  int value = 7;
public:
  Cell(Resource& resource, T tag) : resource(resource), tag(tag) {}
  int read() { return this.value; }
  ~Cell() { observed += this.resource.value; }
};
class Reference<T> {
  T& owner;
public:
  Reference(T& owner) : owner(owner) {}
  T& get() { return this.owner; }
};
Reference<T> bind<T>(T& owner) { return Reference<T>(owner); }
class Collection {
  Cell<int> cell;
public:
  Collection(Cell<int> cell) : cell(std::move(cell)) {}
  Reference<Cell<int>> view() { return Reference<Cell<int>>(this.cell); }
};
Reference<Cell<int>> relay(Reference<Cell<int>> source) { return source; }
class Observer<T> {
  Cell<T>& owner;
public:
  Observer(Cell<T>& owner) : owner(owner) {}
  Cell<T>& get() { return this.owner; }
  ~Observer() { observed += this.get().read(); }
};
int main() {
  Resource resource{};
  Cell<int> owner{resource, 1};
  auto reference = bind(owner);
  Collection collection{Cell<int>(resource, 1)};
  auto view = collection.view();
  auto copied = relay(view);
  observed += copied.get().read();
  Observer<int> observer{owner};
  return reference.get().read() - 7;
}
)gti");
  if (!control)
    return;
  bool checkedFactory = false;
  bool checkedReceiver = false;
  for (const auto &function : control->functionInstances()) {
    const BodyKey key{lang::MirBodyKind::Function, function.id};
    for (const auto &block : function.body.blocks)
      for (const auto &call : block.instructions) {
        const auto *target =
            call.functionTarget
                ? control->findFunctionInstance(*call.functionTarget)
                : nullptr;
        if (call.kind != lang::MirInstructionKind::Call || !call.loan ||
            target == nullptr ||
            target->returnType.kind != lang::SemanticType::Class ||
            !((call.borrowOrigin == lang::BorrowOriginKind::Argument &&
               call.parameterTypes.size() == 1 &&
               call.parameterTypes.front().kind ==
                   lang::SemanticType::Reference) ||
              (call.borrowOrigin == lang::BorrowOriginKind::Receiver &&
               call.borrowFields.empty() && call.parameterTypes.empty())))
          continue;
        const auto *loan = function.body.findLoan(*call.loan);
        const auto *dependency =
            loan ? function.body.findLoan(loan->storedDependency) : nullptr;
        expect(loan && dependency && loan->source != dependency->source,
               "factory distinguishes physical shell from external owner");
        if (!loan || !dependency)
          continue;
        checkedFactory = true;
        checkedReceiver |=
            call.borrowOrigin == lang::BorrowOriginKind::Receiver;
        auto missing = *control;
        body(missing, key).loans[loan->id - 1].storedDependency = 0;
        rejected(missing, "factory wrapper cannot discard external dependency");
        auto flattened = *control;
        body(flattened, key).loans[loan->id - 1].source = dependency->source;
        rejected(
            flattened,
            "factory wrapper cannot substitute external resource for shell");
        const auto end = loanEnd(function.body, dependency->id);
        expect(end.has_value(),
               "factory owner dependency has a checked endpoint");
        if (end) {
          auto early = *control;
          expect(moveBefore(body(early, key), *end, call.id),
                 "factory dependency endpoint mutation applies");
          rejected(early, "factory cannot borrow an already released owner");
        }
      }
  }
  expect(checkedFactory, "source factory produces a checked carrier shell");
  expect(checkedReceiver, "receiver factory produces a checked carrier shell");
  bool checkedDestructor = false;
  for (const auto &destructor : control->destructorInstances()) {
    const BodyKey key{lang::MirBodyKind::Destructor, destructor.id};
    for (const auto &loan : destructor.body.loans) {
      const auto *dependency = destructor.body.findLoan(loan.storedDependency);
      if (loan.kind != lang::MirLoanKind::CallResult || dependency == nullptr)
        continue;
      checkedDestructor = true;
      expect(dependency->entry && !dependency->lifecycleBound &&
                 dependency->kind == lang::MirLoanKind::Stored &&
                 !dependency->designatedFields.empty(),
             "destructor receives exact non-owning caller authority");
      auto missing = *control;
      body(missing, key).loans[loan.id - 1].storedDependency = 0;
      rejected(missing, "destructor accessor cannot erase caller dependency");
      auto forged = *control;
      body(forged, key).loans[dependency->id - 1].lifecycleBound = true;
      rejected(forged,
               "destructor entry cannot acquire another cleanup obligation");
      auto erasedPath = *control;
      body(erasedPath, key).loans[dependency->id - 1].designatedFields.clear();
      rejected(erasedPath,
               "destructor entry cannot erase exact reference field");
    }
  }
  expect(checkedDestructor,
         "generic destructor uses a carrier-reference accessor");
}

void checkSharedOriginFieldIdentities() {
  const auto control = analyze("two guarded fields sharing one origin", R"gti(
class Handle {
  Resource& resource;
public:
  Handle(Resource& resource) : resource(resource) {}
  int read() { return this.resource.value; }
};
class Retained {
  Handle handle;
public:
  Retained(Handle handle) : handle(handle) {}
  ~Retained() { observed = this.handle.read(); }
};
class Pair {
  Retained first;
  Retained second;
public:
  Pair(Handle handle) : first(Retained(handle)), second(Retained(handle)) {}
};
void use(Handle handle) { Pair pair{handle}; }
int main() { Resource resource{}; use(Handle(resource)); return 0; }
)gti");
  if (!control)
    return;
  bool checked = false;
  for (const auto &function : control->functionInstances()) {
    const BodyKey key{lang::MirBodyKind::Function, function.id};
    for (const auto &loan : function.body.loans) {
      if (loan.entry || !loan.lifecycleBound || loan.parent == 0 ||
          loan.designatedFields.empty() ||
          loan.additionalDependencies.size() != 1)
        continue;
      checked = true;
      expect(loan.source == loan.additionalDependencies.front().source &&
                 loan.designatedFields !=
                     loan.additionalDependencies.front().designatedFields,
             "same referent retains two distinct nested field identities");
      auto missing = *control;
      body(missing, key).loans[loan.id - 1].additionalDependencies.clear();
      rejected(std::move(missing), "second field dependency cannot disappear");
      auto collapsed = *control;
      body(collapsed, key)
          .loans[loan.id - 1]
          .additionalDependencies.front()
          .designatedFields = loan.designatedFields;
      rejected(std::move(collapsed),
               "same-owner fields cannot collapse to one path");
      const auto end = loanEnd(function.body, loan.id);
      bool early = false;
      if (end)
        for (const auto &block : function.body.blocks)
          for (const auto &drop : block.instructions)
            if (!early && drop.kind == lang::MirInstructionKind::Drop &&
                drop.destination) {
              const auto *place = function.body.findPlace(*drop.destination);
              if (place == nullptr ||
                  std::find(loan.carriers.begin(), loan.carriers.end(),
                            place->binding) == loan.carriers.end())
                continue;
              auto released = *control;
              early = moveBefore(body(released, key), *end, drop.id);
              expect(early, "guarded pair has a reachable owner drop");
              rejected(std::move(released),
                       "shared-origin loan cannot end before both guards drop");
            }
      expect(early, "shared-origin mutation exercises early release");
    }
  }
  expect(checked, "shared-origin mutation exercises multi-field publication");
}

void checkProjectedEntryConstruction() {
  const auto control = analyze("projected entry construction", R"gti(
class MutGuard {
  mut int& target;
public:
  MutGuard(mut int& target) : target(target) {}
  ~MutGuard() { this.target += 1; }
};
class Parent {
  mut int& first;
  mut int& second;
public:
  Parent(mut int& first, mut int& second) : first(first), second(second) {}
  void apply() mut {
    int other = this.second;
    MutGuard child{this.first};
  }
};
int main() { mut int first = 7; mut int second = 9;
  { mut Parent parent{first, second}; parent.apply(); }
  return first - 8;
}
)gti");
  if (!control)
    return;
  bool checked = false;
  for (const auto &function : control->functionInstances()) {
    for (const auto &loan : function.body.loans) {
      const auto *parent = function.body.findLoan(loan.parent);
      const auto *origin = function.body.findPlace(loan.source);
      if (!loan.lifecycleBound || !parent || !parent->entry || !origin ||
          parent->designatedFields.empty() ||
          origin->root != lang::MirPlaceRootKind::This ||
          origin->projections.size() != 2)
        continue;
      const BodyKey key{lang::MirBodyKind::Function, function.id};
      auto readonly = *control;
      body(readonly, key).loans[parent->id - 1].access =
          lang::AccessMode::ReadOnly;
      rejected(readonly, "projected child cannot upgrade parent access");
      for (const auto &place : function.body.places) {
        if (place.root != origin->root ||
            place.projections.size() != origin->projections.size() ||
            place.projections.front().kind != lang::MirProjectionKind::Field ||
            place.projections.front().field ==
                origin->projections.front().field ||
            place.projections.back().kind !=
                lang::MirProjectionKind::Dereference)
          continue;
        auto redirected = *control;
        body(redirected, key).loans[loan.id - 1].source = place.id;
        rejected(redirected,
                 "projected child origin must match its selected argument");
        auto wrongField = *control;
        body(wrongField, key).loans[parent->id - 1].designatedFields = {
            place.projections.front().field};
        rejected(wrongField,
                 "projected child cannot borrow through another entry field");
        checked = true;
        break;
      }
    }
  }
  expect(checked,
         "projected entry mutations exercise a distinct receiver field");
}

void checkForwardedExternalFieldRetention() {
  const auto control = analyze("forwarded external field retention", R"gti(
struct Pair {
  int& first;
  int& second;
  Pair(int& first, int& second) : first(first), second(second) {}
  int sum() { return this.first + this.second; }
};
Pair forward(Pair pair) { return pair; }
int observe(Pair& pair) { return forward(pair).sum(); }
int main() { int first = 20; int second = 22; Pair pair{first, second};
  return observe(pair) - 42;
}
)gti");
  if (!control)
    return;
  bool checked = false;
  for (const auto &function : control->functionInstances()) {
    for (const auto &child : function.body.loans) {
      const auto *source = function.body.findPlace(child.source);
      if (child.entry || child.kind != lang::MirLoanKind::Stored ||
          child.additionalDependencies.empty() || !source ||
          source->root != lang::MirPlaceRootKind::Binding ||
          source->projections.size() != 3)
        continue;
      for (const auto &entry : function.body.loans) {
        const auto *origin = function.body.findPlace(entry.source);
        if (!entry.entry || entry.kind != lang::MirLoanKind::Stored ||
            entry.designatedFields.empty() || !origin ||
            origin->binding != source->binding)
          continue;
        const auto externalEnd = loanEnd(function.body, entry.id);
        const auto childEnd = loanEnd(function.body, child.id);
        if (!externalEnd || !childEnd)
          continue;
        auto forged = *control;
        const BodyKey key{lang::MirBodyKind::Function, function.id};
        expect(moveBefore(body(forged, key), *externalEnd, *childEnd),
               "external-field mutation moves its exact entry endpoint");
        rejected(forged,
                 "forwarded fields retain their external owner after shell end",
                 "is ended before its child reborrow");
        checked = true;
      }
    }
  }
  expect(checked, "forwarded fields exercise an abstract external entry");
}

} // namespace

int main() {
  const auto moved = analyze("entry carrier moves", R"gti(
void consume(Guard guard, Guard unrelated) {
  auto first = std::move(guard);
  auto second = std::move(first);
}
int main() {
  Resource left{};
  Resource right{};
  consume(Guard(left), Guard(right));
  return 0;
}
)gti");
  bool checkedMoves = false;
  if (moved) {
    for (const auto &function : moved->functionInstances()) {
      for (const auto &loan : function.body.loans) {
        if (!loan.entry || !loan.lifecycleBound || loan.carriers.size() < 2)
          continue;
        const auto *source = function.body.findPlace(loan.source);
        if (!source)
          continue;
        const BodyKey key{lang::MirBodyKind::Function, function.id};
        auto erased = *moved;
        std::erase(body(erased, key).loans[loan.id - 1].carriers,
                   source->binding);
        rejected(erased,
                 "moving an entry owner cannot erase its formal anchor");
        auto unmarked = *moved;
        body(unmarked, key).loans[loan.id - 1].lifecycleBound = false;
        rejected(unmarked,
                 "moving an entry owner cannot erase lifecycle retention");
        for (const auto &other : function.body.loans) {
          if (!other.entry || !other.lifecycleBound || other.id == loan.id)
            continue;
          const auto *unrelated = function.body.findPlace(other.source);
          if (!unrelated)
            continue;
          auto forged = *moved;
          body(forged, key)
              .loans[loan.id - 1]
              .carriers.push_back(unrelated->binding);
          rejected(forged,
                   "unrelated parameter cannot acquire moved provenance",
                   "owned entry carrier lacks an exact lifecycle transfer");
          checkedMoves = true;
        }
        for (const auto &block : function.body.blocks) {
          for (const auto &step : block.instructions) {
            if (step.kind != lang::MirInstructionKind::Move ||
                step.lifecycle.empty())
              continue;
            auto forged = *moved;
            instruction(body(forged, key), step.id)->lifecycle.clear();
            rejected(forged,
                     "entry carrier cannot move without ownership transfer");
          }
        }
      }
    }
  }
  expect(checkedMoves,
         "entry movement mutations exercise independent parameters");
  checkOwnedCall();
  checkClosureCarrier();
  checkNestedPublication();
  checkReturnedAndNestedLifetime();
  checkCompleteDependencySet();
  checkMandatoryLocalRetention();
  checkTransitiveCleanupDependency();
  checkConstructedFieldPublication();
  checkOwnedFieldReference();
  checkNestedViewCopy();
  checkMappedAliasParent();
  checkPassiveCleanupParent();
  checkRangeOverCleanupOwner();
  checkExternalReceiverAdapterReturn();
  checkFactoryCarrierAndDestructor();
  checkSharedOriginFieldIdentities();
  checkProjectedEntryConstruction();
  checkForwardedExternalFieldRetention();
  if (failures == 0)
    std::cout << mutations << " cleanup-bound proof mutations rejected\n";
  return failures == 0 ? 0 : 1;
}
