#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/mir_editor.h"
#include "gti/mir_lowering.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void check(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
lang::FrontendResult analyze(std::string source) {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze("macro-values.gti", std::move(source),
                                  {library / "prelude.gti"}, {}, {library});
}
void diagnostic(std::string source, std::string_view fragment) {
  const auto result = analyze(std::move(source));
  check(!result.canGenerateCode(),
        "invalid result macro must stop before emission");
  const bool found =
      std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                  [&](const auto &value) {
                    return value.message.find(fragment) != std::string::npos;
                  });
  check(found, fragment);
  if (!found)
    for (const auto &value : result.diagnostics)
      std::cerr << value.code << ": " << value.message << '\n';
  check(
      std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                   [](const auto &value) { return value.code == "GTI-B0001"; }),
      "unsupported result shapes need a source diagnostic, not an ICE");
}
void positiveAndMutation() {
  const std::string source = R"gti(
#macro int choose(bool enabled, int first, int second) {
  if (enabled) { return first; }
  return second;
}
#macro int once(int expression) { int saved = expression; return saved + saved; }
#macro int nested(int expression) { return choose(true, once(expression), 0); }
#macro stop() { return; }
int main() {
  mut int effects = 0;
  int answer = choose(false, ++effects, nested(5));
  stop();
  return answer - 10 + effects;
}
)gti";
  auto result = analyze(source);
  if (!result.canGenerateCode())
    for (const auto &value : result.diagnostics)
      std::cerr << value.code << ": " << value.message << '\n';
  check(result.canGenerateCode(),
        "scoped scalar and void result regions must lower");
  if (!result.canGenerateCode())
    return;
  check(lang::verifyHirProgramPlans(result.semantics, result.hir).valid(),
        "result-region HIR must verify independently");
  check(lang::verifyMirProgram(result.mir).valid(),
        "result-region MIR must verify independently");
  const auto inspected = lang::LanguageQueries().macroExpansion(
      result, result.sourceGraph.entryUnit(), source.find("choose(false") + 2);
  check(inspected.has_value(),
        "value calls must support read-only expansion inspection");
  bool mutated = false;
  for (const auto &function : result.hir.functionInstances()) {
    for (const auto &statement : function.body.statements) {
      if (statement.resultRegion == 0)
        continue;
      auto malformed = result.hir;
      auto *changed = const_cast<lang::HirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      const auto found = std::find_if(
          changed->body.statements.begin(), changed->body.statements.end(),
          [&](const auto &candidate) { return candidate.id == statement.id; });
      found->resultRegion = 0;
      check(!lang::verifyHirProgramPlans(result.semantics, malformed).valid(),
            "erasing a macro-local return target must not create a caller "
            "return");
      check(
          !lang::MirLowerer().lower(malformed, result.failureMetadata).valid(),
          "MIR lowering must reject an erased local return target");
      found->resultRegion = statement.resultRegion + 100000;
      check(!lang::verifyHirProgramPlans(result.semantics, malformed).valid(),
            "forged cross-region returns must be rejected");
      found->resultRegion = statement.resultRegion;
      found->statements.push_back(found->id);
      check(!lang::verifyHirProgramPlans(result.semantics, malformed).valid(),
            "cyclic result-region statements must be rejected");
      mutated = true;
      break;
    }
    if (mutated)
      break;
  }
  check(mutated, "fixture must retain a macro-local return identity");
}
void owningResults() {
  const auto result = analyze(R"gti(
#include <std/string>
#macro std::string select(bool enabled, std::string first, std::string second) {
  if (enabled) { return first; }
  return second;
}
#macro std::string local(std::string expression) {
  std::string saved = expression;
  return saved;
}
int main() {
  std::string untouched{"untouched"};
  std::string selected = select(false, std::move(untouched), local(std::string("yes")));
  return int32_t(untouched.size() + selected.size());
}
)gti");
  if (!result.canGenerateCode())
    for (const auto &value : result.diagnostics)
      std::cerr << value.code << ": " << value.message << '\n';
  check(result.canGenerateCode(),
        "move-only results and skipped moves must preserve caller ownership");
}

void receiverResultLivenessMutations() {
  const auto result = analyze(R"gti(
class Owner {
  int number;
public:
  Owner(int value) : number(value) {}
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
  ~Owner() {}
  int read() { return this.number; }
};
struct Passive {
  int number;
  Passive(int value) : number(value) {}
  int read() { return this.number; }
};
#macro Owner choose_owner(bool enabled, Owner first, Owner second) {
  Owner guard{99};
  if (enabled) { return first; }
  return second;
}
#macro Passive choose_passive(bool enabled, Passive first, Passive second) {
  if (enabled) { return first; }
  return second;
}
int inspect_owner(Owner& owner) { return owner.read(); }
int inspect_passive(Passive& value) { return value.read(); }
int exercise_owned(bool enabled) {
  Owner sibling{91};
  int observed = choose_owner(enabled, Owner(7), Owner(9)).read();
  return observed + sibling.read();
}
int exercise_argument(bool enabled) {
  Owner sibling{91};
  int observed = inspect_owner(choose_owner(enabled, Owner(7), Owner(9)));
  return observed + sibling.read();
}
int exercise_passive(bool enabled) {
  int observed = choose_passive(enabled, Passive(3), Passive(5)).read();
  return observed + inspect_passive(choose_passive(enabled, Passive(3), Passive(5)));
}
int main() {
  return exercise_owned(true) + exercise_owned(false) +
         exercise_argument(true) + exercise_argument(false) +
         exercise_passive(true) + exercise_passive(false) - 412;
}
)gti");
  if (!result.canGenerateCode())
    for (const auto &value : result.diagnostics)
      std::cerr << value.code << ": " << value.message << '\n';
  check(result.canGenerateCode(),
        "all normal result edges initialize owning and passive receivers and "
        "ordinary user reference arguments");
  if (!result.canGenerateCode())
    return;
  check(lang::verifyMirProgram(result.mir).valid(),
        "unknown-branch result receivers, reference arguments, and local "
        "cleanup verify positively");
  const auto exerciseInput = [&](std::string_view functionName,
                                 lang::MirCallInputRole role) {
    const lang::HirFunctionInstance *hirFunction = nullptr;
    for (const auto &function : result.hir.functionInstances())
      if (function.source && function.source->name().lexeme == functionName) {
        hirFunction = &function;
        break;
      }
    const auto *function =
        hirFunction ? result.mir.findFunctionInstance(hirFunction->id)
                    : nullptr;
    check(function != nullptr, "fixture retains the owning input function");
    if (!function)
      return;
    const auto &body = function->body;
    const lang::MirInstruction *input = nullptr;
    lang::MirBlockId inputBlock = 0;
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CallInput ||
            instruction.callInputRole != role ||
            instruction.callInputKind !=
                lang::HirCallInputKind::ReadTemporaryBorrow ||
            instruction.operands.size() != 1)
          continue;
        const auto *place = body.findPlace(instruction.operands.front().place);
        if (place && place->root == lang::MirPlaceRootKind::Temporary) {
          input = &instruction;
          inputBlock = block.id;
          break;
        }
      }
    check(input != nullptr, "fixture borrows an exact hidden result slot");
    if (!input)
      return;
    const auto *place = body.findPlace(input->operands.front().place);
    const auto resultDrop =
        std::find_if(body.dropObligations.begin(), body.dropObligations.end(),
                     [&](const auto &drop) {
                       return drop.kind == lang::MirDropObligationKind::Value &&
                              drop.value == input->hirValue &&
                              drop.place == place->id;
                     });
    const auto namedDrop = [&](std::string_view name) {
      for (const auto &binding : hirFunction->body.bindings)
        if (binding.variable && binding.variable->name().lexeme == name)
          for (const auto &drop : body.dropObligations)
            if (drop.kind == lang::MirDropObligationKind::Binding &&
                drop.binding == binding.id)
              return drop.id;
      return lang::MirDropObligationId{0};
    };
    const auto siblingDrop = namedDrop("sibling");
    const auto guardDrop = namedDrop("guard");
    check(
        resultDrop != body.dropObligations.end() && siblingDrop != 0 &&
            guardDrop != 0,
        "fixture retains distinct result, sibling, and macro-local lifetimes");
    if (resultDrop == body.dropObligations.end() || siblingDrop == 0 ||
        guardDrop == 0)
      return;
    check(body.findDropObligation(siblingDrop)->dropType.type == place->type,
          "the sibling is an independent same-type live owner");
    const auto instruction =
        [](auto &changed, lang::MirInstructionId id) -> lang::MirInstruction * {
      for (auto &block : changed.blocks)
        for (auto &candidate : block.instructions)
          if (candidate.id == id)
            return &candidate;
      return nullptr;
    };
    const auto rejected = [&](lang::MirBody changed, std::string_view message,
                              std::string_view expectedError) {
      lang::rebuildMirReachability(changed);
      check(lang::rebuildMirValueUses(changed),
            "liveness mutations retain coherent value-use indexes");
      const auto verification = lang::verifyMirBody(changed, function->id);
      const bool exactFailure = std::any_of(
          verification.errors.begin(), verification.errors.end(),
          [&](const auto &error) {
            return error.message.find(expectedError) != std::string::npos;
          });
      check(exactFailure, message);
      if (!exactFailure)
        for (const auto &error : verification.errors)
          std::cerr << functionName << " liveness mutation: " << error.message
                    << '\n';
    };
    std::vector<lang::MirInstructionId> initializers;
    lang::MirInstructionId resultCleanup = 0;
    lang::MirInstructionId localCleanup = 0;
    lang::MirBlockId localCleanupBlock = 0;
    bool unknownBranch = false;
    for (const auto &block : body.blocks) {
      unknownBranch |=
          block.terminator.kind == lang::MirTerminatorKind::Branch &&
          block.terminator.value &&
          block.terminator.value->kind != lang::MirOperandKind::Constant;
      for (const auto &step : block.instructions) {
        if (step.kind == lang::MirInstructionKind::Initialize &&
            step.destination == place->id &&
            std::any_of(step.lifecycle.begin(), step.lifecycle.end(),
                        [&](const auto &event) {
                          return event.target == resultDrop->id;
                        }))
          initializers.push_back(step.id);
        if (step.kind != lang::MirInstructionKind::Drop ||
            block.activeFailure != 0)
          continue;
        for (const auto &event : step.lifecycle) {
          if (event.source == resultDrop->id && block.id == inputBlock)
            resultCleanup = step.id;
          if (event.source == guardDrop && block.id != inputBlock) {
            localCleanup = step.id;
            localCleanupBlock = block.id;
          }
        }
      }
    }
    check(
        unknownBranch && initializers.size() == 2 && resultCleanup != 0 &&
            localCleanup != 0,
        "borrow-input fixture has two unknown normal edges and exact cleanups");
    if (!unknownBranch || initializers.size() != 2 || resultCleanup == 0 ||
        localCleanup == 0)
      return;
    {
      auto missing = body;
      auto &events = instruction(missing, initializers.front())->lifecycle;
      const auto activation =
          std::find_if(events.begin(), events.end(), [&](const auto &event) {
            return event.target == resultDrop->id;
          });
      // Preserve consumption of the return expression's own temporary. Only
      // activation of the published result is missing on this incoming edge.
      if (activation->source != 0)
        *activation = {.kind = lang::MirLifecycleEventKind::TransferOut,
                       .source = activation->source};
      else
        events.erase(activation);
      for (auto &block : missing.blocks)
        for (auto &step : block.instructions)
          if (step.kind == lang::MirInstructionKind::Drop)
            for (auto &event : step.lifecycle)
              if (event.source == resultDrop->id)
                event.conditional = true;
      rejected(std::move(missing),
               "a result borrow requires activation on every unknown edge, "
               "even when conditional cleanup is coherent",
               "temporary borrow requires its exact owner to be live");
    }
    {
      auto retargeted = body;
      auto &events = instruction(retargeted, initializers.front())->lifecycle;
      const auto activation =
          std::find_if(events.begin(), events.end(), [&](const auto &event) {
            return event.target == resultDrop->id;
          });
      activation->target = siblingDrop;
      rejected(std::move(retargeted),
               "another same-type owner's activation cannot publish the result "
               "slot",
               "lifecycle event does not match its instruction");
    }
    const auto moveCleanup = [&](lang::MirBody &changed,
                                 lang::MirInstructionId cleanup,
                                 lang::MirBlockId sourceBlock,
                                 bool conditional) {
      auto &source = changed.blocks[sourceBlock - 1].instructions;
      const auto found =
          std::find_if(source.begin(), source.end(),
                       [&](const auto &step) { return step.id == cleanup; });
      auto moved = *found;
      source.erase(found);
      for (auto &event : moved.lifecycle)
        event.conditional = conditional;
      auto &destination = changed.blocks[inputBlock - 1].instructions;
      const auto before =
          std::find_if(destination.begin(), destination.end(),
                       [&](const auto &step) { return step.id == input->id; });
      destination.insert(before, std::move(moved));
    };
    {
      auto expired = body;
      // Move the exact existing Drop (no invented identity or stale index)
      // before the borrow. The sibling remains active throughout the call.
      // The earlier exact-boundary gate must already reject this rescheduling.
      moveCleanup(expired, resultCleanup, inputBlock, false);
      rejected(
          std::move(expired),
          "an early exact result drop cannot borrow another live same-type "
          "owner's lifetime",
          "full-expression cleanup is not connected to exactly one boundary "
          "marker");
    }
    {
      auto delayedLocal = body;
      // Cleanup still occurs before body exit, but one local return now leaves
      // its lexical boundary with the guard live. The join's conditional Drop
      // must not substitute for return-edge cleanup. Exact cleanup/marker
      // connectivity rejects the reschedule before lifecycle convergence.
      moveCleanup(delayedLocal, localCleanup, localCleanupBlock, true);
      rejected(
          std::move(delayedLocal),
          "a macro-local owner must be cleaned at its region return, not "
          "after crossing into the caller's input evaluation",
          "cleanup sequence is not connected to its exact boundary marker");
    }
  };
  exerciseInput("exercise_owned", lang::MirCallInputRole::Receiver);
  exerciseInput("exercise_argument", lang::MirCallInputRole::Argument);
}

void programInitializerLocalMetadataMutations() {
  const auto result = analyze(R"gti(
struct Count {
  int number;
  Count(int value) : number(value) {}
};
#macro Count local_count(int expression) {
  Count saved{expression};
  Count& alias = saved;
  int copied = saved.number;
  auto [unused_number] = saved;
  auto [read_number] = saved;
  int projected = read_number;
  int32_t[2] elements = {1, 2};
  auto [unused_first, unused_second] = elements;
  return saved;
}
Count first = local_count(7);
struct Registry { static Count second = local_count(9);  public: Registry() = default; };
int main() { return first.number + Registry::second.number - 16; }
)gti");
  if (!result.canGenerateCode())
    for (const auto &value : result.diagnostics)
      std::cerr << value.code << ": " << value.message << '\n';
  check(result.canGenerateCode(),
        "namespace/static initialization retains ordinary user-type locals");
  if (!result.canGenerateCode())
    return;
  bool checkedIndices = false;
  for (const auto &statement : result.hir.module().statements) {
    if (statement.kind != lang::HirStatementKind::StructuredBinding ||
        statement.structuredBindings.size() != 2 ||
        !statement.structuredBindings.front().index)
      continue;
    const auto id = *statement.structuredBindings.front().index;
    for (int mutation = 0; mutation < 3; ++mutation) {
      auto malformed = result.hir;
      auto &body = const_cast<lang::HirBody &>(malformed.module());
      auto value = std::find_if(
          body.values.begin(), body.values.end(),
          [&](const auto &candidate) { return candidate.id == id; });
      if (mutation == 0)
        value->literal = lang::Literal{std::uint64_t{99}};
      else if (mutation == 1) {
        value->info.type = lang::SemanticType::UInt32;
        value->info.traits = lang::semanticTraits(lang::SemanticType::UInt32);
      } else {
        auto changed =
            std::find_if(body.statements.begin(), body.statements.end(),
                         [&](const auto &candidate) {
                           return candidate.id == statement.id;
                         });
        changed->structuredBindings.back().index = id;
      }
      const auto verification =
          lang::verifyHirProgramPlans(result.semantics, malformed);
      check(std::any_of(verification.errors.begin(), verification.errors.end(),
                        [](const auto &error) {
                          return error.find(
                                     "source-less module initializer value") !=
                                 std::string::npos;
                        }),
            "source-less array ordinals require exact type, semantic index, "
            "and unique decomposition identity");
    }
    checkedIndices = true;
    break;
  }
  check(checkedIndices, "startup fixture retains semantically witnessed "
                        "structured array indices");
  const auto &initialization = result.mir.programInitializationPlan();
  std::vector<std::size_t> selected;
  for (std::size_t index = 0; index < initialization.steps.size(); ++index)
    if (initialization.steps[index].locals.size() >= 2 &&
        !initialization.steps[index].nestedFullExpressions.empty())
      selected.push_back(index);
  check(selected.size() == 2,
        "both initializer steps retain local and nested-expression metadata");
  if (selected.size() != 2)
    return;
  const auto firstIndex = selected[0], secondIndex = selected[1];
  const auto classLocal = [&](const auto &step) {
    for (const auto &local : step.locals)
      if (result.mir.module().findPlace(local.place)->type.kind ==
          lang::SemanticType::Class)
        return local;
    return lang::MirProgramInitializationLocal{};
  };
  const auto first = classLocal(initialization.steps[firstIndex]);
  const auto second = classLocal(initialization.steps[secondIndex]);
  check(first.place != 0 && second.place != 0,
        "fixture retains two distinct ordinary user-type local places");
  if (first.place == 0 || second.place == 0)
    return;
  lang::MirProgramInitializationLocal view;
  for (const auto &local : initialization.steps[firstIndex].locals) {
    const auto *place = result.mir.module().findPlace(local.place);
    if (place && place->binding != local.binding &&
        place->projections.front().kind == lang::MirProjectionKind::Field) {
      view = local;
      break;
    }
  }
  check(view.place != 0,
        "structured local metadata names a final physical projection, not an "
        "unused placeholder binding");
  if (view.place == 0)
    return;
  const auto rejected = [&](std::string_view message,
                            std::string_view expectedError,
                            const auto &mutate) {
    auto malformed = result.mir;
    auto &plan = const_cast<lang::MirProgramInitializationPlan &>(
        malformed.programInitializationPlan());
    auto &body = const_cast<lang::MirBody &>(malformed.module());
    check(mutate(plan, body),
          "startup metadata mutation finds its exact target");
    lang::rebuildMirReachability(body);
    check(lang::rebuildMirValueUses(body),
          "startup mutations retain coherent value-use indexes");
    const auto bodyVerification = lang::verifyMirBody(body);
    check(bodyVerification.valid(),
          "startup mutation remains valid under generic body verification");
    if (!bodyVerification.valid())
      for (const auto &error : bodyVerification.errors)
        std::cerr << "startup body mutation: " << error.message << '\n';
    const auto verification = lang::verifyMirProgram(malformed);
    const bool exactFailure = std::any_of(
        verification.errors.begin(), verification.errors.end(),
        [&](const auto &error) {
          return error.message.find(expectedError) != std::string::npos;
        });
    check(exactFailure, message);
    if (!exactFailure)
      for (const auto &error : verification.errors)
        std::cerr << "startup metadata mutation: " << error.message << '\n';
  };
  rejected("erasing a local row cannot make it persistent program storage",
           "module binding/symbol-root place", [&](auto &plan, auto &) {
             auto &locals = plan.steps[firstIndex].locals;
             locals.erase(std::find(locals.begin(), locals.end(), first));
             return true;
           });
  rejected("persistent storage cannot be relabeled as an initializer local",
           "local overlaps persistent storage", [&](auto &plan, auto &) {
             auto &step = plan.steps[firstIndex];
             step.locals = {{.binding = step.binding,
                             .symbol = step.symbol,
                             .place = step.storagePlace}};
             return true;
           });
  rejected("a forged local symbol cannot replace its binding's exact identity",
           "local has invalid or overlapping identity",
           [&](auto &plan, auto &) {
             plan.steps[firstIndex].locals.front().symbol =
                 plan.steps[firstIndex].symbol;
             return true;
           });
  rejected("nested initializer expressions cannot disappear from their plan",
           "module full-expression inventory", [&](auto &plan, auto &) {
             plan.steps[firstIndex].nestedFullExpressions.clear();
             return true;
           });
  rejected("nested initializer markers cannot be assigned to another step",
           "nested initializer full expression", [&](auto &plan, auto &) {
             auto &nested = plan.steps[firstIndex].nestedFullExpressions;
             plan.steps[secondIndex].nestedFullExpressions.push_back(
                 nested.back());
             nested.pop_back();
             return true;
           });
  rejected("an exact-looking but unused local row is still orphan storage",
           "local has orphan storage", [&](auto &plan, auto &body) {
             auto extra = *body.findPlace(first.place);
             extra.id = body.places.size() + 1;
             for (const auto &place : body.places) {
               extra.binding = std::max(extra.binding, place.binding);
               extra.symbol = std::max(extra.symbol, place.symbol);
             }
             ++extra.binding;
             ++extra.symbol;
             extra.key = lang::PlaceKey{.domain = body.placeDomain,
                                        .root = extra.symbol};
             plan.steps[firstIndex].locals.push_back({.binding = extra.binding,
                                                      .symbol = extra.symbol,
                                                      .place = extra.id});
             body.places.push_back(std::move(extra));
             return true;
           });
  rejected("a later initializer cannot read an earlier macro-local place",
           "program-storage operand/receiver", [&](auto &, auto &body) {
             for (auto &block : body.blocks) {
               if (block.programInitializationStep !=
                   initialization.steps[secondIndex].id)
                 continue;
               for (auto &instruction : block.instructions) {
                 if (instruction.kind != lang::MirInstructionKind::Load ||
                     instruction.operands.size() != 1)
                   continue;
                 const auto *source =
                     body.findPlace(instruction.operands.front().place);
                 if (source &&
                     source->root == lang::MirPlaceRootKind::Binding &&
                     source->binding == second.binding &&
                     source->type == body.findPlace(first.place)->type) {
                   instruction.operands.front().place = first.place;
                   return true;
                 }
               }
             }
             return false;
           });
  rejected("a loan cannot hide an earlier initializer's macro-local source",
           "initializer loan accesses storage outside its owning step",
           [&](auto &, auto &body) {
             for (auto &loan : body.loans) {
               const auto *source = body.findPlace(loan.source);
               if (!source || source->root != lang::MirPlaceRootKind::Binding ||
                   source->binding != second.binding)
                 continue;
               const auto oldSource = loan.source;
               loan.source = first.place;
               for (auto &place : body.places)
                 if (place.root == lang::MirPlaceRootKind::Loan &&
                     place.loan == loan.id && place.key)
                   place.key->root = first.symbol;
               for (auto &block : body.blocks)
                 for (auto &instruction : block.instructions)
                   if (instruction.loan == loan.id)
                     for (auto &operand : instruction.operands)
                       if (operand.place == oldSource)
                         operand.place = first.place;
               return true;
             }
             return false;
           });
  rejected("a projection cannot hide an earlier initializer's local storage",
           "program-storage operand/receiver", [&](auto &, auto &body) {
             for (auto &place : body.places)
               if (place.root == lang::MirPlaceRootKind::Binding &&
                   place.binding == second.binding &&
                   !place.projections.empty()) {
                 place.binding = first.binding;
                 place.symbol = first.symbol;
                 if (place.key)
                   place.key->root = first.symbol;
                 return true;
               }
             return false;
           });
  rejected("a structured local cannot substitute a named storage owner",
           "local has invalid or overlapping identity",
           [&](auto &, auto &body) {
             body.places[view.place - 1].binding = first.binding;
             return true;
           });
  rejected(
      "a structured local cannot forge its semantic place key",
      "local has invalid or overlapping identity", [&](auto &, auto &body) {
        body.places[view.place - 1].key =
            lang::PlaceKey{.domain = body.placeDomain, .root = first.symbol};
        return true;
      });
  rejected("a structured local cannot become symbol-rooted program storage",
           "module binding/symbol-root place", [&](auto &, auto &body) {
             auto &place = body.places[view.place - 1];
             place.root = lang::MirPlaceRootKind::Symbol;
             place.binding = 0;
             return true;
           });
  rejected("an extra unused structured name cannot widen the decomposition",
           "local views are not an exact aggregate decomposition",
           [&](auto &plan, auto &body) {
             auto extra = *body.findPlace(view.place);
             auto local = view;
             extra.id = body.places.size() + 1;
             for (const auto &place : body.places)
               local.symbol = std::max(local.symbol, place.symbol);
             for (const auto &step : plan.steps)
               for (const auto &candidate : step.locals)
                 local.binding = std::max(local.binding, candidate.binding);
             ++local.binding;
             ++local.symbol;
             extra.symbol = local.symbol;
             extra.key.reset();
             local.place = extra.id;
             plan.steps[firstIndex].locals.push_back(local);
             body.places.push_back(std::move(extra));
             return true;
           });
  for (const bool eraseSymbol : {false, true})
    rejected(
        eraseSymbol
            ? "a symbol-erased projection cannot precede its hidden owner"
            : "a structured local cannot precede its hidden owner",
        "local view is accessed before its owner Initialize",
        [&](auto &plan, auto &body) {
          const auto &step = plan.steps[firstIndex];
          for (const auto &local : step.locals) {
            const auto *alias = body.findPlace(local.place);
            if (!alias || alias->binding == local.binding ||
                alias->projections.size() != 1 ||
                alias->projections.front().kind !=
                    lang::MirProjectionKind::Field)
              continue;
            const auto owner =
                std::find_if(step.locals.begin(), step.locals.end(),
                             [&](const auto &candidate) {
                               return candidate.binding == alias->binding;
                             });
            if (owner == step.locals.end())
              continue;
            for (auto &block : body.blocks) {
              if (block.programInitializationStep != step.id)
                continue;
              const auto initialize = std::find_if(
                  block.instructions.begin(), block.instructions.end(),
                  [&](const auto &instruction) {
                    return instruction.kind ==
                               lang::MirInstructionKind::Initialize &&
                           instruction.destination == owner->place;
                  });
              if (initialize == block.instructions.end())
                continue;
              const auto load = std::find_if(
                  initialize + 1, block.instructions.end(),
                  [&](const auto &instruction) {
                    if (instruction.kind != lang::MirInstructionKind::Load ||
                        instruction.operands.size() != 1)
                      return false;
                    const auto *source =
                        body.findPlace(instruction.operands.front().place);
                    return source && source->binding == alias->binding &&
                           source->symbol == local.symbol &&
                           source->projections == alias->projections;
                  });
              if (load == block.instructions.end())
                continue;
              const auto initializeId = initialize->id;
              auto moved = *load;
              if (eraseSymbol) {
                auto source = *body.findPlace(moved.operands.front().place);
                source.id = body.places.size() + 1;
                source.symbol = 0;
                source.key.reset();
                moved.operands.front().place = source.id;
                body.places.push_back(std::move(source));
              }
              block.instructions.erase(load);
              const auto before = std::find_if(
                  block.instructions.begin(), block.instructions.end(),
                  [&](const auto &instruction) {
                    return instruction.id == initializeId;
                  });
              block.instructions.insert(before, std::move(moved));
              return true;
            }
          }
          return false;
        });
}

void initializerResultRegions() {
  const auto result = analyze(R"gti(
mut int initializer_drops = 0;
class InitOwner {
public:
  InitOwner() {}
  InitOwner(InitOwner& other) = delete;
  InitOwner(InitOwner&& other) = default;
  ~InitOwner() { initializer_drops += 1; }
};
struct InitPair {
  int first;
  int second;
  InitPair(int first, int second) : first(first), second(second) {}
};
#macro int relay(int expression) { return expression; }
#macro T typed_relay<T>(T expression) { return expression; }
#macro T initial_value<T>(T expression) {
  InitOwner owner{};
  mut auto saved = expression;
  saved += T(1);
  auto& alias = saved;
  InitPair pair{2, 3};
  auto [left, right] = pair;
  return typed_relay(alias + T(left + right));
}
#macro int make_value(int expression) {
  int saved = expression;
  if (saved > 0) { return relay(saved); }
  return 0;
}
#macro InitOwner initial_owner() {
  InitOwner owner{};
  return InitOwner();
}
struct OwnedRecord { InitOwner field = initial_owner();  public: OwnedRecord() = default; };
struct Record {
  int field = make_value(7);
  static int shared = make_value(9);
  uint64_t wide = initial_value(uint64_t(5));
  static int generic_shared = initial_value(9);
public:
  Record() = default;
};
int global = make_value(3);
namespace setup { int namespaced = initial_value(4); }
int main() {
  Record record{};
  OwnedRecord owned{};
  return record.field + Record::shared + global - 19;
}
)gti");
  if (!result.canGenerateCode())
    for (const auto &value : result.diagnostics)
      std::cerr << value.code << ": " << value.message << '\n';
  check(
      result.canGenerateCode(),
      "namespace, field and static initializers share ordinary typed/inferred, "
      "mutable, reference, structured and owning macro-local rules");
  if (!result.canGenerateCode())
    return;
  std::vector<std::vector<lang::HirValueId>> initializerRoots;
  for (const auto &step : result.hir.programInitializationPlan().steps)
    initializerRoots.push_back(step.initializer
                                   ? std::vector{*step.initializer}
                                   : std::vector<lang::HirValueId>{});
  const auto batched = lang::hirInitializerReachabilityBatch(
      result.hir.module(), initializerRoots);
  check(batched.size() == initializerRoots.size(),
        "batched initializer queries preserve each separate root group");
  for (std::size_t index = 0; index < batched.size(); ++index) {
    const auto single = lang::hirInitializerReachability(
        result.hir.module(), initializerRoots[index]);
    check(single.complete == batched[index].complete &&
              single.values == batched[index].values &&
              single.statements == batched[index].statements &&
              single.bindings == batched[index].bindings,
          "shared initializer indexes preserve exact independent inventories");
  }
  check(lang::verifyHirProgramPlans(result.semantics, result.hir).valid() &&
            lang::verifyMirProgram(result.mir).valid(),
        "initializer result regions preserve independently verified HIR/MIR");
  const auto checkLocalDrops = [&](const lang::HirBody &hir,
                                   const lang::MirBody &mir) {
    std::size_t owners = 0;
    for (const auto &binding : hir.bindings) {
      if (!binding.variable || binding.variable->name().lexeme != "owner")
        continue;
      ++owners;
      const auto *hirDrop =
          binding.dropObligation
              ? hir.findDropObligation(*binding.dropObligation)
              : nullptr;
      check(hirDrop && hirDrop->kind == lang::HirDropObligationKind::Binding &&
                hirDrop->binding == binding.id && !hirDrop->initiallyActive &&
                hirDrop->dropType.type == binding.info.type,
            "initializer macro owners retain exact lexical HIR obligations");
      const auto drop = std::find_if(
          mir.dropObligations.begin(), mir.dropObligations.end(),
          [&](const auto &candidate) {
            return candidate.kind == lang::MirDropObligationKind::Binding &&
                   candidate.binding == binding.id;
          });
      check(drop != mir.dropObligations.end() &&
                drop->dropType.type == binding.info.type,
            "initializer macro owners retain exact lexical MIR obligations");
      for (const auto &candidate : mir.dropObligations) {
        if (candidate.kind != lang::MirDropObligationKind::ConstructionRollback)
          continue;
        const auto *place = mir.findPlace(candidate.place);
        check(!place || place->binding != binding.id,
              "initializer macro owners never become field rollback entries");
      }
    }
    return owners;
  };
  check(checkLocalDrops(result.hir.module(), result.mir.module()) == 2,
        "module initializers preserve both static and namespace macro owners");
  bool checkedOwnedField = false;
  for (const auto &instance : result.hir.classInstances()) {
    if (!instance.source || (instance.source->name().lexeme != "Record" &&
                             instance.source->name().lexeme != "OwnedRecord"))
      continue;
    const auto *mir = result.mir.findClassInstance(instance.id);
    check(mir != nullptr, "initializer fixture retains its MIR class catalog");
    if (!mir)
      continue;
    check(checkLocalDrops(instance.fieldInitializers, mir->fieldInitializers) ==
              1,
          "each field initializer retains its own macro-local owner");
    auto malformed = result.hir;
    const auto changedClass = std::find_if(
        malformed.classInstances().begin(), malformed.classInstances().end(),
        [&](const auto &candidate) { return candidate.id == instance.id; });
    auto &changed =
        const_cast<lang::HirClassInstance &>(*changedClass).fieldInitializers;
    const auto local = std::find_if(
        changed.bindings.begin(), changed.bindings.end(),
        [](const auto &binding) {
          return binding.variable && binding.variable->name().lexeme == "owner";
        });
    check(local != changed.bindings.end(),
          "field rollback fixture retains its lexical guard binding");
    if (local != changed.bindings.end()) {
      local->dropObligation.reset();
      check(
          !lang::MirLowerer().lower(malformed, result.failureMetadata).valid(),
          "missing local cleanup cannot be reinterpreted as field rollback");
    }
    if (instance.source->name().lexeme != "OwnedRecord") {
      check(std::none_of(
                mir->fieldInitializers.dropObligations.begin(),
                mir->fieldInitializers.dropObligations.end(),
                [](const auto &drop) {
                  return drop.kind ==
                         lang::MirDropObligationKind::ConstructionRollback;
                }),
            "scalar fields do not inherit their macro-local owner's rollback");
      continue;
    }
    checkedOwnedField = true;
    std::size_t rollbackCount = 0;
    for (const auto &drop : mir->fieldInitializers.dropObligations) {
      if (drop.kind != lang::MirDropObligationKind::ConstructionRollback)
        continue;
      ++rollbackCount;
      const auto *place = mir->fieldInitializers.findPlace(drop.place);
      check(instance.fields.size() == 1 && place &&
                place->root == lang::MirPlaceRootKind::Binding &&
                place->binding == instance.fields.front().binding &&
                place->symbol == instance.fields.front().info.symbol &&
                drop.dropType.type == instance.fields.front().info.type,
            "only the exact completed owning field arms rollback");
    }
    check(rollbackCount == 1,
          "owning field rollback remains distinct from its macro-local owner");
  }
  check(checkedOwnedField,
        "initializer fixture exercises actual owning field rollback");
  const auto mutateBody = [&](const lang::HirBody &body, const auto &selectBody,
                              std::string_view inventoryError) {
    const auto statement = std::find_if(
        body.statements.begin(), body.statements.end(),
        [](const auto &candidate) { return candidate.resultRegion != 0; });
    if (statement == body.statements.end())
      return false;
    auto malformed = result.hir;
    auto &changedBody = selectBody(malformed);
    const auto found = std::find_if(
        changedBody.statements.begin(), changedBody.statements.end(),
        [&](const auto &candidate) { return candidate.id == statement->id; });
    found->resultRegion = 0;
    check(!lang::verifyHirProgramPlans(result.semantics, malformed).valid(),
          "initializer verification must reject an erased local return target");
    found->resultRegion = statement->resultRegion + 100000;
    check(!lang::verifyHirProgramPlans(result.semantics, malformed).valid(),
          "initializer verification must reject a forged region return target");
    if (inventoryError.empty())
      return true;
    const auto rejectsInventory = [&](const auto &mutate) {
      auto extra = result.hir;
      mutate(selectBody(extra));
      const auto verification =
          lang::verifyHirProgramPlans(result.semantics, extra);
      check(std::any_of(verification.errors.begin(), verification.errors.end(),
                        [&](const auto &error) {
                          return error.find(inventoryError) !=
                                 std::string::npos;
                        }),
            "initializer inventories must reject unreachable extra nodes");
    };
    rejectsInventory([](auto &changed) {
      auto extra = changed.statements.front();
      extra.id += 100000;
      changed.statements.push_back(std::move(extra));
    });
    rejectsInventory([](auto &changed) {
      auto extra = changed.values.front();
      extra.id += 100000;
      changed.values.push_back(std::move(extra));
    });
    check(!body.bindings.empty(),
          "initializer fixture must retain a macro-local binding");
    if (!body.bindings.empty())
      rejectsInventory([](auto &changed) {
        auto extra = changed.bindings.back();
        extra.id += 100000;
        changed.bindings.push_back(std::move(extra));
      });
    return true;
  };
  bool fieldMutated = false;
  for (std::size_t index = 0; index < result.hir.classInstances().size();
       ++index)
    if (mutateBody(result.hir.classInstances()[index].fieldInitializers,
                   [index](auto &program) -> lang::HirBody & {
                     return const_cast<lang::HirClassInstance &>(
                                program.classInstances()[index])
                         .fieldInitializers;
                   },
                   {})) {
      fieldMutated = true;
      break;
    }
  check(fieldMutated, "fixture must retain a field-initializer macro return");
  bool constructorMutated = false;
  for (std::size_t index = 0; index < result.hir.constructorInstances().size();
       ++index) {
    const auto &constructor = result.hir.constructorInstances()[index];
    if (constructor.generatedDefault &&
        mutateBody(
            constructor.body,
            [index](auto &program) -> lang::HirBody & {
              return const_cast<lang::HirConstructorInstance &>(
                         program.constructorInstances()[index])
                  .body;
            },
            "generated constructor differs")) {
      constructorMutated = true;
      break;
    }
  }
  check(constructorMutated,
        "fixture must retain a generated-constructor macro return");
  // Static fields and namespace globals execute in the merged module body,
  // never in the intentionally empty class static initializer catalog.
  check(mutateBody(
            result.hir.module(),
            [](auto &program) -> lang::HirBody & {
              return const_cast<lang::HirBody &>(program.module());
            },
            "module "),
        "fixture must retain a static/global initializer macro return");
}

void passiveInitializerMaterializations() {
  const auto result = analyze(R"gti(
struct Leaf { int number = 7;  public: Leaf() = default; };
class Record {
  Leaf object = Leaf();
  Leaf elements[2] = {};
public:
  int read() { return this.object.number + this.elements[1].number; }
public:
  Record() = default;
};
int main() { Record record{}; return record.read() - 14; }
)gti");
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  check(result.canGenerateCode(), "passive field and array materializations "
                                  "retain exact lifetime transfers");
  check(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
        "trivial field cleanup does not create an unarmed subobject transfer");
}

void macroFreshArgumentBoundaries() {
  const std::string prefix = R"gti(
struct Value { int number; Value(int number) : number(number) {} };
#macro Value fresh(int number) { Value guard{99}; return Value(number); }
)gti";
  diagnostic(prefix + R"gti(
int change(mut Value& value) { value.number = 3; return value.number; }
int main() { return change(fresh(1)); }
)gti",
             "cannot bind a mutable reference to a temporary");
  diagnostic(prefix + R"gti(
Value& escape(Value& value) { return value; }
int main() { Value& value = escape(fresh(1)); return value.number; }
)gti",
             "may return a reference");
  diagnostic(prefix + R"gti(
int observe(Value& value);
int main() { return observe(fresh(1)); }
)gti",
             "declaration-only call target");
  diagnostic(R"gti(
#include <std/utility>
class Owner {
public:
  Owner() {}
  Owner(Owner& other) = delete;
  Owner(Owner&& other) = default;
  int read() { return 1; }
};
#macro Owner take(Owner value) { return value; }
int inspect(Owner& value) { return value.read(); }
int main() {
  Owner owner{};
  Owner& loan = owner;
  int result = inspect(take(std::move(owner)));
  return result + loan.read();
}
)gti",
             "overlapping borrow");
}

void callerConversionsAndPropagation() {
  const auto result = analyze(R"gti(
#include <std/source_location>
mut int cleaned = 0;
struct Guard { ~Guard() { cleaned += 1; }  public: Guard() = default; };
struct Box {
  int value;
  implicit Box(int value) : value(value) {}
};
struct Flag {
  operator bool() { return true; }
  implicit operator int() { return 19; }
public:
  Flag() = default;
};
#macro Flag flag() { return Flag(); }
#macro int scalar(int value) { return value; }
#macro int guarded(int expression) { Guard local{}; return expression; }
#macro std::source_location here(
    std::source_location location = std::source_location::current()) {
  return location;
}
expected<int, int> fallible(bool fail) {
  if (fail) { return unexpected(13); }
  return 31;
}
#macro expected<int, int> result(bool fail) { return fallible(fail); }
expected<int, int> forwarded(bool fail) {
  Guard caller{};
  return guarded(?result(fail));
}
expected<int, int> continued(bool fail) {
  Guard caller{};
  int result = guarded(?fallible(fail));
  return result + 1;
}
Box converted_return() { return scalar(17); }
int main() {
  Box initialized = scalar(11);
  Box returned = converted_return();
  mut int converted = flag();
  if (flag()) { converted += 1; }
  auto success = forwarded(false);
  auto failure = forwarded(true);
  auto continued_success = continued(false);
  auto continued_failure = continued(true);
  std::source_location location = here();
  return 0;
}
)gti");
  if (!result.canGenerateCode())
    for (const auto &value : result.diagnostics)
      std::cerr << value.code << ": " << value.message << '\n';
  check(result.canGenerateCode(),
        "value macros preserve caller-selected conversions, caller propagation "
        "and caller-location defaults");
  if (result.canGenerateCode()) {
    check(lang::verifyHirProgramPlans(result.semantics, result.hir).valid(),
          "converted and propagating result regions retain valid HIR plans");
    check(lang::verifyMirProgram(result.mir).valid(),
          "caller propagation through a local owning guard preserves verified "
          "cleanup");
  }
}
} // namespace
int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--test") {
    const std::string_view name = argv[2];
    if (name == "receiverResultLivenessMutations")
      receiverResultLivenessMutations();
    else if (name == "programInitializerLocalMetadataMutations")
      programInitializerLocalMetadataMutations();
    else if (name == "initializerResultRegions")
      initializerResultRegions();
    else if (name == "callerConversionsAndPropagation")
      callerConversionsAndPropagation();
    else if (name == "macroFreshArgumentBoundaries")
      macroFreshArgumentBoundaries();
    else if (name == "passiveInitializerMaterializations")
      passiveInitializerMaterializations();
    else
      return 2;
    return failures == 0 ? 0 : 1;
  }
  if (argc != 1)
    return 2;
  positiveAndMutation();
  owningResults();
  receiverResultLivenessMutations();
  programInitializerLocalMetadataMutations();
  initializerResultRegions();
  passiveInitializerMaterializations();
  macroFreshArgumentBoundaries();
  callerConversionsAndPropagation();
  diagnostic("#macro int bad(bool flag) { if (flag) { return 1; } }\n"
             "int main() { return bad(false); }",
             "every normally completing path");
  diagnostic("#macro int bad() { return; }\nint main() { return bad(); }",
             "requires a value");
  diagnostic("#macro int bad() { return true; }\nint main() { return bad(); }",
             "macro result");
  diagnostic("#macro stop() { return 1; }\nint main() { stop(); return 0; }",
             "void macro cannot return");
  diagnostic("#macro int bad(expected<int, int> value) { return ?value; }\n"
             "int main() { return 0; }",
             "Propagation");
  diagnostic("#macro int& bad() { int saved = 1; return saved; }\n"
             "int main() { int& value = bad(); return value; }",
             "cannot export a reference");
  diagnostic("#include <std/string>\n#macro std::string identity(std::string "
             "value) { return value; }\n"
             "int main() { std::string value{\"owned\"}; std::string moved = "
             "identity(value); return 0; }",
             "macro result");
  diagnostic(R"gti(
#include <std/string>
#macro std::string inner(std::string argument) { return argument; }
#macro std::string outer() { std::string saved{"owned"}; return inner(saved); }
int main() { std::string value = outer(); return 0; }
)gti",
             "macro result");
  diagnostic(R"gti(
class Pinned {
public:
  Pinned() {}
  Pinned(Pinned& other) = delete;
  Pinned(Pinned&& other) = delete;
};
#macro Pinned pinned() { return Pinned(); }
int main() { Pinned value = pinned(); return 0; }
)gti",
             "movable or copyable");
  return failures == 0 ? 0 : 1;
}
