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
            if (continuation.activeFailure != block.activeFailure)
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
        if (!producer || producer->kind != lang::MirInstructionKind::Construct ||
            !retained || retained->entry || producer->borrowFields.size() != 2)
          continue;
        checked = true;
        auto wrongMap = *control;
        auto *forged = instruction(body(wrongMap, key), producer->id);
        std::swap(forged->borrowFields[0].parameter, forged->borrowFields[1].parameter);
        rejected(wrongMap, "nested producer cannot forge reordered formal mapping");

        auto missingOwner = *control;
        body(missingOwner, key).loans[*publication.loan - 1].additionalDependencies.clear();
        rejected(missingOwner, "nested field publication retains every child dependency");

        const auto end = loanEnd(constructor.body, retained->id);
        expect(end.has_value(), "constructed carrier has an explicit transfer endpoint");
        if (end) {
          auto early = *control;
          expect(moveBefore(body(early, key), *end, publication.id),
                 "constructed child early endpoint mutation is applied");
          rejected(early, "constructed child cannot retire before field publication");
        }
        for (const auto &stageBlock : constructor.body.blocks)
          for (const auto &stage : stageBlock.instructions)
            for (std::size_t i = 0; i < stage.lifecycle.size(); ++i)
              if (stage.kind == lang::MirInstructionKind::Initialize &&
                  stage.lifecycle[i].kind == lang::MirLifecycleEventKind::Reparent &&
                  stage.operands.size() == 1 && stage.operands.front().value == operand.value) {
                auto unarmed = *control;
                auto *changed = instruction(body(unarmed, key), stage.id);
                changed->lifecycle[i].kind = lang::MirLifecycleEventKind::TransferOut;
                changed->lifecycle[i].target = 0;
                rejected(unarmed, "constructed field cannot lose rollback authority");
              }
      }
  }
  expect(checked, "nested constructor fixture exercises child publication");
}

} // namespace

int main() {
  checkOwnedCall();
  checkNestedPublication();
  checkReturnedAndNestedLifetime();
  checkCompleteDependencySet();
  checkMandatoryLocalRetention();
  checkTransitiveCleanupDependency();
  checkConstructedFieldPublication();
  if (failures == 0)
    std::cout << mutations << " cleanup-bound proof mutations rejected\n";
  return failures == 0 ? 0 : 1;
}
