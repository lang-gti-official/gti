#include "../src/compiler/mir_carrier_flow.h"
#include "../src/compiler/mir_loan_liveness.h"
#include "../src/compiler/mir_verification_queries.h"
#include "gti/mir_effects.h"

#include <cstdlib>
#include <iostream>

namespace lang {
struct MirProgramTestAccess {
  static void classes(MirProgram &program,
                      std::vector<MirClassInstance> classes) {
    program.classes = std::move(classes);
  }
  static void functions(MirProgram &program,
                        std::vector<MirFunctionInstance> functions) {
    program.functions = std::move(functions);
  }
};
} // namespace lang

namespace carrier_flow_tests {
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
  result.loans.push_back(
      {.id = 1, .kind = MirLoanKind::Stored, .source = 6, .entry = true});
  result.loans.push_back(
      {.id = 2, .kind = MirLoanKind::Stored, .source = 7, .entry = true});
  return result;
}
MirCarriedLoans set(std::initializer_list<MirLoanId> ids) {
  return std::vector<MirLoanId>(ids);
}
void intrinsicRetentionEffects() {
  auto mir = body();
  mir.blocks[0].instructions = {{.id = 1,
                                 .kind = MirInstructionKind::Call,
                                 .result = 1,
                                 .operands = {loan(1)},
                                 .intrinsic = IntrinsicKind::StringViewSize,
                                 .info = {.type = SemanticType::UInt64}}};
  const auto known = deriveMirCarrierFlow(mir);
  expect(known.complete && known.effectsComplete &&
             known.reads.at(1) == set({1}),
         "a no-write intrinsic observes its input without an unknown retaining "
         "effect");
  mir.blocks[0].instructions[0].intrinsic =
      IntrinsicKind::FunctionPointerInvoke;
  const auto invocation = deriveMirCarrierFlow(mir);
  expect(invocation.effectsComplete && invocation.reads.at(1) == set({1}),
         "a checked invocation-pointer signature preserves its "
         "no-store-retention contract");
  mir.blocks[0].instructions[0].intrinsic = IntrinsicKind::None;
  const auto unresolved = deriveMirCarrierFlow(mir);
  expect(!unresolved.complete && !unresolved.effectsComplete,
         "an unresolved call cannot inherit an intrinsic's no-retention proof");
  mir.blocks[0].instructions[0].intrinsic = IntrinsicKind::StringViewSize;
  mir.blocks[0].instructions[0].rawMemoryAccess = true;
  expect(
      !deriveMirCarrierFlow(mir).effectsComplete,
      "raw writes override a normally non-retaining intrinsic classification");
}

void constantStorageRetentionEffects() {
  const auto rawVoid = SemanticType::rawPointerTo(SemanticType::Void);
  for (const auto kind :
       {IntrinsicKind::ConstantAllocate, IntrinsicKind::ConstantRelease}) {
    auto mir = body();
    MirInstruction call;
    call.id = 1;
    call.kind = MirInstructionKind::Call;
    call.intrinsic = kind;
    call.info.type =
        kind == IntrinsicKind::ConstantAllocate ? rawVoid : SemanticType::Void;
    call.parameterTypes = kind == IntrinsicKind::ConstantAllocate
                              ? std::vector<SemanticType>{SemanticType::UInt64,
                                                          SemanticType::UInt64}
                              : std::vector<SemanticType>{rawVoid};
    for (const auto &type : call.parameterTypes)
      call.operands.push_back({.kind = MirOperandKind::Constant, .type = type});
    mir.blocks[0].instructions = {call};
    expect(deriveMirCarrierFlow(mir).effectsComplete,
           "constant allocation and release do not retain checked inputs");
    expect(
        kind == IntrinsicKind::ConstantAllocate
            ? effects(call).allocates
            : effects(call).writesUnknownMemory,
        "retention precision preserves allocation and release memory effects");
    mir.blocks[0].instructions[0].operands[0].type = SemanticType::referenceTo(
        SemanticType::StringView, AccessMode::ReadOnly);
    expect(!deriveMirCarrierFlow(mir).effectsComplete,
           "a checked reference cannot masquerade as a byte provider input");
    mir.blocks[0].instructions[0] = call;
    mir.blocks[0].instructions[0].intrinsic = IntrinsicKind::None;
    expect(
        !deriveMirCarrierFlow(mir).effectsComplete,
        "ordinary allocation-shaped calls need an independent retention proof");
  }
}

void unsafeMarkedCheckedCopyEffects() {
  auto mir = body();
  SemanticType record;
  record.kind = SemanticType::Class;
  record.classId = 1;
  mir.places[0].type = record;
  mir.loans[0].carriers = {1};
  auto load = read(1, 1);
  load.info.type = record;
  load.unsafeOperation = UnsafeOperationKind::ObjectLifetime;
  mir.blocks[0].instructions = {load};
  const auto copied = deriveMirCarrierFlow(mir);
  expect(copied.effectsComplete && copied.reads.at(1) == set({1}),
         "a checked aggregate copy preserves owner reads without repeating an "
         "unsafe call");
  mir.blocks[0].instructions[0].rawMemoryAccess = true;
  expect(!deriveMirCarrierFlow(mir).effectsComplete,
         "a raw load cannot inherit the checked-copy retention proof");
  mir.blocks[0].instructions[0] = load;
  mir.places[0].foreignStorage = true;
  expect(!deriveMirCarrierFlow(mir).effectsComplete,
         "foreign storage does not receive checked-copy retention precision");
  mir.places[0].foreignStorage = false;
  mir.places[0].projections = {{.kind = MirProjectionKind::RawDereference}};
  expect(!deriveMirCarrierFlow(mir).effectsComplete,
         "raw dereference projections do not receive checked-copy retention "
         "precision");
}

void unsafeReferenceStagingEffects() {
  auto mir = body();
  MirInstruction input;
  input.id = 1;
  input.kind = MirInstructionKind::CallInput;
  input.result = 1;
  input.callInputKind = HirCallInputKind::ReadBorrow;
  input.unsafeOperation = UnsafeOperationKind::ObjectLifetime;
  input.operands = {loan(1)};
  mir.blocks[0].instructions = {input};
  const auto staged = deriveMirCarrierFlow(mir);
  expect(staged.effectsComplete && staged.reads.at(1) == set({1}),
         "shared reference staging preserves reads without inventing a "
         "retaining write");
  for (const auto kind :
       {MirInstructionKind::Borrow, MirInstructionKind::EndBorrow}) {
    auto marker = input;
    marker.kind = kind;
    marker.result.reset();
    marker.loan = 1;
    mir.blocks[0].instructions = {marker};
    expect(deriveMirCarrierFlow(mir).effectsComplete,
           "borrow bookkeeping does not repeat its producer's unsafe call");
    mir.blocks[0].instructions[0].rawMemoryAccess = true;
    expect(!deriveMirCarrierFlow(mir).effectsComplete,
           "borrow bookkeeping cannot hide raw memory effects");
  }
  mir.blocks[0].instructions = {input};
  mir.blocks[0].instructions[0].rawMemoryAccess = true;
  expect(!deriveMirCarrierFlow(mir).effectsComplete,
         "reference staging cannot hide raw memory effects");
  mir.blocks[0].instructions[0] = input;
  mir.blocks[0].instructions[0].callInputKind = HirCallInputKind::CopyValue;
  expect(
      !deriveMirCarrierFlow(mir).effectsComplete,
      "value construction cannot use the shared reference staging exemption");
  mir.blocks[0].instructions[0] = input;
  mir.blocks[0].instructions[0].operands = {
      {.kind = MirOperandKind::Move, .place = 1}};
  expect(!deriveMirCarrierFlow(mir).effectsComplete,
         "a staging operand with a write cannot use the no-write proof");
}

void directReceiverReadIsolation() {
  auto mir = body();
  mir.blocks[0].instructions = {
      {.id = 1, .kind = MirInstructionKind::EndBorrow, .loan = 1},
      {.id = 2,
       .kind = MirInstructionKind::Call,
       .receiver = loan(1),
       .operands = {loan(2)}}};
  const auto flow = deriveMirCarrierFlow(mir);
  expect(flow.receiverReads.at(2) == set({1}) &&
             flow.reads.at(2) == set({1, 2}),
         "direct receiver facts exclude unrelated call argument loans");
  expect(flow.unavailableReceiverReads.at(2) == set({1}),
         "direct receiver facts preserve expired physical access");
}

void expandedPackRetentionEffects() {
  MirFunctionInstance function;
  function.id = 1;
  function.definitionKind = MirDefinitionKind::Source;
  function.parameterBindings = {6};
  function.parameterTypes = {SemanticType::StringView,
                             SemanticType::StringView};
  function.body = body();
  function.body.kind = MirBodyKind::Function;
  auto effects = deriveMirRetainingStoreEffects(function);
  expect(effects.complete && effects.effects.empty(),
         "a store-free expanded pack needs no per-element retention mapping");
  MirProgram program;
  MirProgramTestAccess::functions(program, {function});
  auto caller = body();
  caller.blocks[0].instructions = {{.id = 1,
                                    .kind = MirInstructionKind::Call,
                                    .operands = {loan(1), loan(2)},
                                    .functionTarget = 1}};
  expect(deriveMirCarrierFlow(caller, &program).effectsComplete,
         "a caller uses expanded arity for a proven non-retaining pack helper");
  function.body.blocks[0].instructions = {store(1, 1, loan(1))};
  expect(!deriveMirRetainingStoreEffects(function).complete,
         "a store through an unmapped pack cannot acquire an empty retention "
         "proof");
  function.body.blocks[0].instructions = {
      {.id = 1, .kind = MirInstructionKind::Call}};
  expect(!deriveMirRetainingStoreEffects(function).complete,
         "an opaque operation keeps a store-free pack summary unknown");
}

void retainingPackParameterIdentity() {
  MirBody formalBody;
  SemanticType pack(SemanticType::TypePack);
  pack.concretePack = true;
  pack.arguments = {SemanticType::StringView, SemanticType::UInt64};
  formalBody.places = {{.id = 1,
                        .root = MirPlaceRootKind::Binding,
                        .binding = 8,
                        .type = pack,
                        .initiallyAvailable = true}};
  const std::vector<SemanticType> types = {
      SemanticType::Int32, SemanticType::StringView, SemanticType::UInt64};
  const std::vector<HirBindingId> bindings = {7, 8};
  const auto resolve = [&](MirCarrierRoot root) {
    return mirRetainingParameterIndex(formalBody, types, bindings, root);
  };
  expect(resolve({MirPlaceRootKind::Binding, 7}) == 0 &&
             resolve({MirPlaceRootKind::Binding, 8, 0}) == 1 &&
             resolve({MirPlaceRootKind::Binding, 8, 1}) == 2,
         "pack elements map to distinct expanded caller arguments");
  expect(!resolve({MirPlaceRootKind::Binding, 8}) &&
             !resolve({MirPlaceRootKind::Binding, 7, 0}) &&
             !resolve({MirPlaceRootKind::Binding, 8, 2}),
         "whole packs, projected ordinary formals and invalid lanes cannot "
         "alias a formal");
  formalBody.places[0].type.arguments[1] = SemanticType::StringView;
  expect(!resolve({MirPlaceRootKind::Binding, 8, 0}),
         "a mismatched expanded signature invalidates the entire mapping");
  formalBody.places[0].type = pack;
  formalBody.places[0].type.concretePack = false;
  expect(!resolve({MirPlaceRootKind::Binding, 8, 0}),
         "an unresolved pack cannot certify an expanded argument");
}

void retainedPackElementsStayIndependent() {
  MirFunctionInstance function;
  function.id = 1;
  function.definitionKind = MirDefinitionKind::Source;
  const SemanticType view(SemanticType::StringView);
  const auto owner = SemanticType::referenceTo(view, AccessMode::Mutable);
  SemanticType pack(SemanticType::TypePack);
  pack.concretePack = true;
  pack.arguments = {view, view};
  function.parameterTypes = {owner, view, view};
  function.parameterBindings = {1, 2};
  auto &mir = function.body;
  mir.kind = MirBodyKind::Function;
  mir.entry = 1;
  mir.places = {{.id = 1,
                 .root = MirPlaceRootKind::Binding,
                 .binding = 1,
                 .type = owner,
                 .initiallyAvailable = true},
                {.id = 2,
                 .root = MirPlaceRootKind::Binding,
                 .binding = 2,
                 .type = pack,
                 .initiallyAvailable = true},
                {.id = 3,
                 .root = MirPlaceRootKind::Binding,
                 .binding = 2,
                 .projections = {{.kind = MirProjectionKind::PackElement,
                                  .constantIndex = 0}},
                 .type = view,
                 .traits = semanticTraits(view),
                 .initiallyAvailable = true},
                {.id = 4,
                 .root = MirPlaceRootKind::Binding,
                 .binding = 2,
                 .projections = {{.kind = MirProjectionKind::PackElement,
                                  .constantIndex = 1}},
                 .type = view,
                 .traits = semanticTraits(view),
                 .initiallyAvailable = true},
                {.id = 5,
                 .root = MirPlaceRootKind::Binding,
                 .binding = 1,
                 .projections = {{.kind = MirProjectionKind::Dereference}},
                 .type = view}};
  mir.loans = {{.id = 1,
                .kind = MirLoanKind::Parameter,
                .source = 1,
                .carriers = {1},
                .entry = true},
               {.id = 2,
                .kind = MirLoanKind::Stored,
                .source = 3,
                .carriers = {2},
                .entry = true},
               {.id = 3,
                .kind = MirLoanKind::Stored,
                .source = 4,
                .carriers = {2},
                .entry = true}};
  mir.blocks = {{.id = 1,
                 .instructions = {store(1, 5, copy(4))},
                 .terminator = {.kind = MirTerminatorKind::Return}}};
  auto effects = deriveMirRetainingStoreEffects(function);
  expect(effects.complete && effects.effects.size() == 1 &&
             effects.effects[0].source ==
                 MirCarrierRoot{MirPlaceRootKind::Binding, 2, 1},
         "retaining the second pack element does not retain the first");
  mir.blocks[0].instructions[0].operands = {copy(3)};
  effects = deriveMirRetainingStoreEffects(function);
  expect(effects.complete && effects.effects.size() == 1 &&
             effects.effects[0].source ==
                 MirCarrierRoot{MirPlaceRootKind::Binding, 2, 0},
         "retaining the first pack element does not retain the second");
  mir.places[2].projections[0].constantIndex = 2;
  expect(!deriveMirRetainingStoreEffects(function).complete,
         "an invalid projected entry cannot become a formal retention proof");
}

void ownedStorageInitialization() {
  MirProgram program;
  const auto element = SemanticType(SemanticType::StringView);
  const auto pointer = SemanticType::rawPointerTo(element, AccessMode::Mutable);
  const auto owner =
      SemanticType::referenceTo(SemanticType::UInt64, AccessMode::Mutable);
  MirInstruction initialize{
      .id = 5,
      .kind = MirInstructionKind::Call,
      .unsafeOperation = UnsafeOperationKind::ObjectLifetime,
      .operands = {{.kind = MirOperandKind::Value, .value = 1, .type = pointer},
                   {.kind = MirOperandKind::Value, .value = 2, .type = owner},
                   {.kind = MirOperandKind::Value,
                    .value = 3,
                    .type = element}},
      .parameterTypes = {pointer, owner, element},
      .intrinsic = IntrinsicKind::ObjectInitializeOwned,
      .info = {.type = SemanticType::Void,
               .traits = semanticTraits(SemanticType::Void)}};
  expect(
      mir_detail::objectInitializeOwned(program, initialize),
      "owned initialization binds an exact value and mutable storage witness");
  auto lent = initialize;
  lent.operands[1] = {.kind = MirOperandKind::Loan, .loan = 1, .type = owner};
  expect(mir_detail::objectInitializeOwned(program, lent),
         "the storage witness may be an existing checked reference loan");
  lent.operands[1].loan = 0;
  expect(!mir_detail::objectInitializeOwned(program, lent),
         "a missing loan cannot stand in for the storage witness");
  lent.operands[1] = {.kind = MirOperandKind::Copy, .place = 1, .type = owner};
  expect(!mir_detail::objectInitializeOwned(program, lent),
         "an unstaged owner copy cannot stand in for exclusive access");
  auto invalid = initialize;
  invalid.unsafeOperation = UnsafeOperationKind::None;
  expect(!mir_detail::objectInitializeOwned(program, invalid),
         "an owner witness cannot waive raw address validity obligations");
  invalid = initialize;
  invalid.parameterTypes[1] =
      SemanticType::referenceTo(SemanticType::UInt64, AccessMode::ReadOnly);
  invalid.operands[1].type = invalid.parameterTypes[1];
  expect(!mir_detail::objectInitializeOwned(program, invalid),
         "initializing owned storage requires exclusive access to its witness");
  invalid = initialize;
  invalid.operands[2].type = SemanticType::UInt64;
  expect(!mir_detail::objectInitializeOwned(program, invalid),
         "the transferred value must match the allocated element type");
  auto aggregate = initialize;
  SemanticType array(SemanticType::Array);
  array.arguments = {element};
  array.arrayLength = 2;
  aggregate.parameterTypes[2] = array;
  aggregate.parameterTypes[0] =
      SemanticType::rawPointerTo(array, AccessMode::Mutable);
  aggregate.operands[0].type = aggregate.parameterTypes[0];
  aggregate.operands[2].type = array;
  expect(mir_detail::objectInitializeOwned(program, aggregate),
         "owned initialization supports aggregates, not a scalar text special "
         "case");

  auto mir = body();
  mir.blocks[0].instructions = {
      store(1, 1,
            {.kind = MirOperandKind::Constant, .type = SemanticType::UInt64}),
      {.id = 2,
       .kind = MirInstructionKind::Compute,
       .result = 1,
       .info = {.type = pointer}},
      {.id = 3,
       .kind = MirInstructionKind::CallInput,
       .callInputKind = HirCallInputKind::MutableBorrow,
       .result = 2,
       .operands = {copy(1)},
       .info = {.type = owner}},
      {.id = 4,
       .kind = MirInstructionKind::CallInput,
       .result = 3,
       .operands = {loan(1)},
       .info = {.type = element}},
      initialize,
      read(6, 1)};
  auto flow = deriveMirCarrierFlow(mir, &program);
  expect(flow.effectsComplete && flow.reads.at(6) == set({1}),
         "allocated storage retains the source dependency through its owner "
         "witness");
  mir.blocks[0].instructions[3].operands = {loan(2)};
  auto second = initialize;
  second.id = 8;
  second.operands[2].value = 4;
  mir.blocks[0].instructions.insert(mir.blocks[0].instructions.end() - 1,
                                    {.id = 7,
                                     .kind = MirInstructionKind::CallInput,
                                     .result = 4,
                                     .operands = {loan(1)},
                                     .info = {.type = element}});
  mir.blocks[0].instructions.insert(mir.blocks[0].instructions.end() - 1,
                                    second);
  flow = deriveMirCarrierFlow(mir, &program);
  expect(flow.effectsComplete && flow.reads.at(6) == set({1, 2}),
         "initializing another element widens the owner's retained dependency "
         "set");
}

void transferredStorageDoesNotObserveCleanup() {
  auto mir = body();
  mir.dropObligations = {{.id = 1, .place = 1}, {.id = 2, .place = 2}};
  auto moved = read(2, 1);
  moved.kind = MirInstructionKind::Move;
  moved.result = 1;
  moved.lifecycle = {
      {.kind = MirLifecycleEventKind::Move, .source = 1, .target = 2}};
  mir.blocks[0].instructions = {
      store(1, 1, loan(1)),
      moved,
      store(3, 2, {.kind = MirOperandKind::Value, .value = 1}),
      {.id = 4, .kind = MirInstructionKind::EndBorrow, .loan = 1},
      {.id = 5, .kind = MirInstructionKind::Drop, .destination = 1},
      read(6, 2)};
  const auto flow = deriveMirCarrierFlow(mir);
  expect(flow.reads.at(2) == set({1}) && flow.reads.at(5) == set({}) &&
             flow.unavailableReads.at(5) == set({}) &&
             flow.reads.at(6) == set({1}) &&
             flow.unavailableReads.at(6) == set({1}),
         "moving empties source cleanup without erasing the moved value's "
         "dependency or expiration");
  auto failure = body(3);
  failure.dropObligations = mir.dropObligations;
  failure.blocks[0].instructions = {store(1, 1, loan(1)), moved};
  failure.blocks[0].terminator = {.kind = MirTerminatorKind::Invoke,
                                  .invokeInstruction = 2,
                                  .target = 2,
                                  .elseTarget = 3};
  failure.blocks[1].instructions = {read(3, 1)};
  failure.blocks[2].instructions = {read(4, 1)};
  const auto failed = deriveMirCarrierFlow(failure);
  expect(failed.reads.at(3) == set({}) && failed.reads.at(4) == set({1}),
         "a failed transfer preserves possible source cleanup dependencies");
  mir.blocks[0].instructions[1].lifecycle[0].conditional = true;
  expect(deriveMirCarrierFlow(mir).unavailableReads.at(5) == set({1}),
         "a conditional transfer cannot erase possible observing cleanup");
  mir.blocks[0].instructions[1].lifecycle[0].conditional = false;
  auto projected = mir.places[0];
  projected.id = 8;
  projected.projections = {{.kind = MirProjectionKind::Field}};
  mir.places.push_back(projected);
  mir.dropObligations[0].place = 8;
  expect(deriveMirCarrierFlow(mir).unavailableReads.at(5) == set({1}),
         "a projected move cannot erase the containing storage's dependencies");
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
  auto flow = deriveMirCarrierFlow(mir, nullptr, true);
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
  flow = deriveMirCarrierFlow(mir, nullptr, true);
  expect(flow.complete && flow.reads.at(2) == set({1, 2}),
         "loop backedges reach a finite fixed point");
  expect(flow.reads.at(4) == set({2}),
         "replacement remains precise within a loop");
  expect(
      flow.entryRoots.at(2).at({MirPlaceRootKind::Binding, 1}) == set({1, 2}) &&
          flow.entryRoots.at(3).at({MirPlaceRootKind::Binding, 1}) == set({2}),
      "entry facts distinguish loop-header alternatives from post-store exits");
}
void projectedAndAliasStores() {
  auto mir = body();
  mir.places[3] = {
      .id = 4,
      .root = MirPlaceRootKind::Binding,
      .binding = 1,
      .projections = {{.kind = MirProjectionKind::Field, .field = 7}}};
  mir.loans.push_back(
      {.id = 3, .kind = MirLoanKind::Local, .source = 1, .entry = true});
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
  mir.loans.push_back(
      {.id = 3, .kind = MirLoanKind::Local, .source = 1, .entry = true});
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
  mir.blocks[0].instructions = {
      store(1, 1, loan(1)),
      store(2, 3, loan(3)),
      store(3, 2, copy(1)),
      {.id = 4, .kind = MirInstructionKind::EndBorrow, .loan = 1},
      store(5, 1, {.kind = MirOperandKind::Constant}),
      read(6, 4),
      read(7, 2)};
  const auto expired = deriveMirCarrierFlow(mir);
  expect(expired.unavailableReads.at(6) == set({}) &&
             expired.unavailableReads.at(7) == set({1}),
         "an alias reads current slot validity while an independent copy stays "
         "expired");
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
  mir.blocks[0].instructions.push_back(
      {.id = 5, .kind = MirInstructionKind::EndBorrow, .loan = 1});
  const auto expired = deriveMirCarrierFlow(mir);
  expect(expired.unavailableReads.at(3) == set({1}) &&
             expired.unavailableReads.at(4) == set({1}),
         "loop-carried swaps transport expiration with each simultaneous "
         "argument");
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
  mir.blocks[0].instructions.insert(
      std::next(mir.blocks[0].instructions.begin()),
      {.id = 5, .kind = MirInstructionKind::EndBorrow, .loan = 1});
  const auto expired = deriveMirCarrierFlow(mir);
  expect(expired.unavailableReads.at(3) == set({}) &&
             expired.unavailableReads.at(4) == set({1}),
         "failed replacement preserves the old value's expiration while "
         "success replaces it");
  mir.blocks[0].instructions.erase(
      std::next(mir.blocks[0].instructions.begin()));

  mir.blocks[0].instructions[1].result = 1;
  mir.blocks[2].instructions = {
      {.id = 4,
       .kind = MirInstructionKind::Load,
       .operands = {{.kind = MirOperandKind::Value, .value = 1}}}};
  const auto failedResult = deriveMirCarrierFlow(mir);
  expect(!failedResult.unavailableReads.at(4),
         "a failed SSA result cannot acquire a known validity fact");
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
  mir.blocks[0].instructions = {
      {.id = 1,
       .kind = MirInstructionKind::Call,
       .result = 1,
       .operands = {loan(1)},
       .info = {.type = SemanticType::StringView}},
      {.id = 2, .kind = MirInstructionKind::EndBorrow, .loan = 1}};
  mir.blocks[0].terminator.value = MirOperand{.kind = MirOperandKind::Value,
                                              .value = 1,
                                              .type = SemanticType::StringView};
  const auto uncontractedResult = deriveMirCarrierFlow(mir);
  expect(!uncontractedResult.complete &&
             uncontractedResult.reads.at(1) == set({1}) &&
             !uncontractedResult.exits.at(1) &&
             !uncontractedResult.unavailableExits.at(1),
         "a call reads its argument but cannot claim its result relays that "
         "argument without a contract");
  mir.blocks[0].instructions.front().kind = MirInstructionKind::Construct;
  mir.blocks[0].instructions.front().info.type.kind = SemanticType::Class;
  mir.blocks[0].terminator.value->type.kind = SemanticType::Class;
  const auto uncontractedConstruction = deriveMirCarrierFlow(mir);
  expect(!uncontractedConstruction.complete &&
             !uncontractedConstruction.effectsComplete &&
             uncontractedConstruction.reads.at(1) == set({1}) &&
             !uncontractedConstruction.exits.at(1) &&
             !uncontractedConstruction.unavailableExits.at(1),
         "unknown construction cannot invent either retained inputs or an "
         "ownerless result");
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
  expect(effects.writeEffects.size() == 2 &&
             effects.writeEffects.at(1) ==
                 std::vector<MirRetainingStoreEffect>{effects.effects[0]} &&
             effects.writeEffects.at(2) ==
                 std::vector<MirRetainingStoreEffect>{effects.effects[1]},
         "each boundary write retains its own input relation before summary "
         "union");
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
  expect(
      !deriveMirRetainingStoreEffects(function).complete,
      "an unknown additional owner cannot disappear from a retaining summary");
  mir.loans[0].additionalDependencies.clear();
  mir.places[4] = {.id = 5, .root = MirPlaceRootKind::Loan, .loan = 1};
  mir.places[5] = {.id = 6, .root = MirPlaceRootKind::Loan, .loan = 2};
  mir.loans.push_back(
      {.id = 3, .parent = 1, .kind = MirLoanKind::Local, .source = 5});
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
  mir.places[4].projections = {{.kind = MirProjectionKind::RawDereference}};
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
  wrapper.body.blocks[0].instructions = {{.id = 1,
                                          .kind = MirInstructionKind::Call,
                                          .receiver = copy(4),
                                          .operands = {copy(3), copy(2)},
                                          .functionTarget = leaf.id}};
  auto outer = wrapper;
  outer.id = 3;
  outer.body.blocks[0].instructions[0].functionTarget = wrapper.id;
  MirProgram program;
  const auto checkSession = [&]() {
    // Each mutation below starts a fresh analysis session on the same program
    // address. Query helpers before their callers, then revisit them after
    // extending the solved graph; recursive and unknown facts must agree with
    // independent cold queries in either order.
    MirCarrierAnalysis session(program);
    for (const auto id : {1U, 2U, 3U, 2U, 1U, 3U}) {
      const auto *function = program.findFunctionInstance(id);
      const auto cold = deriveMirRetainingStoreEffects(*function, &program);
      const auto warm = session.retainingStores(id);
      expect(cold.complete == warm.complete && cold.effects == warm.effects &&
                 cold.returnedOrigins == warm.returnedOrigins &&
                 cold.writeEffects == warm.writeEffects,
             "session summaries match cold proofs across helper graphs");
      const auto fresh = deriveMirCarrierFlow(function->body, &program, true);
      const auto reused = session.flow(function->body, true);
      expect(fresh.complete == reused.complete &&
                 fresh.effectsComplete == reused.effectsComplete &&
                 fresh.entryRoots == reused.entryRoots &&
                 fresh.reads == reused.reads &&
                 fresh.unavailableReads == reused.unavailableReads &&
                 fresh.stores == reused.stores &&
                 fresh.storeInputsByTarget == reused.storeInputsByTarget &&
                 fresh.returned == reused.returned &&
                 fresh.unavailableExits == reused.unavailableExits,
             "session flow preserves provenance, expiration and unknown facts");
    }
    // Lowering rebuilds a detached caller while keeping the program supplying
    // callee contracts immutable. Reusing that session must never reuse the
    // caller's previous flow, even at the same body address.
    MirBody detached = program.findFunctionInstance(3)->body;
    for (unsigned revision = 0; revision != 4; ++revision) {
      if (revision == 1)
        std::swap(detached.blocks[0].instructions[0].operands[0],
                  detached.blocks[0].instructions[0].operands[1]);
      if (revision == 2)
        detached.blocks[0].instructions[0].functionTarget = 99;
      if (revision == 3)
        detached.blocks[0].instructions.clear();
      const auto fresh = deriveMirCarrierFlow(detached, &program, true);
      const auto reused = session.flow(detached, true);
      expect(fresh.complete == reused.complete &&
                 fresh.effectsComplete == reused.effectsComplete &&
                 fresh.entryRoots == reused.entryRoots &&
                 fresh.reads == reused.reads && fresh.stores == reused.stores &&
                 fresh.storeInputsByTarget == reused.storeInputsByTarget &&
                 fresh.retainingInputs == reused.retainingInputs &&
                 fresh.unavailableReads == reused.unavailableReads &&
                 fresh.returned == reused.returned,
             "detached caller revisions get fresh flow with cached callees");
    }
    expect(!session.retainingStores(99).complete,
           "a session never certifies an unavailable helper");
  };
  MirProgramTestAccess::functions(program, {leaf, wrapper, outer});
  checkSession();
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
  checkSession();
  const auto emptyCycle = deriveMirRetainingStoreEffects(outer, &program);
  expect(emptyCycle.complete && emptyCycle.effects.empty(),
         "a fully inspected recursive cycle without writes converges to empty");
  leaf.body.blocks[0].instructions[0].operands = {copy(2), copy(3)};
  leaf.body.blocks[0].instructions.push_back(store(2, 4, copy(2)));
  MirProgramTestAccess::functions(program, {leaf, wrapper, outer});
  checkSession();
  const auto recursive = deriveMirRetainingStoreEffects(outer, &program);
  expect(recursive.complete && recursive.effects.size() == 2 &&
             recursive.effects[0].source ==
                 MirCarrierRoot{MirPlaceRootKind::Binding, 2} &&
             recursive.effects[1].source ==
                 MirCarrierRoot{MirPlaceRootKind::Binding, 3},
         "recursive input permutations retain every owner at the fixed point");
  const auto recursiveFlow = deriveMirCarrierFlow(outer.body, &program);
  expect(recursiveFlow.effectsComplete &&
             recursiveFlow.storeInputs.at(1) == set({1, 2}),
         "ordinary caller flow consumes the converged recursive effects");
  auto partialLeaf = leaf;
  partialLeaf.body.blocks[0].instructions.push_back(
      {.id = 3, .kind = MirInstructionKind::Call, .functionTarget = 99});
  MirProgramTestAccess::functions(program, {partialLeaf, wrapper, outer});
  checkSession();
  const auto partial = deriveMirRetainingStoreEffects(outer, &program);
  expect(!partial.complete && !partial.returnedOrigins &&
             partial.effects == recursive.effects,
         "unknown effects cannot erase witnessed recursive retaining writes");
  const auto partialFlow = deriveMirCarrierFlow(outer.body, &program);
  expect(
      !partialFlow.effectsComplete &&
          partialFlow.storeInputs.at(1) == set({1, 2}),
      "partial caller summaries widen obligations without granting precision");
  auto localEscape = leaf;
  localEscape.body.loans[0].entry = false;
  MirProgramTestAccess::functions(program, {localEscape, wrapper, outer});
  checkSession();
  expect(
      !deriveMirRetainingStoreEffects(outer, &program).complete,
      "a local escaping origin invalidates every caller in a recursive cycle");
  auto opaque = leaf;
  opaque.definitionKind = MirDefinitionKind::Declaration;
  MirProgramTestAccess::functions(program, {opaque, wrapper, outer});
  checkSession();
  expect(!deriveMirRetainingStoreEffects(outer, &program).complete,
         "an opaque body cannot be certified by recursive provisional facts");
  leaf.body.blocks[0].instructions[0].functionTarget = 99;
  MirProgramTestAccess::functions(program, {leaf, wrapper, outer});
  checkSession();
  expect(!deriveMirRetainingStoreEffects(outer, &program).complete,
         "an unavailable nested target leaves the outer summary incomplete");
}
void representedResultKeepsRelayedContents() {
  MirFunctionInstance relay;
  relay.id = 1;
  relay.definitionKind = MirDefinitionKind::Source;
  relay.parameterBindings = {1};
  relay.parameterTypes = {SemanticType::StringView};
  relay.returnType = SemanticType::StringView;
  relay.body = body();
  relay.body.kind = MirBodyKind::Function;
  relay.body.returnType = SemanticType::StringView;
  relay.body.loans.resize(1);
  relay.body.loans[0].source = 1;
  relay.body.loans[0].carriers = {1};
  relay.body.places[0].type = SemanticType::StringView;
  relay.body.blocks[0].terminator.value = copy(1);
  MirProgram program;
  MirProgramTestAccess::functions(program, {relay});
  auto caller = body();
  caller.loans.push_back({.id = 3, .kind = MirLoanKind::Stored, .source = 1});
  caller.blocks[0].instructions = {
      {.id = 1,
       .kind = MirInstructionKind::Call,
       .result = 1,
       .operands = {loan(1)},
       .loan = 3,
       .functionTarget = 1,
       .info = {.type = SemanticType::StringView}},
      store(2, 2, {.kind = MirOperandKind::Value, .value = 1}),
      {.id = 3, .kind = MirInstructionKind::EndBorrow, .loan = 1},
      read(4, 2)};
  const auto flow = deriveMirCarrierFlow(caller, &program);
  expect(
      flow.effectsComplete && flow.reads.at(4) == set({1, 3}),
      "a represented result preserves its relayed contents and physical loan");
  expect(flow.unavailableReads.at(4) == set({1}),
         "the physical result loan does not revive an expired relayed owner");
  auto independent = caller;
  independent.loans.back().source = independent.loans.front().source;
  const auto copied = deriveMirCarrierFlow(independent, &program);
  expect(copied.reads.at(4) == set({3}) &&
             copied.unavailableReads.at(4) == set({}),
         "an independently represented copy survives ending the input carrier "
         "loan");
  auto fieldOrigin = independent;
  fieldOrigin.loans.front().designatedFields = {1};
  const auto fieldCopy = deriveMirCarrierFlow(fieldOrigin, &program);
  expect(fieldCopy.reads.at(4) == set({1, 3}) &&
             fieldCopy.unavailableReads.at(4) == set({1}),
         "matching physical sources do not erase distinct field provenance");
  auto sameField = fieldOrigin;
  sameField.loans.back().designatedFields = {1};
  const auto sameFieldCopy = deriveMirCarrierFlow(sameField, &program);
  expect(
      sameFieldCopy.reads.at(4) == set({3}) &&
          sameFieldCopy.unavailableReads.at(4) == set({}),
      "an identity relay preserves exact field provenance without remapping");
  auto opaqueField = independent;
  SemanticType recordType = SemanticType::Class;
  recordType.classId = 17;
  MirClassInstance recordTypeInfo;
  recordTypeInfo.id = 1;
  recordTypeInfo.type = recordType;
  recordTypeInfo.traits.containsBorrowedState = true;
  recordTypeInfo.declaredFields = {
      {.symbol = 1,
       .type = SemanticType::referenceTo(SemanticType::Int32,
                                         AccessMode::ReadOnly)}};
  auto recordRelay = relay;
  recordRelay.parameterTypes = {recordType};
  recordRelay.returnType = recordType;
  recordRelay.body.returnType = recordType;
  recordRelay.body.places[0].type = recordType;
  MirProgram recordProgram;
  MirProgramTestAccess::functions(recordProgram, {recordRelay});
  MirProgramTestAccess::classes(recordProgram, {recordTypeInfo});
  opaqueField.loans.back().designatedFields = {1};
  auto &fieldCall = opaqueField.blocks[0].instructions[0];
  fieldCall.info.type = recordType;
  fieldCall.operands.front().type = recordType;
  fieldCall.borrowFields = {{.resultFields = {1},
                             .origin = BorrowOriginKind::Argument,
                             .parameter = 0,
                             .sourceFields = {1}}};
  const auto opaqueCopy = deriveMirCarrierFlow(opaqueField, &recordProgram);
  expect(opaqueCopy.reads.at(4) == set({3}) &&
             opaqueCopy.unavailableReads.at(4) == set({}),
         "an opaque dependency relays through the proved sole reference field");
  recordTypeInfo.declaredFields.push_back(
      {.symbol = 2,
       .type = SemanticType::referenceTo(SemanticType::Int32,
                                         AccessMode::ReadOnly)});
  MirProgramTestAccess::classes(recordProgram, {recordTypeInfo});
  expect(deriveMirCarrierFlow(opaqueField, &recordProgram)
                 .unavailableReads.at(4) == set({1}),
         "multiple reference fields cannot guess an opaque dependency path");
  recordTypeInfo.declaredFields.back().type = SemanticType::StringView;
  MirProgramTestAccess::classes(recordProgram, {recordTypeInfo});
  expect(deriveMirCarrierFlow(opaqueField, &recordProgram)
                 .unavailableReads.at(4) == set({1}),
         "opaque text beside a reference prevents sole-dependency inference");
  recordTypeInfo.declaredFields.back().type =
      SemanticType::rawPointerTo(SemanticType::Int32);
  MirProgramTestAccess::classes(recordProgram, {recordTypeInfo});
  expect(deriveMirCarrierFlow(opaqueField, &recordProgram)
                 .unavailableReads.at(4) == set({1}),
         "opaque pointer storage cannot prove a sole reference dependency");
  auto mappedField = fieldOrigin;
  mappedField.blocks[0].instructions[0].borrowFields = {
      {.origin = BorrowOriginKind::Argument,
       .parameter = 0,
       .sourceFields = {1}}};
  const auto mapped = deriveMirCarrierFlow(mappedField, &program);
  expect(mapped.reads.at(4) == set({3}) &&
             mapped.unavailableReads.at(4) == set({}),
         "an exact field relay publishes an independent external dependency");
  auto wrongField = mappedField;
  wrongField.blocks[0].instructions[0].borrowFields[0].sourceFields = {2};
  expect(deriveMirCarrierFlow(wrongField, &program).unavailableReads.at(4) ==
             set({1}),
         "a different field relay cannot erase the input dependency");
  auto temporary = mappedField;
  temporary.loans.push_back({.id = 4,
                             .storedDependency = 1,
                             .kind = MirLoanKind::CallArgument,
                             .source = 1,
                             .entry = true});
  temporary.blocks[0].instructions[0].operands = {loan(4)};
  temporary.blocks[0].instructions[2].loan = 4;
  const auto temporaryCopy = deriveMirCarrierFlow(temporary, &program);
  expect(temporaryCopy.reads.at(4) == set({3}) &&
             temporaryCopy.unavailableReads.at(4) == set({}),
         "a field relay does not retain a temporary's physical call shell");
  temporary.blocks[0].instructions.insert(
      temporary.blocks[0].instructions.begin(),
      {.id = 6, .kind = MirInstructionKind::EndBorrow, .loan = 1});
  expect(deriveMirCarrierFlow(temporary, &program).reads.at(4) == set({3, 4}),
         "an expired external dependency cannot be replaced through its shell");

  independent.blocks[0].instructions.insert(
      independent.blocks[0].instructions.begin(),
      {.id = 6, .kind = MirInstructionKind::EndBorrow, .loan = 1});
  const auto stale = deriveMirCarrierFlow(independent, &program);
  expect(stale.unavailableReads.at(4) == set({1}),
         "a fresh result loan cannot revive contents expired before the copy");
  caller.blocks[0].instructions.front().info.traits.containsBorrowedState =
      true;
  caller.blocks[0].instructions.insert(caller.blocks[0].instructions.begin(),
                                       store(5, 1, loan(2)));
  const auto adapter = deriveMirCarrierFlow(caller, &program);
  expect(
      adapter.reads.at(4) == set({1, 2, 3}),
      "a stored-reference result also retains its physical owner's contents");
  caller.places[0].type =
      SemanticType::referenceTo(SemanticType::Int32, AccessMode::ReadOnly);
  const auto scalarOwner = deriveMirCarrierFlow(caller, &program);
  expect(
      scalarOwner.reads.at(4) == set({1, 3}),
      "a scalar referent does not relay the parameter's physical capability");
}

void resultContentsFollowCallWrites() {
  MirFunctionInstance append;
  append.id = 1;
  append.definitionKind = MirDefinitionKind::Source;
  append.owner = 1;
  append.parameterBindings = {2};
  append.parameterTypes = {SemanticType::StringView};
  append.body = body();
  append.body.kind = MirBodyKind::Function;
  append.body.loans.resize(1);
  append.body.loans[0].source = 2;
  append.body.loans[0].carriers = {2};
  append.body.places[3] = {.id = 4, .root = MirPlaceRootKind::This};
  append.body.blocks[0].instructions = {store(1, 4, copy(2))};
  MirProgram program;
  MirProgramTestAccess::functions(program, {append});
  auto caller = body();
  caller.loans.push_back(
      {.id = 3, .kind = MirLoanKind::CallResult, .source = 1});
  caller.loans.push_back(
      {.id = 4, .kind = MirLoanKind::CallResult, .source = 1});
  caller.blocks[0].instructions = {
      store(1, 1, {.kind = MirOperandKind::Constant}),
      {.id = 2,
       .kind = MirInstructionKind::Call,
       .receiver = copy(1),
       .operands = {loan(1)},
       .loan = 3,
       .functionTarget = 1},
      {.id = 3,
       .kind = MirInstructionKind::Call,
       .receiver = copy(1),
       .operands = {loan(2)},
       .loan = 4,
       .functionTarget = 1}};
  auto flow = deriveMirCarrierFlow(caller, &program);
  expect(flow.effectsComplete && flow.resultSourceContents.at(2) == set({1}) &&
             flow.resultSourceContents.at(3) == set({1, 2}),
         "returned storage contents include this call's writes but not future "
         "writes");
  expect(flow.unavailableResultSourceContents.at(2) == set({}) &&
             flow.unavailableResultSourceContents.at(3) == set({}),
         "live returned storage contents retain no expired origins");
  caller.blocks[0].instructions.insert(
      caller.blocks[0].instructions.begin() + 2,
      {.id = 4, .kind = MirInstructionKind::EndBorrow, .loan = 1});
  flow = deriveMirCarrierFlow(caller, &program);
  expect(flow.unavailableResultSourceContents.at(3) == set({1}),
         "a later storage result exposes an expired retained owner");
  caller.blocks[0].instructions.back().functionTarget = 99;
  flow = deriveMirCarrierFlow(caller, &program);
  expect(!flow.resultSourceContents.at(3),
         "an unmodeled call cannot certify its returned storage contents");
}

void independentRetainingDestinations() {
  MirFunctionInstance leaf;
  leaf.id = 1;
  leaf.definitionKind = MirDefinitionKind::Source;
  leaf.parameterBindings = {1, 2, 3, 4};
  const auto reference =
      SemanticType::referenceTo(SemanticType::StringView, AccessMode::Mutable);
  leaf.parameterTypes = {reference, reference, SemanticType::StringView,
                         SemanticType::StringView};
  leaf.body = body();
  auto &mir = leaf.body;
  mir.kind = MirBodyKind::Function;
  mir.loans.clear();
  for (std::size_t id = 1; id <= 4; ++id)
    mir.loans.push_back({.id = id,
                         .kind = MirLoanKind::Stored,
                         .source = id,
                         .carriers = {id},
                         .entry = true});
  mir.places[4] = {.id = 5, .root = MirPlaceRootKind::Loan, .loan = 1};
  mir.places[5] = {.id = 6, .root = MirPlaceRootKind::Loan, .loan = 2};
  mir.blocks[0].instructions = {store(1, 5, copy(3)), store(2, 6, copy(4))};
  auto wrapper = leaf;
  wrapper.id = 2;
  wrapper.body.blocks[0].instructions = {
      {.id = 1,
       .kind = MirInstructionKind::Call,
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
  expect(
      effects.complete && effects.effects.size() == 2 &&
          effects.effects[0] ==
              MirRetainingStoreEffect{first, {MirPlaceRootKind::Binding, 3}} &&
          effects.effects[1] ==
              MirRetainingStoreEffect{second, {MirPlaceRootKind::Binding, 4}},
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
  mir.blocks[0].instructions = {store(1, 1, loan(1)),
                                store(2, 2,
                                      {.kind = MirOperandKind::Copy,
                                       .place = 3,
                                       .type = SemanticType::Int32}),
                                read(3, 2)};
  const auto flow = deriveMirCarrierFlow(mir);
  expect(flow.complete && flow.reads.at(2) == set({1}) &&
             flow.stores.at(2) == set({}) && flow.reads.at(3) == set({}),
         "a scalar projection uses its carrier without retaining it in the "
         "result");
  mir.places[2].projections.push_back({.kind = MirProjectionKind::Dereference});
  mir.blocks[0].instructions.insert(mir.blocks[0].instructions.begin() + 1,
                                    store(4, 1, loan(2)));
  auto replaced = deriveMirCarrierFlow(mir);
  expect(replaced.complete && replaced.reads.at(2) == set({2}) &&
             replaced.stores.at(2) == set({}),
         "a scalar reference-field read uses the replacement's current "
         "dependency without retaining it in the scalar result");
  mir.blocks[0].instructions.insert(
      mir.blocks[0].instructions.begin() + 2,
      {.id = 5, .kind = MirInstructionKind::EndBorrow, .loan = 2});
  replaced = deriveMirCarrierFlow(mir);
  expect(replaced.unavailableReads.at(2) == set({2}),
         "a scalar reference-field read cannot revive an expired owner");
  auto staged = mir;
  auto &input = staged.blocks[0].instructions[3];
  input.kind = MirInstructionKind::CallInput;
  input.callInputKind = HirCallInputKind::ReadBorrow;
  input.result = 1;
  input.operands.front().kind = MirOperandKind::BorrowRead;
  input.operands.front().type =
      SemanticType::referenceTo(SemanticType::Int32, AccessMode::ReadOnly);
  input.info.type = input.operands.front().type;
  staged.blocks[0].instructions.push_back(
      store(6, 4, {.kind = MirOperandKind::Value, .value = 1}));
  const auto stagedFlow = deriveMirCarrierFlow(staged);
  expect(
      stagedFlow.reads.at(2) == set({2}) &&
          stagedFlow.unavailableReads.at(2) == set({2}) &&
          stagedFlow.stores.at(6) == set({2}) &&
          stagedFlow.unavailableReads.at(6) == set({2}),
      "a staged scalar reference keeps the current owner and its expiration");
  mir.places[2].projections.back().kind = MirProjectionKind::RawDereference;
  expect(!deriveMirCarrierFlow(mir).reads.at(2),
         "raw pointer fields do not inherit checked reference-field reads");
  mir.places[2].projections.back().kind = MirProjectionKind::Dereference;
  mir.places[2].type = SemanticType::StringView;
  mir.blocks[0].instructions[3].operands.front().type =
      SemanticType::StringView;
  expect(!deriveMirCarrierFlow(mir).reads.at(2),
         "reading borrowed contents through a reference still requires its "
         "own content provenance");
}
void expiredCarriedValues() {
  auto mir = body();
  mir.blocks[0].instructions = {
      store(1, 1, loan(1)),
      store(2, 2, copy(1)),
      {.id = 3, .kind = MirInstructionKind::EndBorrow, .loan = 1},
      store(4, 1, loan(2)),
      read(5, 1),
      read(6, 2)};
  auto flow = deriveMirCarrierFlow(mir);
  expect(flow.unavailableReads.at(5) == set({}) &&
             flow.unavailableReads.at(6) == set({1}),
         "replacement repairs its slot without repairing an expired copy");
  mir.loans[0].entry = false;
  // Expiration of a value read through SSA also survives loss of all
  // non-entry carrier-list annotations.
  mir.blocks[0].instructions = {
      {.id = 1, .kind = MirInstructionKind::Borrow, .result = 1, .loan = 1},
      {.id = 2, .kind = MirInstructionKind::EndBorrow, .loan = 1},
      store(3, 1, {.kind = MirOperandKind::Value, .value = 1}),
      read(4, 1)};
  flow = deriveMirCarrierFlow(mir);
  expect(flow.unavailableReads.at(3) == set({1}) &&
             flow.unavailableReads.at(4) == set({1}),
         "an ended SSA view cannot be stored or read through a new carrier");
  mir.blocks[0].terminator.value = copy(1);
  flow = deriveMirCarrierFlow(mir);
  expect(flow.unavailableExits.at(1) == set({1}),
         "returning an expired stored view is also an executable use");
}
void expirationAcrossBranches() {
  auto mir = body(4);
  mir.loans[0].entry = false;
  mir.blocks[0].instructions = {
      store(1, 1, {.kind = MirOperandKind::Constant})};
  mir.blocks[0].terminator = {
      .kind = MirTerminatorKind::Branch, .target = 2, .elseTarget = 3};
  mir.blocks[1].instructions = {
      {.id = 2, .kind = MirInstructionKind::Borrow, .result = 1, .loan = 1},
      store(3, 1, {.kind = MirOperandKind::Value, .value = 1})};
  mir.blocks[1].terminator = {.kind = MirTerminatorKind::Goto, .target = 4};
  mir.blocks[2].terminator = {.kind = MirTerminatorKind::Goto, .target = 4};
  mir.blocks[3].instructions = {read(4, 1)};
  // Exercise the concrete text-copy path as emitted by lowering, not just
  // untyped synthetic operands. Absence of a loan on the literal branch is
  // different from expiration of a view that actually carries that loan.
  mir.blocks[3].instructions.front().operands.front().type =
      SemanticType::StringView;
  auto flow = deriveMirCarrierFlow(mir);
  expect(flow.reads.at(4) == set({1}) && flow.unavailableReads.at(4) == set({}),
         "a group absent on one predecessor is not an expired carried value");
  mir.blocks[1].instructions.push_back(
      {.id = 5, .kind = MirInstructionKind::EndBorrow, .loan = 1});
  flow = deriveMirCarrierFlow(mir);
  expect(flow.unavailableReads.at(4) == set({1}),
         "a retained value expired on one executable path remains invalid at "
         "the join");
  mir.blocks[1].instructions.push_back(
      store(6, 1, {.kind = MirOperandKind::Constant}));
  flow = deriveMirCarrierFlow(mir);
  expect(
      flow.unavailableReads.at(4) == set({}),
      "overwriting the expired value before joining removes that obligation");
}
void expirationAcrossLoopReentry() {
  auto mir = body(3);
  mir.loans[0].entry = false;
  mir.blocks[0].instructions = {
      store(1, 1, {.kind = MirOperandKind::Constant}),
      store(2, 2, {.kind = MirOperandKind::Constant})};
  mir.blocks[0].terminator = {
      .kind = MirTerminatorKind::Branch, .target = 2, .elseTarget = 3};
  mir.blocks[1].instructions = {
      store(3, 2, copy(1)),
      {.id = 4, .kind = MirInstructionKind::Borrow, .result = 1, .loan = 1},
      store(5, 1, {.kind = MirOperandKind::Value, .value = 1})};
  mir.blocks[1].terminator = {
      .kind = MirTerminatorKind::Branch, .target = 2, .elseTarget = 3};
  mir.blocks[2].instructions = {read(6, 1), read(7, 2)};
  auto flow = deriveMirCarrierFlow(mir);
  expect(
      flow.unavailableReads.at(6) == set({}) &&
          flow.unavailableReads.at(7) == set({}),
      "zero iterations do not invent an ended dependency for the exit value");
  mir.blocks[1].instructions.push_back(
      {.id = 8, .kind = MirInstructionKind::EndBorrow, .loan = 1});
  flow = deriveMirCarrierFlow(mir);
  expect(flow.unavailableReads.at(5) == set({}) &&
             flow.unavailableReads.at(7) == set({1}),
         "a fresh producer on loop reentry does not resurrect the previous "
         "iteration's copy");
}
void producerAvailability() {
  auto mir = body(4);
  mir.loans[0].entry = false;
  mir.blocks[0].instructions = {
      store(1, 1, {.kind = MirOperandKind::Constant})};
  mir.blocks[0].terminator = {
      .kind = MirTerminatorKind::Branch, .target = 2, .elseTarget = 3};
  mir.blocks[1].instructions = {
      {.id = 2, .kind = MirInstructionKind::Borrow, .result = 1, .loan = 1},
      store(3, 1, {.kind = MirOperandKind::Value, .value = 1})};
  mir.blocks[1].terminator = {.kind = MirTerminatorKind::Goto, .target = 4};
  mir.blocks[2].terminator = {.kind = MirTerminatorKind::Goto, .target = 4};
  mir.blocks[3].instructions = {read(4, 1), store(5, 2, loan(1)), read(6, 2)};
  auto flow = deriveMirCarrierFlow(mir);
  expect(
      flow.unavailableReads.at(4) == set({}) &&
          flow.unavailableReads.at(5) == set({1}) &&
          flow.unavailableReads.at(6) == set({1}),
      "a carried value can be valid while a naked conditional loan ID is not");
  mir.blocks[0].instructions.push_back(store(7, 3, loan(1)));
  flow = deriveMirCarrierFlow(mir);
  expect(flow.unavailableReads.at(7) == set({1}),
         "a loan cannot be stored before its producer executes");
  mir.blocks[0].instructions.pop_back();
  mir.blocks[1].instructions[0].kind = MirInstructionKind::CallInput;
  flow = deriveMirCarrierFlow(mir);
  expect(flow.unavailableReads.at(3) == set({}),
         "a staged physical borrow is also an actual loan producer");

  mir = body(3);
  mir.loans[0].entry = false;
  mir.blocks[0].instructions = {
      {.id = 1, .kind = MirInstructionKind::Call, .result = 1, .loan = 1}};
  mir.blocks[0].terminator = {.kind = MirTerminatorKind::Invoke,
                              .invokeInstruction = 1,
                              .target = 2,
                              .elseTarget = 3};
  mir.blocks[1].instructions = {store(2, 1, loan(1))};
  mir.blocks[2].instructions = {store(3, 1, loan(1))};
  flow = deriveMirCarrierFlow(mir);
  expect(flow.unavailableReads.at(2) == set({}) &&
             flow.unavailableReads.at(3) == set({1}),
         "a failing call cannot publish its loan on the failure edge");
}
void copiedTextSeparatesReferenceStorage() {
  auto mir = body();
  SemanticType array = SemanticType::Array;
  array.arguments = {SemanticType::StringView};
  SemanticType reference = SemanticType::Reference;
  reference.arguments = {array};
  mir.places[0].type = reference;
  mir.places[1].type = SemanticType::StringView;
  mir.places[2].type = SemanticType::StringView;
  mir.places[2].binding = 1;
  mir.places[2].projections = {{.kind = MirProjectionKind::Dereference},
                               {.kind = MirProjectionKind::Index}};
  mir.loans = {
      {.id = 1, .kind = MirLoanKind::Stored, .source = 1, .entry = true},
      {.id = 2,
       .kind = MirLoanKind::Parameter,
       .source = 1,
       .carriers = {1},
       .entry = true}};
  mir.loans[1].storedDependency = 1;
  auto selected = copy(3);
  selected.type = SemanticType::StringView;
  mir.blocks[0].instructions = {
      store(1, 2, selected),
      {.id = 2, .kind = MirInstructionKind::EndBorrow, .loan = 2},
      read(3, 2),
      {.id = 4, .kind = MirInstructionKind::EndBorrow, .loan = 1},
      read(5, 2),
      store(6, 2, selected)};
  const auto flow = deriveMirCarrierFlow(mir);
  expect(flow.stores.at(1) == set({1}),
         "a copied text element retains external data rather than its array "
         "reference");
  expect(
      flow.reads.at(1) == set({1, 2}),
      "extracting a text element still reads the array reference capability");
  expect(flow.unavailableReads.at(3) == set({}),
         "ending the array reference does not invalidate copied external text");
  expect(flow.unavailableReads.at(5) == set({1}),
         "ending the external data capability does invalidate its copied text");
  expect(
      flow.unavailableReads.at(6) == set({1, 2}),
      "extraction after either capability expires cannot recreate permission");
  mir.blocks[0].instructions[1].tombstone = true;
  const auto liveShell = deriveMirCarrierFlow(mir);
  expect(liveShell.unavailableReads.at(6) == set({1}),
         "a live array reference does not revive expired external data");
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
void run() {
  intrinsicRetentionEffects();
  constantStorageRetentionEffects();
  unsafeMarkedCheckedCopyEffects();
  unsafeReferenceStagingEffects();
  directReceiverReadIsolation();
  expandedPackRetentionEffects();
  retainingPackParameterIdentity();
  retainedPackElementsStayIndependent();
  ownedStorageInitialization();
  transferredStorageDoesNotObserveCleanup();
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
  representedResultKeepsRelayedContents();
  resultContentsFollowCallWrites();
  scalarProjectionStillReadsCarrier();
  producerAvailability();
  expiredCarriedValues();
  expirationAcrossBranches();
  expirationAcrossLoopReentry();
  copiedTextSeparatesReferenceStorage();
  partialFactsExtendLiveness();
  std::cout << "MIR carrier flow tests passed\n";
}

} // namespace carrier_flow_tests

// MIR's raw-pointer live-borrow audit and cross-function persistent-storage
// borrow check (RFC 0004 phase 2); MIR is the authority for both.
#include "../src/compiler/mir_borrow_audits.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

const std::filesystem::path root =
    std::filesystem::path(__FILE__).parent_path().parent_path();

lang::FrontendResult analyze(const std::string &source, bool audit) {
  lang::FrontendOptions options;
  if (audit)
    options.rawPointerAudit = lang::RawPointerAuditMode::Warn;
  return lang::Frontend(options).analyze(root / "mir-borrow-audit-test.gti",
                                         source, {root / "stdlib/prelude.gti"},
                                         {}, {root / "stdlib"});
}

std::size_t rawFindings(const std::string &source) {
  const auto result = analyze(source, true);
  if (!result.mirValid) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 99;
  }
  return lang::auditMirRawAccesses(result.mir).size();
}

void rawAccesses() {
  expect(rawFindings(R"(
int main() {
  mut int value = 7;
  unsafe {
    auto pointer = &value;
    int& loan = value;
    *pointer = 9;
    return loan;
  }
})") == 1,
         "a raw write overlapping a live checked loan is a MIR finding");
  expect(rawFindings(R"(
int main() {
  mut int value = 7;
  unsafe {
    auto pointer = &value;
    int& loan = value;
    int before = loan;
    *pointer = before + 2;
    return value;
  }
})") == 0,
         "a loan MIR ended before the raw write is not live");
  expect(rawFindings(R"(
int main() {
  mut int value = 7;
  unsafe {
    auto pointer = &value;
    int& loan = value;
    int read = *pointer;
    return read + loan;
  }
})") == 0,
         "a raw read is compatible with a live shared loan");
  expect(rawFindings(R"(
struct Address { mut int* pointer = nullptr;  public: Address() = default; };
int main() {
  mut int value = 7;
  mut Address address{};
  Address* alias = &address;
  unsafe {
    alias->pointer = &value;
    int& checked = value;
    *address.pointer = 9;
    return checked;
  }
})") == 1,
         "provenance stored through a raw alias reaches the later access");
  expect(rawFindings(R"(
int main() {
  mut int values[2] = {1, 2};
  unsafe {
    auto pointer = &values[0];
    int& loan = values[1];
    *(pointer + 1) = 9;
    return loan;
  }
})") == 1,
         "pointer arithmetic widens an element origin to the array");
  expect(rawFindings(R"(
int main() {
  mut int value = 7;
  mut int other = 1;
  unsafe {
    mut int* pointer = &value;
    int& loan = value;
    pointer = &other;
    *pointer = 9;
    return loan;
  }
})") == 0,
         "reseating a raw pointer replaces its provenance");
  expect(rawFindings(R"(
unsafe void write_address(int* pointer) { unsafe { *pointer = 9; } }
unsafe void write_forwarded(int* pointer) { unsafe { write_address(pointer); } }
int main() {
  mut int owner = 7;
  int* pointer = &owner;
  int& held = owner;
  unsafe { write_forwarded(pointer); }
  return held;
})") == 1,
         "a callee's write through a raw formal is a caller finding, "
         "through forwarding calls");
  expect(rawFindings(R"(
mut int global = 7;
int* source() { return &global; }
int main() {
  auto pointer = source();
  int& checked = global;
  unsafe { *pointer = 9; }
  return checked;
})") == 1,
         "a callee's returned global address keeps its provenance");
  expect(rawFindings(R"(
int main() {
  mut int owner = 7;
  const int* pointer = &owner;
  auto read = [pointer]() -> int { unsafe { return *pointer; } };
  mut int& held = owner;
  int observed = read();
  held = 8;
  return observed + held;
})") == 1,
         "a closure's read through a captured raw pointer is a caller finding");
  expect(rawFindings(R"(
struct Cursor {
  mut int* target = nullptr;
public:
  Cursor() = default;
  void write() { unsafe { *this.target = 9; } }
};
int main() {
  mut int owner = 7;
  mut Cursor cursor{};
  unsafe { cursor.target = &owner; }
  int& held = owner;
  cursor.write();
  return held;
})") == 1,
         "a method's write through a receiver-held raw pointer is a caller "
         "finding");
}

void persistentStorageAuthority() {
  const std::string rejectedSource = R"(
mut int global = 1;
void bump() { global = global + 1; }
int main() {
  int& borrowed = global;
  bump();
  return borrowed;
}
)";
  const auto rejected = analyze(rejectedSource, false);
  const auto storage = std::find_if(
      rejected.diagnostics.begin(), rejected.diagnostics.end(),
      [](const auto &diagnostic) { return diagnostic.code == "GTI-S2045"; });
  const auto call = rejectedSource.find("bump();");
  expect(
      !rejected.semanticValid && storage != rejected.diagnostics.end() &&
          storage->primary.start >= call && storage->primary.end <= call + 6 &&
          std::none_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
                       [](const auto &diagnostic) {
                         return diagnostic.code == "GTI-B0001";
                       }),
      "MIR rejects the invocation at its call, not as an internal error");
  expect(storage != rejected.diagnostics.end() &&
             std::any_of(storage->related.begin(), storage->related.end(),
                         [](const auto &related) {
                           return related.message.find(
                                      "conflicting transitive storage "
                                      "access") != std::string::npos;
                         }) &&
             std::any_of(storage->related.begin(), storage->related.end(),
                         [](const auto &related) {
                           return related.message.find(
                                      "protected borrow originates") !=
                                  std::string::npos;
                         }),
         "the rejection names the transitive access and the protected borrow");
  const auto accepted = analyze(R"(
mut int global = 1;
void bump() { global = global + 1; }
int main() {
  int& borrowed = global;
  int copy = borrowed;
  bump();
  return copy;
}
)",
                                false);
  expect(accepted.mirValid &&
             lang::auditMirPersistentStorageBorrows(accepted.mir).empty(),
         "an ended loan admits the persistent-storage access");
}
void replacedRecordAndPackProofs() {
  const std::string copyAssignmentSource = R"(
class ReadCarrier {
public:
  int& referent;
  ReadCarrier(int& source) : referent(source) {}
};
int use() {
  mut int first_owner = 1;
  mut int second_owner = 2;
  mut ReadCarrier left = ReadCarrier(first_owner);
  ReadCarrier right = ReadCarrier(second_owner);
  left = right;
  first_owner = 3;
  return left.referent - 2;
}
)";
  const lang::FrontendResult copyAssignment = lang::Frontend().analyze(
      "stored-carrier-copy-assignment.gti", copyAssignmentSource);
  expect(copyAssignment.canGenerateCode() &&
             lang::verifyMirProgram(copyAssignment.mir).valid(),
         "whole-record shared replacement releases the previous owner and "
         "retains the replacement's referent");
  auto referenceCallSource =
      std::string("int read_reference(int& value) { return value; }\n") +
      copyAssignmentSource;
  referenceCallSource.replace(
      referenceCallSource.find("return left.referent - 2;"),
      std::string_view("return left.referent - 2;").size(),
      "return read_reference(left.referent) - 2;");
  const auto referenceCall = lang::Frontend().analyze(
      "stored-carrier-replaced-reference-argument.gti", referenceCallSource);
  expect(referenceCall.canGenerateCode() &&
             lang::verifyMirProgram(referenceCall.mir).valid(),
         "passing a replaced record's referent to a helper uses its current "
         "owner rather than retaining the previous value's dependency");
  auto referenceReturnSource = referenceCallSource;
  referenceReturnSource.replace(
      0, std::string_view("int read_reference").size(), "int& read_reference");
  const auto referenceReturn = lang::Frontend().analyze(
      "stored-carrier-replaced-reference-return.gti", referenceReturnSource);
  expect(referenceReturn.canGenerateCode() &&
             lang::verifyMirProgram(referenceReturn.mir).valid(),
         "a reference-returning helper preserves the current field referent");
  if (referenceReturn.canGenerateCode()) {
    auto forged = referenceReturn.mir;
    bool changed = false;
    for (const auto &candidate : forged.functionInstances()) {
      auto &body = const_cast<lang::MirFunctionInstance &>(candidate).body;
      for (auto &block : body.blocks)
        for (auto &call : block.instructions) {
          if (call.kind != lang::MirInstructionKind::Call ||
              call.borrowOrigin != lang::BorrowOriginKind::Argument ||
              !call.loan)
            continue;
          auto *loan = const_cast<lang::MirLoan *>(body.findLoan(*call.loan));
          if (!loan || loan->kind != lang::MirLoanKind::CallResult)
            continue;
          for (const auto &previous : body.loans) {
            if (previous.kind != lang::MirLoanKind::Stored || previous.entry ||
                previous.source == loan->source)
              continue;
            // A live, type-correct previous owner is nevertheless the wrong
            // result origin. Remove parent links so identity is the failing
            // proof, rather than a malformed parent/child relationship.
            loan->source = previous.source;
            loan->parent = 0;
            loan->storedDependency = 0;
            changed = true;
            break;
          }
        }
    }
    const auto verification = lang::verifyMirProgram(forged);
    expect(
        changed &&
            std::any_of(
                verification.errors.begin(), verification.errors.end(),
                [](const auto &error) {
                  return error.message.find(
                             "selected receiver or argument source identity") !=
                         std::string::npos;
                }),
        "independent staged-input flow rejects a result forged to the "
        "previous record owner");
  }
  auto invalidAssignmentSource = copyAssignmentSource;
  invalidAssignmentSource.replace(
      invalidAssignmentSource.find("first_owner = 3"),
      std::string_view("first_owner = 3").size(), "second_owner = 3");
  const auto invalidAssignment = lang::Frontend().analyze(
      "stored-carrier-copy-assignment-owner.gti", invalidAssignmentSource);
  expect(!invalidAssignment.canGenerateCode() &&
             std::any_of(invalidAssignment.diagnostics.begin(),
                         invalidAssignment.diagnostics.end(),
                         [](const auto &d) { return d.code == "GTI-S2017"; }),
         "whole-record shared replacement still protects its new referent");

  {
    const auto mixed = lang::Frontend().analyze(
        "mixed-reference-pack-contents.gti", R"gti(
import std.string;
class Reading {
public:
  int& value;
  Reading(int& source) : value(source) {}
};
int read(std::string_view& first, Reading& middle, std::string_view& last) {
  return first == "first" && middle.value == 7 && last == "last" ? 0 : 1;
}
int through_pack<Args...>(Args&... args) { return read(args...); }
int main() {
  mut std::string first{"first"};
  std::string last{"last"};
  int number = 7;
  Reading middle{number};
  std::string_view a = first.view();
  std::string_view b = last.view();
  return through_pack(a, middle, b);
}
)gti",
        {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
    expect(mixed.canGenerateCode() && lang::verifyMirProgram(mixed.mir).valid(),
           "reference pack lanes preserve text and user-record contents like "
           "ordinary reference formals");
    if (mixed.canGenerateCode()) {
      auto forged = mixed.mir;
      bool changed = false;
      for (const auto &candidate : forged.functionInstances()) {
        auto &body = const_cast<lang::MirFunctionInstance &>(candidate).body;
        for (const auto &candidateLoan : body.loans) {
          auto &loan = const_cast<lang::MirLoan &>(candidateLoan);
          const auto *source = body.findPlace(loan.source);
          if (loan.kind != lang::MirLoanKind::Parameter || !loan.entry ||
              !loan.storedDependency || !source ||
              source->projections.size() != 1 ||
              source->projections.front().kind !=
                  lang::MirProjectionKind::PackElement ||
              source->type.kind != lang::SemanticType::Reference ||
              source->type.arguments.size() != 1 ||
              source->type.arguments.front().kind !=
                  lang::SemanticType::StringView)
            continue;
          loan.storedDependency = 0;
          changed = true;
          break;
        }
        if (changed)
          break;
      }
      expect(changed && !lang::verifyMirProgram(forged).valid(),
             "a text reference pack lane cannot lose its independently "
             "verified external-content dependency");
      auto wrongLane = mixed.mir;
      bool switched = false;
      for (const auto &candidate : wrongLane.functionInstances()) {
        auto &body = const_cast<lang::MirFunctionInstance &>(candidate).body;
        lang::MirLoan *first = nullptr;
        for (const auto &candidateLoan : body.loans) {
          auto &loan = const_cast<lang::MirLoan &>(candidateLoan);
          const auto *source = body.findPlace(loan.source);
          if (loan.kind != lang::MirLoanKind::Parameter || !loan.entry ||
              !loan.storedDependency || !source ||
              source->projections.size() != 1 ||
              source->projections.front().kind !=
                  lang::MirProjectionKind::PackElement ||
              source->type.kind != lang::SemanticType::Reference ||
              source->type.arguments.size() != 1 ||
              source->type.arguments.front().kind !=
                  lang::SemanticType::StringView)
            continue;
          if (!first) {
            first = &loan;
          } else {
            first->storedDependency = loan.storedDependency;
            switched = true;
            break;
          }
        }
        if (switched)
          break;
      }
      expect(switched && !lang::verifyMirProgram(wrongLane).valid(),
             "distinct text reference lanes cannot exchange their content "
             "authorities even when their types match");
    }
  }
}
} // namespace

int main() {
  carrier_flow_tests::run();
  replacedRecordAndPackProofs();
  rawAccesses();
  persistentStorageAuthority();
  return failures == 0 ? 0 : 1;
}
