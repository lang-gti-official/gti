#include "cpp_mir_body_emitter_test_fixture.h"

namespace {

// expected<T, E> and unexpected(e)'s std::detail::unexpected_value<E> are the
// prelude's ordinary source classes.
bool isClassNamed(const lang::FrontendResult &frontend,
                  const lang::SemanticType &type, std::string_view name) {
  const auto *info = type.kind == lang::SemanticType::Class
                         ? frontend.semantics.findClassType(type.classId)
                         : nullptr;
  return info != nullptr && info->declaration != nullptr &&
         info->declaration->name().lexeme == name;
}

bool isExpectedClass(const lang::FrontendResult &frontend,
                     const lang::SemanticType &type) {
  return isClassNamed(frontend, type, "expected");
}

const lang::MirInstruction *definition(const lang::MirBody &body,
                                       const lang::MirOperand &operand) {
  const auto *value = operand.kind == lang::MirOperandKind::Value
                          ? body.findValue(operand.value)
                          : nullptr;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions)
      if (value != nullptr && instruction.id == value->definition)
        return &instruction;
  return nullptr;
}

} // namespace

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
        isExpectedClass(frontend, function.returnType)) {
      const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                         function.id};
      const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
      if (emitter.supportsFailureBodyText(address)) {
        const auto text = emitter.emitFailureBodyText(address, "test", 0);
        for (const auto &drop : function.body.dropObligations) {
          if (drop.kind != lang::MirDropObligationKind::PreparedParameter ||
              drop.dropType.type != function.returnType.arguments.front())
            continue;
          // `?load(fail)` stages the extracted success payload into the
          // prepared parameter of the prelude's expected(T) constructor,
          // which constructs the caller's result in place; take() retires
          // that exact slot.
          const std::string result =
              "::new (static_cast<void *>(__gti_mir_out_result))";
          const std::string slot = "std::move(__gti_mir_p_" +
                                   std::to_string(drop.place) + ".take())";
          bool published = false;
          for (auto at = text.text.find(result); at != std::string::npos;
               at = text.text.find(result, at + 1))
            published =
                published || text.text.substr(at, text.text.find('\n', at) - at)
                                     .find(slot) != std::string::npos;
          expect(
              text.emitted() && published,
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
            !isExpectedClass(frontend, constructor.operands.front().type))
          continue;
        // The owning call result is staged into the constructor's prepared
        // parameter, which the constructor then consumes.
        const auto *staging =
            definition(function.body, constructor.operands.front());
        const lang::MirInstruction *producer =
            staging != nullptr &&
                    staging->kind == lang::MirInstructionKind::CallInput &&
                    staging->operands.size() == 1
                ? definition(function.body, staging->operands.front())
                : nullptr;
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
          // Forged constructor parameter types and pre-live result storage
          // on the generic class construction are left to body analysis.
          const bool verified = lang::verifyMirProgram(forged).valid();
          const lang::CppMirBodyEmitter rejected(forged, rows);
          const bool admitted = rejected.analyze(address).ready() &&
                                rejected.supportsFailureBodyText(address);
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
            !isExpectedClass(frontend, instruction.operands.front().type))
          continue;
        const auto *place = function.body.findPlace(*instruction.destination);
        if (!place || place->root != lang::MirPlaceRootKind::Binding)
          continue;
        // unexpected(make(...)) stages the owned call result through the
        // prelude's unexpected() into the expected(unexpected_value<E>)
        // constructor, whose result the binding publishes.
        const auto *wrapper =
            definition(function.body, instruction.operands.front());
        const auto *staging =
            wrapper != nullptr &&
                    wrapper->kind == lang::MirInstructionKind::Construct &&
                    wrapper->operands.size() == 1 &&
                    isClassNamed(frontend, wrapper->operands.front().type,
                                 "unexpected_value")
                ? definition(function.body, wrapper->operands.front())
                : nullptr;
        if (!staging || staging->kind != lang::MirInstructionKind::CallInput ||
            !staging->destination)
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
                     text.text.find("::new (static_cast<void *>(__gti_mir_p_" +
                                    std::to_string(place->id) +
                                    ".construction_address()))") !=
                         std::string::npos &&
                     text.text.find("std::move(__gti_mir_p_" +
                                    std::to_string(*staging->destination) +
                                    ".take())") != std::string::npos,
                 "the initialized binding consumes the already-owned wrapper "
                 "without replaying its payload-producing call");
        }
        for (int mutation = 0; mutation != 6; ++mutation) {
          auto forged = frontend.mir;
          auto *body = lang::findMirBody(forged, address);
          for (auto &changedBlock : body->blocks) {
            for (auto &changed : changedBlock.instructions) {
              if (changed.id == wrapper->id && mutation == 4)
                changed.operands.front().type.arguments.front() =
                    lang::SemanticType::Int32;
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
              else if (mutation == 5)
                changed.operands.push_back(changed.operands.front());
            }
          }
          // The generic class publication vocabulary leaves lifecycle and
          // staged payload-identity forgeries to body analysis.
          const lang::CppMirBodyEmitter rejected(forged, rows);
          expect(!(rejected.analyze(address).ready() &&
                   rejected.supportsFailureBodyText(address)),
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
        if (!isExpectedClass(frontend, field.type))
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
        // The source expected's empty shell is its payload-enum `state`
        // field's private inactive alternative, which constructs no error
        // payload; the cleanup guard and structural transport are the enum's.
        for (const auto &wrapper : frontend.mir.classInstances()) {
          if (wrapper.type != field.type || wrapper.declaredFields.size() != 1)
            continue;
          const auto &state = wrapper.declaredFields.front().type;
          expect(frontend.mir.findPayloadEnum(state) != nullptr,
                 "the expected empty shell is its payload-enum state");
          for (const bool loseGuard : {false, true}) {
            auto forged = frontend.mir;
            auto *changed = const_cast<lang::MirPayloadEnum *>(
                forged.findPayloadEnum(state));
            if (changed == nullptr)
              continue;
            if (loseGuard)
              changed->requiresActiveCleanup = false;
            else
              changed->traits.movable = false;
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
            !isExpectedClass(frontend, consumer.operands[1].type))
          continue;
        // produce()'s owning result is staged into the constructor's
        // prepared parameter as soon as it succeeds; later arguments'
        // failures retire that prepared parameter.
        const auto *staging = definition(function.body, consumer.operands[1]);
        const lang::MirInstruction *producer =
            staging != nullptr &&
                    staging->kind == lang::MirInstructionKind::CallInput &&
                    staging->operands.size() == 1
                ? definition(function.body, staging->operands.front())
                : nullptr;
        expect(producer && producer->successResultDrop &&
                   staging->preparedParameterDrop &&
                   staging->lifecycle.size() == 1 &&
                   staging->lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Reparent &&
                   staging->lifecycle.front().source ==
                       *producer->successResultDrop &&
                   staging->lifecycle.front().target ==
                       *staging->preparedParameterDrop,
               "Expected argument producer retains its success-only drop");
        if (!producer || !producer->successResultDrop ||
            !staging->preparedParameterDrop)
          continue;
        const auto dropId = *staging->preparedParameterDrop;
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
          // A forged parameter type on the source class construction is
          // left to body analysis by the generic construction vocabulary.
          const lang::CppMirBodyEmitter rejected(forged, rows);
          expect(!lang::verifyMirProgram(forged).valid() &&
                     !(rejected.analyze(address).ready() &&
                       rejected.supportsFailureBodyText(address)),
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
  const lang::SemanticType *fixtureError = nullptr;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *hir = frontend.hir.findFunctionInstance(function.id);
    if (hir && hir->source && hir->source->name().lexeme == "make" &&
        isExpectedClass(frontend, function.returnType) &&
        function.returnType.arguments.size() == 2)
      fixtureError = &function.returnType.arguments.back();
  }
  expect(fixtureError != nullptr,
         "fixture retains its exact owning error type");
  if (fixtureError == nullptr)
    return 1;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &selected : function.body.dropObligations) {
      const bool expected = isExpectedClass(frontend, selected.dropType.type);
      const bool unexpected =
          isClassNamed(frontend, selected.dropType.type, "unexpected_value");
      if ((!expected && !unexpected) || (expected && expectedRecipe) ||
          (unexpected && unexpectedRecipe))
        continue;
      // Other library factories may return expected<Owner, TrivialError>.
      // The mutation oracle specifically targets this fixture's owning Error.
      if (selected.dropType.type.arguments.empty() ||
          selected.dropType.type.arguments.back() != *fixtureError)
        continue;
      expectedRecipe |= expected;
      unexpectedRecipe |= unexpected;
      // expected<T, E> keeps its alternatives in the payload-enum `state`
      // field, whose error alternative is variant 1; unexpected_value<E>
      // holds the error as its only field.
      const auto errorRecipe = [expected](auto &type) -> decltype(&type) {
        if (!expected)
          return type.aggregate.size() == 1 ? &type.aggregate.back() : nullptr;
        if (type.aggregate.size() != 1 ||
            type.aggregate.front().aggregate.empty())
          return nullptr;
        auto &alternative = type.aggregate.front().aggregate.back();
        return alternative.payloadVariant == 1U ? &alternative : nullptr;
      };
      const auto *error = errorRecipe(selected.dropType);
      expect(error != nullptr && error->requiresActiveCleanup &&
                 error->destructor,
             "alternative recipe retains its exact active error destructor");
      if (error == nullptr)
        continue;
      for (int mutation = 0; mutation != 4; ++mutation) {
        auto malformed = frontend.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        for (auto &drop : changed->body.dropObligations) {
          if (drop.dropType.type != selected.dropType.type)
            continue;
          auto *alternative = errorRecipe(drop.dropType);
          if (mutation == 0)
            drop.dropType.aggregate.clear();
          if (mutation == 1 && alternative)
            alternative->requiresActiveCleanup = false;
          if (mutation == 2 && alternative)
            alternative->destructor.reset();
          if (mutation == 3 && alternative)
            alternative->type = lang::SemanticType::Int32;
        }
        expect(!lang::verifyMirProgram(malformed).valid(),
               "independent MIR recipe derivation rejects missing or forged "
               "alternative cleanup");
        // unexpected_value<E> stages through ordinary prepared class
        // parameters, whose recipe forgeries body analysis rejects.
        const lang::CppMirBodyEmitter rejected(malformed, rows);
        const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                           function.id};
        expect(!(rejected.analyze(address).ready() &&
                 rejected.supportsFailureBodyText(address)),
               "backend cannot erase or change active alternative cleanup");
      }
    }
    // unexpected(Error(...)) is wrapped by the prelude's
    // expected(unexpected_value<E>) constructor before the fallible Guard
    // cleanup, so the pending return identity is the returned expected,
    // constructed in the unpublished result storage.
    const auto *source = frontend.hir.findFunctionInstance(function.id);
    if (source == nullptr ||
        source->sourceUnit != frontend.sourceGraph.entryUnit())
      continue;
    for (const auto &pending : function.body.dropObligations) {
      if (pending.kind != lang::MirDropObligationKind::ReturnResult ||
          !isExpectedClass(frontend, pending.dropType.type) ||
          pending.dropType.type.arguments.empty() ||
          pending.dropType.type.arguments.back() != *fixtureError)
        continue;
      ++pendingReturns;
      const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                         function.id};
      const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
      expect(emitter.supportsFailureBodyText(address),
             "pending error result supports its exact cleanup-only "
             "continuation");
      if (emitter.supportsFailureBodyText(address)) {
        const auto text = emitter.emitFailureBodyText(address, "test", 0);
        expect(text.emitted() &&
                   text.text.find("std::destroy_at(__gti_mir_out_result); "
                                  "// GTI MIR unpublished return cleanup") !=
                       std::string::npos &&
                   text.text.find("__gti_mir_out_result->value()") ==
                       std::string::npos,
               "rollback destroys the unpublished in-place error result "
               "without reading its success alternative");
      }
      expect(pending.dropType.type == function.returnType,
             "pending error result carries the exact returned type");
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
        expect(!result.valid(),
               "pending error result rejects malformed lifetime or error "
               "identity");
        auto malformedProgram = frontend.mir;
        const_cast<lang::MirFunctionInstance *>(
            malformedProgram.findFunctionInstance(function.id))
            ->body = malformed;
        // The in-place expected result's lifecycle forgeries are rejected
        // by body analysis rather than the generic class vocabulary.
        const lang::CppMirBodyEmitter rejected(malformedProgram, rows);
        expect(!(rejected.analyze(address).ready() &&
                 rejected.supportsFailureBodyText(address)),
               "emission rejects altered pending error result ownership " +
                   std::to_string(mutation));
        if (mutation == 3)
          expect(std::any_of(result.errors.begin(), result.errors.end(),
                             [](const auto &error) {
                               return error.message.find(
                                          "return ownership transfers before "
                                          "fallible cleanup") !=
                                      std::string::npos;
                             }),
                 "the error result cannot escape ownership before cleanup "
                 "succeeds");
      }
    }
  }
  expect(pendingReturns == 1,
         "fixture retains one pending error-result rollback lifetime");
  expect(
      expectedRecipe && unexpectedRecipe,
      "fixture covers both completed Expected and pending Unexpected recipes");
  return failures ? 1 : 0;
}
