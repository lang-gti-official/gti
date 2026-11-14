#include "../src/compiler/mir_carrier_flow.h"

#include <cstdlib>
#include <iostream>

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
  std::cout << "MIR carrier flow tests passed\n";
}
