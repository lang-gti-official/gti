#include "cpp_mir_body_emitter_test_fixture.h"

void testOwningAggregateArgumentStaging() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/optional_expected_composition.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(input.good() && frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "owning aggregate arguments retain their exact publication schedule");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  std::size_t checked = 0;
  std::size_t retiredReturnTemporaries = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *hir = frontend.hir.findFunctionInstance(function.id);
    if (hir && hir->source && hir->source->name().lexeme == "forward" &&
        function.returnType.kind == lang::SemanticType::Expected) {
      const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                         function.id};
      const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
      if (emitter.supportsFailureBodyText(address)) {
        const auto text = emitter.emitFailureBodyText(address, "test", 0);
        for (const auto &drop : function.body.dropObligations) {
          if (drop.kind != lang::MirDropObligationKind::ReturnResult ||
              drop.dropType.type != function.returnType.arguments.front())
            continue;
          // A potentially failing source destructor keeps the payload pending
          // across earlier cleanup, then take() retires its exact source slot.
          // It no longer uses the old passive-temporary destroy comment.
          const auto publication =
              "construct_representation_at(__gti_mir_out_result, __gti_mir_p_" +
              std::to_string(drop.place) + ".take());";
          expect(
              text.emitted() &&
                  text.text.find(publication) != std::string::npos,
              "successful return transfers and retires its exact pending slot");
          auto forged = frontend.mir;
          auto *body = lang::findMirBody(forged, address);
          std::size_t erased = 0;
          for (auto &block : body->blocks)
            for (auto &instruction : block.instructions)
              erased +=
                  std::erase_if(instruction.lifecycle, [&](const auto &event) {
                    return event.kind ==
                               lang::MirLifecycleEventKind::TransferOut &&
                           event.source == drop.id;
                  });
          expect(
              erased == 1 && !lang::verifyMirProgram(forged).valid(),
              "pending success publication cannot lose its exact TransferOut");
          ++retiredReturnTemporaries;
        }
      }
    }
    if (function.entryKind == lang::ProgramEntryKind::None)
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    expect(lang::CppMirBodyEmitter(frontend.mir, rows)
               .supportsFailureBodyText(address),
           "a constructor can consume an exact owning aggregate call result "
           "before its own result becomes another prepared argument");
    for (const auto &block : function.body.blocks) {
      for (const auto &constructor : block.instructions) {
        if (constructor.kind != lang::MirInstructionKind::Construct ||
            !constructor.constructorTarget || !constructor.successResultDrop ||
            constructor.operands.size() != 1 ||
            constructor.operands.front().kind != lang::MirOperandKind::Value ||
            constructor.operands.front().type.kind !=
                lang::SemanticType::Expected)
          continue;
        const auto *value =
            function.body.findValue(constructor.operands.front().value);
        if (value == nullptr)
          continue;
        const lang::MirInstruction *producer = nullptr;
        for (const auto &producerBlock : function.body.blocks)
          for (const auto &instruction : producerBlock.instructions)
            if (instruction.id == value->definition)
              producer = &instruction;
        if (producer == nullptr ||
            producer->kind != lang::MirInstructionKind::Call ||
            !producer->functionTarget || !producer->successResultDrop)
          continue;
        ++checked;
        for (int mutation = 0; mutation != 6; ++mutation) {
          auto forged = frontend.mir;
          auto *body = lang::findMirBody(forged, address);
          for (auto &changedBlock : body->blocks) {
            for (auto &changed : changedBlock.instructions) {
              if (changed.id == producer->id && mutation == 0)
                changed.successResultDrop.reset();
              if (changed.id == constructor.id && mutation == 1)
                changed.lifecycle.front().source =
                    *constructor.successResultDrop;
              if (changed.id == constructor.id && mutation == 4)
                changed.parameterTypes.front() = lang::SemanticType::Bool;
            }
            if (changedBlock.terminator.invokeInstruction == producer->id &&
                mutation == 2)
              changedBlock.terminator.successLifecycle.front().conditional =
                  true;
          }
          if (mutation == 3)
            body->dropObligations[*producer->successResultDrop - 1]
                .dropType.type = lang::SemanticType::Bool;
          if (mutation == 5) {
            const auto &drop =
                body->dropObligations[*producer->successResultDrop - 1];
            body->places[drop.place - 1].initiallyAvailable = true;
          }
          const bool verified = lang::verifyMirProgram(forged).valid();
          const bool admitted = lang::CppMirBodyEmitter(forged, rows)
                                    .supportsFailureBodyText(address);
          expect(!verified && !admitted,
                 "direct aggregate argument staging rejects forged activation, "
                 "type identity, and ownership transfer");
        }
      }
    }
  }
  expect(checked == 2,
         "both success/error Snapshot argument paths are checked");
  expect(retiredReturnTemporaries == 1,
         "propagating an owned success payload retires its moved-from "
         "synthetic temporary at the exact return transfer");
}

void testOwnedUnexpectedBinding() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/owned_unexpected_binding.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(input.good() && frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "an owned call result can publish through Unexpected into a binding");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  bool found = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            !instruction.destination || instruction.operands.size() != 1 ||
            instruction.operands.front().type.kind !=
                lang::SemanticType::Unexpected)
          continue;
        const auto *place = function.body.findPlace(*instruction.destination);
        if (!place || place->root != lang::MirPlaceRootKind::Binding)
          continue;
        found = true;
        const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                           function.id};
        const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
        expect(emitter.supportsFailureBodyText(address),
               "binding publication preserves completed call/wrapper staging");
        if (emitter.supportsFailureBodyText(address)) {
          const auto text = emitter.emitFailureBodyText(address, "test", 0);
          expect(text.emitted() &&
                     text.text.find(".take())") != std::string::npos &&
                     text.text.find("// transferred error representation") !=
                         std::string::npos,
                 "the initialized binding consumes the already-owned wrapper "
                 "without replaying its payload-producing call");
        }
        for (int mutation = 0; mutation != 6; ++mutation) {
          auto forged = frontend.mir;
          auto *body = lang::findMirBody(forged, address);
          for (auto &changedBlock : body->blocks) {
            for (auto &changed : changedBlock.instructions) {
              if (changed.id != instruction.id)
                continue;
              if (mutation == 0)
                changed.lifecycle.clear();
              else if (mutation == 1)
                changed.lifecycle.front().conditional = true;
              else if (mutation == 2)
                changed.lifecycle.front().failureCleanup = true;
              else if (mutation == 3)
                changed.lifecycle.back().target =
                    changed.lifecycle.front().source;
              else if (mutation == 4)
                changed.operands.front().type.arguments.front() =
                    lang::SemanticType::Int32;
              else
                changed.operands.push_back(changed.operands.front());
            }
          }
          expect(!lang::CppMirBodyEmitter(forged, rows)
                      .supportsFailureBodyText(address),
                 "binding publication rejects missing/conditional/wrong "
                 "ownership transfers and malformed consumer shape");
        }
        for (std::size_t alternative = 0; alternative != 2; ++alternative) {
          auto forged = frontend.mir;
          auto *body = lang::findMirBody(forged, address);
          body->returnType.arguments[alternative] = lang::SemanticType::Bool;
          const bool verified = lang::verifyMirProgram(forged).valid();
          const bool admitted = lang::CppMirBodyEmitter(forged, rows)
                                    .supportsFailureBodyText(address);
          expect(!verified && !admitted,
                 "error-only owning Expected returns reject a forged success "
                 "or error identity instead of treating alternatives alike");
        }
      }
    }
  }
  expect(found, "the regression exercises a named Expected error binding");
}

void testExpectedFieldConstructionRollback() {
  std::size_t fields = 0;
  for (const auto name : {"owning_aggregate_constructor_argument.gti",
                          "expected_field_failure.gti"}) {
    const auto path =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
    std::ifstream input(path);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(input.good() && frontend.canGenerateCode() &&
               lang::verifyMirProgram(frontend.mir).valid(),
           "fallible constructors retain exact owned alternative lifetimes");
    if (!frontend.canGenerateCode())
      continue;
    const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
    expect(lang::CppMirBodyEmitter(frontend.mir, rows).analyzeProgram().ready(),
           "owning Expected arguments publish into nested and binding results");
    for (const auto &owner : frontend.mir.classInstances()) {
      for (const auto &field : owner.declaredFields) {
        if (field.type.kind != lang::SemanticType::Expected)
          continue;
        ++fields;
        expect(lang::cppMirFailureConstructorEmptyStateTypeEligible(
                   frontend.mir, field.type),
               "unpublished Expected fields use a proved empty error state");
        auto invalid = field.type;
        invalid.arguments.pop_back();
        expect(
            !lang::cppMirFailureConstructorEmptyStateTypeEligible(frontend.mir,
                                                                  invalid),
            "empty Expected representation requires both exact alternatives");
        invalid = field.type;
        invalid.arguments.back() = lang::SemanticType::Void;
        expect(!lang::cppMirFailureConstructorEmptyStateTypeEligible(
                   frontend.mir, invalid),
               "a missing error payload cannot establish an empty shell");
        for (const auto &error : frontend.mir.classInstances()) {
          if (error.type != field.type.arguments.back())
            continue;
          for (const bool loseGuard : {false, true}) {
            auto forged = frontend.mir;
            auto *changed = const_cast<lang::MirClassInstance *>(
                forged.findClassInstance(error.id));
            if (loseGuard)
              changed->requiresActiveDropState = false;
            else
              changed->moveConstructor = lang::SpecialMemberStatus::Deleted;
            expect(!lang::cppMirFailureConstructorEmptyStateTypeEligible(
                       forged, field.type),
                   "an owning empty error requires disarmed cleanup and safe "
                   "structural transport");
          }
        }
        for (const auto &constructor : frontend.mir.constructorInstances()) {
          if (constructor.owner != owner.id ||
              !constructor.mayRaiseDefinedFailure)
            continue;
          expect(lang::CppMirBodyEmitter(frontend.mir, rows)
                     .supportsFailureBodyText(
                         {lang::MirBodyKind::Constructor, constructor.id}),
                 "Expected field constructors retain status-ABI rollback");
        }
      }
    }
  }
  expect(fields >= 2, "both passive and owning error fields are exercised");
}

void testExpectedConstructorArguments() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/expected_constructor_arguments.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(input.good() && frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "an owning result between earlier and later arguments has verified "
         "success publication and rollback");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  expect(lang::CppMirBodyEmitter(frontend.mir, rows).analyzeProgram().ready(),
         "owning Expected arguments survive later conditional and fallible "
         "argument evaluation");
  std::size_t checked = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None)
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    for (const auto &block : function.body.blocks) {
      for (const auto &consumer : block.instructions) {
        if (consumer.kind != lang::MirInstructionKind::Construct ||
            consumer.operands.size() != 3 ||
            consumer.operands[1].type.kind != lang::SemanticType::Expected)
          continue;
        const auto *value = function.body.findValue(consumer.operands[1].value);
        const lang::MirInstruction *producer = nullptr;
        for (const auto &producerBlock : function.body.blocks)
          for (const auto &candidate : producerBlock.instructions)
            if (value && candidate.id == value->definition)
              producer = &candidate;
        expect(producer && producer->successResultDrop,
               "Expected argument producer retains its success-only drop");
        if (!producer || !producer->successResultDrop)
          continue;
        const auto dropId = *producer->successResultDrop;
        const auto *drop = function.body.findDropObligation(dropId);
        // Count the failures whose cleanup path destroys the pending
        // argument; the branches may share one copy of that cleanup.
        std::size_t cleanups = 0;
        for (const auto &record : function.body.failureRecords) {
          if (!drop || record.kind != lang::MirFailureRecordKind::Producer)
            continue;
          const lang::MirBlock *cursor =
              function.body.findBlock(record.parameterBlock);
          for (std::size_t steps = 0;
               cursor != nullptr && steps <= function.body.blocks.size();
               ++steps) {
            if (std::any_of(
                    cursor->instructions.begin(), cursor->instructions.end(),
                    [&](const lang::MirInstruction &cleanup) {
                      return cleanup.kind == lang::MirInstructionKind::Drop &&
                             cleanup.destination == drop->place;
                    })) {
              ++cleanups;
              break;
            }
            const auto &terminator = cursor->terminator;
            if (terminator.kind != lang::MirTerminatorKind::Goto &&
                terminator.kind != lang::MirTerminatorKind::Invoke &&
                terminator.kind != lang::MirTerminatorKind::ContinueFailure)
              break;
            cursor = function.body.findBlock(terminator.target);
          }
        }
        expect(cleanups >= 2,
               "both later-argument branches run the pending argument's "
               "failure cleanup");
        for (int mutation = 0; mutation != 4; ++mutation) {
          auto forged = frontend.mir;
          auto *body = lang::findMirBody(forged, address);
          for (auto &changedBlock : body->blocks) {
            for (auto &instruction : changedBlock.instructions) {
              if (drop && instruction.kind == lang::MirInstructionKind::Drop &&
                  instruction.destination == drop->place) {
                if (mutation == 0)
                  instruction.lifecycle.clear();
                if (mutation == 1)
                  instruction.lifecycle.front().failureCleanup = false;
              }
              if (instruction.id == consumer.id) {
                if (mutation == 2)
                  instruction.parameterTypes[1] = lang::SemanticType::Bool;
                if (mutation == 3)
                  instruction.lifecycle.front().conditional = true;
              }
            }
          }
          expect(!lang::verifyMirProgram(forged).valid() &&
                     !lang::CppMirBodyEmitter(forged, rows)
                          .supportsFailureBodyText(address),
                 "constructor arguments cannot lose later-failure cleanup, "
                 "parameter identity or unconditional transfer");
        }
        ++checked;
      }
    }
  }
  expect(checked == 1, "the middle owning argument is independently checked");
}

int main() {
  testExpectedConstructorArguments();
  testExpectedFieldConstructionRollback();
  testOwningAggregateArgumentStaging();
  testOwnedUnexpectedBinding();
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/owned_unexpected_pending_cleanup.gti";
  std::ifstream input(path);
  expect(input.good(), "pending Unexpected fixture exists");
  const std::string source{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "Unexpected stays locally owned through fallible return cleanup");
  if (!frontend.canGenerateCode())
    return 1;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  std::size_t pendingReturns = 0;
  bool expectedRecipe = false;
  bool unexpectedRecipe = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &selected : function.body.dropObligations) {
      const bool expected =
          selected.dropType.type.kind == lang::SemanticType::Expected;
      const bool unexpected =
          selected.dropType.type.kind == lang::SemanticType::Unexpected;
      if ((!expected && !unexpected) || (expected && expectedRecipe) ||
          (unexpected && unexpectedRecipe))
        continue;
      expectedRecipe |= expected;
      unexpectedRecipe |= unexpected;
      expect(selected.dropType.aggregate.size() == (expected ? 2U : 1U) &&
                 selected.dropType.aggregate.back().requiresActiveCleanup &&
                 selected.dropType.aggregate.back().destructor,
             "alternative recipe retains its exact active error destructor");
      if (selected.dropType.aggregate.empty())
        continue;
      for (int mutation = 0; mutation != 4; ++mutation) {
        auto malformed = frontend.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        for (auto &drop : changed->body.dropObligations) {
          if (drop.dropType.type != selected.dropType.type)
            continue;
          if (mutation == 0)
            drop.dropType.aggregate.clear();
          if (mutation == 1)
            drop.dropType.aggregate.back().requiresActiveCleanup = false;
          if (mutation == 2)
            drop.dropType.aggregate.back().destructor.reset();
          if (mutation == 3)
            drop.dropType.aggregate.back().type = lang::SemanticType::Int32;
        }
        expect(!lang::verifyMirProgram(malformed).valid(),
               "independent MIR recipe derivation rejects missing or forged "
               "alternative cleanup");
        expect(!lang::CppMirBodyEmitter(malformed, rows)
                    .supportsFailureBodyText(
                        {lang::MirBodyKind::Function, function.id}),
               "backend cannot erase or change active alternative cleanup");
      }
    }
    for (const auto &pending : function.body.dropObligations) {
      if (pending.kind != lang::MirDropObligationKind::ReturnResult ||
          pending.dropType.type.kind != lang::SemanticType::Unexpected)
        continue;
      ++pendingReturns;
      const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                         function.id};
      const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
      expect(emitter.supportsFailureBodyText(address),
             "pending Unexpected supports its exact cleanup-only continuation");
      if (emitter.supportsFailureBodyText(address)) {
        const auto text = emitter.emitFailureBodyText(address, "test", 0);
        expect(
            text.emitted() &&
                text.text.find("// GTI MIR unpublished Unexpected cleanup") !=
                    std::string::npos &&
                text.text.find("__gti_mir_out_result->value()") ==
                    std::string::npos,
            "rollback destroys local Unexpected rather than unpublished "
            "Expected output");
      }
      expect(pending.dropType.type.arguments.size() == 1 &&
                 function.returnType.kind == lang::SemanticType::Expected &&
                 pending.dropType.type.arguments.front() ==
                     function.returnType.arguments.back(),
             "pending Unexpected carries the exact returned error type");
      for (int mutation = 0; mutation != 6; ++mutation) {
        auto malformed = function.body;
        if (mutation == 0)
          malformed.returnType.arguments.back() = lang::SemanticType::Int32;
        for (auto &drop : malformed.dropObligations)
          if (drop.id == pending.id && mutation == 1)
            drop.initiallyActive = true;
        for (auto &block : malformed.blocks)
          for (auto &instruction : block.instructions)
            for (auto &event : instruction.lifecycle) {
              if (event.kind == lang::MirLifecycleEventKind::Reparent &&
                  event.target == pending.id) {
                if (mutation == 2)
                  event.conditional = true;
                if (mutation == 3) {
                  event.kind = lang::MirLifecycleEventKind::TransferOut;
                  event.target = 0;
                }
              }
              if (event.kind == lang::MirLifecycleEventKind::TransferOut &&
                  event.source == pending.id) {
                if (mutation == 4)
                  event.failureCleanup = true;
                if (mutation == 5)
                  event.conditional = true;
              }
            }
        const auto result = lang::verifyMirBody(malformed, function.id);
        expect(
            !result.valid(),
            "pending Unexpected rejects malformed lifetime or error identity");
        auto malformedProgram = frontend.mir;
        const_cast<lang::MirFunctionInstance *>(
            malformedProgram.findFunctionInstance(function.id))
            ->body = malformed;
        expect(!lang::CppMirBodyEmitter(malformedProgram, rows)
                    .supportsFailureBodyText(address),
               "emission rejects altered pending Unexpected ownership");
        if (mutation == 3)
          expect(std::any_of(result.errors.begin(), result.errors.end(),
                             [](const auto &error) {
                               return error.message.find(
                                          "return ownership transfers before "
                                          "fallible cleanup") !=
                                      std::string::npos;
                             }),
                 "Unexpected cannot escape ownership before cleanup succeeds");
      }
    }
  }
  expect(pendingReturns == 1,
         "fixture retains one pending Unexpected rollback lifetime");
  expect(
      expectedRecipe && unexpectedRecipe,
      "fixture covers both completed Expected and pending Unexpected recipes");
  return failures ? 1 : 0;
}
