#include "../src/compiler/mir_carrier_flow.h"
#include "../src/compiler/mir_loan_liveness.h"

#include <cstdlib>
#include <iostream>

namespace lang {
struct MirProgramTestAccess {
  static void functions(MirProgram &program,
                        std::vector<MirFunctionInstance> functions) {
    program.functions = std::move(functions);
  }
};
} // namespace lang

namespace {
using namespace lang;
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
MirOperand loan(MirLoanId id) {
  return {.kind = MirOperandKind::Loan, .loan = id};
}
MirOperand copy(MirPlaceId id) {
  return {.kind = MirOperandKind::Copy, .place = id};
}
MirInstruction store(MirInstructionId id, MirPlaceId destination,
                     MirOperand input) {
  return {.id = id,
          .kind = MirInstructionKind::Assign,
          .destination = destination,
          .operands = {input}};
}
MirInstruction read(MirInstructionId id, MirPlaceId place) {
  return {
      .id = id, .kind = MirInstructionKind::Load, .operands = {copy(place)}};
}
MirBody body(std::size_t blocks = 1) {
  MirBody result;
  result.entry = 1;
  for (std::size_t id = 1; id <= blocks; ++id)
    result.blocks.push_back(
        {.id = id, .terminator = {.kind = MirTerminatorKind::Return}});
  for (std::size_t id = 1; id <= 7; ++id)
    result.places.push_back(
        {.id = id, .root = MirPlaceRootKind::Binding, .binding = id});
  result.loans.push_back({.id = 1, .kind = MirLoanKind::Stored, .source = 6});
  result.loans.push_back({.id = 2, .kind = MirLoanKind::Stored, .source = 7});
  return result;
}
MirCarriedLoans set(std::initializer_list<MirLoanId> ids) {
  return std::vector<MirLoanId>(ids);
}
void replacementAndCopies() {
  auto mir = body();
  mir.blocks[0].instructions = {store(1, 1, loan(1)), store(2, 2, copy(1)),
                                store(3, 1, loan(2)), read(4, 1), read(5, 2)};
  const auto flow = deriveMirCarrierFlow(mir);
  expect(flow.complete, "direct replacements have complete flow");
  expect(flow.reads.at(4) == set({2}),
         "replacement releases the old dependency");
  expect(flow.reads.at(5) == set({1}),
         "a copy keeps the old dependency independently");
  auto incomplete = flow;
  incomplete.complete = false;
  const auto retained = conservativeMirRetentions(incomplete);
  expect(retained.at(1).carriers == std::vector<HirBindingId>({1, 2}) &&
             retained.at(2).carriers == std::vector<HirBindingId>({1}),
         "fallback liveness preserves every known storage carrier across "
         "replacements and copies");
  incomplete.storeTargets[3] = {{MirPlaceRootKind::Temporary, 8}};
  incomplete.storeInputsByTarget[3] = {
      {{MirPlaceRootKind::Temporary, 8}, set({2})}};
  expect(conservativeMirRetentions(incomplete).at(2).unmappedStorage,
         "retention without a source binding needs a conservative extent");
}
void branchesAndLoop() {
  auto mir = body(4);
  mir.blocks[0].instructions = {store(1, 1, loan(1))};
  mir.blocks[0].terminator = {
      .kind = MirTerminatorKind::Branch, .target = 2, .elseTarget = 3};
  mir.blocks[1].instructions = {store(2, 1, loan(2))};
  mir.blocks[1].terminator = {.kind = MirTerminatorKind::Goto, .target = 4};
  mir.blocks[2].terminator = {.kind = MirTerminatorKind::Goto, .target = 4};
  mir.blocks[3].instructions = {read(3, 1)};
  auto flow = deriveMirCarrierFlow(mir);
  expect(flow.complete && flow.reads.at(3) == set({1, 2}),
         "a join retains dependencies from either predecessor");
  // The first iteration sees owner 1, later iterations owner 2. The read at
  // the header must see both; replacement after it remains exact.
  mir = body(3);
  mir.blocks[0].instructions = {store(1, 1, loan(1))};
  mir.blocks[0].terminator = {.kind = MirTerminatorKind::Goto, .target = 2};
  mir.blocks[1].instructions = {read(2, 1), store(3, 1, loan(2)), read(4, 1)};
  mir.blocks[1].terminator = {
      .kind = MirTerminatorKind::Branch, .target = 2, .elseTarget = 3};
  flow = deriveMirCarrierFlow(mir);
  expect(flow.complete && flow.reads.at(2) == set({1, 2}),
         "loop backedges reach a finite fixed point");
  expect(flow.reads.at(4) == set({2}),
         "replacement remains precise within a loop");
}
void projectedAndAliasStores() {
  auto mir = body();
  mir.places[3] = {
      .id = 4,
      .root = MirPlaceRootKind::Binding,
      .binding = 1,
      .projections = {{.kind = MirProjectionKind::Field, .field = 7}}};
  mir.loans.push_back({.id = 3, .kind = MirLoanKind::Local, .source = 1});
  mir.places[4] = {.id = 5, .root = MirPlaceRootKind::Loan, .loan = 3};
  for (auto destination : {MirPlaceId{4}, MirPlaceId{5}}) {
    mir.blocks[0].instructions = {store(1, 1, loan(1)),
                                  store(2, destination, loan(2)), read(3, 1)};
    const auto flow = deriveMirCarrierFlow(mir);
    expect(flow.complete && flow.reads.at(3) == set({1, 2}),
           "projected and aliased stores widen their owning root");
  }
  mir.blocks[0].instructions = {
      store(1, 1, loan(1)),
      {.id = 2,
       .kind = MirInstructionKind::Load,
       .result = 1,
       .operands = {copy(5)}},
      store(3, 2, {.kind = MirOperandKind::Value, .value = 1}),
      read(4, 2)};
  const auto flow = deriveMirCarrierFlow(mir);
  expect(flow.reads.at(2) == set({1, 3}),
         "reading through an alias uses its physical loan");
  expect(flow.reads.at(4) == set({1}),
         "a copied value does not borrow the container storage");
  mir.loans[2].additionalDependencies.push_back({.source = 2});
  mir.blocks[0].instructions = {store(1, 1, loan(1)), store(2, 2, loan(1)),
                                store(3, 5, loan(2)), read(4, 1), read(5, 2)};
  const auto aliased = deriveMirCarrierFlow(mir);
  expect(aliased.complete && aliased.reads.at(4) == set({1, 2}) &&
             aliased.reads.at(5) == set({1, 2}),
         "an alias selecting either root widens every candidate root");
  mir.blocks[0].instructions = {store(1, 1, loan(1)), read(2, 5)};
  const auto incomplete = deriveMirCarrierFlow(mir);
  expect(!incomplete.complete && !incomplete.reads.at(2),
         "an unknown alias candidate cannot discard a known candidate's "
         "dependencies");
}
void referenceBindingSelectsCurrentStorage() {
  auto mir = body();
  mir.loans.push_back({.id = 3, .kind = MirLoanKind::Local, .source = 1});
  mir.places[3] = {.id = 4,
                   .root = MirPlaceRootKind::Binding,
                   .binding = 3,
                   .projections = {{.kind = MirProjectionKind::Dereference}},
                   .type = SemanticType::StringView};
  mir.blocks[0].instructions = {
      store(1, 1, loan(1)),
      store(2, 3, loan(3)),
      {.id = 3,
       .kind = MirInstructionKind::Load,
       .result = 1,
       .operands = {copy(4)},
       .info = {.type = SemanticType::StringView}},
      store(4, 2, {.kind = MirOperandKind::Value, .value = 1}),
      store(5, 4, loan(2)),
      read(6, 1),
      read(7, 2)};
  const auto flow = deriveMirCarrierFlow(mir);
  expect(flow.complete && flow.reads.at(3) == set({1, 3}),
         "a named reference uses both its physical loan and the referenced "
         "dependencies");
  expect(flow.reads.at(6) == set({1, 2}) && flow.reads.at(7) == set({1}),
         "a store through a named reference widens the actual source while a "
         "copy remains independent");
  expect(flow.reads.at(5) == set({1, 2, 3}),
         "a store through a named reference uses its physical access loan");
  mir.blocks[0].instructions = {read(1, 4)};
  const auto unknown = deriveMirCarrierFlow(mir);
  expect(!unknown.complete && !unknown.reads.at(1),
         "an unresolved named reference cannot become ownerless");
}

void loopArgumentsAreSimultaneous() {
  auto mir = body(3);
  mir.blocks[0].instructions = {
      {.id = 1, .kind = MirInstructionKind::Borrow, .result = 1, .loan = 1},
      {.id = 2, .kind = MirInstructionKind::Borrow, .result = 2, .loan = 2}};
  mir.blocks[0].terminator = {
      .kind = MirTerminatorKind::Goto,
      .target = 2,
      .targetArguments = {{.kind = MirOperandKind::Value, .value = 1},
                          {.kind = MirOperandKind::Value, .value = 2}}};
  mir.blocks[1].parameters = {{.value = 3}, {.value = 4}};
  mir.blocks[1].instructions = {
      {.id = 3,
       .kind = MirInstructionKind::CallInput,
       .operands = {{.kind = MirOperandKind::Value, .value = 3}}},
      {.id = 4,
       .kind = MirInstructionKind::CallInput,
       .operands = {{.kind = MirOperandKind::Value, .value = 4}}}};
  mir.blocks[1].terminator = {
      .kind = MirTerminatorKind::Branch,
      .target = 2,
      .elseTarget = 3,
      .targetArguments = {{.kind = MirOperandKind::Value, .value = 4},
                          {.kind = MirOperandKind::Value, .value = 3}}};
  const auto flow = deriveMirCarrierFlow(mir);
  expect(flow.complete && flow.reads.at(3) == set({1, 2}) &&
             flow.reads.at(4) == set({1, 2}),
         "loop-carried edge arguments bind simultaneously");
}

void failureRetainsBothVersions() {
  auto mir = body(3);
  mir.blocks[0].instructions = {store(1, 1, loan(1)), store(2, 1, loan(2))};
  mir.blocks[0].terminator = {.kind = MirTerminatorKind::Invoke,
                              .invokeInstruction = 2,
                              .target = 2,
                              .elseTarget = 3};
  mir.blocks[1].instructions = {read(3, 1)};
  mir.blocks[2].instructions = {read(4, 1)};
  const auto flow = deriveMirCarrierFlow(mir);
  expect(
      flow.complete && flow.reads.at(3) == set({2}) &&
          flow.reads.at(4) == set({1, 2}),
      "failure keeps both possible versions while success commits replacement");
  mir.blocks[0].instructions[1].result = 1;
  mir.blocks[2].instructions = {
      {.id = 4,
       .kind = MirInstructionKind::Load,
       .operands = {{.kind = MirOperandKind::Value, .value = 1}}}};
  const auto failedResult = deriveMirCarrierFlow(mir);
  expect(!failedResult.complete && !failedResult.reads.at(4),
         "a failed operation cannot publish a usable SSA result");
}

void scalarResultIsIndependent() {
  auto mir = body();
  mir.blocks[0].instructions = {
      store(1, 1, loan(1)),
      {.id = 2,
       .kind = MirInstructionKind::Load,
       .result = 1,
       .operands = {copy(1)},
       .info = {.type = SemanticType::Int32}},
      store(3, 2, {.kind = MirOperandKind::Value, .value = 1}),
      read(4, 2)};
  const auto flow = deriveMirCarrierFlow(mir);
  expect(flow.complete && flow.reads.at(2) == set({1}) &&
             flow.reads.at(4) == set({}),
         "an independent scalar result does not keep its input's loan alive");
}

void unknownAndFailure() {
  auto mir = body();
  mir.blocks[0].instructions = {
      store(1, 1, {.kind = MirOperandKind::Value, .value = 99}), read(2, 1)};
  auto flow = deriveMirCarrierFlow(mir);
  expect(!flow.complete && !flow.reads.at(2),
         "unknown provenance cannot become ownerless");
  mir = body(2);
  mir.blocks[0].terminator = {
      .kind = MirTerminatorKind::Invoke, .target = 2, .elseTarget = 2};
  expect(!deriveMirCarrierFlow(mir).complete,
         "unsupported failure transfers prohibit shortening");
  mir = body();
  mir.blocks[0].instructions = {
      {.id = 1, .kind = MirInstructionKind::Call, .operands = {loan(1)}}};
  expect(!deriveMirCarrierFlow(mir).complete,
         "a call without checked retaining effects prohibits shortening");
  mir = body();
  mir.blocks[0].terminator = {.kind = MirTerminatorKind::Return,
                              .returnLoan = 1};
  expect(deriveMirCarrierFlow(mir).exits.at(1) == set({1}),
         "a returned loan is a use even without a value operand");
}
void missingPredecessorProvenance() {
  for (bool reverse : {false, true}) {
    auto mir = body(4);
    mir.blocks[0].terminator = {.kind = MirTerminatorKind::Branch,
                                .target = reverse ? 3u : 2u,
                                .elseTarget = reverse ? 2u : 3u};
    mir.blocks[1].instructions = {store(1, 1, loan(1))};
    mir.blocks[1].terminator = {.kind = MirTerminatorKind::Goto, .target = 4};
    mir.blocks[2].terminator = {.kind = MirTerminatorKind::Goto, .target = 4};
    mir.blocks[3].instructions = {read(2, 1)};
    const auto flow = deriveMirCarrierFlow(mir);
    expect(!flow.complete && !flow.reads.at(2),
           "a missing predecessor fact stays unknown regardless of edge order");
  }
}
void physicalReceiverAndReturnReads() {
  auto mir = body();
  mir.places[3] = {.id = 4,
                   .root = MirPlaceRootKind::Loan,
                   .loan = 1,
                   .type = SemanticType::Int32};
  mir.blocks[0].instructions = {
      store(1, 6, {.kind = MirOperandKind::Constant}),
      {.id = 2, .kind = MirInstructionKind::Call, .receiver = copy(4)}};
  auto flow = deriveMirCarrierFlow(mir);
  expect(!flow.complete && flow.reads.at(2) == set({1}),
         "a scalar referent receiver still uses its physical loan");
  mir.blocks[0].instructions.pop_back();
  mir.blocks[0].terminator = {.kind = MirTerminatorKind::Return,
                              .value = copy(4)};
  flow = deriveMirCarrierFlow(mir);
  expect(flow.complete && flow.exits.at(1) == set({1}),
         "returning a scalar referent reads its physical loan");
}
void edgeAndPermissionUses() {
  auto mir = body(2);
  mir.blocks[0].instructions = {
      {.id = 1, .kind = MirInstructionKind::ActivateBorrow, .loan = 1}};
  mir.blocks[0].terminator = {.kind = MirTerminatorKind::Goto,
                              .target = 2,
                              .targetArguments = {loan(2)}};
  mir.blocks[1].parameters = {{.value = 1}};
  const auto flow = deriveMirCarrierFlow(mir);
  expect(flow.complete && flow.reads.at(1) == set({1}) &&
             flow.exits.at(1) == set({2}),
         "permission transitions and unused edge arguments are loan uses");
}
void directRetainingEffects() {
  MirFunctionInstance function;
  function.definitionKind = MirDefinitionKind::Source;
  function.owner = 1;
  function.parameterBindings = {2, 3};
  function.parameterTypes = {SemanticType::StringView,
                             SemanticType::StringView};
  function.body = body(4);
  auto &mir = function.body;
  mir.kind = MirBodyKind::Function;
  mir.loans[0].entry = true;
  mir.loans[0].source = 2;
  mir.loans[0].carriers = {2};
  mir.loans[1].entry = true;
  mir.loans[1].source = 3;
  mir.loans[1].carriers = {3};
  mir.places[3] = {
      .id = 4,
      .root = MirPlaceRootKind::This,
      .projections = {{.kind = MirProjectionKind::Field, .field = 9}}};
  mir.blocks[0].terminator = {
      .kind = MirTerminatorKind::Branch, .target = 2, .elseTarget = 3};
  mir.blocks[1].instructions = {store(1, 4, copy(2))};
  mir.blocks[2].instructions = {store(2, 4, copy(3))};
  mir.blocks[1].terminator = {.kind = MirTerminatorKind::Goto, .target = 4};
  mir.blocks[2].terminator = {.kind = MirTerminatorKind::Goto, .target = 4};
  auto effects = deriveMirRetainingStoreEffects(function);
  expect(effects.complete && effects.effects.size() == 2,
         "a receiver store retains either input across a branch");
  expect(effects.effects[0].destination ==
                 MirCarrierRoot{MirPlaceRootKind::This, 0} &&
             effects.effects[0].source ==
                 MirCarrierRoot{MirPlaceRootKind::Binding, 2} &&
             effects.effects[1].source ==
                 MirCarrierRoot{MirPlaceRootKind::Binding, 3},
         "effects map actual input bindings to the receiver without nominal "
         "names");
  // One represented group can retain more than one boundary origin. The
  // summary must export every dependency, including conditional alternatives.
  mir.blocks[2].instructions.clear();
  mir.loans[0].additionalDependencies = {{.source = 3}};
  effects = deriveMirRetainingStoreEffects(function);
  expect(effects.complete && effects.effects.size() == 2 &&
             effects.effects[1].source ==
                 MirCarrierRoot{MirPlaceRootKind::Binding, 3},
         "a retained group exports all of its boundary owners");
  mir.loans[0].additionalDependencies[0].source = 1;
  expect(!deriveMirRetainingStoreEffects(function).complete,
         "a local additional owner cannot disappear from a retaining summary");
  mir.loans[0].additionalDependencies[0].source = 99;
  expect(!deriveMirRetainingStoreEffects(function).complete,
         "an unknown additional owner cannot disappear from a retaining summary");
  mir.loans[0].additionalDependencies.clear();
  mir.places[4] = {.id = 5, .root = MirPlaceRootKind::Loan, .loan = 1};
  mir.places[5] = {.id = 6, .root = MirPlaceRootKind::Loan, .loan = 2};
  mir.loans.push_back({.id = 3, .parent = 1,
                       .kind = MirLoanKind::Local, .source = 5});
  mir.blocks[1].instructions = {store(1, 4, loan(3))};
  effects = deriveMirRetainingStoreEffects(function);
  expect(effects.complete && effects.effects.size() == 1 &&
             effects.effects[0].source ==
                 MirCarrierRoot{MirPlaceRootKind::Binding, 2},
         "an explicit input reborrow reconstructs its boundary origin");
  mir.loans[2].additionalDependencies = {{.source = 6, .parent = 2}};
  effects = deriveMirRetainingStoreEffects(function);
  expect(effects.complete && effects.effects.size() == 2,
         "all alternative reborrow parents contribute retaining obligations");
  mir.places[4].projections = {
      {.kind = MirProjectionKind::RawDereference}};
  expect(!deriveMirRetainingStoreEffects(function).complete,
         "raw reborrow projections need a separate storage proof");
  mir.places[4].projections.clear();
  mir.loans[2].storedDependency = 2;
  expect(!deriveMirRetainingStoreEffects(function).complete,
         "a physical shell cannot silently discard its stored dependency");
  mir.loans[2].storedDependency = 0;
  mir.loans[2].source = 1;
  expect(!deriveMirRetainingStoreEffects(function).complete,
         "a parent link cannot make local physical storage exportable");
  mir.loans[2].source = 5;
  mir.loans[2].parent = 3;
  mir.places[4].loan = 3;
  expect(!deriveMirRetainingStoreEffects(function).complete,
         "cyclic reborrow provenance cannot certify itself");
  mir.loans.pop_back();
  mir.blocks[1].instructions = {store(1, 4, copy(2))};
  // A mutation of a local by-value formal is not a caller-visible store.
  mir.blocks[1].instructions[0].destination = 3;
  mir.blocks[2].instructions.clear();
  effects = deriveMirRetainingStoreEffects(function);
  expect(effects.complete && effects.effects.empty(),
         "by-value formal replacement is local");
  // Unknown provenance must not turn into a non-retaining contract.
  mir.blocks[1].instructions = {
      store(1, 4, {.kind = MirOperandKind::Value, .value = 99})};
  expect(!deriveMirRetainingStoreEffects(function).complete,
         "unknown retained input cannot be certified");
  mir.blocks[1].instructions = {store(1, 4, loan(1))};
  mir.loans[0].entry = false;
  expect(!deriveMirRetainingStoreEffects(function).complete,
         "a local owner cannot be exported as a formal origin");
  mir.loans[0].entry = true;
  mir.blocks[2].instructions = {{.id = 2, .kind = MirInstructionKind::Call}};
  expect(!deriveMirRetainingStoreEffects(function).complete,
         "unmodeled calls are not certified as non-retaining");
  mir.blocks[2].instructions[0].kind = MirInstructionKind::Drop;
  expect(!deriveMirRetainingStoreEffects(function).complete,
         "cleanup effects require their own summary");
  mir.blocks[2].instructions.clear();
  mir.places[4] = {.id = 5,
                   .root = MirPlaceRootKind::Loan,
                   .loan = 1,
                   .type = SemanticType::Int32};
  mir.blocks[1].instructions = {store(
      1, 4,
      {.kind = MirOperandKind::Copy, .place = 5, .type = SemanticType::Int32})};
  effects = deriveMirRetainingStoreEffects(function);
  expect(effects.complete && effects.effects.empty(),
         "storing an independent scalar through a borrow retains no owner");
  function.parameterTypes[0] =
      SemanticType::referenceTo(SemanticType::StringView, AccessMode::Mutable);
  mir.loans[0].kind = MirLoanKind::Parameter;
  mir.places[3] = {.id = 4,
                   .root = MirPlaceRootKind::Binding,
                   .binding = 2,
                   .projections = {{.kind = MirProjectionKind::Dereference}}};
  mir.blocks[1].instructions = {store(1, 4, copy(3))};
  effects = deriveMirRetainingStoreEffects(function);
  expect(effects.complete && effects.effects.size() == 1 &&
             effects.effects[0].destination ==
                 MirCarrierRoot{MirPlaceRootKind::Binding, 2} &&
             effects.effects[0].source ==
                 MirCarrierRoot{MirPlaceRootKind::Binding, 3},
         "writes through a reference formal retain the other input in caller "
         "storage");
}
void composedRetainingEffects() {
  MirFunctionInstance leaf;
  leaf.id = 1;
  leaf.definitionKind = MirDefinitionKind::Source;
  leaf.owner = 1;
  leaf.parameterBindings = {2, 3};
  leaf.parameterTypes = {SemanticType::StringView, SemanticType::StringView};
  leaf.body = body();
  leaf.body.kind = MirBodyKind::Function;
  leaf.body.loans[0].entry = true;
  leaf.body.loans[0].source = 2;
  leaf.body.loans[0].carriers = {2};
  leaf.body.loans[1].entry = true;
  leaf.body.loans[1].source = 3;
  leaf.body.loans[1].carriers = {3};
  leaf.body.places[3] = {.id = 4, .root = MirPlaceRootKind::This};
  leaf.body.blocks[0].instructions = {store(1, 4, copy(2))};
  auto wrapper = leaf;
  wrapper.id = 2;
  wrapper.body.blocks[0].instructions = {
      {.id = 1, .kind = MirInstructionKind::Call,
       .receiver = copy(4), .operands = {copy(3), copy(2)},
       .functionTarget = leaf.id}};
  auto outer = wrapper;
  outer.id = 3;
  outer.body.blocks[0].instructions[0].functionTarget = wrapper.id;
  MirProgram program;
  MirProgramTestAccess::functions(program, {leaf, wrapper, outer});
  expect(!deriveMirRetainingStoreEffects(wrapper).complete,
         "a helper call needs its checked program body");
  const auto first = deriveMirRetainingStoreEffects(wrapper, &program);
  expect(first.complete && first.effects.size() == 1 &&
             first.effects[0].source ==
                 MirCarrierRoot{MirPlaceRootKind::Binding, 3},
         "a helper summary substitutes the caller's permuted input");
  const auto second = deriveMirRetainingStoreEffects(outer, &program);
  expect(second.complete && second.effects.size() == 1 &&
             second.effects[0].source ==
                 MirCarrierRoot{MirPlaceRootKind::Binding, 2},
         "nested helpers compose input substitutions at every call");
  leaf.body.blocks[0].instructions = wrapper.body.blocks[0].instructions;
  leaf.body.blocks[0].instructions[0].functionTarget = wrapper.id;
  MirProgramTestAccess::functions(program, {leaf, wrapper, outer});
  expect(!deriveMirRetainingStoreEffects(outer, &program).complete,
         "recursive helpers cannot assume an empty effect to certify themselves");
  leaf.body.blocks[0].instructions[0].functionTarget = 99;
  MirProgramTestAccess::functions(program, {leaf, wrapper, outer});
  expect(!deriveMirRetainingStoreEffects(outer, &program).complete,
         "an unavailable nested target leaves the outer summary incomplete");
}
void independentRetainingDestinations() {
  MirFunctionInstance leaf;
  leaf.id = 1;
  leaf.definitionKind = MirDefinitionKind::Source;
  leaf.parameterBindings = {1, 2, 3, 4};
  const auto reference = SemanticType::referenceTo(
      SemanticType::StringView, AccessMode::Mutable);
  leaf.parameterTypes = {reference, reference, SemanticType::StringView,
                         SemanticType::StringView};
  leaf.body = body();
  auto &mir = leaf.body;
  mir.kind = MirBodyKind::Function;
  mir.loans.clear();
  for (std::size_t id = 1; id <= 4; ++id)
    mir.loans.push_back({.id = id, .kind = MirLoanKind::Stored,
                         .source = id, .carriers = {id}, .entry = true});
  mir.places[4] = {.id = 5, .root = MirPlaceRootKind::Loan, .loan = 1};
  mir.places[5] = {.id = 6, .root = MirPlaceRootKind::Loan, .loan = 2};
  mir.blocks[0].instructions = {store(1, 5, copy(3)), store(2, 6, copy(4))};
  auto wrapper = leaf;
  wrapper.id = 2;
  wrapper.body.blocks[0].instructions = {
      {.id = 1, .kind = MirInstructionKind::Call,
       .operands = {loan(1), loan(2), copy(3), copy(4)},
       .functionTarget = leaf.id}};
  MirProgram program;
  MirProgramTestAccess::functions(program, {leaf, wrapper});
  const auto flow = deriveMirCarrierFlow(wrapper.body, &program);
  const MirCarrierRoot first{MirPlaceRootKind::Binding, 1};
  const MirCarrierRoot second{MirPlaceRootKind::Binding, 2};
  expect(flow.storeInputsByTarget.at(1).at(first) == set({3}) &&
             flow.storeInputsByTarget.at(1).at(second) == set({4}),
         "a helper's independent destinations retain their own inputs");
  const auto effects = deriveMirRetainingStoreEffects(wrapper, &program);
  expect(effects.complete && effects.effects.size() == 2 &&
             effects.effects[0] == MirRetainingStoreEffect{
                 first, {MirPlaceRootKind::Binding, 3}} &&
             effects.effects[1] == MirRetainingStoreEffect{
                 second, {MirPlaceRootKind::Binding, 4}},
         "nested summaries do not cross-product independent stores");
  const auto conservative = conservativeMirRetentions(flow);
  expect(conservative.at(3).carriers == std::vector<HirBindingId>{1} &&
             conservative.at(4).carriers == std::vector<HirBindingId>{2},
         "fallback liveness preserves the same destination/input relation");
  wrapper.body.blocks[0].instructions[0].operands[1] = loan(1);
  const auto aliased = deriveMirCarrierFlow(wrapper.body, &program);
  expect(aliased.storeInputsByTarget.at(1).at(first) == set({3, 4}) &&
             aliased.storeInputsByTarget.at(1).size() == 1,
         "aliased actual destinations correctly union both inputs");
}
void scalarProjectionStillReadsCarrier() {
  auto mir = body();
  mir.places[2] = {
      .id = 3,
      .root = MirPlaceRootKind::Binding,
      .binding = 1,
      .projections = {{.kind = MirProjectionKind::Field, .field = 9}},
      .type = SemanticType::Int32};
  mir.blocks[0].instructions = {
      store(1, 1, loan(1)),
      store(2, 2, {.kind = MirOperandKind::Copy,
                    .place = 3,
                    .type = SemanticType::Int32}),
      read(3, 2)};
  const auto flow = deriveMirCarrierFlow(mir);
  expect(flow.complete && flow.reads.at(2) == set({1}) &&
             flow.stores.at(2) == set({}) && flow.reads.at(3) == set({}),
         "a scalar projection uses its carrier without retaining it in the result");
}
void partialFactsExtendLiveness() {
  HirBody hir;
  hir.bindings = {{.id = 1, .info = {.symbol = 10}}};
  hir.values = {{.id = 1, .kind = HirValueKind::Variable, .symbol = 10}};
  hir.statements = {{.id = 1, .kind = HirStatementKind::Variable, .binding = 1},
                    {.id = 2, .kind = HirStatementKind::Expression, .value = 1},
                    {.id = 3, .kind = HirStatementKind::Expression},
                    {.id = 4, .kind = HirStatementKind::Empty}};
  hir.roots = {1, 2, 3, 4};
  LoanFacts loan{.id = 1, .carriers = {1}};
  const auto retained = [](HirBindingId) -> std::size_t { return 0; };
  auto ends = deriveLoanEnds(hir, {loan}, retained);
  expect(ends.after.contains(2),
         "ordinary reader liveness ends after its last read");
  loan.additionalUseStatements = {3};
  ends = deriveLoanEnds(hir, {loan}, retained);
  expect(ends.after.contains(3) && !ends.after.contains(2),
         "a known MIR retaining use extends conservative liveness");
  loan.additionalUseStatements = {1};
  ends = deriveLoanEnds(hir, {loan}, retained);
  expect(ends.after.contains(2),
         "partial MIR facts cannot erase later ordinary reads");
  loan.scopeBound = true;
  ends = deriveLoanEnds(hir, {loan}, retained);
  expect(ends.after.empty(),
         "unmapped retained storage forbids an early loan end");
}
} // namespace
int main() {
  replacementAndCopies();
  branchesAndLoop();
  projectedAndAliasStores();
  referenceBindingSelectsCurrentStorage();
  loopArgumentsAreSimultaneous();
  failureRetainsBothVersions();
  scalarResultIsIndependent();
  unknownAndFailure();
  missingPredecessorProvenance();
  physicalReceiverAndReturnReads();
  edgeAndPermissionUses();
  directRetainingEffects();
  composedRetainingEffects();
  independentRetainingDestinations();
  scalarProjectionStillReadsCarrier();
  partialFactsExtendLiveness();
  std::cout << "MIR carrier flow tests passed\n";
}
