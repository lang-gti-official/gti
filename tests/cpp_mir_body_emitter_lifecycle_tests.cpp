#include "../src/compiler/cpp_representation.h"
#include "cpp_mir_body_emitter_test_fixture.h"

namespace {

void testSelectedConstructorDefaultSchedule() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/nominal_field_defaults.gti";
  std::ifstream input(fixture);
  expect(input.good(), "selected-default fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "selected-default fixture must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  bool checked = false;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *owner = frontend.mir.findClassInstance(constructor.owner);
    const auto *info =
        owner == nullptr ? nullptr
                         : frontend.semantics.findClassType(owner->declaration);
    if (!info || !info->declaration ||
        info->declaration->name().lexeme != "ReferenceOverride" ||
        owner->declaredFields.size() != 3 ||
        owner->declaredFields.front().type.kind !=
            lang::SemanticType::Reference)
      continue;
    checked = true;
    const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                       constructor.id};
    expect(lang::CppMirBodyEmitter(frontend.mir, map)
               .supportsFailureBodyText(address),
           "reference, explicitly overridden class, and scalar default use "
           "the selected constructor's status schedule");
    expect(constructor.initializers.size() == 3,
           "each declared field must have one selected initializer");
    if (constructor.initializers.size() != 3)
      continue;
    for (int mutation = 0; mutation != 12; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      lang::MirInstruction *publication = nullptr;
      lang::MirInstruction *end = nullptr;
      for (auto &block : changed->body.blocks) {
        for (auto &instruction : block.instructions) {
          if (instruction.kind == lang::MirInstructionKind::Initialize &&
              instruction.constructorInitializer == 2)
            publication = &instruction;
          if (instruction.fullExpressionEnd == 2)
            end = &instruction;
        }
      }
      expect(publication && end,
             "class publication and full-expression endpoint must exist");
      if (!publication || !end)
        continue;
      if (mutation == 0)
        changed->initializers.pop_back();
      if (mutation == 1)
        changed->initializers[1].field = changed->initializers[0].field;
      if (mutation == 2)
        std::swap(changed->initializers[1], changed->initializers[2]);
      if (mutation == 3)
        changed->initializers[1].targetType = lang::SemanticType::Int32;
      if (mutation == 4)
        publication->constructorInitializer = 0;
      if (mutation == 5) {
        auto *place = const_cast<lang::MirPlace *>(
            changed->body.findPlace(*publication->destination));
        place->projections.front().field = changed->initializers[0].field;
      }
      if (mutation == 6)
        publication->operands.front().value = 0;
      if (mutation == 7)
        publication->hirValue = changed->initializers[2].arguments.front();
      if (mutation == 8)
        end->fullExpressionEnd = 0;
      if (mutation == 9)
        changed->body.fullExpressions[1].roots.front() = 0;
      if (mutation == 10)
        changed->initializers[0].arguments = changed->initializers[1].arguments;
      if (mutation == 11) {
        for (auto &loan : changed->body.loans)
          if (loan.storedField == changed->initializers[0].field)
            loan.source = 0;
      }
      expect(!lang::CppMirBodyEmitter(malformed, map)
                  .supportsFailureBodyText(address),
             "selected schedule rejects missing/duplicate fields, order, "
             "type, publication, endpoint and reference-origin forgery " +
                 std::to_string(mutation));
    }
  }
  expect(checked, "fixture must reach the mixed selected-field schedule");
  bool checkedNullable = false;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *owner = frontend.mir.findClassInstance(constructor.owner);
    const auto *info =
        owner == nullptr ? nullptr
                         : frontend.semantics.findClassType(owner->declaration);
    if (!info || !info->declaration ||
        info->declaration->name().lexeme != "BorrowedOwner")
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                       constructor.id};
    expect(lang::CppMirBodyEmitter(frontend.mir, map)
               .supportsFailureBodyText(address),
           "nullable pointer publication follows the selected owning and "
           "reference fields");
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Compute ||
            instruction.operation != lang::MirOperation::Literal ||
            instruction.info.type != lang::SemanticType::NullPtr)
          continue;
        checkedNullable = true;
        auto malformed = frontend.mir;
        auto *changed = const_cast<lang::MirConstructorInstance *>(
            malformed.findConstructorInstance(constructor.id));
        for (auto &changedBlock : changed->body.blocks)
          for (auto &producer : changedBlock.instructions)
            if (producer.id == instruction.id)
              producer.literal = std::uint64_t{1};
        expect(!lang::CppMirBodyEmitter(malformed, map)
                    .supportsFailureBodyText(address),
               "nullable publication rejects a non-null literal masquerading "
               "as nullptr");
      }
    }
  }
  expect(checkedNullable, "fixture must reach nullable pointer construction");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "selected defaults must reach production emission");
}

const lang::MirInstruction *testInstruction(const lang::MirBody &body,
                                            lang::MirInstructionId id);

void testCheckedReferenceExpectedAssignment() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/reference_expected_assignment.gti";
  std::ifstream input(fixture);
  expect(input.good(), "Expected replacement fixture exists");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "Expected replacement verifies");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  expect(lang::CppMirBodyEmitter(frontend.mir, rows).analyzeProgram().ready(),
         "owning and passive Expected results replace checked referents");
  bool passive = false, owning = false, observableDefault = false;
  for (const auto &function : frontend.mir.functionInstances())
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *destination =
            instruction.destination
                ? function.body.findPlace(*instruction.destination)
                : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Assign ||
            !destination || destination->root != lang::MirPlaceRootKind::Loan ||
            destination->type.kind != lang::SemanticType::Expected ||
            instruction.operands.size() != 1)
          continue;
        const auto *rhs =
            function.body.findValue(instruction.operands.front().value);
        const auto *rhsProducer =
            rhs ? testInstruction(function.body, rhs->definition) : nullptr;
        if (!rhsProducer || rhsProducer->kind != lang::MirInstructionKind::Call)
          continue;
        const bool cleanup = !instruction.lifecycle.empty();
        const auto successOwner = std::find_if(
            frontend.mir.classInstances().begin(),
            frontend.mir.classInstances().end(), [&](const auto &owner) {
              return owner.type == destination->type.arguments.front();
            });
        const bool declaredDefault =
            !cleanup && successOwner != frontend.mir.classInstances().end() &&
            successOwner->defaultConstructor ==
                lang::SpecialMemberStatus::Declared;
        bool &covered = cleanup           ? owning
                        : declaredDefault ? observableDefault
                                          : passive;
        if (covered)
          continue;
        covered = true;
        const auto *loan = function.body.findLoan(destination->loan);
        const lang::MirInstruction *producer = nullptr;
        for (const auto &b : function.body.blocks)
          for (const auto &i : b.instructions)
            if (i.kind == lang::MirInstructionKind::Call && loan &&
                i.loan == loan->id)
              producer = &i;
        const auto *target =
            producer && producer->functionTarget
                ? frontend.mir.findFunctionInstance(*producer->functionTarget)
                : nullptr;
        expect(loan && producer && target && instruction.replacementPair != 0,
               "Expected referent retains exact call, loan and cleanup pair");
        if (!loan || !producer || !target)
          continue;
        if (cleanup)
          expect(instruction.lifecycle.size() == 1 &&
                     instruction.lifecycle.front().kind ==
                         lang::MirLifecycleEventKind::Replace &&
                     instruction.lifecycle.front().target == 0,
                 "referent gains no ownership obligation");
        for (int mutation = 0; mutation != 13; ++mutation) {
          if (!cleanup && (mutation == 10 || mutation == 11))
            continue;
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          auto &changedLoan = body->loans[loan->id - 1];
          auto &assignment = *const_cast<lang::MirInstruction *>(
              testInstruction(*body, instruction.id));
          auto &call = *const_cast<lang::MirInstruction *>(
              testInstruction(*body, producer->id));
          if (mutation == 0)
            changedLoan.access = lang::AccessMode::ReadOnly;
          if (mutation == 1)
            const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(target->id))
                ->returnType.referenceAccess = lang::AccessMode::ReadOnly;
          if (mutation == 2)
            call.borrowAccess = lang::AccessMode::ReadOnly;
          if (mutation == 3)
            body->places[destination->id - 1].loan = 0;
          if (mutation == 4)
            changedLoan.producedBy = 0;
          if (mutation == 5)
            assignment.replacementPair = 0;
          if (mutation == 6)
            changedLoan.source = 0;
          if (mutation == 7)
            for (auto &b : body->blocks)
              for (auto &i : b.instructions)
                if (i.kind == lang::MirInstructionKind::ActivateBorrow &&
                    i.loan == loan->id)
                  i.loan = 0;
          if (mutation == 8) {
            if (cleanup)
              assignment.lifecycle.clear();
            else
              assignment.lifecycle.push_back(
                  {.kind = lang::MirLifecycleEventKind::Replace});
          }
          if (mutation == 9 || mutation == 10) {
            bool removed = false;
            for (auto &b : body->blocks) {
              if (b.activeFailure != 0)
                continue;
              const auto at = std::find_if(
                  b.instructions.begin(), b.instructions.end(),
                  [&](const auto &i) {
                    if (mutation == 9)
                      return i.kind == lang::MirInstructionKind::EndBorrow &&
                             i.loan == loan->id;
                    return i.kind == lang::MirInstructionKind::Drop &&
                           i.lifecycle.size() == 1 &&
                           i.lifecycle.front().kind ==
                               lang::MirLifecycleEventKind::Drop &&
                           i.lifecycle.front().source ==
                               instruction.lifecycle.front().source &&
                           !i.lifecycle.front().failureCleanup;
                  });
              if (at != b.instructions.end()) {
                b.instructions.erase(at);
                removed = true;
                break;
              }
            }
            expect(removed, "normal loan endpoint and RHS shell drop exist");
          }
          if (mutation == 11)
            assignment.lifecycle.front().target =
                assignment.lifecycle.front().source;
          if (mutation == 12) {
            bool duplicated = false;
            for (auto &b : body->blocks) {
              for (auto &i : b.instructions) {
                if (i.id <= assignment.id ||
                    i.kind != lang::MirInstructionKind::Assign ||
                    i.info.type != assignment.info.type ||
                    i.operands.size() != 1)
                  continue;
                // Keep the typed consumer and its destination intact, but
                // forge a second consumption of this same ready RHS value.
                i.operands.front() = assignment.operands.front();
                duplicated = true;
                break;
              }
              if (duplicated)
                break;
            }
            expect(duplicated,
                   "fixture has a second typed assignment consumer");
          }
          expect(lang::rebuildMirValueUses(*body),
                 "forgery repairs the use census before admission");
          const bool mirValid = lang::verifyMirProgram(malformed).valid();
          if (mutation == 12 && !cleanup)
            expect(mirValid,
                   "cleanup-free SSA reuse is valid MIR but loses the exact "
                   "single-consumer representation proof");
          else
            expect(!mirValid, "MIR rejects forged Expected authority " +
                                  std::to_string(mutation));
          expect(!lang::CppMirBodyEmitter(malformed, rows)
                      .analyzeProgram()
                      .ready(),
                 "backend rejects forged Expected publication " +
                     std::to_string(mutation));
        }
      }
  expect(passive && owning && observableDefault,
         "fixture covers both Expected cleanup families and observable default "
         "construction");
  bool freeParameter = false, memberParameter = false;
  bool directParameter = false, genericParameter = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.parameterBindings.size() != 1 ||
        function.parameterTypes.size() != 1 ||
        (function.parameterTypes.front().kind != lang::SemanticType::Expected &&
         function.parameterTypes.front().kind != lang::SemanticType::Class))
      continue;
    const auto cleanupType = lang::mirUniqueCleanupType(
        frontend.mir, function.parameterTypes.front());
    const auto passiveClass = std::find_if(
        frontend.mir.classInstances().begin(),
        frontend.mir.classInstances().end(), [&](const auto &instance) {
          return instance.type == function.parameterTypes.front() &&
                 !instance.requiresActiveCleanup;
        });
    if (cleanupType ? cleanupType->requiresActiveCleanup
                    : passiveClass == frontend.mir.classInstances().end())
      continue;
    const auto parameter = std::find_if(
        function.body.places.begin(), function.body.places.end(),
        [&](const auto &place) {
          return place.root == lang::MirPlaceRootKind::Binding &&
                 place.projections.empty() &&
                 place.binding == function.parameterBindings.front() &&
                 place.type == function.parameterTypes.front() &&
                 !place.traits.copyable && place.traits.movable;
        });
    if (parameter == function.body.places.end())
      continue;
    if (function.parameterTypes.front().kind == lang::SemanticType::Expected)
      (function.owner ? memberParameter : freeParameter) = true;
    else {
      directParameter = true;
      genericParameter |= !function.typeArguments.empty();
    }
    for (int mutation = 0; mutation != 3; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      auto &place = changed->body.places[parameter->id - 1];
      if (mutation == 0)
        place.traits.copyable = true;
      if (mutation == 1)
        place.traits.movable = false;
      if (mutation == 2)
        changed->parameterTypes.front() = lang::SemanticType::Int32;
      const auto verification = lang::verifyMirProgram(malformed);
      expect(!verification.valid(),
             "MIR rejects forged move-only formal metadata " +
                 std::to_string(mutation));
      if (mutation != 2)
        expect(std::any_of(
                   verification.errors.begin(), verification.errors.end(),
                   [&](const auto &error) {
                     return error.bodyKind == lang::MirBodyKind::Function &&
                            error.owner == function.id &&
                            error.message ==
                                "by-value formal copy/move facts do "
                                "not match its concrete MIR type";
                   }),
               "formal forgery is rejected by the concrete type-fact check");
      expect(!lang::CppMirBodyEmitter(malformed, rows).analyzeProgram().ready(),
             "backend rejects forged move-only formal metadata " +
                 std::to_string(mutation));
    }
  }
  expect(freeParameter && memberParameter,
         "cleanup-free move-only Expected has free and member formal controls");
  expect(directParameter && genericParameter,
         "the same formal transport rule covers direct user records and "
         "concrete generic parameters");
  bool constructorParameter = false;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    if (constructor.parameterBindings.size() != 1)
      continue;
    const auto formal = std::find_if(
        constructor.body.places.begin(), constructor.body.places.end(),
        [&](const auto &place) {
          return place.root == lang::MirPlaceRootKind::Binding &&
                 place.projections.empty() &&
                 place.binding == constructor.parameterBindings.front() &&
                 place.type.kind == lang::SemanticType::Expected &&
                 !place.traits.copyable && place.traits.movable;
        });
    if (formal == constructor.body.places.end())
      continue;
    constructorParameter = true;
    for (int mutation = 0; mutation != 2; ++mutation) {
      auto malformed = frontend.mir;
      auto *body = lang::findMirBody(
          malformed, {lang::MirBodyKind::Constructor, constructor.id});
      auto &place = body->places[formal->id - 1];
      if (mutation == 0)
        place.traits.copyable = true;
      else
        place.traits.movable = false;
      const auto verification = lang::verifyMirProgram(malformed);
      expect(!verification.valid() &&
                 std::any_of(verification.errors.begin(),
                             verification.errors.end(),
                             [&](const auto &error) {
                               return error.bodyKind ==
                                          lang::MirBodyKind::Constructor &&
                                      error.owner == constructor.id &&
                                      error.message ==
                                          "by-value formal copy/move facts do "
                                          "not match its concrete MIR type";
                             }),
             "constructor formal uses the same concrete type-fact authority");
      expect(!lang::CppMirBodyEmitter(malformed, rows).analyzeProgram().ready(),
             "backend rejects forged constructor formal metadata");
    }
  }
  expect(constructorParameter,
         "a constructor formal covers the same move-only transport contract");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "Expected replacement emits");
}

void testCheckedReferenceClassAssignment() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/reference_class_assignment.gti";
  std::ifstream input(fixture);
  expect(input.good(), "checked reference assignment fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "checked reference assignments verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  expect(lang::CppMirBodyEmitter(frontend.mir, rows).analyzeProgram().ready(),
         "fresh, copied, moved and returned class values replace referents");
  bool passive = false;
  bool owning = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *destination =
            instruction.destination
                ? function.body.findPlace(*instruction.destination)
                : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Assign ||
            !destination ||
            destination->root != lang::MirPlaceRootKind::Binding ||
            destination->projections.size() != 1 ||
            destination->projections.front().kind !=
                lang::MirProjectionKind::Dereference ||
            destination->type.kind != lang::SemanticType::Class)
          continue;
        const bool cleanup = !instruction.lifecycle.empty();
        if (cleanup ? owning : passive)
          continue;
        (cleanup ? owning : passive) = true;
        const auto carrier = std::find_if(
            function.body.places.begin(), function.body.places.end(),
            [&](const auto &place) {
              return place.root == lang::MirPlaceRootKind::Binding &&
                     place.binding == destination->binding &&
                     place.projections.empty();
            });
        expect(carrier != function.body.places.end(),
               "referent retains its pointer carrier");
        if (carrier == function.body.places.end())
          continue;
        if (cleanup)
          expect(instruction.lifecycle.size() == 1 &&
                     instruction.lifecycle.front().kind ==
                         lang::MirLifecycleEventKind::Replace &&
                     instruction.lifecycle.front().target == 0 &&
                     instruction.replacementPair != 0,
                 "referent replacement keeps paired cleanup without acquiring "
                 "ownership");
        for (int mutation = 0; mutation < (cleanup ? 9 : 6); ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          auto &changed = *std::find_if(
              body->places.begin(), body->places.end(),
              [&](const auto &p) { return p.id == destination->id; });
          auto &root =
              *std::find_if(body->places.begin(), body->places.end(),
                            [&](const auto &p) { return p.id == carrier->id; });
          auto &assign = *std::find_if(
              body->findBlock(block.id)->instructions.begin(),
              body->findBlock(block.id)->instructions.end(),
              [&](const auto &i) { return i.id == instruction.id; });
          if (mutation == 0)
            root.type.referenceAccess = lang::AccessMode::ReadOnly;
          if (mutation == 1)
            root.type.arguments.clear();
          if (mutation == 2)
            changed.projections.front().kind =
                lang::MirProjectionKind::RawDereference;
          if (mutation == 3)
            changed.symbol = 0;
          if (mutation == 4 || mutation == 5) {
            for (auto &loan : body->loans) {
              if (std::find(loan.carriers.begin(), loan.carriers.end(),
                            root.binding) == loan.carriers.end())
                continue;
              if (mutation == 4)
                loan.access = lang::AccessMode::ReadOnly;
              else
                loan.carriers.clear();
            }
          }
          if (mutation == 6)
            assign.lifecycle.clear();
          if (mutation == 7)
            assign.lifecycle.front().target = assign.lifecycle.front().source;
          if (mutation == 8)
            assign.replacementPair = 0;
          expect(!lang::CppMirBodyEmitter(malformed, rows)
                      .analyzeProgram()
                      .ready(),
                 "reference assignment rejects forged carrier/access/cleanup " +
                     std::to_string(mutation));
          if (mutation >= 6)
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "MIR independently rejects forged replacement ownership " +
                       std::to_string(mutation));
        }
      }
    }
  }
  expect(
      passive && owning,
      "fixture exercises both cleanup-free and owning reference replacement");
  std::size_t cursorAssignments = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *destination =
            instruction.destination
                ? function.body.findPlace(*instruction.destination)
                : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Assign ||
            !destination || destination->root != lang::MirPlaceRootKind::Loan ||
            !destination->projections.empty() ||
            destination->type.kind != lang::SemanticType::Class)
          continue;
        const auto *loan = function.body.findLoan(destination->loan);
        expect(loan && loan->kind == lang::MirLoanKind::CallResult &&
                   loan->access == lang::AccessMode::Mutable &&
                   loan->twoPhase &&
                   destination->access == lang::AccessMode::ReadOnly &&
                   instruction.replacementPair != 0 &&
                   instruction.lifecycle.size() == 1 &&
                   instruction.lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Replace &&
                   instruction.lifecycle.front().target == 0,
               "contextual operator value info cannot replace the checked "
               "mutable reference and exact replacement cleanup authority");
        if (!loan)
          continue;
        ++cursorAssignments;
        const lang::MirInstruction *producer = nullptr;
        for (const auto &candidateBlock : function.body.blocks)
          for (const auto &candidate : candidateBlock.instructions)
            if (candidate.kind == lang::MirInstructionKind::Call &&
                candidate.loan == loan->id)
              producer = &candidate;
        const auto *target =
            producer && producer->functionTarget
                ? frontend.mir.findFunctionInstance(*producer->functionTarget)
                : nullptr;
        expect(producer && target &&
                   target->returnType.kind == lang::SemanticType::Reference &&
                   target->returnType.referenceAccess ==
                       lang::AccessMode::Mutable &&
                   producer->info.access == lang::AccessMode::ReadOnly,
               "writable user-defined operator selects an exact mutable "
               "reference declaration independent of assignment context");
        if (!producer || !target)
          continue;
        for (int mutation = 0; mutation != 8; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          auto &changedLoan = body->loans[loan->id - 1];
          auto &assignment =
              *std::find_if(body->findBlock(block.id)->instructions.begin(),
                            body->findBlock(block.id)->instructions.end(),
                            [&](const auto &candidate) {
                              return candidate.id == instruction.id;
                            });
          if (mutation == 0)
            changedLoan.access = lang::AccessMode::ReadOnly;
          if (mutation == 1) {
            auto *callee = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(target->id));
            callee->returnType.referenceAccess = lang::AccessMode::ReadOnly;
          }
          if (mutation == 2)
            for (auto &candidateBlock : body->blocks)
              for (auto &candidate : candidateBlock.instructions)
                if (candidate.id == producer->id)
                  candidate.borrowAccess = lang::AccessMode::ReadOnly;
          if (mutation == 3)
            body->places[destination->id - 1].loan = 0;
          if (mutation == 4)
            changedLoan.producedBy = 0;
          if (mutation == 5)
            assignment.replacementPair = 0;
          if (mutation == 6)
            assignment.lifecycle.clear();
          if (mutation == 7)
            changedLoan.source = 0;
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "MIR rejects forged cursor declaration/access/loan/cleanup " +
                     std::to_string(mutation));
          expect(!lang::CppMirBodyEmitter(malformed, rows)
                      .analyzeProgram()
                      .ready(),
                 "C++ admission rejects forged cursor authority rather than "
                 "trusting contextual access " +
                     std::to_string(mutation));
        }
      }
    }
  }
  expect(cursorAssignments == 2,
         "unrelated named cursor repeats owning reference replacement");
  const auto readonly = analyze("readonly-reference-assignment.gti", R"(
struct Point { mut int value; Point(int value) : value(value) {} };
void replace(Point& target) { target = Point(2); }
int main() { return 0; }
)");
  expect(!readonly.canGenerateCode() &&
             std::any_of(readonly.diagnostics.begin(),
                         readonly.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2002";
                         }),
         "read-only references cannot replace their referents");
}

void testContainedClosureConstructorPlan() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/constructor_callback_field.gti";
  std::ifstream input(fixture);
  expect(input.good(), "contained closure constructor fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "closure field construction must verify");
  if (!frontend.canGenerateCode())
    return;
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const auto rows = buildRows(frontend, frontend.mir, standard);
    std::size_t closureConstructors = 0;
    for (const auto &row : rows.containedConstructors) {
      const auto *constructor =
          frontend.mir.findConstructorInstance(row.constructor);
      if (constructor &&
          std::any_of(constructor->parameterTypes.begin(),
                      constructor->parameterTypes.end(), [](const auto &type) {
                        return type.kind == lang::SemanticType::Lambda;
                      }))
        ++closureConstructors;
    }
    expect(closureConstructors == 2,
           "both library and unrelated generic constructors need containment");
    expect(!gti_test::emitCpp(frontend, standard).contents.empty(),
           "containment must retain the admitted closure-dependent body plan");
  }
}

void testOwningCallFieldInputTransfers() {
  for (const auto *name : {"constructor_collection_field_inputs",
                           "constructor_owning_call_inputs"}) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures" / (std::string(name) + ".gti");
    std::ifstream input(fixture);
    expect(input.good(), "owning call field fixture must exist");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
    expect(frontend.canGenerateCode(), "owning call fields must verify");
    if (!frontend.canGenerateCode())
      continue;
    const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    expect(emitter.analyzeProgram().ready(),
           "owning function inputs and field results retain separate handoffs");
    std::size_t checked = 0;
    for (const auto &constructor : frontend.mir.constructorInstances()) {
      for (const auto &block : constructor.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Call ||
              !instruction.functionTarget || !instruction.successResultDrop ||
              instruction.lifecycle.empty() || !instruction.result)
            continue;
          const auto uses = constructor.body.usesOf(*instruction.result);
          const auto publication =
              std::find_if(uses.begin(), uses.end(), [&](const auto &use) {
                const auto *consumer =
                    testInstruction(constructor.body, use.instruction);
                return consumer &&
                       consumer->kind == lang::MirInstructionKind::Initialize &&
                       consumer->constructorInitializer != 0;
              });
          if (publication == uses.end())
            continue;
          ++checked;
          const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                             constructor.id};
          expect(emitter.supportsFailureBodyText(address),
                 "field-producing call must retain its failure body");
          for (int mutation = 0; mutation != 8; ++mutation) {
            auto malformed = frontend.mir;
            auto *body = lang::findMirBody(malformed, address);
            auto &instructions = body->findBlock(block.id)->instructions;
            auto changed =
                std::find_if(instructions.begin(), instructions.end(),
                             [&](const auto &candidate) {
                               return candidate.id == instruction.id;
                             });
            auto &event = changed->lifecycle.front();
            if (mutation == 0)
              changed->lifecycle.clear();
            if (mutation == 1)
              event.source = *instruction.successResultDrop;
            if (mutation == 2)
              event.target = *instruction.successResultDrop;
            if (mutation == 3)
              event.conditional = true;
            if (mutation == 4)
              event.failureCleanup = true;
            if (mutation == 5)
              changed->lifecycle.push_back(event);
            if (mutation == 6)
              changed->successResultDrop.reset();
            if (mutation == 7) {
              for (auto &candidateBlock : body->blocks)
                for (auto &candidate : candidateBlock.instructions)
                  if (candidate.id == publication->instruction)
                    candidate.lifecycle.clear();
            }
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "forged input transfer or field publication must fail MIR "
                   "verification " +
                       std::to_string(mutation));
          }
        }
      }
    }
    expect(checked != 0,
           "fixture must consume owning inputs to produce a field");
    expect(!gti_test::emitCpp(frontend).contents.empty(),
           "owning field calls must reach production emission");
  }
}

void testOwnedExpectedMemberArgumentTransfer() {
  const auto frontend = analyze("owning-expected-member-argument.gti", R"(
enum class errc { absent };
mut int owned_drops = 0;
class Owned {
public:
  int id;
  Owned(int id) : id(id) {}
  ~Owned() { ++owned_drops; }
};
expected<Owned, errc> make_owned(bool present) {
  if (!present) { return unexpected(errc::absent); }
  return Owned(7);
}
expected<Owned, errc> make_checked(int divisor) { return Owned(12 / divisor); }
class Sink {
  mut int count = 0;
public:
  void accept(expected<Owned, errc> value) mut { ++this.count; }
  int inspect(expected<Owned, errc> value) { return value ? value.value().id : 0; }
  int calls() { return this.count; }
  static int static_inspect(expected<Owned, errc> value) {
    return value ? value.value().id : 0;
  }
};
class UnrelatedSink {
  mut int count = 0;
public:
  void accept(expected<Owned, errc> value) mut { ++this.count; }
  int inspect(expected<Owned, errc> value) { return value ? value.value().id : 0; }
};
mut Sink& alias(mut Sink& sink) { return sink; }
int main() {
  mut Sink sink{};
  sink.accept(make_owned(true));
  sink.accept(make_checked(1));
  alias(sink).accept(make_owned(true));
  int static_control = Sink::static_inspect(make_owned(false));
  mut UnrelatedSink unrelated{};
  unrelated.accept(make_owned(false));
  int unrelated_control = unrelated.inspect(make_owned(false));
  return sink.calls() - 3 + sink.inspect(make_owned(false)) +
         static_control + unrelated_control;
}
)");
  expect(frontend.canGenerateCode(), "owned Expected member arguments verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  expect(lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
         "borrowed member receivers independently consume owned call results");
  unsigned checked = 0;
  unsigned checkedFailureProducer = 0;
  unsigned checkedPlainProducer = 0;
  unsigned checkedReadOnlyReceiver = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None)
      continue;
    const auto address =
        lang::MirBodyAddress{lang::MirBodyKind::Function, function.id};
    for (const auto &block : function.body.blocks) {
      for (const auto &consumer : block.instructions) {
        if (consumer.kind != lang::MirInstructionKind::Call ||
            !consumer.functionTarget || !consumer.receiver ||
            consumer.operands.size() != 1 ||
            consumer.operands.front().kind != lang::MirOperandKind::Value ||
            consumer.operands.front().type.kind != lang::SemanticType::Expected)
          continue;
        const auto *value =
            function.body.findValue(consumer.operands.front().value);
        const auto *producer =
            value ? testInstruction(function.body, value->definition) : nullptr;
        const auto *target =
            frontend.mir.findFunctionInstance(*consumer.functionTarget);
        const auto transfer = std::find_if(
            consumer.lifecycle.begin(), consumer.lifecycle.end(),
            [](const auto &event) {
              return event.kind == lang::MirLifecycleEventKind::TransferOut;
            });
        expect(producer && producer->kind == lang::MirInstructionKind::Call &&
                   target && target->owner &&
                   transfer != consumer.lifecycle.end(),
               "exact factory result has a distinct member-argument transfer");
        if (!producer || !target || transfer == consumer.lifecycle.end())
          continue;
        ++checked;
        if (producer->successResultDrop)
          ++checkedFailureProducer;
        else
          ++checkedPlainProducer;
        if (target->receiverMutability == lang::ReceiverMutability::ReadOnly)
          ++checkedReadOnlyReceiver;
        const auto unrelatedTarget = std::find_if(
            frontend.mir.functionInstances().begin(),
            frontend.mir.functionInstances().end(), [&](const auto &candidate) {
              return candidate.owner && candidate.owner != target->owner &&
                     !candidate.staticMember &&
                     candidate.receiverMutability ==
                         target->receiverMutability &&
                     candidate.parameterTypes == target->parameterTypes &&
                     candidate.returnType == target->returnType;
            });
        expect(unrelatedTarget != frontend.mir.functionInstances().end(),
               "same-signature member exists on an unrelated receiver class");
        for (int mutation = 0; mutation != 9; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(malformed, address);
          auto &instructions = body->findBlock(block.id)->instructions;
          auto changed = std::find_if(instructions.begin(), instructions.end(),
                                      [&](const auto &candidate) {
                                        return candidate.id == consumer.id;
                                      });
          if (mutation == 0)
            changed->receiver.reset();
          if (mutation == 1)
            changed->receiver->type = lang::SemanticType::Int32;
          if (mutation == 2) {
            auto *changedTarget = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(target->id));
            changedTarget->receiverMutability =
                target->receiverMutability == lang::ReceiverMutability::Mutable
                    ? lang::ReceiverMutability::ReadOnly
                    : lang::ReceiverMutability::Mutable;
          }
          if (mutation == 3)
            changed->lifecycle.clear();
          if (mutation == 4)
            changed->lifecycle.push_back(*transfer);
          if (mutation == 5)
            body->dropObligations[transfer->source - 1].initiallyActive = true;
          if (mutation == 6)
            changed->parameterTypes.front() = lang::SemanticType::Int32;
          if (mutation == 7) {
            auto *changedTarget = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(target->id));
            changedTarget->staticMember = true;
          }
          if (mutation == 8 &&
              unrelatedTarget != frontend.mir.functionInstances().end())
            changed->functionTarget = unrelatedTarget->id;
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "MIR rejects forged member argument/drop authority " +
                     std::to_string(mutation));
          expect(
              !lang::CppMirBodyEmitter(malformed, map).analyzeProgram().ready(),
              "C++ member argument admission fails closed on forgery " +
                  std::to_string(mutation));
        }
      }
    }
  }
  expect(checked == 6 && checkedFailureProducer == 1 &&
             checkedPlainProducer == 5 && checkedReadOnlyReceiver == 2,
         "both publication forms and receiver access modes retain exact "
         "transfers");
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23})
    expect(!gti_test::emitCpp(frontend, standard).contents.empty(),
           "verified member argument transfers reach production emission");
}

void testExpectedRetainedTemplateLayout() {
  for (const auto *name : {"expected_recursive_container_layout",
                           "expected_native_record_container"}) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures" / (std::string(name) + ".gti");
    std::ifstream input(fixture);
    expect(input.good(), "Expected retained-layout fixture exists");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
    expect(frontend.canGenerateCode(),
           "finite pointer-backed Expected layout verifies");
    if (!frontend.canGenerateCode())
      continue;
    bool checkedRecord = false;
    for (const auto &record : frontend.mir.classInstances()) {
      if (!record.cAbiRecord)
        continue;
      expect(lang::cppRequiresIndirectValueTransport(frontend.mir, record.type),
             "direct native pointer record retains its indirect value ABI");
      for (const auto &instance : frontend.mir.classInstances()) {
        for (const auto &argument : instance.type.arguments) {
          if (argument.kind != lang::SemanticType::Expected ||
              argument.arguments.empty() ||
              argument.arguments.front() != record.type)
            continue;
          checkedRecord = true;
          expect(lang::cppRequiresBytePreservingTransport(frontend.mir,
                                                          argument) &&
                     !lang::cppRequiresIndirectValueTransport(frontend.mir,
                                                              argument),
                 "Expected alternative preserves bytes without changing its "
                 "value ABI");
        }
      }
    }
    if (std::string_view(name) == "expected_native_record_container")
      expect(checkedRecord,
             "native-record Expected classification is exercised");
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      const auto output = gti_test::emitCpp(frontend, standard).contents;
      expect(!output.empty(),
             "retained Expected layout reaches production emission");
      expect(output.find("struct byte_transport<byte_expected<Value, Error>> : "
                         "std::false_type") != std::string::npos &&
                 output.find(
                     standard == lang::CppStandard::Cpp23
                         ? "struct byte_transport<std::expected<Value, Error>> "
                           ": std::false_type"
                         : "struct byte_transport<::nonstd::expected<Value, "
                           "Error>> : std::false_type") != std::string::npos,
             "trusted Expected envelope classification does not depend on "
             "payload layout");
    }
  }
  const auto recursive =
      lang::Frontend().analyze("inline-recursive-layout.gti", R"(
struct Impossible { Impossible value; };
int main() { return 0; }
)");
  expect(!recursive.canGenerateCode() &&
             std::any_of(recursive.diagnostics.begin(),
                         recursive.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2082";
                         }),
         "real by-value layout cycles remain rejected rather than becoming "
         "pointer-backed");
  const auto inlineExpected =
      lang::Frontend().analyze("inline-expected-layout.gti", R"(
enum class errc { absent };
struct Impossible {
  expected<Impossible, errc> value;
  Impossible() : value(unexpected(errc::absent)) {}
};
int main() { Impossible value{}; return 0; }
)");
  expect(!inlineExpected.canGenerateCode() &&
             std::any_of(inlineExpected.diagnostics.begin(),
                         inlineExpected.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2082";
                         }),
         "an inline Expected payload cycle is rejected by semantics before "
         "native envelope classification or backend layout");
}

void testExplicitVirtualResultChannel() {
  const auto frontend = analyze("explicit-virtual-result-channel.gti", R"(
interface Channel<T> { T read(T value); };
class IntegerChannel : public Channel<int> {
public:
  int read(int value) override { return value; }
};
int plain(int value) { return value; }
int main() {
  IntegerChannel channel{};
  Channel<int>& base = channel;
  return plain(base.read(9)) - 9;
}
)");
  expect(frontend.canGenerateCode(), "generic virtual channel must analyze");
  if (!frontend.canGenerateCode())
    return;
  for (auto standard : {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const auto rows = buildRows(frontend, frontend.mir, standard);
    const lang::CppMirBodyEmissionMap map(rows);
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    expect(emitter.analyzeProgram().ready(),
           "explicit virtual result-channel representation must be ready");
    unsigned channels = 0;
    for (std::size_t index = 0; index < rows.bodies.size(); ++index) {
      const auto &row = rows.bodies[index];
      if (row.address.kind != lang::MirBodyKind::Function)
        continue;
      const auto *function =
          frontend.mir.findFunctionInstance(row.address.owner);
      if (!function)
        continue;
      expect(row.conditionalResultParameter == function->virtualMethod,
             "only the generic virtual family carries the result channel");
      if (row.conditionalResultParameter)
        ++channels;
      auto changed = rows;
      changed.bodies[index].conditionalResultParameter =
          !row.conditionalResultParameter;
      const lang::CppMirBodyEmissionMap changedMap(changed);
      const lang::CppMirBodyEmitter invalid(frontend.mir, changedMap);
      expect(
          !invalid.analyzeProgram().ready(),
          "missing family channels and forged ordinary channels fail closed");
    }
    expect(channels == 2,
           "generic declaration and nongeneric override share the channel");
  }
}

void testGeneratedMoveReferenceBindings() {
  const auto frontend =
      analyzeWithStandardLibrary("generated-move-reference-bindings.gti", R"(
class Payload {
  int value = 9;
public:
  ~Payload() {}
};
class BoundMove {
  mut int& target;
  int& source;
  Payload payload{};
public:
  BoundMove(mut int& target, int& source) : target(target), source(source) {}
  ~BoundMove() { this.target += this.source; }
};
int main() {
  mut int target = 0;
  int source = 3;
  {
    BoundMove original{target, source};
    BoundMove moved{std::move(original)};
  }
  return target - 3;
}
)");
  expect(frontend.canGenerateCode(),
         "cleanup-owning movement with mutable and readonly reference fields "
         "must verify");
  if (!frontend.canGenerateCode())
    return;

  bool checked = false;
  for (const auto &owner : frontend.hir.classInstances()) {
    if (!owner.source || owner.source->name().lexeme != "BoundMove")
      continue;
    const auto *concrete = frontend.mir.findClassInstance(owner.id);
    expect(
        concrete &&
            concrete->moveConstructor == lang::SpecialMemberStatus::Generated &&
            concrete->copyConstructor == lang::SpecialMemberStatus::Deleted &&
            concrete->moveAssignment == lang::SpecialMemberStatus::Deleted,
        "binding transport must preserve frontend movement, noncopyability "
        "and the prohibition on reference reseating");
    checked = true;
  }
  expect(checked, "the generated reference-binding owner must be concrete");

  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  bool checkedTransfer = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Construct ||
            instruction.constructorTarget ||
            instruction.constructorKind != lang::ConstructorKind::Move ||
            !instruction.loan)
          continue;
        const auto *loan = function.body.findLoan(*instruction.loan);
        expect(
            loan && loan->access == lang::AccessMode::Mutable &&
                loan->designatedFields.size() == 1 &&
                loan->additionalDependencies.size() == 1 &&
                loan->additionalDependencies.front().access ==
                    lang::AccessMode::ReadOnly &&
                loan->additionalDependencies.front().designatedFields.size() ==
                    1,
            "generated construction preserves independent mutable and "
            "readonly owners with their exact destination fields");
        if (!loan || loan->additionalDependencies.size() != 1)
          continue;
        for (unsigned mutation = 0; mutation < 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          auto *changedLoan =
              const_cast<lang::MirLoan *>(changed->body.findLoan(loan->id));
          if (mutation == 0)
            changedLoan->additionalDependencies.clear();
          else if (mutation == 1)
            changedLoan->additionalDependencies.front().access =
                lang::AccessMode::Mutable;
          else
            changedLoan->additionalDependencies.front().designatedFields =
                changedLoan->designatedFields;
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "generated movement cannot lose an owner, increase a "
                 "borrow's permission or merge its destination field");
          const lang::CppMirBodyEmitter invalid(malformed, map);
          expect(!invalid.supportsFailureBodyText(
                     {lang::MirBodyKind::Function, function.id}),
                 "native construction admission must reject a forged "
                 "stored-dependency handoff");
        }
        const lang::MirInstruction *input = nullptr;
        for (const auto &inputBlock : function.body.blocks)
          for (const auto &candidate : inputBlock.instructions)
            if (!instruction.operands.empty() &&
                candidate.result == instruction.operands.front().value)
              input = &candidate;
        expect(input && input->kind == lang::MirInstructionKind::Move &&
                   input->lifecycle.size() == 1 &&
                   instruction.lifecycle.size() == 2,
               "generated construction must transfer the exact moved SSA "
               "obligation into its own result");
        if (input && input->lifecycle.size() == 1 &&
            instruction.lifecycle.size() == 2) {
          for (unsigned mutation = 0; mutation < 3; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            for (auto &changedBlock : changed->body.blocks)
              for (auto &candidate : changedBlock.instructions) {
                if (candidate.id != instruction.id)
                  continue;
                if (mutation == 0)
                  candidate.lifecycle.erase(candidate.lifecycle.begin());
                else if (mutation == 1)
                  candidate.lifecycle.front().source =
                      input->lifecycle.front().source;
                else
                  candidate.constructorKind = lang::ConstructorKind::Copy;
              }
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "generated move handoff cannot omit its source transfer, "
                   "retire a different same-typed obligation or become copy");
            const lang::CppMirBodyEmitter invalid(malformed, map);
            expect(!invalid.supportsFailureBodyText(
                       {lang::MirBodyKind::Function, function.id}),
                   "native construction admission must require the exact "
                   "source-to-result lifecycle schedule");
          }
        }
        checkedTransfer = true;
      }
    }
  }
  expect(checkedTransfer, "fixture must exercise generated borrowed movement");

  const auto artifact = gti_test::emitCpp(frontend);
  const auto contains = [&](std::string_view text) {
    return artifact.contents.find(text) != std::string::npos;
  };
  expect(contains("target(other.target)") && contains("source(other.source)") &&
             contains("payload(std::move(other.payload))"),
         "generated movement must preserve both reference identities while "
         "moving the owned field");
  expect(!contains("target(std::move(other.target))") &&
             !contains("source(std::move(other.source))") &&
             !contains("target = std::move(other.target)") &&
             !contains("source = std::move(other.source)"),
         "generated lifecycle code must not move or assign through referents");
  expect(contains("BoundMove &operator=(BoundMove &&) = delete;"),
         "reference-carrying assignment must remain deleted in native code");
  // The moved binding feeds the generated construction directly, so the
  // backend spells std::move over its slot. The generated move disarms that
  // shell's active drop state and its ordinary scope drop retires it; no
  // separately staged shell exists, so no extra retirement may be invented.
  const std::string retirement = "GTI MIR generated-move source transfer";
  expect(contains(".construct(std::move(__gti_mir_p_3.get()));") &&
             !contains(retirement),
         "the cleanup-free generated move must consume its moved binding "
         "without inventing another native retirement");
}

void testPassiveRecordComposition() {
  for (const std::string name :
       {"passive_expected_records", "passive_callable_records",
        "temporary_operator_records", "nested_receiver_array"}) {
    const auto path = std::filesystem::path(__FILE__).parent_path() /
                      "fixtures" / (name + ".gti");
    std::ifstream input(path);
    expect(input.good(), "the passive record composition fixture must exist");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(path.string(), source);
    expect(frontend.canGenerateCode(), "record compositions must verify");
    if (!frontend.canGenerateCode())
      continue;
    const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
    // Production planning also applies the exact deduced-callable template
    // overlays. A raw representation map alone is not that whole-program plan.
    expect(
        !gti_test::emitCpp(frontend, frontend.mir, frontend.mir)
             .contents.empty(),
        "record compositions must have a complete verified-MIR representation");
    bool mutated = false;
    for (const auto &function : frontend.mir.functionInstances()) {
      if (mutated)
        break;
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      for (auto &place : changed->body.places) {
        if (name == "nested_receiver_array" &&
            place.root == lang::MirPlaceRootKind::This &&
            place.projections.size() >= 3 &&
            place.projections.back().kind == lang::MirProjectionKind::Index) {
          place.projections.front().field = 999999;
          mutated = true;
          break;
        }
      }
      for (auto &block : changed->body.blocks) {
        for (auto &instruction : block.instructions) {
          if (mutated)
            break;
          if (name == "passive_callable_records" && instruction.lambdaTarget &&
              instruction.callableInvocation &&
              instruction.info.type == changed->returnType &&
              changed->returnType.kind == lang::SemanticType::Class) {
            // A passive result still requires the exact invoked lambda's
            // result type; absence of a semantic drop is not permission to
            // publish some other representation into caller storage.
            instruction.info.type = lang::SemanticType::Int32;
            mutated = true;
          } else if (name == "temporary_operator_records" &&
                     instruction.kind == lang::MirInstructionKind::Call &&
                     instruction.receiver && instruction.callSite == 0 &&
                     instruction.receiver->kind ==
                         lang::MirOperandKind::BorrowWrite) {
            instruction.receiver->kind = lang::MirOperandKind::BorrowRead;
            mutated = true;
          } else if (name == "passive_expected_records" &&
                     instruction.operation == lang::MirOperation::Unexpected &&
                     instruction.operands.size() == 1 &&
                     instruction.operands.front().type.kind ==
                         lang::SemanticType::Class) {
            instruction.operands.push_back(instruction.operands.front());
            mutated = true;
          }
        }
      }
      if (mutated) {
        const lang::CppMirBodyEmitter invalid(malformed, map);
        expect(!invalid.supportsFailureBodyText(
                   {lang::MirBodyKind::Function, function.id}),
               "record staging must reject mismatched fields, receivers, "
               "payloads, and results");
        bool rejected = false;
        try {
          static_cast<void>(gti_test::emitCpp(frontend, malformed, malformed));
        } catch (const std::exception &) {
          rejected = true;
        }
        expect(rejected,
               "the production plan must reject the same mutation after "
               "accepting the original program with its callable overlays");
      }
    }
    expect(mutated,
           "each record regression must exercise its exact negative proof");
  }
}

void testGeneratedDefaultOwningFieldInitializer() {
  const auto frontend = analyze("generated-default-owning-field.gti", R"(
class Resource {
  int value = 4;
public:
  ~Resource() {}
};
class Holder { Resource value{}; };
int main() { Holder holder{}; return 0; }
)");
  expect(frontend.canGenerateCode(),
         "generated default construction of an owning field must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  bool checked = false;
  for (const auto &owner : frontend.hir.classInstances()) {
    if (!owner.source || owner.source->name().lexeme != "Holder")
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::FieldInitializers,
                                       owner.id};
    const auto schedule = emitter.initializerSchedule(address);
    expect(schedule.supported && schedule.fields.size() == 1,
           "braced default construction preserves the exact owning field "
           "schedule");
    auto malformed = frontend.mir;
    auto *changed = const_cast<lang::MirClassInstance *>(
        malformed.findClassInstance(owner.id));
    expect(changed && changed->fieldInitializers.dropObligations.size() == 2,
           "fixture retains both temporary and field rollback identities");
    if (changed && changed->fieldInitializers.dropObligations.size() == 2) {
      auto &drops = changed->fieldInitializers.dropObligations;
      drops.front().place = drops.back().place;
      const lang::CppMirBodyEmitter invalid(malformed, map);
      expect(!invalid.initializerSchedule(address).supported,
             "field publication cannot erase or conflate its source lifetime");
    }
    checked = true;
  }
  expect(checked, "fixture must reach Holder's field initializer body");
}

void testPassiveStatusDefaultField() {
  for (const bool abstract : {false, true}) {
    const std::string source =
        "class Base { public: int stamp = 5; Base() {} " +
        std::string(abstract ? "virtual int read() = 0;" : "") +
        "}; class Derived : public Base { public: int read() " +
        std::string(abstract ? "override " : "") +
        "{ return this.stamp; } }; struct Holder { Derived value; }; "
        "int main() { Holder owner{}; return owner.value.read() - 5; }";
    const auto frontend = analyze("passive-status-default.gti", source);
    expect(frontend.canGenerateCode(), "selected base defaults verify");
    if (!frontend.canGenerateCode())
      continue;
    bool checked = false;
    for (const auto &constructor : frontend.mir.constructorInstances()) {
      const auto *owner = frontend.mir.findClassInstance(constructor.owner);
      if (!owner || owner->declaredFields.size() != 1 ||
          owner->declaredFields.front().type.kind != lang::SemanticType::Class)
        continue;
      const auto fields = lang::cppMirConstructedFieldInitializerBindings(
          frontend.mir, constructor);
      expect(fields.size() == 1 && fields.front().failureStatus,
             "status-only child keeps exact native field construction");
      if (fields.size() != 1)
        continue;
      checked = true;
      const auto child = fields.front().constructor;
      expect(lang::cppMirConstructorStatusCannotFail(frontend.mir, child),
             "exact selected base Invoke is independently non-failing");
      for (int mutation = 0; mutation < 9; ++mutation) {
        auto malformed = frontend.mir;
        auto *changed = const_cast<lang::MirConstructorInstance *>(
            malformed.findConstructorInstance(constructor.id));
        auto *entry = const_cast<lang::MirBlock *>(
            changed->body.findBlock(changed->body.entry));
        auto *success = const_cast<lang::MirBlock *>(
            changed->body.findBlock(entry->terminator.target));
        if (mutation == 0)
          changed->initializers.clear();
        if (mutation == 1)
          changed->initializers.front().field += 10000;
        if (mutation == 2)
          entry->instructions.front().constructorTarget = 0;
        if (mutation == 3)
          entry->terminator.failureRecord = 0;
        if (mutation == 4)
          entry->terminator.elseTarget = entry->terminator.target;
        if (mutation == 5)
          success->instructions.front().constructorInitializer = 0;
        if (mutation == 6)
          success->instructions.front().operands.front().value = 0;
        if (mutation == 7)
          success->instructions.back().fullExpressionEnd = 0;
        if (mutation == 8) {
          auto *nested = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(child));
          nested->initializers.front().constructorTarget = 0;
          expect(!lang::cppMirConstructorStatusCannotFail(malformed, child),
                 "base construction retains its exact selected target");
        }
        expect(
            lang::cppMirConstructedFieldInitializerBindings(malformed, *changed)
                .empty(),
            "status field rejects missing/forged selection, target, outcome "
            "and publication " +
                std::to_string(mutation));
      }
    }
    expect(checked, "status default fixture reaches selected field proof");
    expect(!gti_test::emitCpp(frontend).contents.empty(),
           "selected status-only default reaches production emission");
  }
}

const lang::MirInstruction *testInstruction(const lang::MirBody &body,
                                            lang::MirInstructionId id) {
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.id == id)
        return &instruction;
  return nullptr;
}

const lang::MirInstruction *
testFieldPublication(const lang::MirBody &body,
                     const lang::MirInstruction &producer) {
  if (!producer.result)
    return nullptr;
  const lang::MirInstruction *publication = nullptr;
  for (const auto &use : body.usesOf(*producer.result)) {
    if (use.kind != lang::MirValueUseKind::InstructionOperand)
      continue;
    const auto *instruction = testInstruction(body, use.instruction);
    if (publication || !instruction || use.operandIndex != 0 ||
        instruction->kind != lang::MirInstructionKind::Initialize ||
        instruction->constructorInitializer == 0)
      return nullptr;
    publication = instruction;
  }
  return publication;
}

void testPackParameterCleanup() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/pack_parameter_cleanup_failure.gti";
  std::ifstream input(fixture);
  expect(input.good(), "the owning-pack cleanup regression must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "owning pack parameter cleanup must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "owning pack cleanup must have a complete C++ representation");
  std::size_t owningPacks = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    std::vector<lang::MirDropObligationId> elements;
    for (const auto &drop : function.body.dropObligations) {
      const auto *place = function.body.findPlace(drop.place);
      if (drop.kind == lang::MirDropObligationKind::Binding &&
          place != nullptr && place->projections.size() == 1 &&
          place->projections.front().kind ==
              lang::MirProjectionKind::PackElement)
        elements.push_back(drop.id);
    }
    if (elements.empty())
      continue;
    ++owningPacks;
    expect(elements.size() == 2,
           "both owning pack elements need entry obligations");
    if (elements.size() != 2)
      continue;
    for (const auto id : elements) {
      const auto *drop = function.body.findDropObligation(id);
      expect(drop->initiallyActive && drop->hirObligation != 0 &&
                 drop->dropType.requiresActiveCleanup,
             "each element must retain its concrete HIR cleanup identity");
    }
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    expect(emitter.supportsFailureBodyText(address),
           "fallible pack cleanup must use the status ABI");
    if (emitter.supportsFailureBodyText(address)) {
      const auto text =
          emitter.emitFailureBodyText(address, "pack-cleanup", 1).text;
      expect(text.find(".construct(std::move(__gti_mir_arg_1))") !=
                     std::string::npos &&
                 text.find(".construct(std::move(__gti_mir_arg_2))") !=
                     std::string::npos,
             "pack parameters must enter distinct explicit lifetime slots");
    }
    for (int mutation = 0; mutation != 3; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      auto &first = changed->body.dropObligations[elements.front() - 1];
      auto &second = changed->body.dropObligations[elements.back() - 1];
      if (mutation == 0)
        first.initiallyActive = false;
      else if (mutation == 1)
        second.place = first.place;
      else
        std::swap(first.constructionOrder, second.constructionOrder);
      expect(!lang::verifyMirProgram(malformed).valid(),
             "pack entry lifetime, element coverage, and cleanup order must "
             "reject forgery");
    }
  }
  expect(owningPacks == 1,
         "the regression must exercise one two-element owning pack");

  const auto copyFixture = fixture.parent_path() / "pack_parameter_copies.gti";
  std::ifstream copyInput(copyFixture);
  expect(copyInput.good(), "the repeated pack-copy regression must exist");
  const std::string copySource{std::istreambuf_iterator<char>(copyInput), {}};
  const auto copies =
      analyzeWithStandardLibrary(copyFixture.string(), copySource);
  expect(copies.canGenerateCode(),
         "repeated pack copies must preserve entry-owned source lifetimes");
  if (!copies.canGenerateCode())
    return;
  std::size_t copyStages = 0;
  for (const auto &function : copies.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CallInput ||
            instruction.callInputKind != lang::HirCallInputKind::CopyValue ||
            instruction.operands.size() != 1)
          continue;
        const auto *place =
            function.body.findPlace(instruction.operands.front().place);
        if (place == nullptr || place->projections.size() != 1 ||
            place->projections.front().kind !=
                lang::MirProjectionKind::PackElement)
          continue;
        ++copyStages;
        expect(place->sourceValue == 0 && instruction.lifecycle.size() == 1 &&
                   instruction.lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Initialize &&
                   instruction.lifecycle.front().source == 0,
               "pack copy stages initialize new owners without consuming "
               "the shared entry lifetime");
        auto malformed = copies.mir;
        auto *body = lang::findMirBody(
            malformed, {lang::MirBodyKind::Function, function.id});
        body->places[place->id - 1].sourceValue = instruction.hirValue + 1;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "entry-pack copy admission must not permit forged expression "
               "provenance");
      }
    }
  }
  expect(copyStages == 6,
         "two forwarding expansions and allocation must independently copy "
         "both pack elements");

  const auto borrowFixture =
      fixture.parent_path() / "pack_parameter_borrows.gti";
  std::ifstream borrowInput(borrowFixture);
  expect(borrowInput.good(), "the pack-fold borrowing regression must exist");
  const std::string borrowSource{std::istreambuf_iterator<char>(borrowInput),
                                 {}};
  const auto borrows =
      analyzeWithStandardLibrary(borrowFixture.string(), borrowSource);
  expect(borrows.canGenerateCode(), "owning pack folds must verify");
  if (!borrows.canGenerateCode())
    return;
  std::size_t borrowedElements = 0;
  for (const auto &function : borrows.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Call)
          continue;
        for (const auto &operand : instruction.operands) {
          const auto *place = function.body.findPlace(operand.place);
          if (operand.kind != lang::MirOperandKind::BorrowRead ||
              place == nullptr || place->projections.size() != 1 ||
              place->projections.front().kind !=
                  lang::MirProjectionKind::PackElement)
            continue;
          ++borrowedElements;
          expect(std::any_of(function.body.dropObligations.begin(),
                             function.body.dropObligations.end(),
                             [&](const auto &drop) {
                               return drop.place == place->id &&
                                      drop.initiallyActive;
                             }),
                 "a fold must borrow the live owning slot, not a moved "
                 "native parameter shell");
          auto malformed = borrows.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          auto alias = *place;
          alias.id = body->places.size() + 1;
          body->places.push_back(alias);
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "an alternate owning pack projection without its entry "
                 "lifetime must be rejected");
        }
      }
    }
  }
  expect(borrowedElements == 4,
         "two folds must borrow both owning elements from their live slots");
}

void testForwardListEmplacementPublication() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/forward_list_emplacement_cleanup.gti";
  std::ifstream input(fixture);
  expect(input.good(), "the emplacement cleanup regression must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "fallible emplacement must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "a conditional argument must preserve the staged list receiver");
  std::size_t directFields = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                       constructor.id};
    if (!emitter.supportsFailureBodyText(address))
      continue;
    const auto text =
        emitter.emitFailureBodyText(address, "emplacement", 1).text;
    if (text.find("in-place constructor field reparent") == std::string::npos)
      continue;
    const lang::MirInstruction *elementPublication = nullptr;
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Initialize &&
            instruction.constructorInitializer != 0 &&
            instruction.destination &&
            instruction.info.type.kind == lang::SemanticType::Class &&
            !instruction.info.traits.movable)
          elementPublication = &instruction;
      }
    }
    // Generated owning-wrapper constructors may also publish fields in
    // place. This regression concerns the selected immovable list element.
    if (elementPublication == nullptr)
      continue;
    ++directFields;
    const std::string elementPlace =
        elementPublication == nullptr
            ? ""
            : "__gti_mir_p_" + std::to_string(*elementPublication->destination);
    expect(
        elementPublication != nullptr &&
            text.find(
                "::new (static_cast<void *>(std::addressof(" + elementPlace +
                "))) ::__gti_program::Element("
                "::gti_internal::backend::mir_failure_constructor_tag_v1<") !=
                std::string::npos &&
            text.find(elementPlace + " = std::move(") == std::string::npos &&
            text.find("mir_lifetime_slot<::__gti_program::Element>") ==
                std::string::npos,
        "fallible immovable elements construct in their final field, not "
        "in a temporary followed by assignment");
    auto malformed = frontend.mir;
    auto *body = lang::findMirBody(malformed, address);
    bool mutated = false;
    for (auto &block : body->blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Initialize &&
            instruction.constructorInitializer != 0 &&
            instruction.info.type.kind == lang::SemanticType::Class) {
          instruction.lifecycle.clear();
          mutated = true;
        }
      }
    }
    expect(mutated && !lang::verifyMirProgram(malformed).valid(),
           "in-place publication still requires exact rollback reparenting");
    expect(!lang::CppMirBodyEmitter(malformed, map)
                .supportsFailureBodyText(address),
           "the backend must reject missing in-place rollback publication");
  }
  expect(directFields == 1, "the node needs one in-place element publication");
  // Emplacement moves its node before publishing the returned element, so
  // the moved-from node shell has no fallible cleanup. A guard whose
  // destructor can fail after the returned borrow forms still abandons it.
  const auto returned =
      analyzeWithStandardLibrary("unpublished-return-cleanup.gti", R"(
#include <std/format>
class Guard {
  int32_t divisor;
public:
  Guard(int32_t divisor) : divisor(divisor) {}
  Guard(Guard& other) = delete;
  Guard(Guard&& other) = delete;
  ~Guard() { std::println(100 / this.divisor); }
};
mut int32_t& pick(mut int32_t& value, int32_t divisor) {
  Guard guard{divisor};
  return value;
}
int main() {
  mut int32_t value = 0;
  pick(value, 1) = 3;
  return value - 3;
}
)");
  expect(returned.canGenerateCode(),
         "a fallible guard after a returned borrow must verify");
  std::size_t abandonedReturns = 0;
  for (const auto &function : returned.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      if (block.activeFailure == 0)
        continue;
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::EndBorrow ||
            !instruction.loan)
          continue;
        const auto loan = std::find_if(
            function.body.loans.begin(), function.body.loans.end(),
            [&](const auto &item) { return item.id == *instruction.loan; });
        if (loan == function.body.loans.end() ||
            loan->kind != lang::MirLoanKind::Return)
          continue;
        ++abandonedReturns;
        auto malformed = returned.mir;
        auto *body = lang::findMirBody(
            malformed, {lang::MirBodyKind::Function, function.id});
        for (auto &changed : body->blocks) {
          std::erase_if(changed.instructions, [&](const auto &item) {
            return item.id == instruction.id;
          });
        }
        expect(!lang::verifyMirProgram(malformed).valid(),
               "a failed return cleanup must retire its unpublished loan");
      }
    }
  }
  expect(abandonedReturns != 0,
         "a fallible guard must exercise unpublished-return cleanup");
}

void testContainerSwapStorageAndMemberResults() {
  const auto frontend =
      analyzeWithStandardLibrary("container-swap-slots.gti", R"(
#include <std/memory>
#include <std/vector>
#include <std/forward_list>
class Item {
public:
  int key;
  Item(int value) : key(value) {}
  Item(Item& other) = delete;
  Item(Item&& other) = default;
  ~Item() { std::println(this.key); }
};
std::detail::prefix_owner<Item> make_storage() {
  mut std::detail::prefix_owner<Item> result{uint64_t(1)};
  return std::move(result);
}
int main() {
  auto storage = make_storage();
  mut std::vector<Item> a = std::vector<Item>();
  mut std::vector<Item> b = std::vector<Item>();
  a.push_back(Item(1));
  a.swap(b);
  mut std::forward_list<Item> left = std::forward_list<Item>();
  mut std::forward_list<Item> right = std::forward_list<Item>();
  left.push_front(Item(2));
  left.swap(right);
  return 0;
}
)");
  expect(frontend.canGenerateCode(), "container swaps must pass the frontend");
  if (!frontend.canGenerateCode())
    return;
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    auto rows = buildRows(frontend, frontend.mir, standard);
    const lang::CppMirBodyEmissionMap map(rows);
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    expect(emitter.analyzeProgram().ready(),
           "storage-return and owned member-result swaps must be MIR-ready");
    std::size_t storageReturns = 0;
    std::size_t memberPublications = 0;
    for (const auto &function : frontend.mir.functionInstances()) {
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                         .owner = function.id};
      const bool sourcePrefixOwner = std::any_of(
          frontend.hir.classInstances().begin(),
          frontend.hir.classInstances().end(), [&](const auto &owner) {
            return owner.type == function.returnType && owner.source &&
                   owner.source->name().lexeme == "prefix_owner";
          });
      if (sourcePrefixOwner &&
          function.definitionKind == lang::MirDefinitionKind::Source) {
        ++storageReturns;
        expect(
            emitter.supportsFailureBodyText(address),
            "ordinary move-only storage returns use the explicit status ABI");
        const auto text =
            emitter.emitFailureBodyText(address, "storage-return-test", 1);
        expect(text.emitted() &&
                   text.text.find("construct_representation_at("
                                  "__gti_mir_out_result, std::move(") !=
                       std::string::npos,
               "ordinary storage publication move-constructs the output slot");
        auto missing = rows;
        std::erase_if(missing.types, [&](const auto &row) {
          return row.type == function.returnType;
        });
        const lang::CppMirBodyEmissionMap missingMap(std::move(missing));
        expect(!lang::CppMirBodyEmitter(frontend.mir, missingMap)
                    .supportsFailureBodyText(address),
               "storage returns require their exact representation row");
      }
      for (const auto &block : function.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Call ||
              !instruction.receiver || !instruction.functionTarget ||
              !instruction.successResultDrop ||
              instruction.info.type.kind != lang::SemanticType::Class)
            continue;
          const bool replacesField = std::any_of(
              function.body.blocks.begin(), function.body.blocks.end(),
              [&](const auto &candidate) {
                return std::any_of(
                    candidate.instructions.begin(),
                    candidate.instructions.end(), [&](const auto &user) {
                      return user.kind == lang::MirInstructionKind::Assign &&
                             std::any_of(
                                 user.lifecycle.begin(), user.lifecycle.end(),
                                 [&](const auto &event) {
                                   return event.kind ==
                                              lang::MirLifecycleEventKind::
                                                  Replace &&
                                          event.source ==
                                              *instruction.successResultDrop &&
                                          event.target == 0;
                                 });
                    });
              });
          if (!replacesField)
            continue;
          ++memberPublications;
          for (int mutation = 0; mutation != 3; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = const_cast<lang::MirFunctionInstance *>(
                malformed.findFunctionInstance(function.id));
            for (auto &changedBlock : changed->body.blocks) {
              for (auto &changedInstruction : changedBlock.instructions) {
                if (changedInstruction.id != instruction.id)
                  continue;
                if (mutation == 0)
                  changedInstruction.receiver.reset();
                else if (mutation == 1)
                  changedInstruction.successResultDrop.reset();
                else
                  changedInstruction.callSite += 10000;
              }
            }
            expect(!lang::CppMirBodyEmitter(malformed, map)
                        .supportsFailureBodyText(address),
                   "member field publication rejects missing receivers, "
                   "activation, "
                   "and retargeted input schedules");
          }
        }
      }
    }
    expect(storageReturns > 0 && memberPublications > 0,
           "both new result representations must be exercised");
  }
}

void testToyEngineConstructorInitializers() {
  const auto frontend =
      analyzeWithStandardLibrary("toy-engine-initializers.gti", R"(
#include <std/memory>
mut int destroyed = 0;
class Resource { public: Resource() {} ~Resource() { destroyed += 1; } };
class Point { float x; public: Point(float value) : x(value) {} };
class Camera { Point position; public: Camera() : position(Point(5.0)) {} };
enum class Mode { idle, running };
class EnumState {
  Mode direct;
  Mode grouped;
public: EnumState(Mode initial) : direct(initial), grouped((initial)) {}
};
class Engine {
  std::unique_ptr<Resource> first = nullptr;
  std::unique_ptr<Resource> second = nullptr;
public: Engine() {}
};
int main() {
  Camera camera = Camera();
  Engine engine = Engine();
  EnumState state = EnumState(Mode::running);
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "toy engine initializer cases pass the frontend");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  std::size_t constructedFields = 0;
  std::size_t floatingStages = 0;
  std::size_t enumStages = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Construct &&
            instruction.parameterTypes ==
                std::vector<lang::SemanticType>{lang::SemanticType::Float} &&
            testFieldPublication(constructor.body, instruction))
          ++constructedFields;
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            (instruction.info.type != lang::SemanticType::Float &&
             instruction.info.type.kind != lang::SemanticType::Enum))
          continue;
        if (instruction.info.type.kind == lang::SemanticType::Enum)
          ++enumStages;
        else
          ++floatingStages;
        auto malformed = frontend.mir;
        auto *changed = lang::findMirBody(
            malformed,
            {.kind = lang::MirBodyKind::Constructor, .owner = constructor.id});
        for (auto &candidate : changed->blocks) {
          std::erase_if(candidate.instructions, [&](const auto &item) {
            return item.id == instruction.id;
          });
        }
        expect(lang::rebuildMirValueUses(*changed),
               "initializer mutation should retain coherent use lists");
        expect(!lang::verifyMirProgram(malformed).valid(),
               "removing an explicit scalar field publication must "
               "invalidate MIR");
      }
    }
  }
  expect(constructedFields == 1 && floatingStages == 1,
         "the nested Point initializer must retain one exact floating "
         "publication");
  expect(enumStages == 2,
         "direct and grouped enum fields require explicit MIR publication");
  std::size_t cleanupMutations = 0;
  for (const auto &owner : frontend.mir.classInstances()) {
    bool mutated = false;
    auto malformed = frontend.mir;
    const lang::MirBodyAddress address{
        .kind = lang::MirBodyKind::FieldInitializers, .owner = owner.id};
    auto *body = lang::findMirBody(malformed, address);
    for (auto &block : body->blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Drop &&
            instruction.definedFailure.propagation ==
                lang::FailurePropagationKind::Destructor) {
          instruction.definedFailure.propagation =
              lang::FailurePropagationKind::DirectCall;
          mutated = true;
        }
      }
    }
    if (mutated) {
      ++cleanupMutations;
      expect(!lang::CppMirBodyEmitter(malformed, map)
                  .initializerSchedule(address)
                  .supported,
             "unreachable owner cleanup must still reject forged propagation "
             "metadata");
    }
  }
  expect(cleanupMutations != 0,
         "the null-owner case must exercise unreachable fallible cleanup");
  expect(emitter.analyzeProgram().ready(),
         "toy engine field and constructor initializers have complete MIR "
         "emission");
}

void testFallibleFieldInitializerPropagation() {
  const auto frontend =
      analyzeWithStandardLibrary("fallible-field-initializer.gti", R"(
#include <std/memory>
class Resource { public: Resource() {} };
class Application {
  std::unique_ptr<Resource> resource = std::make_unique<Resource>();
  std::unique_ptr<Resource> shadow = std::make_unique<Resource>();
public: Application() {}
};
int main() { Application application = Application(); return 0; }
)");
  expect(frontend.canGenerateCode(),
         "a fallible field initializer is valid GTI");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  bool found = false;
  for (const auto &owner : frontend.mir.classInstances()) {
    const lang::MirBodyAddress address{
        .kind = lang::MirBodyKind::FieldInitializers, .owner = owner.id};
    const auto analysis =
        lang::CppMirBodyEmitter(frontend.mir, map).analyze(address);
    if (owner.declaredFields.size() != 2 ||
        owner.declaredFields.front().type.kind != lang::SemanticType::Class)
      continue;
    const auto constructor = std::find_if(
        frontend.mir.constructorInstances().begin(),
        frontend.mir.constructorInstances().end(), [&](const auto &item) {
          return item.owner == owner.id && item.initializers.size() == 2 &&
                 item.initializers.front().field ==
                     owner.declaredFields.front().symbol;
        });
    if (constructor == frontend.mir.constructorInstances().end())
      continue;
    found = true;
    expect(analysis.ready(),
           "covered declaration defaults are nonexecuting metadata");
    const auto schedule =
        lang::CppMirBodyEmitter(frontend.mir, map).initializerSchedule(address);
    expect(schedule.supported && schedule.fields.size() == 2 &&
               std::all_of(
                   schedule.fields.begin(), schedule.fields.end(),
                   [](const auto &field) { return field.spelling.empty(); }),
           "source constructor defaults must not execute again as native field "
           "defaults");
    expect(constructor->mayRaiseDefinedFailure &&
               !constructor->body.dropObligations.empty(),
           "the selected constructor owns the default's failure and rollback "
           "schedule");
    for (int mutation = 0; mutation < 4; ++mutation) {
      auto damaged = frontend.mir;
      auto &constructors =
          const_cast<std::vector<lang::MirConstructorInstance> &>(
              damaged.constructorInstances());
      auto &selected = constructors[constructor->id - 1];
      if (mutation == 0)
        selected.initializers.clear();
      else if (mutation == 1)
        selected.initializers.push_back(selected.initializers.front());
      else if (mutation == 2)
        std::swap(selected.initializers.front(), selected.initializers.back());
      else
        selected.initializers.front().targetType = lang::SemanticType::Int32;
      expect(!lang::verifyMirProgram(damaged).valid(),
             "missing, duplicate, reordered, or mistyped selected fields must "
             "fail verification");
      expect(!lang::CppMirBodyEmitter(damaged, map)
                  .initializerSchedule(address)
                  .supported,
             "incomplete selected constructor coverage cannot erase defaults");
    }
    auto malformed = frontend.mir;
    auto *body = lang::findMirBody(malformed, address);
    for (auto &block : body->blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.functionTarget) {
          instruction.functionTarget = 999999;
        }
      }
    }
    expect(!lang::verifyMirProgram(malformed).valid(),
           "a corrupt call target remains invalid MIR, not a known limitation");
  }
  expect(found, "the owning declaration default must be selected into its "
                "source constructor");
  bool emitted = false;
  try {
    const auto artifact = gti_test::emitCpp(frontend);
    emitted = !artifact.contents.empty();
  } catch (const std::exception &) {
  }
  expect(emitted,
         "the complete backend must emit the source-constructor field call");

  const auto mixed =
      analyzeWithStandardLibrary("mixed-generic-field-initializers.gti", R"(
#include <std/memory>
class Record<T> {
  T value;
  int status = 7;
public:
  Record(T value) : value(std::move(value)) {}
  int get() { return this.status; }
};
int inspect<T>(T value) {
  Record<T> record{std::move(value)};
  return record.get();
}
int main() {
  auto identity = [](int value) -> int { return value; };
  std::unique_ptr<Record<int>> absent{};
  Record<std::unique_ptr<int>> present{std::make_unique<int>(5)};
  return inspect(std::move(identity)) + present.get() - 14;
}
)");
  expect(mixed.canGenerateCode(),
         "mixed constructed and type-only generic instances are valid GTI");
  if (!mixed.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap mixedMap(buildRows(mixed, mixed.mir));
  std::size_t typeOnly = 0;
  std::size_t constructed = 0;
  for (const auto &owner : mixed.mir.classInstances()) {
    if (owner.declaredFields.size() != 2 ||
        owner.declaredFields.back().type != lang::SemanticType::Int32)
      continue;
    const bool selected = std::any_of(
        mixed.mir.constructorInstances().begin(),
        mixed.mir.constructorInstances().end(),
        [&](const auto &constructor) { return constructor.owner == owner.id; });
    selected ? ++constructed : ++typeOnly;
    expect(owner.defaultConstructor != lang::SpecialMemberStatus::Generated,
           "type-only catalog erasure must exclude generated construction");
    const lang::MirBodyAddress address{
        .kind = lang::MirBodyKind::FieldInitializers, .owner = owner.id};
    const auto schedule = lang::CppMirBodyEmitter(mixed.mir, mixedMap)
                              .initializerSchedule(address);
    expect(schedule.supported && schedule.fields.size() == 2 &&
               std::all_of(
                   schedule.fields.begin(), schedule.fields.end(),
                   [](const auto &field) { return field.spelling.empty(); }),
           "constructed and unconstructed instances share nonexecuting "
           "declaration-default catalogs");
  }
  expect(typeOnly == 1 && constructed == 2,
         "the regression must retain one unconstructed and two constructed "
         "applications of the same generic owner");
  bool mixedEmitted = false;
  try {
    mixedEmitted = !gti_test::emitCpp(mixed).contents.empty();
  } catch (const std::exception &) {
  }
  expect(mixedEmitted,
         "retained generic carriers must not reject type-only sibling "
         "initializer catalogs");
}

void testOwningCheckedBodyEmitsWholeCleanupProof() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-owning-failure.gti", R"(
class FailureCleanup {
public:
  int32_t bit;
  FailureCleanup(int32_t value) : bit(value) {}
  ~FailureCleanup() { this.bit + 1; }
};

int32_t checked_increment(int32_t value) {
  FailureCleanup scope = FailureCleanup(1);
  return value + 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning checked-operation fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto function = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(),
      [](const lang::MirFunctionInstance &candidate) {
        return !candidate.body.failureRecords.empty() &&
               std::any_of(candidate.body.dropObligations.begin(),
                           candidate.body.dropObligations.end(),
                           [](const lang::MirDropObligation &obligation) {
                             return obligation.dropType.requiresActiveCleanup;
                           });
      });
  expect(function != frontend.mir.functionInstances().end(),
         "the owning fixture should retain failure records and cleanup "
         "obligations in one function body");
  if (function == frontend.mir.functionInstances().end()) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(completeRows(frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = function->id};
  const lang::CppMirBodyEmissionAnalysis analysis = emitter.analyze(address);
  expect(analysis.ready(),
         "an owning checked body should be ready once MIR carries its exact "
         "failure-cleanup drop");
  expect(emitter.supportsFailureBodyText(address),
         "an owning checked body should prove its failure-form cleanup text");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "owning-cleanup-test-v0", 1);
  expect(text.emitted() &&
             text.text.find(".destroy_with_failure(") != std::string::npos &&
             text.text.find("return false;") != std::string::npos,
         "the failure edge must run the engaged slot's fallible destructor "
         "before returning failure");
}

void testRichFieldInitializerSchedule() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-rich-field-initializers.gti", R"(
enum class Mode {
  Ready,
};

int32_t baseline = 11;

class Reading {
  int32_t value;
public:
  Reading(int32_t initial) : value(initial) {}
};

struct Marker {};

struct Component {
  mut uint64_t offset = 0;
  mut uint64_t length = 0;
};

class Dashboard {
  Mode mode = Mode::Ready;
  int32_t baseline_copy = baseline;
  Reading initial{7};
  int32_t samples[2] = {2, 3};
  Marker marker{};
  Component component{};
};

int main() {
  Dashboard dashboard = Dashboard();
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the rich field-initializer fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::CppMirInitializerScheduleText *selected = nullptr;
  std::optional<lang::CppMirInitializerScheduleText> schedule;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    lang::CppMirInitializerScheduleText candidate = emitter.initializerSchedule(
        {.kind = lang::MirBodyKind::FieldInitializers, .owner = instance.id});
    if (candidate.supported && candidate.fields.size() == 6) {
      schedule = std::move(candidate);
      selected = &*schedule;
      break;
    }
  }
  expect(selected != nullptr,
         "the dashboard initializer body should expose its six-field MIR "
         "schedule");
  if (selected == nullptr) {
    return;
  }
  expect(std::all_of(selected->fields.begin(), selected->fields.end(),
                     [](const auto &field) { return field.spelling.empty(); }),
         "selected declaration defaults must not execute again as native "
         "in-class initializers");
  const auto dashboard = std::find_if(
      frontend.mir.constructorInstances().begin(),
      frontend.mir.constructorInstances().end(), [&](const auto &constructor) {
        return constructor.generatedDefault &&
               constructor.initializers.size() == 6;
      });
  const bool bodySupported =
      dashboard != frontend.mir.constructorInstances().end() &&
      emitter.supportsFailureBodyText(
          {.kind = lang::MirBodyKind::Constructor, .owner = dashboard->id});
  expect(bodySupported,
         "the selected constructor must retain the mixed enum, global, "
         "constructed-class, fixed-array and empty-field schedule");
  if (!bodySupported)
    return;

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("Marker marker;") != std::string::npos &&
             artifact.contents.find("Marker{}") != std::string::npos &&
             artifact.contents.find("Component component;") !=
                 std::string::npos &&
             artifact.contents.find("passive generated default publishes") !=
                 std::string::npos &&
             // Resolved enumerator reads are exact MIR constants, not loads
             // of a native enumerator symbol. Keep the selected enum type and
             // discriminant in this default-publication assertion.
             artifact.contents.find("static_cast<::__gti_program::Mode>(0)") !=
                 std::string::npos &&
             artifact.contents.find("::__gti_program::baseline") !=
                 std::string::npos &&
             artifact.contents.find("std::array<std::int32_t, 2>") !=
                 std::string::npos,
         "the complete backend must spell each rich declaration default in "
         "its selected MIR constructor, not duplicate in-class defaults");

  for (const bool explicitConstructor : {false, true}) {
    const auto mixed = analyze("array-empty-field-schedule.gti", std::string(R"(
struct Empty {};
class Holder {
  int32_t samples[2] = {2, 3};
  Empty empty{};
public:
)") + (explicitConstructor ? "  Holder() {}\n" : "") + R"(
};
int main() { Holder holder{}; return 0; }
)");
    expect(mixed.canGenerateCode(),
           "array plus empty-class declaration defaults must verify");
    if (!mixed.canGenerateCode())
      continue;
    const lang::CppMirBodyEmissionMap mixedMap(buildRows(mixed, mixed.mir));
    const lang::CppMirBodyEmitter mixedEmitter(mixed.mir, mixedMap);
    const auto holder = std::find_if(
        mixed.mir.constructorInstances().begin(),
        mixed.mir.constructorInstances().end(), [](const auto &constructor) {
          return constructor.initializers.size() == 2;
        });
    expect(holder != mixed.mir.constructorInstances().end(),
           "the mixed default schedule must have an exact constructor");
    if (holder == mixed.mir.constructorInstances().end())
      continue;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                       .owner = holder->id};
    expect(mixedEmitter.supportsBodyText(address),
           "explicit and generated constructors both admit exact empty "
           "construction beside array initialization");
    for (int mutation = 0; mutation != 3; ++mutation) {
      auto forged = mixed.mir;
      auto *body = lang::findMirBody(forged, address);
      bool changed = false;
      for (auto &block : body->blocks) {
        for (auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Construct ||
              instruction.constructorTarget || !instruction.result)
            continue;
          if (mutation == 0) {
            instruction.hirValue = 0;
          } else {
            auto &classes = const_cast<std::vector<lang::MirClassInstance> &>(
                forged.classInstances());
            const auto empty = std::find_if(
                classes.begin(), classes.end(), [&](const auto &type) {
                  return type.type == instruction.info.type;
                });
            if (empty == classes.end())
              continue;
            if (mutation == 1)
              empty->declaredFields.push_back({});
            else
              empty->requiresActiveCleanup = true;
          }
          changed = true;
          break;
        }
        if (changed)
          break;
      }
      const lang::CppMirBodyEmitter forgedEmitter(forged, mixedMap);
      expect(changed && !forgedEmitter.supportsBodyText(address) &&
                 !forgedEmitter.supportsFailureBodyText(address),
             "empty-field publication rejects forged provenance, nonempty "
             "layout and active cleanup rather than erasing their semantics");
    }
  }
}

void testDependentFieldInitializerSchedule() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-body-dependent-field-initializers.gti", R"(
class StaticArray<T, uint64_t N> {
  T values[N] = {};
};

class WrappedArray<T, uint64_t N> {
  StaticArray<T, N> value = StaticArray<T, N>();
};

int main() {
  StaticArray<int32_t, 4> four = StaticArray<int32_t, 4>();
  StaticArray<int32_t, 8> eight = StaticArray<int32_t, 8>();
  WrappedArray<int32_t, 4> wrapped_four = WrappedArray<int32_t, 4>();
  WrappedArray<int32_t, 8> wrapped_eight = WrappedArray<int32_t, 8>();
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "dependent field-initializer fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  std::size_t arrays = 0;
  std::size_t wrappers = 0;
  std::unordered_set<std::uint64_t> arrayExtents;
  std::unordered_set<lang::HirClassInstanceId> wrappedOwners;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    const lang::ClassTypeInfo *info =
        frontend.semantics.findClassType(instance.declaration);
    if (info == nullptr || info->declaration == nullptr ||
        (info->declaration->name().lexeme != "StaticArray" &&
         info->declaration->name().lexeme != "WrappedArray")) {
      continue;
    }
    const lang::CppMirInitializerScheduleText schedule =
        emitter.initializerSchedule(
            {.kind = lang::MirBodyKind::FieldInitializers,
             .owner = instance.id});
    expect(schedule.supported && schedule.fields.size() == 1,
           "each dependent class instance should retain one verified field "
           "initializer");
    if (!schedule.supported || schedule.fields.size() != 1) {
      continue;
    }
    expect(schedule.fields.front().spelling.empty(),
           "selected generated constructors must suppress duplicate native "
           "declaration defaults");
    const auto constructor = std::find_if(
        frontend.mir.constructorInstances().begin(),
        frontend.mir.constructorInstances().end(), [&](const auto &candidate) {
          return candidate.owner == instance.id && candidate.generatedDefault;
        });
    expect(constructor != frontend.mir.constructorInstances().end() &&
               constructor->initializers.size() == 1 &&
               instance.declaredFields.size() == 1,
           "each concrete dependent owner must have its exact generated "
           "one-field constructor");
    if (constructor == frontend.mir.constructorInstances().end() ||
        constructor->initializers.size() != 1 ||
        instance.declaredFields.size() != 1)
      continue;
    const auto &initializer = constructor->initializers.front();
    const auto &field = instance.declaredFields.front();
    expect(initializer.field == field.symbol &&
               initializer.targetType == field.type &&
               emitter.supportsFailureBodyText(
                   {.kind = lang::MirBodyKind::Constructor,
                    .owner = constructor->id}),
           "concrete initializer identities must retain an executable "
           "verified-MIR constructor schedule");
    if (info->declaration->name().lexeme == "StaticArray") {
      expect(field.type.kind == lang::SemanticType::Array &&
                 field.type.arguments.size() == 1 &&
                 field.type.arguments.front() == lang::SemanticType::Int32 &&
                 (field.type.arrayLength == 4 || field.type.arrayLength == 8),
             "each array initializer must retain its concrete element and "
             "extent rather than a representative generic spelling");
      arrayExtents.insert(field.type.arrayLength);
      ++arrays;
    } else {
      const auto wrapped = std::find_if(
          frontend.mir.classInstances().begin(),
          frontend.mir.classInstances().end(),
          [&](const auto &candidate) { return candidate.type == field.type; });
      expect(wrapped != frontend.mir.classInstances().end() &&
                 wrapped->declaredFields.size() == 1 &&
                 wrapped->declaredFields.front().type.kind ==
                     lang::SemanticType::Array,
             "each wrapper must select its exact concrete array owner");
      if (wrapped != frontend.mir.classInstances().end())
        wrappedOwners.insert(wrapped->id);
      ++wrappers;
    }
  }
  expect(arrays == 2 && wrappers == 2 && arrayExtents.size() == 2 &&
             wrappedOwners.size() == 2,
         "the dependent initializer fixture should exercise both concrete "
         "instances without collapsing their physical representations");
  bool emittedDistinctExtents = false;
  try {
    const auto artifact = gti_test::emitCpp(frontend);
    emittedDistinctExtents =
        artifact.contents.find("std::array<std::int32_t, 4>") !=
            std::string::npos &&
        artifact.contents.find("std::array<std::int32_t, 8>") !=
            std::string::npos;
  } catch (const std::exception &) {
  }
  expect(emittedDistinctExtents,
         "the complete backend must preserve both concrete array extents "
         "through the selected generated constructor schedules");
}

void testContainedOwningFieldConstructor() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-body-owning-field-constructor.gti", R"(
#include <std/vector>

class Holder {
  mut std::vector<int32_t> values = std::vector<int32_t>();

public:
  Holder() {}
};

int32_t main() {
  Holder value{};
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning field-constructor fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::HirClassInstance *holder = nullptr;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source != nullptr &&
        instance.source->name().lexeme == "Holder") {
      holder = &instance;
      break;
    }
  }
  expect(holder != nullptr,
         "the fixture should retain the Holder HIR class instance");
  if (holder == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirClassInstance *owner =
      frontend.mir.findClassInstance(holder->id);
  expect(owner != nullptr, "the fixture should retain the Holder MIR instance");
  if (owner == nullptr) {
    return;
  }
  const lang::CppMirInitializerScheduleText schedule =
      emitter.initializerSchedule(
          {.kind = lang::MirBodyKind::FieldInitializers, .owner = owner->id});
  expect(schedule.supported && schedule.fields.size() == 1,
         "Holder's native in-class field schedule should be verified by MIR");
  expect(owner->fieldInitializers.dropObligations.size() == 2 &&
             !owner->fieldInitializers.failureRecords.empty(),
         "Holder's field-initializer MIR should retain the temporary, field "
         "rollback, and checked construction edge erased by native direct "
         "initialization");

  const lang::MirConstructorInstance *ownerConstructor = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    if (constructor.owner == owner->id) {
      ownerConstructor = &constructor;
    }
  }
  expect(ownerConstructor != nullptr &&
             ownerConstructor->mayRaiseDefinedFailure,
         "Holder's concrete constructor should retain the field-initializer "
         "failure effect even when its own source body is empty");
  if (ownerConstructor == nullptr) {
    return;
  }
  const std::size_t defaultFieldRollbacks = std::count_if(
      ownerConstructor->body.dropObligations.begin(),
      ownerConstructor->body.dropObligations.end(),
      [](const lang::MirDropObligation &drop) {
        return drop.kind == lang::MirDropObligationKind::ConstructionRollback &&
               drop.initiallyActive;
      });
  const std::size_t selectedFieldRollbacks = std::count_if(
      ownerConstructor->body.dropObligations.begin(),
      ownerConstructor->body.dropObligations.end(),
      [](const lang::MirDropObligation &drop) {
        return drop.kind == lang::MirDropObligationKind::ConstructionRollback &&
               !drop.initiallyActive;
      });
  expect(defaultFieldRollbacks == 0 && selectedFieldRollbacks == 1 &&
             ownerConstructor->initializers.size() == 1 &&
             schedule.fields.front().spelling.empty(),
         "the source constructor must start with an inactive field obligation "
         "and execute its selected default before publishing the field");
  const lang::CppMirBodyEmissionAnalysis constructorAnalysis = emitter.analyze(
      {.kind = lang::MirBodyKind::Constructor, .owner = ownerConstructor->id});
  expect(constructorAnalysis.ready() &&
             !hasIssue(constructorAnalysis,
                       lang::CppMirBodyEmissionIssueKind::
                           MissingPartialConstructionRollbackMir),
         "the body emitter should consume the verified default-field "
         "rollback instead of reporting a construction gap");
  const lang::MirBodyAddress constructorAddress{
      .kind = lang::MirBodyKind::Constructor, .owner = ownerConstructor->id};
  expect(emitter.supportsFailureBodyText(constructorAddress) &&
             !emitter.supportsBodyText(constructorAddress),
         "an in-place failure constructor must not advertise a plain body "
         "that addresses its erased intermediate value slot");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto marker = [&](std::size_t constructor) {
    return artifact.contents.find(
               "scalar-cfg-constructor-failure-v1 constructor-instance " +
               std::to_string(constructor) + "\n") != std::string::npos;
  };
  expect(marker(ownerConstructor->id),
         "the ordinary Holder constructor should publish from verified MIR "
         "with checked field construction in its selected initialization "
         "schedule");
}

void testConstructedPrvalueCheckedFieldReplacement() {
  const auto frontend =
      analyzeWithStandardLibrary("constructed-prvalue-checked-field.gti", R"(
#include <std/memory>
class Payload {
  mut std::unique_ptr<int32_t> data;
public:
  Payload(int32_t value) : data(std::make_unique<int32_t>(value)) {}
};
struct Holder { mut Payload value = Payload(1); };
void replace(mut std::unique_ptr<Holder>& owner) {
  owner->value = Payload(2);
}
int main() {
  mut auto owner = std::make_unique<Holder>();
  replace(owner);
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "an unrelated owning constructed prvalue must assign through a "
         "checked owner field");
  if (!frontend.canGenerateCode())
    return;
  lang::HirFunctionInstanceId target = 0;
  for (const auto &function : frontend.hir.functionInstances())
    if (function.source && function.source->name().lexeme == "replace")
      target = function.id;
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = target};
  const auto *body = lang::findMirBody(frontend.mir, address);
  expect(target != 0 && body != nullptr,
         "checked replacement must have a concrete function body");
  if (body == nullptr)
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.supportsFailureBodyText(address),
         "constructed RHS staging must preserve checked receiver evaluation "
         "and its success-edge field publication");
  lang::MirInstructionId producerId = 0, endId = 0, dropId = 0;
  lang::MirDropObligationId obligation = 0;
  lang::MirLoanId destinationLoan = 0;
  for (const auto &block : body->blocks)
    for (const auto &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Construct &&
          instruction.constructorTarget && instruction.successResultDrop) {
        producerId = instruction.id;
        obligation = *instruction.successResultDrop;
      }
    }
  for (const auto &block : body->blocks)
    for (const auto &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::Assign ||
          !instruction.destination || instruction.lifecycle.size() != 1 ||
          instruction.lifecycle.front().source != obligation)
        continue;
      const auto *destination = body->findPlace(*instruction.destination);
      if (destination != nullptr &&
          destination->root == lang::MirPlaceRootKind::Loan)
        destinationLoan = destination->loan;
    }
  std::size_t destinationEnds = 0;
  for (const auto &block : body->blocks) {
    if (block.activeFailure != 0)
      continue;
    for (const auto &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::EndBorrow &&
          instruction.loan == destinationLoan) {
        endId = instruction.id;
        ++destinationEnds;
      }
      if (instruction.kind == lang::MirInstructionKind::Drop &&
          instruction.lifecycle.size() == 1 &&
          instruction.lifecycle.front().source == obligation)
        dropId = instruction.id;
    }
  }
  expect(producerId && obligation && destinationLoan && destinationEnds == 1 &&
             endId && dropId,
         "constructed field replacement retains exact activation, loan end "
         "and staging-shell destruction identities");
  if (!producerId || !obligation || !destinationLoan || destinationEnds != 1 ||
      !endId || !dropId)
    return;
  constexpr std::array<std::string_view, 4> mutationReasons = {
      "checked constructed-field publication rejects missing destination loan "
      "retirement",
      "checked constructed-field publication rejects destination retirement "
      "after staging-shell destruction",
      "checked constructed-field publication rejects lost staging-shell "
      "cleanup",
      "checked constructed-field publication rejects lost success activation"};
  for (int mutation = 0; mutation < 4; ++mutation) {
    auto forged = frontend.mir;
    auto *changedBody = lang::findMirBody(forged, address);
    bool changed = false;
    for (auto &block : changedBody->blocks) {
      auto end =
          std::find_if(block.instructions.begin(), block.instructions.end(),
                       [&](const auto &i) { return i.id == endId; });
      auto drop =
          std::find_if(block.instructions.begin(), block.instructions.end(),
                       [&](const auto &i) { return i.id == dropId; });
      if (mutation == 0 && end != block.instructions.end()) {
        block.instructions.erase(end);
        changed = true;
      } else if (mutation == 1 && end != block.instructions.end() &&
                 drop != block.instructions.end()) {
        std::iter_swap(end, drop);
        changed = true;
      } else if (mutation == 2 && drop != block.instructions.end()) {
        block.instructions.erase(drop);
        changed = true;
      } else if (mutation == 3) {
        for (auto &instruction : block.instructions)
          if (instruction.id == producerId) {
            instruction.successResultDrop.reset();
            changed = true;
          }
      }
    }
    const lang::CppMirBodyEmitter forgedEmitter(forged, map);
    expect(changed && !forgedEmitter.supportsFailureBodyText(address),
           mutationReasons[mutation]);
  }
}

void testPreparedConstructorReplacement() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/recursive_cleanup_failure.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "recursive replacement fixture lowers");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  bool found = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &producer : block.instructions) {
        if (producer.kind != lang::MirInstructionKind::Construct ||
            !producer.result || !producer.successResultDrop ||
            producer.lifecycle.size() != 1 ||
            producer.lifecycle.front().kind !=
                lang::MirLifecycleEventKind::TransferOut)
          continue;
        bool replacement = false;
        for (const auto &use : function.body.usesOf(*producer.result))
          for (const auto &candidateBlock : function.body.blocks)
            for (const auto &consumer : candidateBlock.instructions)
              if (consumer.id == use.instruction &&
                  consumer.kind == lang::MirInstructionKind::Assign)
                replacement = true;
        if (!replacement)
          continue;
        found = true;
        const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                           function.id};
        expect(lang::CppMirBodyEmitter(frontend.mir, map)
                   .supportsFailureBodyText(address),
               "prepared arguments transfer at invocation while the result "
               "activates only on success before replacement cleanup");
        for (int mutation = 0; mutation != 4; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(malformed, address);
          for (auto &changed : body->findBlock(block.id)->instructions) {
            if (changed.id != producer.id)
              continue;
            if (mutation == 0)
              changed.lifecycle.clear();
            if (mutation == 1)
              changed.lifecycle.front().source = *producer.successResultDrop;
            if (mutation == 2)
              changed.lifecycle.push_back(changed.lifecycle.front());
            if (mutation == 3)
              changed.lifecycle.front().conditional = true;
          }
          expect(!lang::CppMirBodyEmitter(malformed, map)
                      .supportsFailureBodyText(address),
                 "missing, mismatched, duplicate or conditional input handoff "
                 "cannot publish a replacement constructor result");
        }
        return;
      }
    }
  }
  expect(found, "fixture exercises prepared constructor replacement");
}

void testExpectedArgumentLoopTransport() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/expected_argument_loop.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "expected loop arguments must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(), "expected argument loops must emit");
  size_t retired = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    for (const auto &block : function.body.blocks) {
      for (const auto &stage : block.instructions) {
        if (stage.kind != lang::MirInstructionKind::CallInput ||
            !stage.preparedParameterDrop || stage.lifecycle.size() != 1)
          continue;
        const auto &event = stage.lifecycle.front();
        const auto *drop = function.body.findDropObligation(event.source);
        const auto *place =
            drop ? function.body.findPlace(drop->place) : nullptr;
        if (event.kind != lang::MirLifecycleEventKind::Reparent || !place ||
            place->root != lang::MirPlaceRootKind::Temporary)
          continue;
        ++retired;
        const auto text =
            emitter.emitFailureBodyText(address, "expected-loop", 1).text;
        expect(text.find("__gti_mir_p_" + std::to_string(place->id) +
                         ".destroy();") != std::string::npos,
               "reparented temporary carrier must retire before loop reuse");
        for (int mutation = 0; mutation != 2; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(malformed, address);
          for (auto &candidate : body->blocks)
            for (auto &instruction : candidate.instructions)
              if (instruction.id == stage.id) {
                if (mutation == 0)
                  instruction.lifecycle.clear();
                else
                  instruction.lifecycle.front().source =
                      *stage.preparedParameterDrop;
              }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "prepared transfer must retain its exact temporary lifetime");
        }
      }
    }
  }
  expect(retired == 2,
         "free and member loop arguments must retire temporary carriers");
}

void testCopiedBorrowedArgumentAcrossInvoke() {
  testExpectedArgumentLoopTransport();
  {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures/expected_temporary_observer.gti";
    std::ifstream input(fixture);
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
    expect(frontend.canGenerateCode(),
           "temporary state-only observers must verify");
    if (!frontend.canGenerateCode())
      return;
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      const lang::CppMirBodyEmissionMap map(
          buildRows(frontend, frontend.mir, standard));
      expect(
          lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
          "state-only observation retains one placed Expected on both "
          "standards");
      size_t checked = 0;
      for (const auto &function : frontend.mir.functionInstances()) {
        const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                           function.id};
        for (const auto &block : function.body.blocks) {
          for (const auto &producer : block.instructions) {
            if (producer.kind != lang::MirInstructionKind::Call ||
                !producer.functionTarget || !producer.successResultDrop ||
                producer.info.type.kind != lang::SemanticType::Expected)
              continue;
            ++checked;
            for (int mutation = 0; mutation != 5; ++mutation) {
              auto malformed = frontend.mir;
              auto *body = lang::findMirBody(malformed, address);
              const auto *resultDrop =
                  body->findDropObligation(*producer.successResultDrop);
              const bool conditional =
                  body->findPlace(resultDrop->place)->root ==
                  lang::MirPlaceRootKind::Temporary;
              if (mutation >= 3 && !conditional)
                continue;
              for (auto &candidate : body->blocks)
                for (auto &instruction : candidate.instructions) {
                  if (mutation == 0 && instruction.id == producer.id)
                    instruction.successResultDrop.reset();
                  if (mutation == 1 &&
                      instruction.kind == lang::MirInstructionKind::Drop &&
                      !instruction.lifecycle.empty() &&
                      instruction.lifecycle.front().source ==
                          *producer.successResultDrop)
                    instruction.lifecycle.clear();
                  if (mutation == 4 &&
                      instruction.kind == lang::MirInstructionKind::Drop &&
                      !instruction.lifecycle.empty() &&
                      instruction.lifecycle.front().source ==
                          *producer.successResultDrop)
                    instruction.lifecycle.front().conditional = false;
                }
              if (mutation == 2) {
                auto *drop =
                    body->findDropObligation(*producer.successResultDrop);
                body->places[drop->place - 1].initiallyAvailable = true;
              }
              if (mutation == 3)
                body->places[resultDrop->place - 1].sourceValue = 0;
              if (mutation == 0 || mutation == 1 || mutation == 4)
                expect(
                    !lang::verifyMirProgram(malformed).valid(),
                    "observed temporary requires success-only publication and "
                    "exact cleanup");
              if (mutation != 1 && mutation != 4)
                expect(!lang::CppMirBodyEmitter(malformed, map)
                            .supportsFailureBodyText(address),
                       "observer storage proof must reject missing publication "
                       "or pre-live storage");
            }
          }
        }
      }
      expect(checked == 4,
             "both success and error temporary observers need exact storage");
    }
  }
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/owner_tied_by_value_argument.gti";
  std::ifstream input(path);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  expect(frontend.canGenerateCode(), "borrowed copied arguments lower");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  bool found = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &value : function.body.values) {
      const auto uses = function.body.usesOf(value.id);
      if (value.info.type.kind != lang::SemanticType::Class ||
          !value.info.traits.containsBorrowedState || uses.size() != 1 ||
          uses.front().kind != lang::MirValueUseKind::InstructionOperand ||
          uses.front().block == value.definitionBlock)
        continue;
      const auto *block = function.body.findBlock(value.definitionBlock);
      const auto producer =
          std::find_if(block->instructions.begin(), block->instructions.end(),
                       [&](const auto &instruction) {
                         return instruction.id == value.definition;
                       });
      if (producer == block->instructions.end() ||
          producer->kind != lang::MirInstructionKind::CallInput ||
          producer->callInputKind != lang::HirCallInputKind::CopyValue ||
          !producer->preparedParameterDrop || producer->operands.size() != 1 ||
          producer->operands.front().kind != lang::MirOperandKind::Copy)
        continue;
      found = true;
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                         .owner = function.id};
      expect(
          lang::CppMirBodyEmitter(frontend.mir, map)
              .supportsFailureBodyText(address),
          "copy before a later argument Invoke has a prepared-parameter slot");
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(malformed, address);
        for (auto &candidateBlock : body->blocks)
          for (auto &instruction : candidateBlock.instructions)
            if (instruction.id == producer->id) {
              if (mutation == 0)
                instruction.operands.front().place = 0;
              if (mutation == 1)
                instruction.operands.front().kind = lang::MirOperandKind::Move;
            }
        if (mutation == 2)
          for (const auto &candidate : malformed.classInstances())
            if (candidate.type == value.info.type)
              const_cast<lang::MirClassInstance &>(candidate)
                  .requiresActiveCleanup = true;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "copied argument requires exact Copy source and matching "
               "prepared cleanup before backend admission");
      }
      break;
    }
    if (found)
      break;
  }
  expect(found,
         "fixture must retain a prepared borrowed Copy across Invoke blocks");
}

void testNonFailingOwningDeclarationInitializer() {
  const auto frontend =
      analyzeWithStandardLibrary("non-failing-owning-default.gti", R"gti(
class Cleanup { public: Cleanup() {} ~Cleanup() {} };
class Holder {
  Cleanup cleanup = Cleanup();
public:
  int operator()(int amount) mut { return amount; }
};
interface Marker { int marker(); };
class Passive : public Marker {
public:
  Passive() {}
  int marker() override { return 1; }
};
class Mixed {
  int tag = 7;
  Passive value = Passive();
public:
  Mixed() {}
};
int main() { Mixed mixed{}; Holder value{}; return std::move(value)(3) - 3; }
)gti");
  expect(frontend.canGenerateCode(), "owning generated-default fixture lowers");
  if (!frontend.canGenerateCode())
    return;
  const lang::MirClassInstance *owner = nullptr;
  for (const auto &candidate : frontend.mir.classInstances())
    if (candidate.fieldInitializers.dropObligations.size() == 2)
      owner = &candidate;
  expect(owner != nullptr, "field default has value and rollback obligations");
  if (!owner)
    return;
  const lang::MirBodyAddress address{
      .kind = lang::MirBodyKind::FieldInitializers, .owner = owner->id};
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const auto schedule =
      lang::CppMirBodyEmitter(frontend.mir, map).initializerSchedule(address);
  expect(schedule.supported && schedule.fields.size() == 1 &&
             schedule.fields.front().spelling.empty() &&
             owner->fieldInitializers.failureRecords.empty(),
         "generated constructor owns the field default without duplicate "
         "native declaration execution");
  expect(!gti_test::emitCppText(frontend).empty(),
         "production emission accepts the generated constructor and consuming "
         "fallback");
  const lang::MirConstructorInstance *mixed = nullptr;
  for (const auto &constructor : frontend.mir.constructorInstances())
    if (constructor.initializers.size() == 2 &&
        constructor.initializers.front().targetType ==
            lang::SemanticType::Int32)
      mixed = &constructor;
  expect(mixed != nullptr, "mixed declaration-default constructor exists");
  if (mixed) {
    const lang::MirBodyAddress mixedAddress{lang::MirBodyKind::Constructor,
                                            mixed->id};
    const auto *entry = mixed->body.findBlock(mixed->body.entry);
    const auto *success =
        entry && entry->terminator.kind == lang::MirTerminatorKind::Invoke
            ? mixed->body.findBlock(entry->terminator.target)
            : nullptr;
    expect(lang::CppMirBodyEmitter(frontend.mir, map)
                   .supportsFailureBodyText(mixedAddress) &&
               entry && success && !success->instructions.empty() &&
               success->instructions.front().kind ==
                   lang::MirInstructionKind::Initialize &&
               success->instructions.front().constructorInitializer == 2 &&
               std::count_if(
                   entry->instructions.begin(), entry->instructions.end(),
                   [](const auto &i) {
                     return i.kind == lang::MirInstructionKind::Initialize &&
                            i.constructorInitializer == 1;
                   }) == 1,
           "scalar default precedes construction; passive field publication "
           "follows its exact Invoke success edge");
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto malformed = frontend.mir;
      auto &constructor = const_cast<lang::MirConstructorInstance &>(
          *malformed.findConstructorInstance(mixed->id));
      if (mutation == 0)
        std::swap(constructor.initializers[0], constructor.initializers[1]);
      if (mutation == 3 && entry) {
        auto &terminator = constructor.body.findBlock(entry->id)->terminator;
        terminator.kind = lang::MirTerminatorKind::Goto;
        terminator.invokeInstruction = 0;
        terminator.failureRecord = 0;
        terminator.elseTarget = 0;
      }
      for (auto &block : constructor.body.blocks)
        for (auto &instruction : block.instructions) {
          if (mutation == 1 && instruction.constructorInitializer == 1 &&
              instruction.kind == lang::MirInstructionKind::Initialize)
            instruction.operands.clear();
          if (mutation == 2 &&
              instruction.operation == lang::MirOperation::Literal)
            instruction.literal.reset();
        }
      expect(!lang::verifyMirProgram(malformed).valid(),
             "field order, exact publication, literal identity and the "
             "constructor failure edge remain mandatory");
    }
  }
  for (int mutation = 0; mutation != 4; ++mutation) {
    auto malformed = frontend.mir;
    auto *body = lang::findMirBody(malformed, address);
    for (auto &block : body->blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Construct) {
          if (mutation == 0)
            instruction.lifecycle.clear();
          if (mutation == 3)
            instruction.lifecycle.front().conditional = true;
        }
        if (mutation == 1 &&
            instruction.kind == lang::MirInstructionKind::Initialize)
          instruction.lifecycle.clear();
        if (mutation == 2 &&
            instruction.kind == lang::MirInstructionKind::Lifecycle &&
            !instruction.lifecycle.empty())
          instruction.lifecycle.clear();
      }
    }
    expect(!lang::CppMirBodyEmitter(malformed, map)
                .initializerSchedule(address)
                .supported,
           "missing activation, reparent, handoff or conditional activation "
           "cannot erase cleanup");
  }
}

void testGeneratedDefaultScheduleObligation() {
  const auto rejectOrdinarySubstitute = [](const lang::MirProgram &program,
                                           lang::IntrinsicKind intrinsic) {
    auto substituted = program;
    std::optional<lang::MirConstructorInstance> replacement;
    lang::MirBodyAddress selected;
    lang::MirInstructionId selectedInstruction = 0;
    for (const auto address : lang::enumerateMirBodyAddresses(substituted)) {
      auto *body = lang::findMirBody(substituted, address);
      if (!body || replacement)
        continue;
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions) {
          if (replacement || instruction.intrinsic != intrinsic ||
              !instruction.constructorTarget)
            continue;
          const auto *target = substituted.findConstructorInstance(
              *instruction.constructorTarget);
          if (!target || !target->generatedDefault)
            continue;
          replacement = *target;
          replacement->id = substituted.constructorInstances().size() + 1;
          replacement->generatedDefault = false;
          instruction.constructorTarget = replacement->id;
          selected = address;
          selectedInstruction = instruction.id;
        }
    }
    if (replacement)
      const_cast<std::vector<lang::MirConstructorInstance> &>(
          substituted.constructorInstances())
          .push_back(std::move(*replacement));
    const auto rejected = lang::verifyMirProgram(substituted);
    expect(replacement &&
               std::any_of(rejected.errors.begin(), rejected.errors.end(),
                           [&](const auto &issue) {
                             return issue.bodyKind == selected.kind &&
                                    issue.owner == selected.owner &&
                                    issue.instruction == selectedInstruction &&
                                    issue.message.find(
                                        "required generated schedule") !=
                                        std::string::npos;
                           }),
           "an ordinary constructor with a matching signature cannot replace "
           "an obligated generated-default target");
  };
  const auto frontend = analyze("generated-default-obligation.gti", R"gti(
class Box<T> {
  int marker = 1;
public:
  Box(int value) : marker(value) {}
};
int main() { Box<int> first{}; Box<bool> second{2}; return 0; }
)gti");
  expect(frontend.canGenerateCode(),
         "generic defaults retain an obligation without eagerly instantiating "
         "an unused default constructor");
  if (!frontend.canGenerateCode())
    return;
  const lang::MirClassInstance *unusedDefault = nullptr;
  for (const auto &owner : frontend.mir.classInstances()) {
    if (owner.type.arguments !=
        std::vector<lang::SemanticType>{lang::SemanticType::Bool})
      continue;
    unusedDefault = &owner;
  }
  expect(unusedDefault && unusedDefault->generatedDefaultRequiresSchedule &&
             std::none_of(frontend.mir.constructorInstances().begin(),
                          frontend.mir.constructorInstances().end(),
                          [&](const auto &constructor) {
                            return constructor.owner == unusedDefault->id &&
                                   constructor.generatedDefault;
                          }),
         "the uncalled specialization retains its semantic obligation without "
         "an executable generated body");
  if (!unusedDefault)
    return;
  expect(!gti_test::emitCppText(frontend).empty(),
         "generic declaration catalogs agree when only one specialization "
         "actually invokes its default");
  auto erased = frontend.mir;
  const_cast<lang::MirClassInstance *>(
      erased.findClassInstance(unusedDefault->id))
      ->generatedDefaultRequiresSchedule = false;
  expect(lang::verifyMirProgram(erased).valid() &&
             !lang::verifyMirOptimizationCoherence(frontend.mir, erased, {})
                  .valid(),
         "optimization replay preserves unused generated-default obligations "
         "as immutable class metadata");
  auto malformedHir = frontend.hir;
  const_cast<lang::HirClassInstance &>(
      malformedHir.classInstances()[unusedDefault->id - 1])
      .generatedDefaultRequiresSchedule = false;
  expect(!lang::verifyHirProgramPlans(frontend.semantics, malformedHir).valid(),
         "HIR cannot erase the semantic generated-default obligation");
  auto detached = gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  auto &detachedMir = const_cast<lang::MirProgram &>(detached.mir());
  const_cast<lang::MirClassInstance *>(
      detachedMir.findClassInstance(unusedDefault->id))
      ->generatedDefaultRequiresSchedule = false;
  const auto issues = lang::verifyLoweredProgram(detached);
  expect(
      std::any_of(issues.begin(), issues.end(),
                  [](const auto &issue) {
                    return issue.detail.find("generated-default obligation") !=
                           std::string::npos;
                  }),
      "detached declarations independently retain unused default obligations");

  const auto direct = analyze("generated-default-target-erasure.gti", R"gti(
struct Only { int value = 1; };
int main() { Only value{}; return 0; }
)gti");
  expect(direct.canGenerateCode() &&
             direct.mir.constructorInstances().size() == 1,
         "target-erasure control has one exact generated constructor");
  if (!direct.canGenerateCode() ||
      direct.mir.constructorInstances().size() != 1)
    return;
  rejectOrdinarySubstitute(direct.mir, lang::IntrinsicKind::None);
  auto missing = direct.mir;
  bool erasedTarget = false;
  for (const auto address : lang::enumerateMirBodyAddresses(missing)) {
    auto *body = lang::findMirBody(missing, address);
    if (!body)
      continue;
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.constructorTarget) {
          instruction.constructorTarget.reset();
          instruction.definedFailure = {};
          erasedTarget = true;
        }
  }
  const_cast<std::vector<lang::MirConstructorInstance> &>(
      missing.constructorInstances())
      .clear();
  const auto invalid = lang::verifyMirProgram(missing);
  expect(erasedTarget &&
             std::any_of(invalid.errors.begin(), invalid.errors.end(),
                         [](const auto &issue) {
                           return issue.message.find(
                                      "missing its selected schedule") !=
                                  std::string::npos;
                         }),
         "erasing both target and generated body cannot restore native "
         "default construction");
  const std::string inPlaceSource = R"gti(
#include <std/memory>
struct Element { int value = 1; };
int main() {
  unsafe {
    Element* address = nullptr;
    std::construct_at(address);
  }
  mut std::detail::prefix_owner<Element> appended{uint64_t(1)};
  appended.append();
  mut std::detail::prefix_owner<Element> inserted{uint64_t(1)};
  inserted.insert(uint64_t(0), Element());
  return 0;
}
)gti";
  // Source containers and direct raw construction share the same exact
  // ObjectConstruct contract; no trusted fixture or container opcode exists.
  const auto inPlace = analyzeWithStandardLibrary(
      "generated-in-place-obligation.gti", inPlaceSource);
  if (!inPlace.canGenerateCode())
    for (const auto &diagnostic : inPlace.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(inPlace.canGenerateCode(),
         "raw and prefix default construction retain selected schedules");
  if (inPlace.canGenerateCode()) {
    for (const auto intrinsic : {lang::IntrinsicKind::ObjectConstruct}) {
      rejectOrdinarySubstitute(inPlace.mir, intrinsic);
      auto absent = inPlace.mir;
      bool changed = false;
      for (const auto address : lang::enumerateMirBodyAddresses(absent)) {
        auto *body = lang::findMirBody(absent, address);
        if (!body)
          continue;
        for (auto &block : body->blocks)
          for (auto &instruction : block.instructions)
            if (instruction.intrinsic == intrinsic &&
                instruction.constructorTarget) {
              instruction.constructorTarget.reset();
              instruction.definedFailure.propagation =
                  lang::FailurePropagationKind::None;
              changed = true;
            }
      }
      expect(changed && !lang::verifyMirProgram(absent).valid(),
             "in-place default construction cannot discard its required "
             "generated constructor target");
    }
  }

  const auto bases = analyze("source-constructor-default-obligation.gti", R"gti(
class Base { int value = 1; };
class Derived : public Base {
public:
  Derived() : Base() {}
};
class DeclaredBase { public: DeclaredBase() {} };
class DeclaredDerived : public DeclaredBase {
public:
  DeclaredDerived() : DeclaredBase() {}
};
int main() {
  Derived derived{};
  DeclaredDerived declared{};
  Base direct{};
  return 0;
}
)gti");
  expect(bases.canGenerateCode(),
         "source constructors preserve generated and explicit zero-argument "
         "base selection as distinct valid cases");
  if (!bases.canGenerateCode())
    return;
  bool mutatedBase = false;
  for (const auto &constructor : bases.mir.constructorInstances()) {
    if (constructor.generatedDefault)
      continue;
    for (std::size_t index = 0; index < constructor.initializers.size();
         ++index) {
      const auto &initializer = constructor.initializers[index];
      const auto *base = initializer.base
                             ? bases.mir.findClassInstance(*initializer.base)
                             : nullptr;
      if (initializer.kind != lang::ConstructorInitializerTargetKind::Base ||
          !base || !base->generatedDefaultRequiresSchedule)
        continue;
      auto malformed = bases.mir;
      auto &changed = const_cast<lang::MirConstructorInstance *>(
                          malformed.findConstructorInstance(constructor.id))
                          ->initializers[index];
      changed.generatedDefault = false;
      changed.constructorTarget.reset();
      const auto rejected = lang::verifyMirProgram(malformed);
      expect(std::any_of(
                 rejected.errors.begin(), rejected.errors.end(),
                 [&](const auto &issue) {
                   return issue.owner == constructor.id &&
                          issue.bodyKind == lang::MirBodyKind::Constructor &&
                          issue.message.find("required exact schedule") !=
                              std::string::npos;
                 }),
             "erasing a source constructor's base-default flag and target "
             "cannot bypass the exact base class obligation");
      mutatedBase = true;
    }
  }
  expect(mutatedBase, "the source-constructor base mutation must be exercised");

  const auto unrelated = std::find_if(
      bases.mir.constructorInstances().begin(),
      bases.mir.constructorInstances().end(), [](const auto &constructor) {
        return !constructor.generatedDefault &&
               constructor.parameterTypes.empty() &&
               constructor.initializers.empty();
      });
  auto wrongOwner = bases.mir;
  bool redirected = false;
  if (unrelated != bases.mir.constructorInstances().end()) {
    for (const auto address : lang::enumerateMirBodyAddresses(wrongOwner)) {
      auto *body = lang::findMirBody(wrongOwner, address);
      if (!body)
        continue;
      for (auto &block : body->blocks)
        for (auto &instruction : block.instructions) {
          const auto *target = instruction.constructorTarget
                                   ? wrongOwner.findConstructorInstance(
                                         *instruction.constructorTarget)
                                   : nullptr;
          if (instruction.kind != lang::MirInstructionKind::Construct ||
              !target || !target->generatedDefault)
            continue;
          instruction.constructorTarget = unrelated->id;
          redirected = true;
        }
    }
  }
  const auto rejectedOwner = lang::verifyMirProgram(wrongOwner);
  expect(redirected &&
             std::any_of(rejectedOwner.errors.begin(),
                         rejectedOwner.errors.end(),
                         [](const auto &issue) {
                           return issue.message.find(
                                      "result type differs from its exact "
                                      "constructor owner") != std::string::npos;
                         }),
         "an unrelated zero-argument constructor cannot evade the result "
         "class's generated-default schedule obligation");
}

void testGeneratedDeclarationFailureRollback() {
  const auto pointerDefault =
      analyzeWithStandardLibrary("generated-null-default.gti", R"gti(
using Callback = (int32_t) -> int32_t;
struct RawHolder {
  void* address = nullptr;
  c_string text = nullptr;
  Callback callback = nullptr;
};
int main() { RawHolder holder{}; return 0; }
)gti");
  expect(pointerDefault.canGenerateCode(),
         "generated declaration defaults preserve exact null-to-pointer, "
         "C-string, and native-function "
         "initialization");
  const auto scalarDefaults = analyze("generated-scalar-default.gti", R"gti(
int16_t small() { return 1; }
struct ScalarDefaults {
  double wide_float = small();
  float narrow_float = small();
  int64_t wide_integer = small();
  uint8_t typed_literal = 1;
  int8_t signed_literal = -1;
  uint8_t grouped_literal = (1);
};
int main() { ScalarDefaults value{}; return 0; }
)gti");
  expect(scalarDefaults.canGenerateCode(),
         "generated declaration defaults preserve existing scalar widening "
         "and contextually typed literals");
  if (scalarDefaults.canGenerateCode()) {
    auto malformed = scalarDefaults.hir;
    bool changedInteger = false;
    for (const auto &constructor : malformed.constructorInstances()) {
      for (const auto &initializer : constructor.initializers) {
        if (initializer.targetType != lang::SemanticType::Int64 ||
            initializer.arguments.size() != 1)
          continue;
        auto *value = const_cast<lang::HirValue *>(
            constructor.body.findValue(initializer.arguments.front()));
        if (value) {
          value->info.type = lang::SemanticType::Double;
          changedInteger = true;
          break;
        }
      }
    }
    expect(changedInteger &&
               !lang::verifyHirProgramPlans(scalarDefaults.semantics, malformed)
                    .valid(),
           "selected integer fields cannot acquire an implicit floating "
           "narrowing conversion");
  }
  const auto expectedDefaults = analyze("generated-expected-default.gti", R"gti(
class Payload<T> { int marker = 1; public: ~Payload() {} };
class Wrapped<T> {
  expected<Payload<T>, int> success = Payload<T>();
  expected<int, Payload<T>> error = unexpected(Payload<T>());
  expected<int, Payload<T>> passive = 1;
};
int main() { Wrapped<int> value{}; return 0; }
)gti");
  expect(expectedDefaults.canGenerateCode(),
         "generated expected defaults publish exact generic success, error, "
         "and passive alternatives with typed rollback");
  if (expectedDefaults.canGenerateCode()) {
    bool foundTypedTransfer = false;
    bool corruptedAlternative = false;
    auto wrongAlternative = expectedDefaults.mir;
    for (const auto &constructor :
         expectedDefaults.mir.constructorInstances()) {
      for (const auto &block : constructor.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Initialize ||
              instruction.info.type.kind != lang::SemanticType::Expected)
            continue;
          foundTypedTransfer |= instruction.lifecycle.size() == 2 &&
                                instruction.lifecycle[0].kind ==
                                    lang::MirLifecycleEventKind::TransferOut &&
                                instruction.lifecycle[1].kind ==
                                    lang::MirLifecycleEventKind::Initialize;
        }
      }
      for (const auto &drop : constructor.body.dropObligations) {
        if (drop.kind != lang::MirDropObligationKind::ConstructionRollback ||
            drop.dropType.type.kind != lang::SemanticType::Expected ||
            drop.dropType.aggregate.size() != 2)
          continue;
        auto *changed = const_cast<lang::MirConstructorInstance *>(
            wrongAlternative.findConstructorInstance(constructor.id));
        std::swap(
            changed->body.dropObligations[drop.id - 1].dropType.aggregate[0],
            changed->body.dropObligations[drop.id - 1].dropType.aggregate[1]);
        corruptedAlternative = true;
        break;
      }
      if (corruptedAlternative)
        break;
    }
    expect(foundTypedTransfer,
           "a payload publication transfers its lifetime and initializes the "
           "distinct expected field lifetime");
    expect(corruptedAlternative &&
               !lang::verifyMirProgram(wrongAlternative).valid(),
           "an expected field rollback cannot exchange success and error "
           "cleanup alternatives");
    auto wrongPayload = expectedDefaults.hir;
    bool changedPayload = false;
    for (const auto &constructor : wrongPayload.constructorInstances()) {
      for (const auto &initializer : constructor.initializers) {
        if (initializer.targetType.kind != lang::SemanticType::Expected ||
            initializer.targetType.arguments.front().kind !=
                lang::SemanticType::Class ||
            initializer.arguments.size() != 1)
          continue;
        auto *value = const_cast<lang::HirValue *>(
            constructor.body.findValue(initializer.arguments.front()));
        if (value) {
          value->info.type = lang::SemanticType(lang::SemanticType::Unexpected,
                                                {value->info.type});
          changedPayload = true;
          break;
        }
      }
      if (changedPayload)
        break;
    }
    expect(changedPayload && !lang::verifyHirProgramPlans(
                                  expectedDefaults.semantics, wrongPayload)
                                  .valid(),
           "a success payload cannot become an incompatible error alternative");
  }
  const auto genericDefaults =
      analyzeWithStandardLibrary("generated-generic-default.gti", R"gti(
#include <std/memory>
class GenericPointer<T> { std::unique_ptr<T> value = std::unique_ptr<T>(); };
int main() { GenericPointer<int> first{}; GenericPointer<bool> second{}; return 0; }
)gti");
  expect(genericDefaults.canGenerateCode(),
         "primary default expressions preserve concrete generic owner types");
  if (genericDefaults.canGenerateCode()) {
    bool changedEmptyOwner = false;
    auto wrongEmptyOwner = genericDefaults.hir;
    for (const auto &candidate : wrongEmptyOwner.constructorInstances()) {
      if (!candidate.generatedDefault)
        continue;
      for (const auto &initializer : candidate.initializers) {
        if (initializer.targetType.kind != lang::SemanticType::UniqueOwner ||
            initializer.arguments.size() != 1)
          continue;
        auto *value = const_cast<lang::HirValue *>(
            candidate.body.findValue(initializer.arguments.front()));
        if (value && value->info.type == lang::SemanticType::NullPtr) {
          value->info.type = lang::SemanticType::Int32;
          changedEmptyOwner = true;
          break;
        }
      }
      if (changedEmptyOwner)
        break;
    }
    expect(changedEmptyOwner && !lang::verifyHirProgramPlans(
                                     genericDefaults.semantics, wrongEmptyOwner)
                                     .valid(),
           "empty unique-owner defaults admit null, not arbitrary scalar "
           "initializers");
    bool changedTarget = false;
    auto malformed = genericDefaults.hir;
    for (const auto &candidate : malformed.constructorInstances()) {
      for (const auto &value : candidate.body.values) {
        if (!value.constructorTarget)
          continue;
        const auto *selected =
            malformed.findConstructorInstance(*value.constructorTarget);
        if (!selected || !selected->generatedDefault)
          continue;
        const auto &selectedOwner =
            malformed.classInstances()[selected->owner - 1];
        const auto other = std::find_if(
            malformed.constructorInstances().begin(),
            malformed.constructorInstances().end(), [&](const auto &target) {
              const auto &owner = malformed.classInstances()[target.owner - 1];
              return target.generatedDefault &&
                     owner.declaration == selectedOwner.declaration &&
                     owner.type != selectedOwner.type;
            });
        if (other != malformed.constructorInstances().end()) {
          const_cast<lang::HirValue &>(value).constructorTarget = other->id;
          changedTarget = true;
          break;
        }
      }
      if (changedTarget)
        break;
    }
    expect(changedTarget && !lang::verifyHirProgramPlans(
                                 genericDefaults.semantics, malformed)
                                 .valid(),
           "an unresolved primary cannot authorize a different concrete "
           "constructor owner than the selected value or storage");
  }
  const auto frontend =
      analyzeWithStandardLibrary("generated-declaration-failure.gti", R"gti(
class First { public: First() {} ~First() {} };
class Failing {
public:
  Failing() {
    int[1] values = {1};
    int index = 2;
    int failure = values[index];
  }
  ~Failing() {}
};
class Outer {
  First first = First();
  Failing second = Failing();
public:
  ~Outer() {}
};
int main() { Outer object{}; return 0; }
)gti");
  expect(frontend.canGenerateCode(), "generated defaults have concrete MIR");
  if (!frontend.canGenerateCode())
    return;
  const lang::HirConstructorInstance *generated = nullptr;
  for (const auto &constructor : frontend.hir.constructorInstances()) {
    const auto &owner = frontend.hir.classInstances()[constructor.owner - 1];
    if (constructor.generatedDefault && owner.source->name().lexeme == "Outer")
      generated = &constructor;
  }
  expect(generated && generated->initializers.size() == 2 &&
             generated->initializerValues.size() == 2,
         "the generated body contains both selected declaration defaults");
  if (!generated)
    return;
  const auto *constructor = frontend.mir.findConstructorInstance(generated->id);
  expect(
      constructor && constructor->mayRaiseDefinedFailure &&
          std::any_of(
              constructor->body.dropObligations.begin(),
              constructor->body.dropObligations.end(),
              [](const auto &drop) {
                return drop.kind ==
                           lang::MirDropObligationKind::ConstructionRollback &&
                       !drop.initiallyActive;
              }),
      "completed field publication arms explicit rollback, never entry-live "
      "or incomplete-object cleanup");
  expect(!gti_test::emitCppText(frontend).empty(),
         "production consumes the generated failure constructor MIR");
  for (const bool eraseGenerated : {false, true}) {
    auto malformed =
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
    auto &mir = const_cast<lang::MirProgram &>(malformed.mir());
    const auto selected = std::find_if(
        mir.constructorInstances().begin(), mir.constructorInstances().end(),
        [&](const auto &candidate) {
          return eraseGenerated ? candidate.id == generated->id
                                : !candidate.generatedDefault;
        });
    expect(selected != mir.constructorInstances().end(),
           "generated and ordinary constructor controls are both present");
    if (selected != mir.constructorInstances().end())
      const_cast<lang::MirConstructorInstance &>(*selected).generatedDefault =
          !eraseGenerated;
    expect(!lang::verifyLoweredProgram(malformed).empty(),
           "detached constructor catalog rejects erased or forged generated "
           "identity independently of body arity");
  }
  for (int mutation = 0; mutation != 5; ++mutation) {
    auto malformed = frontend.hir;
    auto *changed = const_cast<lang::HirConstructorInstance *>(
        malformed.findConstructorInstance(generated->id));
    if (mutation == 0)
      changed->initializers.erase(changed->initializers.begin());
    else if (mutation == 1)
      std::swap(changed->initializers[0], changed->initializers[1]);
    else if (mutation == 2)
      changed->initializers[1].arguments = changed->initializers[0].arguments;
    else if (mutation == 3)
      changed->body.roots.push_back(999999);
    else
      changed->initializerValues.clear();
    expect(!lang::verifyHirProgramPlans(frontend.semantics, malformed).valid(),
           "generated field schedule cannot omit, reorder or substitute "
           "semantic defaults or add executable roots");
  }
  for (int mutation = 0; mutation != 3; ++mutation) {
    auto malformed = frontend.mir;
    auto *changed = const_cast<lang::MirConstructorInstance *>(
        malformed.findConstructorInstance(generated->id));
    bool mutated = false;
    for (auto &block : changed->body.blocks) {
      for (auto &instruction : block.instructions) {
        if (!mutated && mutation == 0 &&
            instruction.kind == lang::MirInstructionKind::Initialize &&
            !instruction.lifecycle.empty()) {
          instruction.lifecycle.clear();
          mutated = true;
        }
        if (!mutated && mutation == 1 &&
            instruction.kind == lang::MirInstructionKind::Drop &&
            !instruction.lifecycle.empty() &&
            instruction.lifecycle.front().failureCleanup) {
          instruction.lifecycle.clear();
          mutated = true;
        }
      }
    }
    if (mutation == 2) {
      for (auto &drop : changed->body.dropObligations) {
        if (drop.kind == lang::MirDropObligationKind::ConstructionRollback) {
          drop.initiallyActive = true;
          mutated = true;
          break;
        }
      }
    }
    expect(mutated && !lang::verifyMirProgram(malformed).valid(),
           "field activation, failure cleanup and unconstructed-state "
           "authority are independently checked");
  }
  const auto inherited = analyze("generated-base-field-default.gti", R"gti(
class Base { public: Base() {} ~Base() {} };
class Derived : public Base {
  int value = 3;
public:
  ~Derived() {}
};
int main() { Derived value{}; return 0; }
)gti");
  expect(inherited.canGenerateCode(),
         "generated default schedule composes exact base and declared fields");
  if (!inherited.canGenerateCode())
    return;
  for (const auto &candidate : inherited.mir.constructorInstances()) {
    if (!candidate.generatedDefault || candidate.initializers.size() != 2)
      continue;
    for (int mutation = 0; mutation != 5; ++mutation) {
      auto malformed = inherited.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(candidate.id));
      if (mutation == 0)
        changed->initializers.erase(changed->initializers.begin());
      else if (mutation == 1)
        changed->initializers.insert(changed->initializers.begin(),
                                     candidate.initializers.front());
      else if (mutation == 2)
        changed->initializers.front().base = candidate.owner;
      else if (mutation == 3)
        changed->initializers.front().targetType = lang::SemanticType::Int32;
      else
        changed->initializers.front().constructorTarget = candidate.id;
      expect(!lang::verifyMirProgram(malformed).valid(),
             "generated base schedule rejects omitted, duplicate, substituted "
             "or incorrectly targeted base initialization");
    }
  }
}

void testGeneratedOwningStringFieldConstructor() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-body-owning-string-field-constructor.gti", R"(
#include <std/string>

class StringHolder {
public:
  std::size_t size() {
    return text.size() + shadow.size();
  }

private:
  mut std::string text = std::string("");
  mut std::string shadow = std::string("");
};

int main() {
  mut StringHolder value = StringHolder();
  return value.size() == 0 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning string-field fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirClassInstance *owner = nullptr;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    const lang::ClassTypeInfo *info =
        frontend.semantics.findClassType(instance.declaration);
    if (info != nullptr && info->declaration != nullptr &&
        info->declaration->name().lexeme == "StringHolder") {
      owner = &instance;
      break;
    }
  }
  expect(owner != nullptr,
         "the string-field fixture should retain its concrete MIR owner");
  if (owner == nullptr) {
    return;
  }

  const lang::MirInstruction *fieldConstruct = nullptr;
  std::size_t fieldConstructs = 0;
  for (const lang::MirBlock &block : owner->fieldInitializers.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::Construct ||
          !instruction.constructorTarget) {
        continue;
      }
      ++fieldConstructs;
      if (fieldConstruct == nullptr) {
        fieldConstruct = &instruction;
      }
    }
  }
  const lang::MirConstructorInstance *fieldConstructor =
      fieldConstruct == nullptr ? nullptr
                                : frontend.mir.findConstructorInstance(
                                      *fieldConstruct->constructorTarget);
  expect(fieldConstructs == 2 && fieldConstruct != nullptr &&
             fieldConstructor != nullptr &&
             fieldConstructor->mayRaiseDefinedFailure,
         "both std::string field constructions should retain their exact "
         "checked constructor target");
  if (fieldConstructs != 2 || fieldConstruct == nullptr ||
      fieldConstructor == nullptr) {
    return;
  }
  std::size_t rollbackFailures = 0;
  for (const auto &record : owner->fieldInitializers.failureRecords) {
    const auto *block =
        owner->fieldInitializers.findBlock(record.producerBlock);
    if (block == nullptr)
      continue;
    for (const auto &instruction : block->instructions) {
      if (instruction.id != record.producerInstruction ||
          instruction.kind != lang::MirInstructionKind::Drop)
        continue;
      const auto *place =
          instruction.destination
              ? owner->fieldInitializers.findPlace(*instruction.destination)
              : nullptr;
      expect(place && place->root == lang::MirPlaceRootKind::Binding &&
                 place->projections.empty() &&
                 place->symbol == owner->declaredFields.front().symbol &&
                 instruction.definedFailure.propagation ==
                     lang::FailurePropagationKind::Destructor &&
                 block->terminator.kind == lang::MirTerminatorKind::Invoke &&
                 block->terminator.invokeInstruction == instruction.id,
             "rollback of the first source-owned field retains its own "
             "checked destructor failure edge");
      ++rollbackFailures;
    }
  }
  expect(owner->fieldInitializers.dropObligations.size() == 4 &&
             owner->fieldInitializers.failureRecords.size() == 3 &&
             rollbackFailures == 1 &&
             owner->fieldInitializers.cleanupBoundaries.size() == 1 &&
             std::any_of(
                 owner->fieldInitializers.blocks.begin(),
                 owner->fieldInitializers.blocks.end(),
                 [](const lang::MirBlock &block) {
                   return std::any_of(
                       block.instructions.begin(), block.instructions.end(),
                       [](const lang::MirInstruction &instruction) {
                         return instruction.kind ==
                                    lang::MirInstructionKind::Drop &&
                                instruction.lifecycle.size() == 1 &&
                                instruction.lifecycle.front().failureCleanup;
                       });
                 }),
         "the second owning field should retain exact rollback MIR for the "
         "first completed field");

  lang::CppMirBodyEmissionMapRows rows =
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23);
  const auto contained = std::find_if(
      rows.containedConstructors.begin(), rows.containedConstructors.end(),
      [&](const lang::CppMirContainedConstructorRepresentation &row) {
        return row.constructor == fieldConstructor->id;
      });
  expect(contained != rows.containedConstructors.end() &&
             contained->ownerType == fieldConstruct->info.type &&
             contained->parameterTypes == fieldConstructor->parameterTypes,
         "the representation row builder should inventory the exact contained "
         "std::string constructor representation");
  if (contained == rows.containedConstructors.end()) {
    return;
  }

  lang::CppMirBodyEmissionMapRows duplicateRows = rows;
  duplicateRows.containedConstructors.push_back(*contained);
  const lang::CppMirBodyEmissionMap duplicateMap(std::move(duplicateRows));
  const lang::CppMirProgramEmissionAnalysis duplicateAnalysis =
      lang::CppMirBodyEmitter(frontend.mir, duplicateMap).analyzeProgram();
  expect(std::any_of(duplicateAnalysis.issues.begin(),
                     duplicateAnalysis.issues.end(),
                     [](const lang::CppMirBodyEmissionIssue &issue) {
                       return issue.kind ==
                              lang::CppMirBodyEmissionIssueKind::
                                  DuplicateContainedConstructorRepresentation;
                     }),
         "a duplicate contained-constructor row should invalidate the copied "
         "representation map");

  const lang::CppMirBodyEmissionMap map(rows);
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::CppMirInitializerScheduleText schedule =
      emitter.initializerSchedule(
          {.kind = lang::MirBodyKind::FieldInitializers, .owner = owner->id});
  expect(
      schedule.supported && schedule.fields.size() == 2 &&
          std::all_of(schedule.fields.begin(), schedule.fields.end(),
                      [&](const lang::CppMirFieldInitializerSpelling &field) {
                        return field.spelling.empty();
                      }),
      "both defaults execute in generated constructor MIR, never terminal "
      "containment before its rollback scope");

  std::erase_if(rows.containedConstructors,
                [&](const lang::CppMirContainedConstructorRepresentation &row) {
                  return row.constructor == fieldConstructor->id;
                });
  const lang::CppMirBodyEmissionMap missingRowMap(std::move(rows));
  const lang::CppMirBodyEmitter missingRowEmitter(frontend.mir, missingRowMap);
  expect(missingRowEmitter
             .initializerSchedule({.kind = lang::MirBodyKind::FieldInitializers,
                                   .owner = owner->id})
             .supported,
         "unused declaration-only containment rows are not execution authority "
         "for generated constructor field defaults");

  const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
  const auto generated = std::find_if(
      frontend.mir.constructorInstances().begin(),
      frontend.mir.constructorInstances().end(), [&](const auto &candidate) {
        return candidate.owner == owner->id && candidate.generatedDefault;
      });
  expect(generated != frontend.mir.constructorInstances().end() &&
             generated->mayRaiseDefinedFailure &&
             artifact.contents.find(lang::cppMirFailureConstructorTagSpelling(
                 fieldConstructor->id)) != std::string::npos &&
             artifact.contents.find(lang::cppMirFailureConstructorTagSpelling(
                 generated->id)) != std::string::npos &&
             artifact.contents.find("gti_rt_failure_terminate_v2") !=
                 std::string::npos,
         "the C++ artifact emits the selected outer and field failure "
         "constructors, preserving first failure through rollback");
}

void testBodyOrderedOwnedParameterConstructor() {
  for (const bool declaredDestructor : {false, true}) {
    std::string source = R"(
#include <std/string>

class OrderedPath {
public:
  OrderedPath(std::string value)
      : first(value.clone()), second(std::move(value)) {
    first.append("!");
  }
  // destructor-stage

  std::size_t first_size() {
    return first.size();
  }

  std::size_t second_size() {
    return second.size();
  }

private:
  mut std::string first = std::string("");
  mut std::string second = std::string("");
};

int main() {
  mut OrderedPath path = OrderedPath(std::string("hello"));
  if (path.first_size() != 6 || path.second_size() != 5) {
    return 1;
  }
  return 0;
}
)";
    if (declaredDestructor) {
      const std::string marker = "// destructor-stage";
      source.replace(source.find(marker), marker.size(), "~OrderedPath() {}");
    }
    const lang::FrontendResult frontend = analyzeWithStandardLibrary(
        "cpp-mir-body-ordered-owned-parameter-constructor.gti", source);
    expect(frontend.canGenerateCode(),
           "the body-ordered owned-parameter constructor should pass the "
           "frontend");
    if (!frontend.canGenerateCode()) {
      return;
    }

    const lang::MirConstructorInstance *constructor = nullptr;
    for (const lang::MirConstructorInstance &candidate :
         frontend.mir.constructorInstances()) {
      if (candidate.initializers.size() == 2 &&
          !candidate.initializers.front().ownedParameter &&
          candidate.initializers.back().ownedParameter) {
        constructor = &candidate;
        break;
      }
    }
    expect(constructor != nullptr && constructor->mayRaiseDefinedFailure,
           "the ordered constructor should retain its clone, move, and body "
           "failure schedule");
    if (constructor == nullptr) {
      return;
    }

    expect(
        lang::cppMirFailureConstructorEmptyFieldsEligible(frontend.mir,
                                                          constructor->owner),
        "the constructor should have proved empty fields for body publication");
    expect(lang::cppMirFailureConstructorEmptyStateEligible(frontend.mir,
                                                            constructor->owner),
           "the private empty representation supports generated fields and "
           "an independently verified disarmed source destructor");
    if (declaredDestructor) {
      lang::MirProgram malformed = frontend.mir;
      auto &classes = const_cast<std::vector<lang::MirClassInstance> &>(
          malformed.classInstances());
      for (lang::MirClassInstance &owner : classes) {
        if (owner.id == constructor->owner) {
          owner.requiresActiveDropState = false;
        }
      }
      expect(
          !lang::cppMirFailureConstructorEmptyFieldsEligible(
              malformed, constructor->owner),
          "empty fields with a source destructor require the lifecycle guard");
      expect(!lang::cppMirFailureConstructorEmptyStateEligible(
                 malformed, constructor->owner),
             "private empty representation cannot activate an unguarded "
             "source destructor");
    }

    const lang::CppMirBodyEmissionMap map(
        buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                       .owner = constructor->id};
    expect(emitter.analyze(address).ready() &&
               emitter.supportsFailureBodyText(address),
           "the transformed constructor should preserve body-ordered clone and "
           "owned-parameter publication");
    if (!emitter.supportsFailureBodyText(address)) {
      return;
    }

    const std::string text =
        emitter
            .emitFailureBodyText(
                address, "body-ordered-owned-parameter-constructor-v0", 1)
            .text;
    const std::size_t clone = text.find("clone__gti_mir_failure");
    const std::size_t parameterMove =
        text.find("std::move(__gti_mir_p_", clone);
    expect(clone != std::string::npos && parameterMove != std::string::npos &&
               clone < parameterMove &&
               text.find("// GTI MIR field rollback") != std::string::npos &&
               text.find(".template __gti_mir_unique_cleanup_failure<") !=
                   std::string::npos &&
               text.find("construction rollback deferred") == std::string::npos,
           "the emitted constructor should clone before moving the parameter "
           "and retain exact later-failure rollback");

    const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
    expect(!artifact.contents.empty() &&
               artifact.contents.find("mir_failure_constructor_tag_v1") !=
                   std::string::npos &&
               artifact.contents.find("mir_empty_state_v1") !=
                   std::string::npos,
           "production emission should build the transformed initializer list "
           "without dereferencing absent native owned-field bindings");

    lang::MirProgram mismatchedReceiver = frontend.mir;
    auto &constructors =
        const_cast<std::vector<lang::MirConstructorInstance> &>(
            mismatchedReceiver.constructorInstances());
    bool mutated = false;
    for (lang::MirConstructorInstance &candidate : constructors) {
      if (candidate.id != constructor->id) {
        continue;
      }
      for (lang::MirBlock &block : candidate.body.blocks) {
        for (lang::MirInstruction &instruction : block.instructions) {
          if (instruction.kind == lang::MirInstructionKind::CallInput &&
              instruction.callInputRole == lang::MirCallInputRole::Receiver) {
            instruction.callInputRole = lang::MirCallInputRole::Argument;
            mutated = true;
            break;
          }
        }
        if (mutated) {
          break;
        }
      }
    }
    expect(mutated,
           "the near-miss fixture should expose the clone receiver stage");
    if (mutated) {
      const lang::CppMirBodyEmitter mismatchedEmitter(mismatchedReceiver, map);
      expect(!mismatchedEmitter.supportsFailureBodyText(address),
             "member-call field publication should fail closed when its exact "
             "receiver schedule is absent");
    }
  }
}

void testSourceDestructorOwningFieldCleanup() {
  for (const auto name : {"source_destructor_cleanup_failure.gti",
                          "temporary_constructor_argument_user_drop_gap.gti"}) {
    const auto fixture =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
    std::ifstream input(fixture);
    expect(input.good(), "source-destructor cleanup fixture must exist");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
    expect(frontend.canGenerateCode(), "source-destructor fixture must verify");
    if (!frontend.canGenerateCode())
      return;
    const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    bool checkedReceiver = false;
    for (const auto &constructor : frontend.mir.constructorInstances()) {
      const auto owned = std::find_if(
          constructor.initializers.begin(), constructor.initializers.end(),
          [&](const auto &initializer) {
            return initializer.ownedParameter &&
                   std::any_of(frontend.mir.classInstances().begin(),
                               frontend.mir.classInstances().end(),
                               [&](const auto &owner) {
                                 return owner.type == initializer.targetType &&
                                        owner.destructor;
                               });
          });
      if (owned == constructor.initializers.end())
        continue;
      const auto nativeBindings =
          lang::cppMirOwnedParameterFieldBindings(frontend.mir, constructor);
      const auto ownedCount = std::count_if(
          constructor.initializers.begin(), constructor.initializers.end(),
          [](const auto &initializer) {
            return initializer.ownedParameter.has_value();
          });
      expect(nativeBindings &&
                 nativeBindings->size() == static_cast<std::size_t>(ownedCount),
             "the native prefix must cover every owned field exactly once");
      if (nativeBindings) {
        for (const auto &initializer : constructor.initializers) {
          if (!initializer.ownedParameter)
            continue;
          const auto binding = std::find_if(
              nativeBindings->begin(), nativeBindings->end(),
              [&](const auto &candidate) {
                return candidate.field == initializer.field &&
                       candidate.parameter == *initializer.ownedParameter;
              });
          expect(binding != nativeBindings->end() &&
                     binding->initializeInstruction != 0 &&
                     binding->parameterDrop != 0 && binding->valueDrop != 0 &&
                     binding->fieldDrop != 0 &&
                     binding->fieldTransferInstruction != 0,
                 "each native field binding must preserve parameter, moved "
                 "value, rollback and successful transfer identities");
        }
      }
      const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                         .owner = constructor.id};
      const auto text =
          emitter.emitFailureBodyText(address, "source-fields-v0", 1);
      const bool hasFailurePath = !constructor.body.failureRecords.empty();
      const auto *owner = frontend.mir.findClassInstance(constructor.owner);
      const auto *ownerInfo =
          owner == nullptr
              ? nullptr
              : frontend.semantics.findClassType(owner->declaration);
      if (ownerInfo != nullptr && ownerInfo->declaration != nullptr &&
          ownerInfo->declaration->name().lexeme == "Receiver") {
        checkedReceiver = true;
        expect(hasFailurePath,
               "the Receiver control must retain executable failure rollback");
      }
      expect(
          text.emitted() &&
              text.text.find(" = std::move(__gti_mir_v_") ==
                  std::string::npos &&
              text.text.find("field initialization publishes in the member") !=
                  std::string::npos &&
              (!hasFailurePath ||
               text.text.find("__gti_mir_unique_cleanup_failure<") !=
                   std::string::npos),
          "the native prefix consumes each MIR publication without a second "
          "move and retains source-destructor rollback on failure paths");
    }
    if (std::string_view(name) ==
        "temporary_constructor_argument_user_drop_gap.gti")
      expect(checkedReceiver,
             "receiver regression must check its owned field publication");
    const auto artifact = gti_test::emitCpp(frontend);
    expect(artifact.contents.find(
               "if (!sequence.step(__gti_mir_destruct_failure(") !=
               std::string::npos,
           "source cleanup must execute before its aggregate children");
    if (std::string_view(name) == "source_destructor_cleanup_failure.gti") {
      expect(
          artifact.contents.find("Nested::Nested() {") == std::string::npos &&
              artifact.contents.find("Nested::Nested(::gti_internal::backend::"
                                     "mir_failure_constructor_tag_v1") !=
                  std::string::npos,
          "nested construction cannot emit a plain call to a status-only "
          "owning constructor");
    }
  }
}

void testOwningBaseConstructorStatus() {
  for (const auto fixtureName :
       {"inherited_unique_allocation.gti", "generated_base_constructor.gti",
        "generated_base_owner_constructor.gti",
        "polymorphic_owning_base_constructor.gti"}) {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures" / fixtureName;
    std::ifstream input(fixture);
    expect(input.good(), "the inherited-allocation fixture must exist");
    const std::string source{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
    expect(frontend.canGenerateCode(), "the owning-base fixture must verify");
    if (!frontend.canGenerateCode())
      return;
    for (const auto &generated : frontend.hir.constructorInstances()) {
      if (!generated.generatedDefault)
        continue;
      const auto &owner = frontend.hir.classInstances()[generated.owner - 1];
      const auto *lifecycle =
          owner.source == nullptr
              ? nullptr
              : frontend.semantics.findClassLifecycle(*owner.source);
      if (lifecycle && lifecycle->generatedDefaultFieldInitializers)
        continue;
      expect(
          generated.declaration == 0 && generated.source == nullptr &&
              generated.initializers.size() == 1,
          "generated construction needs a selected body without synthetic AST");
      for (int mutation = 0; mutation != 7; ++mutation) {
        auto malformed = frontend.hir;
        auto *changed = const_cast<lang::HirConstructorInstance *>(
            malformed.findConstructorInstance(generated.id));
        if (mutation == 0)
          changed->generatedDefault = false;
        else if (mutation == 1)
          changed->initializers.front().constructorTarget = generated.id;
        else if (mutation == 2)
          changed->initializers.front().base = 999999;
        else if (mutation == 3)
          changed->initializers.front().targetType = lang::SemanticType::Int32;
        else if (mutation == 4)
          changed->body.roots.push_back(999999);
        else if (mutation == 5)
          changed->owner = 999999;
        else
          changed->declaration = 999999;
        expect(
            !lang::verifyHirProgramPlans(frontend.semantics, malformed).valid(),
            "generated construction must retain its exact semantic selection");
      }
      bool erasedCall = false;
      auto malformed = frontend.hir;
      for (const auto &function : malformed.functionInstances()) {
        auto *changed = const_cast<lang::HirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        for (auto &value : changed->body.values) {
          if (value.constructorTarget == generated.id) {
            value.constructorTarget.reset();
            value.definedFailure.propagation =
                lang::FailurePropagationKind::None;
            erasedCall = true;
          }
        }
      }
      expect(erasedCall &&
                 !lang::verifyHirProgramPlans(frontend.semantics, malformed)
                      .valid(),
             "generated construction cannot erase both target and propagation "
             "into a native default call");
    }
    std::size_t selected = 0;
    for (const auto &constructor : frontend.mir.constructorInstances()) {
      const auto *constructorOwner =
          frontend.mir.findClassInstance(constructor.owner);
      if (constructorOwner && constructorOwner->polymorphic &&
          constructorOwner->destructor) {
        const lang::CppMirBodyEmissionMap map(
            buildRows(frontend, frontend.mir));
        const lang::MirBodyAddress address{
            .kind = lang::MirBodyKind::Constructor, .owner = constructor.id};
        const lang::CppMirBodyEmitter emitter(frontend.mir, map);
        expect(emitter.supportsFailureBodyText(address),
               "an abstract or concrete polymorphic owner retains guarded "
               "source cleanup during status construction");
        const auto text =
            emitter.emitFailureBodyText(address, "guarded-owning-base-v1", 1);
        const auto guard = "(*this).__gti_lifecycle_active_" +
                           std::to_string(constructorOwner->declaration);
        expect(text.emitted() &&
                   text.text.find(guard + " = false;") != std::string::npos &&
                   text.text.find(guard + " = true;") != std::string::npos,
               "each declaring class disarms its own destructor until success");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto forged = frontend.mir;
          auto *changed = const_cast<lang::MirClassInstance *>(
              forged.findClassInstance(constructor.owner));
          if (mutation == 0)
            changed->requiresActiveDropState = false;
          else if (mutation == 1)
            changed->destructorStatus = lang::SpecialMemberStatus::Generated;
          else
            changed->fieldDropOrder.clear();
          expect(!lang::CppMirBodyEmitter(forged, map)
                      .supportsFailureBodyText(address),
                 "polymorphic rollback must retain its guard, declared "
                 "destructor identity and exact reverse field cleanup");
        }
      }
      const auto *entry = constructor.body.findBlock(constructor.body.entry);
      if (!entry || entry->instructions.empty() ||
          entry->instructions.front().kind !=
              lang::MirInstructionKind::InitializeBase)
        continue;
      ++selected;
      const auto &stage = entry->instructions.front();
      const auto bindings = lang::cppMirBaseParameterInitializerBindings(
          frontend.mir, constructor);
      expect(bindings && bindings->size() == 1 &&
                 bindings->front().parameters.empty() &&
                 bindings->front().constructor == stage.constructorTarget &&
                 bindings->front().initializer + 1 ==
                     stage.constructorInitializer,
             "zero-argument base initialization must retain its exact native "
             "binding");
      for (const auto standard :
           {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
        const lang::CppMirBodyEmissionMap map(
            buildRows(frontend, frontend.mir, standard));
        const lang::CppMirBodyEmitter emitter(frontend.mir, map);
        const lang::MirBodyAddress address{
            .kind = lang::MirBodyKind::Constructor, .owner = constructor.id};
        expect(emitter.supportsFailureBodyText(address) &&
                   !emitter.supportsBodyText(address),
               "base initialization must consume its MIR failure edge, not a "
               "terminal plain call");
        const auto text =
            emitter.emitFailureBodyText(address, "owning-base-test-v0", 1);
        expect(
            text.emitted() &&
                text.text.find(" = *__gti_mir_constructor_success;") !=
                    std::string::npos &&
                text.text.find("if (__gti_mir_construct_success_" +
                               std::to_string(stage.id) + ")") !=
                    std::string::npos &&
                text.text.find("auto &__gti_mir_p_" +
                               std::to_string(*stage.destination) + " =") ==
                    std::string::npos &&
                text.text.find("mir_rollback_field_v1(") == std::string::npos,
            "the base must use its status edge without a field alias or field "
            "replacement");
        auto malformed = frontend.mir;
        auto &constructors =
            const_cast<std::vector<lang::MirConstructorInstance> &>(
                malformed.constructorInstances());
        for (auto &candidate : constructors) {
          if (candidate.id == constructor.id)
            candidate.body.blocks[candidate.body.entry - 1]
                .instructions.front()
                .successResultDrop.reset();
        }
        expect(!lang::CppMirBodyEmitter(malformed, map)
                    .supportsFailureBodyText(address),
               "base status emission must reject missing rollback activation");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto forged = frontend.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              forged.findConstructorInstance(constructor.id));
          if (mutation == 0)
            changed->initializers.front().base = 999999;
          else if (mutation == 1)
            changed->body.dropObligations[*stage.successResultDrop - 1]
                .dropType.requiresActiveCleanup =
                !changed->body.dropObligations[*stage.successResultDrop - 1]
                     .dropType.requiresActiveCleanup;
          else
            changed->body.blocks[changed->body.entry - 1]
                .terminator.successLifecycle.clear();
          expect(!lang::verifyMirProgram(forged).valid(),
                 "base identity, cleanup requirement and success activation "
                 "are exact");
        }
      }
    }
    expect(selected == 1,
           "the fixture must exercise exactly one owning-base stage");
  }
}

void testBodyOrderedPassivePolicyConstructor() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/owned_constructor_policy_forwarding.gti";
  std::ifstream input(fixture);
  expect(input.good(), "the passive policy forwarding fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "an owning constructor may move a passive policy after allocation");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "nested owning construction must preserve body-ordered policy moves");
  const lang::MirConstructorInstance *constructor = nullptr;
  for (const auto &candidate : frontend.mir.constructorInstances()) {
    if (candidate.initializers.size() == 2 &&
        !candidate.initializers.front().ownedParameter &&
        candidate.initializers.back().ownedParameter)
      constructor = &candidate;
  }
  expect(constructor != nullptr, "the fixture must have a late policy move");
  if (constructor == nullptr)
    return;
  expect(!lang::cppMirOwnedParameterFieldBindings(frontend.mir, *constructor),
         "the passive policy must not move before the earlier allocation");
  for (int mutation = 0; mutation != 3; ++mutation) {
    auto malformed = frontend.mir;
    auto *changed = const_cast<lang::MirConstructorInstance *>(
        malformed.findConstructorInstance(constructor->id));
    const auto argument = changed->initializers.back().arguments.front();
    bool applied = false;
    for (auto &block : changed->body.blocks) {
      for (auto &instruction : block.instructions) {
        if (mutation < 2 &&
            instruction.kind == lang::MirInstructionKind::Move &&
            instruction.hirValue == argument) {
          if (mutation == 0)
            instruction.ownership.reset();
          else
            instruction.lifecycle.push_back({});
          applied = true;
        }
        if (mutation == 2 && instruction.fullExpressionEnd != 0) {
          const auto expression = std::find_if(
              changed->body.fullExpressions.begin(),
              changed->body.fullExpressions.end(), [&](const auto &candidate) {
                return candidate.id == instruction.fullExpressionEnd;
              });
          if (expression != changed->body.fullExpressions.end() &&
              expression->constructorInitializer == 2) {
            instruction.fullExpressionEnd = 0;
            applied = true;
          }
        }
      }
    }
    expect(applied, "each passive publication mutation must change its proof");
    const lang::CppMirBodyEmitter malformedEmitter(malformed, map);
    expect(!malformedEmitter.analyzeProgram().ready(),
           "missing ownership/boundary or invented lifecycle must fail closed");
  }
}

void testPassiveOwnedParameterConstructorFields() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-passive-owned-parameter-constructor.gti", R"(
struct Pair<T, U> {
  mut T first;
  mut U second;

  Pair(T first_value, U second_value)
      : first(std::move(first_value)), second(std::move(second_value)) {}
};

int main() {
  mut Pair<int32_t, int64_t> value =
      Pair<int32_t, int64_t>(int32_t(3), int64_t(4));
  return value.first + int32_t(value.second) - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the passive owned-parameter constructor should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirConstructorInstance *constructor = nullptr;
  for (const lang::MirConstructorInstance &candidate :
       frontend.mir.constructorInstances()) {
    if (candidate.parameterTypes ==
            std::vector<lang::SemanticType>{lang::SemanticType::Int32,
                                            lang::SemanticType::Int64} &&
        candidate.initializers.size() == 2 &&
        std::all_of(candidate.initializers.begin(),
                    candidate.initializers.end(),
                    [](const lang::MirConstructorInitializer &initializer) {
                      return initializer.ownedParameter.has_value();
                    })) {
      constructor = &candidate;
      break;
    }
  }
  expect(constructor != nullptr && constructor->mayRaiseDefinedFailure,
         "the concrete generic constructor should retain both conservative "
         "owned-parameter initializers");
  if (constructor == nullptr) {
    return;
  }

  const auto bindings =
      lang::cppMirOwnedParameterFieldBindings(frontend.mir, *constructor);
  expect(bindings && bindings->size() == 2 &&
             std::all_of(
                 bindings->begin(), bindings->end(),
                 [](const lang::CppMirOwnedParameterFieldBinding &binding) {
                   return binding.sourcePlace != 0 && binding.movedValue != 0 &&
                          binding.moveInstruction != 0 &&
                          binding.initializeInstruction != 0 &&
                          binding.dropInstruction == 0 &&
                          binding.parameterDrop == 0;
                 }),
         "passive moved parameters should bind both fields while proving the "
         "absence of lexical drop records");
  if (!bindings || bindings->empty()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                     .owner = constructor->id};
  expect(emitter.supportsFailureBodyText(address) &&
             emitter.analyzeProgram().ready(),
         "the exact passive move schedule should make the constructor and "
         "its caller ready for C++ text emission");

  const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
  expect(!artifact.contents.empty() &&
             artifact.contents.find("first(std::move(__gti_mir_arg_0))") !=
                 std::string::npos &&
             artifact.contents.find("second(std::move(__gti_mir_arg_1))") !=
                 std::string::npos,
         "production emission should publish both passive fields directly "
         "from their moved constructor parameters");

  lang::MirProgram malformed = frontend.mir;
  auto &constructors = const_cast<std::vector<lang::MirConstructorInstance> &>(
      malformed.constructorInstances());
  auto changed =
      std::find_if(constructors.begin(), constructors.end(),
                   [&](const lang::MirConstructorInstance &candidate) {
                     return candidate.id == constructor->id;
                   });
  if (changed != constructors.end()) {
    auto &places =
        const_cast<std::vector<lang::MirPlace> &>(changed->body.places);
    auto source = std::find_if(
        places.begin(), places.end(), [&](const lang::MirPlace &place) {
          return place.id == bindings->front().sourcePlace;
        });
    if (source != places.end()) {
      source->traits.drop = lang::DropKind::Lexical;
    }
  }
  const lang::MirConstructorInstance *changedConstructor =
      malformed.findConstructorInstance(constructor->id);
  expect(changedConstructor != nullptr &&
             !lang::cppMirOwnedParameterFieldBindings(malformed,
                                                      *changedConstructor),
         "a lexical parameter with no drop schedule should still fail closed");
}

// A move followed by an invoke leaves the moved shell's drop in the invoke's
// continuation blocks. The body proof follows invoke edges back to the move;
// other edges, merges, the entry, re-initialization on the success edge and
// cycles stay conservative. Lowering and verification share this proof, so a
// destructor that may fail never gains an edge for a moved-from shell.
void testCrossBlockDefiniteMoveRetiresShell() {
  const auto instruction = [](lang::MirInstructionId id,
                              lang::MirInstructionKind kind,
                              std::vector<lang::MirLifecycleEvent> events) {
    lang::MirInstruction result;
    result.id = id;
    result.kind = kind;
    result.lifecycle = std::move(events);
    return result;
  };
  const auto block = [](lang::MirBlockId id,
                        std::vector<lang::MirInstruction> instructions,
                        lang::MirTerminator terminator) {
    lang::MirBlock result;
    result.id = id;
    result.instructions = std::move(instructions);
    result.terminator = std::move(terminator);
    result.reachable = true;
    return result;
  };
  lang::MirBody proof;
  proof.entry = 1;
  proof.blocks = {
      block(1,
            {instruction(1, lang::MirInstructionKind::Move,
                         {{.kind = lang::MirLifecycleEventKind::Move,
                           .source = 1,
                           .target = 2}}),
             instruction(2, lang::MirInstructionKind::Construct, {})},
            {.kind = lang::MirTerminatorKind::Invoke,
             .invokeInstruction = 2,
             .failureRecord = 1,
             .target = 2,
             .elseTarget = 3}),
      block(2,
            {instruction(
                3, lang::MirInstructionKind::Drop,
                {{.kind = lang::MirLifecycleEventKind::Drop, .source = 1}})},
            {.kind = lang::MirTerminatorKind::Return}),
      block(3,
            {instruction(4, lang::MirInstructionKind::Drop,
                         {{.kind = lang::MirLifecycleEventKind::Drop,
                           .source = 1,
                           .failureCleanup = true}})},
            {.kind = lang::MirTerminatorKind::PropagateFailure,
             .failureRecord = 1})};
  const auto moved = [](const lang::MirBody &body, lang::MirBlockId block,
                        lang::MirDropObligationId obligation,
                        lang::MirInstructionId before) {
    return lang::mirDropFollowsDefiniteMove(body, *body.findBlock(block),
                                            obligation, before);
  };
  expect(moved(proof, 2, 1, 3) && moved(proof, 3, 1, 4) &&
             moved(proof, 1, 1, 2) &&
             !lang::mirDropFollowsDefiniteMove(*proof.findBlock(2), 1, 3),
         "both continuations of an invoke after a move must retire the "
         "moved-from shell, which the same-block proof cannot see");
  expect(!moved(proof, 1, 1, 1) && !moved(proof, 2, 0, 3) &&
             !moved(proof, 2, 1, 99) && !moved(proof, 2, 2, 3),
         "the entry state, a missing obligation, an unknown instruction and "
         "an obligation that was the move's target are not moved shells");
  for (int mutation = 0; mutation < 5; ++mutation) {
    lang::MirBody changed = proof;
    bool success = false;
    bool failure = false;
    switch (mutation) {
    case 0:
      // A successful invoke re-initializes the obligation on its success
      // edge only; the failure continuation still sees the move.
      changed.blocks[0].terminator.successLifecycle.push_back(
          {.kind = lang::MirLifecycleEventKind::Initialize, .target = 1});
      failure = true;
      break;
    case 1:
      changed.blocks[0].instructions.front().lifecycle.front().conditional =
          true;
      break;
    case 2:
      // The failure route rejoining the success block makes it a merge.
      changed.blocks[2].terminator = {.kind = lang::MirTerminatorKind::Goto,
                                      .target = 2};
      failure = true;
      break;
    case 3:
      changed.blocks[0].instructions.front().tombstone = true;
      break;
    case 4:
      // A block entered by a Goto may still gain a loop back edge after
      // lowering appends its drop, so only invoke edges carry the proof.
      changed.blocks[0].terminator = {.kind = lang::MirTerminatorKind::Goto,
                                      .target = 2};
      break;
    }
    expect(moved(changed, 2, 1, 3) == success &&
               moved(changed, 3, 1, 4) == failure,
           "re-initialization, conditional or tombstoned moves, merges and "
           "non-invoke edges must keep the drop's destructor edge");
  }
  lang::MirBody cycle;
  cycle.entry = 1;
  cycle.blocks = {
      block(1, {}, {.kind = lang::MirTerminatorKind::Return}),
      block(2,
            {instruction(
                1, lang::MirInstructionKind::Drop,
                {{.kind = lang::MirLifecycleEventKind::Drop, .source = 1}})},
            {.kind = lang::MirTerminatorKind::Goto, .target = 3}),
      block(3, {}, {.kind = lang::MirTerminatorKind::Goto, .target = 2})};
  expect(!moved(cycle, 2, 1, 1),
         "an untouched predecessor cycle must stop without a proof");

  // End to end: the moved by-value parameter is retired after the field
  // construction's invoke without a fallible destructor edge, even though
  // its type's destructor can fail. Restoring that edge breaks verification.
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cross-block-moved-parameter.gti", R"(
#include <std/utility>
class Pool {
  int32_t live;
public:
  Pool() : live(0) {}
  void release(int32_t slot) { int32_t checked = this.live + slot; }
};
class Lease {
  Pool& pool;
  mut int32_t slot;
public:
  Lease(Pool& pool) : pool(pool), slot(0) {}
  Lease(Lease&& other) = default;
  ~Lease() {
    if (this.slot != 0) { this.pool.release(this.slot); }
  }
};
class Buffer {
  Lease lease;
public:
  Buffer(Lease lease) : lease(std::move(lease)) {}
};
class Slots {
  Buffer buffer;
  mut int32_t length;
public:
  Slots(Lease lease) : buffer(Buffer(std::move(lease))), length(0) {}
};
int main() {
  Pool pool{};
  Slots slots{Lease(pool)};
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the moved-parameter constructor chain should reach MIR");
  std::size_t crossBlockDrops = 0;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    for (const lang::MirBlock &home : constructor.body.blocks) {
      for (const lang::MirInstruction &drop : home.instructions) {
        if (drop.kind != lang::MirInstructionKind::Drop ||
            drop.lifecycle.size() != 1)
          continue;
        const lang::MirDropObligation *obligation =
            constructor.body.findDropObligation(drop.lifecycle.front().source);
        if (obligation == nullptr ||
            obligation->kind != lang::MirDropObligationKind::Binding ||
            !lang::mirDropMayRaiseDefinedFailure(frontend.mir,
                                                 obligation->dropType) ||
            lang::mirDropFollowsDefiniteMove(home, obligation->id, drop.id) ||
            !lang::mirDropFollowsDefiniteMove(constructor.body, home,
                                              obligation->id, drop.id))
          continue;
        ++crossBlockDrops;
        expect(drop.definedFailure.empty(),
               "a moved-from parameter shell must not invoke its fallible "
               "destructor");
        lang::MirProgram malformed = frontend.mir;
        auto &constructors =
            const_cast<std::vector<lang::MirConstructorInstance> &>(
                malformed.constructorInstances());
        for (auto &candidate : constructors) {
          if (candidate.id != constructor.id)
            continue;
          for (auto &changedBlock : candidate.body.blocks)
            for (auto &changedDrop : changedBlock.instructions)
              if (changedDrop.id == drop.id)
                changedDrop.definedFailure.propagation =
                    lang::FailurePropagationKind::Destructor;
        }
        const lang::MirVerificationResult verified =
            lang::verifyMirProgram(malformed);
        expect(std::any_of(verified.errors.begin(), verified.errors.end(),
                           [](const lang::MirVerificationError &issue) {
                             return issue.message.find("drop propagation") !=
                                    std::string::npos;
                           }),
               "a destructor edge on a moved-from shell must fail the shared "
               "propagation rule");
      }
    }
  }
  expect(crossBlockDrops >= 2,
         "the moved parameter's success and failure retirements should both "
         "follow the invoke that split their block");
}

void testMoveOnlyClassResultReceiverFieldTransfer() {
  lang::MirBlock proof;
  lang::MirInstruction move;
  move.id = 1;
  move.kind = lang::MirInstructionKind::Move;
  move.lifecycle.push_back(
      {.kind = lang::MirLifecycleEventKind::Move, .source = 1, .target = 2});
  lang::MirInstruction drop;
  drop.id = 2;
  drop.kind = lang::MirInstructionKind::Drop;
  drop.lifecycle.push_back(
      {.kind = lang::MirLifecycleEventKind::Drop, .source = 1});
  proof.instructions = {move, drop};
  expect(lang::mirDropFollowsDefiniteMove(proof, 1, 2) &&
             !lang::mirDropFollowsDefiniteMove(proof, 2, 2) &&
             !lang::mirDropFollowsDefiniteMove(proof, 1) &&
             !lang::mirDropFollowsDefiniteMove(proof, 0, 2) &&
             !lang::mirDropFollowsDefiniteMove(proof, 1, 99),
         "moved-shell proof must name the exact still-moved source before "
         "its retirement");
  for (int mutation = 0; mutation < 6; ++mutation) {
    lang::MirBlock changed = proof;
    auto &instruction = changed.instructions.front();
    auto &event = instruction.lifecycle.front();
    switch (mutation) {
    case 0:
      event.conditional = true;
      break;
    case 1:
      event.kind = lang::MirLifecycleEventKind::Reparent;
      break;
    case 2:
      event.kind = lang::MirLifecycleEventKind::TransferOut;
      break;
    case 3:
      instruction.tombstone = true;
      break;
    case 4:
      instruction.lifecycle.push_back(
          {.kind = lang::MirLifecycleEventKind::Initialize, .target = 1});
      break;
    case 5:
      event.kind = lang::MirLifecycleEventKind::Replace;
      event.target = 1;
      break;
    }
    expect(!lang::mirDropFollowsDefiniteMove(changed, 1, 2),
           "conditional, retired, tombstoned, or reinitialized owners must "
           "not acquire a moved-shell proof");
  }

  const lang::FrontendResult classFields =
      analyzeWithStandardLibrary("cpp-mir-class-owned-parameter-fields.gti", R"(
#include <std/memory>
class Passive {
public:
  int32_t value;
  Passive() : value(0) {}
  Passive(int32_t initial) : value(initial) {}
  Passive(Passive& other) = delete;
  Passive(Passive&& other) = default;
};
class Active {
public:
  int32_t value;
  Active() : value(0) {}
  Active(int32_t initial) : value(initial) {}
  Active(Active& other) = delete;
  Active(Active&& other) = default;
  ~Active() { std::println(this.value); }
};
struct Node<T> {
  mut T value;
  mut std::unique_ptr<Node<T>> next = std::unique_ptr<Node<T>>();
  Node() requires std::default_initializable<T> : value(T()) {}
  Node(T initial, std::unique_ptr<Node<T>> following)
      : value(std::move(initial)), next(std::move(following)) {}
};
int main() {
  auto passive = std::make_unique<Node<Passive>>(
      Passive(1), std::unique_ptr<Node<Passive>>());
  auto active = std::make_unique<Node<Active>>(
      Active(2), std::unique_ptr<Node<Active>>());
  auto defaultPassive = std::make_unique<Node<Passive>>();
  auto defaultActive = std::make_unique<Node<Active>>();
  return 0;
}
)");
  expect(classFields.canGenerateCode(),
         "mixed and cleanup-owning node constructors should reach MIR");
  std::size_t classFieldConstructors = 0;
  for (const lang::MirConstructorInstance &constructor :
       classFields.mir.constructorInstances()) {
    if (constructor.parameterTypes.size() != 2 ||
        constructor.parameterTypes.front().kind != lang::SemanticType::Class ||
        constructor.initializers.size() != 2) {
      continue;
    }
    const auto bindings =
        lang::cppMirOwnedParameterFieldBindings(classFields.mir, constructor);
    expect(bindings && bindings->size() == 2,
           "the field-order proof must accept passive class moves and a "
           "shared transfer of two active field obligations");
    if (!bindings || bindings->size() != 2) {
      continue;
    }
    ++classFieldConstructors;
    const auto &first = bindings->front();
    const auto &second = bindings->back();
    bool sawMovedDrop = false;
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.id == first.dropInstruction && first.fieldDrop != 0) {
          sawMovedDrop = true;
          expect(lang::mirDropFollowsDefiniteMove(block, first.parameterDrop,
                                                  instruction.id) &&
                     instruction.definedFailure.empty(),
                 "a definitely moved constructor parameter must retire its "
                 "shell without a fallible source-destructor edge");
        }
      }
    }
    if (first.fieldDrop == 0) {
      expect(first.initializeInstruction != 0 && first.parameterDrop != 0 &&
                 second.initializeInstruction != 0,
             "a passive lexical class publishes through an explicit Initialize "
             "without inventing an active field obligation");
    } else {
      expect(sawMovedDrop,
             "the active class parameter must retain its shell drop");
      expect(first.fieldDrop != second.fieldDrop &&
                 first.fieldTransferInstruction ==
                     second.fieldTransferInstruction,
             "distinct active field obligations may share one transfer record");
    }
    for (int mutation = 0; mutation < 2; ++mutation) {
      lang::MirProgram malformed = classFields.mir;
      auto &constructors =
          const_cast<std::vector<lang::MirConstructorInstance> &>(
              malformed.constructorInstances());
      for (auto &candidate : constructors) {
        if (candidate.id != constructor.id) {
          continue;
        }
        for (auto &block : candidate.body.blocks) {
          for (auto &instruction : block.instructions) {
            if (instruction.id != second.fieldTransferInstruction) {
              continue;
            }
            for (auto &event : instruction.lifecycle) {
              if (event.source != second.fieldDrop) {
                continue;
              }
              if (mutation == 0) {
                event.conditional = true;
              } else {
                event.source = first.parameterDrop;
              }
            }
          }
        }
        expect(
            !lang::cppMirOwnedParameterFieldBindings(malformed, candidate),
            "conditional or retargeted batch field transfer must fail closed");
      }
    }
  }
  expect(classFieldConstructors == 2,
         "both concrete node-constructor families need an exact field proof");
  const lang::CppMirBodyEmissionMap defaultMap(
      buildRows(classFields, classFields.mir));
  const lang::CppMirBodyEmitter defaultEmitter(classFields.mir, defaultMap);
  std::size_t defaultFieldConstructors = 0;
  for (const auto &constructor : classFields.mir.constructorInstances()) {
    if (!constructor.parameterTypes.empty() ||
        constructor.initializers.size() != 2 ||
        constructor.initializers.front().targetType.kind !=
            lang::SemanticType::Class) {
      continue;
    }
    const auto *owner = classFields.mir.findClassInstance(constructor.owner);
    std::vector<const lang::MirInstruction *> publications;
    std::vector<const lang::MirInstruction *> producers;
    std::vector<lang::MirDropObligationId> rollbackDrops;
    bool exactStages = owner != nullptr && owner->declaredFields.size() == 2;
    for (std::size_t index = 0; index < constructor.initializers.size();
         ++index) {
      const auto &initializer = constructor.initializers[index];
      const lang::MirInstruction *publication = nullptr;
      for (const auto &block : constructor.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.constructorInitializer != index + 1)
            continue;
          exactStages &= publication == nullptr;
          publication = &instruction;
        }
      }
      const auto *place =
          publication && publication->destination
              ? constructor.body.findPlace(*publication->destination)
              : nullptr;
      const auto *value =
          publication && publication->operands.size() == 1 &&
                  publication->operands.front().kind ==
                      lang::MirOperandKind::Value
              ? constructor.body.findValue(publication->operands.front().value)
              : nullptr;
      const lang::MirInstruction *producer = nullptr;
      for (const auto &block : constructor.body.blocks)
        for (const auto &instruction : block.instructions)
          if (value && instruction.id == value->definition)
            producer = &instruction;
      const auto *target = producer && producer->constructorTarget
                               ? classFields.mir.findConstructorInstance(
                                     *producer->constructorTarget)
                               : nullptr;
      const auto *targetOwner =
          target ? classFields.mir.findClassInstance(target->owner) : nullptr;
      exactStages &=
          publication && producer && target && targetOwner && place &&
          publication->kind == lang::MirInstructionKind::Initialize &&
          producer->kind == lang::MirInstructionKind::Construct &&
          initializer.arguments.size() == 1 &&
          producer->hirValue == initializer.arguments.front() &&
          value->sourceValue == initializer.arguments.front() &&
          targetOwner->type == initializer.targetType &&
          target->parameterTypes.empty() &&
          place->root == lang::MirPlaceRootKind::This &&
          place->projections.size() == 1 &&
          place->projections.front().kind == lang::MirProjectionKind::Field &&
          place->projections.front().field == initializer.field &&
          place->type == initializer.targetType && owner &&
          index < owner->declaredFields.size() &&
          owner->declaredFields[index].symbol == initializer.field;
      lang::MirDropObligationId rollback = 0;
      if (publication) {
        for (const auto &event : publication->lifecycle) {
          if (event.kind == lang::MirLifecycleEventKind::Reparent)
            rollback = event.target;
        }
      }
      const auto *drop = constructor.body.findDropObligation(rollback);
      if (owner && index < owner->declaredFields.size() &&
          owner->declaredFields[index].requiresActiveCleanup)
        exactStages &=
            drop && !drop->initiallyActive && place &&
            drop->kind == lang::MirDropObligationKind::ConstructionRollback &&
            drop->place == place->id &&
            drop->dropType.type == initializer.targetType;
      if (index == 1)
        exactStages &= target && target->generatedDefault && rollback != 0;
      publications.push_back(publication);
      producers.push_back(producer);
      rollbackDrops.push_back(rollback);
    }
    expect(exactStages,
           "both defaults retain exact concrete constructor targets, field "
           "publication, and success-armed typed rollback");
    if (!exactStages) {
      continue;
    }
    ++defaultFieldConstructors;
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                       .owner = constructor.id};
    const auto text =
        defaultEmitter.emitFailureBodyText(address, "default-fields-v0", 1);
    expect(lang::cppMirConstructedFieldInitializerBindings(classFields.mir,
                                                           constructor)
                   .empty() &&
               text.emitted() &&
               text.text.find("in-place constructor field reparent") !=
                   std::string::npos &&
               text.text.find("field initialization publishes in the member") ==
                   std::string::npos &&
               publications.front()->id < producers.back()->id,
           "selected generated defaults retain body-ordered publication, "
           "not an assumed native-prologue fusion");
    for (int mutation = 0; mutation < 3; ++mutation) {
      lang::MirProgram malformed = classFields.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      bool mutated = false;
      if (mutation == 0) {
        for (auto &block : changed->body.blocks) {
          for (auto &instruction : block.instructions) {
            if (instruction.id == producers.front()->id) {
              instruction.callSite = instruction.hirValue + 10000;
              mutated = true;
            }
          }
        }
      } else if (mutation == 1) {
        for (auto &drop : changed->body.dropObligations) {
          if (drop.id == rollbackDrops.back()) {
            drop.initiallyActive = true;
            mutated = true;
          }
        }
      } else {
        for (auto &block : changed->body.blocks) {
          for (auto &instruction : block.instructions) {
            if (instruction.id == producers.back()->id) {
              instruction.constructorTarget = constructor.id;
              mutated = true;
            }
          }
        }
      }
      const lang::CppMirBodyEmitter malformedEmitter(malformed, defaultMap);
      expect(mutated && (!lang::verifyMirProgram(malformed).valid() ||
                         !malformedEmitter.supportsFailureBodyText(address)),
             "forged call identity, premature cleanup, and a substituted "
             "concrete constructor must fail verification or body admission");
    }
  }
  expect(defaultFieldConstructors == 2,
         "both passive and destructor-bearing defaults need an exact proof");
  if (classFieldConstructors == 2) {
    expect(!gti_test::emitCpp(classFields).contents.empty(),
           "mixed and active node constructors must have production emission");
  }

  const auto observableDefault =
      analyzeWithStandardLibrary("cpp-mir-observable-default-order.gti", R"(
#include <std/memory>
mut int32_t order = 0;
std::unique_ptr<int32_t> make_next() {
  order = order * 10 + 2;
  return std::unique_ptr<int32_t>();
}
class Payload {
public:
  Payload() { order = order * 10 + 1; }
  ~Payload() {}
};
class Ordered {
  Payload value;
  std::unique_ptr<int32_t> next = make_next();
public:
  Ordered() : value(Payload()) {}
};
int main() { Ordered value{}; return order - 12; }
)");
  expect(observableDefault.canGenerateCode(),
         "an observable later default remains valid source construction");
  bool checkedObservableDefault = false;
  if (observableDefault.canGenerateCode()) {
    const lang::CppMirBodyEmissionMap map(
        buildRows(observableDefault, observableDefault.mir));
    const lang::CppMirBodyEmitter emitter(observableDefault.mir, map);
    for (const auto &constructor :
         observableDefault.mir.constructorInstances()) {
      if (!constructor.parameterTypes.empty() ||
          constructor.initializers.size() != 2)
        continue;
      checkedObservableDefault = true;
      lang::MirInstructionId firstPublication = 0;
      lang::MirInstructionId laterCall = 0;
      for (const auto &block : constructor.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.constructorInitializer == 1)
            firstPublication = instruction.id;
          if (instruction.kind == lang::MirInstructionKind::Call &&
              instruction.functionTarget)
            laterCall = instruction.id;
        }
      }
      expect(firstPublication != 0 && laterCall > firstPublication &&
                 lang::cppMirConstructedFieldInitializerBindings(
                     observableDefault.mir, constructor)
                     .empty() &&
                 emitter.supportsFailureBodyText(
                     {.kind = lang::MirBodyKind::Constructor,
                      .owner = constructor.id}),
             "an observable default call stays after the first field's "
             "publication and cannot enter native-prologue fusion");
    }
    expect(!gti_test::emitCpp(observableDefault).contents.empty(),
           "observable body-ordered defaults retain production emission");
  }
  expect(checkedObservableDefault,
         "the observable-default negative fusion control must be exercised");

  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-move-only-result-field-transfer.gti", R"(
#include <std/memory>

class Node {
public:
  mut int32_t value;
  mut std::unique_ptr<Node> next = std::unique_ptr<Node>();

  Node(int32_t initial, std::unique_ptr<Node> following)
      : value(std::move(initial)), next(std::move(following)) {}
};

class List {
  mut std::unique_ptr<Node> head = std::unique_ptr<Node>();

public:
  List() {}

  void push(int32_t value) mut {
    this.head =
        std::make_unique<Node>(std::move(value), std::move(this.head));
  }

  int32_t front() { return this.head->value; }
};

int main() {
  mut List values = List();
  values.push(1);
  return values.front() - 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the unique-owner replacement fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirConstructorInstance *nodeConstructor = nullptr;
  for (const lang::MirConstructorInstance &candidate :
       frontend.mir.constructorInstances()) {
    if (candidate.parameterTypes.size() == 2 &&
        candidate.parameterTypes.front() == lang::SemanticType::Int32 &&
        candidate.parameterTypes.back().kind == lang::SemanticType::Class &&
        candidate.initializers.size() == 2 &&
        std::all_of(candidate.initializers.begin(),
                    candidate.initializers.end(),
                    [](const lang::MirConstructorInitializer &initializer) {
                      return initializer.ownedParameter.has_value();
                    })) {
      nodeConstructor = &candidate;
      break;
    }
  }
  const auto constructorBindings =
      nodeConstructor == nullptr
          ? std::optional<std::vector<lang::CppMirOwnedParameterFieldBinding>>{}
          : lang::cppMirOwnedParameterFieldBindings(frontend.mir,
                                                    *nodeConstructor);
  expect(nodeConstructor != nullptr && constructorBindings &&
             constructorBindings->size() == 2 &&
             constructorBindings->front().parameterDrop == 0 &&
             constructorBindings->front().valueDrop == 0 &&
             constructorBindings->front().fieldDrop == 0 &&
             constructorBindings->front().fieldTransferInstruction == 0 &&
             constructorBindings->back().parameterDrop != 0 &&
             constructorBindings->back().valueDrop != 0 &&
             constructorBindings->back().fieldDrop != 0 &&
             constructorBindings->back().fieldTransferInstruction != 0,
         "the mixed constructor should distinguish its passive field from "
         "the exact active Move/Reparent/TransferOut field schedule");

  const lang::MirFunctionInstance *push = nullptr;
  const lang::MirInstruction *store = nullptr;
  const lang::MirInstruction *resultDrop = nullptr;
  const lang::MirPlace *resultShell = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (push != nullptr ||
            instruction.kind != lang::MirInstructionKind::Assign ||
            !instruction.destination || instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Value ||
            instruction.operands.front().type.kind !=
                lang::SemanticType::Class) {
          continue;
        }
        const lang::MirPlace *destination =
            function.body.findPlace(*instruction.destination);
        if (destination == nullptr ||
            destination->root != lang::MirPlaceRootKind::This) {
          continue;
        }
        const lang::MirValue *assigned =
            function.body.findValue(instruction.operands.front().value);
        const lang::MirInstruction *producer = nullptr;
        if (assigned != nullptr) {
          for (const lang::MirBlock &candidateBlock : function.body.blocks) {
            for (const lang::MirInstruction &candidate :
                 candidateBlock.instructions) {
              if (candidate.id == assigned->definition) {
                producer = &candidate;
              }
            }
          }
        }
        const lang::MirFunctionInstance *factory =
            producer != nullptr && producer->functionTarget
                ? frontend.mir.findFunctionInstance(*producer->functionTarget)
                : nullptr;
        const bool allocationFactory =
            factory != nullptr &&
            std::any_of(factory->body.blocks.begin(),
                        factory->body.blocks.end(),
                        [](const lang::MirBlock &candidateBlock) {
                          return std::any_of(
                              candidateBlock.instructions.begin(),
                              candidateBlock.instructions.end(),
                              [](const lang::MirInstruction &candidate) {
                                return candidate.intrinsic ==
                                       lang::IntrinsicKind::AllocateUniqueOwner;
                              });
                        });
        if (!allocationFactory) {
          continue;
        }
        const lang::MirPlace *shell = nullptr;
        for (const lang::MirPlace &place : function.body.places) {
          if (place.root == lang::MirPlaceRootKind::Value &&
              place.value == instruction.operands.front().value &&
              place.projections.empty()) {
            shell = &place;
          }
        }
        const lang::MirInstruction *drop = nullptr;
        if (shell != nullptr) {
          for (const lang::MirBlock &candidateBlock : function.body.blocks) {
            for (const lang::MirInstruction &candidate :
                 candidateBlock.instructions) {
              if (candidate.kind == lang::MirInstructionKind::Drop &&
                  candidate.destination == shell->id) {
                drop = &candidate;
              }
            }
          }
        }
        if (shell != nullptr && drop != nullptr) {
          push = &function;
          store = &instruction;
          resultShell = shell;
          resultDrop = drop;
        }
      }
    }
  }
  expect(push != nullptr && store != nullptr && resultShell != nullptr &&
             resultDrop != nullptr,
         "the replacement should retain its consuming field store and the "
         "moved-from result-shell drop");
  if (push == nullptr || store == nullptr || resultShell == nullptr ||
      resultDrop == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = push->id};
  const lang::CppMirBodyEmissionAnalysis bodyAnalysis =
      emitter.analyze(address);
  const lang::CppMirProgramEmissionAnalysis programAnalysis =
      emitter.analyzeProgram();
  const bool failureBody = emitter.supportsFailureBodyText(address);
  if (!failureBody || !bodyAnalysis.ready() || !programAnalysis.ready()) {
    for (const lang::CppMirBodyEmissionIssue &issue : bodyAnalysis.issues) {
      std::cerr << "move-only field body issue: " << issue.detail << '\n';
    }
    for (const lang::CppMirBodyEmissionIssue &issue : programAnalysis.issues) {
      std::cerr << "move-only field program issue: " << issue.detail << '\n';
    }
    std::cerr << "move-only field plain=" << emitter.supportsBodyText(address)
              << " failure=" << failureBody << '\n';
  }
  expect(failureBody && bodyAnalysis.ready() && programAnalysis.ready(),
         "the verified move-only class store should admit the transformed "
         "body");
  if (!failureBody) {
    return;
  }

  const std::string text =
      emitter
          .emitFailureBodyText(address,
                               "move-only-result-receiver-field-transfer-v0", 1)
          .text;
  const std::string destinationMove =
      "__gti_mir_p_" + std::to_string(*store->destination) +
      " = std::move(__gti_mir_p_" + std::to_string(resultShell->id) + ".get())";
  const std::string shellDestroy =
      "__gti_mir_p_" + std::to_string(resultShell->id) + ".destroy();";
  expect(text.find(destinationMove) != std::string::npos &&
             text.find(shellDestroy) != std::string::npos,
         "the field store should move from the placement result and destroy "
         "only its moved-from shell");

  const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
  expect(!artifact.contents.empty() &&
             artifact.contents.find(destinationMove) != std::string::npos &&
             artifact.contents.find(shellDestroy) != std::string::npos,
         "production emission should accept the same field publication");

  lang::MirProgram malformedStore = frontend.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      malformedStore.functionInstances());
  auto changed = std::find_if(functions.begin(), functions.end(),
                              [&](const lang::MirFunctionInstance &candidate) {
                                return candidate.id == push->id;
                              });
  bool forgedPublication = false;
  if (changed != functions.end()) {
    for (lang::MirBlock &block : changed->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == store->id && instruction.lifecycle.size() == 1) {
          instruction.lifecycle.front().conditional = true;
          forgedPublication = true;
        }
      }
    }
  }
  expect(forgedPublication,
         "the near-miss fixture should expose the field publication event");
  if (forgedPublication) {
    const lang::CppMirBodyEmitter malformedEmitter(malformedStore, map);
    expect(!malformedEmitter.supportsFailureBodyText(address),
           "a conditional move-only field publication should fail closed");
  }

  if (nodeConstructor != nullptr && constructorBindings &&
      !constructorBindings->empty()) {
    lang::MirProgram malformedConstructor = frontend.mir;
    auto &constructors =
        const_cast<std::vector<lang::MirConstructorInstance> &>(
            malformedConstructor.constructorInstances());
    auto changedConstructor =
        std::find_if(constructors.begin(), constructors.end(),
                     [&](const lang::MirConstructorInstance &candidate) {
                       return candidate.id == nodeConstructor->id;
                     });
    bool forgedTransfer = false;
    if (changedConstructor != constructors.end()) {
      for (lang::MirBlock &block : changedConstructor->body.blocks) {
        for (lang::MirInstruction &instruction : block.instructions) {
          if (instruction.id ==
                  constructorBindings->back().fieldTransferInstruction &&
              instruction.lifecycle.size() == 1) {
            instruction.lifecycle.front().conditional = true;
            forgedTransfer = true;
          }
        }
      }
    }
    const lang::MirConstructorInstance *malformedNode =
        malformedConstructor.findConstructorInstance(nodeConstructor->id);
    expect(forgedTransfer && malformedNode != nullptr &&
               !lang::cppMirOwnedParameterFieldBindings(malformedConstructor,
                                                        *malformedNode),
           "a conditional rollback transfer should invalidate the complete "
           "mixed-field constructor proof");
  }
}

void testMoveOnlyReceiverFieldLifecycleSchedules() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-forward-list-field-lifecycles.gti", R"(
#include <std/forward_list>
#include <std/memory>

class ResetOwner {
  mut std::unique_ptr<int32_t> value = std::unique_ptr<int32_t>();
public:
  void reset() mut { this.value = std::unique_ptr<int32_t>(); }
};

int main() {
  mut ResetOwner owner = ResetOwner();
  owner.reset();
  mut std::forward_list<int32_t> values = std::forward_list<int32_t>();
  values.push_front(1);
  values.pop_front();
  values.clear();
  return values.empty() ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the forward-list lifecycle fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  struct FieldTransfer {
    const lang::MirFunctionInstance *function = nullptr;
    const lang::MirInstruction *producer = nullptr;
    const lang::MirInstruction *consumer = nullptr;
    const lang::MirInstruction *destroy = nullptr;
    const lang::MirPlace *slot = nullptr;
    const lang::MirPlace *source = nullptr;
    lang::MirInstructionId successEndBorrow = 0;
  };
  FieldTransfer movedField;
  FieldTransfer defaultField;

  const auto definitionFor = [](const lang::MirBody &body,
                                lang::MirInstructionId id) {
    const lang::MirInstruction *found = nullptr;
    for (const lang::MirBlock &block : body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == id) {
          if (found != nullptr) {
            return static_cast<const lang::MirInstruction *>(nullptr);
          }
          found = &instruction;
        }
      }
    }
    return found;
  };

  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Assign ||
            !instruction.destination || instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Value ||
            instruction.operands.front().type.kind !=
                lang::SemanticType::Class) {
          continue;
        }
        const lang::MirPlace *destination =
            function.body.findPlace(*instruction.destination);
        const lang::MirValue *assigned =
            function.body.findValue(instruction.operands.front().value);
        const lang::MirInstruction *producer =
            assigned == nullptr
                ? nullptr
                : definitionFor(function.body, assigned->definition);
        if (destination == nullptr ||
            destination->root != lang::MirPlaceRootKind::This ||
            destination->projections.size() != 1 || producer == nullptr) {
          continue;
        }
        const lang::MirPlace *slot = nullptr;
        for (const lang::MirPlace &place : function.body.places) {
          if (place.root == lang::MirPlaceRootKind::Value &&
              place.value == assigned->id && place.projections.empty()) {
            if (slot != nullptr) {
              slot = nullptr;
              break;
            }
            slot = &place;
          }
        }
        const lang::MirInstruction *destroy = nullptr;
        if (slot != nullptr) {
          for (const lang::MirBlock &candidateBlock : function.body.blocks) {
            for (const lang::MirInstruction &candidate :
                 candidateBlock.instructions) {
              if (candidate.kind == lang::MirInstructionKind::Drop &&
                  candidate.destination == slot->id &&
                  candidate.lifecycle.size() == 1 &&
                  !candidate.lifecycle.front().failureCleanup) {
                if (destroy != nullptr) {
                  destroy = nullptr;
                  break;
                }
                destroy = &candidate;
              }
            }
          }
        }
        if (slot == nullptr || destroy == nullptr) {
          continue;
        }
        const auto *defaultConstructor =
            producer->constructorTarget ? frontend.mir.findConstructorInstance(
                                              *producer->constructorTarget)
                                        : nullptr;
        const bool generatedDefault =
            producer->kind == lang::MirInstructionKind::Construct &&
            defaultConstructor != nullptr &&
            defaultConstructor->generatedDefault &&
            defaultConstructor->parameterTypes.empty();
        if (((producer->kind == lang::MirInstructionKind::Call &&
              !producer->constructorTarget) ||
             generatedDefault) &&
            !producer->functionTarget && !producer->receiver &&
            producer->operands.empty() &&
            producer->intrinsic == lang::IntrinsicKind::None) {
          defaultField = {.function = &function,
                          .producer = producer,
                          .consumer = &instruction,
                          .destroy = destroy,
                          .slot = slot};
          continue;
        }
        const lang::MirPlace *source =
            producer->kind == lang::MirInstructionKind::Move &&
                    producer->operands.size() == 1
                ? function.body.findPlace(producer->operands.front().place)
                : nullptr;
        if (source == nullptr || source->root != lang::MirPlaceRootKind::Loan ||
            source->projections.size() != 1) {
          continue;
        }
        lang::MirInstructionId successEndBorrow = 0;
        for (const lang::MirBlock &candidateBlock : function.body.blocks) {
          const auto producerPosition =
              std::find_if(candidateBlock.instructions.begin(),
                           candidateBlock.instructions.end(),
                           [&](const lang::MirInstruction &candidate) {
                             return candidate.id == producer->id;
                           });
          if (producerPosition == candidateBlock.instructions.end()) {
            continue;
          }
          const lang::MirBlock *commitBlock = &candidateBlock;
          auto consumerPosition = std::next(producerPosition);
          if (consumerPosition != candidateBlock.instructions.end() &&
              consumerPosition->kind ==
                  lang::MirInstructionKind::CleanupBeforeAssign) {
            if (consumerPosition->replacementPair != instruction.id ||
                instruction.replacementPair != consumerPosition->id ||
                consumerPosition->destination != instruction.destination ||
                !consumerPosition->lifecycle.empty() ||
                !consumerPosition->replacementCleanup)
              continue;
            if (consumerPosition->definedFailure.propagation ==
                lang::FailurePropagationKind::Destructor) {
              if (std::next(consumerPosition) !=
                      candidateBlock.instructions.end() ||
                  candidateBlock.terminator.kind !=
                      lang::MirTerminatorKind::Invoke ||
                  candidateBlock.terminator.invokeInstruction !=
                      consumerPosition->id)
                continue;
              commitBlock =
                  function.body.findBlock(candidateBlock.terminator.target);
              if (commitBlock == nullptr)
                continue;
              consumerPosition = commitBlock->instructions.begin();
            } else {
              ++consumerPosition;
            }
          }
          if (consumerPosition == commitBlock->instructions.end() ||
              consumerPosition->id != instruction.id) {
            continue;
          }
          const auto endBorrow = std::find_if(
              std::next(consumerPosition), commitBlock->instructions.end(),
              [&](const lang::MirInstruction &candidate) {
                return candidate.kind == lang::MirInstructionKind::EndBorrow &&
                       candidate.loan && *candidate.loan == source->loan;
              });
          const auto drop =
              std::find_if(endBorrow, commitBlock->instructions.end(),
                           [&](const lang::MirInstruction &candidate) {
                             return candidate.id == destroy->id;
                           });
          if (endBorrow != commitBlock->instructions.end() &&
              drop != commitBlock->instructions.end() && endBorrow < drop) {
            successEndBorrow = endBorrow->id;
          }
        }
        movedField = {.function = &function,
                      .producer = producer,
                      .consumer = &instruction,
                      .destroy = destroy,
                      .slot = slot,
                      .source = source,
                      .successEndBorrow = successEndBorrow};
      }
    }
  }

  expect(movedField.function != nullptr && movedField.producer != nullptr &&
             movedField.consumer != nullptr && movedField.destroy != nullptr &&
             movedField.consumer->replacementPair != 0 &&
             movedField.slot != nullptr && movedField.source != nullptr &&
             movedField.successEndBorrow != 0,
         "pop_front should expose the exact borrowed-field move, replace, "
         "end-borrow, and shell-drop schedule");
  expect(defaultField.function != nullptr && defaultField.producer != nullptr &&
             defaultField.consumer != nullptr &&
             defaultField.destroy != nullptr && defaultField.slot != nullptr,
         "reset should expose the exact default-construction, receiver-field "
         "replace, and shell-drop schedule");
  if (movedField.function == nullptr || movedField.consumer == nullptr ||
      movedField.slot == nullptr || movedField.successEndBorrow == 0 ||
      defaultField.function == nullptr || defaultField.producer == nullptr ||
      defaultField.consumer == nullptr || defaultField.slot == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress movedAddress{.kind = lang::MirBodyKind::Function,
                                          .owner = movedField.function->id};
  const lang::MirBodyAddress defaultAddress{.kind = lang::MirBodyKind::Function,
                                            .owner = defaultField.function->id};
  expect(emitter.supportsFailureBodyText(movedAddress) &&
             emitter.supportsFailureBodyText(defaultAddress) &&
             emitter.analyzeProgram().ready(),
         "the two exact receiver-field lifecycle schedules should be ready "
         "for transformed C++ emission");

  const std::string movedText =
      emitter
          .emitFailureBodyText(movedAddress,
                               "move-only-borrowed-field-transfer-v0", 1)
          .text;
  const std::string defaultText =
      emitter
          .emitFailureBodyText(defaultAddress,
                               "move-only-default-field-transfer-v0", 1)
          .text;
  const auto destinationMove = [](const FieldTransfer &transfer) {
    return "__gti_mir_p_" + std::to_string(*transfer.consumer->destination) +
           " = std::move(__gti_mir_p_" + std::to_string(transfer.slot->id) +
           ".get())";
  };
  const auto shellDestroy = [](const FieldTransfer &transfer) {
    return "__gti_mir_p_" + std::to_string(transfer.slot->id) + ".destroy();";
  };
  expect(movedText.find("__gti_mir_p_" + std::to_string(movedField.slot->id) +
                        ".construct(std::move(") != std::string::npos &&
             movedText.find(destinationMove(movedField)) != std::string::npos &&
             movedText.find(shellDestroy(movedField)) != std::string::npos,
         "pop_front should construct from the borrowed field, replace the "
         "receiver field, and destroy only the moved-from shell");
  const auto defaultSlot =
      "__gti_mir_p_" + std::to_string(defaultField.slot->id);
  const bool exactDefaultConstruction =
      defaultField.producer->constructorTarget
          ? defaultText.find(defaultSlot + ".construction_address()") !=
                    std::string::npos &&
                defaultText.find(lang::cppMirFailureConstructorTagSpelling(
                    *defaultField.producer->constructorTarget)) !=
                    std::string::npos &&
                defaultText.find(defaultSlot + ".mark_constructed();") !=
                    std::string::npos
          : defaultText.find(defaultSlot + ".construct();") !=
                std::string::npos;
  expect(exactDefaultConstruction &&
             defaultText.find(destinationMove(defaultField)) !=
                 std::string::npos &&
             defaultText.find(shellDestroy(defaultField)) != std::string::npos,
         "reset should construct the empty owner in its MIR slot, replace "
         "the receiver field, and destroy only the moved-from shell");

  lang::MirProgram malformedMove = frontend.mir;
  auto &moveFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      malformedMove.functionInstances());
  auto changedMove =
      std::find_if(moveFunctions.begin(), moveFunctions.end(),
                   [&](const lang::MirFunctionInstance &candidate) {
                     return candidate.id == movedField.function->id;
                   });
  bool removedEndBorrow = false;
  if (changedMove != moveFunctions.end()) {
    for (lang::MirBlock &block : changedMove->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == movedField.successEndBorrow) {
          instruction.loan.reset();
          removedEndBorrow = true;
        }
      }
    }
  }
  expect(removedEndBorrow,
         "the pop_front near miss should expose its success end-borrow");
  if (removedEndBorrow) {
    const lang::CppMirBodyEmitter malformedEmitter(malformedMove, map);
    expect(!malformedEmitter.supportsFailureBodyText(movedAddress),
           "a borrowed-field move without the sequenced success end-borrow "
           "should fail closed");
  }

  lang::MirProgram malformedDefault = frontend.mir;
  auto &defaultFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      malformedDefault.functionInstances());
  auto changedDefault =
      std::find_if(defaultFunctions.begin(), defaultFunctions.end(),
                   [&](const lang::MirFunctionInstance &candidate) {
                     return candidate.id == defaultField.function->id;
                   });
  bool forgedActivation = false;
  if (changedDefault != defaultFunctions.end()) {
    for (lang::MirBlock &block : changedDefault->body.blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == defaultField.producer->id &&
            instruction.lifecycle.size() == 1) {
          instruction.lifecycle.front().conditional = true;
          forgedActivation = true;
        }
      }
      if (block.terminator.kind == lang::MirTerminatorKind::Invoke &&
          block.terminator.invokeInstruction == defaultField.producer->id &&
          block.terminator.successLifecycle.size() == 1) {
        block.terminator.successLifecycle.front().conditional = true;
        forgedActivation = true;
      }
    }
  }
  expect(forgedActivation,
         "the clear near miss should expose its slot activation event");
  if (forgedActivation) {
    const lang::CppMirBodyEmitter malformedEmitter(malformedDefault, map);
    expect(!malformedEmitter.supportsFailureBodyText(defaultAddress),
           "a conditional default-value activation should fail closed");
  }
}

void testNestedReceiverFieldProjection() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-body-nested-receiver-field.gti", R"(
#include <std/string>

struct Component {
  mut std::size_t offset = 0;
  mut std::size_t length = 0;
};

class Path {
public:
  std::string filename() {
    mut std::string result = std::string("");
    auto offset = this.component.offset;
    return std::move(result);
  }

private:
  mut std::string text = std::string("");
  mut Component component{};
};

int main() {
  mut Path path = Path();
  std::string name = path.filename();
  return int32_t(name.size());
}
)");
  expect(frontend.canGenerateCode(),
         "the nested receiver-field fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *filename = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const lang::HirFunctionInstance *hir =
        frontend.hir.findFunctionInstance(function.id);
    if (hir != nullptr && hir->source != nullptr && function.owner &&
        hir->source->name().lexeme == "filename") {
      filename = &function;
      break;
    }
  }
  expect(filename != nullptr && filename->mayRaiseDefinedFailure,
         "filename should retain its owning local's exact failure effect");
  if (filename == nullptr) {
    return;
  }

  const lang::MirPlace *nested = nullptr;
  for (const lang::MirPlace &place : filename->body.places) {
    if (place.root == lang::MirPlaceRootKind::This &&
        place.projections.size() == 2 &&
        std::all_of(place.projections.begin(), place.projections.end(),
                    [](const lang::MirPlaceProjection &projection) {
                      return projection.kind == lang::MirProjectionKind::Field;
                    })) {
      nested = &place;
      break;
    }
  }
  expect(nested != nullptr,
         "filename MIR should retain the component.offset projection chain");
  if (nested == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = filename->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a failure-form method should admit an exact nested receiver-field "
         "projection");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "nested-receiver-field-test-v0", 1);
  expect(text.emitted() && text.text.find(" = (*this).") != std::string::npos,
         "the emitted method should bind the nested receiver field from the "
         "live object");

  const lang::BackendArtifact artifact = gti_test::emitCpp(frontend);
  expect(artifact.contents.find(" = (*this).") != std::string::npos,
         "production C++ emission should retain the verified nested field "
         "chain without a vocabulary error");
}

void testContainedAbstractBaseConstructor() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-body-contained-abstract-base.gti", R"(
#include <std/memory>
#include <std/vector>

class Item {
public:
  Item() {}
};

class Base {
  mut std::unique_ptr<Item> item;

public:
  Base() : item(std::make_unique<Item>()) {}
  virtual void run() mut = 0;
};

class Derived : public Base {
  mut bool ready = false;

public:
  Derived() : Base() {}

  void run() mut override {
    this.ready = true;
  }
};

void instantiate() {
  Derived value{};
}
)");
  expect(frontend.canGenerateCode(),
         "the contained abstract-base fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::HirClassInstance *baseHir = nullptr;
  const lang::HirClassInstance *derivedHir = nullptr;
  for (const lang::HirClassInstance &instance : frontend.hir.classInstances()) {
    if (instance.source == nullptr) {
      continue;
    }
    if (instance.source->name().lexeme == "Base") {
      baseHir = &instance;
    } else if (instance.source->name().lexeme == "Derived") {
      derivedHir = &instance;
    }
  }
  expect(baseHir != nullptr && derivedHir != nullptr,
         "the fixture should retain both concrete HIR class instances");
  if (baseHir == nullptr || derivedHir == nullptr) {
    return;
  }

  const lang::MirClassInstance *base =
      frontend.mir.findClassInstance(baseHir->id);
  const lang::MirClassInstance *derived =
      frontend.mir.findClassInstance(derivedHir->id);
  expect(base != nullptr && derived != nullptr,
         "the fixture should retain both MIR class instances");
  if (base == nullptr || derived == nullptr) {
    return;
  }
  expect(base->abstract && base->polymorphic && base->requiresActiveCleanup,
         "the base should retain its abstract polymorphic cleanup metadata");
  expect(derived->bases.size() == 1 &&
             derived->bases.front().instance == base->id,
         "the derived class should retain its exact concrete base instance");

  const lang::MirConstructorInstance *baseConstructor = nullptr;
  const lang::MirConstructorInstance *derivedConstructor = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    if (constructor.owner == base->id) {
      baseConstructor = &constructor;
    } else if (constructor.owner == derived->id) {
      derivedConstructor = &constructor;
    }
  }
  expect(baseConstructor != nullptr && derivedConstructor != nullptr,
         "the fixture should retain both source constructor instances");
  if (baseConstructor == nullptr || derivedConstructor == nullptr) {
    return;
  }
  expect(baseConstructor->mayRaiseDefinedFailure &&
             derivedConstructor->mayRaiseDefinedFailure,
         "the owning base initializer failure effect should reach both "
         "constructors");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(
      !emitter.supportsNativeContainedBaseConstruction(derivedConstructor->id),
      "the derived constructor now needs its explicit base failure edge, "
      "not the older terminally contained native-base proof");

  const lang::CppMirBodyEmissionText baseText = emitter.emitBodyText(
      {.kind = lang::MirBodyKind::Constructor, .owner = baseConstructor->id},
      "contained-abstract-base-v0", 1);
  const lang::CppMirBodyEmissionText derivedText = emitter.emitFailureBodyText(
      {.kind = lang::MirBodyKind::Constructor, .owner = derivedConstructor->id},
      "contained-derived-v0", 1);
  expect(baseText.emitted(),
         "an abstract base's source constructor must remain directly "
         "spellable from MIR");
  expect(derivedText.emitted() &&
             derivedText.text.find(" = *__gti_mir_constructor_success;") !=
                 std::string::npos,
         "the derived constructor must consume the selected base's MIR status");
  expect(baseText.text.find("__gti_mir_boundary_failure_") != std::string::npos,
         "the base field call should contain its transformed failure at the "
         "ordinary constructor boundary");
}

// Sweeps the shipped example corpus through the generic body-emission gate.
//
// This exists because the readiness figure is otherwise measured only by
// ad-hoc tooling, and a change that breaks compilation silently "improves" it:
// a rejected source contributes no bodies at all. Asserting that every example
// still reaches MIR keeps that failure mode visible, and asserting that no
// frontend-produced body is Incoherent keeps the gate honest about the
// difference between "needs more MIR authority" and "this MIR is wrong".

void testCleanupFixtureFunctionBodiesAreReady() {
  const std::filesystem::path fixture =
      std::filesystem::path(__FILE__).parent_path() / "fixtures" /
      "mir_backend_class_default_cleanup.gti";
  std::ifstream input(fixture);
  std::stringstream buffer;
  buffer << input.rdbuf();
  const lang::FrontendResult frontend =
      lang::Frontend().analyze(fixture.string(), buffer.str());
  expect(frontend.canGenerateCode(),
         "the class-default-cleanup fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  bool functionBodiesReady = true;
  std::size_t scalarFunctionBodies = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None ||
        function.mayRaiseDefinedFailure) {
      continue;
    }
    ++scalarFunctionBodies;
    const lang::CppMirBodyEmissionAnalysis analysis = emitter.analyze(
        {.kind = lang::MirBodyKind::Function, .owner = function.id});
    if (!analysis.ready()) {
      functionBodiesReady = false;
      std::cerr << "cleanup function-instance " << function.id
                << " is not ready: "
                << (analysis.issues.empty() ? std::string("<no issue>")
                                            : analysis.issues.front().detail)
                << '\n';
    }
  }
  expect(scalarFunctionBodies >= 3 && functionBodiesReady,
         "every non-entry failure-free cleanup-fixture function body should "
         "be analysis-Ready under production rows");

  // The destructor-definition route dissolved into the same per-body
  // admission: every failure-free declared destructor body is Ready and
  // inside the text vocabulary, and the checked-arithmetic near miss
  // (CheckedCleanup) stays outside admission.
  std::size_t admittedDestructorBodies = 0;
  std::size_t declinedDestructorBodies = 0;
  for (const lang::MirDestructorInstance &destructor :
       frontend.mir.destructorInstances()) {
    if (destructor.definitionKind != lang::MirDefinitionKind::Source) {
      continue;
    }
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Destructor,
                                       .owner = destructor.id};
    if (emitter.analyze(address).ready() && emitter.supportsBodyText(address)) {
      ++admittedDestructorBodies;
    } else {
      ++declinedDestructorBodies;
    }
  }
  expect(admittedDestructorBodies == 4 && declinedDestructorBodies == 1,
         "exactly the four ordinary declared destructor bodies should be "
         "admitted and the checked-arithmetic near miss declined");
}

// Milestone gate for the rollback campaign: every constructor and
// field-initializer body of the owned-lifecycle fixture is analysis-Ready
// under production rows, and the scalar user destructor summarizes
// provably failure-free since the effect-proof widening. Native-closing
// prelude destructors and the prelude function bodies legitimately keep
// their containment flags and withheld capability rows.

void testOwnedLifecycleConstructionBodiesReady() {
  const std::filesystem::path repository =
      std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::filesystem::path fixture =
      repository / "tests" / "fixtures" / "mir_backend_owned_lifecycle.gti";
  const std::filesystem::path standardLibrary = repository / "stdlib";
  std::ifstream input(fixture);
  std::stringstream buffer;
  buffer << input.rdbuf();
  const lang::FrontendResult frontend = lang::Frontend().analyze(
      fixture.string(), buffer.str(), {standardLibrary / "prelude.gti"}, {},
      {standardLibrary});
  expect(frontend.canGenerateCode(),
         "the owned-lifecycle fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::CppMirProgramEmissionAnalysis program = emitter.analyzeProgram();
  bool constructionReady = program.issues.empty();
  std::size_t constructionBodies = 0;
  std::size_t readyDestructors = 0;
  for (const lang::CppMirBodyEmissionAnalysis &body : program.bodies) {
    if (body.body.kind == lang::MirBodyKind::Destructor) {
      readyDestructors += body.ready() ? 1 : 0;
      continue;
    }
    if (body.body.kind != lang::MirBodyKind::Constructor &&
        body.body.kind != lang::MirBodyKind::FieldInitializers) {
      continue;
    }
    ++constructionBodies;
    if (!body.ready()) {
      constructionReady = false;
      std::cerr << "owned-lifecycle construction body kind="
                << static_cast<int>(body.body.kind)
                << " owner=" << body.body.owner << " not ready: "
                << (body.issues.empty() ? std::string("<no issue>")
                                        : body.issues.front().detail)
                << '\n';
    }
  }
  expect(constructionBodies >= 3 && constructionReady,
         "every owned-lifecycle constructor and field-initializer body must "
         "stay analysis-Ready under production rows");
  expect(readyDestructors >= 1,
         "the scalar user destructor should summarize provably failure-free "
         "and reach analysis-Ready; native-closing prelude destructors "
         "legitimately keep their containment flags");
}

// A safety discharge proves every local failure outcome impossible even when
// the optimized CFG still carries its structural Invoke. The terminator emits
// only the success edge, so the read must use the unchecked helper and omit a
// dead status carrier. Near misses have no discharge and remain checked.

void testDefaultConstructionPreparedParameterStaging() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-default-prepared-parameter.gti", R"(
mut int observed = 0;
struct Scalar {
  int value = 0;
};
struct Explicit {
  int value;
  Explicit() : value(7) {}
};
void mark() { observed = 9; }

T transfer_value<T>(T value) {
  return std::move(value);
}

int main() {
  Scalar scalar = transfer_value(Scalar());
  expected<Scalar, int> result = Scalar();
  expected<Explicit, int> explicit_result = Explicit();
  if (explicit_result.value().value != 7) { return 1; }
  mark();
  return scalar.value;
}
)");
  expect(frontend.canGenerateCode(),
         "the default prepared-parameter fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto entry =
      std::find_if(frontend.mir.functionInstances().begin(),
                   frontend.mir.functionInstances().end(),
                   [](const lang::MirFunctionInstance &function) {
                     return function.entryKind != lang::ProgramEntryKind::None;
                   });
  expect(entry != frontend.mir.functionInstances().end(),
         "the default prepared-parameter fixture should retain its entry");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "selected zero-argument constructions should retain exact prepared "
         "parameter and Expected publication schedules");
  if (!emitter.supportsFailureBodyText(address))
    return;

  lang::MirInstructionId payloadProducer = 0, publication = 0, marker = 0;
  lang::HirConstructorInstanceId otherConstructor = 0;
  for (const auto &block : entry->body.blocks)
    for (const auto &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.functionTarget && instruction.operands.empty() &&
          !instruction.result)
        marker = instruction.id;
      if (instruction.kind != lang::MirInstructionKind::Initialize ||
          !instruction.destination || instruction.operands.size() != 1 ||
          instruction.operands.front().kind != lang::MirOperandKind::Value)
        continue;
      const auto *destination = entry->body.findPlace(*instruction.destination);
      const auto *value =
          entry->body.findValue(instruction.operands.front().value);
      if (!destination ||
          destination->type.kind != lang::SemanticType::Expected || !value)
        continue;
      for (const auto &producerBlock : entry->body.blocks)
        for (const auto &producer : producerBlock.instructions) {
          if (producer.id != value->definition || !producer.constructorTarget)
            continue;
          const auto *target =
              frontend.mir.findConstructorInstance(*producer.constructorTarget);
          if (target && target->generatedDefault) {
            payloadProducer = producer.id;
            publication = instruction.id;
          } else if (target) {
            otherConstructor = target->id;
          }
        }
    }
  expect(payloadProducer && publication && marker && otherConstructor,
         "the publication mutations select exact constructor, consumer and "
         "observable call identities");
  if (!payloadProducer || !publication || !marker || !otherConstructor)
    return;
  constexpr std::array<std::string_view, 3> mutationReasons = {
      "Expected default publication rejects a constructor of another owner",
      "Expected default publication cannot move construction across an "
      "observable call",
      "Expected default publication rejects a mismatched destination drop"};
  for (int mutation = 0; mutation != 3; ++mutation) {
    auto forged = frontend.mir;
    auto *body = lang::findMirBody(forged, address);
    std::optional<lang::MirInstruction> movedMarker;
    if (mutation == 1)
      for (auto &block : body->blocks) {
        const auto found = std::find_if(
            block.instructions.begin(), block.instructions.end(),
            [&](const auto &instruction) { return instruction.id == marker; });
        if (found != block.instructions.end()) {
          movedMarker = *found;
          block.instructions.erase(found);
          break;
        }
      }
    for (auto &block : body->blocks)
      for (auto position = block.instructions.begin();
           position != block.instructions.end(); ++position) {
        if (mutation == 0 && position->id == payloadProducer)
          position->constructorTarget = otherConstructor;
        if (position->id != publication)
          continue;
        if (mutation == 1 && movedMarker) {
          block.instructions.insert(position, *movedMarker);
          break;
        }
        if (mutation == 2 && !position->lifecycle.empty())
          position->lifecycle.front().target = 0;
      }
    const lang::CppMirBodyEmitter forgedEmitter(forged, map);
    expect(!forgedEmitter.supportsFailureBodyText(address),
           mutationReasons[mutation]);
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(
      artifact.contents.find("function-instance " + std::to_string(entry->id) +
                             "\n") != std::string::npos &&
          artifact.contents.find(".construct();") != std::string::npos &&
          artifact.contents.find("stages a placement class result") !=
              std::string::npos &&
          artifact.contents.find("publishes at its Expected initializer") !=
              std::string::npos &&
          artifact.contents.find(".construct(::__gti_program::Scalar{});") !=
              std::string::npos &&
          artifact.contents.find(".construct(::__gti_program::Explicit{});") !=
              std::string::npos,
      "production emission should invoke exact generated and explicit "
      "constructors through their verified destination schedules");
}

// A generic member on a non-generic class cannot use the generic-owner
// explicit-specialization spelling. Its transformed ABI is one concrete
// overload declared on the class and defined without a template<> prefix.

void testCheckedPassiveExpectedConstruction() {
  const auto frontend = analyzeWithStandardLibrary(
      "checked-passive-expected-construction.gti", R"(
mut int observed = 0;
int checked(int divisor) { return 24 / divisor; }
void mark() { observed = 1; }
struct Scalar { int value = checked(3); };
struct Other { int value = 7; };
int main() {
  Other other = Other();
  expected<Scalar, int> result = Scalar();
  mark();
  return result.value().value - 8;
}
)");
  expect(frontend.canGenerateCode(),
         "fallible generated passive construction must retain typed Expected "
         "publication in MIR");
  if (!frontend.canGenerateCode())
    return;
  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind != lang::ProgramEntryKind::None;
      });
  if (entry == frontend.mir.functionInstances().end()) {
    expect(false, "checked passive Expected fixture retains its entry");
    return;
  }
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.supportsFailureBodyText(address),
         "checked passive constructor publishes only on Invoke success into "
         "its exact Expected binding");
  lang::MirInstructionId producerId = 0, publicationId = 0, markerId = 0;
  lang::HirConstructorInstanceId otherConstructor = 0;
  lang::MirValueId payload = 0;
  for (const auto &block : entry->body.blocks)
    for (const auto &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.functionTarget && !instruction.result &&
          instruction.operands.empty())
        markerId = instruction.id;
      if (instruction.kind != lang::MirInstructionKind::Construct ||
          !instruction.constructorTarget)
        continue;
      if (instruction.definedFailure.propagation ==
          lang::FailurePropagationKind::Constructor) {
        producerId = instruction.id;
        payload = instruction.result.value_or(0);
      } else {
        otherConstructor = *instruction.constructorTarget;
      }
    }
  for (const auto &block : entry->body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Initialize &&
          instruction.operands.size() == 1 &&
          instruction.operands.front().kind == lang::MirOperandKind::Value &&
          instruction.operands.front().value == payload)
        publicationId = instruction.id;
  expect(producerId && publicationId && markerId && payload && otherConstructor,
         "passive Expected mutation sites are exact and concrete");
  if (!producerId || !publicationId || !markerId || !payload ||
      !otherConstructor)
    return;
  expect(std::none_of(
             entry->body.dropObligations.begin(),
             entry->body.dropObligations.end(),
             [&](const auto &drop) { return drop.generatedValue == payload; }),
         "passive constructor staging does not invent a semantic payload drop");
  constexpr std::array<std::string_view, 5> reasons = {
      "passive Expected rejects a constructor targeting another payload type",
      "passive Expected rejects publication after an intervening observable "
      "call",
      "passive Expected rejects lost binding activation",
      "passive Expected rejects publication on the failure edge",
      "passive Expected staging does not erase borrowed state"};
  for (int mutation = 0; mutation != 5; ++mutation) {
    auto forged = frontend.mir;
    auto *body = lang::findMirBody(forged, address);
    std::optional<lang::MirInstruction> moved;
    if (mutation == 1)
      for (auto &block : body->blocks) {
        const auto marker =
            std::find_if(block.instructions.begin(), block.instructions.end(),
                         [&](const auto &instruction) {
                           return instruction.id == markerId;
                         });
        if (marker != block.instructions.end()) {
          moved = *marker;
          block.instructions.erase(marker);
          break;
        }
      }
    for (auto &block : body->blocks) {
      if (mutation == 3 && block.terminator.invokeInstruction == producerId)
        std::swap(block.terminator.target, block.terminator.elseTarget);
      for (auto it = block.instructions.begin(); it != block.instructions.end();
           ++it) {
        if (it->id == producerId) {
          if (mutation == 0)
            it->constructorTarget = otherConstructor;
          if (mutation == 4)
            it->info.traits.containsBorrowedState = true;
        }
        if (it->id != publicationId)
          continue;
        if (mutation == 1 && moved) {
          block.instructions.insert(it, *moved);
          break;
        }
        if (mutation == 2 && !it->lifecycle.empty())
          it->lifecycle.front().target = 0;
      }
    }
    if (mutation == 4)
      for (auto &value : body->values)
        if (value.id == payload)
          value.info.traits.containsBorrowedState = true;
    const lang::CppMirBodyEmitter forgedEmitter(forged, map);
    expect(!forgedEmitter.supportsFailureBodyText(address), reasons[mutation]);
  }
  if (emitter.supportsFailureBodyText(address)) {
    const auto optimizations = lang::OptimizationPipeline().run(
        frontend.hir, lang::OptimizationLevel::O0);
    const auto artifact =
        gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
    lang::MirPlaceId destination = 0;
    for (const auto &block : entry->body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.id == publicationId)
          destination = instruction.destination.value_or(0);
    const auto entryMarker = artifact.contents.find(
        "function-instance " + std::to_string(entry->id) + "\n");
    const std::string declaration =
        " __gti_mir_p_" + std::to_string(destination) + ";\n";
    const auto end = artifact.contents.find(declaration, entryMarker);
    const auto start = end == std::string::npos
                           ? std::string::npos
                           : artifact.contents.rfind('\n', end);
    const std::string line =
        start == std::string::npos
            ? std::string{}
            : artifact.contents.substr(start + 1, end - start - 1);
    expect(
        destination && entryMarker != std::string::npos &&
            line.find("mir_lifetime_slot<") != std::string::npos &&
            artifact.contents.find("__gti_mir_p_" +
                                       std::to_string(destination) +
                                       ".construct(std::move(__gti_mir_v_" +
                                       std::to_string(payload) + ".get()));",
                                   entryMarker) != std::string::npos,
        "checked passive Expected publication declares an uninitialized "
        "wrapper lifetime slot, never an eagerly default-constructed wrapper");
  }
}

void testReferenceReturnFailureAbi() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-reference-return.gti", R"(
class tally {
public:
  mut int total = 3;

  mut int& bump(int amount) mut {
    this.total += amount;
    return this.total;
  }
};

int main() {
  mut tally counter = tally();
  mut int& first = counter.bump(4);
  first = 17;
  return counter.bump(0) - 17;
}
)");
  expect(frontend.canGenerateCode(),
         "the reference-return fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirFunctionInstance *bump = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.returnType.kind == lang::SemanticType::Reference &&
        function.mayRaiseDefinedFailure) {
      bump = &function;
    }
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
  }
  expect(bump != nullptr && entry != nullptr,
         "the fixture should lower the loan-returning member and its caller");
  if (bump == nullptr || entry == nullptr) {
    return;
  }
  const lang::MirPlace *referenceCarrier = nullptr;
  for (const lang::MirPlace &place : entry->body.places) {
    const bool parameter =
        std::find(entry->parameterBindings.begin(),
                  entry->parameterBindings.end(),
                  place.binding) != entry->parameterBindings.end();
    if (!parameter && place.root == lang::MirPlaceRootKind::Binding &&
        place.projections.empty() &&
        place.type.kind == lang::SemanticType::Reference) {
      referenceCarrier = &place;
    }
  }
  const lang::MirLoan *retained = nullptr;
  std::size_t carrierLoans = 0;
  if (referenceCarrier != nullptr) {
    for (const lang::MirLoan &loan : entry->body.loans) {
      if (std::find(loan.carriers.begin(), loan.carriers.end(),
                    referenceCarrier->binding) != loan.carriers.end()) {
        ++carrierLoans;
        retained = &loan;
      }
    }
  }
  bool initializesFromRetainedLoan = false;
  if (referenceCarrier != nullptr && retained != nullptr) {
    for (const lang::MirBlock &block : entry->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        initializesFromRetainedLoan |=
            instruction.kind == lang::MirInstructionKind::Initialize &&
            instruction.destination == referenceCarrier->id &&
            instruction.operands.size() == 1 &&
            instruction.operands.front().kind == lang::MirOperandKind::Loan &&
            instruction.operands.front().loan == retained->id;
      }
    }
  }
  expect(referenceCarrier != nullptr && carrierLoans == 1 &&
             retained != nullptr &&
             retained->kind == lang::MirLoanKind::CallResult &&
             retained->parent == 0 && retained->semanticLoan != 0 &&
             initializesFromRetainedLoan,
         "a retained root call result should preserve its exact loan pointer "
         "as the local reference carrier");
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = bump->id};
  expect(emitter.supportsFailureBodyText(address),
         "the loan-returning body should prove its transformed text");
  const lang::MirBodyAddress entryAddress{.kind = lang::MirBodyKind::Function,
                                          .owner = entry->id};
  expect(emitter.supportsFailureBodyText(entryAddress),
         "the retained-reference caller should prove its transformed text");
  if (!emitter.supportsFailureBodyText(address) ||
      !emitter.supportsFailureBodyText(entryAddress)) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const auto contains = [&](std::string_view needle) {
    return text.find(needle) != std::string::npos;
  };
  expect(contains("std::int32_t **__gti_mir_out_result"),
         "the transformed signature should carry the pointer out-parameter");
  expect(contains("*__gti_mir_out_result = __gti_mir_loan_"),
         "the Return-with-loan should publish through the out-parameter");
  expect(contains("std::int32_t *__gti_mir_p_") &&
             contains(" = __gti_mir_loan_") && contains("&__gti_mir_loan_"),
         "the caller should retain the published pointer in its reference "
         "carrier");
  expect(!contains("__gti_mir_boundary_result"),
         "the direct transformed call must not retain a terminal boundary "
         "wrapper");
}

void testGenericOwnerReferenceReturnFailureAbi() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-generic-reference-return.gti", R"(
class box<T> {
  T value;

public:
  box(T initial) : value(initial) {}

  // The checked division keeps the getter failure-capable, so it returns its
  // reference through the failure ABI's pointer carrier.
  T& get(int32_t divisor) {
    int32_t checked = int32_t(1) / divisor;
    return this.value;
  }
};

int main() {
  box<int> value{7};
  return value.get(1) - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the generic reference-return fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirFunctionInstance *getter = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.returnType.kind == lang::SemanticType::Reference) {
      getter = &function;
    }
    if (function.entryKind == lang::ProgramEntryKind::NoArguments) {
      entry = &function;
    }
  }
  expect(getter != nullptr && entry != nullptr,
         "the fixture should lower the generic member and hosted entry");
  if (getter == nullptr || entry == nullptr) {
    return;
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto contains = [&](std::string_view needle) {
    return artifact.contents.find(needle) != std::string::npos;
  };
  expect(!contains("const T **__gti_mir_out_result") &&
             contains("const std::int32_t **__gti_mir_out_result") &&
             contains("__gti_type_") && contains("_box") &&
             contains("__gti_member_") && contains("_get__gti_mir_failure"),
         "the selected direct generic class member should publish its "
         "concrete reference through the pointer carrier without retaining a "
         "native dependent primary");
  expect(contains("scalar-cfg-failure-v1 function-instance " +
                  std::to_string(getter->id)) &&
             contains("scalar-cfg-failure-v1 function-instance " +
                      std::to_string(entry->id)),
         "the generic reference-return chain should remain closed through "
         "the hosted entry");
}

// The stores-reference constructor schedule (ADR 018): each reference
// field pairs bijectively with one Stored loan whose source is the
// dereference carrier of a reference parameter; the emitted C++ binds the
// field in the member initializer list, the Borrow spells as a comment,
// and no loan pointer materializes. A nonzero constructor borrow origin
// is that schedule's caller-side lifetime fact.

void testStoredReferenceConstructorSchedule() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-stored-reference.gti", R"(
class holder {
  int& target;

public:
  holder(int& value) : target(value) {}

  int read() { return this.target; }
};

int main() {
  mut int cell = 42;
  holder bound = holder(cell);
  return bound.read() - 42;
}
)");
  expect(frontend.canGenerateCode(),
         "the stored-reference fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirConstructorInstance *constructor = nullptr;
  for (const lang::MirConstructorInstance &candidate :
       frontend.mir.constructorInstances()) {
    if (!candidate.body.loans.empty()) {
      constructor = &candidate;
    }
  }
  expect(constructor != nullptr,
         "the fixture should lower the loan-carrying constructor");
  if (constructor == nullptr) {
    return;
  }
  const std::optional<std::vector<lang::CppMirStoredReferenceBinding>>
      bindings = lang::cppMirStoredReferenceBindings(*constructor);
  expect(bindings.has_value() && bindings->size() == 1 &&
             (*bindings)[0].parameter == 0,
         "the schedule should pair the reference field with the reference "
         "parameter");
  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Constructor,
                                     .owner = constructor->id};
  expect(emitter.analyze(address).ready() && emitter.supportsBodyText(address),
         "the stored-reference constructor should be ready and in the text "
         "vocabulary");
  if (!emitter.supportsBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitBodyText(address, "stored-reference-test-v0", 1);
  expect(text.text.find("binds its reference field in the initializer "
                        "list") != std::string::npos &&
             text.text.find("__gti_mir_loan_") == std::string::npos,
         "the Borrow should spell as a comment with no loan pointer local");

  // The full backend chain binds the field in the emitted member
  // initializer list of the selected constructor definition.
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find(" : target(__gti_mir_arg_0) ") !=
             std::string::npos,
         "the emitted constructor should bind the reference field in its "
         "member initializer list");
  expect(artifact.contents.find("constructor-instance " +
                                std::to_string(constructor->id)) !=
             std::string::npos,
         "the selected constructor should carry its verified-MIR banner");
}

// A constructor of a generic class publishes inside its GTI-owned concrete
// class and carries the same verified schedule. The concrete declaration must
// still precede any later class initializer that constructs the application.

void testGenericOwnerConstructorSpecialization() {
  const lang::FrontendResult frontend = analyze("cpp-mir-generic-ctor.gti", R"(
class wrapper<T> {
  T& target;

public:
  wrapper(T& value) : target(value) {}

  T read() { return this.target; }
};

class reading<T> {
  T value;

public:
  reading(T initial) : value(initial) {}

  T read() { return this.value; }
};

class dashboard {
  reading<int> initial{7};

public:
  int read() { return this.initial.read(); }
};

int main() {
  mut int cell = 7;
  wrapper<int> bound = wrapper<int>(cell);
  dashboard display{};
  return bound.read() + display.read() - 14;
}
)");
  expect(frontend.canGenerateCode(),
         "the generic-owner fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto contains = [&](std::string_view needle) {
    return artifact.contents.find(needle) != std::string::npos;
  };
  expect(contains("_wrapper(const std::int32_t & __gti_mir_arg_0);") &&
             contains("class __gti_type_") &&
             !contains("template <> __gti_program::wrapper"),
         "the selected wrapper constructor should declare on its ordinary "
         "GTI-owned concrete class");
  const std::size_t readingDeclaration =
      artifact.contents.find("_reading(std::int32_t __gti_mir_arg_0);");
  const std::size_t dashboardDefinition =
      artifact.contents.find("class dashboard {");
  expect(readingDeclaration != std::string::npos &&
             dashboardDefinition != std::string::npos &&
             readingDeclaration < dashboardDefinition,
         "a direct generic constructor should be declared before a later "
         "class initializer uses its concrete application");
  expect(contains(" : target(__gti_mir_arg_0) "),
         "the concrete constructor should bind the reference field in its "
         "member initializer list");
}

// Generic source constructors and generic members use concrete private
// overloads for their transformed ABI. The source templates remain available
// for compatibility emission, while each admitted MIR instance owns one
// independently nameable definition.

void testConcreteGenericConstructorAndMemberPackPublication() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-concrete-generic-overloads.gti", R"(
#include <std/vector>

class extent {
  uint64_t count;

public:
  extent<uint64_t N>(int32_t values[N]) : count(N) {}

  uint64_t size() { return this.count; }
};

int32_t main() {
  extent fixed = extent({1, 2});
  mut std::vector<int32_t> values{};
  [[discard]] values.emplace_back(4);
  return int32_t(fixed.size()) + values[std::size_t(0)] - 6;
}
)");
  expect(frontend.canGenerateCode(),
         "the concrete generic overload fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirConstructorInstance *genericConstructor = nullptr;
  for (const lang::MirConstructorInstance &constructor :
       frontend.mir.constructorInstances()) {
    const lang::HirConstructorInstance *source =
        frontend.hir.findConstructorInstance(constructor.id);
    if (source != nullptr && source->source != nullptr &&
        !source->source->genericParameters().empty()) {
      genericConstructor = &constructor;
      break;
    }
  }
  const lang::MirFunctionInstance *genericMember = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    const lang::HirFunctionInstance *source =
        frontend.hir.findFunctionInstance(function.id);
    if (source != nullptr && source->source != nullptr && function.owner &&
        std::any_of(source->source->genericParameters().begin(),
                    source->source->genericParameters().end(),
                    [](const lang::GenericParameter &parameter) {
                      return parameter.pack;
                    })) {
      genericMember = &function;
    }
  }
  expect(genericConstructor != nullptr && genericMember != nullptr &&
             entry != nullptr,
         "the fixture should retain concrete generic constructor, member-pack, "
         "and entry bodies");
  if (genericConstructor == nullptr || genericMember == nullptr ||
      entry == nullptr) {
    return;
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto containsMarker = [&](std::string_view kind, std::size_t id) {
    return artifact.contents.find(std::string(kind) + " " + std::to_string(id) +
                                  "\n") != std::string::npos;
  };
  expect(containsMarker("constructor-instance", genericConstructor->id) &&
             containsMarker("function-instance", genericMember->id) &&
             containsMarker("function-instance", entry->id),
         "the concrete generic overloads and their entry caller should all "
         "publish verified-MIR bodies");
  expect(artifact.contents.find("mir_failure_constructor_tag_v1<" +
                                std::to_string(genericConstructor->id) + ">") !=
             std::string::npos,
         "the generic constructor should publish through its unique private "
         "tag overload");
}

void testPassiveConstructorFailureSummary() {
  const lang::FrontendResult frontend = analyze("cpp-mir-passive-ctor.gti", R"(
class counter {
  mut int value;

public:
  counter(int initial) : value(initial) {}

  int tick() mut {
    this.value += 1;
    return this.value;
  }
};

int main() {
  mut counter value{0};
  return value.tick() - 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the passive-constructor fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const lang::MirConstructorInstance *constructor =
      frontend.mir.constructorInstances().empty()
          ? nullptr
          : &frontend.mir.constructorInstances().front();
  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  const lang::MirDefinedFailureEffects effects =
      lang::deriveMirDefinedFailureEffects(frontend.mir);
  expect(constructor != nullptr && !constructor->mayRaiseDefinedFailure &&
             constructor->id <= effects.constructors.size() &&
             !effects.constructors[constructor->id - 1],
         "a constructor covering trivial scalar fields from matching scalar "
         "parameters should have an exact failure-free MIR summary");
  expect(entry != frontend.mir.functionInstances().end(),
         "the fixture should lower its no-argument entry");
  if (constructor == nullptr ||
      entry == frontend.mir.functionInstances().end()) {
    return;
  }
  bool exactConstruction = false;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      exactConstruction =
          exactConstruction ||
          (instruction.kind == lang::MirInstructionKind::Construct &&
           instruction.constructorTarget == constructor->id &&
           instruction.definedFailure.empty() &&
           instruction.localFailureSites.empty());
    }
  }
  expect(exactConstruction,
         "a call to the proved constructor should lower without a failure "
         "edge in its caller");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("constructor-instance " +
                                std::to_string(constructor->id)) !=
                 std::string::npos &&
             artifact.contents.find("function-instance " +
                                    std::to_string(entry->id)) !=
                 std::string::npos,
         "the proved constructor and its failure-capable caller should both "
         "publish from verified MIR");
}

void testPassiveFixedArrayFailureClosure() {
  const lang::FrontendResult frontend = analyze("cpp-mir-fixed-array.gti", R"(
int32_t sum(int32_t values[4]) {
  mut int32_t total = 0;
  for (mut int32_t index = 0; index < 4; index++) {
    total += values[index];
  }
  return total;
}

class pair<T> {
  T values[2];

public:
  pair(T left, T right) : values({left, right}) {}

  T first() { return this.values[0]; }
};

int32_t main() {
  int32_t values[4] = {1, 2, 3, 4};
  int32_t grid[2][2] = {{1, 2}, {3, 4}};
  pair<int32_t> value = pair<int32_t>(7, 8);
  return sum(values) + grid[1][0] + value.first() - 20;
}
)");
  expect(frontend.canGenerateCode(),
         "the passive fixed-array fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  expect(frontend.mir.constructorInstances().size() == 1,
         "the fixed-array fixture should lower one constructor");
  if (frontend.mir.constructorInstances().size() != 1) {
    return;
  }
  const lang::MirConstructorInstance &constructor =
      frontend.mir.constructorInstances().front();
  const lang::MirDefinedFailureEffects effects =
      lang::deriveMirDefinedFailureEffects(frontend.mir);
  expect(!constructor.mayRaiseDefinedFailure &&
             constructor.id <= effects.constructors.size() &&
             !effects.constructors[constructor.id - 1],
         "a complete passive fixed-array initializer should be proved "
         "failure-free");

  auto missingStage = frontend.mir;
  auto *changedConstructor = const_cast<lang::MirConstructorInstance *>(
      missingStage.findConstructorInstance(constructor.id));
  bool detachedStage = false;
  for (auto &block : changedConstructor->body.blocks) {
    for (auto &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Initialize &&
          instruction.constructorInitializer == 1) {
        instruction.constructorInitializer = 0;
        detachedStage = true;
      }
    }
  }
  const auto missingEffects =
      lang::deriveMirDefinedFailureEffects(missingStage);
  expect(detachedStage && !lang::verifyMirProgram(missingStage).valid() &&
             missingEffects.constructors[constructor.id - 1],
         "an aggregate computation without its exact field-write stage cannot "
         "prove a passive array constructor failure-free");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const auto contains = [&](std::string_view needle) {
    return artifact.contents.find(needle) != std::string::npos;
  };
  expect(contains("scalar-cfg-v1 constructor-instance " +
                  std::to_string(constructor.id)),
         "the passive fixed-array constructor should publish from verified "
         "MIR");
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    expect(contains("scalar-cfg-failure-v1 function-instance " +
                    std::to_string(function.id)),
           "every function in the fixed-array failure component should use "
           "the transformed ABI");
  }
  expect(contains("[static_cast<std::size_t>(1)], UINT64_C(0), "
                  "&__gti_mir_v_"),
         "a nested array read should check the terminal index over its "
         "proved-safe parent projection");
  bool checkedReceiverArray = false;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &place : function.body.places) {
      if (place.root == lang::MirPlaceRootKind::This &&
          place.type.kind == lang::SemanticType::Array &&
          place.projections.size() == 1 &&
          place.projections.front().kind == lang::MirProjectionKind::Field) {
        checkedReceiverArray =
            checkedReceiverArray ||
            contains("mir_checked_array_read_v1(__gti_mir_p_" +
                     std::to_string(place.id) + ", UINT64_C(0), &__gti_mir_v_");
      }
    }
  }
  expect(checkedReceiverArray,
         "a receiver fixed-array field read should use the checked failure "
         "operation");
}

void testOwningFixedArrayAggregateAndCheckedMove() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-owning-fixed-array.gti", R"(
#include <std/memory>

struct record {
  int32_t value;
  record(int32_t initial) : value(initial) {}
};

int32_t partial_drop() {
  mut std::unique_ptr<record> owners[2] = {
      std::make_unique<record>(7), std::make_unique<record>(8)};
  auto first = std::move(owners[0]);
  return first->value;
}

int32_t main() {
  return partial_drop() == 7 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning fixed-array fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *selected = nullptr;
  const lang::MirInstruction *checkedMove = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    bool owningAggregate = false;
    const lang::MirInstruction *candidateMove = nullptr;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        owningAggregate =
            owningAggregate ||
            (instruction.kind == lang::MirInstructionKind::Compute &&
             instruction.operation == lang::MirOperation::Aggregate &&
             instruction.info.type.kind == lang::SemanticType::Array &&
             instruction.info.type.arguments.size() == 1 &&
             instruction.info.type.arguments.front().kind ==
                 lang::SemanticType::Class);
        if (instruction.kind == lang::MirInstructionKind::Move &&
            instruction.localFailureSites.size() == 1) {
          candidateMove = &instruction;
        }
      }
    }
    if (owningAggregate && candidateMove != nullptr) {
      selected = &function;
      checkedMove = candidateMove;
      break;
    }
  }
  expect(selected != nullptr && checkedMove != nullptr,
         "the owning fixed-array fixture should retain its aggregate and "
         "checked element move in MIR");
  if (selected == nullptr || checkedMove == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = selected->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the owning aggregate and checked move should support transformed "
         "MIR emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "owning-fixed-array-test-v0", 1)
          .text;
  expect(text.find("mir_lifetime_slot<") != std::string::npos &&
             text.find(".construction_address()") != std::string::npos &&
             text.find("std::array<") != std::string::npos &&
             text.find("std::move(__gti_mir_v_") != std::string::npos &&
             text.find("::gti_internal::backend::array_at(") !=
                 std::string::npos &&
             text.find("failure cleanup drop-obligation") != std::string::npos,
         "the transformed body should stage both owners in exact lifetime "
         "slots, roll back a partial aggregate, and bounds-check the move");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("scalar-cfg-failure-v1 function-instance " +
                                std::to_string(selected->id) + "\n") !=
             std::string::npos,
         "production emission should select the owning fixed-array body from "
         "verified MIR");
}

void testPolymorphicClassArrayFieldLifecycleSchedule() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-polymorphic-class-array-field-lifecycle.gti", R"(
mut int32_t constructed = 0;
mut int32_t destroyed = 0;

interface readable {
  int32_t read();
};

class passive_item : public readable {
public:
  passive_item() {}
  int32_t read() override { return 7; }
};

class counted_item : public readable {
public:
  counted_item() {
    if (constructed == 0) { constructed = 1; }
    else { constructed = 2; }
  }
  ~counted_item() {
    if (destroyed == 0) { destroyed = 1; }
    else { destroyed = 2; }
  }
  int32_t read() override { return 9; }
};

class owner {
public:
  owner() {}
  mut passive_item passive[2] = {};
  mut counted_item counted[2] = {};
};

int main() {
  { mut owner value = owner(); }
  return constructed == 2 && destroyed == 2 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the polymorphic class-array field fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirClassInstance *owner = nullptr;
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    if (instance.declaredFields.size() == 2 &&
        std::all_of(instance.declaredFields.begin(),
                    instance.declaredFields.end(),
                    [](const lang::MirClassFieldInfo &field) {
                      return field.type.kind == lang::SemanticType::Array &&
                             field.type.arguments.size() == 1 &&
                             field.type.arguments.front().kind ==
                                 lang::SemanticType::Class;
                    })) {
      owner = &instance;
      break;
    }
  }
  expect(owner != nullptr,
         "the fixture should retain one exact two-array field owner");
  if (owner == nullptr) {
    return;
  }

  const lang::MirBodyAddress address{
      .kind = lang::MirBodyKind::FieldInitializers, .owner = owner->id};
  const lang::MirBody &body = owner->fieldInitializers;
  std::size_t aggregates = 0;
  std::size_t fixedArrayDrops = 0;
  std::size_t containedDestructorDrops = 0;
  for (const lang::MirDropObligation &drop : body.dropObligations) {
    fixedArrayDrops += drop.dropType.fixedArray.has_value();
  }
  for (const lang::MirBlock &block : body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      aggregates += instruction.kind == lang::MirInstructionKind::Compute &&
                    instruction.operation == lang::MirOperation::Aggregate;
      containedDestructorDrops +=
          instruction.kind == lang::MirInstructionKind::Drop &&
          instruction.definedFailure.propagation ==
              lang::FailurePropagationKind::Destructor &&
          instruction.lifecycle.size() == 1 &&
          instruction.lifecycle.front().failureCleanup;
    }
  }
  expect(aggregates == 2 && fixedArrayDrops == 4 &&
             containedDestructorDrops == 1,
         "the field body should retain two array aggregates, their result and "
         "rollback obligations, and one status-proven destructor cleanup");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirInitializerScheduleText schedule =
      lang::CppMirBodyEmitter(frontend.mir, map).initializerSchedule(address);
  expect(schedule.supported && schedule.fields.size() == 2,
         "interface-only passive and destructor-bearing class arrays should "
         "publish from one verified field-initializer schedule");

  lang::MirProgram malformed = frontend.mir;
  lang::MirBody *malformedBody = lang::findMirBody(malformed, address);
  lang::MirInstruction *malformedDrop = nullptr;
  if (malformedBody != nullptr) {
    for (lang::MirBlock &block : malformedBody->blocks) {
      for (lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Drop &&
            instruction.definedFailure.propagation ==
                lang::FailurePropagationKind::Destructor) {
          malformedDrop = &instruction;
          break;
        }
      }
      if (malformedDrop != nullptr) {
        break;
      }
    }
  }
  if (malformedDrop != nullptr) {
    malformedDrop->definedFailure.propagation =
        lang::FailurePropagationKind::DirectCall;
  }
  expect(malformedDrop != nullptr && !lang::CppMirBodyEmitter(malformed, map)
                                          .initializerSchedule(address)
                                          .supported,
         "retargeting the contained destructor propagation must fail closed");
}

void testPassiveClassArrayMoveAssignmentSchedule() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-passive-class-array-move.gti", R"(
struct record {
  int32_t value;
  record(int32_t initial) : value(initial) {}
};

int32_t main() {
  mut record values[2] = {record(1), record(2)};
  record first = std::move(values[0]);
  record second = std::move(values[1]);
  values[0] = std::move(second);
  values[1] = std::move(first);
  if (values[0].value == 2 and values[1].value == 1) {
    return 0;
  }
  return 1;
}
)");
  expect(frontend.canGenerateCode(),
         "the passive class-array move fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *selected = nullptr;
  const lang::MirInstruction *aggregate = nullptr;
  std::vector<const lang::MirInstruction *> assignmentMoves;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    const lang::MirInstruction *candidateAggregate = nullptr;
    std::vector<const lang::MirInstruction *> candidateMoves;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const lang::MirInstruction &instruction = block.instructions[index];
        if (instruction.kind == lang::MirInstructionKind::Compute &&
            instruction.operation == lang::MirOperation::Aggregate &&
            instruction.info.type.kind == lang::SemanticType::Array &&
            instruction.info.type.arguments.size() == 1 &&
            instruction.info.type.arguments.front().kind ==
                lang::SemanticType::Class) {
          candidateAggregate = &instruction;
        }
        if (index + 1 >= block.instructions.size() ||
            instruction.kind != lang::MirInstructionKind::Move ||
            !instruction.result) {
          continue;
        }
        const lang::MirInstruction &consumer = block.instructions[index + 1];
        const lang::MirPlace *destination =
            consumer.destination
                ? function.body.findPlace(*consumer.destination)
                : nullptr;
        if (consumer.kind == lang::MirInstructionKind::Assign &&
            consumer.operands.size() == 1 &&
            consumer.operands.front().kind == lang::MirOperandKind::Value &&
            consumer.operands.front().value == *instruction.result &&
            destination != nullptr &&
            std::any_of(destination->projections.begin(),
                        destination->projections.end(),
                        [](const lang::MirPlaceProjection &projection) {
                          return projection.kind ==
                                 lang::MirProjectionKind::Index;
                        })) {
          candidateMoves.push_back(&instruction);
        }
      }
    }
    if (candidateAggregate != nullptr && candidateMoves.size() == 2) {
      selected = &function;
      aggregate = candidateAggregate;
      assignmentMoves = std::move(candidateMoves);
      break;
    }
  }
  expect(selected != nullptr && aggregate != nullptr &&
             assignmentMoves.size() == 2,
         "the fixture should retain its passive aggregate and two adjacent "
         "class Move/Assign pairs");
  if (selected == nullptr || aggregate == nullptr ||
      assignmentMoves.size() != 2) {
    return;
  }

  const lang::MirPlace *arraySlot = nullptr;
  for (const lang::MirBlock &block : selected->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Initialize &&
          instruction.destination && instruction.operands.size() == 1 &&
          instruction.operands.front().kind == lang::MirOperandKind::Value &&
          instruction.operands.front().value == *aggregate->result) {
        arraySlot = selected->body.findPlace(*instruction.destination);
      }
    }
  }
  expect(arraySlot != nullptr,
         "the passive aggregate should initialize one exact array binding");
  if (arraySlot == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = selected->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the passive class-array move schedule should support transformed "
         "MIR emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter
          .emitFailureBodyText(address, "passive-class-array-move-test-v0", 1)
          .text;
  const std::string arrayStorage =
      "__gti_mir_p_" + std::to_string(arraySlot->id);
  bool erasedMoveLocals = true;
  for (const lang::MirInstruction *move : assignmentMoves) {
    erasedMoveLocals =
        erasedMoveLocals && move != nullptr && move->result &&
        text.find("__gti_mir_v_" + std::to_string(*move->result)) ==
            std::string::npos;
  }
  expect(text.find(arrayStorage + ".construction_address()") !=
                 std::string::npos &&
             text.find("stages into fixed-array assignment") !=
                 std::string::npos &&
             text.find("mir_checked_array_write_v1") != std::string::npos &&
             text.find(", std::move(__gti_mir_p_") != std::string::npos &&
             text.find(arrayStorage + ".destroy();") != std::string::npos &&
             text.find("].value") != std::string::npos && erasedMoveLocals,
         "the transformed body should construct the non-default array in its "
         "slot, move through checked assignments without SSA objects, read "
         "constant element fields, and destroy the slot on return");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("scalar-cfg-failure-v1 function-instance " +
                                std::to_string(selected->id) + "\n") !=
             std::string::npos,
         "production emission should select the passive class-array body "
         "from verified MIR");
}

void testCheckedClassArrayElementAssignmentFamily() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-checked-class-array-assignment.gti", R"(
class leaf {
public:
  leaf() {}
  mut int32_t v = 1;
};

int main() {
  mut leaf items[4] = {};
  mut leaf source = leaf();
  items[0].v = 2;
  int32_t observed = items[0].v;
  items[1] = source;
  items[2] = std::move(source);
  items[3] = leaf();
  return int(observed + items[1].v + items[2].v + items[3].v) - 5;
}
)");
  expect(frontend.canGenerateCode(),
         "the five-row class-array assignment fixture should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the five-row fixture should lower its entry function");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const lang::MirInstruction *copyLoad = nullptr;
  const lang::MirInstruction *copyAssignment = nullptr;
  const lang::MirInstruction *move = nullptr;
  const lang::MirInstruction *moveAssignment = nullptr;
  const lang::MirInstruction *freshConstruct = nullptr;
  const lang::MirInstruction *freshAssignment = nullptr;
  const lang::MirPlace *copyDestination = nullptr;
  const lang::MirPlace *freshDestination = nullptr;
  bool fieldRead = false;
  bool fieldWrite = false;
  const auto wholeElement = [](const lang::MirPlace *place) {
    return place != nullptr && place->type.kind == lang::SemanticType::Class &&
           place->projections.size() == 1 &&
           place->projections.front().kind == lang::MirProjectionKind::Index;
  };
  const auto elementField = [](const lang::MirPlace *place) {
    return place != nullptr && place->projections.size() == 2 &&
           place->projections.front().kind == lang::MirProjectionKind::Index &&
           place->projections.back().kind == lang::MirProjectionKind::Field;
  };
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (std::size_t index = 0; index < block.instructions.size(); ++index) {
      const lang::MirInstruction &instruction = block.instructions[index];
      if (instruction.kind == lang::MirInstructionKind::Load &&
          instruction.operands.size() == 1) {
        fieldRead = fieldRead || elementField(entry->body.findPlace(
                                     instruction.operands.front().place));
      }
      if (instruction.kind == lang::MirInstructionKind::Assign &&
          instruction.destination) {
        fieldWrite =
            fieldWrite ||
            elementField(entry->body.findPlace(*instruction.destination));
      }
      if (index + 1 >= block.instructions.size() || !instruction.result) {
        continue;
      }
      const lang::MirInstruction &consumer = block.instructions[index + 1];
      const lang::MirPlace *destination =
          consumer.destination ? entry->body.findPlace(*consumer.destination)
                               : nullptr;
      if (consumer.kind != lang::MirInstructionKind::Assign ||
          !consumer.result || consumer.operands.size() != 1 ||
          consumer.operands.front().kind != lang::MirOperandKind::Value ||
          consumer.operands.front().value != *instruction.result ||
          !wholeElement(destination)) {
        continue;
      }
      if (instruction.kind == lang::MirInstructionKind::Load &&
          instruction.operands.front().kind == lang::MirOperandKind::Copy) {
        copyLoad = &instruction;
        copyAssignment = &consumer;
        copyDestination = destination;
      } else if (instruction.kind == lang::MirInstructionKind::Move) {
        move = &instruction;
        moveAssignment = &consumer;
      } else if (instruction.kind == lang::MirInstructionKind::Construct &&
                 instruction.constructorKind ==
                     lang::ConstructorKind::Ordinary) {
        freshConstruct = &instruction;
        freshAssignment = &consumer;
        freshDestination = destination;
      }
    }
  }

  const auto exactBoundsFailure = [&](const lang::MirInstruction *assignment) {
    if (assignment == nullptr || assignment->localFailureSites.size() != 1 ||
        assignment->definedFailure.localOrigins.size() != 1) {
      return false;
    }
    const lang::DefinedFailureOrigin &origin =
        assignment->definedFailure.localOrigins.front();
    const std::vector<lang::DefinedFailureOutcome> expected = {
        {.code = lang::DefinedFailureCode::IndexOutOfBounds,
         .detail = lang::DefinedFailureDetail::FixedArray}};
    const std::optional<lang::FailureSiteId> site =
        frontend.mir.failureMetadata().siteFor(origin);
    const lang::FailureSiteDescriptor *descriptor =
        frontend.mir.failureMetadata().findSite(
            assignment->localFailureSites.front());
    return site && *site == assignment->localFailureSites.front() &&
           origin.outcomes == expected && descriptor != nullptr &&
           descriptor->outcomes == expected;
  };
  const auto mutableArrayBase = [&](const lang::MirPlace *destination) {
    return destination != nullptr &&
           std::any_of(entry->body.places.begin(), entry->body.places.end(),
                       [&](const lang::MirPlace &candidate) {
                         return candidate.root == destination->root &&
                                candidate.binding == destination->binding &&
                                candidate.projections.empty() &&
                                candidate.type.kind ==
                                    lang::SemanticType::Array &&
                                candidate.access == lang::AccessMode::Mutable;
                       });
  };
  expect(fieldRead && fieldWrite && copyLoad != nullptr &&
             copyAssignment != nullptr && move != nullptr &&
             moveAssignment != nullptr && freshConstruct != nullptr &&
             freshAssignment != nullptr && copyDestination != nullptr &&
             freshDestination != nullptr,
         "MIR should retain the field read/write and adjacent copy, move, and "
         "fresh-value whole-element assignment families");
  if (copyLoad == nullptr || copyAssignment == nullptr || move == nullptr ||
      moveAssignment == nullptr || freshConstruct == nullptr ||
      freshAssignment == nullptr || copyDestination == nullptr ||
      freshDestination == nullptr) {
    return;
  }
  expect(copyDestination->access == lang::AccessMode::ReadOnly &&
             freshDestination->access == lang::AccessMode::ReadOnly &&
             mutableArrayBase(copyDestination) &&
             mutableArrayBase(freshDestination) &&
             exactBoundsFailure(copyAssignment) &&
             exactBoundsFailure(moveAssignment) &&
             exactBoundsFailure(freshAssignment),
         "checked whole-element stores should use projected read-only places, "
         "mutable sibling array storage, and one paired fixed-array bounds "
         "site");

  const bool fusedValuesHaveCleanup =
      std::any_of(entry->body.places.begin(), entry->body.places.end(),
                  [&](const lang::MirPlace &place) {
                    return place.root == lang::MirPlaceRootKind::Value &&
                           (place.value == *copyLoad->result ||
                            place.value == *copyAssignment->result ||
                            place.value == *freshConstruct->result ||
                            place.value == *freshAssignment->result);
                  }) ||
      std::any_of(entry->body.dropObligations.begin(),
                  entry->body.dropObligations.end(),
                  [&](const lang::MirDropObligation &drop) {
                    return drop.generatedValue == *copyLoad->result ||
                           drop.generatedValue == *copyAssignment->result ||
                           drop.generatedValue == *freshConstruct->result ||
                           drop.generatedValue == *freshAssignment->result;
                  });
  expect(!fusedValuesHaveCleanup && copyLoad->lifecycle.empty() &&
             copyAssignment->lifecycle.empty() &&
             freshConstruct->lifecycle.empty() &&
             freshAssignment->lifecycle.empty(),
         "the copy and fresh-value fusions should own no value-rooted place, "
         "drop, or lifecycle cleanup");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the five-row checked class-array body should support transformed "
         "MIR emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "class-array-assignment-test-v0", 1)
          .text;
  std::size_t checkedWrites = 0;
  for (std::size_t position = 0;
       (position =
            text.find(" = ::gti_internal::backend::mir_checked_array_write_v1(",
                      position)) != std::string::npos;
       ++checkedWrites, ++position) {
  }
  const std::string copyValue =
      "__gti_mir_v_" + std::to_string(*copyLoad->result);
  const std::string freshValue =
      "__gti_mir_v_" + std::to_string(*freshConstruct->result);
  expect(checkedWrites == 3 &&
             text.find("class copy load " + std::to_string(*copyLoad->result) +
                       " fuses into assignment " +
                       std::to_string(copyAssignment->id)) !=
                 std::string::npos &&
             text.find("construct " + std::to_string(*freshConstruct->result) +
                       " fuses into checked array assignment") !=
                 std::string::npos &&
             text.find("stages into fixed-array assignment") !=
                 std::string::npos &&
             text.find(copyValue) == std::string::npos &&
             text.find(freshValue) == std::string::npos,
         "the copy, move, and fresh rows should each spell one checked write "
         "without materializing copy/fresh SSA objects");

  const lang::FrontendResult invalid =
      lang::Frontend().analyze("cpp-mir-move-only-array-copy.gti", R"(
class move_only {
public:
  move_only() {}
  move_only(move_only& other) = delete;
  move_only(move_only&& other) = default;
};

int main() {
  mut move_only items[2] = {};
  move_only source = move_only();
  items[1] = source;
  return 0;
}
)");
  const bool copyDiagnostic = std::any_of(
      invalid.diagnostics.begin(), invalid.diagnostics.end(),
      [](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == "GTI-S2003" &&
               diagnostic.message.find("array element") != std::string::npos;
      });
  expect(!invalid.canGenerateCode() && copyDiagnostic,
         "a named move-only class value should remain rejected as an array "
         "element copy");
}

void testGeneratedSpecialMemberConstructionSchedule() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-generated-special-members.gti", R"(
class value {
  int32_t payload;

public:
  value(int32_t initial) : payload(initial) {}
  value(value& other) = default;
  value(value&& other) = default;
};

int32_t main() {
  value original = value(7);
  value copied = value(original);
  mut value movable = value(9);
  value moved = value(std::move(movable));
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the generated special-member fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the generated special-member fixture should lower its entry");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  std::size_t copies = 0;
  std::size_t moves = 0;
  bool copyUsesSourcePlace = false;
  bool moveUsesExplicitMove = false;
  bool everyResultActivatesBinding = true;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::Construct ||
          instruction.constructorKind == lang::ConstructorKind::Ordinary) {
        continue;
      }
      copies += instruction.constructorKind == lang::ConstructorKind::Copy;
      moves += instruction.constructorKind == lang::ConstructorKind::Move;
      if (instruction.operands.size() == 1) {
        const lang::MirOperand &operand = instruction.operands.front();
        copyUsesSourcePlace =
            copyUsesSourcePlace ||
            (instruction.constructorKind == lang::ConstructorKind::Copy &&
             operand.kind == lang::MirOperandKind::BorrowRead &&
             entry->body.findPlace(operand.place) != nullptr);
        if (instruction.constructorKind == lang::ConstructorKind::Move &&
            operand.kind == lang::MirOperandKind::Value) {
          const lang::MirValue *value = entry->body.findValue(operand.value);
          for (const lang::MirBlock &candidate : entry->body.blocks) {
            for (const lang::MirInstruction &definition :
                 candidate.instructions) {
              moveUsesExplicitMove =
                  moveUsesExplicitMove ||
                  (value != nullptr && definition.id == value->definition &&
                   definition.kind == lang::MirInstructionKind::Move);
            }
          }
        }
      }

      bool activated = false;
      if (instruction.result) {
        for (const lang::MirValueUse &use :
             entry->body.usesOf(*instruction.result)) {
          for (const lang::MirBlock &candidate : entry->body.blocks) {
            for (const lang::MirInstruction &consumer :
                 candidate.instructions) {
              activated =
                  activated ||
                  (consumer.id == use.instruction &&
                   consumer.kind == lang::MirInstructionKind::Initialize &&
                   consumer.destination && consumer.lifecycle.size() == 1 &&
                   consumer.lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Initialize &&
                   consumer.lifecycle.front().target != 0);
            }
          }
        }
      }
      everyResultActivatesBinding = everyResultActivatesBinding && activated;
    }
  }
  expect(copies == 1 && moves == 1 && copyUsesSourcePlace &&
             moveUsesExplicitMove && everyResultActivatesBinding,
         "generated copy/move MIR should retain exact source, ownership, and "
         "destination-lifetime authority");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  const bool supported = entry->mayRaiseDefinedFailure
                             ? emitter.supportsFailureBodyText(address)
                             : emitter.supportsBodyText(address);
  expect(emitter.analyze(address).ready() && supported,
         "the fully scheduled generated special members should enter the "
         "general MIR text path");
  if (!supported) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      entry->mayRaiseDefinedFailure
          ? emitter.emitFailureBodyText(address,
                                        "generated-special-member-test-v0", 1)
          : emitter.emitBodyText(address, "generated-special-member-test-v0",
                                 1);
  expect(text.text.find("generated special-member construct") !=
                 std::string::npos &&
             text.text.find(".construct(std::move(") != std::string::npos,
         "generated copy/move emission should publish through the proved slot "
         "and move only the explicit move source");
}

void testConditionalClassReturnAndPreparedResultStage() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-conditional-class-return.gti", R"(
#include <std/memory>

struct Reading {
  int32_t value;

  Reading(int32_t initial) : value(initial) {}
};

// The checked division keeps the selection failure-capable, so its
// conditional class return enters the transformed failure body.
std::unique_ptr<Reading> choose_owner(
    bool use_left,
    std::unique_ptr<Reading> left,
    std::unique_ptr<Reading> right,
    int32_t divisor) {
  int32_t checked = int32_t(1) / divisor;
  return use_left ? std::move(left) : std::move(right);
}

int32_t main() {
  auto reading = choose_owner(
      true,
      std::make_unique<Reading>(7),
      std::make_unique<Reading>(11),
      int32_t(1));
  return reading->value - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "the conditional class-return fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *conditional = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    const bool hasBranch = std::any_of(
        function.body.blocks.begin(), function.body.blocks.end(),
        [](const lang::MirBlock &block) {
          return block.terminator.kind == lang::MirTerminatorKind::Branch;
        });
    if (function.parameterTypes.size() == 4 &&
        function.parameterTypes.front().kind == lang::SemanticType::Bool &&
        function.returnType.kind == lang::SemanticType::Class && hasBranch) {
      conditional = &function;
    }
  }
  expect(conditional != nullptr && entry != nullptr,
         "the fixture should lower its conditional owner return and entry");
  if (conditional == nullptr || entry == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);

  const lang::MirBodyAddress conditionalAddress{
      .kind = lang::MirBodyKind::Function, .owner = conditional->id};
  expect(emitter.analyze(conditionalAddress).ready() &&
             emitter.supportsFailureBodyText(conditionalAddress),
         "the conditional class return should enter the transformed "
         "failure-body path");
  if (emitter.supportsFailureBodyText(conditionalAddress)) {
    const lang::CppMirBodyEmissionText text = emitter.emitFailureBodyText(
        conditionalAddress, "conditional-class-return-test-v0", 1);
    const lang::MirPlace *returnedSlot = nullptr;
    for (const auto &block : conditional->body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Return ||
          !block.terminator.value)
        continue;
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Move ||
            instruction.result != block.terminator.value->value ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Move)
          continue;
        returnedSlot =
            conditional->body.findPlace(instruction.operands.front().place);
      }
    }
    expect(returnedSlot &&
               returnedSlot->root == lang::MirPlaceRootKind::Temporary &&
               returnedSlot->projections.empty() &&
               returnedSlot->type == conditional->returnType,
           "the final return must move the exact conditional owner temporary");
    if (returnedSlot) {
      std::size_t initializations = 0;
      std::size_t transfers = 0;
      std::size_t drops = 0;
      for (const auto &block : conditional->body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind == lang::MirInstructionKind::Initialize &&
              instruction.destination == returnedSlot->id)
            ++initializations;
          for (const auto &event : instruction.lifecycle) {
            const auto *drop =
                conditional->body.findDropObligation(event.source);
            if (!drop || drop->place != returnedSlot->id)
              continue;
            if (event.kind == lang::MirLifecycleEventKind::TransferOut &&
                event.target == 0 && !event.conditional &&
                !event.failureCleanup &&
                drop->kind == lang::MirDropObligationKind::Value &&
                !drop->initiallyActive)
              ++transfers;
            if (event.kind == lang::MirLifecycleEventKind::Drop)
              ++drops;
          }
        }
      }
      expect(initializations == 2 && transfers == 1 && drops == 0,
             "both conditional arms must publish to one slot whose exact "
             "drop obligation transfers once without dropping the owner");
      const std::string slot =
          "__gti_mir_p_" + std::to_string(returnedSlot->id);
      const auto occurrences = [&](const std::string &needle) {
        std::size_t count = 0;
        for (auto offset = text.text.find(needle); offset != std::string::npos;
             offset = text.text.find(needle, offset + needle.size()))
          ++count;
        return count;
      };
      const auto publication = text.text.find(
          "construct_representation_at(__gti_mir_out_result, std::move(" +
          slot + ".get()))");
      const auto retirement = text.text.find(slot + ".destroy();");
      expect(text.emitted() && occurrences(slot + ".construct(") == 2 &&
                 publication != std::string::npos &&
                 occurrences(slot + ".destroy();") == 1 &&
                 retirement > publication,
             "emission must construct either arm in that slot, move it into "
             "the result, then retire its moved-from shell exactly once");
    }
  }

  const lang::MirBodyAddress entryAddress{.kind = lang::MirBodyKind::Function,
                                          .owner = entry->id};
  expect(emitter.analyze(entryAddress).ready() &&
             emitter.supportsFailureBodyText(entryAddress),
         "the caller should enter the transformed failure-body path");
  if (emitter.supportsFailureBodyText(entryAddress)) {
    const lang::CppMirBodyEmissionText text = emitter.emitFailureBodyText(
        entryAddress, "prepared-class-result-test-v0", 1);
    expect(text.emitted() &&
               text.text.find(".construction_address()") != std::string::npos &&
               text.text.find(".mark_constructed()") != std::string::npos &&
               text.text.find("stages a placement class result") !=
                   std::string::npos,
           "nested class results should construct in their prepared call "
           "slots and activate those slots only after success");
  }
}

void testConditionalFailureDestructorDrop() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-conditional-drop.gti", R"(
class Cleanup {
public:
  Cleanup() {}
  ~Cleanup() { std::println("cleanup"); }
  operator bool() { return true; }
};

int main() {
  bool skipped = false && Cleanup();
  return skipped ? 1 : 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the conditional class-drop fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *entry = nullptr;
  const lang::MirInstruction *conditionalDrop = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None) {
      continue;
    }
    entry = &function;
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Drop &&
            instruction.destination && instruction.lifecycle.size() == 1 &&
            instruction.lifecycle.front().kind ==
                lang::MirLifecycleEventKind::Drop &&
            instruction.lifecycle.front().conditional) {
          conditionalDrop = &instruction;
        }
      }
    }
  }
  expect(entry != nullptr && conditionalDrop != nullptr,
         "short-circuit cleanup should lower one conditional MIR Drop");
  if (entry == nullptr || conditionalDrop == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the conditional class-drop fixture should enter transformed MIR "
         "emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "conditional-drop-test-v0", 1).text;
  const std::string conditionalDestroy =
      "__gti_mir_p_" + std::to_string(*conditionalDrop->destination) +
      ".destroy_with_failure_if_engaged(";
  expect(text.find(conditionalDestroy) != std::string::npos,
         "a conditional fallible Drop should succeed without touching an "
         "unengaged lifetime slot");
}

void testVirtualFailureInterfaceFamily() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-virtual-failure-family.gti", R"(
interface Iterator<T> {
  T& operator*();
  void operator++() mut;
  int32_t stable_value();
};

class Counter : public Iterator<int32_t> {
  mut int32_t value;

public:
  Counter(int32_t initial) : value(initial) {}
  int32_t& operator*() override { return this.value; }
  void operator++() mut override { this.value++; }
  int32_t stable_value() override { return 7; }
};

class Unrelated {
public:
  Unrelated() {}
  int32_t marker() { return 9; }
};

int32_t direct_stable() {
  mut Counter counter{1};
  return counter.stable_value();
}

int32_t main() {
  mut Counter counter{1};
  Unrelated unrelated{};
  mut Iterator<int32_t>& iterator = counter;
  int32_t first = *iterator;
  ++iterator;
  return *iterator - first - 1 + iterator.stable_value() - 7 +
         direct_stable() - 7 + unrelated.marker() - 9;
}
)");
  expect(frontend.canGenerateCode(),
         "the virtual failure-family fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *dereferenceRoot = nullptr;
  const lang::MirFunctionInstance *dereferenceOverride = nullptr;
  const lang::MirFunctionInstance *incrementRoot = nullptr;
  const lang::MirFunctionInstance *incrementOverride = nullptr;
  const lang::MirFunctionInstance *stableRoot = nullptr;
  const lang::MirFunctionInstance *stableOverride = nullptr;
  const lang::MirFunctionInstance *unrelatedMarker = nullptr;
  const lang::MirFunctionInstance *directCaller = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  lang::HirFunctionInstanceId unrelatedMarkerId = 0;
  lang::HirFunctionInstanceId directCallerId = 0;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source == nullptr) {
      continue;
    }
    if (function.source->name().lexeme == "marker") {
      unrelatedMarkerId = function.id;
    } else if (function.source->name().lexeme == "direct_stable") {
      directCallerId = function.id;
    }
  }
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    if (function.id == directCallerId) {
      directCaller = &function;
    }
    if (!function.overloadedOperator) {
      if (function.id == unrelatedMarkerId) {
        unrelatedMarker = &function;
      } else if (function.pureVirtual) {
        stableRoot = &function;
      } else if (function.overrideMethod) {
        stableOverride = &function;
      }
      continue;
    }
    const bool dereference =
        *function.overloadedOperator == lang::OverloadedOperator::Dereference;
    if (function.pureVirtual) {
      (dereference ? dereferenceRoot : incrementRoot) = &function;
    } else if (function.overrideMethod) {
      (dereference ? dereferenceOverride : incrementOverride) = &function;
    }
  }
  expect(dereferenceRoot != nullptr && dereferenceOverride != nullptr &&
             incrementRoot != nullptr && incrementOverride != nullptr &&
             stableRoot != nullptr && stableOverride != nullptr &&
             unrelatedMarker != nullptr && directCaller != nullptr &&
             entry != nullptr,
         "the fixture should lower all pure contracts, all overrides, and "
         "its unrelated control and entry body");
  if (dereferenceRoot == nullptr || dereferenceOverride == nullptr ||
      incrementRoot == nullptr || incrementOverride == nullptr ||
      stableRoot == nullptr || stableOverride == nullptr ||
      unrelatedMarker == nullptr || directCaller == nullptr ||
      entry == nullptr) {
    return;
  }
  expect(
      lang::cppMirVirtualFailureContractRoot(frontend.mir, *dereferenceRoot) ==
              dereferenceRoot &&
          lang::cppMirVirtualFailureContractRoot(
              frontend.mir, *dereferenceOverride) == dereferenceRoot &&
          lang::cppMirVirtualFailureContractRoot(
              frontend.mir, *incrementRoot) == incrementRoot &&
          lang::cppMirVirtualFailureContractRoot(
              frontend.mir, *incrementOverride) == incrementRoot &&
          lang::cppMirVirtualFailureContractRoot(frontend.mir, *stableRoot) ==
              stableRoot &&
          lang::cppMirVirtualFailureContractRoot(frontend.mir,
                                                 *stableOverride) == stableRoot,
      "each override should resolve to its exact pure interface contract");
  expect(!stableOverride->mayRaiseDefinedFailure,
         "the stable override should exercise a failure-free implementation "
         "of a conservatively failure-capable virtual contract");

  const lang::MirInstruction *directStableCall = nullptr;
  for (const lang::MirBlock &block : directCaller->body.blocks) {
    const auto found = std::find_if(
        block.instructions.begin(), block.instructions.end(),
        [&](const lang::MirInstruction &instruction) {
          return instruction.kind == lang::MirInstructionKind::Call &&
                 instruction.functionTarget == stableOverride->id;
        });
    if (found != block.instructions.end()) {
      directStableCall = &*found;
      break;
    }
  }
  const lang::MirClassInstance *stableOwner =
      stableOverride->owner
          ? frontend.mir.findClassInstance(*stableOverride->owner)
          : nullptr;
  const lang::MirClassInstance *unrelatedOwner =
      unrelatedMarker->owner
          ? frontend.mir.findClassInstance(*unrelatedMarker->owner)
          : nullptr;
  expect(directStableCall != nullptr && stableOwner != nullptr &&
             unrelatedOwner != nullptr &&
             directStableCall->dispatch == lang::CallDispatch::Virtual &&
             directStableCall->dispatchOwner == stableOwner->type &&
             directStableCall->definedFailure.propagation ==
                 lang::FailurePropagationKind::VirtualCall,
         "a concrete override call should retain its exact target, virtual "
         "dispatch owner, and conservative invoke edge in MIR");
  if (directStableCall == nullptr || stableOwner == nullptr ||
      unrelatedOwner == nullptr || unrelatedMarker == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress directAddress{.kind = lang::MirBodyKind::Function,
                                           .owner = directCaller->id};
  for (const lang::MirFunctionInstance *function :
       {dereferenceOverride, incrementOverride, stableOverride, entry}) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function->id};
    expect(emitter.analyze(address).ready() &&
               emitter.supportsFailureBodyText(address),
           "the complete virtual family should support failure-form MIR "
           "emission");
  }
  expect(emitter.analyze(directAddress).ready() &&
             (emitter.supportsBodyText(directAddress) ||
              emitter.supportsFailureBodyText(directAddress)),
         "the direct concrete override caller should remain inside the "
         "complete C++ MIR vocabulary");

  const auto mutableFunction = [](lang::MirProgram &program,
                                  lang::HirFunctionInstanceId id) {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        program.functionInstances());
    const auto found =
        std::find_if(functions.begin(), functions.end(),
                     [&](const lang::MirFunctionInstance &candidate) {
                       return candidate.id == id;
                     });
    return found == functions.end() ? nullptr : &*found;
  };
  const auto mutableInstruction = [&](lang::MirProgram &program,
                                      lang::MirInstructionId id) {
    lang::MirFunctionInstance *function =
        mutableFunction(program, directCaller->id);
    if (function == nullptr) {
      return static_cast<lang::MirInstruction *>(nullptr);
    }
    for (lang::MirBlock &block : function->body.blocks) {
      const auto found =
          std::find_if(block.instructions.begin(), block.instructions.end(),
                       [&](const lang::MirInstruction &instruction) {
                         return instruction.id == id;
                       });
      if (found != block.instructions.end()) {
        return &*found;
      }
    }
    return static_cast<lang::MirInstruction *>(nullptr);
  };
  const auto rejects = [&](lang::MirProgram program, std::string_view message) {
    const lang::CppMirBodyEmitter malformed(program, map);
    expect(!malformed.supportsBodyText(directAddress) &&
               !malformed.supportsFailureBodyText(directAddress),
           message);
  };

  lang::MirProgram missingOwner = frontend.mir;
  mutableFunction(missingOwner, stableRoot->id)->owner.reset();
  rejects(std::move(missingOwner),
          "a virtual-family candidate without an owner must fail closed");

  lang::MirProgram unrelatedTargetOwner = frontend.mir;
  mutableFunction(unrelatedTargetOwner, stableOverride->id)->owner =
      unrelatedOwner->id;
  rejects(std::move(unrelatedTargetOwner),
          "a concrete virtual target with an unrelated owner must fail "
          "closed");

  lang::MirProgram forgedRoot = frontend.mir;
  mutableFunction(forgedRoot, stableOverride->id)->virtualRoots = {
      unrelatedMarker->declaration};
  rejects(std::move(forgedRoot),
          "a concrete virtual target with a forged contract root must fail "
          "closed");

  lang::MirProgram unrelatedDispatchOwner = frontend.mir;
  mutableInstruction(unrelatedDispatchOwner, directStableCall->id)
      ->dispatchOwner = unrelatedOwner->type;
  rejects(std::move(unrelatedDispatchOwner),
          "a concrete virtual call with an unrelated dispatch owner must "
          "fail closed");

  lang::MirProgram forgedTarget = frontend.mir;
  mutableInstruction(forgedTarget, directStableCall->id)->functionTarget =
      unrelatedMarker->id;
  rejects(std::move(forgedTarget),
          "a concrete virtual call with a forged target must fail closed");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  expect(text.find("virtual bool "
                   "__gti_operator_dereference__gti_mir_failure(") !=
                 std::string::npos &&
             text.find("__gti_operator_dereference__gti_mir_failure("
                       "const std::int32_t **__gti_mir_out_result, "
                       "::gti_failure_record_v2 *__gti_mir_failure_record) "
                       "const override;") != std::string::npos,
         "the interface and concrete class should publish one virtual status "
         "ABI");
  expect(text.find("const std::int32_t &Counter::"
                   "__gti_operator_dereference() const {") !=
                 std::string::npos &&
             text.find("void Counter::__gti_operator_pre_increment() {") !=
                 std::string::npos,
         "ordinary virtual entries should be defined as terminal boundary "
         "wrappers for non-MIR callers");
  expect(text.find("scalar-cfg-failure-v1 function-instance " +
                   std::to_string(entry->id)) != std::string::npos &&
             text.find(".__gti_operator_dereference__gti_mir_failure(") !=
                 std::string::npos &&
             text.find("stable_value__gti_mir_failure(") != std::string::npos &&
             text.find(".stable_value()") != std::string::npos &&
             text.find(".Iterator<std::int32_t>::"
                       "__gti_operator_dereference__gti_mir_failure(") ==
                 std::string::npos,
         "the MIR entry should call the unqualified status member so native "
         "virtual dispatch remains active");
}

void testConcreteOnlyGenericVirtualContractRoots() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-concrete-only-virtual-roots.gti", R"(
interface Iterator<T> {
  T& operator*();
  void operator++() mut;
};

class Counter : public Iterator<int32_t> {
  mut int32_t value;

public:
  Counter(int32_t initial) : value(initial) {}
  int32_t& operator*() override { return this.value; }
  void operator++() mut override { this.value++; }
  bool operator!=(Counter& other) {
    return this.value != other.value;
  }
};

class Range {
  int32_t first;
  int32_t last;

public:
  Range(int32_t first, int32_t last) : first(first), last(last) {}
  Counter begin() { return Counter(this.first); }
  Counter end() { return Counter(this.last); }
};

int32_t main() {
  Range values{1, 4};
  mut int32_t total = 0;
  for (auto& value : values) {
    total += value;
  }
  return total - 6;
}
)");
  expect(frontend.canGenerateCode(),
         "the concrete-only virtual contract fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *dereferenceRoot = nullptr;
  const lang::MirFunctionInstance *incrementRoot = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    if (!function.pureVirtual || !function.overloadedOperator) {
      continue;
    }
    if (*function.overloadedOperator == lang::OverloadedOperator::Dereference) {
      dereferenceRoot = &function;
    } else if (*function.overloadedOperator ==
               lang::OverloadedOperator::PreIncrement) {
      incrementRoot = &function;
    }
  }
  expect(dereferenceRoot != nullptr && incrementRoot != nullptr &&
             entry != nullptr,
         "concrete overrides should materialize their exact generic pure "
         "contract roots without an interface-typed source use");
  if (dereferenceRoot == nullptr || incrementRoot == nullptr ||
      entry == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress entryAddress{.kind = lang::MirBodyKind::Function,
                                          .owner = entry->id};
  expect(emitter.analyze(entryAddress).ready() &&
             emitter.supportsFailureBodyText(entryAddress),
         "the concrete-only range caller should retain its transformed MIR "
         "body once the virtual contract family is complete");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("scalar-cfg-failure-v1 function-instance " +
                                std::to_string(entry->id)) !=
                 std::string::npos &&
             artifact.contents.find(
                 ".__gti_operator_dereference__gti_mir_failure(") !=
                 std::string::npos,
         "the production backend should emit the concrete-only range caller "
         "through the virtual failure ABI");
}

void testAbstractBaseFailureFamilyAndInheritedMembers() {
  const lang::FrontendResult frontend =
      analyze("cpp-mir-abstract-base-failure-family.gti", R"(
class Base {
public:
  mut int32_t state = 0;

  virtual bool advance() mut = 0;
  void stop() mut { this.state = 9; }
  bool run() mut { return this.advance(); }
};

class Derived : public Base {
public:
  Derived() {}

  bool advance() mut override {
    this.state++;
    this.stop();
    return true;
  }

  bool complete() { return this.state == 9; }
};

int32_t main() {
  mut Derived value{};
  if (!value.run()) {
    return 1;
  }
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the abstract-base failure-family fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *root = nullptr;
  const lang::MirFunctionInstance *override = nullptr;
  const lang::MirFunctionInstance *inheritedCaller = nullptr;
  const lang::MirFunctionInstance *fieldReader = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    } else if (function.pureVirtual) {
      root = &function;
    } else if (function.overrideMethod) {
      override = &function;
      inheritedCaller = &function;
    } else if (function.owner) {
      const bool callsBaseMember = std::any_of(
          function.body.blocks.begin(), function.body.blocks.end(),
          [&](const lang::MirBlock &block) {
            return std::any_of(block.instructions.begin(),
                               block.instructions.end(),
                               [&](const lang::MirInstruction &instruction) {
                                 if (!instruction.functionTarget) {
                                   return false;
                                 }
                                 const lang::MirFunctionInstance *target =
                                     frontend.mir.findFunctionInstance(
                                         *instruction.functionTarget);
                                 return target != nullptr && target->owner &&
                                        *target->owner != *function.owner;
                               });
          });
      if (callsBaseMember) {
        inheritedCaller = &function;
      } else if (override != nullptr && function.owner == override->owner &&
                 std::any_of(
                     function.body.places.begin(), function.body.places.end(),
                     [](const lang::MirPlace &place) {
                       return place.root == lang::MirPlaceRootKind::This &&
                              !place.projections.empty() &&
                              place.projections.front().kind ==
                                  lang::MirProjectionKind::Field;
                     })) {
        fieldReader = &function;
      }
    }
  }
  const lang::MirConstructorInstance *constructor = nullptr;
  if (override != nullptr && override->owner) {
    for (const lang::MirConstructorInstance &candidate :
         frontend.mir.constructorInstances()) {
      if (candidate.owner == *override->owner) {
        constructor = &candidate;
        break;
      }
    }
  }
  expect(root != nullptr && override != nullptr && inheritedCaller != nullptr &&
             fieldReader != nullptr && entry != nullptr &&
             constructor != nullptr,
         "the fixture should lower its abstract contract, override, inherited "
         "member call, inherited field read, constructor, and entry");
  if (root == nullptr || override == nullptr || inheritedCaller == nullptr ||
      fieldReader == nullptr || entry == nullptr || constructor == nullptr) {
    return;
  }
  expect(lang::cppMirVirtualFailureContractRoot(frontend.mir, *root) == root &&
             lang::cppMirVirtualFailureContractRoot(frontend.mir, *override) ==
                 root,
         "a pure virtual declared on an abstract class should own the same "
         "status ABI as its concrete override");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  for (const lang::MirFunctionInstance *function :
       {override, inheritedCaller, fieldReader, entry}) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function->id};
    expect(emitter.analyze(address).ready() &&
               (emitter.supportsBodyText(address) ||
                emitter.supportsFailureBodyText(address)),
           "abstract-base family body " + std::to_string(function->id) +
               " should have verified MIR text");
  }
  const lang::MirBodyAddress constructorAddress{
      .kind = lang::MirBodyKind::Constructor, .owner = constructor->id};
  expect(emitter.analyze(constructorAddress).ready() &&
             emitter.supportsFailureBodyText(constructorAddress),
         "the passive default-base constructor should have verified failure "
         "MIR text");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("virtual bool advance__gti_mir_failure(") !=
                 std::string::npos &&
             artifact.contents.find("advance__gti_mir_failure(") !=
                 artifact.contents.rfind("advance__gti_mir_failure(") &&
             artifact.contents.find("constructor-instance " +
                                    std::to_string(constructor->id) + "\n") !=
                 std::string::npos &&
             artifact.contents.find("function-instance " +
                                    std::to_string(entry->id) + "\n") !=
                 std::string::npos,
         "an abstract class and its override should publish the virtual "
         "failure contract, passive constructor, and entry implementation");
}

void testUniqueOwnerUpcastFailureBody() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-unique-owner-upcast.gti", R"(
#include <std/memory>

interface Base {
  int32_t value();
};

class Derived : public Base {
public:
  Derived() {}
  int32_t value() override { return 7; }
};

class Other : public Base {
public:
  Other() {}
  int32_t value() override { return 5; }
};

int32_t main() {
  auto scalar = std::make_unique<int32_t>(7);
  auto zero = std::make_unique<int32_t>();
  if (*scalar != 7 || *zero != 0) { return 1; }
  mut std::unique_ptr<Derived> derived = std::make_unique<Derived>();
  mut std::unique_ptr<Base> base =
      std::upcast_unique<Base, Derived>(std::move(derived));
  mut std::unique_ptr<Other> other = std::make_unique<Other>();
  mut std::unique_ptr<Base> other_base =
      std::upcast_unique<Base, Other>(std::move(other));
  return base->value() + other_base->value() - 12;
}
)");
  expect(frontend.canGenerateCode(),
         "the unique-owner upcast fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *upcast = nullptr;
  const lang::MirInstruction *upcastIntrinsic = nullptr;
  const lang::MirInstruction *discardedNullAssignment = nullptr;
  const lang::MirInstruction *returnMove = nullptr;
  const auto findInstruction = [](const lang::MirBody &body,
                                  lang::MirInstructionId id) {
    for (const lang::MirBlock &block : body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == id) {
          return &instruction;
        }
      }
    }
    return static_cast<const lang::MirInstruction *>(nullptr);
  };
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.intrinsic == lang::IntrinsicKind::UniqueOwnerUpcast) {
          upcast = &function;
          upcastIntrinsic = &instruction;
        }
        if (instruction.kind == lang::MirInstructionKind::Assign &&
            instruction.result && instruction.operands.size() == 1 &&
            instruction.operands.front().type.kind ==
                lang::SemanticType::NullPtr &&
            function.body.usesOf(*instruction.result).empty()) {
          discardedNullAssignment = &instruction;
        }
      }
      if (upcast == &function &&
          block.terminator.kind == lang::MirTerminatorKind::Return &&
          block.terminator.value &&
          block.terminator.value->kind == lang::MirOperandKind::Value) {
        const lang::MirValue *returned =
            function.body.findValue(block.terminator.value->value);
        const lang::MirInstruction *construct =
            returned == nullptr
                ? nullptr
                : findInstruction(function.body, returned->definition);
        if (construct != nullptr &&
            construct->kind == lang::MirInstructionKind::Construct &&
            construct->operands.size() == 1 &&
            construct->operands.front().kind == lang::MirOperandKind::Value) {
          const lang::MirValue *moved =
              function.body.findValue(construct->operands.front().value);
          const lang::MirInstruction *definition =
              moved == nullptr
                  ? nullptr
                  : findInstruction(function.body, moved->definition);
          if (definition != nullptr &&
              definition->kind == lang::MirInstructionKind::Move) {
            returnMove = definition;
          }
        }
      }
    }
  }
  expect(upcast != nullptr && upcastIntrinsic != nullptr &&
             discardedNullAssignment != nullptr && returnMove != nullptr,
         "the fixture should lower the consuming intrinsic, discarded null "
         "assignment, and fused owner return");
  if (upcast == nullptr || upcastIntrinsic == nullptr ||
      discardedNullAssignment == nullptr || returnMove == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = upcast->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the unique-owner upcast body should support transformed MIR "
         "emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "unique-owner-upcast-test-v0", 1);
  std::size_t checkedBorrows = 0;
  std::size_t checkedAllocations = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.intrinsic == lang::IntrinsicKind::AllocateUniqueOwner &&
            instruction.info.type.arguments ==
                std::vector<lang::SemanticType>{lang::SemanticType::Int32}) {
          ++checkedAllocations;
          const lang::MirBodyAddress allocationAddress{
              .kind = lang::MirBodyKind::Function, .owner = function.id};
          expect(emitter.analyze(allocationAddress).ready() &&
                     !emitter.supportsBodyText(allocationAddress) &&
                     emitter.supportsFailureBodyText(allocationAddress),
                 "scalar allocation must require the MIR failure ABI");
          if (emitter.supportsFailureBodyText(allocationAddress)) {
            const auto allocationText = emitter.emitFailureBodyText(
                allocationAddress, "scalar-allocation-test-v0", 1);
            const auto allocate =
                allocationText.text.find("mir_allocate_unique_scalar_v1<");
            const auto check = allocationText.text.find(
                ".code == GTI_FAILURE_CODE_ALLOCATION_FAILURE_V1");
            const auto record = allocationText.text.find(
                "__gti_allocation_record = ::gti_failure_record_v2{");
            const auto abort = allocationText.text.find(
                "::gti_rt_failure_abort_allocation_v2(");
            const auto publication = allocationText.text.find(
                "construct_representation_at(__gti_mir_out_result,");
            expect(
                allocate != std::string::npos && check != std::string::npos &&
                    record != std::string::npos && abort != std::string::npos &&
                    publication != std::string::npos && allocate < check &&
                    check < record && record < abort && abort < publication &&
                    allocationText.text.find("return false;") ==
                        std::string::npos &&
                    allocationText.text.find("*__gti_mir_failure_record =") ==
                        std::string::npos &&
                    allocationText.text.find("::backend::make_unique<") ==
                        std::string::npos,
                "scalar exhaustion must report its exact local record and "
                "abort before publication, without a cleanup failure return");
          }
        }
        if (instruction.intrinsic != lang::IntrinsicKind::UniqueOwnerBorrow &&
            instruction.intrinsic !=
                lang::IntrinsicKind::UniqueOwnerBorrowMut) {
          continue;
        }
        ++checkedBorrows;
        const lang::MirBodyAddress borrowAddress{
            .kind = lang::MirBodyKind::Function, .owner = function.id};
        expect(
            emitter.analyze(borrowAddress).ready() &&
                !emitter.supportsBodyText(borrowAddress) &&
                emitter.supportsFailureBodyText(borrowAddress),
            "owner borrowing must require the cleanup-preserving failure ABI");
        if (!emitter.supportsFailureBodyText(borrowAddress)) {
          continue;
        }
        const auto borrowText = emitter.emitFailureBodyText(
            borrowAddress, "unique-owner-borrow-test-v0", 1);
        const auto check =
            borrowText.text.find("GTI_FAILURE_CODE_EMPTY_OWNER_ACCESS_V1");
        const auto publication =
            borrowText.text.find("*__gti_mir_out_result =");
        expect(check != std::string::npos && publication != std::string::npos &&
                   check < publication &&
                   borrowText.text.find(".get();", publication) !=
                       std::string::npos &&
                   borrowText.text.find("return false;") != std::string::npos &&
                   borrowText.text.find("owner_access(") == std::string::npos,
               "the MIR detector must precede success publication and preserve "
               "a failure return");
      }
    }
  }
  expect(checkedBorrows != 0,
         "the upcast client must exercise checked owner access");
  expect(checkedAllocations == 2,
         "both scalar allocation forms need checked production emission");
  const std::size_t upcastCall = text.text.find("unique_owner_upcast<");
  const std::size_t movedArgument = text.text.find(">(std::move(", upcastCall);
  expect(
      text.text.find("unique_owner_upcast<") != std::string::npos &&
          movedArgument != std::string::npos &&
          text.text.find("__gti_mir_v_" +
                         std::to_string(*discardedNullAssignment->result) +
                         " =") == std::string::npos &&
          text.text.find("std::move(__gti_mir_v_" +
                         std::to_string(*returnMove->result)) ==
              std::string::npos &&
          text.text.find(
              "construct_representation_at(__gti_mir_out_result, std::move(") !=
              std::string::npos,
      "the upcast should consume its owner, omit the discarded assignment "
      "readback, and publish the fused move from its source place");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::size_t primary = artifact.contents.find("class unique_ptr");
  const std::size_t nextMember = artifact.contents.find("const T &", primary);
  std::vector<std::string> declarations;
  if (primary != std::string::npos && nextMember != std::string::npos) {
    std::size_t cursor = primary;
    while ((cursor = artifact.contents.find("upcast__gti_mir_failure(",
                                            cursor)) != std::string::npos &&
           cursor < nextMember) {
      const std::size_t nameStart =
          artifact.contents.rfind("__gti_member_", cursor);
      if (nameStart == std::string::npos || nameStart < primary) {
        break;
      }
      declarations.push_back(artifact.contents.substr(
          nameStart, cursor - nameStart +
                         std::string_view("upcast__gti_mir_failure").size()));
      ++cursor;
    }
  }
  const auto occurrenceCount = [&](const std::string &name) {
    std::size_t count = 0;
    std::size_t cursor = 0;
    while ((cursor = artifact.contents.find(name + '(', cursor)) !=
           std::string::npos) {
      ++count;
      ++cursor;
    }
    return count;
  };
  const auto ownerCount = [&](const std::string &name) {
    std::size_t count = 0;
    for (std::string_view owner : {"Derived", "Other"}) {
      count += artifact.contents.find(
                   "unique_ptr<::__gti_program::" + std::string(owner) +
                   ">::" + name + '(') != std::string::npos
                   ? 1
                   : 0;
    }
    return count;
  };
  expect(declarations.size() == 2 && declarations[0] != declarations[1] &&
             std::all_of(declarations.begin(), declarations.end(),
                         [&](const std::string &name) {
                           return occurrenceCount(name) == 4 &&
                                  ownerCount(name) == 1;
                         }),
         "each selected generic upcast on a retained owner carrier should "
         "publish one distinct owner-qualified declaration, specialization, "
         "call, and definition");
}

void testCleanupFreeExpectedMemberProducer() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/expected_cleanup_free_member_result.gti";
  std::ifstream input(fixture);
  expect(input.good(), "cleanup-free member result fixture exists");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode() &&
             lang::verifyMirProgram(frontend.mir).valid(),
         "independent Expected member results retain exact receiver inputs");
  if (!frontend.canGenerateCode())
    return;

  const lang::MirFunctionInstance *forwarder = nullptr;
  const lang::MirInstruction *producer = nullptr;
  const lang::MirFunctionInstance *checkedForwarder = nullptr;
  const lang::MirInstruction *checkedProducer = nullptr;
  for (const auto &function : frontend.mir.functionInstances()) {
    const auto *hir = frontend.hir.findFunctionInstance(function.id);
    if (!hir || !hir->source ||
        (hir->source->name().lexeme != "forward_method" &&
         hir->source->name().lexeme != "forward_checked"))
      continue;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        const auto *target =
            instruction.functionTarget
                ? frontend.mir.findFunctionInstance(*instruction.functionTarget)
                : nullptr;
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.result && instruction.receiver && target &&
            target->owner && !target->staticMember &&
            instruction.info.type == function.returnType) {
          if (hir->source->name().lexeme == "forward_method" &&
              !target->mayRaiseDefinedFailure) {
            forwarder = &function;
            producer = &instruction;
          } else if (hir->source->name().lexeme == "forward_checked" &&
                     target->mayRaiseDefinedFailure &&
                     block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                     block.terminator.invokeInstruction == instruction.id) {
            checkedForwarder = &function;
            checkedProducer = &instruction;
          }
        }
      }
  }
  expect(forwarder && producer,
         "fixture returns one plain member result across fallible cleanup");
  expect(checkedForwarder && checkedProducer,
         "fixture invokes one checked member result across fallible cleanup");
  if (!forwarder || !producer || !checkedForwarder || !checkedProducer)
    return;

  const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                     forwarder->id};
  for (const auto standard :
       {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    const lang::CppMirBodyEmissionMap rows(
        buildRows(frontend, frontend.mir, standard));
    const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
    expect(emitter.analyzeProgram().ready() &&
               emitter.supportsFailureBodyText(address),
           "member result staging supports both backend standards");
    if (!emitter.supportsFailureBodyText(address))
      continue;
    const auto text = emitter.emitFailureBodyText(address, "member-result", 1);
    const auto value = "__gti_mir_v_" + std::to_string(*producer->result);
    expect(text.text.find(", true> " + value + ";") != std::string::npos &&
               text.text.find(value + ".construct_from([&]() { return ") !=
                   std::string::npos &&
               text.text.find(value + "{}") == std::string::npos,
           "plain member constructs one uninitialized result, not a default "
           "success object");
    expect(!gti_test::emitCpp(frontend, standard).contents.empty(),
           "member Return, observer, by-value argument and replacement use "
           "production verified-MIR emission");

    const lang::MirBodyAddress checkedAddress{lang::MirBodyKind::Function,
                                              checkedForwarder->id};
    expect(emitter.supportsFailureBodyText(checkedAddress),
           "transformed member result stages across local cleanup");
    if (emitter.supportsFailureBodyText(checkedAddress)) {
      const auto checkedText = emitter.emitFailureBodyText(
          checkedAddress, "checked-member-result", 1);
      const auto checkedValue =
          "__gti_mir_v_" + std::to_string(*checkedProducer->result);
      expect(
          checkedText.text.find(", true> " + checkedValue + ";") !=
                  std::string::npos &&
              checkedText.text.find(checkedValue + ".construction_address()") !=
                  std::string::npos &&
              checkedText.text.find(checkedValue + "{}") == std::string::npos,
          "Invoke publishes into an uninitialized independent result before "
          "cleanup, without default construction");
    }

    for (int mutation = 0; mutation != 5; ++mutation) {
      auto malformed = frontend.mir;
      auto *body = lang::findMirBody(malformed, address);
      auto *call = const_cast<lang::MirInstruction *>(
          testInstruction(*body, producer->id));
      if (mutation == 0)
        call->receiver.reset();
      if (mutation == 1)
        call->receiver->type = lang::SemanticType::Int32;
      if (mutation == 2)
        const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(*call->functionTarget))
            ->staticMember = true;
      if (mutation == 3)
        const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(*call->functionTarget))
            ->returnType = lang::SemanticType::Int32;
      if (mutation == 4) {
        body->places.push_back({.id = body->places.size() + 1,
                                .root = lang::MirPlaceRootKind::Value,
                                .value = *call->result,
                                .type = call->info.type,
                                .access = lang::AccessMode::ReadOnly,
                                .traits = call->info.traits});
      }
      expect(lang::rebuildMirValueUses(*body),
             "member forgery independently repairs its use census");
      const bool valid = lang::verifyMirProgram(malformed).valid();
      expect(mutation == 4 ? valid : !valid,
             "exact member identity failures are MIR errors; a result-place "
             "alias instead loses the single-slot representation proof");
      // Vocabulary probes do not replace MIR verification. A mistyped
      // receiver is rejected by structural readiness; an otherwise valid
      // alias must independently fail the result-slot vocabulary proof.
      const lang::CppMirBodyEmitter forgedEmitter(malformed, rows);
      expect(!forgedEmitter.analyzeProgram().ready() &&
                 (!valid || !forgedEmitter.supportsFailureBodyText(address)),
             "missing/wrong receivers, static targets, return substitutions "
             "and result aliases cannot enter member staging: " +
                 std::to_string(mutation));
    }
  }
}

void testExpectedLifetimeSlotObserverFusion() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-expected-slot-observer.gti", R"(
#include <std/format>
#include <std/string>

int32_t main() {
  auto formatted = std::try_format("value={}", int32_t(7));
  if (!formatted) {
    return 1;
  }
  std::println(formatted.value());
  return 0;
}
)");
  expect(frontend.canGenerateCode(),
         "the expected-slot observer fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *selected = nullptr;
  lang::MirValueId observerValue = 0;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &load : block.instructions) {
        if (load.kind != lang::MirInstructionKind::Load || !load.result ||
            load.info.type.kind != lang::SemanticType::Expected ||
            load.info.type.arguments.size() != 2 ||
            load.info.type.arguments.front().kind !=
                lang::SemanticType::Class ||
            load.operands.size() != 1 || load.operands.front().place == 0) {
          continue;
        }
        const bool ownedSlot =
            std::any_of(function.body.dropObligations.begin(),
                        function.body.dropObligations.end(),
                        [&](const lang::MirDropObligation &drop) {
                          return drop.place == load.operands.front().place;
                        });
        bool observer = false;
        bool extraction = false;
        for (const lang::MirBlock &candidate : function.body.blocks) {
          for (const lang::MirInstruction &instruction :
               candidate.instructions) {
            observer = observer ||
                       (instruction.kind == lang::MirInstructionKind::Compute &&
                        instruction.operation ==
                            lang::MirOperation::ExpectedHasValue &&
                        instruction.operands.size() == 1 &&
                        instruction.operands.front().kind ==
                            lang::MirOperandKind::Value &&
                        instruction.operands.front().value == *load.result);
            extraction =
                extraction ||
                (instruction.kind == lang::MirInstructionKind::Call &&
                 instruction.receiver &&
                 instruction.receiver->place == load.operands.front().place &&
                 (instruction.intrinsic == lang::IntrinsicKind::ExpectedValue ||
                  instruction.intrinsic == lang::IntrinsicKind::ExpectedError));
          }
        }
        if (ownedSlot && observer && extraction) {
          selected = &function;
          observerValue = *load.result;
        }
      }
    }
  }
  expect(selected != nullptr,
         "the fixture should lower an owned Expected slot with an observer "
         "and extraction");
  if (selected == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = selected->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the owned Expected slot should enter the transformed failure-body "
         "path");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }
  const lang::CppMirBodyEmissionText text =
      emitter.emitFailureBodyText(address, "expected-slot-observer-test-v0", 1);
  const std::string observerLocal =
      "__gti_mir_v_" + std::to_string(observerValue) + "{}";
  const bool extractionUsesLiveValue =
      text.text.find(".get().value()") != std::string::npos ||
      text.text.find(".get().error()") != std::string::npos ||
      (text.text.find("mir_expected_") != std::string::npos &&
       text.text.find(".get(), &__gti_mir_") != std::string::npos);
  expect(text.text.find(".get().has_value()") != std::string::npos &&
             extractionUsesLiveValue,
         "Expected observers and extraction helpers should read the live "
         "value inside its lifetime slot");
  expect(text.text.find(observerLocal) == std::string::npos,
         "the Expected observer's explicit MIR load must not materialize a "
         "copying SSA local");
}

void testExpectedOwningPayloadPublicationAndForwarding() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-expected-owning-payload.gti", R"(
#include <std/tcp>

int main() {
  auto opened = std::tcp::open();
  if (!opened) {
    return 1;
  }
  return opened.value().is_open() ? 0 : 2;
}
)");
  expect(frontend.canGenerateCode(),
         "the owning Expected payload fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::MirFunctionInstance *payloadProducer = nullptr;
  const lang::MirFunctionInstance *forwarder = nullptr;
  const lang::MirFunctionInstance *entry = nullptr;
  for (const lang::MirFunctionInstance &function :
       frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      entry = &function;
    }
    if (function.returnType.kind != lang::SemanticType::Expected ||
        function.returnType.arguments.size() != 2 ||
        function.returnType.arguments.front().kind !=
            lang::SemanticType::Class) {
      continue;
    }
    for (const lang::MirBlock &block : function.body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Construct &&
            instruction.result &&
            instruction.info.type == function.returnType.arguments.front()) {
          payloadProducer = &function;
        }
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.result &&
            instruction.info.type == function.returnType) {
          forwarder = &function;
        }
      }
    }
  }
  expect(payloadProducer != nullptr && forwarder != nullptr && entry != nullptr,
         "the fixture should lower a payload constructor, an unchanged "
         "Expected forwarder, and a hosted caller");
  if (payloadProducer == nullptr || forwarder == nullptr || entry == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  for (const lang::MirFunctionInstance *function :
       {payloadProducer, forwarder, entry}) {
    const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                       .owner = function->id};
    expect(emitter.analyze(address).ready() &&
               emitter.supportsFailureBodyText(address),
           "each owning Expected call-chain body should support transformed "
           "failure emission");
  }

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  const std::string &text = artifact.contents;
  const auto hasMarker = [&](const lang::MirFunctionInstance &function) {
    return text.find("scalar-cfg-failure-v1 function-instance " +
                     std::to_string(function.id)) != std::string::npos;
  };
  expect(hasMarker(*payloadProducer) && hasMarker(*forwarder) &&
             hasMarker(*entry),
         "production emission should select the complete owning Expected "
         "call chain");
  expect(text.find("handle(std::move(__gti_mir_arg_0))") != std::string::npos &&
             text.find(".construct_from([&]() { return ") !=
                 std::string::npos &&
             text.find(", true> __gti_mir_v_") != std::string::npos,
         "the private owned field should initialize in place and the payload "
         "should publish through one representation-cleanup slot");
  expect(text.find("placement value published at its call") !=
                 std::string::npos &&
             text.find("(__gti_mir_out_result, __gti_mir_failure_record)") !=
                 std::string::npos,
         "the unchanged Expected forwarder should pass its caller's result "
         "storage directly to the transformed callee");

  // A plain source call must instead retain its owning result locally when
  // fallible cleanup intervenes before return publication. The runtime fixture
  // exercises successful cleanup, primary failure, and double failure with
  // unrelated user-defined owners rather than a standard-library policy.
  {
    const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                         "fixtures/source_prefix_forwarded_result_cleanup.gti";
    std::ifstream input(fixture);
    expect(input.good(),
           "the unpublished plain-call result fixture must exist");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto retained = analyzeWithStandardLibrary(fixture.string(), source);
    expect(retained.canGenerateCode(),
           "plain owning Expected forwarding across fallible cleanup verifies");
    if (!retained.canGenerateCode())
      return;

    const lang::MirFunctionInstance *retainedForwarder = nullptr;
    const lang::MirInstruction *plainCall = nullptr;
    const lang::MirPlace *resultSlot = nullptr;
    const lang::MirDropObligation *valueDrop = nullptr;
    const lang::MirDropObligation *pendingDrop = nullptr;
    const lang::MirInstruction *localCleanup = nullptr;
    const lang::MirInstruction *resultRollback = nullptr;
    for (const auto &function : retained.mir.functionInstances()) {
      if (!function.mayRaiseDefinedFailure ||
          function.returnType.kind != lang::SemanticType::Expected)
        continue;
      for (const auto &block : function.body.blocks) {
        for (const auto &instruction : block.instructions) {
          const auto *target = instruction.functionTarget
                                   ? retained.mir.findFunctionInstance(
                                         *instruction.functionTarget)
                                   : nullptr;
          if (instruction.kind != lang::MirInstructionKind::Call ||
              !instruction.result || !target ||
              target->mayRaiseDefinedFailure ||
              instruction.info.type != function.returnType ||
              instruction.receiver || instruction.successResultDrop)
            continue;
          for (const auto &drop : function.body.dropObligations) {
            const auto *place = function.body.findPlace(drop.place);
            if (drop.kind != lang::MirDropObligationKind::Value || !place ||
                place->root != lang::MirPlaceRootKind::Value ||
                place->value != *instruction.result ||
                place->type != function.returnType)
              continue;
            for (const auto &pending : function.body.dropObligations) {
              if (pending.kind == lang::MirDropObligationKind::ReturnResult &&
                  pending.place == place->id) {
                retainedForwarder = &function;
                plainCall = &instruction;
                resultSlot = place;
                valueDrop = &drop;
                pendingDrop = &pending;
              }
            }
          }
        }
      }
    }
    if (retainedForwarder && resultSlot && pendingDrop) {
      for (const auto &block : retainedForwarder->body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Drop ||
              !instruction.destination ||
              instruction.definedFailure.propagation !=
                  lang::FailurePropagationKind::Destructor)
            continue;
          if (block.activeFailure == 0 &&
              *instruction.destination != resultSlot->id)
            localCleanup = &instruction;
          if (block.activeFailure != 0 &&
              *instruction.destination == resultSlot->id &&
              instruction.lifecycle.size() == 1 &&
              instruction.lifecycle.front().source == pendingDrop->id &&
              instruction.lifecycle.front().failureCleanup)
            resultRollback = &instruction;
        }
      }
    }
    expect(
        retainedForwarder && plainCall && resultSlot && valueDrop &&
            pendingDrop && localCleanup && resultRollback,
        "the plain call owns one result slot and one pending-return identity "
        "with independent fallible local cleanup and result rollback");
    if (!retainedForwarder || !plainCall || !resultSlot || !valueDrop ||
        !pendingDrop || !localCleanup || !resultRollback)
      return;

    const lang::MirBodyAddress retainedAddress{lang::MirBodyKind::Function,
                                               retainedForwarder->id};
    for (const auto standard :
         {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
      const lang::CppMirBodyEmissionMap retainedRows(
          buildRows(retained, retained.mir, standard));
      const lang::CppMirBodyEmitter retainedEmitter(retained.mir, retainedRows);
      const auto emitted = retainedEmitter.emitFailureBodyText(
          retainedAddress, "plain-expected-return-cleanup", 1);
      expect(retainedEmitter.analyze(retainedAddress).ready() &&
                 retainedEmitter.supportsFailureBodyText(retainedAddress) &&
                 emitted.emitted(),
             "C++20/23 admit exact plain Expected publication through cleanup");
      const std::string slot = "__gti_mir_p_" + std::to_string(resultSlot->id);
      const auto construction = emitted.text.find(slot + ".construct_from(");
      const auto cleanup = emitted.text.find(
          "__gti_mir_drop_success_" + std::to_string(localCleanup->id) + " = ",
          construction);
      const auto publication = emitted.text.find(
          "construct_representation_at(__gti_mir_out_result, " + slot +
          ".take())");
      expect(construction != std::string::npos &&
                 cleanup != std::string::npos &&
                 publication != std::string::npos && construction < cleanup &&
                 cleanup < publication,
             "the producer constructs locally before cleanup and publishes "
             "only after its successful return edge");
      expect(emitted.text.find(slot + ".destroy_cleanup_with_failure(") !=
                     std::string::npos &&
                 emitted.text.find(
                     "GTI MIR unpublished returned result cleanup") !=
                     std::string::npos &&
                 emitted.text.find("__gti_mir_v_" +
                                   std::to_string(*plainCall->result) + "{}") ==
                     std::string::npos,
             "rollback destroys the retained result without inventing a "
             "default-constructed success alternative");
    }

    const lang::CppMirBodyEmissionMap retainedRows(
        buildRows(retained, retained.mir));
    const std::array<std::string_view, 11> mutations{
        "missing plain-call activation",
        "activation of the wrong identity",
        "prematurely active value identity",
        "missing pending-return reparent",
        "wrong pending-return reparent",
        "missing return transfer",
        "rollback of a different destination",
        "rollback of an unowned identity",
        "rollback without failure-cleanup authority",
        "mismatched pending slot",
        "additional result-place consumer"};
    for (std::size_t mutation = 0; mutation < mutations.size(); ++mutation) {
      auto malformed = retained.mir;
      auto *body = lang::findMirBody(malformed, retainedAddress);
      bool changed = false;
      for (auto &block : body->blocks) {
        for (auto &instruction : block.instructions) {
          if (instruction.id == plainCall->id && mutation < 2) {
            if (mutation == 0)
              instruction.lifecycle.clear();
            else
              instruction.lifecycle.front().target = pendingDrop->id;
            changed = true;
          }
          for (auto event = instruction.lifecycle.begin();
               event != instruction.lifecycle.end();) {
            if (event->kind == lang::MirLifecycleEventKind::Reparent &&
                event->source == valueDrop->id &&
                event->target == pendingDrop->id &&
                (mutation == 3 || mutation == 4)) {
              if (mutation == 3)
                event = instruction.lifecycle.erase(event);
              else {
                event->target = 0;
                ++event;
              }
              changed = true;
            } else if (event->kind ==
                           lang::MirLifecycleEventKind::TransferOut &&
                       event->source == pendingDrop->id && mutation == 5) {
              event = instruction.lifecycle.erase(event);
              changed = true;
            } else {
              ++event;
            }
          }
          if (instruction.id == resultRollback->id && mutation >= 6 &&
              mutation <= 8) {
            if (mutation == 6)
              instruction.destination = localCleanup->destination;
            else if (mutation == 7)
              instruction.lifecycle.front().source = 0;
            else
              instruction.lifecycle.front().failureCleanup = false;
            changed = true;
          }
        }
      }
      for (auto &drop : body->dropObligations) {
        if (mutation == 2 && drop.id == valueDrop->id) {
          drop.initiallyActive = true;
          changed = true;
        } else if (mutation == 9 && drop.id == pendingDrop->id) {
          drop.place = *localCleanup->destination;
          changed = true;
        }
      }
      if (mutation == 10) {
        auto alias = *resultSlot;
        for (const auto &place : body->places)
          alias.id = std::max(alias.id, place.id);
        ++alias.id;
        body->places.push_back(alias);
        changed = lang::rebuildMirValueUses(*body);
      }
      const std::string reason = "plain owning Expected forwarding rejects " +
                                 std::string(mutations[mutation]);
      expect(changed, reason + " (mutation applied)");
      // Ownership corruptions are verifier errors; the extra exact place is
      // also independently outside the backend's single-consumer proof.
      const auto verification = lang::verifyMirProgram(malformed);
      if (mutation == 10)
        expect(verification.valid(), reason + " preserves otherwise valid MIR");
      else
        expect(!verification.valid(), reason + " in MIR lifetime verification");
      expect(!lang::CppMirBodyEmitter(malformed, retainedRows)
                  .supportsFailureBodyText(retainedAddress),
             reason + " in backend publication admission");
    }
  }

  // Nominal call operators use the same owned Expected publication schedule
  // as named methods. This proof is independent of the standard wrappers.
  const auto callable =
      analyzeWithStandardLibrary("cpp-mir-callable-expected-result.gti", R"gti(
class Product {
  int value;
public:
  Product(int value) : value(value) {}
  ~Product() {}
};
struct Factory {
  expected<Product, int> operator()(int divisor) {
    int value = 6 / divisor;
    return Product(value);
  }
};
int main() {
  Factory factory{};
  auto result = factory(2);
  return 0;
}
)gti");
  expect(callable.canGenerateCode(),
         "nominal Expected-result invocation has valid frontend/MIR facts");
  if (!callable.canGenerateCode())
    return;
  const lang::MirFunctionInstance *caller = nullptr;
  const lang::MirInstruction *call = nullptr;
  for (const auto &function : callable.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None)
      continue;
    caller = &function;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.callableInvocation && instruction.functionTarget &&
            instruction.info.type.kind == lang::SemanticType::Expected)
          call = &instruction;
  }
  expect(caller && call && call->successResultDrop,
         "nominal invocation activates an exact Expected value obligation");
  if (!caller || !call || !call->successResultDrop)
    return;
  const lang::CppMirBodyEmissionMap callableRows(
      buildRows(callable, callable.mir));
  const lang::MirBodyAddress callerAddress{lang::MirBodyKind::Function,
                                           caller->id};
  expect(lang::CppMirBodyEmitter(callable.mir, callableRows)
             .supportsFailureBodyText(callerAddress),
         "Expected class results publish through ordinary call operators");
  for (int mutation = 0; mutation != 5; ++mutation) {
    auto malformed = callable.mir;
    auto *body = lang::findMirBody(malformed, callerAddress);
    bool changed = false;
    for (auto &block : body->blocks) {
      for (auto &instruction : block.instructions) {
        if (instruction.id == call->id) {
          if (mutation == 0) {
            instruction.functionTarget.reset();
            changed = true;
          } else if (mutation == 1) {
            instruction.successResultDrop.reset();
            changed = true;
          }
        }
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().value != call->result)
          continue;
        if (mutation == 2) {
          instruction.lifecycle.clear();
          changed = true;
        } else if (mutation == 3 && !instruction.lifecycle.empty()) {
          instruction.lifecycle.front().source = 0;
          changed = true;
        } else if (mutation == 4 && !instruction.lifecycle.empty()) {
          instruction.lifecycle.front().conditional = true;
          changed = true;
        }
      }
    }
    expect(changed && !lang::CppMirBodyEmitter(malformed, callableRows)
                           .supportsFailureBodyText(callerAddress),
           "callable Expected publication rejects missing target, activation "
           "and missing or forged binding reparent events");
  }
}

void testCleanupOwningTemporaryArgumentTransfer() {
  const lang::FrontendResult frontend =
      analyzeWithStandardLibrary("cpp-mir-cleanup-temporary-argument.gti", R"(
class cleanup_t {
public:
  cleanup_t(int32_t initial) : value(initial) {}
  cleanup_t(cleanup_t& other) = delete;
  cleanup_t(cleanup_t&& other) = default;
  ~cleanup_t() {}

  int32_t value;
};

int32_t read(cleanup_t value) { return value.value; }

expected<cleanup_t, int32_t> make_ready(int32_t value) {
  return cleanup_t(value);
}

int main() {
  int32_t direct = read(cleanup_t(3));
  expected<cleanup_t, int32_t> ready = make_ready(4);
  return direct + read(std::move(ready).value()) - 7;
}
)");
  expect(frontend.canGenerateCode(),
         "cleanup-owning constructor and Expected results should pass the "
         "frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the cleanup-temporary fixture should retain its entry function");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const auto findInstruction = [](const lang::MirBody &body,
                                  lang::MirInstructionId id) {
    for (const lang::MirBlock &block : body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.id == id) {
          return &instruction;
        }
      }
    }
    return static_cast<const lang::MirInstruction *>(nullptr);
  };
  const lang::MirInstruction *directStage = nullptr;
  const lang::MirInstruction *expectedStage = nullptr;
  const lang::MirInstruction *expectedProducer = nullptr;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::CallInput ||
          instruction.callInputKind != lang::HirCallInputKind::MoveValue ||
          instruction.operands.size() != 1 ||
          instruction.operands.front().kind != lang::MirOperandKind::Value) {
        continue;
      }
      const lang::MirValue *source =
          entry->body.findValue(instruction.operands.front().value);
      const lang::MirInstruction *producer =
          source == nullptr ? nullptr
                            : findInstruction(entry->body, source->definition);
      if (producer != nullptr &&
          producer->kind == lang::MirInstructionKind::Construct) {
        directStage = &instruction;
      }
      if (producer != nullptr &&
          producer->kind == lang::MirInstructionKind::Call &&
          producer->intrinsic == lang::IntrinsicKind::ExpectedValue) {
        expectedStage = &instruction;
        expectedProducer = producer;
      }
    }
  }

  const lang::MirDropObligation *sourceDrop =
      expectedStage == nullptr || expectedStage->lifecycle.size() != 1
          ? nullptr
          : entry->body.findDropObligation(
                expectedStage->lifecycle.front().source);
  const lang::MirDropObligation *preparedDrop =
      expectedStage == nullptr || !expectedStage->preparedParameterDrop
          ? nullptr
          : entry->body.findDropObligation(
                *expectedStage->preparedParameterDrop);
  const lang::MirPlace *sourcePlace =
      sourceDrop == nullptr ? nullptr
                            : entry->body.findPlace(sourceDrop->place);
  const lang::MirPlace *preparedPlace =
      preparedDrop == nullptr ? nullptr
                              : entry->body.findPlace(preparedDrop->place);
  const lang::MirPlace *directPlace =
      directStage == nullptr || !directStage->destination
          ? nullptr
          : entry->body.findPlace(*directStage->destination);
  expect(directStage != nullptr && expectedStage != nullptr &&
             expectedProducer != nullptr && sourceDrop != nullptr &&
             preparedDrop != nullptr && sourcePlace != nullptr &&
             preparedPlace != nullptr && directPlace != nullptr &&
             sourceDrop->kind == lang::MirDropObligationKind::Value &&
             preparedDrop->kind ==
                 lang::MirDropObligationKind::PreparedParameter &&
             sourcePlace->root == lang::MirPlaceRootKind::Value &&
             sourcePlace->projections.empty() &&
             preparedPlace->root == lang::MirPlaceRootKind::Temporary &&
             preparedPlace->projections.empty() &&
             sourceDrop->dropType == preparedDrop->dropType &&
             sourceDrop->dropType.requiresActiveCleanup &&
             expectedStage->lifecycle.front().kind ==
                 lang::MirLifecycleEventKind::Reparent &&
             expectedStage->lifecycle.front().target == preparedDrop->id,
         "the owning extraction should retain one exact Value-drop to "
         "PreparedParameter reparent chain");
  if (directStage == nullptr || expectedStage == nullptr ||
      expectedProducer == nullptr || sourceDrop == nullptr ||
      preparedDrop == nullptr || sourcePlace == nullptr ||
      preparedPlace == nullptr || directPlace == nullptr) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the verified cleanup-owning transfers should support transformed "
         "failure emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "cleanup-temporary-argument-v0", 1)
          .text;
  const auto count = [&](std::string_view needle) {
    std::size_t occurrences = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
      ++occurrences;
    }
    return occurrences;
  };
  const std::string sourceSlot =
      "__gti_mir_p_" + std::to_string(sourcePlace->id);
  const std::string sourceValue =
      "__gti_mir_v_" + std::to_string(expectedStage->operands.front().value);
  const std::string preparedSlot =
      "__gti_mir_p_" + std::to_string(preparedPlace->id);
  const std::string directSlot =
      "__gti_mir_p_" + std::to_string(directPlace->id);
  expect(count("mir_expected_value_move_v1") == 1 &&
             count("&" + preparedSlot) == 1 &&
             count("std::move(" + preparedSlot + ".get())") == 1 &&
             count(preparedSlot + ".destroy();") == 1 &&
             text.find(sourceSlot) == std::string::npos &&
             text.find(sourceValue) == std::string::npos,
         "the Expected extraction should construct the prepared slot once, "
         "consume and retire it once, and emit no source storage");
  expect(count(directSlot + ".construct(") == 1 &&
             count("std::move(" + directSlot + ".get())") == 1 &&
             count(directSlot + ".destroy();") == 1,
         "the direct constructor temporary should use one prepared slot and "
         "one post-call retirement");

  const auto mutableEntry = [&](lang::MirProgram &program) {
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        program.functionInstances());
    const auto found =
        std::find_if(functions.begin(), functions.end(),
                     [&](const auto &item) { return item.id == entry->id; });
    return found == functions.end() ? nullptr : &*found;
  };
  const auto mutableInstruction = [](lang::MirBody &body,
                                     lang::MirInstructionId id) {
    for (lang::MirBlock &block : body.blocks) {
      const auto found =
          std::find_if(block.instructions.begin(), block.instructions.end(),
                       [&](const lang::MirInstruction &instruction) {
                         return instruction.id == id;
                       });
      if (found != block.instructions.end()) {
        return &*found;
      }
    }
    return static_cast<lang::MirInstruction *>(nullptr);
  };
  const auto rejected = [&](const lang::MirProgram &program) {
    const lang::CppMirBodyEmissionMap changedMap(completeRows(program));
    return !lang::CppMirBodyEmitter(program, changedMap)
                .supportsFailureBodyText(address);
  };

  lang::MirProgram duplicateUse = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableEntry(duplicateUse)) {
    if (lang::MirInstruction *stage =
            mutableInstruction(function->body, expectedStage->id)) {
      stage->operands.push_back(stage->operands.front());
      (void)lang::rebuildMirValueUses(function->body);
    }
  }
  expect(rejected(duplicateUse),
         "an owning extraction with a duplicate staged use should remain "
         "unsupported");

  lang::MirProgram projectedSource = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableEntry(projectedSource)) {
    if (lang::MirPlace *place = const_cast<lang::MirPlace *>(
            function->body.findPlace(sourcePlace->id))) {
      place->projections.push_back(
          {.kind = lang::MirProjectionKind::Field, .field = 0});
    }
  }
  expect(rejected(projectedSource),
         "a projected source drop identity should remain unsupported");

  lang::MirProgram aliasedSource = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableEntry(aliasedSource)) {
    lang::MirLoanId nextLoan = 1;
    for (const lang::MirLoan &loan : function->body.loans) {
      nextLoan = std::max(nextLoan, loan.id + 1);
    }
    function->body.loans.push_back({.id = nextLoan,
                                    .source = sourcePlace->id,
                                    .access = lang::AccessMode::ReadOnly});
  }
  expect(rejected(aliasedSource),
         "an aliased source drop identity should remain unsupported");

  lang::MirProgram mismatchedDrop = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableEntry(mismatchedDrop)) {
    for (lang::MirDropObligation &drop : function->body.dropObligations) {
      if (drop.id == preparedDrop->id) {
        drop.dropType.type = lang::SemanticType::Int32;
      }
    }
  }
  expect(rejected(mismatchedDrop),
         "a prepared parameter with mismatched drop metadata should remain "
         "unsupported");

  lang::MirProgram unprovenFailure = frontend.mir;
  if (lang::MirFunctionInstance *function = mutableEntry(unprovenFailure)) {
    if (lang::MirInstruction *producer =
            mutableInstruction(function->body, expectedProducer->id)) {
      producer->localFailureSites.clear();
    }
  }
  expect(rejected(unprovenFailure),
         "an owning extraction without its exact failure proof should remain "
         "unsupported");
}

void testConstructedArgumentTemporaryBorrow() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-constructed-argument-temporary-borrow.gti", R"(
#include <std/string>
class Text {
  std::string data;
public:
  Text(std::string value) : data(value.clone()) {}
  std::string get() { return this.data.clone(); }
  std::string update() mut { return this.data.clone(); }
};
int32_t observe(Text& value) { return 1; }
int32_t main() {
  std::println(Text(std::string("read")).get());
  std::println((Text(std::string("mutable"))).update());
  return observe(Text(std::string("borrow"))) - 1;
}
)");
  expect(frontend.canGenerateCode(),
         "constructed owning arguments of borrowed temporaries should pass "
         "the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }
  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the constructed-argument fixture should lower its entry");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a moved constructor argument should not require naming the outer "
         "temporary receiver or read-borrowed argument");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }
  const std::string text =
      emitter.emitFailureBodyText(address, "constructed-argument-test-v0", 1)
          .text;
  expect(text.find(".construction_address()") != std::string::npos &&
             text.find(".mark_constructed();") != std::string::npos,
         "inner owners should publish into uninitialized result storage");

  const lang::MirInstruction *stage = nullptr;
  const lang::MirInstruction *outer = nullptr;
  bool readReceiver = false;
  bool mutableReceiver = false;
  bool readArgument = false;
  std::size_t constructorStages = 0;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind != lang::MirInstructionKind::CallInput) {
        continue;
      }
      if (instruction.callInputKind == lang::HirCallInputKind::MoveValue &&
          instruction.preparedParameterDrop && instruction.destination &&
          instruction.lifecycle.size() == 1 &&
          instruction.lifecycle.front().kind ==
              lang::MirLifecycleEventKind::Reparent) {
        for (const auto &candidate : entry->body.blocks)
          for (const auto &consumer : candidate.instructions)
            if (consumer.kind == lang::MirInstructionKind::Construct &&
                consumer.callSite == instruction.callSite) {
              ++constructorStages;
              stage = &instruction;
              const auto slot =
                  "__gti_mir_p_" + std::to_string(*instruction.destination);
              expect(text.find(slot + ".take()") != std::string::npos,
                     "constructor handoff must move and retire each prepared "
                     "representation");
            }
      }
      readReceiver |=
          instruction.callInputKind ==
              lang::HirCallInputKind::ReadTemporaryBorrow &&
          instruction.callInputRole == lang::MirCallInputRole::Receiver;
      mutableReceiver |=
          instruction.callInputKind ==
              lang::HirCallInputKind::MutableTemporaryBorrow &&
          instruction.callInputRole == lang::MirCallInputRole::Receiver;
      readArgument |=
          instruction.callInputKind ==
              lang::HirCallInputKind::ReadTemporaryBorrow &&
          instruction.callInputRole == lang::MirCallInputRole::Argument;
    }
  }
  expect(stage != nullptr && constructorStages == 3 && readReceiver &&
             mutableReceiver && readArgument,
         "MIR should retain all three prepared arguments and temporary borrow "
         "roles");
  if (stage == nullptr || !stage->result || stage->operands.size() != 1) {
    return;
  }
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Construct &&
          std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      [&](const lang::MirOperand &operand) {
                        return operand.kind == lang::MirOperandKind::Value &&
                               operand.value == *stage->result;
                      })) {
        outer = &instruction;
      }
    }
  }
  const lang::MirValue *inner =
      entry->body.findValue(stage->operands.front().value);
  expect(inner != nullptr && outer != nullptr && outer->successResultDrop,
         "the transfer should connect one inner producer and one activated "
         "outer temporary");
  if (inner == nullptr || outer == nullptr || !outer->successResultDrop) {
    return;
  }
  const auto mutate = [&](const auto &edit, std::string_view message) {
    lang::MirProgram changed = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        changed.functionInstances());
    auto function = std::find_if(
        functions.begin(), functions.end(),
        [&](const auto &candidate) { return candidate.id == entry->id; });
    edit(function->body);
    expect(!lang::verifyMirProgram(changed).valid(), message);
    // Keep the representation rows from the valid program: lowering an
    // invalid mutation would reject before this independent backend probe.
    expect(
        !lang::CppMirBodyEmitter(changed, map).supportsFailureBodyText(address),
        message);
  };
  mutate(
      [&](lang::MirBody &body) {
        for (lang::MirBlock &block : body.blocks) {
          for (lang::MirInstruction &instruction : block.instructions) {
            if (instruction.id == stage->id) {
              instruction.lifecycle.clear();
            }
          }
        }
      },
      "an inner argument without its transfer must fail closed");
  for (lang::MirInstructionId producer : {inner->definition, outer->id}) {
    mutate(
        [&](lang::MirBody &body) {
          for (lang::MirBlock &block : body.blocks) {
            if (block.terminator.invokeInstruction == producer) {
              block.terminator.successLifecycle.clear();
            }
          }
        },
        "an inner or outer owner without success activation must fail closed");
  }
  mutate(
      [&](lang::MirBody &body) {
        for (lang::MirBlock &block : body.blocks) {
          for (lang::MirInstruction &instruction : block.instructions) {
            if (instruction.kind == lang::MirInstructionKind::Drop &&
                instruction.lifecycle.size() == 1 &&
                instruction.lifecycle.front().source ==
                    *outer->successResultDrop) {
              instruction.lifecycle.clear();
            }
          }
        }
      },
      "an outer temporary without scheduled cleanup must fail closed");
  mutate(
      [&](lang::MirBody &body) {
        for (lang::MirPlace &place : body.places) {
          if (place.root == lang::MirPlaceRootKind::Value &&
              place.value == *outer->result) {
            place.access = lang::AccessMode::ReadOnly;
          }
        }
      },
      "an outer temporary without mutable materialized storage must fail "
      "closed");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("scalar-cfg-failure-v1 function-instance " +
                                std::to_string(entry->id) + "\n") !=
             std::string::npos,
         "production must emit the complete entry from verified MIR");
}

void testFailureCapableClassReturnTemporaryReceiver() {
  const lang::FrontendResult frontend = analyzeWithStandardLibrary(
      "cpp-mir-class-return-temporary-receiver.gti", R"(
#include <std/string>

int32_t main() {
  if (std::to_string(int8_t(0)) != "0") {
    return 1;
  }
  return 0;
}
)");
  expect(
      frontend.canGenerateCode(),
      "the class-return temporary-receiver fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the fixture should lower its no-argument entry");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "a fallible class-return call used as a fallible operator receiver "
         "should enter transformed MIR emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "class-return-receiver-test-v0", 1)
          .text;
  lang::MirDropObligationId resultDrop = 0;
  for (const auto &block : entry->body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.info.type.kind == lang::SemanticType::Class &&
          instruction.successResultDrop)
        resultDrop = *instruction.successResultDrop;
  std::size_t normalDrops = 0;
  std::size_t failureDrops = 0;
  for (const auto &block : entry->body.blocks) {
    for (const auto &instruction : block.instructions) {
      if (!resultDrop || instruction.kind != lang::MirInstructionKind::Drop ||
          instruction.lifecycle.size() != 1 ||
          instruction.lifecycle.front().source != resultDrop)
        continue;
      const bool failure = instruction.lifecycle.front().failureCleanup;
      normalDrops += !failure;
      failureDrops += failure;
      auto forged = frontend.mir;
      auto *changed = lang::findMirBody(forged, address);
      for (auto &changedBlock : changed->blocks)
        for (auto &drop : changedBlock.instructions)
          if (drop.id == instruction.id)
            drop.lifecycle.clear();
      expect(!lang::verifyMirProgram(forged).valid(),
             "source-owned temporary cleanup cannot disappear on either path");
    }
  }
  expect(normalDrops != 0 && failureDrops != 0,
         "one exact result lifetime is retired on success and failure");
  expect(text.find("mir_lifetime_slot<") != std::string::npos &&
             text.find("to_string__gti_mir_failure(") != std::string::npos &&
             text.find(".construction_address()") != std::string::npos &&
             text.find(".mark_constructed();") != std::string::npos &&
             text.find("string::__gti_mir_op_ne__gti_mir_failure(") !=
                 std::string::npos &&
             text.find(".get().") != std::string::npos &&
             text.find("destroy_unique_aggregate_with_failure") !=
                 std::string::npos &&
             text.find("mir_failure_cleanup_scope_v1") != std::string::npos,
         "the returned class should publish into one MIR lifetime slot, serve "
         "as the operator receiver, and retire on success and failure");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(artifact.contents.find("scalar-cfg-failure-v1 function-instance " +
                                std::to_string(entry->id) + "\n") !=
             std::string::npos,
         "production emission should select the temporary-receiver entry from "
         "verified MIR");
}

void checkFailureCapableClassReturnFieldRead(bool immovable) {
  const std::string source = std::string(R"(
struct Packet {
  int32_t value;
  Packet(int32_t initial) : value(initial) {}
)") +
                             (immovable ? R"(
  Packet(Packet& other) = delete;
  Packet(Packet&& other) = delete;
)"
                                        : "") +
                             R"(
};

// The checked division keeps the producer failure-capable.
Packet make_packet(int32_t divisor) {
  int32_t checked = int32_t(1) / divisor;
  return Packet(7);
}

int32_t main() { return make_packet(int32_t(1)).value - 7; }
)";
  const lang::FrontendResult frontend =
      analyze("cpp-mir-class-return-field-read.gti", source);
  expect(frontend.canGenerateCode(),
         "a field read from a returned class value should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the returned-field fixture should lower its entry function");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  lang::MirValueId classResult = 0;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Call &&
          instruction.functionTarget && instruction.result &&
          instruction.info.type.kind == lang::SemanticType::Class) {
        classResult = *instruction.result;
      }
    }
  }
  expect(classResult != 0,
         "the returned-field fixture should retain its class call result");
  const auto *result = entry->body.findValue(classResult);
  expect(result && result->info.traits.movable != immovable,
         "the factory cases must cover both movable and immovable results");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the returned class field read should enter transformed MIR "
         "emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "class-return-field-read-v0", 1)
          .text;
  const std::string slot = "__gti_mir_v_" + std::to_string(classResult);
  expect(text.find("mir_lifetime_slot<") != std::string::npos &&
             text.find(slot + ".construction_address()") != std::string::npos &&
             text.find(slot + ".mark_constructed()") != std::string::npos &&
             text.find(slot + ".get().") != std::string::npos,
         "the returned class should be constructed into one representation "
         "slot and projected from that slot");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(!artifact.contents.empty(),
         "the complete backend should emit a returned-class field read");

  lang::MirProgram duplicateProjectionUse = frontend.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      duplicateProjectionUse.functionInstances());
  lang::MirFunctionInstance *mutatedEntry = nullptr;
  for (lang::MirFunctionInstance &function : functions) {
    if (function.id == entry->id) {
      mutatedEntry = &function;
    }
  }
  lang::MirPlace *projection = nullptr;
  if (mutatedEntry != nullptr) {
    for (lang::MirPlace &place : mutatedEntry->body.places) {
      if (place.root == lang::MirPlaceRootKind::Value &&
          place.value == classResult && !place.projections.empty()) {
        projection = &place;
      }
    }
  }
  bool replacedReturn = false;
  if (mutatedEntry != nullptr && projection != nullptr) {
    for (lang::MirBlock &block : mutatedEntry->body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Return) {
        continue;
      }
      block.terminator.value =
          lang::MirOperand{.kind = lang::MirOperandKind::Copy,
                           .place = projection->id,
                           .type = projection->type};
      replacedReturn = true;
    }
    (void)lang::rebuildMirValueUses(mutatedEntry->body);
  }
  expect(replacedReturn &&
             lang::verifyMirProgram(duplicateProjectionUse).valid(),
         "the returned-field near miss should remain structurally valid MIR");
  if (replacedReturn &&
      lang::verifyMirProgram(duplicateProjectionUse).valid()) {
    const lang::CppMirBodyEmissionMap duplicateMap(
        completeRows(duplicateProjectionUse));
    const lang::CppMirBodyEmitter duplicateEmitter(duplicateProjectionUse,
                                                   duplicateMap);
    expect(!duplicateEmitter.supportsFailureBodyText(address),
           "the class lifetime-slot selector should reject a projected place "
           "with a second direct consumer");
  }
  if (immovable && result) {
    const auto escaped =
        lang::Frontend().analyze("immovable-field-escape.gti",
                                 source + "\nint32_t& escape(Packet& owner) { "
                                          "return make_packet(1).value; }\n");
    expect(!escaped.canGenerateCode() &&
               std::any_of(escaped.diagnostics.begin(),
                           escaped.diagnostics.end(),
                           [](const auto &item) {
                             return item.code == "GTI-S2017" &&
                                    item.message.find("derive from read-only "
                                                      "parameter 'owner'") !=
                                        std::string::npos;
                           }),
           "observing the scalar must not permit a reference to escape its "
           "temporary owner");
    const auto copied = lang::Frontend().analyze(
        "immovable-copy.gti",
        source + "\nPacket copy(Packet& value) { return Packet(value); }\n");
    expect(!copied.canGenerateCode() &&
               std::any_of(copied.diagnostics.begin(), copied.diagnostics.end(),
                           [](const auto &item) {
                             return item.code == "GTI-S2020" &&
                                    item.message.find("Copy construction of "
                                                      "'Packet' is deleted") !=
                                        std::string::npos;
                           }),
           "scalar observation must not unlock copying an immovable owner");
    for (int mutation = 0; mutation != 3; ++mutation) {
      auto malformed = frontend.mir;
      auto &classes = const_cast<std::vector<lang::MirClassInstance> &>(
          malformed.classInstances());
      bool changed = false;
      for (auto &owner : classes) {
        if (owner.type != result->info.type)
          continue;
        changed = true;
        if (mutation == 0)
          owner.requiresActiveCleanup = true;
        else if (mutation == 1)
          owner.requiresActiveDropState = true;
        else
          owner.traits.containsBorrowedState = true;
      }
      const lang::CppMirBodyEmitter rejected(malformed, map);
      expect(changed && !rejected.supportsFailureBodyText(address),
             "immovable field-read storage must not absorb active cleanup, "
             "drop state or borrowed-state lifetime obligations");
    }
  }
}

void testFailureCapableClassReturnFieldRead() {
  checkFailureCapableClassReturnFieldRead(false);
  checkFailureCapableClassReturnFieldRead(true);
}

void testDiscardedCleanupFreeClassResult() {
  const auto frontend = analyze("discarded-class-result.gti", R"(
struct Packet {
  int value;
  Packet(int value) : value(value) {}
};
Packet make_packet(int divisor) { return Packet(12 / divisor); }
int main() { [[discard]] make_packet(2); return 0; }
)");
  expect(frontend.canGenerateCode(), "discarded nominal results must verify");
  if (!frontend.canGenerateCode())
    return;
  for (const auto &entry : frontend.mir.functionInstances()) {
    if (entry.entryKind != lang::ProgramEntryKind::NoArguments)
      continue;
    lang::MirValueId result = 0;
    lang::SemanticType resultType;
    for (const auto &block : entry.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.functionTarget && instruction.result &&
            instruction.info.type.kind == lang::SemanticType::Class) {
          result = *instruction.result;
          resultType = instruction.info.type;
        }
      }
    }
    expect(result != 0 && entry.body.usesOf(result).empty(),
           "the call must retain an unused nominal SSA result");
    const lang::MirBodyAddress address{lang::MirBodyKind::Function, entry.id};
    const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    expect(emitter.supportsFailureBodyText(address),
           "unused cleanup-free results must admit transformed emission");
    if (!emitter.supportsFailureBodyText(address))
      return;
    const auto text =
        emitter.emitFailureBodyText(address, "discard-result", 1).text;
    const auto slot = "__gti_mir_v_" + std::to_string(result);
    const auto construct = text.find(slot + ".construction_address()");
    const auto activate = text.find(slot + ".mark_constructed()", construct);
    const auto destroy = text.find(slot + ".destroy()", activate);
    expect(construct != std::string::npos && activate != std::string::npos &&
               destroy != std::string::npos,
           "successful placement must retire the discarded representation");
    for (int mutation = 0; mutation != 2; ++mutation) {
      auto malformed = frontend.mir;
      auto &classes = const_cast<std::vector<lang::MirClassInstance> &>(
          malformed.classInstances());
      bool changed = false;
      for (auto &owner : classes) {
        if (owner.type != resultType)
          continue;
        changed = true;
        if (mutation == 0)
          owner.requiresActiveCleanup = true;
        else
          owner.traits.containsBorrowedState = true;
      }
      const lang::CppMirBodyEmitter rejected(malformed, map);
      expect(changed && !rejected.supportsFailureBodyText(address),
             "discard storage must not absorb semantic cleanup or borrows");
    }
  }
}

void testFreshOwnedCallableArgument() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/owning_function_policy_failure.gti";
  std::ifstream input(fixture);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "fresh owning callable arguments verify");
  if (!frontend.canGenerateCode())
    return;
  const auto emitted = gti_test::emitCpp(frontend);
  expect(emitted.contents.find(
             "fresh closure already occupies its prepared argument slot") !=
             std::string::npos,
         "fresh closure publication must use prepared storage at its creation "
         "step");
  std::size_t stages = 0;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.entryKind != lang::ProgramEntryKind::OwnedArguments)
      continue;
    for (const auto &block : function.body.blocks) {
      for (const auto &stage : block.instructions) {
        if (stage.kind != lang::MirInstructionKind::CallInput ||
            stage.info.type.kind != lang::SemanticType::Lambda ||
            stage.lifecycle.size() != 1 ||
            stage.lifecycle.front().kind !=
                lang::MirLifecycleEventKind::Reparent)
          continue;
        ++stages;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          for (auto &changed : body->findBlock(block.id)->instructions) {
            if (changed.id != stage.id)
              continue;
            if (mutation == 0)
              changed.lifecycle.front().source = 0;
            else if (mutation == 1)
              changed.lifecycle.front().kind =
                  lang::MirLifecycleEventKind::Initialize;
            else
              changed.callInputKind = lang::HirCallInputKind::CopyValue;
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "fresh closure staging cannot invent, duplicate or copy "
                 "ownership");
          expect(!lang::CppMirBodyEmitter(malformed, map)
                      .supportsFailureBodyText(
                          {lang::MirBodyKind::Function, function.id}),
                 "fresh closure representation rejects forged preparation");
        }
      }
    }
  }
  expect(stages != 0,
         "fresh replacement closure must exercise reparented preparation");
}

void testErasedOwnedResultReturn() {
  for (const char *name : {"owning_function_resource_result.gti",
                           "owning_function_string_result.gti"}) {
    const auto fixture =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
    std::ifstream input(fixture);
    expect(input.good(), "owned callback result fixture must exist");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
    expect(frontend.canGenerateCode(), "owned callback results must verify");
    if (!frontend.canGenerateCode())
      return;
    const auto lowered =
        gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
    const auto baseRows =
        lang::buildCppMirBodyEmissionMapRows(lowered, lang::CppStandard::Cpp23);
    expect(!gti_test::emitCpp(frontend).contents.empty(),
           "owned erased callback results must pass production preflight");
    std::size_t returns = 0;
    for (const auto &function : frontend.mir.functionInstances()) {
      for (const auto &block : function.body.blocks) {
        for (const auto &call : block.instructions) {
          if (call.kind != lang::MirInstructionKind::Call ||
              (call.dispatch != lang::CallDispatch::Virtual &&
               !call.lambdaTarget) ||
              !call.result || !call.successResultDrop ||
              call.info.type != function.returnType ||
              call.info.type.kind != lang::SemanticType::Class)
            continue;
          ++returns;
          const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                             function.id};
          // Stored closures have an unnameable concrete type. Production
          // emits their generic model through the exact declaration overlay.
          auto rows = baseRows;
          const auto *declaration =
              lowered.findFunctionDeclaration(function.declaration);
          expect(declaration != nullptr, "return target declaration exists");
          if (declaration == nullptr)
            continue;
          const auto overlay = lang::cppMirApplyCallableTemplateTypeOverlays(
              rows, lowered, lang::CppStandard::Cpp23, *declaration, function);
          expect(overlay.has_value(), "return target overlay is coherent");
          const lang::CppMirBodyEmissionMap map(std::move(rows));
          const lang::CppMirBodyEmitter emitter(frontend.mir, map);
          expect(emitter.supportsFailureBodyText(address),
                 "owned virtual results need the failure-aware return path");
          if (emitter.supportsFailureBodyText(address)) {
            const auto text =
                emitter.emitFailureBodyText(address, "owned-return", 1).text;
            expect(text.find("__gti_mir_out_result") != std::string::npos,
                   "result storage belongs to the caller of the wrapper");
          }
          for (int mutation = 0; mutation != 3; ++mutation) {
            auto malformed = frontend.mir;
            auto *body = lang::findMirBody(malformed, address);
            for (auto &changed : body->findBlock(block.id)->instructions) {
              if (changed.id != call.id)
                continue;
              if (mutation == 0)
                changed.successResultDrop.reset();
              else if (mutation == 1) {
                if (call.lambdaTarget)
                  changed.lambdaTarget.reset();
                else
                  changed.dispatchOwner = lang::SemanticType::Int32;
              } else
                changed.definedFailure.propagation =
                    lang::FailurePropagationKind::DirectCall;
            }
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "result activation, virtual owner and failure dispatch "
                   "cannot be forged");
          }
        }
      }
    }
    expect(returns != 0,
           "fixture must exercise owned virtual return forwarding");
  }
}

void testConstructedCallbackConditionalArgument() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/constructed_callback_argument.gti";
  std::ifstream input(fixture);
  expect(input.good(), "constructed callback argument fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "constructed callback inputs must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "fresh class arguments followed by conditional arguments must emit");
  std::size_t constructors = 0;
  for (const auto &entry : frontend.mir.functionInstances()) {
    if (entry.entryKind != lang::ProgramEntryKind::OwnedArguments)
      continue;
    for (const auto &block : entry.body.blocks) {
      for (const auto &construct : block.instructions) {
        if (construct.kind != lang::MirInstructionKind::Construct ||
            !construct.constructorTarget || !construct.result ||
            construct.successResultDrop || construct.successResultDestination ||
            construct.operands.size() != 2)
          continue;
        ++constructors;
        expect(block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == construct.id,
               "cleanup-free constructors still need failure routing");
        const lang::MirInstruction *stage = nullptr;
        for (const auto &candidate : entry.body.blocks) {
          for (const auto &instruction : candidate.instructions) {
            if (instruction.kind == lang::MirInstructionKind::CallInput &&
                instruction.operands.size() == 1 &&
                instruction.operands.front().kind ==
                    lang::MirOperandKind::Value &&
                instruction.operands.front().value == *construct.result) {
              stage = &instruction;
              expect(candidate.id == block.terminator.target,
                     "only successful construction can prepare an argument");
            }
          }
        }
        expect(stage && stage->preparedParameterDrop && stage->destination,
               "a fresh argument must retain its exact preparation slot");
        if (!stage || !stage->preparedParameterDrop)
          continue;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, entry.id});
          auto &term = body->findBlock(block.id)->terminator;
          if (mutation == 0) {
            term.kind = lang::MirTerminatorKind::Goto;
            term.invokeInstruction = 0;
            term.failureRecord = 0;
            term.elseTarget = 0;
          } else if (mutation == 1) {
            std::swap(term.target, term.elseTarget);
          } else {
            body->dropObligations[*stage->preparedParameterDrop - 1]
                .initiallyActive = true;
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "missing failure edge, reversed publication edge, and "
                 "premature lifetime must reject");
        }
      }
    }
  }
  expect(constructors == 3, "fixture must cover three prepared constructors");
}

void testConditionalNumericConversionArgument() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/conditional_numeric_argument.gti";
  std::ifstream input(fixture);
  expect(input.good(), "conditional numeric argument fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "alias and type-parameter conversions must retain verified MIR");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  expect(lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
         "numeric argument conversion must preserve a receiver across later "
         "conditional arguments");
  std::size_t aliases = 0;
  std::size_t parameters = 0;
  for (const auto &entry : frontend.mir.functionInstances()) {
    for (const auto &block : entry.body.blocks) {
      for (const auto &conversion : block.instructions) {
        if (conversion.kind != lang::MirInstructionKind::Call ||
            conversion.info.type != lang::SemanticType::UInt8 ||
            (conversion.intrinsic !=
                 lang::IntrinsicKind::NumericAliasConversion &&
             conversion.intrinsic !=
                 lang::IntrinsicKind::NumericTypeParameterConversion))
          continue;
        if (conversion.intrinsic == lang::IntrinsicKind::NumericAliasConversion)
          ++aliases;
        else
          ++parameters;
        expect(block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == conversion.id,
               "checked conversion retains its exact Invoke before the "
               "argument diamonds");
        const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                           entry.id};
        for (int mutation = 0; mutation != 5; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(malformed, address);
          auto *changedBlock = body->findBlock(block.id);
          auto found = std::find_if(changedBlock->instructions.begin(),
                                    changedBlock->instructions.end(),
                                    [&](const auto &candidate) {
                                      return candidate.id == conversion.id;
                                    });
          if (mutation == 0)
            found->intrinsic = lang::IntrinsicKind::IntegerCheckedAdd;
          else if (mutation == 1)
            found->operands.push_back(found->operands.front());
          else if (mutation == 2)
            found->receiver = found->operands.front();
          else if (mutation == 3) {
            auto &term = changedBlock->terminator;
            term.kind = lang::MirTerminatorKind::Goto;
            term.invokeInstruction = 0;
            term.failureRecord = 0;
            term.elseTarget = 0;
          } else
            std::swap(changedBlock->terminator.target,
                      changedBlock->terminator.elseTarget);
          // The numeric intrinsic vocabulary is also checked by admission;
          // generic MIR verification independently owns the operand and
          // failure schedule, not every intrinsic's representation family.
          if (mutation != 0)
            expect(!lang::verifyMirProgram(malformed).valid(),
                   "forged conversion arity, receiver or failure schedule "
                   "must fail MIR verification: " +
                       std::to_string(mutation));
          expect(!lang::CppMirBodyEmitter(malformed, map)
                      .supportsFailureBodyText(address),
                 "numeric staging does not admit malformed conversion "
                 "schedules: " +
                     std::to_string(mutation));
        }
      }
    }
  }
  expect(aliases == 1 && parameters == 1,
         "fixture must cover alias and concrete type-parameter conversions");
}

void testGenericScalarConstructedFieldPublication() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/associative_node_constructor.gti";
  std::ifstream input(fixture);
  expect(input.good(), "generic constructed-field fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "generic scalar node constructors must verify");
  if (!frontend.canGenerateCode())
    return;
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "generic scalar move constructors must publish native fields");
  std::size_t nodes = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    if (constructor.initializers.size() != 3 ||
        constructor.initializers.front().targetType.kind !=
            lang::SemanticType::Class)
      continue;
    const lang::MirInstruction *construct = nullptr;
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Construct &&
            instruction.operands.size() == 2 &&
            testFieldPublication(constructor.body, instruction))
          construct = &instruction;
    expect(construct != nullptr,
           "node entry must retain both exact scalar moved arguments");
    if (!construct)
      continue;
    const auto *publication =
        testFieldPublication(constructor.body, *construct);
    const auto *stagedValue =
        constructor.body.findValue(construct->operands.front().value);
    const auto *stage =
        stagedValue ? testInstruction(constructor.body, stagedValue->definition)
                    : nullptr;
    expect(stage && stage->kind == lang::MirInstructionKind::CallInput &&
               stage->callInputIndex == 0 && publication &&
               publication->constructorInitializer == 1,
           "generic construction retains ordered input and field identity");
    if (!stage || !publication)
      continue;
    ++nodes;
    for (int mutation = 0; mutation < 4; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      for (auto &block : changed->body.blocks) {
        for (auto &instruction : block.instructions) {
          if (mutation == 0 && instruction.id == stage->id)
            instruction.operands.front().type =
                lang::SemanticType(lang::SemanticType::Bool);
          if (mutation == 1 && instruction.id == stage->id)
            instruction.callInputIndex = 1;
          if (mutation == 2 && instruction.id == publication->id)
            instruction.constructorInitializer = 2;
        }
      }
      if (mutation == 3) {
        auto *target = const_cast<lang::MirConstructorInstance *>(
            malformed.findConstructorInstance(*construct->constructorTarget));
        for (auto &instruction : target->body.blocks.front().instructions)
          if (instruction.kind == lang::MirInstructionKind::Lifecycle)
            instruction.fullExpressionEnd = 0;
      }
      expect(!lang::verifyMirProgram(malformed).valid(),
             "forged moved type, argument index, field identity, or target "
             "boundary must reject");
    }
  }
  expect(nodes == 2,
         "both concrete generic node instances must exercise the proof");
}

void testRawPointerConstructorFieldPublication() {
  const auto frontend = analyzeWithStandardLibrary(
      "cpp-mir-raw-pointer-constructor-field.gti", R"gti(
mut int drops = 0;
class Empty {
public:
  Empty() {}
  void use() {}
};
class Owner {
  Empty empty{};
  void* data = nullptr;
  void* other = nullptr;
  int* emptyPointer = nullptr;
  int value;
public:
  Owner(void* data, void* other, int value)
      : data(data), other(other), value(value) {}
  ~Owner() { this.empty.use(); drops++; }
};
int main() {
  { Owner value{nullptr, nullptr, 1}; }
  return drops == 1 ? 0 : 1;
}
)gti");
  expect(frontend.canGenerateCode(),
         "raw pointer fields beside a default class field must verify");
  if (!frontend.canGenerateCode())
    return;
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "raw pointer parameter fields must preserve native construction");
  std::size_t checked = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto bindings =
        lang::cppMirCopyParameterFieldBindings(frontend.mir, constructor);
    if (!bindings)
      continue;
    std::vector<lang::CppMirCopyParameterFieldBinding> pointers;
    for (const auto &binding : *bindings)
      if (constructor.parameterTypes[binding.parameter].kind ==
          lang::SemanticType::RawPointer)
        pointers.push_back(binding);
    if (pointers.empty())
      continue;
    expect(pointers.size() == 2,
           "each raw pointer field requires its own exact publication");
    if (pointers.size() != 2)
      continue;
    ++checked;
    const auto &first = pointers.front();
    const auto &second = pointers.back();
    const auto *otherPublication =
        testInstruction(constructor.body, second.initializeInstruction);
    expect(first.initializeInstruction != 0 && otherPublication,
           "raw pointer copies cannot be inferred only from HIR metadata");
    if (!otherPublication)
      continue;
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      for (auto &block : changed->body.blocks) {
        if (mutation == 0)
          std::erase_if(block.instructions, [&](const auto &instruction) {
            return instruction.id == first.initializeInstruction;
          });
        for (auto &instruction : block.instructions) {
          if (instruction.id == first.initializeInstruction) {
            if (mutation == 1)
              instruction.destination = otherPublication->destination;
            if (mutation == 2)
              instruction.operands.front().value = second.loadedValue;
          }
          if (mutation == 3 && instruction.id == first.loadInstruction)
            instruction.ownership->reachable = false;
        }
      }
      const auto rejected =
          lang::cppMirCopyParameterFieldBindings(malformed, *changed);
      expect(!rejected || std::none_of(rejected->begin(), rejected->end(),
                                       [&](const auto &binding) {
                                         return binding.field == first.field;
                                       }),
             "raw pointer publication rejects missing initialization, "
             "same-typed wrong field/value and forged source ownership " +
                 std::to_string(mutation));
    }
    const auto fields = lang::cppMirConstructedFieldInitializerBindings(
        frontend.mir, constructor);
    const auto nullField =
        std::find_if(fields.begin(), fields.end(), [](const auto &binding) {
          return binding.fieldType.kind == lang::SemanticType::RawPointer;
        });
    expect(nullField != fields.end(),
           "typed null pointer default must retain exact field publication");
    if (nullField == fields.end())
      continue;
    for (int mutation = 0; mutation != 2; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      for (auto &block : changed->body.blocks)
        for (auto &instruction : block.instructions) {
          if (mutation == 0 &&
              instruction.id == nullField->initializeInstruction) {
            auto *place = const_cast<lang::MirPlace *>(
                changed->body.findPlace(*instruction.destination));
            place->type = constructor.parameterTypes[first.parameter];
          }
          if (mutation == 1 &&
              instruction.id == nullField->constructInstruction) {
            instruction.info.type = lang::SemanticType::Int32;
            instruction.literal = std::uint64_t{1};
            auto *value = const_cast<lang::MirValue *>(
                changed->body.findValue(*instruction.result));
            value->info.type = lang::SemanticType::Int32;
          }
          if (mutation == 1 &&
              instruction.id == nullField->initializeInstruction)
            instruction.operands.front().type = lang::SemanticType::Int32;
        }
      expect(!lang::verifyMirProgram(malformed).valid() &&
                 lang::cppMirConstructedFieldInitializerBindings(malformed,
                                                                 *changed)
                     .empty(),
             "nullable field publication rejects a wrong pointer type or "
             "non-null integer source " +
                 std::to_string(mutation));
    }
  }
  expect(checked == 1,
         "raw pointer fixture must exercise the exact copy publication gate");
}

void testPassiveClassFieldRollback() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/constructor_passive_field_rollback.gti";
  std::ifstream input(fixture);
  expect(input.good(), "passive class field rollback fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "passive field rollback must verify");
  if (!frontend.canGenerateCode())
    return;
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "passive fields must not prevent exact owning constructor rollback");
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  std::size_t checked = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *owner = frontend.mir.findClassInstance(constructor.owner);
    if (!owner || !owner->destructor || owner->declaredFields.size() != 3 ||
        constructor.parameterTypes !=
            std::vector<lang::SemanticType>{lang::SemanticType::Int32})
      continue;
    ++checked;
    const auto &field = owner->declaredFields.front();
    const auto passive = std::find_if(
        frontend.mir.classInstances().begin(),
        frontend.mir.classInstances().end(),
        [&](const auto &instance) { return instance.type == field.type; });
    expect(passive != frontend.mir.classInstances().end() &&
               !field.requiresActiveCleanup,
           "fixture must contain a cleanup-free lexical class field");
    if (passive == frontend.mir.classInstances().end())
      continue;
    for (int mutation = 0; mutation != 5; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirClassInstance *>(
          malformed.findClassInstance(passive->id));
      if (mutation == 0)
        changed->requiresActiveCleanup = true;
      if (mutation == 1)
        changed->destructorStatus = lang::SpecialMemberStatus::Declared;
      if (mutation == 2) {
        auto *changedOwner = const_cast<lang::MirClassInstance *>(
            malformed.findClassInstance(owner->id));
        changedOwner->fieldDropOrder.pop_back();
      }
      if (mutation >= 3) {
        auto *changedConstructor = const_cast<lang::MirConstructorInstance *>(
            malformed.findConstructorInstance(constructor.id));
        bool changedPublication = false;
        for (auto &block : changedConstructor->body.blocks) {
          for (auto &instruction : block.instructions) {
            if (instruction.kind != lang::MirInstructionKind::Initialize ||
                instruction.constructorInitializer != 1)
              continue;
            if (mutation == 3) {
              instruction.constructorInitializer = 2;
            } else {
              auto *destination = const_cast<lang::MirPlace *>(
                  changedConstructor->body.findPlace(*instruction.destination));
              destination->projections.front().field =
                  owner->declaredFields[1].symbol;
            }
            changedPublication = true;
          }
        }
        expect(changedPublication,
               "passive field mutation must corrupt its exact publication");
      }
      expect(!lang::verifyMirProgram(malformed).valid() &&
                 !lang::CppMirBodyEmitter(malformed, map)
                      .supportsFailureBodyText(
                          {lang::MirBodyKind::Constructor, constructor.id}),
             "passive shell proof rejects active cleanup, user destruction, "
             "an incomplete drop schedule or mismatched field publication " +
                 std::to_string(mutation));
    }
    std::size_t transports = 0;
    for (const auto &block : constructor.body.blocks) {
      for (const auto &move : block.instructions) {
        if (move.kind != lang::MirInstructionKind::Move || !move.result ||
            move.operands.size() != 1)
          continue;
        const auto *source =
            constructor.body.findPlace(move.operands.front().place);
        const auto uses = constructor.body.usesOf(*move.result);
        const auto *assignment =
            uses.size() == 1
                ? testInstruction(constructor.body, uses.front().instruction)
                : nullptr;
        if (!source || source->root != lang::MirPlaceRootKind::Temporary ||
            !assignment || assignment->kind != lang::MirInstructionKind::Assign)
          continue;
        ++transports;
        for (int mutation = 0; mutation != 4; ++mutation) {
          auto malformed = frontend.mir;
          auto *changedConstructor = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(constructor.id));
          auto *changedSource = const_cast<lang::MirPlace *>(
              changedConstructor->body.findPlace(source->id));
          if (mutation == 0)
            changedSource->initiallyAvailable = true;
          if (mutation == 1) {
            auto *destination = const_cast<lang::MirPlace *>(
                changedConstructor->body.findPlace(*assignment->destination));
            destination->projections.front().field = field.symbol;
          }
          if (mutation == 2) {
            auto &classes = const_cast<std::vector<lang::MirClassInstance> &>(
                malformed.classInstances());
            for (auto &type : classes)
              if (type.type == source->type)
                type.moveAssignment = lang::SpecialMemberStatus::Deleted;
          }
          if (mutation == 3) {
            auto *changedOwner = const_cast<lang::MirClassInstance *>(
                malformed.findClassInstance(constructor.owner));
            const auto *destination =
                constructor.body.findPlace(*assignment->destination);
            for (auto &member : changedOwner->declaredFields)
              if (member.symbol == destination->projections.front().field)
                member.access = lang::AccessMode::ReadOnly;
          }
          expect(!lang::CppMirBodyEmitter(malformed, map)
                      .supportsFailureBodyText(
                          {lang::MirBodyKind::Constructor, constructor.id}),
                 "passive assignment requires exact empty source, receiver "
                 "mutable field and generated assignment " +
                     std::to_string(mutation));
        }
      }
    }
    expect(transports == 1,
           "fixture must exercise one passive temporary-to-field assignment");
    std::size_t fieldCopies = 0;
    for (const auto &function : frontend.mir.functionInstances()) {
      if (function.owner != owner->id ||
          function.parameterTypes !=
              std::vector<lang::SemanticType>{owner->declaredFields[1].type})
        continue;
      const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                         function.id};
      for (const auto &block : function.body.blocks) {
        for (const auto &load : block.instructions) {
          if (load.kind != lang::MirInstructionKind::Load || !load.result ||
              load.info.type != owner->declaredFields[1].type)
            continue;
          const auto uses = function.body.usesOf(*load.result);
          const auto *assignment =
              uses.size() == 1
                  ? testInstruction(function.body, uses.front().instruction)
                  : nullptr;
          if (!assignment ||
              assignment->kind != lang::MirInstructionKind::Assign)
            continue;
          ++fieldCopies;
          for (int mutation = 0; mutation != 3; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = lang::findMirBody(malformed, address);
            if (mutation == 0)
              for (auto &changedBlock : changed->blocks)
                for (auto &instruction : changedBlock.instructions)
                  if (instruction.id == load.id)
                    instruction.ownership->reachable = false;
            auto &classes = const_cast<std::vector<lang::MirClassInstance> &>(
                malformed.classInstances());
            for (auto &type : classes) {
              if (mutation == 1 && type.type == load.info.type)
                type.copyAssignment = lang::SpecialMemberStatus::Deleted;
              if (mutation == 2 && type.id == owner->id)
                type.declaredFields[1].access = lang::AccessMode::ReadOnly;
            }
            const lang::CppMirBodyEmitter rejected(malformed, map);
            expect(!rejected.supportsBodyText(address) &&
                       !rejected.supportsFailureBodyText(address),
                   "passive receiver-field copy rejects forged reads, deleted "
                   "assignment and immutable fields " +
                       std::to_string(mutation));
          }
        }
      }
    }
    expect(
        fieldCopies == 1,
        "fixture must exercise one passive copied receiver-field assignment");
    std::size_t freshAssignments = 0;
    for (const auto &function : frontend.mir.functionInstances()) {
      if (function.owner != owner->id ||
          function.returnType != owner->declaredFields[1].type)
        continue;
      const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                         function.id};
      for (const auto &block : function.body.blocks) {
        for (const auto &construct : block.instructions) {
          if (construct.kind != lang::MirInstructionKind::Construct ||
              !construct.result)
            continue;
          const auto uses = function.body.usesOf(*construct.result);
          const auto *assignment =
              uses.size() == 1
                  ? testInstruction(function.body, uses.front().instruction)
                  : nullptr;
          if (!assignment ||
              assignment->kind != lang::MirInstructionKind::Assign)
            continue;
          ++freshAssignments;
          for (int mutation = 0; mutation != 5; ++mutation) {
            auto malformed = frontend.mir;
            auto *changed = lang::findMirBody(malformed, address);
            for (auto &changedBlock : changed->blocks)
              for (auto &instruction : changedBlock.instructions) {
                if (instruction.id == assignment->id && mutation == 0) {
                  auto *destination = const_cast<lang::MirPlace *>(
                      changed->findPlace(*instruction.destination));
                  destination->projections.front().field = field.symbol;
                }
                if (instruction.id == construct.id && mutation == 3)
                  instruction.constructorTarget = 0;
                if (instruction.id == construct.id && mutation == 4)
                  instruction.unsafeOperation =
                      lang::UnsafeOperationKind::RawDereference;
              }
            auto &classes = const_cast<std::vector<lang::MirClassInstance> &>(
                malformed.classInstances());
            for (auto &type : classes) {
              if (mutation == 1 && type.type == construct.info.type)
                type.moveAssignment = lang::SpecialMemberStatus::Deleted;
              if (mutation == 2 && type.id == owner->id)
                type.declaredFields[1].access = lang::AccessMode::ReadOnly;
            }
            const lang::CppMirBodyEmitter rejected(malformed, map);
            expect(!rejected.supportsBodyText(address) &&
                       !rejected.supportsFailureBodyText(address),
                   "fresh receiver-field assignment rejects wrong field, "
                   "lifecycle, mutability, target and unsafe metadata " +
                       std::to_string(mutation));
          }
        }
      }
    }
    expect(freshAssignments == 1,
           "fixture must exercise one fresh receiver-field assignment");
  }
  expect(checked == 1, "rollback fixture must exercise one guarded owner");
}

void testBoundPassiveClassFieldRollback() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/constructor_bound_passive_field_rollback.gti";
  std::ifstream input(fixture);
  expect(input.good(), "bound passive rollback fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "bound passive rollback must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  std::size_t checked = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *owner = frontend.mir.findClassInstance(constructor.owner);
    if (!owner || owner->sourceUnit != frontend.sourceGraph.entryUnit() ||
        !owner->destructor || owner->declaredFields.size() != 2)
      continue;
    const auto copied =
        lang::cppMirCopyParameterFieldBindings(frontend.mir, constructor);
    expect(copied && copied->size() == 1,
           "bound field requires one exact native copy publication");
    if (!copied || copied->size() != 1)
      continue;
    ++checked;
    const auto &binding = copied->front();
    const auto &field = owner->declaredFields.front();
    expect(!lang::cppMirFailureConstructorEmptyStateTypeEligible(frontend.mir,
                                                                 field.type),
           "an inert bound reference shell is not an empty-state witness");
    const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                       constructor.id};
    expect(lang::CppMirBodyEmitter(frontend.mir, map)
               .supportsFailureBodyText(address),
           "exact bound passive field prefix supports constructor rollback");
    for (int mutation = 0; mutation != 5; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      auto *entry = const_cast<lang::MirBlock *>(
          changed->body.findBlock(changed->body.entry));
      if (mutation == 0)
        std::swap(entry->instructions[0], entry->instructions[4]);
      if (mutation == 1)
        std::erase_if(entry->instructions, [&](const auto &instruction) {
          return instruction.id == binding.initializeInstruction;
        });
      if (mutation == 2)
        entry->instructions[2].operands.front().value = 0;
      auto &classes = const_cast<std::vector<lang::MirClassInstance> &>(
          malformed.classInstances());
      for (auto &type : classes) {
        if (type.type != field.type)
          continue;
        if (mutation == 3)
          type.destructorStatus = lang::SpecialMemberStatus::Declared;
        if (mutation == 4)
          type.declaredFields.front().type.referenceAccess =
              lang::AccessMode::Mutable;
      }
      expect(!lang::CppMirBodyEmitter(malformed, map)
                  .supportsFailureBodyText(address),
             "bound shell rejects reordered/missing/wrong publication, "
             "user destruction or exclusive fields " +
                 std::to_string(mutation));
    }
  }
  expect(checked == 1, "fixture must exercise one bound passive prefix");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "bound passive prefix must reach production emission");
}

void testNativeStatusFieldRollback() {
  for (const auto name : {"constructor_status_field_rollback.gti",
                          "constructor_status_copied_field_rollback.gti"}) {
    const auto fixture =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
    std::ifstream input(fixture);
    expect(input.good(), "native status field fixture must exist");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
    expect(frontend.canGenerateCode(),
           "native status field fixture must verify");
    if (!frontend.canGenerateCode())
      return;
    const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
    std::size_t checked = 0;
    for (const auto &constructor : frontend.mir.constructorInstances()) {
      const auto fields = lang::cppMirConstructedFieldInitializerBindings(
          frontend.mir, constructor);
      if (fields.size() != 1 || !fields.front().failureStatus)
        continue;
      ++checked;
      const auto &field = fields.front();
      const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                         constructor.id};
      const lang::CppMirBodyEmitter emitter(frontend.mir, map);
      expect(emitter.supportsFailureBodyText(address) &&
                 !emitter.supportsBodyText(address),
             "raising native field must preserve a status-only body boundary");
      if (emitter.supportsFailureBodyText(address)) {
        const auto text =
            emitter.emitFailureBodyText(address, "native-status", 1).text;
        const auto status = "__gti_mir_construct_success_" +
                            std::to_string(field.constructInstruction);
        expect(
            text.find("const bool " + status +
                      " = *__gti_mir_constructor_success;") !=
                    std::string::npos &&
                text.find("if (" + status + ")") != std::string::npos,
            "native child's captured result controls its original MIR Invoke");
      }
      const auto *entryBlock =
          constructor.body.findBlock(constructor.body.entry);
      const bool copiedInput =
          entryBlock->instructions.front().preparedParameterDrop.has_value();
      if (copiedInput) {
        const auto *child =
            frontend.mir.findConstructorInstance(field.constructor);
        const auto copies =
            lang::cppMirCopyParameterFieldBindings(frontend.mir, *child);
        expect(copies && copies->size() == 1,
               "copied native child retains one exact parameter publication");
        if (copies && copies->size() == 1) {
          const auto childText =
              emitter
                  .emitFailureBodyText(
                      {lang::MirBodyKind::Constructor, child->id},
                      "copied-formal", 1)
                  .text;
          const auto &copy = copies->front();
          expect(childText.find(
                     "auto &__gti_mir_p_" + std::to_string(copy.sourcePlace) +
                     " = __gti_mir_arg_" + std::to_string(copy.parameter) +
                     ";") != std::string::npos,
                 "post-publication copied formal projections use an alias, "
                 "not a second object or an undeclared local");
        }
      }
      for (int mutation = 0; mutation != (copiedInput ? 8 : 6); ++mutation) {
        auto malformed = frontend.mir;
        auto *changed = const_cast<lang::MirConstructorInstance *>(
            malformed.findConstructorInstance(constructor.id));
        auto *entry = const_cast<lang::MirBlock *>(
            changed->body.findBlock(changed->body.entry));
        if (mutation == 0)
          entry->terminator.failureRecord = 0;
        if (mutation == 1)
          entry->terminator.elseTarget = entry->terminator.target;
        if (mutation == 2)
          entry->instructions.back().constructorTarget = 0;
        if (mutation == 3)
          entry->instructions.front().callSite = 0;
        if (mutation == 4) {
          const auto *target =
              malformed.findConstructorInstance(field.constructor);
          auto *child = const_cast<lang::MirClassInstance *>(
              malformed.findClassInstance(target->owner));
          child->requiresActiveCleanup = false;
        }
        if (mutation == 5) {
          auto *success = const_cast<lang::MirBlock *>(
              changed->body.findBlock(entry->terminator.target));
          success->instructions.front().lifecycle.front().target =
              field.valueDrop;
        }
        if (mutation == 6)
          entry->instructions.front().lifecycle.front().target = 0;
        if (mutation == 7)
          entry->instructions.back().lifecycle.front().source = 0;
        expect(
            lang::cppMirConstructedFieldInitializerBindings(malformed, *changed)
                    .empty() &&
                !lang::CppMirBodyEmitter(malformed, map)
                     .supportsFailureBodyText(address),
            "native status prefix rejects wrong failure identity, target, "
            "input, child cleanup, rollback publication and prepared "
            "transfer " +
                std::to_string(mutation));
      }
    }
    expect(checked == 1,
           "fixture must exercise one raising native field prefix");
    expect(!gti_test::emitCpp(frontend).contents.empty(),
           "native field status must reach production emission");
  }
}

void testPassiveBorrowedConstructedFieldPrefix() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/cleanup_bound_nested_view.gti";
  std::ifstream input(fixture);
  expect(input.good(), "nested passive carrier fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "nested passive carrier must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  std::size_t checked = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *owner = frontend.mir.findClassInstance(constructor.owner);
    if (!owner || owner->destructor || owner->declaredFields.size() != 2 ||
        !owner->traits.containsBorrowedState)
      continue;
    ++checked;
    const auto fields = lang::cppMirConstructedFieldInitializerBindings(
        frontend.mir, constructor);
    expect(fields.size() == 1,
           "passive nested construction retains one exact native prefix");
    if (fields.size() != 1)
      continue;
    const auto &field = fields.front();
    expect(
        field.valueDrop == 0 && field.fieldDrop == 0 && field.slotPlace == 0 &&
            !field.failureStatus &&
            !lang::cppMirFailureConstructorEmptyStateTypeEligible(
                frontend.mir, field.fieldType),
        "bound passive child has no invented cleanup or empty-state authority");
    const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                       constructor.id};
    expect(lang::CppMirBodyEmitter(frontend.mir, map)
               .supportsFailureBodyText(address),
           "native passive prefix preserves later body instructions");
    for (int mutation = 0; mutation != 12; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      auto *entry = const_cast<lang::MirBlock *>(
          changed->body.findBlock(changed->body.entry));
      auto &construct = entry->instructions.back();
      auto *success = const_cast<lang::MirBlock *>(
          changed->body.findBlock(entry->terminator.target));
      auto *failure = const_cast<lang::MirBlock *>(
          changed->body.findBlock(entry->terminator.elseTarget));
      auto *sourceLoan =
          const_cast<lang::MirLoan *>(changed->body.findLoan(*construct.loan));
      auto *published = const_cast<lang::MirLoan *>(
          changed->body.findLoan(*success->instructions[1].loan));
      if (mutation == 0)
        sourceLoan->lifecycleBound = true;
      if (mutation == 1)
        published->lifecycleBound = true;
      if (mutation == 2)
        entry->terminator.successLifecycle.push_back(
            {.kind = lang::MirLifecycleEventKind::Initialize, .target = 1});
      if (mutation == 3)
        success->instructions.front().lifecycle.push_back(
            {.kind = lang::MirLifecycleEventKind::Reparent,
             .source = 1,
             .target = 2});
      if (mutation == 4)
        success->instructions[1].operands.front().value = 0;
      if (mutation == 5)
        published->source = 0;
      if (mutation == 6)
        entry->terminator.failureRecord = 0;
      if (mutation == 7 || mutation == 8) {
        const auto *target =
            malformed.findConstructorInstance(field.constructor);
        auto *child = const_cast<lang::MirClassInstance *>(
            malformed.findClassInstance(target->owner));
        if (mutation == 7)
          child->declaredFields.front().type.referenceAccess =
              lang::AccessMode::Mutable;
        else
          child->destructorStatus = lang::SpecialMemberStatus::Declared;
      }
      if (mutation == 9)
        success->instructions[2].loan = published->id;
      if (mutation == 10)
        failure->instructions.clear();
      if (mutation == 11)
        construct.constructorTarget = 0;
      expect(
          lang::cppMirConstructedFieldInitializerBindings(malformed, *changed)
                  .empty() &&
              !lang::CppMirBodyEmitter(malformed, map)
                   .supportsFailureBodyText(address),
          "passive prefix rejects invented cleanup, lost exact publication, "
          "failure identity, unsafe shell shape and loan retirement " +
              std::to_string(mutation));
    }
  }
  expect(checked == 1,
         "fixture must exercise one passive borrowed child prefix");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "nested passive constructor and copied field return reach production");
}

void testNestedConstructorArgumentPublication() {
  std::size_t checked = 0;
  std::size_t nested = 0;
  std::size_t movedFormals = 0;
  std::size_t passiveMovedFormals = 0;
  for (const auto name : {"nested_carrier_arguments.gti",
                          "nested_owning_constructor_arguments.gti",
                          "source_resource_prefix_cleanup.gti",
                          "nested_cleanup_free_moved_formal.gti"}) {
    const auto fixture =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
    std::ifstream input(fixture);
    expect(input.good(), "nested argument fixture must exist");
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
    expect(frontend.canGenerateCode(), "nested argument fixture must verify");
    if (!frontend.canGenerateCode())
      continue;
    const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
    for (const auto &constructor : frontend.mir.constructorInstances()) {
      const auto fields = lang::cppMirConstructedFieldInitializerBindings(
          frontend.mir, constructor);
      for (const auto &field : fields) {
        const bool tree =
            std::any_of(field.arguments.begin(), field.arguments.end(),
                        [](const auto &argument) {
                          return argument.constructor.has_value();
                        });
        const bool formal = std::any_of(
            field.arguments.begin(), field.arguments.end(),
            [](const auto &argument) {
              return argument.moved && argument.parameter.has_value();
            });
        if (!tree && !formal)
          continue;
        ++checked;
        nested += tree;
        movedFormals += formal;
        const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                           constructor.id};
        expect(
            field.constructBlock != constructor.body.entry || formal,
            "nested argument publication follows its actual successful prefix");
        expect(
            field.argumentInstructions.size() == field.argumentValues.size(),
            "every claimed nested producer and input has one exact SSA result");
        expect(lang::CppMirBodyEmitter(frontend.mir, map)
                   .supportsFailureBodyText(address),
               "nested argument tree preserves the outer status and rollback "
               "route");
        for (int mutation = 0; mutation != 13; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(constructor.id));
          const auto instruction =
              [&](lang::MirInstructionId id) -> lang::MirInstruction * {
            for (auto &block : changed->body.blocks)
              for (auto &candidate : block.instructions)
                if (candidate.id == id)
                  return &candidate;
            return nullptr;
          };
          auto *root = instruction(field.constructInstruction);
          auto *block = const_cast<lang::MirBlock *>(
              changed->body.findBlock(field.constructBlock));
          auto *success = const_cast<lang::MirBlock *>(
              changed->body.findBlock(block->terminator.target));
          auto *failure = const_cast<lang::MirBlock *>(
              changed->body.findBlock(block->terminator.elseTarget));
          auto *stage = instruction(field.argumentInstructions.front());
          auto *prepared = stage;
          for (const auto id : field.argumentInstructions)
            if (auto *candidate = instruction(id);
                candidate->preparedParameterDrop)
              prepared = candidate;
          expect(root && block && success && failure && stage &&
                     prepared->preparedParameterDrop,
                 "nested tree retains concrete producer, stage and failure "
                 "identities");
          if (!root || !block || !success || !failure || !stage ||
              !prepared->preparedParameterDrop)
            continue;
          if (mutation == 0)
            stage->info.type = lang::SemanticType::Int32;
          if (mutation == 1)
            prepared->callSite = 0;
          if (mutation == 2)
            prepared->callInputIndex = 999;
          if (mutation == 3)
            prepared->callInputKind = lang::HirCallInputKind::Value;
          if (mutation == 4)
            prepared->lifecycle.clear();
          if (mutation == 5)
            root->lifecycle.push_back(
                {.kind = lang::MirLifecycleEventKind::TransferOut,
                 .source = 999});
          if (mutation == 6)
            block->terminator.failureRecord = 0;
          if (mutation == 7)
            success->instructions[1].operands.front().value = 0;
          if (mutation == 8)
            failure->activeFailure = 0;
          if (mutation == 9)
            const_cast<lang::MirLoan *>(changed->body.findLoan(*root->loan))
                ->escapes = true;
          if (mutation == 10)
            instruction(field.initializeInstruction)->info.type =
                lang::SemanticType::Int32;
          if (mutation == 11)
            instruction(field.boundaryInstruction)->fullExpressionEnd = 0;
          if (mutation == 12) {
            auto *drop = const_cast<lang::MirDropObligation *>(
                changed->body.findDropObligation(
                    *prepared->preparedParameterDrop));
            drop->initiallyActive = true;
          }
          expect(lang::cppMirConstructedFieldInitializerBindings(malformed,
                                                                 *changed)
                         .empty() &&
                     !lang::CppMirBodyEmitter(malformed, map)
                          .supportsFailureBodyText(address),
                 "nested tree rejects lost types, input order, transfer, "
                 "publication, "
                 "rollback and lifetime activation " +
                     std::to_string(mutation) + " in " + name);
        }
        if (tree) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(constructor.id));
          const auto argument =
              std::find_if(field.arguments.begin(), field.arguments.end(),
                           [](const auto &candidate) {
                             return candidate.constructor.has_value();
                           });
          auto *child = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(*argument->constructor));
          child->definitionKind = lang::MirDefinitionKind::Declaration;
          expect(lang::cppMirConstructedFieldInitializerBindings(malformed,
                                                                 *changed)
                     .empty(),
                 "unproved inner constructor cannot move across its Invoke "
                 "boundary");
        }
        // A cleanup-free moved formal has no Value handoff: its exact Move
        // event, formal identity and passive obligations carry the proof.
        bool passive = false;
        for (int mutation = 0; formal && mutation != 4; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(constructor.id));
          lang::MirInstruction *move = nullptr;
          for (auto &block : changed->body.blocks)
            for (auto &candidate : block.instructions)
              if (!move && candidate.kind == lang::MirInstructionKind::Move &&
                  std::find(field.argumentInstructions.begin(),
                            field.argumentInstructions.end(),
                            candidate.id) != field.argumentInstructions.end())
                move = &candidate;
          const auto *place =
              move ? changed->body.findPlace(move->operands.front().place)
                   : nullptr;
          lang::MirDropObligation *formalDrop = nullptr;
          for (auto &drop : changed->body.dropObligations)
            if (place && drop.kind == lang::MirDropObligationKind::Binding &&
                drop.place == place->id)
              formalDrop = &drop;
          if (!move || !place || !formalDrop ||
              formalDrop->dropType.requiresActiveCleanup)
            break;
          passive = true;
          if (mutation == 0)
            move->ownership->kind = lang::OwnershipEventKind::Read;
          if (mutation == 1)
            formalDrop->dropType.requiresActiveCleanup = true;
          if (mutation == 2)
            move->lifecycle.push_back(
                {.kind = lang::MirLifecycleEventKind::Move,
                 .source = formalDrop->id,
                 .target = formalDrop->id});
          if (mutation == 3)
            for (auto &binding : changed->parameterBindings)
              if (binding == place->binding)
                binding = 0;
          expect(lang::cppMirConstructedFieldInitializerBindings(malformed,
                                                                 *changed)
                         .empty() &&
                     !lang::CppMirBodyEmitter(malformed, map)
                          .supportsFailureBodyText(address),
                 "cleanup-free moved formal rejects a lost Move event, "
                 "cleanup-bearing formal, invented handoff or foreign "
                 "binding " +
                     std::to_string(mutation) + " in " + name);
        }
        passiveMovedFormals += passive;
      }
    }
    expect(!gti_test::emitCpp(frontend).contents.empty(),
           "nested constructor fixture reaches production emission");
  }
  expect(checked >= 4 && nested >= 3 && movedFormals >= 1 &&
             passiveMovedFormals >= 5,
         "fixtures cover shared, exclusive, owning, moved resource-formal "
         "and cleanup-free moved-formal trees");
}

void testPhysicalCarrierReferencePublication() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/borrowed_physical_adapter.gti";
  std::ifstream input(fixture);
  expect(input.good(), "physical carrier adapter fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "physical carrier adapter must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  std::size_t fields = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &loan : constructor.body.loans) {
      if (loan.kind != lang::MirLoanKind::Stored || !loan.escapes ||
          loan.storedField == 0 || loan.storedDependency == 0)
        continue;
      ++fields;
      const auto bindings = lang::cppMirStoredReferenceBindings(constructor);
      expect(bindings && bindings->size() == 1,
             "shared physical reference binds the exact native formal");
      for (int mutation = 0; mutation != 6; ++mutation) {
        auto changed = constructor;
        auto *stored =
            const_cast<lang::MirLoan *>(changed.body.findLoan(loan.id));
        auto *dependency = const_cast<lang::MirLoan *>(
            changed.body.findLoan(loan.storedDependency));
        if (mutation == 0)
          stored->storedDependency = 0;
        if (mutation == 1)
          dependency->source = 0;
        if (mutation == 2)
          dependency->designatedFields.clear();
        if (mutation == 3)
          for (auto &shell : changed.body.loans)
            if (shell.kind == lang::MirLoanKind::Parameter)
              shell.storedDependency = 0;
        if (mutation == 4)
          for (auto &block : changed.body.blocks)
            for (auto &instruction : block.instructions)
              if (instruction.kind == lang::MirInstructionKind::Borrow &&
                  instruction.loan == loan.id)
                instruction.operands.front().place = 0;
        if (mutation == 5)
          changed.parameterTypes.front() = lang::SemanticType::Int32;
        expect(!lang::cppMirStoredReferenceBindings(changed),
               "physical formal publication rejects forged source/dependency, "
               "shell, producer or parameter type " +
                   std::to_string(mutation));
      }
    }
  }
  std::size_t returns = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &loan : function.body.loans) {
      if (loan.kind != lang::MirLoanKind::Return ||
          loan.storedDependency == 0 ||
          function.returnType.kind != lang::SemanticType::Class)
        continue;
      ++returns;
      const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                         function.id};
      const lang::CppMirBodyEmitter emitter(frontend.mir, map);
      expect(emitter.supportsFailureBodyText(address),
             "constructed adapter retains physical source and external "
             "dependency");
      if (emitter.supportsFailureBodyText(address)) {
        const auto text =
            emitter.emitFailureBodyText(address, "physical", 1).text;
        expect(
            text.find("stored-value return loan " + std::to_string(loan.id)) !=
                    std::string::npos &&
                text.find("__gti_mir_loan_" + std::to_string(loan.id) + " =") ==
                    std::string::npos,
            "class result carries the borrow without an invented pointer copy");
      }
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(malformed, address);
        auto *result = const_cast<lang::MirLoan *>(body->findLoan(loan.id));
        if (mutation == 0)
          result->storedDependency = 0;
        if (mutation == 1)
          result->source = 0;
        if (mutation == 2)
          for (auto &block : body->blocks)
            for (auto &instruction : block.instructions)
              if (instruction.kind == lang::MirInstructionKind::Construct)
                instruction.constructorTarget = 0;
        expect(
            !lang::verifyMirProgram(malformed).valid(),
            "exact result provenance stays mandatory before representation " +
                std::to_string(mutation));
      }
    }
  }
  expect(
      fields == 1 && returns == 1,
      "unrelated adapter exercises one physical field and return publication");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "physical borrowed adapter must reach production emission");
}

void testGeneratedDefaultOwningFieldPublication() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/cleanup_bound_destructor_receiver.gti";
  std::ifstream input(fixture);
  expect(input.good(), "destructor receiver fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "destructor receiver must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  std::size_t checked = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &block : constructor.body.blocks) {
      for (std::size_t index = 0; index + 1 < block.instructions.size();
           ++index) {
        const auto &producer = block.instructions[index];
        const auto &initialize = block.instructions[index + 1];
        if (producer.kind != lang::MirInstructionKind::Construct ||
            producer.constructorTarget || !producer.result ||
            producer.lifecycle.size() != 1 ||
            initialize.kind != lang::MirInstructionKind::Initialize ||
            initialize.constructorInitializer == 0 ||
            initialize.lifecycle.size() != 1)
          continue;
        ++checked;
        const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                           constructor.id};
        const lang::CppMirBodyEmitter emitter(frontend.mir, map);
        expect(emitter.supportsFailureBodyText(address),
               "generated default owns exact direct field publication");
        if (emitter.supportsFailureBodyText(address)) {
          const auto text =
              emitter.emitFailureBodyText(address, "default-field", 1).text;
          expect(
              text.find("GTI MIR in-place constructor field reparent into p" +
                        std::to_string(*initialize.destination)) !=
                      std::string::npos &&
                  text.find("::new (static_cast<void *>(std::addressof(") !=
                      std::string::npos &&
                  text.find(" __gti_mir_v_" + std::to_string(*producer.result) +
                            "{};") == std::string::npos,
              "default construction arms the field once without an SSA "
              "object or assignment");
        }
        for (int mutation = 0; mutation != 8; ++mutation) {
          auto malformed = frontend.mir;
          auto *changed = const_cast<lang::MirConstructorInstance *>(
              malformed.findConstructorInstance(constructor.id));
          auto *changedBlock =
              const_cast<lang::MirBlock *>(changed->body.findBlock(block.id));
          auto &construction = changedBlock->instructions[index];
          auto &publication = changedBlock->instructions[index + 1];
          auto *valueDrop = const_cast<lang::MirDropObligation *>(
              changed->body.findDropObligation(
                  producer.lifecycle.front().target));
          if (mutation == 0)
            construction.lifecycle.clear();
          if (mutation == 1)
            publication.lifecycle.front().source = 0;
          if (mutation == 2)
            publication.lifecycle.front().target = valueDrop->id;
          if (mutation == 3)
            changed->initializers[initialize.constructorInitializer - 1]
                .arguments.front() = 0;
          if (mutation == 4)
            valueDrop->fullExpression = 0;
          if (mutation == 5)
            construction.constructorKind = lang::ConstructorKind::Copy;
          if (mutation == 6) {
            for (const auto &instance : malformed.classInstances())
              if (instance.type == producer.info.type)
                const_cast<lang::MirClassInstance &>(instance)
                    .defaultConstructor = lang::SpecialMemberStatus::Deleted;
          }
          if (mutation == 7)
            for (auto &candidateBlock : changed->body.blocks)
              for (auto &instruction : candidateBlock.instructions)
                for (auto &event : instruction.lifecycle)
                  if (event.kind == lang::MirLifecycleEventKind::TransferOut &&
                      event.source == initialize.lifecycle.front().target)
                    event.source = valueDrop->id;
          expect(!lang::CppMirBodyEmitter(malformed, map)
                      .supportsFailureBodyText(address),
                 "default field rejects forged activation, reparent, selected "
                 "initializer, full-expression, constructor and retirement " +
                     std::to_string(mutation));
        }
      }
    }
  }
  expect(checked == 1,
         "fixture must contain one active generated-default field");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "destructor receiver and owning default field reach production");
}

void testPassiveExpectedMoveReturnHasNoSsaShell() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/expected_passive_move_return.gti";
  std::ifstream input(fixture);
  expect(input.good(), "passive expected move fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "passive expected move must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  std::size_t moves = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.returnType.kind != lang::SemanticType::Expected)
      continue;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Move ||
            !instruction.result || instruction.info.type != function.returnType)
          continue;
        ++moves;
        const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                           function.id};
        const lang::CppMirBodyEmitter emitter(frontend.mir, map);
        // The relay has no failure source of its own and is proved
        // failure-free: its plain form stages the moved value and returns it
        // from that slot, where a failure form publishes at the Move.
        const bool failureForm = function.mayRaiseDefinedFailure;
        const bool supported = failureForm
                                   ? emitter.supportsFailureBodyText(address)
                                   : emitter.supportsBodyText(address);
        expect(supported,
               "passive expected uses its existing exact move-return proof");
        if (!supported)
          continue;
        const std::string value =
            "__gti_mir_v_" + std::to_string(*instruction.result);
        const auto text =
            failureForm
                ? emitter.emitFailureBodyText(address, "expected-move", 1).text
                : emitter.emitBodyText(address, "expected-move", 1).text;
        expect(text.find(" " + value + "{};") == std::string::npos &&
                   text.find(failureForm
                                 ? std::string("construct_representation_at(__"
                                               "gti_mir_out_result, std::move(")
                                 : "return std::move(" + value + ".get());") !=
                       std::string::npos,
               "return publication creates no extra default-constructed "
               "expected payload");
      }
  }
  expect(moves == 1, "fixture must contain one passive expected move return");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "non-default passive expected return must reach production emission");
}

void testProvenanceOnlyReceiverFields() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/constructor_provenance_only_fields.gti";
  std::ifstream input(fixture);
  expect(input.good(), "private receiver provenance fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "private receiver provenance must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  std::size_t checked = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &loan : function.body.loans) {
      const auto *place = function.body.findPlace(loan.source);
      if (loan.kind != lang::MirLoanKind::CallResult || !place ||
          place->root != lang::MirPlaceRootKind::This ||
          place->projections.size() < 2)
        continue;
      ++checked;
      const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                         function.id};
      const lang::CppMirBodyEmitter emitter(frontend.mir, map);
      expect(emitter.supportsFailureBodyText(address),
             "accessor pointer, not its private origin, is the runtime value");
      if (!emitter.supportsFailureBodyText(address))
        continue;
      const auto text =
          emitter.emitFailureBodyText(address, "provenance", 1).text;
      // A checked accessor writes the loan through its `T **` out-argument;
      // a failure-free one returns the reference the loan binds by address.
      const std::string loanCarrier =
          "__gti_mir_loan_" + std::to_string(loan.id);
      expect(text.find("auto &__gti_mir_p_" + std::to_string(place->id) +
                       " =") == std::string::npos &&
                 text.find("(*this).view.owner") == std::string::npos &&
                 (text.find("&" + loanCarrier) != std::string::npos ||
                  text.find(loanCarrier + " = &(") != std::string::npos) &&
                 text.find("(*this).view;") != std::string::npos &&
                 text.find("(*this).reads;") != std::string::npos,
             "erase only provenance aliases while retaining call output and "
             "executable receiver/read-write field aliases");
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto malformed = frontend.mir;
        auto *changed = lang::findMirBody(malformed, address);
        auto *origin =
            const_cast<lang::MirPlace *>(changed->findPlace(place->id));
        if (mutation == 0)
          origin->projections.front().field = 0;
        if (mutation == 1)
          origin->type = lang::SemanticType::Int32;
        if (mutation == 2)
          for (auto &changedBlock : changed->blocks)
            for (auto &instruction : changedBlock.instructions)
              if (instruction.loan == loan.id &&
                  instruction.kind == lang::MirInstructionKind::Call)
                instruction.functionTarget = 0;
        expect(!lang::CppMirBodyEmitter(malformed, map)
                    .supportsFailureBodyText(address),
               "erasure preserves exact origin rows and executable accessor "
               "producer admission " +
                   std::to_string(mutation));
      }
    }
  }
  expect(
      checked == 2,
      "fixture must contain both observed and returned private call origins");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "private provenance must reach production emission");
}

void testConservativeConstructorAssignmentFusion() {
  const auto frontend = analyzeWithStandardLibrary(
      "cpp-mir-conservative-constructor-assignment.gti", R"gti(
#include <std/memory>
interface Operation { int call(int value); };
class Passive : public Operation {
public:
  Passive() {}
  int call(int value) override { return value; }
};
int main() {
  mut auto owner = std::make_unique<Passive>();
  *owner = Passive();
  return 0;
}
)gti");
  expect(frontend.canGenerateCode(),
         "passive borrowed replacement must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "replacement fixture must contain its entry body");
  if (entry == frontend.mir.functionInstances().end())
    return;
  const lang::MirBodyAddress address{lang::MirBodyKind::Function, entry->id};
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.supportsFailureBodyText(address),
         "a conservative nonfailing constructor edge must retain adjacent "
         "borrowed replacement without inventing a temporary lifetime");
  std::size_t constructions = 0;
  for (const auto &block : entry->body.blocks) {
    const auto *producer =
        testInstruction(entry->body, block.terminator.invokeInstruction);
    const auto *success = entry->body.findBlock(block.terminator.target);
    if (!producer || producer->kind != lang::MirInstructionKind::Construct ||
        !success || success->instructions.size() < 2 ||
        success->instructions.front().kind !=
            lang::MirInstructionKind::ActivateBorrow)
      continue;
    ++constructions;
    const auto text =
        emitter.emitFailureBodyText(address, "borrowed-replace", 1).text;
    expect(text.find("fuses into binding assignment") != std::string::npos &&
               text.find("if (__gti_mir_construct_success_" +
                         std::to_string(producer->id) + ")") ==
                   std::string::npos,
           "fused construction must not leave a false status branch");
    for (int mutation = 0; mutation != 5; ++mutation) {
      auto malformed = frontend.mir;
      auto *body = lang::findMirBody(malformed, address);
      auto &changed = body->blocks[success->id - 1];
      auto &invoke = body->blocks[block.id - 1].terminator;
      if (mutation == 0)
        changed.instructions.front().loan = 0;
      if (mutation == 1)
        changed.instructions.front().fullExpressionEnd = 1;
      if (mutation == 2)
        invoke.elseTarget = invoke.target;
      if (mutation == 3)
        invoke.successLifecycle.push_back(
            {.kind = lang::MirLifecycleEventKind::Initialize, .target = 1});
      if (mutation == 4)
        std::swap(changed.instructions[0], changed.instructions[1]);
      expect(!lang::CppMirBodyEmitter(malformed, map)
                  .supportsFailureBodyText(address),
             "fusion must reject a forged activation, intervening boundary, "
             "extra edge, lifetime publication or changed evaluation order " +
                 std::to_string(mutation));
    }
  }
  expect(constructions == 1, "fixture must exercise one cross-Invoke fusion");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "polymorphic replacement must retain production emission");
}

void testNestedConstructorRepresentationPrecedence() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/issue_117_generic_indirect.gti";
  std::ifstream input(fixture);
  expect(input.good(), "nested generic construction fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "nested default constructors must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  std::size_t claimed = 0;
  std::size_t restoredPlacement = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto bindings = lang::cppMirConstructedFieldInitializerBindings(
        frontend.mir, constructor);
    for (const auto &binding : bindings) {
      const auto *producer =
          testInstruction(constructor.body, binding.constructInstruction);
      if (!binding.constructedValue || !producer ||
          producer->kind != lang::MirInstructionKind::Construct ||
          !producer->constructorTarget)
        continue;
      ++claimed;
      expect(!lang::cppMirConstructorRequiresFieldPlacement(frontend.mir,
                                                            constructor) &&
                 emitter.supportsBodyText(
                     {lang::MirBodyKind::Constructor, constructor.id}),
             "exact native initializer publication must not also require "
             "body placement or hide the ordinary constructor definition");
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      for (auto &block : changed->body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.id == binding.constructInstruction)
            instruction.callSite = instruction.hirValue + 10000;
      expect(
          lang::cppMirConstructedFieldInitializerBindings(malformed, *changed)
              .empty(),
          "a forged call identity must lose native initializer ownership");
      if (lang::cppMirConstructorRequiresFieldPlacement(malformed, *changed)) {
        ++restoredPlacement;
        expect(!lang::CppMirBodyEmitter(malformed, map)
                    .supportsBodyText(
                        {lang::MirBodyKind::Constructor, constructor.id}),
               "unclaimed direct construction must retain status-only "
               "placement, not an ordinary overload using an erased value");
      }
      malformed = frontend.mir;
      changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      for (auto &block : changed->body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.id == binding.initializeInstruction)
            instruction.constructorInitializer = 0;
      expect(!lang::verifyMirProgram(malformed).valid(),
             "native publication cannot detach its exact field identity");
    }
  }
  expect(claimed != 0 && restoredPlacement != 0,
         "fixture must exercise competing native and placement proofs");
  expect(!gti_test::emitCpp(frontend).contents.empty(),
         "nested generic constructors must publish consistent definitions");
}

void testOwningConstructedFieldInputTransfers() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/associative_owning_node_constructor.gti";
  std::ifstream input(fixture);
  expect(input.good(), "owning nested constructor fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "owning nested constructors must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "owning field construction must preserve prepared input transfers");
  std::size_t transfers = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    if (constructor.initializers.size() != 3)
      continue;
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Construct ||
            instruction.lifecycle.empty())
          continue;
        ++transfers;
        expect(instruction.successResultDrop.has_value(),
               "field result activates only on constructor success");
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(
            malformed, {lang::MirBodyKind::Constructor, constructor.id});
        for (auto &changed : body->findBlock(block.id)->instructions)
          if (changed.id == instruction.id)
            changed.lifecycle.clear();
        expect(!lang::verifyMirProgram(malformed).valid(),
               "constructor cannot omit owning input handoff");
      }
    }
  }
  expect(transfers == 2,
         "one- and two-owner field constructors must be exercised");
}

void testMovedClassArgumentBeforeCheckedArgument() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/associative_owned_argument.gti";
  std::ifstream input(fixture);
  expect(input.good(), "owned argument staging fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "owned argument staging must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "a prepared class move must survive later argument failure edges");
  std::size_t stages = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &stage : block.instructions) {
        if (stage.kind != lang::MirInstructionKind::CallInput ||
            stage.callInputKind != lang::HirCallInputKind::MoveValue ||
            stage.info.type.kind != lang::SemanticType::Class ||
            !stage.preparedParameterDrop || !stage.result || !stage.destination)
          continue;
        const auto uses = function.body.usesOf(*stage.result);
        if (uses.size() != 1 || uses.front().block == block.id)
          continue;
        ++stages;
        const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                           function.id};
        const auto text =
            emitter.emitFailureBodyText(address, "prepared", 1).text;
        const auto slot = "__gti_mir_p_" + std::to_string(*stage.destination);
        expect(
            text.find(slot + ".construct(std::move(") != std::string::npos &&
                text.find("std::move(" + slot + ".get())") !=
                    std::string::npos &&
                text.find(slot + ".destroy()") != std::string::npos,
            "prepare, call transfer, and retirement must share the exact slot");
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(malformed, address);
        body->dropObligations[*stage.preparedParameterDrop - 1]
            .initiallyActive = true;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "prepared argument cannot be active before its move initializes "
               "it");
      }
    }
  }
  expect(stages != 0,
         "fixture must move a class across a later checked argument");
}

void testBorrowedFieldCompoundAssignment() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/associative_iterator_compound.gti";
  std::ifstream input(fixture);
  expect(input.good(), "borrowed field compound assignment fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const std::pair<std::string, std::string> sources[] = {
      {fixture.string(), source},
      {"unrelated-borrowed-field-compound.gti", R"(
struct Counter { mut int value = 30; };
int main() {
  mut Counter owner{};
  mut Counter& entry = owner;
  entry.value += 1;
  return entry.value - 31;
}
)"},
  };
  for (const auto &[name, fixtureSource] : sources) {
    const auto frontend = analyzeWithStandardLibrary(name, fixtureSource);
    expect(frontend.canGenerateCode(),
           "iterator compound assignment must verify");
    if (!frontend.canGenerateCode())
      continue;
    const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
    const lang::CppMirBodyEmitter emitter(frontend.mir, map);
    expect(emitter.analyzeProgram().ready(),
           "checked compound assignment must retain its borrowed field lvalue");
    const auto definition =
        [](const lang::MirBody &body,
           lang::MirValueId valueId) -> const lang::MirInstruction * {
      const auto *value = body.findValue(valueId);
      const auto *block =
          value ? body.findBlock(value->definitionBlock) : nullptr;
      if (block != nullptr)
        for (const auto &instruction : block->instructions)
          if (instruction.id == value->definition)
            return &instruction;
      return nullptr;
    };
    std::size_t assignments = 0;
    for (const auto &function : frontend.mir.functionInstances()) {
      if (function.entryKind != lang::ProgramEntryKind::NoArguments)
        continue;
      for (const auto &block : function.body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.kind != lang::MirInstructionKind::Assign ||
              instruction.operation != lang::MirOperation::Assign ||
              !instruction.destination || instruction.operands.size() != 1 ||
              instruction.operands.front().kind != lang::MirOperandKind::Value)
            continue;
          const auto *place = function.body.findPlace(*instruction.destination);
          const bool projectedField =
              place && place->root == lang::MirPlaceRootKind::Binding &&
              place->projections.size() == 2 &&
              place->projections.front().kind ==
                  lang::MirProjectionKind::Dereference &&
              place->projections.back().kind == lang::MirProjectionKind::Field;
          const auto *loan =
              place && place->root == lang::MirPlaceRootKind::Loan
                  ? function.body.findLoan(place->loan)
                  : nullptr;
          const bool accessorReferent =
              loan && place->projections.empty() &&
              loan->kind == lang::MirLoanKind::CallResult &&
              loan->access == lang::AccessMode::Mutable;
          if (!projectedField && !accessorReferent)
            continue;
          const auto *sum =
              definition(function.body, instruction.operands.front().value);
          if (!sum || sum->kind != lang::MirInstructionKind::Compute ||
              sum->operation != lang::MirOperation::Add ||
              sum->operands.size() != 2 ||
              sum->operands.front().kind != lang::MirOperandKind::Value ||
              !sum->result)
            continue;
          ++assignments;
          const auto *read =
              definition(function.body, sum->operands.front().value);
          const auto *sumValue = function.body.findValue(*sum->result);
          const auto *sumBlock =
              sumValue ? function.body.findBlock(sumValue->definitionBlock)
                       : nullptr;
          expect(
              read && read->kind == lang::MirInstructionKind::Load &&
                  read->operands.size() == 1 &&
                  read->operands.front().kind == lang::MirOperandKind::Copy &&
                  read->operands.front().place == place->id &&
                  read->info.type == place->type &&
                  sum->info.type == place->type,
              "borrowed compound update reads the exact checked field before "
              "computing its sum");
          expect(sum->localFailureSites.size() == 1 &&
                     sum->definedFailure.localOrigins.size() == 1 && sumBlock &&
                     sumBlock->terminator.kind ==
                         lang::MirTerminatorKind::Invoke &&
                     sumBlock->terminator.invokeInstruction == sum->id &&
                     sumBlock->terminator.target == block.id &&
                     sumBlock->terminator.elseTarget != block.id,
                 "borrowed field commit occurs only after the checked addition "
                 "succeeds");
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          if (projectedField)
            body->places[place->id - 1].projections.back().field = 0;
          else
            body->loans[loan->id - 1].access = lang::AccessMode::ReadOnly;
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "compound assignment cannot forge a borrowed field projection "
                 "or mutable accessor loan");
        }
      }
    }
    expect(
        assignments == 1,
        "one checked-reference compound assignment must exercise each route: " +
            name + ": " + std::to_string(assignments));
  }
}

void testBorrowedTemporaryOperatorReceiver() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/borrowed_temporary_operator_receiver.gti";
  std::ifstream input(fixture);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "borrowed temporary operators must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
  expect(emitter.analyzeProgram().ready(),
         "borrowed operator receivers must alias their exact lifetime slot");
  std::size_t receivers = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::CallInput ||
            instruction.callInputKind !=
                lang::HirCallInputKind::ReadTemporaryBorrow ||
            instruction.callInputRole != lang::MirCallInputRole::Receiver ||
            !instruction.info.traits.containsBorrowedState)
          continue;
        ++receivers;
        const auto *loan = function.body.findLoan(instruction.loan.value_or(0));
        const auto *place =
            loan == nullptr ? nullptr : function.body.findPlace(loan->source);
        const auto dependency = place == nullptr
                                    ? lang::MirTemporaryCarrierDependency{}
                                    : lang::findMirTemporaryCarrierDependency(
                                          function.body, *place);
        expect(dependency && loan->storedDependency == dependency.loan->id &&
                   loan->parent == 0 &&
                   loan->access == lang::AccessMode::ReadOnly,
               "eligible borrowed operators reuse the exact temporary shell "
               "and stored-owner dependency schedule");
        expect(!instruction.destination && !instruction.preparedParameterDrop &&
                   instruction.lifecycle.empty(),
               "receiver identity stage must not copy, move, or own cleanup");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          for (auto &changedBlock : body->blocks) {
            for (auto &changed : changedBlock.instructions) {
              if (changed.id != instruction.id)
                continue;
              if (mutation == 0)
                changed.callSite = 0;
              else if (mutation == 1)
                changed.callInputRole = lang::MirCallInputRole::Argument;
              else
                changed.callInputKind = lang::HirCallInputKind::MoveValue;
            }
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "temporary receiver schedule must reject forged site, role, "
                 "or ownership-transfer kind");
        }
      }
    }
  }
  expect(receivers == 4, "grouped, conditional, and direct comparisons need "
                         "ordered temporary-borrow receiver stages");
}

void testPassiveNestedConstructorArgument() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/passive_nested_constructor_argument.gti";
  std::ifstream input(fixture);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "passive nested constructor argument must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  expect(lang::CppMirBodyEmitter(frontend.mir, rows).analyzeProgram().ready(),
         "source constructors can move prepared passive class arguments into "
         "fields");
  std::size_t tested = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    for (const auto &producerBlock : constructor.body.blocks) {
      for (const auto &construct : producerBlock.instructions) {
        if (construct.kind != lang::MirInstructionKind::Construct ||
            construct.parameterTypes.size() != 2 ||
            construct.parameterTypes.front().kind !=
                lang::SemanticType::Class ||
            !testFieldPublication(constructor.body, construct))
          continue;
        ++tested;
        expect(
            producerBlock.terminator.kind == lang::MirTerminatorKind::Invoke &&
                producerBlock.terminator.invokeInstruction == construct.id &&
                !construct.lifecycle.empty(),
            "prepared passive input handoff precedes success-only publication");
        for (int mutation = 0; mutation != 2; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Constructor, constructor.id});
          for (auto &block : body->blocks) {
            for (auto &instruction : block.instructions) {
              if (mutation == 0 && instruction.id == construct.id)
                instruction.lifecycle.clear();
              if (mutation == 1 &&
                  instruction.kind == lang::MirInstructionKind::CallInput &&
                  instruction.preparedParameterDrop)
                instruction.callInputIndex = 1;
            }
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "passive prepared arguments require exact input position and "
                 "invocation handoff");
        }
      }
    }
  }
  expect(tested == 1,
         "fixture must exercise one moved passive class constructor argument");
}

void testNestedOwnedFieldAssignment() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/nested_owned_field_assignment.gti";
  std::ifstream input(fixture);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "nested owned field replacement must verify");
  if (!frontend.canGenerateCode())
    return;
  const auto originalRows = buildRows(frontend, frontend.mir);
  const lang::CppMirBodyEmissionMap rows(originalRows);
  const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
  expect(emitter.analyzeProgram().ready(),
         "nested owned fields must preserve explicit replacement cleanup");
  std::size_t assignments = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *destination =
            instruction.destination
                ? function.body.findPlace(*instruction.destination)
                : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Assign ||
            destination == nullptr ||
            destination->root != lang::MirPlaceRootKind::Loan ||
            destination->projections.size() != 2)
          continue;
        ++assignments;
        expect(instruction.replacementPair != 0 &&
                   instruction.lifecycle.size() == 1 &&
                   instruction.lifecycle.front().kind ==
                       lang::MirLifecycleEventKind::Replace,
               "nested replacement retains paired cleanup and exact source "
               "lifetime transfer");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Function, function.id});
          auto &changed = body->places[destination->id - 1];
          if (mutation == 0)
            changed.projections.front().field = 0;
          else if (mutation == 1)
            changed.projections.back().kind =
                lang::MirProjectionKind::Dereference;
          else {
            for (const auto &instance : malformed.classInstances()) {
              auto *owner = const_cast<lang::MirClassInstance *>(&instance);
              for (auto &field : owner->declaredFields) {
                if (field.symbol == destination->projections.front().field)
                  field.access = lang::AccessMode::ReadOnly;
              }
            }
          }
          const lang::CppMirBodyEmitter rejected(malformed, rows);
          expect(!rejected.analyzeProgram().ready(),
                 "nested replacement must reject forged field, projection "
                 "kind, or field mutability");
        }
        auto missingRows = originalRows;
        const auto field = destination->projections.front().field;
        std::erase_if(missingRows.symbols, [&](const auto &row) {
          return row.kind == lang::CppMirSymbolRepresentationKind::Field &&
                 row.symbol == field;
        });
        const lang::CppMirBodyEmissionMap incomplete(std::move(missingRows));
        const lang::CppMirBodyEmitter rejected(frontend.mir, incomplete);
        expect(!rejected.analyzeProgram().ready(),
               "every intermediate nominal field requires its exact native "
               "representation row");
        auto wrongEnd = frontend.mir;
        auto *changedBody = lang::findMirBody(
            wrongEnd, {lang::MirBodyKind::Function, function.id});
        const auto *child = changedBody->findLoan(destination->loan);
        expect(
            child != nullptr && child->parent != 0,
            "indexed owner replacement must carry its parent call-result loan");
        if (child != nullptr && child->parent != 0) {
          bool changed = false;
          for (auto &changedBlock : changedBody->blocks) {
            for (auto &ending : changedBlock.instructions) {
              if (ending.kind == lang::MirInstructionKind::EndBorrow &&
                  ending.loan == child->id) {
                ending.loan = child->parent;
                changed = true;
              }
            }
          }
          expect(changed && !lang::CppMirBodyEmitter(wrongEnd, rows)
                                 .analyzeProgram()
                                 .ready(),
                 "nested replacement cannot retire the parent before its child "
                 "loan");
        }
      }
    }
  }
  expect(assignments == 1,
         "fixture must exercise one nested loan-rooted owned replacement");

  const auto implicitFixture =
      std::filesystem::path(__FILE__).parent_path() /
      "fixtures/implicit_receiver_owned_assignment.gti";
  std::ifstream implicitInput(implicitFixture);
  const std::string implicitSource{
      std::istreambuf_iterator<char>(implicitInput), {}};
  const auto implicit =
      analyzeWithStandardLibrary(implicitFixture.string(), implicitSource);
  expect(implicit.canGenerateCode(),
         "implicit receiver replacement must verify");
  if (!implicit.canGenerateCode())
    return;
  const auto implicitRows = buildRows(implicit, implicit.mir);
  const lang::CppMirBodyEmissionMap implicitMap(implicitRows);
  expect(
      lang::CppMirBodyEmitter(implicit.mir, implicitMap)
          .analyzeProgram()
          .ready(),
      "implicit and explicit receiver paths must admit the same replacement");
  std::size_t implicitAssignments = 0;
  const auto checkImplicit = [&](lang::MirBodyAddress address,
                                 const lang::MirBody &body) {
    for (const auto &block : body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *destination = instruction.destination
                                      ? body.findPlace(*instruction.destination)
                                      : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Assign ||
            !destination ||
            destination->root != lang::MirPlaceRootKind::Symbol ||
            destination->projections.size() != 2)
          continue;
        ++implicitAssignments;
        for (int mutation = 0; mutation != 5; ++mutation) {
          auto malformed = implicit.mir;
          auto *changedBody = lang::findMirBody(malformed, address);
          auto &changed = changedBody->places[destination->id - 1];
          if (mutation == 0)
            changed.symbol = 0;
          else if (mutation == 1)
            changed.projections.front().field = 0;
          else if (mutation == 2)
            changed.projections.back().kind =
                lang::MirProjectionKind::Dereference;
          else if (mutation == 3)
            changed.capture = 1;
          else {
            for (const auto &instance : malformed.classInstances()) {
              auto *owner = const_cast<lang::MirClassInstance *>(&instance);
              for (auto &field : owner->declaredFields)
                if (field.symbol == destination->symbol)
                  field.access = lang::AccessMode::ReadOnly;
            }
          }
          expect(!lang::CppMirBodyEmitter(malformed, implicitMap)
                      .analyzeProgram()
                      .ready(),
                 "implicit receiver replacement rejects invalid root, suffix, "
                 "capture, and immutable containing field");
        }
        auto missingRows = implicitRows;
        std::erase_if(missingRows.symbols, [&](const auto &row) {
          return row.kind == lang::CppMirSymbolRepresentationKind::Field &&
                 row.symbol == destination->symbol;
        });
        const lang::CppMirBodyEmissionMap missing(std::move(missingRows));
        expect(!lang::CppMirBodyEmitter(implicit.mir, missing)
                    .analyzeProgram()
                    .ready(),
               "implicit receiver root requires its exact representation row");
      }
    }
  };
  for (const auto &constructor : implicit.mir.constructorInstances())
    checkImplicit({lang::MirBodyKind::Constructor, constructor.id},
                  constructor.body);
  for (const auto &function : implicit.mir.functionInstances())
    checkImplicit({lang::MirBodyKind::Function, function.id}, function.body);
  expect(implicitAssignments == 2,
         "constructor must replace two nested Symbol-rooted fields; methods "
         "already use canonical This-rooted paths");

  const auto freshFixture = std::filesystem::path(__FILE__).parent_path() /
                            "fixtures/constexpr_dependent_branches.gti";
  std::ifstream freshInput(freshFixture);
  const std::string freshSource{std::istreambuf_iterator<char>(freshInput), {}};
  const auto fresh =
      analyzeWithStandardLibrary(freshFixture.string(), freshSource);
  expect(fresh.canGenerateCode(),
         "dependent receiver restoration must retain a concrete MIR schedule");
  if (!fresh.canGenerateCode())
    return;
  const auto freshRows = buildRows(fresh, fresh.mir);
  const lang::CppMirBodyEmissionMap freshMap(freshRows);
  expect(lang::CppMirBodyEmitter(fresh.mir, freshMap).analyzeProgram().ready(),
         "a fresh class value can restore an exact mutable owned field");
  std::size_t restoredFields = 0;
  std::size_t passiveFields = 0;
  for (const auto &function : fresh.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *destination =
            instruction.destination
                ? function.body.findPlace(*instruction.destination)
                : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Assign ||
            destination == nullptr ||
            destination->root != lang::MirPlaceRootKind::This ||
            destination->projections.size() != 1 ||
            destination->type.kind != lang::SemanticType::Class)
          continue;
        const auto *owner = function.owner
                                ? fresh.mir.findClassInstance(*function.owner)
                                : nullptr;
        const lang::MirClassFieldInfo *selectedField = nullptr;
        if (owner) {
          for (const auto &field : owner->declaredFields) {
            if (field.symbol == destination->projections.front().field &&
                field.type == destination->type)
              selectedField = &field;
          }
        }
        expect(selectedField != nullptr,
               "receiver assignment must select an exact declared field "
               "before classifying its cleanup obligation");
        if (!selectedField)
          continue;
        const bool owning = selectedField->requiresActiveCleanup;
        if (owning) {
          restoredFields += owner != nullptr &&
                            owner->sourceUnit == fresh.sourceGraph.entryUnit();
          expect(instruction.replacementPair != 0 &&
                     instruction.lifecycle.size() == 1 &&
                     instruction.lifecycle.front().kind ==
                         lang::MirLifecycleEventKind::Replace &&
                     instruction.lifecycle.front().target == 0,
                 "receiver restoration keeps paired cleanup and the containing "
                 "object's sole field cleanup authority");
        } else {
          ++passiveFields;
          expect(instruction.replacementPair == 0 &&
                     instruction.lifecycle.empty(),
                 "passive descriptor assignment must not invent owning "
                 "replacement cleanup or lifetime publication");
        }
        // Access and representation authority apply to every class field.
        // Only an owning field has cleanup/publication records to remove;
        // selecting by those records would hide a missing-cleanup regression.
        for (int mutation = 0; mutation != (owning ? 4 : 2); ++mutation) {
          auto malformed = fresh.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          if (mutation == 0) {
            changed->receiverMutability = lang::ReceiverMutability::ReadOnly;
          } else if (mutation == 1) {
            auto *owner = const_cast<lang::MirClassInstance *>(
                malformed.findClassInstance(*function.owner));
            for (auto &field : owner->declaredFields)
              if (field.symbol == destination->projections.front().field)
                field.access = lang::AccessMode::ReadOnly;
          } else {
            for (auto &changedBlock : changed->body.blocks)
              for (auto &assignment : changedBlock.instructions)
                if (assignment.id == instruction.id) {
                  if (mutation == 2)
                    assignment.replacementPair = 0;
                  else
                    assignment.lifecycle.clear();
                }
          }
          expect(!lang::CppMirBodyEmitter(malformed, freshMap)
                      .analyzeProgram()
                      .ready(),
                 "fresh field replacement rejects immutable access, missing "
                 "cleanup pairing, or missing lifetime publication");
        }
        auto incomplete = freshRows;
        std::erase_if(incomplete.symbols, [&](const auto &row) {
          return row.kind == lang::CppMirSymbolRepresentationKind::Field &&
                 row.symbol == destination->projections.front().field;
        });
        const lang::CppMirBodyEmissionMap missing(std::move(incomplete));
        expect(!lang::CppMirBodyEmitter(fresh.mir, missing)
                    .analyzeProgram()
                    .ready(),
               "fresh field publication requires its exact representation row");
      }
    }
  }
  expect(restoredFields == 1,
         "dependent fixture exercises exactly one fresh owned field restore");
  expect(passiveFields != 0,
         "source allocator descriptors must exercise passive field assignment "
         "without weakening the owning restoration checks");
}

void testNestedBorrowedFieldProjection() {
  const auto mutableFixture = std::filesystem::path(__FILE__).parent_path() /
                              "fixtures/nested_borrow_fields_mutable.gti";
  std::ifstream mutableInput(mutableFixture);
  const std::string mutableSource{std::istreambuf_iterator<char>(mutableInput),
                                  {}};
  const auto mutableFrontend =
      analyzeWithStandardLibrary(mutableFixture.string(), mutableSource);
  expect(mutableFrontend.canGenerateCode(),
         "mutable nested field fixture must verify");
  if (mutableFrontend.canGenerateCode()) {
    const lang::CppMirBodyEmissionMap mutableRows(
        buildRows(mutableFrontend, mutableFrontend.mir));
    expect(lang::CppMirBodyEmitter(mutableFrontend.mir, mutableRows)
               .analyzeProgram()
               .ready(),
           "nested construction must consume staged reference inputs and "
           "discarded shells must preserve explicit dependency retirement");
    bool mutated = false;
    auto malformed = mutableFrontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        malformed.functionInstances());
    for (auto &function : functions) {
      for (auto &block : function.body.blocks) {
        for (auto &instruction : block.instructions) {
          if (instruction.kind == lang::MirInstructionKind::Call &&
              instruction.info.traits.containsBorrowedState &&
              instruction.result && instruction.loan &&
              function.body.usesOf(*instruction.result).empty()) {
            instruction.loan.reset();
            mutated = true;
          }
        }
      }
    }
    expect(mutated && !lang::verifyMirProgram(malformed).valid(),
           "discarding a carrier shell cannot erase its owner dependency");
  }
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/nested_borrow_fields.gti";
  std::ifstream input(fixture);
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "nested field loan fixture must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
  expect(emitter.analyzeProgram().ready(),
         "nested one-owner fields must retain exact constructor and return "
         "publication");
  std::size_t paths = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    for (const auto &place : function.body.places) {
      if (place.root != lang::MirPlaceRootKind::Binding ||
          place.projections.size() != 3 ||
          place.projections[0].kind != lang::MirProjectionKind::Field ||
          place.projections[1].kind != lang::MirProjectionKind::Field ||
          place.projections[2].kind != lang::MirProjectionKind::Dereference)
        continue;
      ++paths;
      for (int mutation = 0; mutation != 3; ++mutation) {
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(
            malformed, {lang::MirBodyKind::Function, function.id});
        auto &changed = body->places[place.id - 1];
        if (mutation == 0)
          changed.projections[1].field = 0;
        if (mutation == 1)
          changed.projections.back().kind = lang::MirProjectionKind::Field;
        if (mutation == 2)
          changed.type = lang::SemanticType::Bool;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "nested reference projection cannot forge field, dereference, "
               "or referent type");
      }
    }
  }
  expect(paths != 0,
         "fixture must exercise a binding field-field-reference chain");
}

void testPassiveConstructorFieldPublication() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/passive_constructor_field.gti";
  std::ifstream input(fixture);
  expect(input.good(), "passive constructor-field fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "passive constructor fields must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "callback result with a generated passive field must emit");
  std::size_t publications = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    // Only the fixture's own record; library owners have their own fields.
    const auto owner = std::find_if(
        frontend.hir.classInstances().begin(),
        frontend.hir.classInstances().end(),
        [&](const auto &item) { return item.id == constructor.owner; });
    if (owner == frontend.hir.classInstances().end() || !owner->source ||
        owner->source->name().lexeme != "Packet")
      continue;
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            instruction.constructorInitializer == 0 ||
            instruction.info.type.kind != lang::SemanticType::Class ||
            !instruction.lifecycle.empty())
          continue;
        ++publications;
        expect(instruction.lifecycle.empty() && instruction.destination &&
                   instruction.operands.size() == 1,
               "passive fields publish without a fabricated drop obligation");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(
              malformed, {lang::MirBodyKind::Constructor, constructor.id});
          for (auto &changed : body->findBlock(block.id)->instructions) {
            if (changed.id != instruction.id)
              continue;
            if (mutation == 0)
              changed.constructorInitializer = 0;
            else if (mutation == 1)
              changed.constructorInitializer =
                  constructor.initializers.size() + 1;
            else
              body->places[*changed.destination - 1].projections.front().field =
                  0;
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "missing publication identity and forged initializer/field "
                 "must reject");
        }
      }
    }
  }
  expect(publications == 1, "fixture must exercise exactly one passive field");
}

void testScalarConstructorFieldPublication() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/constructor_scalar_side_effects.gti";
  std::ifstream input(fixture);
  expect(input.good(), "scalar initializer side-effect fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "side-effecting scalar field initializers must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
  expect(emitter.analyzeProgram().ready(),
         "scalar field initialization must preserve its exact MIR schedule");
  std::size_t discardedAssignments = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    if (constructor.initializers.size() != 7)
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                       constructor.id};
    const auto text =
        emitter.emitFailureBodyText(address, "scalar-field-publication", 1)
            .text;
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Assign &&
            instruction.result &&
            constructor.body.usesOf(*instruction.result).empty()) {
          ++discardedAssignments;
          expect(text.find("std::move(__gti_mir_v_" +
                           std::to_string(*instruction.result) + ")") ==
                     std::string::npos,
                 "discarded assignment metadata must not publish a field");
        }
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            instruction.constructorInitializer < 2 ||
            instruction.constructorInitializer > 6)
          continue;
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(malformed, address);
        for (auto &changed : body->findBlock(block.id)->instructions) {
          if (changed.id == instruction.id)
            changed.tombstone = true;
        }
        expect(!lang::verifyMirProgram(malformed).valid(),
               "source initializer metadata cannot replace an explicit "
               "scalar field publication");
      }
    }
  }
  expect(discardedAssignments >= 2,
         "prefix/postfix initializers must exercise discarded assignment "
         "records sharing the published expression's HIR identity");
}

void testPassiveUnexpectedBinding() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/expected_scalar_error_binding.gti";
  std::ifstream input(fixture);
  expect(input.good(), "passive error binding fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "passive error bindings must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
  expect(
      emitter.analyzeProgram().ready(),
      "passive error binding must not construct an inactive success payload");
  std::size_t publications = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None)
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().type.kind !=
                lang::SemanticType::Unexpected)
          continue;
        ++publications;
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(malformed, address);
          for (auto &changed : body->findBlock(block.id)->instructions) {
            if (changed.id != instruction.id)
              continue;
            if (mutation == 0)
              changed.operands.front().type.arguments.front() =
                  lang::SemanticType::Bool;
            else if (mutation == 1)
              changed.operands.push_back(changed.operands.front());
            else
              body->places[*changed.destination - 1].type.arguments.back() =
                  lang::SemanticType::Bool;
          }
          expect(
              !lang::CppMirBodyEmitter(malformed, rows)
                   .supportsFailureBodyText(address),
              "error binding rejects forged payload identity or extra inputs");
        }
      }
    }
  }
  expect(publications == 3,
         "scalar, owning-inactive and void success alternatives are covered");
}

void testExpectedConstructorPayloadPublication() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/expected_constructor_payload.gti";
  std::ifstream input(fixture);
  expect(input.good(), "the owning constructor payload fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "nested owning constructor payloads must preserve verified MIR");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap rows(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, rows);
  expect(emitter.analyzeProgram().ready(),
         "an owning constructor result must publish into Expected without "
         "requiring assignment or losing field rollback");
  std::size_t publications = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None)
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::Function,
                                       function.id};
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            instruction.info.type.kind != lang::SemanticType::Expected ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().kind != lang::MirOperandKind::Value)
          continue;
        const auto *value =
            function.body.findValue(instruction.operands.front().value);
        const auto *producer =
            value ? testInstruction(function.body, value->definition) : nullptr;
        if (!producer || producer->kind != lang::MirInstructionKind::Construct)
          continue;
        ++publications;
        expect(producer->successResultDrop &&
                   producer->definedFailure.propagation ==
                       lang::FailurePropagationKind::Constructor &&
                   instruction.lifecycle.size() == 2,
               "constructor success must own a payload before transferring "
               "it into the exact Expected binding");
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(malformed, address);
          for (auto &changedBlock : body->blocks) {
            for (auto &changed : changedBlock.instructions) {
              if (changed.id == producer->id && mutation == 0)
                changed.successResultDrop.reset();
              if (changed.id == instruction.id && mutation == 1)
                changed.lifecycle.clear();
              if (changed.id == producer->id && mutation == 2)
                changed.constructorTarget.reset();
            }
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "constructor identity, successful ownership and Expected "
                 "publication cannot be erased");
          expect(!lang::CppMirBodyEmitter(malformed, rows)
                      .supportsFailureBodyText(address),
                 "backend admission must independently reject an incomplete "
                 "constructor payload publication");
        }
      }
    }
  }
  expect(publications == 1,
         "the fixture must contain an exact constructor-to-Expected transfer");
}

void testCleanupFreeConstructorFieldFailure() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/constructor_cleanup_free_failure.gti";
  std::ifstream input(fixture);
  expect(input.good(), "the nested constructor failure fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(),
         "cleanup-free nested construction must retain verified failure MIR");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  for (const auto &body : emitter.analyzeProgram().bodies) {
    for (const auto &issue : body.issues) {
      std::cerr << "constructor fixture: body " << body.body.owner << " i"
                << issue.instruction << ": " << issue.detail << '\n';
    }
  }
  expect(emitter.analyzeProgram().ready(),
         "fallible cleanup-free fields and completed owning fields must "
         "compose through the same constructor status boundary");
  std::size_t checkedFields = 0;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *constructedOwner =
        frontend.mir.findClassInstance(constructor.owner);
    if (constructedOwner == nullptr ||
        constructedOwner->sourceUnit != frontend.sourceGraph.entryUnit())
      continue;
    for (const auto &block : constructor.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *target = instruction.constructorTarget
                                 ? frontend.mir.findConstructorInstance(
                                       *instruction.constructorTarget)
                                 : nullptr;
        const auto *owner =
            target ? frontend.mir.findClassInstance(target->owner) : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Construct ||
            !instruction.result || !owner || owner->requiresActiveCleanup ||
            instruction.definedFailure.propagation !=
                lang::FailurePropagationKind::Constructor)
          continue;
        ++checkedFields;
        expect(!instruction.successResultDrop &&
                   lang::requiresMirFailureControlFlow(
                       instruction, lang::MirFailureControlFlowPosition::None),
               "a cleanup-free constructor needs an edge at every position");
        expect(block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == instruction.id,
               "failure must precede cleanup-free field publication");
        if (block.terminator.kind != lang::MirTerminatorKind::Invoke)
          continue;
        const auto *success =
            constructor.body.findBlock(block.terminator.target);
        expect(success && !success->instructions.empty() &&
                   success->instructions.front().kind ==
                       lang::MirInstructionKind::Initialize &&
                   success->instructions.front().constructorInitializer != 0,
               "the exact field is published only on the success continuation");
        auto malformed = frontend.mir;
        auto *body = lang::findMirBody(
            malformed, {lang::MirBodyKind::Constructor, constructor.id});
        auto &terminator = body->findBlock(block.id)->terminator;
        terminator.kind = lang::MirTerminatorKind::Goto;
        terminator.invokeInstruction = 0;
        terminator.failureRecord = 0;
        terminator.elseTarget = 0;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "removing a cleanup-free constructor failure edge must reject");
      }
    }
  }
  expect(checkedFields == 2,
         "the fixture must cover both levels of cleanup-free construction");
  if (emitter.analyzeProgram().ready()) {
    const auto artifact = gti_test::emitCpp(frontend);
    expect(!artifact.contents.empty(),
           "the complete constructor schedule must reach the native backend");
  }
}

void testNestedCleanupFreeClassResult() {
  const auto fixture = std::filesystem::path(__FILE__).parent_path() /
                       "fixtures/nested_nominal_result.gti";
  std::ifstream input(fixture);
  expect(input.good(), "the nested nominal-result fixture must exist");
  const std::string source{std::istreambuf_iterator<char>(input), {}};
  const auto frontend = analyzeWithStandardLibrary(fixture.string(), source);
  expect(frontend.canGenerateCode(), "nested class results must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  expect(emitter.analyzeProgram().ready(),
         "nested member, erased callable, and constructor inputs must emit");
  std::size_t nestedResults = 0;
  std::size_t callableResults = 0;
  for (const auto &entry : frontend.mir.functionInstances()) {
    if (entry.entryKind != lang::ProgramEntryKind::OwnedArguments)
      continue;
    const lang::MirBodyAddress address{lang::MirBodyKind::Function, entry.id};
    for (const auto &block : entry.body.blocks) {
      for (const auto &call : block.instructions) {
        if (call.kind != lang::MirInstructionKind::Call ||
            !call.functionTarget || !call.result ||
            call.info.type.kind != lang::SemanticType::Class)
          continue;
        ++nestedResults;
        callableResults += call.callableInvocation.has_value();
        expect(!call.successResultDrop && !call.successResultDestination,
               "cleanup-free results must not need an owning result identity");
        expect(block.terminator.kind == lang::MirTerminatorKind::Invoke &&
                   block.terminator.invokeInstruction == call.id,
               "nested nominal calls must route failure before publication");
        const lang::MirInstruction *stage = nullptr;
        for (const auto &candidateBlock : entry.body.blocks) {
          for (const auto &candidate : candidateBlock.instructions) {
            if (candidate.kind == lang::MirInstructionKind::CallInput &&
                candidate.operands.size() == 1 &&
                candidate.operands.front().kind ==
                    lang::MirOperandKind::Value &&
                candidate.operands.front().value == *call.result) {
              stage = &candidate;
              expect(candidateBlock.id == block.terminator.target,
                     "only the success successor may prepare the result");
            }
          }
        }
        expect(stage != nullptr && stage->preparedParameterDrop &&
                   stage->destination,
               "each nested result must use an exact prepared argument slot");
        if (stage == nullptr || !stage->preparedParameterDrop ||
            !stage->destination)
          continue;
        if (emitter.supportsFailureBodyText(address)) {
          const auto text =
              emitter.emitFailureBodyText(address, "nested", 1).text;
          const auto slot =
              "__gti_mir_p_" + std::to_string(*stage->destination);
          bool constructorConsumer = false;
          for (const auto &candidateBlock : entry.body.blocks) {
            for (const auto &candidate : candidateBlock.instructions) {
              if (candidate.kind == lang::MirInstructionKind::Construct &&
                  candidate.callSite == stage->callSite)
                constructorConsumer = true;
            }
          }
          const bool handoff =
              constructorConsumer
                  ? text.find(slot + ".take()") != std::string::npos
                  : text.find("std::move(" + slot + ".get())") !=
                            std::string::npos &&
                        text.find(slot + ".destroy()") != std::string::npos;
          expect(text.find(slot + ".construction_address()") !=
                         std::string::npos &&
                     text.find(slot + ".mark_constructed()") !=
                         std::string::npos &&
                     handoff,
                 "result placement, successful activation, and handoff must "
                 "agree");
        }
        if (call.callableInvocation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(malformed, address);
          for (auto &candidate : body->findBlock(block.id)->instructions) {
            if (candidate.id == call.id)
              candidate.callableInvocation =
                  lang::CallableInvocationCapability::Once;
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "result staging must not relax the selected operator's "
                 "receiver capability");
        }
        for (int mutation = 0; mutation != 3; ++mutation) {
          auto malformed = frontend.mir;
          auto *body = lang::findMirBody(malformed, address);
          auto &term = body->findBlock(block.id)->terminator;
          if (mutation == 0) {
            term.kind = lang::MirTerminatorKind::Goto;
            term.invokeInstruction = 0;
            term.failureRecord = 0;
            term.elseTarget = 0;
          } else if (mutation == 1) {
            std::swap(term.target, term.elseTarget);
          } else {
            body->dropObligations[*stage->preparedParameterDrop - 1]
                .initiallyActive = true;
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "missing failure routing, reversed edges, and early "
                 "activation must fail");
        }
      }
    }
  }
  expect(nestedResults == 3 && callableResults == 1,
         "the fixture must cover three producers including one nominal "
         "operator()");
}

void checkCleanupFreeConstructorTemporaryFieldRead(bool immovable) {
  const std::string source = std::string(R"(
class plain_t {
public:
  plain_t() {}
)") +
                             (immovable ? R"(
  plain_t(plain_t& other) = delete;
  plain_t(plain_t&& other) = delete;
)"
                                        : "") +
                             R"(
  mut int32_t value = 7;
};

int32_t main() { return plain_t().value - 7; }
)";
  const lang::FrontendResult frontend =
      analyze("cpp-mir-constructor-temporary-field-read.gti", source);
  expect(frontend.canGenerateCode(),
         "a direct field read from a cleanup-free constructor temporary "
         "should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const auto entry = std::find_if(
      frontend.mir.functionInstances().begin(),
      frontend.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind == lang::ProgramEntryKind::NoArguments;
      });
  expect(entry != frontend.mir.functionInstances().end(),
         "the constructor-temporary fixture should lower its entry function");
  if (entry == frontend.mir.functionInstances().end()) {
    return;
  }

  const lang::MirInstruction *construct = nullptr;
  for (const lang::MirBlock &block : entry->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (instruction.kind == lang::MirInstructionKind::Construct &&
          instruction.constructorTarget && instruction.result &&
          instruction.info.type.kind == lang::SemanticType::Class) {
        expect(construct == nullptr,
               "the canonical fixture should have one class construction");
        construct = &instruction;
      }
    }
  }
  expect(construct != nullptr,
         "the direct constructor receiver should remain an ordinary MIR "
         "Construct result");
  if (construct == nullptr) {
    return;
  }

  const lang::MirValueId classResult = *construct->result;
  const lang::MirValue *constructed = entry->body.findValue(classResult);
  const lang::MirConstructorInstance *constructor =
      frontend.mir.findConstructorInstance(*construct->constructorTarget);
  const lang::MirClassInstance *owner =
      constructor == nullptr
          ? nullptr
          : frontend.mir.findClassInstance(constructor->owner);
  const std::vector<lang::MirValueUse> uses = entry->body.usesOf(classResult);
  const lang::MirPlace *projection =
      uses.size() == 1 && uses.front().kind == lang::MirValueUseKind::PlaceRoot
          ? entry->body.findPlace(uses.front().place)
          : nullptr;
  const lang::MirInstruction *load = nullptr;
  if (projection != nullptr) {
    for (const lang::MirBlock &block : entry->body.blocks) {
      for (const lang::MirInstruction &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Load &&
            instruction.operands.size() == 1 &&
            instruction.operands.front().kind == lang::MirOperandKind::Copy &&
            instruction.operands.front().place == projection->id) {
          expect(load == nullptr,
                 "the projected field should have one scalar Load consumer");
          load = &instruction;
        }
      }
    }
  }

  expect(
      constructed != nullptr && constructor != nullptr && owner != nullptr &&
          constructed->info.category == lang::ValueCategory::Value &&
          construct->constructorKind == lang::ConstructorKind::Ordinary &&
          !construct->destination && !construct->receiver &&
          construct->localFailureSites.empty() &&
          construct->definedFailure.localOrigins.empty() &&
          construct->lifecycle.empty() && construct->fullExpressionEnd == 0 &&
          construct->cleanupBoundaryEnd == 0 && !construct->successResultDrop &&
          !construct->successResultDestination &&
          constructor->owner == owner->id &&
          owner->type == constructed->info.type &&
          constructor->parameterTypes == construct->parameterTypes &&
          construct->parameterTypes.size() == construct->operands.size() &&
          owner->destructorStatus == lang::SpecialMemberStatus::Generated &&
          !owner->destructor && !owner->requiresActiveDropState &&
          !owner->requiresActiveCleanup,
      "the construction should carry exact target/type/parameter metadata "
      "without semantic cleanup obligations");
  expect(constructed && constructed->info.traits.movable != immovable,
         "the constructor cases must cover movable and immovable results");
  expect(uses.size() == 1 && projection != nullptr &&
             projection->root == lang::MirPlaceRootKind::Value &&
             projection->value == classResult &&
             projection->projections.size() == 1 &&
             projection->projections.front().kind ==
                 lang::MirProjectionKind::Field &&
             load != nullptr && load->result &&
             load->info.type == projection->type &&
             load->info.traits.drop == lang::DropKind::Trivial &&
             load->info.traits.copyable && load->lifecycle.empty() &&
             load->localFailureSites.empty() && load->definedFailure.empty(),
         "the construction result should have one value-rooted Field place "
         "and one exact trivial scalar copy Load");
  expect(std::none_of(entry->body.dropObligations.begin(),
                      entry->body.dropObligations.end(),
                      [&](const lang::MirDropObligation &drop) {
                        return drop.generatedValue == classResult ||
                               (projection != nullptr &&
                                drop.place == projection->id);
                      }),
         "the cleanup-free constructor value and field view should own no MIR "
         "drop obligations");

  const lang::CppMirBodyEmissionMap map(
      buildRows(frontend, frontend.mir, lang::CppStandard::Cpp23));
  const lang::CppMirBodyEmitter emitter(frontend.mir, map);
  const lang::MirBodyAddress address{.kind = lang::MirBodyKind::Function,
                                     .owner = entry->id};
  expect(emitter.analyze(address).ready() &&
             emitter.supportsFailureBodyText(address),
         "the exact constructor field read should enter transformed MIR "
         "emission");
  if (!emitter.supportsFailureBodyText(address)) {
    return;
  }

  const std::string text =
      emitter.emitFailureBodyText(address, "constructor-field-read-v0", 1).text;
  const std::string slot = "__gti_mir_v_" + std::to_string(classResult);
  const auto count = [&](std::string_view needle) {
    std::size_t occurrences = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
      ++occurrences;
    }
    return occurrences;
  };
  const bool exactPublication =
      constructor->mayRaiseDefinedFailure
          ? count("construct_representation_at(" + slot +
                  ".construction_address()") == 1 &&
                count(slot + ".mark_constructed()") == 1
          : count(slot + ".construct()") == 1 &&
                count(slot + ".mark_constructed()") == 0;
  expect(text.find("mir_lifetime_slot<") != std::string::npos &&
             exactPublication && count(slot + ".get().") == 1 &&
             count(slot + ".destroy()") == 1 &&
             text.find(slot + ".destroy()") > text.find(slot + ".get().") &&
             count("std::move(" + slot + ".get())") == 0,
         "the constructor should publish once into its value slot and the "
         "field Load should read it once and retire its cleanup-free "
         "representation slot without inventing a semantic copy or drop");

  const lang::OptimizationResult optimizations =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      gti_test::emitCpp(frontend, frontend.mir, frontend.mir, optimizations);
  expect(!artifact.contents.empty(),
         "the complete backend should emit the direct constructor field read");

  lang::MirProgram duplicateProjectionUse = frontend.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      duplicateProjectionUse.functionInstances());
  lang::MirFunctionInstance *mutatedEntry = nullptr;
  for (lang::MirFunctionInstance &function : functions) {
    if (function.id == entry->id) {
      mutatedEntry = &function;
    }
  }
  bool replacedReturn = false;
  if (mutatedEntry != nullptr && projection != nullptr) {
    for (lang::MirBlock &block : mutatedEntry->body.blocks) {
      if (block.terminator.kind != lang::MirTerminatorKind::Return ||
          !block.terminator.value) {
        continue;
      }
      block.terminator.value =
          lang::MirOperand{.kind = lang::MirOperandKind::Copy,
                           .place = projection->id,
                           .type = projection->type};
      replacedReturn = true;
    }
    (void)lang::rebuildMirValueUses(mutatedEntry->body);
  }
  expect(replacedReturn &&
             lang::verifyMirProgram(duplicateProjectionUse).valid(),
         "the duplicate field consumer near miss should remain valid MIR");
  if (replacedReturn &&
      lang::verifyMirProgram(duplicateProjectionUse).valid()) {
    const lang::CppMirBodyEmissionMap duplicateMap(
        completeRows(duplicateProjectionUse));
    const lang::CppMirBodyEmitter duplicateEmitter(duplicateProjectionUse,
                                                   duplicateMap);
    expect(!duplicateEmitter.supportsFailureBodyText(address),
           "a constructor result field place with a second consumer should "
           "remain outside the slot representation");
  }

  const lang::FrontendResult nested =
      analyze("cpp-mir-constructor-temporary-nested-field.gti", R"(
struct inner_t { int32_t value = 7; };
class outer_t {
public:
  outer_t() {}
  inner_t inner = inner_t();
};
int32_t main() { return outer_t().inner.value - 7; }
)");
  expect(nested.canGenerateCode(),
         "the nested constructor field near miss should pass the frontend");
  if (nested.canGenerateCode()) {
    const auto nestedEntry = std::find_if(
        nested.mir.functionInstances().begin(),
        nested.mir.functionInstances().end(), [](const auto &function) {
          return function.entryKind == lang::ProgramEntryKind::NoArguments;
        });
    expect(nestedEntry != nested.mir.functionInstances().end(),
           "the nested constructor field near miss should lower its entry");
    if (nestedEntry != nested.mir.functionInstances().end()) {
      const lang::CppMirBodyEmissionMap nestedMap(
          buildRows(nested, nested.mir, lang::CppStandard::Cpp23));
      const lang::CppMirBodyEmitter nestedEmitter(nested.mir, nestedMap);
      expect(
          !nestedEmitter.supportsFailureBodyText(
              {.kind = lang::MirBodyKind::Function, .owner = nestedEntry->id}),
          "a two-field constructor projection should remain unsupported");
    }
  }

  const lang::FrontendResult cleanup =
      analyze("cpp-mir-constructor-temporary-cleanup-field.gti", R"(
class cleanup_t {
public:
  cleanup_t() {}
  ~cleanup_t() {}
  int32_t value = 7;
};
int32_t main() { return cleanup_t().value - 7; }
)");
  expect(cleanup.canGenerateCode(),
         "the cleanup-owning constructor field near miss should pass the "
         "frontend");
  if (cleanup.canGenerateCode()) {
    const auto cleanupEntry = std::find_if(
        cleanup.mir.functionInstances().begin(),
        cleanup.mir.functionInstances().end(), [](const auto &function) {
          return function.entryKind == lang::ProgramEntryKind::NoArguments;
        });
    expect(cleanupEntry != cleanup.mir.functionInstances().end(),
           "the cleanup-owning constructor near miss should lower its entry");
    if (cleanupEntry != cleanup.mir.functionInstances().end()) {
      const lang::CppMirBodyEmissionMap cleanupMap(
          buildRows(cleanup, cleanup.mir, lang::CppStandard::Cpp23));
      const lang::CppMirBodyEmitter cleanupEmitter(cleanup.mir, cleanupMap);
      expect(
          !cleanupEmitter.supportsFailureBodyText(
              {.kind = lang::MirBodyKind::Function, .owner = cleanupEntry->id}),
          "an explicit-destructor constructor temporary should remain "
          "outside the cleanup-free slot representation");
    }
  }
}

void testCleanupFreeConstructorTemporaryFieldRead() {
  checkCleanupFreeConstructorTemporaryFieldRead(false);
  checkCleanupFreeConstructorTemporaryFieldRead(true);
}

// A generated constructor with two or more owned-parameter fields, one of
// them a text view, keeps that parameter's entry loan and ends it in the
// prologue. The record has no runtime effect, so the member-initializer
// proof admits it; a producer, an escape or any metadata on the record
// returns the constructor to the ordinary body route.
void testOwnedPrologueFormalEntryLoanEnd() {
  const auto frontend = analyzeWithStandardLibrary("text-view-owned.gti", R"(
struct Player { std::string_view name = ""; uint32_t health = 100; };
uint32_t read(Player& value) { return value.health; }
int main() {
  Player hero = {.health = uint32_t(7)};
  return read(hero) == 7 ? 0 : 1;
}
)");
  expect(frontend.canGenerateCode(), "designated text-view struct must verify");
  if (!frontend.canGenerateCode())
    return;
  const lang::CppMirBodyEmissionMap map(buildRows(frontend, frontend.mir));
  expect(lang::CppMirBodyEmitter(frontend.mir, map).analyzeProgram().ready(),
         "the whole program must stay inside the C++ text vocabulary");
  bool checked = false;
  for (const auto &constructor : frontend.mir.constructorInstances()) {
    const auto *owner = frontend.mir.findClassInstance(constructor.owner);
    const auto *info =
        owner == nullptr ? nullptr
                         : frontend.semantics.findClassType(owner->declaration);
    if (!info || !info->declaration ||
        info->declaration->name().lexeme != "Player" ||
        constructor.initializers.size() != 2)
      continue;
    checked = true;
    expect(std::all_of(constructor.initializers.begin(),
                       constructor.initializers.end(),
                       [](const lang::MirConstructorInitializer &initializer) {
                         return initializer.ownedParameter.has_value();
                       }),
           "both designated fields publish owned parameters");
    const lang::MirBodyAddress address{lang::MirBodyKind::Constructor,
                                       constructor.id};
    const auto admitted = [&](const lang::MirProgram &program) {
      const lang::CppMirBodyEmitter emitter(program, map);
      return constructor.mayRaiseDefinedFailure
                 ? emitter.supportsFailureBodyText(address)
                 : emitter.supportsBodyText(address);
    };
    expect(admitted(frontend.mir),
           "the text-view parameter's entry loan end is admitted beside the "
           "owned field moves");
    const lang::MirInstruction *end = nullptr;
    for (const auto &block : constructor.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::EndBorrow)
          end = &instruction;
    const auto *loan =
        end && end->loan ? constructor.body.findLoan(*end->loan) : nullptr;
    expect(loan != nullptr && loan->entry && !loan->escapes &&
               loan->producedBy == 0,
           "the prologue ends exactly the parameter's entry loan");
    if (loan == nullptr)
      continue;
    for (int mutation = 0; mutation != 5; ++mutation) {
      auto malformed = frontend.mir;
      auto *changed = const_cast<lang::MirConstructorInstance *>(
          malformed.findConstructorInstance(constructor.id));
      const auto changedLoan =
          std::find_if(changed->body.loans.begin(), changed->body.loans.end(),
                       [&](const lang::MirLoan &candidate) {
                         return candidate.id == loan->id;
                       });
      lang::MirInstruction *changedEnd = nullptr;
      for (auto &block : changed->body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.kind == lang::MirInstructionKind::EndBorrow)
            changedEnd = &instruction;
      expect(changedLoan != changed->body.loans.end() && changedEnd,
             "copied body keeps the records");
      if (changedLoan == changed->body.loans.end() || !changedEnd)
        continue;
      if (mutation == 0)
        changedLoan->entry = false;
      if (mutation == 1)
        changedLoan->escapes = true;
      if (mutation == 2)
        changedLoan->producedBy = 1;
      if (mutation == 3)
        changedEnd->cleanupBoundaryEnd = 1;
      if (mutation == 4)
        changedEnd->lifecycle.push_back({});
      expect(!admitted(malformed),
             "mutation " + std::to_string(mutation) +
                 " must leave the native member-initializer route");
    }
  }
  expect(checked, "the Player constructor must be inspected");
}

constexpr NamedTest kTests[] = {
    {"testCheckedReferenceExpectedAssignment",
     testCheckedReferenceExpectedAssignment},
    {"testCheckedReferenceClassAssignment",
     testCheckedReferenceClassAssignment},
    {"testContainedClosureConstructorPlan",
     testContainedClosureConstructorPlan},
    {"testOwningCallFieldInputTransfers", testOwningCallFieldInputTransfers},
    {"testOwnedExpectedMemberArgumentTransfer",
     testOwnedExpectedMemberArgumentTransfer},
    {"testExpectedRetainedTemplateLayout", testExpectedRetainedTemplateLayout},
    {"testExplicitVirtualResultChannel", testExplicitVirtualResultChannel},
    {"testGeneratedMoveReferenceBindings", testGeneratedMoveReferenceBindings},
    {"testPreparedConstructorReplacement", testPreparedConstructorReplacement},
    {"testConstructedPrvalueCheckedFieldReplacement",
     testConstructedPrvalueCheckedFieldReplacement},
    {"testNonFailingOwningDeclarationInitializer",
     testNonFailingOwningDeclarationInitializer},
    {"testGeneratedDeclarationFailureRollback",
     testGeneratedDeclarationFailureRollback},
    {"testGeneratedDefaultScheduleObligation",
     testGeneratedDefaultScheduleObligation},
    {"testCopiedBorrowedArgumentAcrossInvoke",
     testCopiedBorrowedArgumentAcrossInvoke},
    {"testPackParameterCleanup", testPackParameterCleanup},
    {"testForwardListEmplacementPublication",
     testForwardListEmplacementPublication},
    {"testContainerSwapStorageAndMemberResults",
     testContainerSwapStorageAndMemberResults},
    {"testOwningCheckedBodyEmitsWholeCleanupProof",
     testOwningCheckedBodyEmitsWholeCleanupProof},
    {"testRichFieldInitializerSchedule", testRichFieldInitializerSchedule},
    {"testToyEngineConstructorInitializers",
     testToyEngineConstructorInitializers},
    {"testFallibleFieldInitializerPropagation",
     testFallibleFieldInitializerPropagation},
    {"testDependentFieldInitializerSchedule",
     testDependentFieldInitializerSchedule},
    {"testContainedOwningFieldConstructor",
     testContainedOwningFieldConstructor},
    {"testGeneratedOwningStringFieldConstructor",
     testGeneratedOwningStringFieldConstructor},
    {"testBodyOrderedOwnedParameterConstructor",
     testBodyOrderedOwnedParameterConstructor},
    {"testSourceDestructorOwningFieldCleanup",
     testSourceDestructorOwningFieldCleanup},
    {"testOwningBaseConstructorStatus", testOwningBaseConstructorStatus},
    {"testPassiveOwnedParameterConstructorFields",
     testPassiveOwnedParameterConstructorFields},
    {"testBodyOrderedPassivePolicyConstructor",
     testBodyOrderedPassivePolicyConstructor},
    {"testCrossBlockDefiniteMoveRetiresShell",
     testCrossBlockDefiniteMoveRetiresShell},
    {"testMoveOnlyClassResultReceiverFieldTransfer",
     testMoveOnlyClassResultReceiverFieldTransfer},
    {"testMoveOnlyReceiverFieldLifecycleSchedules",
     testMoveOnlyReceiverFieldLifecycleSchedules},
    {"testNestedReceiverFieldProjection", testNestedReceiverFieldProjection},
    {"testContainedAbstractBaseConstructor",
     testContainedAbstractBaseConstructor},
    {"testCleanupFixtureFunctionBodiesAreReady",
     testCleanupFixtureFunctionBodiesAreReady},
    {"testOwnedLifecycleConstructionBodiesReady",
     testOwnedLifecycleConstructionBodiesReady},
    {"testDefaultConstructionPreparedParameterStaging",
     testDefaultConstructionPreparedParameterStaging},
    {"testCheckedPassiveExpectedConstruction",
     testCheckedPassiveExpectedConstruction},
    {"testReferenceReturnFailureAbi", testReferenceReturnFailureAbi},
    {"testGenericOwnerReferenceReturnFailureAbi",
     testGenericOwnerReferenceReturnFailureAbi},
    {"testStoredReferenceConstructorSchedule",
     testStoredReferenceConstructorSchedule},
    {"testGenericOwnerConstructorSpecialization",
     testGenericOwnerConstructorSpecialization},
    {"testConcreteGenericConstructorAndMemberPackPublication",
     testConcreteGenericConstructorAndMemberPackPublication},
    {"testPassiveConstructorFailureSummary",
     testPassiveConstructorFailureSummary},
    {"testPassiveRecordComposition", testPassiveRecordComposition},
    {"testPassiveFixedArrayFailureClosure",
     testPassiveFixedArrayFailureClosure},
    {"testOwningFixedArrayAggregateAndCheckedMove",
     testOwningFixedArrayAggregateAndCheckedMove},
    {"testPolymorphicClassArrayFieldLifecycleSchedule",
     testPolymorphicClassArrayFieldLifecycleSchedule},
    {"testPassiveClassArrayMoveAssignmentSchedule",
     testPassiveClassArrayMoveAssignmentSchedule},
    {"testCheckedClassArrayElementAssignmentFamily",
     testCheckedClassArrayElementAssignmentFamily},
    {"testGeneratedSpecialMemberConstructionSchedule",
     testGeneratedSpecialMemberConstructionSchedule},
    {"testConditionalClassReturnAndPreparedResultStage",
     testConditionalClassReturnAndPreparedResultStage},
    {"testConditionalFailureDestructorDrop",
     testConditionalFailureDestructorDrop},
    {"testVirtualFailureInterfaceFamily", testVirtualFailureInterfaceFamily},
    {"testConcreteOnlyGenericVirtualContractRoots",
     testConcreteOnlyGenericVirtualContractRoots},
    {"testAbstractBaseFailureFamilyAndInheritedMembers",
     testAbstractBaseFailureFamilyAndInheritedMembers},
    {"testUniqueOwnerUpcastFailureBody", testUniqueOwnerUpcastFailureBody},
    {"testCleanupFreeExpectedMemberProducer",
     testCleanupFreeExpectedMemberProducer},
    {"testExpectedLifetimeSlotObserverFusion",
     testExpectedLifetimeSlotObserverFusion},
    {"testExpectedOwningPayloadPublicationAndForwarding",
     testExpectedOwningPayloadPublicationAndForwarding},
    {"testCleanupOwningTemporaryArgumentTransfer",
     testCleanupOwningTemporaryArgumentTransfer},
    {"testConstructedArgumentTemporaryBorrow",
     testConstructedArgumentTemporaryBorrow},
    {"testFailureCapableClassReturnTemporaryReceiver",
     testFailureCapableClassReturnTemporaryReceiver},
    {"testFailureCapableClassReturnFieldRead",
     testFailureCapableClassReturnFieldRead},
    {"testCleanupFreeConstructorTemporaryFieldRead",
     testCleanupFreeConstructorTemporaryFieldRead},
    {"testDiscardedCleanupFreeClassResult",
     testDiscardedCleanupFreeClassResult},
    {"testNestedCleanupFreeClassResult", testNestedCleanupFreeClassResult},
    {"testCleanupFreeConstructorFieldFailure",
     testCleanupFreeConstructorFieldFailure},
    {"testExpectedConstructorPayloadPublication",
     testExpectedConstructorPayloadPublication},
    {"testScalarConstructorFieldPublication",
     testScalarConstructorFieldPublication},
    {"testPassiveUnexpectedBinding", testPassiveUnexpectedBinding},
    {"testPassiveConstructorFieldPublication",
     testPassiveConstructorFieldPublication},
    {"testGenericScalarConstructedFieldPublication",
     testGenericScalarConstructedFieldPublication},
    {"testRawPointerConstructorFieldPublication",
     testRawPointerConstructorFieldPublication},
    {"testPassiveClassFieldRollback", testPassiveClassFieldRollback},
    {"testBoundPassiveClassFieldRollback", testBoundPassiveClassFieldRollback},
    {"testNativeStatusFieldRollback", testNativeStatusFieldRollback},
    {"testPassiveBorrowedConstructedFieldPrefix",
     testPassiveBorrowedConstructedFieldPrefix},
    {"testNestedConstructorArgumentPublication",
     testNestedConstructorArgumentPublication},
    {"testPhysicalCarrierReferencePublication",
     testPhysicalCarrierReferencePublication},
    {"testPassiveExpectedMoveReturnHasNoSsaShell",
     testPassiveExpectedMoveReturnHasNoSsaShell},
    {"testGeneratedDefaultOwningFieldPublication",
     testGeneratedDefaultOwningFieldPublication},
    {"testProvenanceOnlyReceiverFields", testProvenanceOnlyReceiverFields},
    {"testNestedConstructorRepresentationPrecedence",
     testNestedConstructorRepresentationPrecedence},
    {"testConservativeConstructorAssignmentFusion",
     testConservativeConstructorAssignmentFusion},
    {"testBorrowedFieldCompoundAssignment",
     testBorrowedFieldCompoundAssignment},
    {"testNestedBorrowedFieldProjection", testNestedBorrowedFieldProjection},
    {"testNestedOwnedFieldAssignment", testNestedOwnedFieldAssignment},
    {"testPassiveNestedConstructorArgument",
     testPassiveNestedConstructorArgument},
    {"testBorrowedTemporaryOperatorReceiver",
     testBorrowedTemporaryOperatorReceiver},
    {"testMovedClassArgumentBeforeCheckedArgument",
     testMovedClassArgumentBeforeCheckedArgument},
    {"testOwningConstructedFieldInputTransfers",
     testOwningConstructedFieldInputTransfers},
    {"testConstructedCallbackConditionalArgument",
     testConstructedCallbackConditionalArgument},
    {"testConditionalNumericConversionArgument",
     testConditionalNumericConversionArgument},
    {"testErasedOwnedResultReturn", testErasedOwnedResultReturn},
    {"testFreshOwnedCallableArgument", testFreshOwnedCallableArgument},
    {"testGeneratedDefaultOwningFieldInitializer",
     testGeneratedDefaultOwningFieldInitializer},
    {"testPassiveStatusDefaultField", testPassiveStatusDefaultField},
    {"testSelectedConstructorDefaultSchedule",
     testSelectedConstructorDefaultSchedule},
    {"testOwnedPrologueFormalEntryLoanEnd",
     testOwnedPrologueFormalEntryLoanEnd},
};

} // namespace

int main(int argc, char **argv) {
  return runNamedTests(argc, argv, kTests, "lifecycle");
}
