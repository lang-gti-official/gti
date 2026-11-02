#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_lowering.h"
#include "gti/mir_verifier.h"
#include "mir_source_debug.h"
#include "mir_verification_queries.h"
#include "mir_verification_support.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>

namespace lang {

namespace {

// The first-use outcomes a thread-local access raises through its
// EnsureThreadLocal guard rather than through the accessing instruction.
[[nodiscard]] bool threadLocalGuardOutcome(DefinedFailureOutcome outcome) {
  return outcome.code == DefinedFailureCode::HostedRuntimeContractFailure &&
         (outcome.detail ==
              DefinedFailureDetail::RecursiveThreadLocalInitialization ||
          outcome.detail ==
              DefinedFailureDetail::ThreadLocalInitializationDuringShutdown ||
          outcome.detail ==
              DefinedFailureDetail::ThreadLocalOutsideAttachedLifetime);
}

[[nodiscard]] bool isMirFixedWidthIntegerType(const SemanticType &type) {
  return type.kind == SemanticType::Int8 || type.kind == SemanticType::Int16 ||
         type.kind == SemanticType::Int32 || type.kind == SemanticType::Int64 ||
         type.kind == SemanticType::UInt8 ||
         type.kind == SemanticType::UInt16 ||
         type.kind == SemanticType::UInt32 || type.kind == SemanticType::UInt64;
}

[[nodiscard]] std::optional<Literal>
programConstantLiteral(const ConstantValue &constant) {
  if (const auto *integer = std::get_if<ConstantInteger>(&constant)) {
    return Literal{integer->magnitude};
  }
  if (const auto *enumerator = std::get_if<ConstantEnum>(&constant)) {
    return Literal{enumerator->value.magnitude};
  }
  if (const auto *floating = std::get_if<BinaryFloat>(&constant)) {
    return Literal{*floating};
  }
  if (const auto *character = std::get_if<CharacterLiteral>(&constant)) {
    return Literal{*character};
  }
  if (const auto *string = std::get_if<std::string>(&constant)) {
    return Literal{*string};
  }
  if (const auto *boolean = std::get_if<bool>(&constant)) {
    return Literal{*boolean};
  }
  if (std::holds_alternative<NullConstant>(constant)) {
    return Literal{nullptr};
  }
  return std::nullopt;
}
} // namespace
class MirBodyLowerer {
public:
  // Cleared only to lower a program with one cleanup chain per failure edge.
  bool shareFailureCleanupEnabled = true;
  // Cleared only to lower a program in which every normal exit destroys its
  // own live owners.
  bool shareExitCleanupEnabled = true;

  MirBodyLowerer(
      const HirProgram &program, const FailureMetadata &failureMetadata,
      const HirBody &source, MirBodyKind kind, SemanticType returnType,
      const MirDefinedFailureEffects &definedFailureEffects,
      bool implicitZeroReturn = false,
      const HirFunctionInstance *function = nullptr,
      const HirConstructorInstance *constructor = nullptr,
      const HirLambda *lambda = nullptr,
      const HirProgramInitializationPlan *programInitialization = nullptr,
      MirProgramInitializationPlan *loweredProgramInitialization = nullptr,
      const MirClassInstance *constructorOwner = nullptr,
      const HirDestructorInstance *destructor = nullptr)
      : program(program), failureMetadata(failureMetadata), source(source),
        definedFailureEffects(definedFailureEffects),
        implicitZeroReturn(implicitZeroReturn), function(function),
        constructor(constructor), destructor(destructor), lambda(lambda),
        constructorOwner(constructorOwner),
        programInitialization(programInitialization),
        loweredProgramInitialization(loweredProgramInitialization) {
    output.kind = kind;
    if (kind == MirBodyKind::Module && programInitialization != nullptr)
      for (const auto &step : programInitialization->steps)
        nonLexicalBindings.insert(step.binding);
    if (kind == MirBodyKind::FieldInitializers ||
        kind == MirBodyKind::StaticFieldInitializers) {
      for (const HirClassInstance &candidate : program.classInstances())
        if ((kind == MirBodyKind::FieldInitializers &&
             &candidate.fieldInitializers == &source) ||
            (kind == MirBodyKind::StaticFieldInitializers &&
             &candidate.staticFieldInitializers == &source)) {
          const auto &fields = kind == MirBodyKind::FieldInitializers
                                   ? candidate.fields
                                   : candidate.staticFields;
          for (const HirClassField &field : fields)
            nonLexicalBindings.insert(field.binding);
          if (kind == MirBodyKind::FieldInitializers)
            fieldInitializerOwner = &candidate;
          break;
        }
      if (kind == MirBodyKind::FieldInitializers &&
          fieldInitializerOwner == nullptr)
        valid = false;
    }
    output.hasCallerLocation = function != nullptr && function->tracksCaller;
    output.placeDomain = source.placeDomain;
    captureSourceDebug();
    output.returnType = std::move(returnType);
    for (const HirValue &value : source.values) {
      values.emplace(value.id, &value);
      if (value.programConstantSubstitution) {
        if (!value.constant) {
          valid = false;
        } else {
          output.programConstantSubstitutions.push_back(
              {.hirValue = value.id, .constant = *value.constant});
        }
      }
    }
    // An exact borrowed-state call result retains its owner's stored loan,
    // but its carrier is still transferred exactly once into the selected
    // ordinary call or constructor parameter. Record only the producers
    // named by that explicit HIR call-input plan so their failure-capable
    // result is armed on the success edge before the later CallInput moves it.
    for (const HirValue &consumer : source.values) {
      if (!consumer.callPlan ||
          !(consumer.functionTarget ||
            consumer.kind == HirValueKind::PayloadConstruction ||
            (consumer.constructorTarget &&
             consumer.constructorKind == ConstructorKind::Ordinary))) {
        continue;
      }
      for (const HirCallArgument &argument : consumer.callPlan->arguments) {
        if (argument.kind == HirCallInputKind::MoveBorrowedTemporary) {
          borrowedTemporaryTransfers.insert(argument.value);
        }
      }
    }
    for (const HirStatement &statement : source.statements) {
      statements.emplace(statement.id, &statement);
      for (const SemanticLoanId loan : statement.endedLoans) {
        const auto [entry, inserted] =
            uniqueSemanticLoanEndpoints.emplace(loan, statement.id);
        if (!inserted && entry->second != statement.id)
          entry->second = 0;
      }
      if (statement.kind == HirStatementKind::Return &&
          statement.resultRegion == 0) {
        ++exitCount;
      }
    }
    // Every `?` propagation is an exit too, and a body whose last statement
    // is not a return may also complete by reaching its end.
    for (const HirValue &value : source.values) {
      if (value.propagation) {
        ++exitCount;
      }
    }
    const HirStatement *last =
        source.roots.empty() ? nullptr : findStatement(source.roots.back());
    while (last != nullptr && last->kind == HirStatementKind::Block &&
           !last->statements.empty()) {
      last = findStatement(last->statements.back());
    }
    if (last == nullptr || last->kind != HirStatementKind::Return) {
      ++exitCount;
    }
    for (const HirBinding &binding : source.bindings) {
      bindings.emplace(binding.id, &binding);
      if (binding.info.symbol != 0) {
        localSymbols.insert_or_assign(binding.info.symbol, binding.id);
      }
    }
    for (const HirStatement &statement : source.statements) {
      if (statement.value && statement.binding) {
        const auto *binding = findBinding(*statement.binding);
        if (binding != nullptr && (binding->info.retainedLoan != 0 ||
                                   binding->info.traits.containsBorrowedState))
          retainedInitializerLoans.emplace(*statement.value,
                                           binding->info.retainedLoan);
      }
    }
    if (lambda != nullptr) {
      for (std::size_t index = 0; index < lambda->captures.size(); ++index) {
        if (lambda->captures[index].bindingSymbol != 0) {
          lambdaCaptures.insert_or_assign(lambda->captures[index].bindingSymbol,
                                          index + 1);
        }
      }
    }
  }

  [[nodiscard]] MirBody
  lower(const std::vector<HirValueId> &prologueValues = {},
        const std::vector<HirConstructorInitializer> *initializers = nullptr) {
    scopes.push_back({});
    if (programInitialization != nullptr) {
      lowerProgramInitializationPlan();
    } else {
      output.entry = appendBlock();
      current = output.entry;
      if (bodylessDefinition()) {
        seedBodylessParameterPlaces();
      } else {
        seedParameterDrops();
        seedDefaultFieldRollbacks(initializers);
        seedEntryLoans();
        seedReturnBorrow();
      }

      (void)prologueValues;
      if (output.kind == MirBodyKind::Constructor && initializers != nullptr) {
        // Decide body-wide, before any edge is routed, whether some
        // initializer will silently transfer a subobject into `this`
        // without arming rollback; the verifier holds the matching
        // body-wide rule. Stage-eligible single owning arguments either arm
        // rollback or stay inactive, so they never transfer unarmed. An owned
        // base argument hands its prepared parameter to the staged base
        // constructor, which is not a transfer into `this` either.
        for (const HirConstructorInitializer &initializer : *initializers) {
          const HirConstructorInstance *target =
              initializer.constructorTarget
                  ? program.findConstructorInstance(
                        *initializer.constructorTarget)
                  : nullptr;
          const bool stageEligible =
              constructorRollbackStageEligible(initializer);
          for (std::size_t index = 0; index < initializer.arguments.size();
               ++index) {
            if (initializer.storesReference || (stageEligible && index == 0) ||
                (target != nullptr && index < target->parameterTypes.size() &&
                 target->parameterTypes[index].kind ==
                     SemanticType::Reference)) {
              continue;
            }
            if (ownedBaseArgument(initializer, index) &&
                stagedBaseShape(initializer) != nullptr)
              continue;
            const HirValue *argument = findValue(initializer.arguments[index]);
            if (argument != nullptr && argument->dropObligation) {
              constructorUnarmedTransfer = true;
            }
          }
        }
      }
      if (initializers != nullptr) {
        for (std::size_t initializerIndex = 0;
             initializerIndex < initializers->size(); ++initializerIndex) {
          const HirConstructorInitializer &initializer =
              (*initializers)[initializerIndex];
          std::vector<Scope> incomingScopes = scopes;
          const std::vector<TemporaryDrop> incomingTemporaryDrops =
              temporaryDrops;
          const bool stagedBaseInputs = stagedBase(initializer) != nullptr;
          std::vector<std::optional<MirOperand>> preparedBaseInputs(
              initializer.arguments.size());
          std::vector<MirDropObligationId> preparedBaseDrops;
          for (std::size_t argumentIndex = 0;
               argumentIndex < initializer.arguments.size(); ++argumentIndex) {
            const HirValueId argument = initializer.arguments[argumentIndex];
            if (stagedBaseInputs &&
                ownedBaseArgument(initializer, argumentIndex)) {
              // The owned value moves into its prepared parameter, which the
              // InitializeBase hands to the base constructor.
              const auto &parameter =
                  program
                      .findConstructorInstance(*initializer.constructorTarget)
                      ->parameterTypes[argumentIndex];
              MirOperand operand = valueOperand(argument);
              if (terminated())
                break;
              auto prepared = prepareCallInput(
                  0, argument, parameter, HirCallInputKind::MoveValue,
                  MirCallInputRole::Argument, argumentIndex, std::move(operand),
                  true, false, 0, initializerIndex + 1);
              preparedBaseInputs[argumentIndex] = std::move(prepared.operand);
              if (prepared.parameterDrop != 0)
                preparedBaseDrops.push_back(prepared.parameterDrop);
              continue;
            }
            if (stagedBaseInputs &&
                forwardedBaseReference(initializer, argumentIndex)) {
              const auto &parameter =
                  program
                      .findConstructorInstance(*initializer.constructorTarget)
                      ->parameterTypes[argumentIndex];
              auto prepared = prepareCallInput(
                  0, argument, parameter,
                  parameter.referenceAccess == AccessMode::Mutable
                      ? HirCallInputKind::MutableBorrow
                      : HirCallInputKind::ReadBorrow,
                  MirCallInputRole::Argument, argumentIndex,
                  argumentOperand(argument, parameter), false, false, 0,
                  initializerIndex + 1);
              preparedBaseInputs[argumentIndex] = std::move(prepared.operand);
              continue;
            }
            // The initializer reads each argument after all of them run, so
            // a class assignment's result is copied at its own commit.
            const HirValue *input = findValue(argument);
            if (input != nullptr && hasAssignmentResultCopy(*input))
              (void)assignmentResultValue(*input);
            else
              (void)emitValue(argument);
          }
          stageBaseInitializer(initializer, initializerIndex,
                               preparedBaseInputs, preparedBaseDrops);
          const auto constructorOwnerIterator =
              constructor == nullptr
                  ? program.classInstances().end()
                  : std::find_if(program.classInstances().begin(),
                                 program.classInstances().end(),
                                 [&](const HirClassInstance &candidate) {
                                   return candidate.id == constructor->owner;
                                 });
          const HirClassInstance *constructorOwner =
              constructorOwnerIterator == program.classInstances().end()
                  ? nullptr
                  : &*constructorOwnerIterator;
          const auto field =
              constructorOwner == nullptr
                  ? std::vector<HirClassField>::const_iterator{}
                  : std::find_if(constructorOwner->fields.begin(),
                                 constructorOwner->fields.end(),
                                 [&](const HirClassField &candidate) {
                                   return candidate.info.symbol ==
                                          initializer.field;
                                 });
          const bool explicitCleanupFreeFieldStage =
              constructorOwner != nullptr &&
              field != constructorOwner->fields.end() &&
              initializer.kind == ConstructorInitializerTargetKind::Field &&
              !initializer.storesReference && !initializer.constructorTarget &&
              !initializer.generatedDefault &&
              initializer.arguments.size() == 1 &&
              field->info.type == initializer.targetType &&
              (isMirFixedWidthIntegerType(initializer.targetType) ||
               initializer.targetType == SemanticType::Float ||
               initializer.targetType == SemanticType::Double ||
               initializer.targetType == SemanticType::Bool ||
               initializer.targetType == SemanticType::Char ||
               initializer.targetType.kind == SemanticType::RawPointer ||
               initializer.targetType.kind == SemanticType::Array ||
               initializer.targetType.kind == SemanticType::Class ||
               // Only a borrow-carrying closure takes the explicit stage; a
               // closure copied or moved without borrowed state publishes
               // through the constructor record, as the backend expects.
               (initializer.targetType.kind == SemanticType::Lambda &&
                field->info.traits.containsBorrowedState) ||
               initializer.targetType.kind == SemanticType::Enum) &&
              (field->info.traits.drop == DropKind::Trivial ||
               ((initializer.targetType.kind == SemanticType::Class ||
                 initializer.targetType.kind == SemanticType::Lambda ||
                 initializer.targetType.kind == SemanticType::Enum ||
                 initializer.targetType.kind == SemanticType::Array) &&
                !field->requiresActiveCleanup)) &&
              // A closure environment stored by value is a borrow-carrying
              // field exactly like a class carrier: the same Initialize,
              // Borrow and EndBorrow shape publishes its dependencies.
              (!field->info.traits.containsBorrowedState ||
               (constructor != nullptr &&
                (constructor->borrowOrigin == BorrowOriginKind::Argument ||
                 constructor->completeBorrow.has_value()) &&
                (initializer.targetType.kind == SemanticType::Class ||
                 initializer.targetType.kind == SemanticType::Enum ||
                 initializer.targetType.kind == SemanticType::Lambda)));
          const MirDropObligationId owningStageSource =
              constructorRollbackStageEligible(initializer)
                  ? dropObligationForValue(initializer.arguments.front())
                  : 0;
          const bool explicitOwningFieldStage =
              owningStageSource != 0 && temporaryIsActive(owningStageSource);
          // A lexical aggregate temporary can already carry a transfer token
          // even when its fields require no active cleanup. Its owning stage
          // is the sole publication; never initialize the same field twice.
          if (explicitCleanupFreeFieldStage && !explicitOwningFieldStage) {
            const MirPlaceId destination =
                appendPlace({.root = MirPlaceRootKind::This,
                             .projections = {{.kind = MirProjectionKind::Field,
                                              .field = initializer.field}},
                             .type = initializer.targetType,
                             .access = AccessMode::Mutable,
                             .traits = field->info.traits,
                             .sourceValue = initializer.arguments.front()});
            (void)appendInstruction(
                {.kind = MirInstructionKind::Initialize,
                 .hirValue = initializer.arguments.front(),
                 .constructorInitializer = initializerIndex + 1,
                 .destination = destination,
                 .operands = {valueOperand(initializer.arguments.front())},
                 .info = {.type = initializer.targetType,
                          .category = ValueCategory::Place,
                          .access = AccessMode::Mutable,
                          .traits = field->info.traits}});
          }
          // A class field completed from one owning constructed temporary
          // becomes an explicit stage: the Initialize reparents the
          // temporary's obligation into a ConstructionRollback obligation on
          // the exact This-rooted field place. Failure edges after this
          // stage drain armed rollback obligations in reverse order and
          // normal completion transfers them to the caller, so a
          // mid-construction defined failure can no longer leak a completed
          // subobject.
          if (explicitOwningFieldStage) {
            const MirPlaceId destination =
                appendPlace({.root = MirPlaceRootKind::This,
                             .projections = {{.kind = MirProjectionKind::Field,
                                              .field = initializer.field}},
                             .type = initializer.targetType,
                             .access = AccessMode::Mutable,
                             .traits = field->info.traits,
                             .sourceValue = initializer.arguments.front()});
            const MirDropObligation *sourceDrop =
                output.findDropObligation(owningStageSource);
            const MirDropObligationId rollback =
                output.dropObligations.size() + 1;
            output.dropObligations.push_back(
                {.id = rollback,
                 .constructionOrder = rollback,
                 .kind = MirDropObligationKind::ConstructionRollback,
                 .place = destination,
                 .dropType = sourceDrop == nullptr ? MirDropType{}
                                                   : sourceDrop->dropType});
            MirInstruction initialize{
                .kind = MirInstructionKind::Initialize,
                .hirValue = initializer.arguments.front(),
                .constructorInitializer = initializerIndex + 1,
                .destination = destination,
                .operands = {valueOperand(initializer.arguments.front())},
                .info = {.type = initializer.targetType,
                         .category = ValueCategory::Place,
                         .access = AccessMode::Mutable,
                         .traits = field->info.traits}};
            if (temporaryIsActive(owningStageSource)) {
              appendReparentOrTypedTransfer(initialize, owningStageSource,
                                            rollback);
              (void)removeTemporary(owningStageSource);
            } else {
              appendLifecycle(initialize,
                              {.kind = MirLifecycleEventKind::Initialize,
                               .target = rollback});
            }
            (void)appendInstruction(std::move(initialize));
            constructorRollback.push_back(rollback);
          }
          if (initializer.storesReference &&
              initializer.arguments.size() == 1) {
            markStoredBorrow(initializer.arguments.front(), initializer.field,
                             initializer.borrowAccess);
          } else if (constructorOwner != nullptr &&
                     field != constructorOwner->fields.end() &&
                     field->info.traits.containsBorrowedState &&
                     (initializer.targetType.kind == SemanticType::Class ||
                      initializer.targetType.kind == SemanticType::Enum ||
                      initializer.targetType.kind == SemanticType::Lambda) &&
                     initializer.arguments.size() == 1) {
            const auto *input = findValue(initializer.arguments.front());
            const bool knownEmpty =
                input && input->borrowDependencies &&
                input->borrowDependencies->dependencies.empty() &&
                !input->borrowDependencies->possibleAlternatives.empty();
            // An independently typed empty alternative has no owner to
            // publish. Every nonempty carrier still transfers its full group.
            if (!knownEmpty) {
              const MirLoanId transferred = markStoredCarrierBorrow(
                  initializer.arguments.front(), initializer.field,
                  initializer.borrowAccess);
              // Publication retired this exact entry dependency inside the
              // initializer. It is no longer part of the pre-expression live
              // prefix against which transient cleanup is computed.
              for (Scope &scope : incomingScopes) {
                std::erase(scope.loans, transferred);
              }
            }
          }
          const HirConstructorInstance *target =
              initializer.constructorTarget
                  ? program.findConstructorInstance(
                        *initializer.constructorTarget)
                  : nullptr;
          for (std::size_t index = 0; index < initializer.arguments.size();
               ++index) {
            const bool referenceParameter =
                target != nullptr && index < target->parameterTypes.size() &&
                target->parameterTypes[index].kind == SemanticType::Reference;
            if (explicitOwningFieldStage && index == 0) {
              continue;
            }
            if (!initializer.storesReference && !referenceParameter) {
              emitTemporaryTransfer(initializer.arguments[index]);
            }
          }
          endFullExpressionLoans(incomingScopes);
          emitTemporaryDrops(incomingTemporaryDrops, 0, 0,
                             initializerIndex + 1);
        }
      }
      lowerStatements(source.roots);
    }
    if (!terminated()) {
      retireConstructorRollback();
      // A void body's end is an exit like any `return;`: it may share its
      // cleanup with the body's other exits.
      const bool sharedEnd = (output.kind == MirBodyKind::Function ||
                              output.kind == MirBodyKind::Lambda) &&
                             output.returnType.kind == SemanticType::Void &&
                             !(function != nullptr && function->neverReturns) &&
                             sharedExitEligible(temporaryDrops) &&
                             emitSharedExit(SharedExitCarrier{}, 0);
      if (!sharedEnd) {
        emitScopeExit(0);
        if (output.kind == MirBodyKind::Module ||
            output.kind == MirBodyKind::FieldInitializers ||
            output.kind == MirBodyKind::StaticFieldInitializers) {
          terminate({.kind = MirTerminatorKind::Exit});
        } else if (function != nullptr && function->neverReturns) {
          terminate({.kind = MirTerminatorKind::Unreachable});
        } else if (implicitZeroReturn) {
          terminate({.kind = MirTerminatorKind::Return,
                     .value = MirOperand{.kind = MirOperandKind::Constant,
                                         .literal = Literal{std::uint64_t{0}},
                                         .type = SemanticType::Int32}});
        } else if (output.returnType.kind == SemanticType::Void) {
          terminate({.kind = MirTerminatorKind::Return});
        } else {
          terminate({.kind = MirTerminatorKind::Unreachable});
        }
      }
    }
    finishFailureCleanupAndVerify();
    return std::move(output);
  }

  [[nodiscard]] bool isValid() const { return valid; }
  // Whether any exit was lowered through the shared exit ladders, and
  // whether one of them joined another exit's cleanup.
  [[nodiscard]] bool usedExitSharing() const { return exitSharingUsed; }
  [[nodiscard]] bool joinedExits() const { return exitsJoined; }
  [[nodiscard]] const MirVerificationResult &proof() const {
    return verification;
  }
  [[nodiscard]] const std::optional<std::pair<HirValueId, std::string>> &
  rejection() const {
    return firstRejection;
  }

private:
  void reject(HirValueId value, std::string message) {
    valid = false;
    if (!firstRejection)
      firstRejection = std::pair{value, std::move(message)};
  }
  struct Scope {
    std::vector<MirPlaceId> drops;
    std::vector<MirLoanId> loans;
    std::size_t sourceLocals = 0;
  };
  struct SourceScopeLocal {
    SourceLocalId local = 0;
    std::size_t previous = 0;
  };
  // Scope snapshots retain immutable indices into one flat transient list.
  std::vector<SourceScopeLocal> sourceScopeLocals;

  void captureSourceDebug() {
    if (!isValidSourceDebugRecord(source.sourceDebug)) {
      valid = false;
      return;
    }
    output.sourceDebug = mir_detail::describeHirSourceDebug(source);
  }

  void registerSourceLocal(HirBindingId binding) {
    if (!output.sourceDebug.tables || scopes.empty())
      return;
    for (const auto &local : output.sourceDebug.tables->locals)
      if (local.declaration.binding == binding) {
        auto &head = scopes.back().sourceLocals;
        bool present = false;
        for (auto index = head; index;
             index = sourceScopeLocals[index - 1].previous)
          present |= sourceScopeLocals[index - 1].local == local.declaration.id;
        if (!present) {
          sourceScopeLocals.push_back({local.declaration.id, head});
          head = sourceScopeLocals.size();
        }
      }
  }

  void recordSourceLocalEnds(std::size_t keepScopes) {
    if (!output.sourceDebug.tables || terminated())
      return;
    const auto *block = currentBlock();
    if (!block)
      return;
    const MirDebugPoint point{
        .block = block->id,
        .instruction =
            block->instructions.empty() ? 0 : block->instructions.back().id,
        .after = !block->instructions.empty(),
        .blockEntry = block->instructions.empty()};
    auto &ends = output.sourceDebug.tables->localEnds;
    for (auto depth = scopes.size(); depth > keepScopes; --depth)
      for (auto index = scopes[depth - 1].sourceLocals; index;
           index = sourceScopeLocals[index - 1].previous) {
        const auto local = sourceScopeLocals[index - 1].local;
        if (std::find(ends.begin(), ends.end(),
                      MirSourceLocalEnd{local, point}) == ends.end())
          ends.push_back({local, point});
      }
  }

  void finishSourceDebug() {
    if (!output.sourceDebug.tables)
      return;
    for (auto &local : output.sourceDebug.tables->locals) {
      if (local.declaration.receiver) {
        const auto found = std::find_if(
            output.places.begin(), output.places.end(), [&](const auto &place) {
              return place.root == MirPlaceRootKind::This &&
                     place.projections.empty() &&
                     place.type == local.declaration.type && place.key &&
                     place.key->domain == output.placeDomain &&
                     place.key->receiver && !place.key->root &&
                     place.key->projections.empty();
            });
        if (found != output.places.end()) {
          local.place = found->id;
          local.policy = MirSourceLocalPolicy::ExistingPlace;
        }
        continue;
      }
      const auto found = bindingPlaces.find(local.declaration.binding);
      if (found == bindingPlaces.end())
        continue;
      const auto *place = output.findPlace(found->second);
      if (place && place->root == MirPlaceRootKind::Binding &&
          place->binding == local.declaration.binding &&
          place->symbol == local.declaration.symbol &&
          place->type == local.declaration.type && place->projections.empty() &&
          place->key && place->key->domain == output.placeDomain) {
        local.place = place->id;
        local.policy = MirSourceLocalPolicy::ExistingPlace;
      }
    }
    output.sourceDebug.tables->valueEvents =
        mir_detail::deriveMirSourceValueEvents(output);
  }

  struct TemporaryDrop {
    MirDropObligationId obligation = 0;
    bool conditional = false;

    friend bool operator==(const TemporaryDrop &,
                           const TemporaryDrop &) = default;
  };

  // Armed ConstructionRollback obligations in stage order; failure edges
  // drain them in reverse and normal completion retires them.
  std::vector<MirDropObligationId> constructorRollback;
  // Set when a constructor body silently transfers a temporary into `this`
  // without arming rollback (owned-parameter and other unstaged initializer
  // forms). From that point on no defined-failure edge may be routed in this
  // body: such an edge could not drain the transferred subobject, so the
  // checked operations after it stay on the compatibility authority instead
  // of leaking through verified MIR.
  bool constructorUnarmedTransfer = false;

  // A forwarded base reference names a passive scalar, or a class or enum
  // object that holds no borrowed state of its own.
  bool forwardableBaseReferent(const SemanticType &referent) const {
    return isPassiveScalarValueType(referent) ||
           ((referent.kind == SemanticType::Class ||
             referent.kind == SemanticType::Enum) &&
            !resolvedTraits(referent).containsBorrowedState);
  }

  // A reference-base family forwards one exact incoming reference. The
  // semantic formal supplies access and referent type; the existing place
  // engine supplies its evaluated address. No referent value is loaded merely
  // to pass this borrow, and the base cannot retain it.
  bool forwardedBaseReference(const HirConstructorInitializer &initializer,
                              std::size_t index) const {
    const auto *target =
        initializer.constructorTarget
            ? program.findConstructorInstance(*initializer.constructorTarget)
            : nullptr;
    if (!constructor ||
        initializer.kind != ConstructorInitializerTargetKind::Base || !target ||
        index >= target->parameterTypes.size() ||
        index >= initializer.arguments.size())
      return false;
    const auto &parameter = target->parameterTypes[index];
    const auto *value = findValue(initializer.arguments[index]);
    if (parameter.kind != SemanticType::Reference ||
        parameter.arguments.size() != 1 ||
        !forwardableBaseReferent(parameter.arguments.front()) || !value ||
        value->kind != HirValueKind::Variable ||
        value->info.category != ValueCategory::Place ||
        value->info.type != parameter.arguments.front())
      return false;
    const auto local = localSymbols.find(value->symbol);
    if (local == localSymbols.end())
      return false;
    const auto formal =
        std::find(constructor->parameterBindings.begin(),
                  constructor->parameterBindings.end(), local->second);
    if (formal == constructor->parameterBindings.end())
      return false;
    const auto ordinal = static_cast<std::size_t>(
        formal - constructor->parameterBindings.begin());
    if (ordinal >= constructor->parameterTypes.size())
      return false;
    const auto &source = constructor->parameterTypes[ordinal];
    return source.kind == SemanticType::Reference &&
           source.arguments == parameter.arguments &&
           (parameter.referenceAccess == AccessMode::ReadOnly ||
            source.referenceAccess == AccessMode::Mutable);
  }

  // An owned argument a base initializer moves into a by-value parameter of
  // its selected constructor: staged as a prepared parameter whose obligation
  // the InitializeBase hands to the base constructor, as a construction does.
  bool ownedBaseArgument(const HirConstructorInitializer &initializer,
                         std::size_t index) const {
    const auto *target =
        initializer.constructorTarget
            ? program.findConstructorInstance(*initializer.constructorTarget)
            : nullptr;
    if (initializer.kind != ConstructorInitializerTargetKind::Base || !target ||
        index >= target->parameterTypes.size() ||
        index >= initializer.arguments.size() ||
        std::any_of(target->parameterTypes.begin(),
                    target->parameterTypes.end(), [](const auto &type) {
                      return type.kind == SemanticType::Reference;
                    }))
      return false;
    const auto &parameter = target->parameterTypes[index];
    const auto *value = findValue(initializer.arguments[index]);
    return (parameter.kind == SemanticType::Class ||
            parameter.kind == SemanticType::Enum ||
            parameter.kind == SemanticType::Lambda) &&
           value != nullptr && value->dropObligation &&
           value->info.type == parameter &&
           value->info.category == ValueCategory::Value &&
           value->info.traits.movable &&
           value->info.traits.drop != DropKind::Trivial &&
           !resolvedTraits(parameter).containsBorrowedState &&
           preparedParameterDropType(parameter).has_value();
  }

  const HirClassInstance *
  stagedBase(const HirConstructorInitializer &initializer) const {
    return constructorUnarmedTransfer ? nullptr : stagedBaseShape(initializer);
  }

  const HirClassInstance *
  stagedBaseShape(const HirConstructorInitializer &initializer) const {
    // A base is a distinct under-construction place. Its exact selected
    // initializer completes before derived fields and arms one rollback token.
    // Argument effects stay in this MIR prefix; a backend must prove how that
    // prefix executes before its native base initializer, not move it later.
    if (constructor == nullptr || constructorOwner == nullptr ||
        initializer.kind != ConstructorInitializerTargetKind::Base ||
        (!initializer.constructorTarget && !initializer.generatedDefault) ||
        initializer.storesReference || initializer.ownedParameter ||
        std::any_of(
            output.dropObligations.begin(), output.dropObligations.end(),
            [](const MirDropObligation &drop) {
              return drop.kind == MirDropObligationKind::ConstructionRollback &&
                     drop.initiallyActive;
            }) ||
        std::none_of(constructorOwner->bases.begin(),
                     constructorOwner->bases.end(),
                     [&](const HirBaseInstance &base) {
                       return !base.interface &&
                              base.instance == initializer.base &&
                              base.type == initializer.targetType;
                     })) {
      return nullptr;
    }
    const auto base = std::find_if(
        program.classInstances().begin(), program.classInstances().end(),
        [&](const HirClassInstance &candidate) {
          return candidate.id == initializer.base &&
                 candidate.type == initializer.targetType;
        });
    if (base == program.classInstances().end() ||
        base->traits.containsBorrowedState) {
      return nullptr;
    }
    if (!initializer.constructorTarget &&
        (base->defaultConstructor != SpecialMemberStatus::Generated ||
         base->generatedDefaultRequiresSchedule ||
         std::any_of(
             base->bases.begin(), base->bases.end(),
             [](const HirBaseInstance &nested) { return !nested.interface; }) ||
         std::any_of(base->fields.begin(), base->fields.end(),
                     [](const HirClassField &field) {
                       return field.requiresActiveCleanup ||
                              field.info.traits.drop != DropKind::Trivial ||
                              field.info.traits.containsBorrowedState;
                     }) ||
         std::any_of(base->fieldInitializers.values.begin(),
                     base->fieldInitializers.values.end(),
                     [](const HirValue &value) {
                       return !value.definedFailure.empty() ||
                              value.functionTarget || value.constructorTarget ||
                              value.lambdaTarget;
                     })))
      return nullptr;
    const auto *selectedConstructor =
        initializer.constructorTarget
            ? program.findConstructorInstance(*initializer.constructorTarget)
            : nullptr;
    if (selectedConstructor && selectedConstructor->parameterTypes.size() !=
                                   initializer.arguments.size())
      return nullptr;
    const bool referenceFamily =
        selectedConstructor &&
        std::any_of(selectedConstructor->parameterTypes.begin(),
                    selectedConstructor->parameterTypes.end(),
                    [](const auto &type) {
                      return type.kind == SemanticType::Reference;
                    });
    for (std::size_t index = 0; index < initializer.arguments.size(); ++index) {
      if (forwardedBaseReference(initializer, index) ||
          ownedBaseArgument(initializer, index))
        continue;
      const auto *value = findValue(initializer.arguments[index]);
      if (!value ||
          (selectedConstructor &&
           selectedConstructor->parameterTypes[index].kind ==
               SemanticType::Reference) ||
          value->info.traits.drop != DropKind::Trivial ||
          resolvedTraits(value->info.type).containsBorrowedState ||
          value->info.type.kind == SemanticType::Reference)
        return nullptr;
      if (referenceFamily) {
        // This checkpoint cannot abandon a prepared reference across a later
        // effectful/terminal argument. Keep those valid source families
        // unstaged until the shared prepared-input protocol covers them.
        if (!value->definedFailure.empty() ||
            !isPassiveScalarValueType(value->info.type))
          return nullptr;
        if (value->kind == HirValueKind::Literal)
          continue;
        const auto local = localSymbols.find(value->symbol);
        if (value->kind != HirValueKind::Variable ||
            local == localSymbols.end())
          return nullptr;
        const auto formal =
            std::find(constructor->parameterBindings.begin(),
                      constructor->parameterBindings.end(), local->second);
        if (formal == constructor->parameterBindings.end() ||
            constructor->parameterTypes[static_cast<std::size_t>(
                formal - constructor->parameterBindings.begin())] !=
                value->info.type)
          return nullptr;
      }
    }
    return &*base;
  }

  void
  stageBaseInitializer(const HirConstructorInitializer &initializer,
                       std::size_t initializerIndex,
                       std::span<const std::optional<MirOperand>> prepared,
                       std::span<const MirDropObligationId> preparedDrops) {
    // Check the entire initializer before preparing any input. Unsupported
    // later arguments must not leave orphaned, partly staged CallInputs.
    const auto *base = stagedBase(initializer);
    if (!base)
      return;
    std::vector<MirOperand> arguments;
    std::vector<SemanticType> parameterTypes;
    const auto *selectedConstructor =
        initializer.constructorTarget
            ? program.findConstructorInstance(*initializer.constructorTarget)
            : nullptr;
    for (std::size_t index = 0; index < initializer.arguments.size(); ++index) {
      const auto argument = initializer.arguments[index];
      const HirValue *value = findValue(argument);
      if (forwardedBaseReference(initializer, index) ||
          ownedBaseArgument(initializer, index)) {
        const auto &parameter = selectedConstructor->parameterTypes[index];
        if (!prepared[index]) {
          valid = false;
          return;
        }
        arguments.push_back(*prepared[index]);
        parameterTypes.push_back(parameter);
        continue;
      }
      // A transient dependency on an incoming parameter is not borrowed
      // state stored in the base. Preserve the value's loan metadata while
      // classifying the parameter using the same concrete type contract as
      // program verification; the selected base itself cannot retain borrows.
      arguments.push_back(valueOperand(argument));
      parameterTypes.push_back(value->info.type);
    }
    const SemanticTypeTraits traits = resolvedTraits(initializer.targetType);
    const MirPlaceId destination =
        appendPlace({.root = MirPlaceRootKind::This,
                     .projections = {{.kind = MirProjectionKind::Base,
                                      .selection = base->id}},
                     .type = base->type,
                     .access = AccessMode::Mutable,
                     .traits = traits});
    const MirDropObligationId rollback = output.dropObligations.size() + 1;
    output.dropObligations.push_back(
        {.id = rollback,
         .constructionOrder = rollback,
         .kind = MirDropObligationKind::ConstructionRollback,
         .place = destination,
         .dropType = lowerDropType(
             {.type = base->type,
              .classInstance = base->id,
              .requiresActiveCleanup = base->requiresActiveCleanup})});
    const auto propagation = initializer.constructorTarget
                                 ? normalizedConstructorPropagation(
                                       FailurePropagationKind::Constructor,
                                       initializer.constructorTarget)
                                 : FailurePropagationKind::None;
    MirInstruction stage{.kind = MirInstructionKind::InitializeBase,
                         .constructorInitializer = initializerIndex + 1,
                         .successResultDrop = rollback,
                         .destination = destination,
                         .operands = std::move(arguments),
                         .parameterTypes = std::move(parameterTypes),
                         .definedFailure = {.propagation = propagation},
                         .constructorTarget = initializer.constructorTarget,
                         .info = {.type = base->type,
                                  .category = ValueCategory::Place,
                                  .access = AccessMode::Mutable,
                                  .traits = traits}};
    // The base constructor owns each prepared parameter once it begins.
    for (const MirDropObligationId drop : preparedDrops) {
      if (!removeTemporary(drop))
        valid = false;
      stage.lifecycle.push_back(
          {.kind = MirLifecycleEventKind::TransferOut, .source = drop});
    }
    if (propagation == FailurePropagationKind::None)
      stage.lifecycle.push_back(
          {.kind = MirLifecycleEventKind::Initialize, .target = rollback});
    (void)appendInstruction(std::move(stage));
    (void)removeTemporary(rollback);
    constructorRollback.push_back(rollback);
  }

  void seedDefaultFieldRollbacks(
      const std::vector<HirConstructorInitializer> *initializers) {
    if (output.kind != MirBodyKind::Constructor || constructor == nullptr ||
        constructorOwner == nullptr) {
      return;
    }

    std::unordered_set<SymbolId> explicitlyInitialized;
    if (initializers != nullptr) {
      for (const HirConstructorInitializer &initializer : *initializers) {
        if (initializer.kind == ConstructorInitializerTargetKind::Field &&
            initializer.field != 0) {
          explicitlyInitialized.insert(initializer.field);
        }
      }
    }

    for (const MirClassFieldInfo &field : constructorOwner->declaredFields) {
      if (field.dropKind != DropKind::Lexical || !field.requiresActiveCleanup ||
          explicitlyInitialized.contains(field.symbol)) {
        continue;
      }

      const MirDropObligation *initializerRollback = nullptr;
      for (const MirDropObligation &candidate :
           constructorOwner->fieldInitializers.dropObligations) {
        if (candidate.kind != MirDropObligationKind::ConstructionRollback) {
          continue;
        }
        const MirPlace *place =
            constructorOwner->fieldInitializers.findPlace(candidate.place);
        if (place == nullptr || place->root != MirPlaceRootKind::Binding ||
            place->binding != field.field || place->symbol != field.symbol ||
            !place->projections.empty() || place->type != field.type ||
            candidate.dropType.type != field.type) {
          continue;
        }
        if (initializerRollback != nullptr) {
          valid = false;
          initializerRollback = nullptr;
          break;
        }
        initializerRollback = &candidate;
      }
      if (initializerRollback == nullptr) {
        continue;
      }

      const MirPlaceId destination =
          appendPlace({.root = MirPlaceRootKind::This,
                       .projections = {{.kind = MirProjectionKind::Field,
                                        .field = field.symbol}},
                       .type = field.type,
                       .access = AccessMode::Mutable,
                       .traits = resolvedTraits(field.type)});
      const MirDropObligationId rollback = output.dropObligations.size() + 1;
      output.dropObligations.push_back(
          {.id = rollback,
           .constructionOrder = rollback,
           .kind = MirDropObligationKind::ConstructionRollback,
           .place = destination,
           .dropType = initializerRollback->dropType,
           .initiallyActive = true});
      constructorRollback.push_back(rollback);
    }
  }

  struct PreparedCallInput {
    MirOperand operand;
    MirDropObligationId parameterDrop = 0;
    MirLoanId loan = 0;
  };

  [[nodiscard]] static bool sameScopeState(const std::vector<Scope> &left,
                                           const std::vector<Scope> &right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](const Scope &lhs, const Scope &rhs) {
                        return lhs.drops == rhs.drops && lhs.loans == rhs.loans;
                      });
  }

  [[nodiscard]] static std::unordered_map<HirBindingId, MirLoanId>
  projectOuterBindingLoans(
      const std::unordered_map<HirBindingId, MirLoanId> &incoming,
      const std::unordered_map<HirBindingId, MirLoanId> &outgoing) {
    std::unordered_map<HirBindingId, MirLoanId> projected;
    projected.reserve(incoming.size());
    for (const auto &[binding, _] : incoming) {
      if (const auto found = outgoing.find(binding); found != outgoing.end()) {
        projected.emplace(binding, found->second);
      }
    }
    return projected;
  }

  struct BreakContext {
    MirBlockId target = 0;
    std::size_t keepScopes = 0;
    std::vector<SemanticLoanId> exitLoans;
  };

  struct ContinueContext {
    MirBlockId target = 0;
    std::size_t keepScopes = 0;
  };

  struct LoweredLoopShape {
    MirBlockId conditionEntry = 0;
    MirBlockId condition = 0;
    MirBlockId body = 0;
    MirBlockId increment = 0;
    MirBlockId cleanup = 0;
    MirBlockId exit = 0;
  };

  [[nodiscard]] const HirValue *findValue(HirValueId id) const {
    const auto found = values.find(id);
    return found == values.end() ? nullptr : found->second;
  }

  [[nodiscard]] const HirStatement *findStatement(HirStatementId id) const {
    const auto found = statements.find(id);
    return found == statements.end() ? nullptr : found->second;
  }

  [[nodiscard]] const HirBinding *findBinding(HirBindingId id) const {
    const auto found = bindings.find(id);
    return found == bindings.end() ? nullptr : found->second;
  }

  // A constructor initializer can transfer its owning temporary into an
  // exact field only when lowering can reparent that temporary's drop into a
  // ConstructionRollback obligation. Keep this one predicate shared by the
  // body-wide failure-routing prepass and the initializer staging step: if
  // those decisions diverge, an earlier failure edge can observe an unarmed
  // subobject. The rule is representation-neutral; storage and future owning
  // library values qualify through their drop obligation rather than a list
  // of compiler-known type kinds.
  [[nodiscard]] bool constructorRollbackStageEligible(
      const HirConstructorInitializer &initializer) const {
    if (constructor == nullptr ||
        initializer.kind != ConstructorInitializerTargetKind::Field ||
        initializer.field == 0 || initializer.storesReference ||
        initializer.generatedDefault || initializer.arguments.size() != 1) {
      return false;
    }
    const HirValue *argument = findValue(initializer.arguments.front());
    if (argument == nullptr || !argument->dropObligation ||
        argument->info.type != initializer.targetType) {
      return false;
    }
    const auto owner = std::find_if(program.classInstances().begin(),
                                    program.classInstances().end(),
                                    [&](const HirClassInstance &candidate) {
                                      return candidate.id == constructor->owner;
                                    });
    if (owner == program.classInstances().end()) {
      return false;
    }
    const auto field =
        std::find_if(owner->fields.begin(), owner->fields.end(),
                     [&](const HirClassField &candidate) {
                       return candidate.info.symbol == initializer.field;
                     });
    return field != owner->fields.end() &&
           field->info.type == initializer.targetType &&
           field->info.traits == argument->info.traits;
  }

  [[nodiscard]] FailurePropagationKind normalizedCallPropagation(
      FailurePropagationKind propagation, CallDispatch dispatch,
      const std::optional<HirFunctionInstanceId> &target) const {
    if (propagation != FailurePropagationKind::DirectCall ||
        dispatch != CallDispatch::Static || !target || *target == 0 ||
        *target > definedFailureEffects.functions.size()) {
      return propagation;
    }
    return definedFailureEffects.functions[*target - 1]
               ? FailurePropagationKind::DirectCall
               : FailurePropagationKind::None;
  }

  [[nodiscard]] FailurePropagationKind normalizedConstructorPropagation(
      FailurePropagationKind propagation,
      const std::optional<HirConstructorInstanceId> &target) const {
    if (propagation != FailurePropagationKind::Constructor || !target ||
        *target == 0 || *target > definedFailureEffects.constructors.size()) {
      return propagation;
    }
    return definedFailureEffects.constructors[*target - 1]
               ? FailurePropagationKind::Constructor
               : FailurePropagationKind::None;
  }

  // Whether destroying this obligation's value may raise, from its type's
  // destructor summaries alone.
  [[nodiscard]] bool destructorMayRaise(MirDropObligationId obligation) const {
    const MirDropObligation *drop = output.findDropObligation(obligation);
    if (drop == nullptr) {
      return false;
    }
    bool mayRaise = mirDropHasPolymorphicCleanup(drop->dropType);
    for (const auto target : mirDropDestructorTargets(drop->dropType)) {
      mayRaise = mayRaise || target == 0 ||
                 target > definedFailureEffects.destructors.size() ||
                 definedFailureEffects.destructors[target - 1];
    }
    return mayRaise;
  }

  [[nodiscard]] FailurePropagationKind
  normalizedDestructorPropagation(MirDropObligationId obligation) const {
    // A moved-from shell is retired without running its destructor, even
    // when the move happened before an invoke split its block.
    const MirBlock *block = output.findBlock(current);
    return destructorMayRaise(obligation) &&
                   (block == nullptr ||
                    !mirDropFollowsDefiniteMove(output, invokeContinuations,
                                                *block, obligation))
               ? FailurePropagationKind::Destructor
               : FailurePropagationKind::None;
  }

  [[nodiscard]] std::optional<MirDropType>
  uniqueCleanupType(const SemanticType &type,
                    std::unordered_set<HirClassInstanceId> &visiting) const {
    if (type.kind == SemanticType::Array) {
      if (type.arguments.size() != 1 || type.arrayLength == 0 ||
          type.arrayLengthParameterId != 0)
        return std::nullopt;
      auto child = uniqueCleanupType(type.arguments.front(), visiting);
      if (!child || !child->requiresActiveCleanup)
        return std::nullopt;
      SemanticType leaf = type;
      MirFixedArrayDropSchedule schedule;
      while (leaf.kind == SemanticType::Array) {
        if (leaf.arguments.size() != 1 || leaf.arrayLength == 0 ||
            leaf.arrayLengthParameterId != 0)
          return std::nullopt;
        schedule.extents.push_back(leaf.arrayLength);
        leaf = SemanticType(leaf.arguments.front());
      }
      schedule.leafType = leaf;
      const auto owner = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const auto &candidate) { return candidate.type == leaf; });
      std::optional<HirDestructorInstanceId> destructor;
      if (owner != program.classInstances().end()) {
        schedule.leafClassInstance = owner->id;
        destructor = owner->destructor;
      }
      return MirDropType{.type = type,
                         .destructor = destructor,
                         .fixedArray = std::move(schedule),
                         .aggregate = {*child},
                         .requiresActiveCleanup = true};
    }
    if (type.kind == SemanticType::Enum) {
      const auto enumeration = std::find_if(
          program.enumDeclarations().begin(), program.enumDeclarations().end(),
          [&](const auto &candidate) { return candidate.type == type; });
      if (enumeration == program.enumDeclarations().end() ||
          !enumeration->payload)
        return std::nullopt;
      MirDropType result{.type = type,
                         .requiresActiveCleanup =
                             enumeration->requiresActiveCleanup};
      for (const auto &variant : enumeration->enumerators) {
        for (std::size_t index = variant.payloadTypes.size(); index != 0;
             --index) {
          if (!variant.payloadRequiresActiveCleanup[index - 1])
            continue;
          auto child =
              uniqueCleanupType(variant.payloadTypes[index - 1], visiting);
          if (!child)
            return std::nullopt;
          child->payloadVariant = variant.variantIndex;
          child->payloadField = index - 1;
          result.aggregate.push_back(*child);
        }
      }
      return result;
    }
    if (type.kind == SemanticType::Lambda) {
      const auto lambda = std::find_if(
          program.lambdaInstances().begin(), program.lambdaInstances().end(),
          [&](const HirLambda &candidate) { return candidate.type == type; });
      if (lambda == program.lambdaInstances().end() ||
          lambda->captures.size() !=
              lambda->captureRequiresActiveCleanup.size())
        return std::nullopt;
      MirDropType result{.type = type, .lambdaInstance = lambda->id};
      for (std::size_t index = lambda->captures.size(); index != 0; --index) {
        if (!lambda->captureRequiresActiveCleanup[index - 1])
          continue;
        auto child =
            uniqueCleanupType(lambda->captures[index - 1].type, visiting);
        if (!child)
          return std::nullopt;
        child->captureIndex = index - 1;
        result.aggregate.push_back(*child);
      }
      result.requiresActiveCleanup = !result.aggregate.empty();
      return result;
    }
    if (type.kind == SemanticType::UniqueOwner && type.arguments.size() == 1) {
      const auto &value = type.arguments.front();
      if ((value.kind >= SemanticType::Int8 &&
           value.kind <= SemanticType::Char) ||
          value.kind == SemanticType::RawPointer ||
          value.kind == SemanticType::NativeFunction ||
          value.kind == SemanticType::NullPtr ||
          value.kind == SemanticType::CString ||
          value.kind == SemanticType::StringView ||
          (value.kind == SemanticType::Enum &&
           std::none_of(program.enumDeclarations().begin(),
                        program.enumDeclarations().end(),
                        [&](const auto &item) {
                          return item.type == value && item.payload;
                        }))) {
        return MirDropType{.type = type, .requiresActiveCleanup = true};
      }
      if (value.kind != SemanticType::Class)
        return std::nullopt;
      const auto pointee = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const HirClassInstance &candidate) {
            return candidate.type == type.arguments.front();
          });
      if (pointee != program.classInstances().end() && pointee->polymorphic)
        return MirDropType{.type = type,
                           .uniquePointeeClass = pointee->id,
                           .polymorphicCleanup = true,
                           .requiresActiveCleanup = true};
      if (pointee != program.classInstances().end() && pointee->destructor &&
          pointee->requiresActiveDropState && pointee->bases.empty() &&
          std::none_of(pointee->fields.begin(), pointee->fields.end(),
                       [](const HirClassField &field) {
                         return field.requiresActiveCleanup;
                       })) {
        return MirDropType{.type = type,
                           .destructor = pointee->destructor,
                           .uniquePointeeClass = pointee->id,
                           .requiresActiveCleanup = true};
      }
      if (pointee == program.classInstances().end())
        return std::nullopt;
      if (!pointee->requiresActiveCleanup)
        return MirDropType{.type = type,
                           .uniquePointeeClass = pointee->id,
                           .requiresActiveCleanup = true};
      auto child = uniqueCleanupType(pointee->type, visiting);
      if (!child)
        return std::nullopt;
      return MirDropType{.type = type,
                         .uniquePointeeClass = pointee->id,
                         .aggregate = {*child},
                         .requiresActiveCleanup = true};
    }
    if (type.kind != SemanticType::Class)
      return std::nullopt;
    const auto owner = std::find_if(program.classInstances().begin(),
                                    program.classInstances().end(),
                                    [&](const HirClassInstance &candidate) {
                                      return candidate.type == type;
                                    });
    if (owner != program.classInstances().end() &&
        owner->compilerCapability == CompilerCapabilityTypeKind::SharedOwner) {
      if (type.arguments.size() != 1)
        return std::nullopt;
      auto pointee = uniqueCleanupType(
          SemanticType::uniqueOwnerOf(type.arguments.front()), visiting);
      if (!pointee)
        return std::nullopt;
      return MirDropType{.type = type,
                         .classInstance = owner->id,
                         .aggregate = {*pointee},
                         .requiresActiveCleanup = true};
    }
    if (owner != program.classInstances().end() &&
        (owner->compilerCapability ==
             CompilerCapabilityTypeKind::MutexStorage ||
         owner->compilerCapability ==
             CompilerCapabilityTypeKind::OptionalStorage)) {
      if (type.arguments.size() != 1 || !owner->requiresActiveCleanup)
        return std::nullopt;
      auto payload = uniqueCleanupType(type.arguments.front(), visiting);
      const bool shell =
          owner->compilerCapability == CompilerCapabilityTypeKind::MutexStorage;
      if (!shell && (!payload || !payload->requiresActiveCleanup))
        return std::nullopt;
      if (shell && !payload) {
        const auto passive = [&](const auto &self,
                                 const SemanticType &value) -> bool {
          if (value.kind == SemanticType::Array)
            return value.arguments.size() == 1 &&
                   self(self, value.arguments.front());
          if (value.kind == SemanticType::Class)
            return std::any_of(program.classInstances().begin(),
                               program.classInstances().end(),
                               [&](const auto &instance) {
                                 return instance.type == value &&
                                        !instance.requiresActiveCleanup;
                               });
          if (value.kind == SemanticType::Enum)
            return std::any_of(program.enumDeclarations().begin(),
                               program.enumDeclarations().end(),
                               [&](const auto &enumeration) {
                                 return enumeration.type == value &&
                                        !enumeration.requiresActiveCleanup;
                               });
          if (value.kind == SemanticType::Lambda)
            return std::any_of(
                program.lambdaInstances().begin(),
                program.lambdaInstances().end(), [&](const auto &lambda) {
                  return lambda.type == value &&
                         std::none_of(
                             lambda.captureRequiresActiveCleanup.begin(),
                             lambda.captureRequiresActiveCleanup.end(),
                             [](bool active) { return active; });
                });
          return value.kind != SemanticType::UniqueOwner &&
                 value.kind != SemanticType::SharedPointer &&
                 value != SemanticType::Unknown;
        };
        if (!passive(passive, type.arguments.front()))
          return std::nullopt;
      }
      return MirDropType{.type = type,
                         .classInstance = owner->id,
                         .aggregate = payload
                                          ? std::vector<MirDropType>{*payload}
                                          : std::vector<MirDropType>{},
                         .requiresActiveCleanup = true};
    }
    if (owner != program.classInstances().end() && owner->destructor &&
        owner->requiresActiveDropState && owner->bases.empty() &&
        std::none_of(owner->fields.begin(), owner->fields.end(),
                     [](const HirClassField &field) {
                       return field.requiresActiveCleanup;
                     })) {
      return MirDropType{.type = type,
                         .classInstance = owner->id,
                         .destructor = owner->destructor,
                         .requiresActiveCleanup = true};
    }
    if (owner == program.classInstances().end() || owner->unionLayout ||
        (owner->destructor
             ? (!owner->requiresActiveDropState ||
                owner->destructorStatus != SpecialMemberStatus::Declared)
             : (owner->requiresActiveDropState ||
                owner->destructorStatus != SpecialMemberStatus::Generated)))
      return std::nullopt;
    if (!visiting.insert(owner->id).second)
      return MirDropType{.type = type,
                         .classInstance = owner->id,
                         .recursiveCleanup = true,
                         .requiresActiveCleanup = true};
    MirDropType selected{.type = type,
                         .classInstance = owner->id,
                         .destructor = owner->destructor,
                         .requiresActiveCleanup = true};
    bool complete = true;
    for (auto cursor = owner->fields.rbegin(); cursor != owner->fields.rend();
         ++cursor) {
      const auto &field = *cursor;
      if (!field.requiresActiveCleanup)
        continue;
      auto child = uniqueCleanupType(field.info.type, visiting);
      if (child) {
        child->uniqueOwnerField = field.info.symbol;
        selected.aggregate.push_back(*child);
      } else {
        complete = false;
      }
    }
    for (const auto &baseInfo : owner->bases) {
      const auto base = std::find_if(program.classInstances().begin(),
                                     program.classInstances().end(),
                                     [&](const HirClassInstance &candidate) {
                                       return candidate.id == baseInfo.instance;
                                     });
      if (base == program.classInstances().end()) {
        complete = false;
        continue;
      }
      if (!base->requiresActiveCleanup)
        continue;
      auto child = uniqueCleanupType(base->type, visiting);
      if (child) {
        child->uniqueOwnerBase = base->id;
        selected.aggregate.push_back(*child);
      } else {
        complete = false;
      }
    }
    visiting.erase(owner->id);
    if (!complete || (!selected.destructor && selected.aggregate.empty()))
      return std::nullopt;
    return selected;
  }

  [[nodiscard]] MirDropType lowerDropType(const HirDropType &type) const {
    MirDropType result{.type = type.type,
                       .classInstance = type.classInstance,
                       .lambdaInstance = type.lambdaInstance,
                       .destructor = type.destructor,
                       .requiresActiveCleanup = type.requiresActiveCleanup};
    std::unordered_set<HirClassInstanceId> visiting;
    if (auto unique = uniqueCleanupType(type.type, visiting))
      return *unique;
    if (type.type.kind != SemanticType::Array) {
      return result;
    }

    SemanticType leaf = type.type;
    MirFixedArrayDropSchedule schedule;
    while (leaf.kind == SemanticType::Array) {
      if (leaf.arguments.size() != 1 || leaf.arrayLengthParameterId != 0 ||
          leaf.arrayLength == 0) {
        return result;
      }
      schedule.extents.push_back(leaf.arrayLength);
      SemanticType element = leaf.arguments.front();
      leaf = std::move(element);
    }
    schedule.leafType = leaf;
    if (leaf.kind == SemanticType::Class) {
      const HirClassInstance *selected = nullptr;
      const HirClassInstance *exactSpecialization = nullptr;
      for (const HirClassInstance &candidate : program.classInstances()) {
        if (candidate.type == leaf) {
          selected = &candidate;
          break;
        }
        // Exact class specializations have their own stable ClassId and no
        // remaining generic arguments. Array element types can retain the
        // source arguments alongside that specialization identity, so match
        // the unique concrete instance by the stronger declaration identity.
        if (candidate.type.classId == leaf.classId &&
            candidate.type.arguments.empty() &&
            candidate.type.valueArguments.empty()) {
          if (exactSpecialization != nullptr) {
            return result;
          }
          exactSpecialization = &candidate;
        }
      }
      selected = selected == nullptr ? exactSpecialization : selected;
      if (selected != nullptr) {
        schedule.leafClassInstance = selected->id;
        result.destructor = selected->destructor;
      }
    }
    result.fixedArray = std::move(schedule);
    return result;
  }

  [[nodiscard]] MirDropObligationId
  ensureDropObligation(const HirDropObligation &obligation,
                       MirPlaceId place = 0) {
    if (const auto found = dropObligations.find(obligation.id);
        found != dropObligations.end()) {
      MirDropObligation &existing = output.dropObligations[found->second - 1];
      if (place != 0) {
        if (existing.place != 0 && existing.place != place) {
          valid = false;
        } else {
          existing.place = place;
        }
      }
      return found->second;
    }

    if (place == 0) {
      if (obligation.kind == HirDropObligationKind::Binding) {
        place = placeForBinding(obligation.binding);
        if (obligation.packElement) {
          const MirPlace *root = output.findPlace(place);
          if (root == nullptr || root->type.kind != SemanticType::TypePack ||
              !root->type.concretePack || !root->key ||
              *obligation.packElement >= root->type.arguments.size() ||
              root->type.arguments[*obligation.packElement] !=
                  obligation.dropType.type) {
            valid = false;
            return 0;
          }
          MirPlace element = *root;
          element.id = 0;
          element.projections.push_back(
              {.kind = MirProjectionKind::PackElement,
               .constantIndex = *obligation.packElement});
          element.key->projections.push_back(
              {.kind = PlaceProjectionKind::ConstantIndex,
               .index = *obligation.packElement});
          element.type = obligation.dropType.type;
          element.traits = resolvedTraits(element.type);
          place = appendPlace(std::move(element));
        }
      } else {
        const HirValue *value = findValue(obligation.value);
        if (value != nullptr) {
          place = appendPlace({.root = MirPlaceRootKind::Value,
                               .value = mirValueFor(*value),
                               .type = value->info.type,
                               .access = AccessMode::Mutable,
                               .traits = value->info.traits,
                               .sourceValue = value->id});
        }
      }
    }
    if (place == 0) {
      valid = false;
      return 0;
    }
    const MirDropObligationId id = output.dropObligations.size() + 1;
    output.dropObligations.push_back(
        {.id = id,
         .hirObligation = obligation.id,
         .constructionOrder = id,
         .kind = obligation.kind == HirDropObligationKind::Binding
                     ? MirDropObligationKind::Binding
                     : MirDropObligationKind::Value,
         .place = place,
         .binding = obligation.binding,
         .value = obligation.value,
         .hirFullExpression = obligation.fullExpression,
         .fullExpression = fullExpressionIds.contains(obligation.fullExpression)
                               ? fullExpressionIds.at(obligation.fullExpression)
                               : 0,
         .dropType = lowerDropType(obligation.dropType),
         .initiallyActive = obligation.initiallyActive});
    dropObligations.emplace(obligation.id, id);
    return id;
  }

  [[nodiscard]] MirDropObligationId
  dropObligationForBinding(HirBindingId bindingId) {
    const HirBinding *binding = findBinding(bindingId);
    if (binding == nullptr || !binding->dropObligation) {
      return 0;
    }
    const HirDropObligation *obligation =
        source.findDropObligation(*binding->dropObligation);
    return obligation == nullptr ? 0 : ensureDropObligation(*obligation);
  }

  [[nodiscard]] MirDropObligationId
  dropObligationForLocalPlace(const MirPlace *place) {
    if (place == nullptr || place->root != MirPlaceRootKind::Binding)
      return 0;
    if (place->projections.empty())
      return dropObligationForBinding(place->binding);
    for (const MirDropObligation &drop : output.dropObligations)
      if (drop.kind == MirDropObligationKind::Binding &&
          drop.place == place->id)
        return drop.id;
    return 0;
  }

  [[nodiscard]] MirDropObligationId
  dropObligationForValue(HirValueId valueId, MirPlaceId place = 0) {
    const HirValue *value = findValue(valueId);
    if (value == nullptr || !value->dropObligation) {
      return 0;
    }
    const HirDropObligation *obligation =
        source.findDropObligation(*value->dropObligation);
    return obligation == nullptr ? 0 : ensureDropObligation(*obligation, place);
  }

  [[nodiscard]] SemanticTypeTraits
  resolvedTraits(const SemanticType &type) const {
    if (type.kind == SemanticType::Enum) {
      const auto enumeration = std::find_if(
          program.enumDeclarations().begin(), program.enumDeclarations().end(),
          [&](const HirEnum &candidate) { return candidate.type == type; });
      if (enumeration != program.enumDeclarations().end())
        return enumeration->traits;
    }
    if (type.kind == SemanticType::Lambda) {
      const auto lambda = std::find_if(
          program.lambdaInstances().begin(), program.lambdaInstances().end(),
          [&](const HirLambda &candidate) { return candidate.type == type; });
      if (lambda != program.lambdaInstances().end())
        return lambda->traits;
    }
    if (type.kind == SemanticType::Class) {
      const auto instance = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const HirClassInstance &candidate) {
            return candidate.type == type;
          });
      if (instance != program.classInstances().end()) {
        return instance->traits;
      }
    }
    return semanticTraits(type);
  }

  [[nodiscard]] bool
  isLifecycleBoundBorrowCarrier(const SemanticType &type) const {
    return type.kind == SemanticType::Class &&
           std::any_of(program.classInstances().begin(),
                       program.classInstances().end(),
                       [&](const HirClassInstance &candidate) {
                         return candidate.type == type &&
                                candidate.lifecycleBoundBorrowCarrier;
                       });
  }

  [[nodiscard]] std::optional<MirDropType>
  preparedParameterDropType(const SemanticType &type) const {
    if (type.kind == SemanticType::Lambda) {
      const auto lambda = std::find_if(
          program.lambdaInstances().begin(), program.lambdaInstances().end(),
          [&](const HirLambda &candidate) { return candidate.type == type; });
      if (lambda == program.lambdaInstances().end() ||
          lambda->captures.size() !=
              lambda->captureRequiresActiveCleanup.size())
        return std::nullopt;
      return lowerDropType({.type = type,
                            .lambdaInstance = lambda->id,
                            .requiresActiveCleanup = std::any_of(
                                lambda->captureRequiresActiveCleanup.begin(),
                                lambda->captureRequiresActiveCleanup.end(),
                                [](bool active) { return active; })});
    }
    if (type.kind == SemanticType::Enum) {
      const auto enumeration = std::find_if(
          program.enumDeclarations().begin(), program.enumDeclarations().end(),
          [&](const HirEnum &candidate) { return candidate.type == type; });
      if (enumeration == program.enumDeclarations().end() ||
          !enumeration->requiresActiveCleanup) {
        return std::nullopt;
      }
      return lowerDropType({.type = type, .requiresActiveCleanup = true});
    }
    if (type.kind != SemanticType::Class) {
      return std::nullopt;
    }
    const auto instance = std::find_if(program.classInstances().begin(),
                                       program.classInstances().end(),
                                       [&](const HirClassInstance &candidate) {
                                         return candidate.type == type;
                                       });
    if (instance == program.classInstances().end()) {
      return std::nullopt;
    }
    return lowerDropType(
        {.type = type,
         .classInstance = instance->id,
         .destructor = instance->destructor,
         .requiresActiveCleanup = instance->requiresActiveCleanup});
  }

  [[nodiscard]] MirDropObligationId
  appendPreparedParameterDrop(HirValueId callSite, HirValueId sourceValue,
                              MirValueId result, const ExpressionInfo &info,
                              const MirDropType &dropType) {
    // A base initializer's input has no call value; its argument's full
    // expression is the initializer's.
    const HirValue *call =
        callSite != 0 ? findValue(callSite) : findValue(sourceValue);
    if (call == nullptr || call->fullExpression == 0 || result == 0) {
      valid = false;
      return 0;
    }
    const MirPlaceId place = appendPlace({.root = MirPlaceRootKind::Temporary,
                                          .temporary = nextTemporary++,
                                          .type = info.type,
                                          .access = AccessMode::Mutable,
                                          .traits = info.traits,
                                          .sourceValue = sourceValue});
    const MirDropObligationId id = output.dropObligations.size() + 1;
    output.dropObligations.push_back(
        {.id = id,
         .constructionOrder = id,
         .kind = MirDropObligationKind::PreparedParameter,
         .place = place,
         .hirFullExpression = call->fullExpression,
         .fullExpression = fullExpressionIds.contains(call->fullExpression)
                               ? fullExpressionIds.at(call->fullExpression)
                               : 0,
         .dropType = dropType});
    return id;
  }

  [[nodiscard]] MirDropObligationId
  appendObjectAllocation(const HirValue &call) {
    if (call.fullExpression == 0 ||
        call.info.type.kind != SemanticType::UniqueOwner ||
        call.info.type.arguments.size() != 1 ||
        call.info.type.arguments.front().kind != SemanticType::Class) {
      valid = false;
      return 0;
    }
    const MirPlaceId place = appendPlace({.root = MirPlaceRootKind::Temporary,
                                          .temporary = nextTemporary++,
                                          .type = call.info.type,
                                          .access = AccessMode::Mutable,
                                          .traits = call.info.traits,
                                          .sourceValue = call.id});
    const MirDropObligationId id = output.dropObligations.size() + 1;
    output.dropObligations.push_back(
        {.id = id,
         .constructionOrder = id,
         .kind = MirDropObligationKind::ObjectAllocation,
         .place = place,
         .hirFullExpression = call.fullExpression,
         .fullExpression = fullExpressionIds.contains(call.fullExpression)
                               ? fullExpressionIds.at(call.fullExpression)
                               : 0,
         .dropType = {.type = call.info.type, .requiresActiveCleanup = true}});
    DefinedFailureOperation failure = call.definedFailure;
    failure.propagation = FailurePropagationKind::None;
    (void)appendInstruction(
        {.kind = MirInstructionKind::AllocateObject,
         .hirValue = call.id,
         .destination = place,
         .definedFailure = std::move(failure),
         .objectAllocation = id,
         .info = call.info,
         .lifecycle = {
             {.kind = MirLifecycleEventKind::Initialize, .target = id}}});
    registerTemporary(id);
    return id;
  }

  [[nodiscard]] MirDropObligationId
  exactBindingDropObligation(HirValueId valueId) {
    const HirValue *value = findValue(valueId);
    if (value == nullptr) {
      return 0;
    }
    if (value->dropObligation) {
      return dropObligationForValue(valueId);
    }
    const MirPlaceId place = placeForValue(valueId);
    const MirPlace *resolved = output.findPlace(place);
    if (resolved == nullptr || resolved->root != MirPlaceRootKind::Binding ||
        !resolved->projections.empty()) {
      return 0;
    }
    return dropObligationForBinding(resolved->binding);
  }

  [[nodiscard]] auto temporaryDrop(MirDropObligationId obligation) {
    return std::find_if(temporaryDrops.begin(), temporaryDrops.end(),
                        [&](const TemporaryDrop &candidate) {
                          return candidate.obligation == obligation;
                        });
  }

  [[nodiscard]] auto temporaryDrop(MirDropObligationId obligation) const {
    return std::find_if(temporaryDrops.begin(), temporaryDrops.end(),
                        [&](const TemporaryDrop &candidate) {
                          return candidate.obligation == obligation;
                        });
  }

  [[nodiscard]] bool temporaryIsActive(MirDropObligationId obligation) const {
    return obligation != 0 && temporaryDrop(obligation) != temporaryDrops.end();
  }

  void registerTemporary(MirDropObligationId obligation,
                         bool conditional = false) {
    if (obligation == 0) {
      return;
    }
    if (auto found = temporaryDrop(obligation); found != temporaryDrops.end()) {
      found->conditional = found->conditional || conditional;
      return;
    }
    temporaryDrops.push_back(
        {.obligation = obligation, .conditional = conditional});
  }

  [[nodiscard]] bool removeTemporary(MirDropObligationId obligation) {
    const auto found = temporaryDrop(obligation);
    if (found == temporaryDrops.end()) {
      return false;
    }
    temporaryDrops.erase(found);
    return true;
  }

  [[nodiscard]] std::vector<TemporaryDrop>
  mergeTemporaryDrops(const std::vector<TemporaryDrop> &left,
                      const std::vector<TemporaryDrop> &right) {
    std::vector<TemporaryDrop> result = left;
    for (TemporaryDrop &candidate : result) {
      const auto found = std::find_if(
          right.begin(), right.end(), [&](const TemporaryDrop &other) {
            return other.obligation == candidate.obligation;
          });
      candidate.conditional =
          candidate.conditional || found == right.end() || found->conditional;
    }
    for (const TemporaryDrop &candidate : right) {
      if (std::none_of(result.begin(), result.end(),
                       [&](const TemporaryDrop &other) {
                         return other.obligation == candidate.obligation;
                       })) {
        result.push_back(
            {.obligation = candidate.obligation, .conditional = true});
      }
    }
    for (const TemporaryDrop &candidate : result) {
      if (!candidate.conditional) {
        continue;
      }
      MirDropObligation *obligation =
          candidate.obligation == 0 ||
                  candidate.obligation > output.dropObligations.size()
              ? nullptr
              : &output.dropObligations[candidate.obligation - 1];
      MirPlace *place = obligation == nullptr || obligation->place == 0 ||
                                obligation->place > output.places.size()
                            ? nullptr
                            : &output.places[obligation->place - 1];
      if (place == nullptr) {
        valid = false;
        continue;
      }
      if (place->root == MirPlaceRootKind::Value) {
        const MirValueId producedValue = place->value;
        const MirValue *value = output.findValue(producedValue);
        MirBlock *definitionBlock =
            value == nullptr || value->definitionBlock == 0 ||
                    value->definitionBlock > output.blocks.size()
                ? nullptr
                : &output.blocks[value->definitionBlock - 1];
        const auto definition =
            definitionBlock == nullptr
                ? std::vector<MirInstruction>::iterator{}
                : std::find_if(definitionBlock->instructions.begin(),
                               definitionBlock->instructions.end(),
                               [&](const MirInstruction &instruction) {
                                 return instruction.id == value->definition;
                               });
        const bool materializingKind =
            definitionBlock != nullptr &&
            definition != definitionBlock->instructions.end() &&
            (definition->kind == MirInstructionKind::Compute ||
             definition->kind == MirInstructionKind::Move ||
             definition->kind == MirInstructionKind::Call ||
             definition->kind == MirInstructionKind::Construct);
        const bool initializesObligation =
            materializingKind && definition->result == producedValue &&
            std::any_of(
                definition->lifecycle.begin(), definition->lifecycle.end(),
                [&](const MirLifecycleEvent &event) {
                  return event.target == candidate.obligation &&
                         (event.kind == MirLifecycleEventKind::Initialize ||
                          event.kind == MirLifecycleEventKind::Move ||
                          event.kind == MirLifecycleEventKind::Reparent);
                });
        const bool successEdgeInitializesObligation =
            materializingKind && definition->result == producedValue &&
            definition->successResultDrop == candidate.obligation &&
            definitionBlock->instructions.back().id == definition->id &&
            definitionBlock->terminator.kind == MirTerminatorKind::Invoke &&
            definitionBlock->terminator.invokeInstruction == definition->id &&
            definitionBlock->terminator.successLifecycle.size() == 1 &&
            definitionBlock->terminator.successLifecycle.front().kind ==
                MirLifecycleEventKind::Initialize &&
            definitionBlock->terminator.successLifecycle.front().source == 0 &&
            definitionBlock->terminator.successLifecycle.front().target ==
                candidate.obligation &&
            !definitionBlock->terminator.successLifecycle.front().conditional &&
            !definitionBlock->terminator.successLifecycle.front()
                 .failureCleanup;
        if ((!initializesObligation && !successEdgeInitializesObligation) ||
            (definition->destination &&
             *definition->destination != obligation->place)) {
          valid = false;
          continue;
        }
        // A branch-only owning value needs executable storage that remains
        // nameable after the CFG merge. Retarget its existing producer to the
        // hidden result slot; this is direct materialization, not an extra
        // observable move.
        if (!successEdgeInitializesObligation) {
          definition->destination = obligation->place;
        }
        place->root = MirPlaceRootKind::Temporary;
        place->temporary = nextTemporary++;
        place->value = 0;
      }
    }
    std::stable_sort(
        result.begin(), result.end(),
        [&](const TemporaryDrop &leftDrop, const TemporaryDrop &rightDrop) {
          const MirDropObligation *left =
              output.findDropObligation(leftDrop.obligation);
          const MirDropObligation *right =
              output.findDropObligation(rightDrop.obligation);
          return left != nullptr && right != nullptr &&
                 left->constructionOrder < right->constructionOrder;
        });
    return result;
  }

  void appendLifecycle(MirInstruction &instruction, MirLifecycleEvent event) {
    instruction.lifecycle.push_back(event);
  }

  void appendReparentOrTypedTransfer(MirInstruction &instruction,
                                     MirDropObligationId source,
                                     MirDropObligationId target) {
    const MirDropObligation *sourceDrop = output.findDropObligation(source);
    const MirDropObligation *targetDrop = output.findDropObligation(target);
    if (sourceDrop == nullptr || targetDrop == nullptr) {
      valid = false;
      return;
    }
    if (sourceDrop->dropType.type == targetDrop->dropType.type) {
      appendLifecycle(instruction, {.kind = MirLifecycleEventKind::Reparent,
                                    .source = source,
                                    .target = target});
      return;
    }
    // A conversion into a distinct owning wrapper transfers the source object
    // and begins a different typed lifetime. It is not a same-type reparenting
    // of one object identity.
    appendLifecycle(instruction, {.kind = MirLifecycleEventKind::TransferOut,
                                  .source = source});
    appendLifecycle(instruction, {.kind = MirLifecycleEventKind::Initialize,
                                  .target = target});
  }

  [[nodiscard]] MirBlockId appendBlock(MirFailureRecordId activeFailure = 0) {
    const MirBlockId id = output.blocks.size() + 1;
    output.blocks.push_back(
        {.id = id,
         .programInitializationStep = currentProgramInitializationStep,
         .activeFailure = activeFailure});
    return id;
  }

  [[nodiscard]] MirBlock *currentBlock() {
    return current == 0 || current > output.blocks.size()
               ? nullptr
               : &output.blocks[current - 1];
  }

  [[nodiscard]] bool terminated() const {
    return current == 0 || current > output.blocks.size() ||
           output.blocks[current - 1].terminator.kind !=
               MirTerminatorKind::None;
  }

  void terminate(MirTerminator terminator) {
    if (MirBlock *block = currentBlock();
        block != nullptr && block->terminator.kind == MirTerminatorKind::None) {
      block->terminator = std::move(terminator);
      invokeContinuations.record(*block);
    }
  }

  void
  appendFailureCleanupBoundary(std::vector<MirDropObligationId> obligations) {
    if (PlannedFailureCleanup *plan = planningCleanup()) {
      if (!obligations.empty()) {
        plan->steps.push_back({.kind = PlannedCleanupStep::Kind::Boundary});
      }
      return;
    }
    if (obligations.empty()) {
      return;
    }
    const std::size_t id = output.cleanupBoundaries.size() + 1;
    output.cleanupBoundaries.push_back({.id = id,
                                        .kind = MirCleanupBoundaryKind::Failure,
                                        .obligations = std::move(obligations)});
    (void)appendInstruction(
        {.kind = MirInstructionKind::Lifecycle, .cleanupBoundaryEnd = id});
  }

  void appendCleanupFailureControlFlow(MirInstructionId producerInstruction,
                                       MirFailureRecordId activeFailure) {
    const MirBlockId producerBlock = current;
    const MirBlock *producer = currentBlock();
    if (activeFailure == 0 || producer == nullptr ||
        producer->activeFailure != activeFailure ||
        producer->instructions.empty() ||
        producer->instructions.back().id != producerInstruction) {
      valid = false;
      return;
    }

    const MirBlockId normalBlock = appendBlock(activeFailure);
    const MirBlockId failureBlock = appendBlock(activeFailure);
    const MirFailureRecordId secondary = output.failureRecords.size() + 1;
    output.failureRecords.push_back({.id = secondary,
                                     .producerBlock = producerBlock,
                                     .producerInstruction = producerInstruction,
                                     .parameterBlock = failureBlock});
    output.blocks[failureBlock - 1].failureParameter = secondary;
    current = producerBlock;
    terminate({.kind = MirTerminatorKind::Invoke,
               .invokeInstruction = producerInstruction,
               .failureRecord = secondary,
               .target = normalBlock,
               .elseTarget = failureBlock});

    current = failureBlock;
    terminate({.kind = MirTerminatorKind::TerminateCleanupFailure,
               .failureRecord = secondary});
    current = normalBlock;
  }

  MirInstructionId appendFailureCleanupDrop(MirDropObligationId obligation,
                                            MirPlaceId destination,
                                            ExpressionInfo info,
                                            bool conditional = false) {
    if (PlannedFailureCleanup *plan = planningCleanup()) {
      // Decided here, where the drop's block still leads back to its
      // producer: a moved-from shell needs no destructor only on paths the
      // move proof can see.
      const FailurePropagationKind propagation =
          normalizedDestructorPropagation(obligation);
      // A prepared call argument's drop is counted along its own producer's
      // cleanup chain, so it never moves into a shared suffix.
      const MirDropObligation *record = output.findDropObligation(obligation);
      const bool prepared =
          record != nullptr &&
          record->kind == MirDropObligationKind::PreparedParameter;
      plan->steps.push_back(
          {.kind = PlannedCleanupStep::Kind::Drop,
           .obligation = obligation,
           .destination = destination,
           .info = std::move(info),
           .conditional = conditional,
           .propagation = propagation,
           .fallible = propagation == FailurePropagationKind::Destructor &&
                       routeFailureEdgesHere(),
           .contextFree =
               !prepared &&
               propagation == (destructorMayRaise(obligation)
                                   ? FailurePropagationKind::Destructor
                                   : FailurePropagationKind::None)});
      return 0;
    }
    MirInstruction drop{.kind = MirInstructionKind::Drop,
                        .destination = destination,
                        .info = std::move(info),
                        .lifecycle = {{.kind = MirLifecycleEventKind::Drop,
                                       .source = obligation,
                                       .conditional = conditional,
                                       .failureCleanup = true}}};
    drop.definedFailure.propagation =
        normalizedDestructorPropagation(obligation);
    const bool mayRaise =
        drop.definedFailure.propagation == FailurePropagationKind::Destructor;
    const MirInstructionId instruction =
        appendInstruction(std::move(drop), true, false);
    if (instruction != 0 && routeFailureEdgesHere() && mayRaise) {
      const MirBlock *block = currentBlock();
      appendCleanupFailureControlFlow(
          instruction, block == nullptr ? 0 : block->activeFailure);
    }
    return instruction;
  }

  [[nodiscard]] static bool
  containsDrop(const std::vector<MirDropObligationId> &drops,
               MirDropObligationId drop) {
    return std::find(drops.begin(), drops.end(), drop) != drops.end();
  }

  [[nodiscard]] static bool containsLoan(const std::vector<MirLoanId> &loans,
                                         MirLoanId loan) {
    return std::find(loans.begin(), loans.end(), loan) != loans.end();
  }

  [[nodiscard]] MirLoanId lifecycleLoanForBinding(HirBindingId binding) const {
    return lifecycleLoanForBinding(binding, scopes);
  }

  [[nodiscard]] MirLoanId
  lifecycleLoanForBinding(HirBindingId binding,
                          const std::vector<Scope> &scopeState) const {
    for (auto scope = scopeState.rbegin(); scope != scopeState.rend();
         ++scope) {
      for (auto candidate = scope->loans.rbegin();
           candidate != scope->loans.rend(); ++candidate) {
        const MirLoan *loan = output.findLoan(*candidate);
        if (loan != nullptr && loan->lifecycleBound && !loan->escapes &&
            std::find(loan->carriers.begin(), loan->carriers.end(), binding) !=
                loan->carriers.end()) {
          return loan->id;
        }
      }
    }
    return 0;
  }

  [[nodiscard]] MirLoanId lifecycleLoanForPlace(const MirPlace &place) const {
    return lifecycleLoanForPlace(place, scopes);
  }

  [[nodiscard]] MirLoanId
  lifecycleLoanForPlace(const MirPlace &place,
                        const std::vector<Scope> &scopeState) const {
    if (place.root != MirPlaceRootKind::Binding)
      return 0;
    if (!place.projections.empty()) {
      const auto found = packEntryLoans.find(place.id);
      const auto *loan = found == packEntryLoans.end()
                             ? nullptr
                             : output.findLoan(found->second);
      return loan != nullptr && loan->lifecycleBound && !loan->escapes
                 ? loan->id
                 : MirLoanId{0};
    }
    return lifecycleLoanForBinding(place.binding, scopeState);
  }

  [[nodiscard]] MirTemporaryCarrierDependency
  temporaryReceiverCarrier(const MirPlace &place) const {
    const auto carrier = findMirTemporaryCarrierDependency(output, place);
    if (!carrier)
      return {};
    // Both an ephemeral receiver and a borrowed operator result may protect
    // this physical shell. Its stored dependencies must survive conditional
    // joins until the exact temporary is destroyed, even after that shell
    // loan ends. An external-only operator result does not name this place;
    // ordinary stored/returned values keep their own transfer schedules.
    const bool receiver = std::any_of(
        output.loans.begin(), output.loans.end(), [&](const MirLoan &loan) {
          return (loan.kind == MirLoanKind::CallArgument ||
                  loan.kind == MirLoanKind::CallResult) &&
                 loan.source == place.id &&
                 loan.storedDependency == carrier.loan->id;
        });
    return receiver ? carrier : MirTemporaryCarrierDependency{};
  }

  [[nodiscard]] std::unordered_set<MirLoanId> temporaryCarrierLoans(
      const std::vector<MirDropObligationId> &retired = {}) const {
    std::unordered_set<MirLoanId> retained;
    for (const TemporaryDrop &temporary : temporaryDrops) {
      if (containsDrop(retired, temporary.obligation))
        continue;
      const auto *drop = output.findDropObligation(temporary.obligation);
      const auto *place =
          drop == nullptr ? nullptr : output.findPlace(drop->place);
      const auto carrier = place == nullptr ? MirTemporaryCarrierDependency{}
                                            : temporaryReceiverCarrier(*place);
      if (!carrier)
        continue;
      for (const MirLoan &loan : output.loans) {
        if (mirLoanRetainsDependency(output, carrier.loan->id, loan.id))
          retained.insert(loan.id);
      }
    }
    return retained;
  }

  [[nodiscard]] std::vector<MirLoanId>
  retiringTemporaryCarrierLoans(MirDropObligationId obligation,
                                const std::vector<MirDropObligationId> &retired,
                                const std::vector<MirLoanId> &alreadyEnded = {},
                                bool failureCleanup = false) const {
    const auto *drop = output.findDropObligation(obligation);
    const auto *place =
        drop == nullptr ? nullptr : output.findPlace(drop->place);
    const auto carrier = place == nullptr ? MirTemporaryCarrierDependency{}
                                          : temporaryReceiverCarrier(*place);
    if (!carrier)
      return {};
    auto retained = failureCleanup ? temporaryCarrierLoans(retired)
                                   : retainedReborrowAncestors(retired);
    // Retiring this temporary does not retire an independently owned carrier
    // whose cleanup still needs the same resource ancestry. Exclude only the
    // just-dropped temporary's own lifecycle root, then retain every remaining
    // live cleanup-bound carrier and all of its parent/dependency ancestors.
    // Otherwise a passive cursor can release its provider before the storage
    // owner has performed its resource deallocation on a failure edge.
    auto retiredLoans = alreadyEnded;
    retiredLoans.push_back(carrier.loan->id);
    const auto lifecycleRetained = lifecycleRetainedLoans(retiredLoans);
    retained.insert(lifecycleRetained.begin(), lifecycleRetained.end());
    std::vector<MirLoanId> result;
    // Producers precede their dependents in the lowering scope inventory.
    // Retire the carrier's dependency before any transient ancestor.
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      for (auto candidate = scope->loans.rbegin();
           candidate != scope->loans.rend(); ++candidate) {
        const auto *loan = output.findLoan(*candidate);
        if (loan != nullptr && (failureCleanup || !loan->entry) &&
            (!loan->lifecycleBound || loan->id == carrier.loan->id) &&
            !loan->escapes && (failureCleanup || loan->carriers.empty()) &&
            !retained.contains(loan->id) &&
            !containsLoan(alreadyEnded, loan->id) &&
            mirLoanRetainsDependency(output, carrier.loan->id, loan->id)) {
          result.push_back(loan->id);
        }
      }
    }
    // The exact temporary has just been retired, including its own cleanup
    // obligation. End that Stored child before any resource ancestor; other
    // lifecycle-bound carriers still belong to their own cleanup schedule.
    return childFirstMirLoans(result);
  }

  [[nodiscard]] std::unordered_set<MirLoanId> lifecycleRetainedLoans(
      const std::vector<MirLoanId> &ended,
      const std::vector<Scope> *selectedScopes = nullptr) const {
    std::unordered_set<MirLoanId> retained;
    std::vector<MirLoanId> pending;
    for (const Scope &scope : selectedScopes ? *selectedScopes : scopes)
      for (const auto id : scope.loans) {
        const auto *loan = output.findLoan(id);
        if (loan != nullptr && loan->lifecycleBound && !containsLoan(ended, id))
          pending.push_back(id);
      }
    while (!pending.empty()) {
      const auto id = pending.back();
      pending.pop_back();
      if (id == 0 || containsLoan(ended, id) || !retained.insert(id).second)
        continue;
      if (const auto *loan = output.findLoan(id)) {
        pending.push_back(loan->parent);
        pending.push_back(loan->storedDependency);
        for (const auto &dependency : loan->additionalDependencies)
          pending.push_back(dependency.parent);
      }
    }
    return retained;
  }

  // The loans of `scope` that its cleanup ends now, given the loans already
  // ended on this path: child first, keeping lifecycle-bound loans and their
  // ancestors for the drop of their carrier.
  [[nodiscard]] std::vector<MirLoanId>
  scopeLoansToEnd(const Scope &scope, const std::vector<MirLoanId> &ended,
                  bool failureCleanup,
                  const std::vector<Scope> *scopeState = nullptr) const {
    const auto retained = lifecycleRetainedLoans(ended, scopeState);
    const auto *returned = output.findLoan(pendingReturnLoan);
    std::vector<MirLoanId> ending;
    for (const auto id : childFirstMirLoans(scope.loans)) {
      if (containsLoan(ended, id) || retained.contains(id) ||
          containsLoan(constructorPublishedLoans, id) ||
          (!failureCleanup && returned != nullptr &&
           returned->storedDependency == id))
        continue;
      ending.push_back(id);
    }
    return ending;
  }

  void endScopeLoansBeforeCleanup(const Scope &scope,
                                  std::vector<MirLoanId> &ended,
                                  bool failureCleanup) {
    for (const auto id : scopeLoansToEnd(scope, ended, failureCleanup)) {
      (void)appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = id});
      ended.push_back(id);
    }
  }

  void endPendingReturnLoanAfterDrop(MirDropObligationId drop,
                                     std::vector<MirLoanId> &ended) {
    const auto *loan = output.findLoan(pendingReturnLoan);
    if (drop == pendingReturnDrop && loan != nullptr && loan->lifecycleBound &&
        !containsLoan(ended, loan->id)) {
      (void)appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = loan->id});
      ended.push_back(loan->id);
    }
  }

  void
  endUncarriedLifecycleLoans(bool updateState = true,
                             std::vector<MirLoanId> *alreadyEnded = nullptr) {
    std::vector<MirLoanId> ended;
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      for (auto loan = scope->loans.rbegin(); loan != scope->loans.rend();
           ++loan) {
        const MirLoan *record = output.findLoan(*loan);
        if (record == nullptr) {
          valid = false;
          continue;
        }
        if (!record->lifecycleBound || !record->carriers.empty() ||
            record->escapes) {
          continue;
        }
        if (!containsLoan(ended, record->id) &&
            (alreadyEnded == nullptr ||
             !containsLoan(*alreadyEnded, record->id))) {
          ended.push_back(record->id);
        }
      }
    }
    for (const MirLoanId loan : ended) {
      (void)appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = loan});
      if (alreadyEnded != nullptr)
        alreadyEnded->push_back(loan);
      if (updateState) {
        removeLoanFromState(loan, scopes, bindingLoans);
      }
    }
  }

  void emitFailureTemporaryCleanup(
      const std::vector<TemporaryDrop> &activeTemporaries,
      const std::vector<MirDropObligationId> &consumedDrops,
      std::vector<MirLoanId> &endedLoans) {
    std::vector<MirDropObligationId> cleanup;
    auto retired = consumedDrops;
    cleanup.reserve(activeTemporaries.size());
    for (auto candidate = activeTemporaries.rbegin();
         candidate != activeTemporaries.rend(); ++candidate) {
      if (containsDrop(consumedDrops, candidate->obligation)) {
        continue;
      }
      const MirDropObligation *obligation =
          output.findDropObligation(candidate->obligation);
      const MirPlace *place =
          obligation == nullptr ? nullptr : output.findPlace(obligation->place);
      if (obligation == nullptr || place == nullptr) {
        valid = false;
        continue;
      }
      (void)appendFailureCleanupDrop(
          candidate->obligation, obligation->place,
          ExpressionInfo{.type = obligation->dropType.type,
                         .category = ValueCategory::Place,
                         .access = AccessMode::Mutable,
                         .traits = place->traits},
          candidate->conditional);
      endPendingReturnLoanAfterDrop(candidate->obligation, endedLoans);
      cleanup.push_back(candidate->obligation);
      retired.push_back(candidate->obligation);
      for (const auto loan : retiringTemporaryCarrierLoans(
               candidate->obligation, retired, endedLoans, true)) {
        (void)appendInstruction(
            {.kind = MirInstructionKind::EndBorrow, .loan = loan});
        endedLoans.push_back(loan);
      }
    }
    endUncarriedLifecycleLoans(false, &endedLoans);
    appendFailureCleanupBoundary(std::move(cleanup));
  }

  void
  emitFailureScopeCleanup(const Scope &scope,
                          const std::vector<MirDropObligationId> &consumedDrops,
                          std::vector<MirLoanId> &endedLoans) {
    endScopeLoansBeforeCleanup(scope, endedLoans, true);
    std::vector<MirDropObligationId> cleanup;
    cleanup.reserve(scope.drops.size());
    for (auto drop = scope.drops.rbegin(); drop != scope.drops.rend(); ++drop) {
      const MirPlace *place = output.findPlace(*drop);
      const MirDropObligationId obligation = dropObligationForLocalPlace(place);
      if (containsDrop(consumedDrops, obligation)) {
        continue;
      }
      if (place == nullptr || obligation == 0) {
        valid = false;
        continue;
      }
      const MirLoanId lifecycleLoan = lifecycleLoanForPlace(*place);
      (void)appendFailureCleanupDrop(
          obligation, *drop,
          ExpressionInfo{.type = place->type,
                         .category = ValueCategory::Place,
                         .access = place->access,
                         .traits = place->traits});
      if (lifecycleLoan != 0 && !containsLoan(endedLoans, lifecycleLoan)) {
        (void)appendInstruction(
            {.kind = MirInstructionKind::EndBorrow, .loan = lifecycleLoan});
        endedLoans.push_back(lifecycleLoan);
      }
      endScopeLoansBeforeCleanup(scope, endedLoans, true);
      cleanup.push_back(obligation);
    }
    appendFailureCleanupBoundary(std::move(cleanup));
  }

  // Shared failure cleanup.
  //
  // Function and lambda bodies record each failure edge's cleanup as a plan
  // of steps at the edge and emit every plan once the body is complete.
  // Plans that end in the same steps share one emitted copy of that suffix,
  // a continuation record entered by ContinueFailure edges, so cleanup grows
  // with distinct cleanup states rather than with failure sites times live
  // owners. A step is shared only when its meaning does not depend on the
  // path that reaches it. A suffix is shared only between plans whose loans
  // left live after cleanup agree, because loan flow state must be equal on
  // every edge into a block. A plan that shares nothing is emitted exactly
  // as an unshared chain.
  [[nodiscard]] bool shareFailureCleanup() const {
    return shareFailureCleanupEnabled &&
           (output.kind == MirBodyKind::Function ||
            output.kind == MirBodyKind::Lambda);
  }

  struct PlannedCleanupStep {
    enum class Kind : std::uint8_t { EndBorrow, Drop, Boundary };
    Kind kind = Kind::EndBorrow;
    MirLoanId loan = 0;
    MirDropObligationId obligation = 0;
    MirPlaceId destination = 0;
    ExpressionInfo info;
    bool conditional = false;
    FailurePropagationKind propagation = FailurePropagationKind::None;
    // Routes an Invoke and a secondary record for its destructor.
    bool fallible = false;
    // False for a drop proved to retire a moved-from shell: the proof walks
    // back to the drop's producer, which a shared copy cannot repeat.
    bool contextFree = true;
  };

  struct PlannedFailureCleanup {
    MirFailureRecordId record = 0;
    MirBlockId parameterBlock = 0;
    std::vector<PlannedCleanupStep> steps;
    std::vector<MirLoanId> residualLoans;
  };

  [[nodiscard]] PlannedFailureCleanup *planningCleanup() {
    return planningFailureCleanup
               ? &plannedFailureCleanup[*planningFailureCleanup]
               : nullptr;
  }

  // Loans still live once this cleanup ends: the active scope loans and
  // pending publications it does not end.
  [[nodiscard]] std::vector<MirLoanId>
  residualCleanupLoans(const PlannedFailureCleanup &plan,
                       const std::vector<MirLoanId> &ended) const {
    std::vector<MirLoanId> retired = ended;
    for (const PlannedCleanupStep &step : plan.steps) {
      if (step.kind == PlannedCleanupStep::Kind::EndBorrow) {
        retired.push_back(step.loan);
      }
    }
    std::sort(retired.begin(), retired.end());
    std::vector<MirLoanId> live;
    for (const Scope &scope : scopes) {
      live.insert(live.end(), scope.loans.begin(), scope.loans.end());
    }
    if (pendingReturnLoan != 0) {
      live.push_back(pendingReturnLoan);
    }
    live.insert(live.end(), constructorPublishedLoans.begin(),
                constructorPublishedLoans.end());
    std::sort(live.begin(), live.end());
    live.erase(std::unique(live.begin(), live.end()), live.end());
    std::erase_if(live, [&](MirLoanId loan) {
      return std::binary_search(retired.begin(), retired.end(), loan);
    });
    return live;
  }

  // Every plan's steps interned by identical suffix. Each plan is a path of
  // nodes ending at an end node for its residual loans; a node passed by
  // several plans and entered from more than one predecessor or plan is a
  // join, and becomes the entry of one shared continuation.
  struct CleanupSharing {
    struct Node {
      bool end = false;
      bool join = false;
      std::size_t plan = 0;
      std::size_t step = 0;
      std::size_t next = 0;
      std::size_t passes = 0;
      std::size_t incoming = 0;
      MirFailureRecordId continuation = 0;
      MirBlockId entry = 0;
    };
    std::vector<Node> nodes;
    std::vector<std::size_t> heads;
    std::size_t joins = 0;
  };

  [[nodiscard]] CleanupSharing shareCleanupSuffixes(bool share) const {
    CleanupSharing sharing;
    sharing.nodes.emplace_back();
    std::map<std::vector<MirLoanId>, std::size_t> ends;
    std::unordered_map<std::size_t, std::vector<std::size_t>> interned;
    const auto sameStep = [](const PlannedCleanupStep &left,
                             const PlannedCleanupStep &right) {
      return left.kind == right.kind && left.loan == right.loan &&
             left.obligation == right.obligation &&
             left.destination == right.destination &&
             left.conditional == right.conditional &&
             left.propagation == right.propagation &&
             left.fallible == right.fallible && left.info == right.info;
    };
    for (std::size_t planIndex = 0; planIndex < plannedFailureCleanup.size();
         ++planIndex) {
      const PlannedFailureCleanup &plan = plannedFailureCleanup[planIndex];
      std::size_t next = 0;
      if (share) {
        const auto [found, inserted] =
            ends.try_emplace(plan.residualLoans, sharing.nodes.size());
        if (inserted) {
          sharing.nodes.push_back({.end = true});
        }
        next = found->second;
      } else {
        next = sharing.nodes.size();
        sharing.nodes.push_back({.end = true});
      }
      for (std::size_t index = plan.steps.size(); index-- > 0;) {
        const PlannedCleanupStep &step = plan.steps[index];
        const bool shareable = share && step.contextFree;
        std::size_t hash = next;
        for (const std::size_t part :
             {static_cast<std::size_t>(step.kind), step.loan, step.obligation,
              step.destination, static_cast<std::size_t>(step.conditional),
              static_cast<std::size_t>(step.propagation),
              static_cast<std::size_t>(step.fallible)}) {
          hash ^= part + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        }
        std::size_t node = 0;
        if (shareable) {
          for (const std::size_t candidate : interned[hash]) {
            const CleanupSharing::Node &existing = sharing.nodes[candidate];
            if (existing.next == next &&
                sameStep(
                    plannedFailureCleanup[existing.plan].steps[existing.step],
                    step)) {
              node = candidate;
              break;
            }
          }
        }
        if (node == 0) {
          node = sharing.nodes.size();
          sharing.nodes.push_back(
              {.plan = planIndex, .step = index, .next = next});
          if (shareable) {
            interned[hash].push_back(node);
          }
        }
        next = node;
      }
      sharing.heads.push_back(next);
    }
    for (const std::size_t head : sharing.heads) {
      ++sharing.nodes[head].incoming;
      for (std::size_t node = head; !sharing.nodes[node].end;
           node = sharing.nodes[node].next) {
        ++sharing.nodes[node].passes;
      }
    }
    for (std::size_t node = 1; node < sharing.nodes.size(); ++node) {
      if (!sharing.nodes[node].end) {
        ++sharing.nodes[sharing.nodes[node].next].incoming;
      }
    }
    for (CleanupSharing::Node &node : sharing.nodes) {
      node.join = !node.end && node.passes >= 2 && node.incoming >= 2;
      sharing.joins += node.join ? 1 : 0;
    }
    return sharing;
  }

  // Emits one run of cleanup into the current block: a plan's private
  // prefix, or a shared continuation from its entry node. A failure-cleanup
  // boundary closes at each recorded boundary and wherever the run ends, so
  // no boundary spans a join.
  void emitCleanupRun(const CleanupSharing &sharing, std::size_t start,
                      MirFailureRecordId record, bool continuation) {
    std::vector<MirDropObligationId> group;
    std::size_t node = start;
    bool first = continuation;
    while (!sharing.nodes[node].end && (first || !sharing.nodes[node].join)) {
      first = false;
      const CleanupSharing::Node &entry = sharing.nodes[node];
      const PlannedCleanupStep &step =
          plannedFailureCleanup[entry.plan].steps[entry.step];
      switch (step.kind) {
      case PlannedCleanupStep::Kind::EndBorrow:
        (void)appendInstruction(
            {.kind = MirInstructionKind::EndBorrow, .loan = step.loan});
        break;
      case PlannedCleanupStep::Kind::Drop: {
        MirInstruction drop{.kind = MirInstructionKind::Drop,
                            .destination = step.destination,
                            .info = step.info,
                            .lifecycle = {{.kind = MirLifecycleEventKind::Drop,
                                           .source = step.obligation,
                                           .conditional = step.conditional,
                                           .failureCleanup = true}}};
        drop.definedFailure.propagation = step.propagation;
        const MirInstructionId instruction =
            appendInstruction(std::move(drop), true, false);
        if (instruction != 0 && step.fallible) {
          const MirBlock *block = currentBlock();
          appendCleanupFailureControlFlow(
              instruction, block == nullptr ? 0 : block->activeFailure);
        }
        group.push_back(step.obligation);
        break;
      }
      case PlannedCleanupStep::Kind::Boundary:
        appendFailureCleanupBoundary(std::move(group));
        group.clear();
        break;
      }
      node = entry.next;
    }
    appendFailureCleanupBoundary(std::move(group));
    if (sharing.nodes[node].end) {
      terminate({.kind = MirTerminatorKind::PropagateFailure,
                 .failureRecord = record});
    } else {
      terminate({.kind = MirTerminatorKind::ContinueFailure,
                 .failureRecord = sharing.nodes[node].continuation,
                 .target = sharing.nodes[node].entry});
    }
  }

  void emitPlannedFailureCleanup(CleanupSharing &sharing) {
    for (CleanupSharing::Node &node : sharing.nodes) {
      if (!node.join) {
        continue;
      }
      node.entry = appendBlock();
      node.continuation = output.failureRecords.size() + 1;
      output.failureRecords.push_back(
          {.id = node.continuation,
           .kind = MirFailureRecordKind::Continuation,
           .parameterBlock = node.entry});
      output.blocks[node.entry - 1].failureParameter = node.continuation;
      output.blocks[node.entry - 1].activeFailure = node.continuation;
    }
    for (std::size_t index = 1; index < sharing.nodes.size(); ++index) {
      if (sharing.nodes[index].join) {
        current = sharing.nodes[index].entry;
        emitCleanupRun(sharing, index, sharing.nodes[index].continuation, true);
      }
    }
    for (std::size_t planIndex = 0; planIndex < plannedFailureCleanup.size();
         ++planIndex) {
      const PlannedFailureCleanup &plan = plannedFailureCleanup[planIndex];
      const std::size_t head = sharing.heads[planIndex];
      current = plan.parameterBlock;
      if (sharing.nodes[head].join) {
        terminate({.kind = MirTerminatorKind::ContinueFailure,
                   .failureRecord = sharing.nodes[head].continuation,
                   .target = sharing.nodes[head].entry});
      } else {
        emitCleanupRun(sharing, head, plan.record, false);
      }
    }
  }

  void canonicalizeFullExpressions() {
    // Nested result regions finish their own expressions before the enclosing
    // expression. Publication order is therefore not source identity order.
    // Canonicalize once per completed candidate, repairing body-local and
    // startup-plan references rather than weakening the ordering rule.
    if (std::is_sorted(output.fullExpressions.begin(),
                       output.fullExpressions.end(),
                       [](const auto &left, const auto &right) {
                         return left.hirExpression < right.hirExpression;
                       }))
      return;
    std::vector<HirFullExpressionId> remap(output.fullExpressions.size() + 1);
    std::sort(output.fullExpressions.begin(), output.fullExpressions.end(),
              [](const auto &left, const auto &right) {
                return left.hirExpression < right.hirExpression;
              });
    fullExpressionIds.clear();
    for (std::size_t index = 0; index < output.fullExpressions.size();
         ++index) {
      auto &expression = output.fullExpressions[index];
      if (expression.id == 0 || expression.id >= remap.size() ||
          remap[expression.id] != 0) {
        valid = false;
        return;
      }
      remap[expression.id] = index + 1;
      expression.id = index + 1;
      fullExpressionIds.emplace(expression.hirExpression, expression.id);
    }
    const auto repair = [&](HirFullExpressionId &id) {
      if (id >= remap.size() || (id != 0 && remap[id] == 0))
        valid = false;
      else
        id = remap[id];
    };
    for (auto &drop : output.dropObligations)
      repair(drop.fullExpression);
    for (auto &block : output.blocks)
      for (auto &instruction : block.instructions)
        repair(instruction.fullExpressionEnd);
    if (loweredProgramInitialization)
      for (auto &step : loweredProgramInitialization->steps) {
        repair(step.fullExpression);
        for (auto &id : step.nestedFullExpressions)
          repair(id);
        std::sort(step.nestedFullExpressions.begin(),
                  step.nestedFullExpressions.end());
      }
  }

  void finishBodyVerification() {
    canonicalizeFullExpressions();
    rebuildMirReachability(output);
    valid = rebuildMirValueUses(output) && valid;
    valid = validateSourceProvenance() && valid;
    finishSourceDebug();
    verification = verifyMirBody(output);
    valid = verification.valid() && valid;
  }

  // Emits every planned failure cleanup, sharing suffixes, then verifies the
  // body. Should the shared schedule not verify, the body is lowered again
  // with one cleanup chain per failure edge, its established form.
  void finishFailureCleanupAndVerify() {
    if (plannedFailureCleanup.empty()) {
      finishBodyVerification();
      return;
    }
    CleanupSharing shared = shareCleanupSuffixes(true);
    if (shared.joins == 0) {
      emitPlannedFailureCleanup(shared);
      finishBodyVerification();
      return;
    }
    MirBody unshared = output;
    const auto unsharedFullExpressionIds = fullExpressionIds;
    const auto unsharedInitialization =
        loweredProgramInitialization
            ? std::optional(*loweredProgramInitialization)
            : std::nullopt;
    const MirInvokeContinuations continuations = invokeContinuations;
    const MirInstructionId instruction = nextInstruction;
    const bool previouslyValid = valid;
    emitPlannedFailureCleanup(shared);
    finishBodyVerification();
    if (verification.valid()) {
      return;
    }
    output = std::move(unshared);
    fullExpressionIds = unsharedFullExpressionIds;
    if (unsharedInitialization)
      *loweredProgramInitialization = *unsharedInitialization;
    invokeContinuations = continuations;
    nextInstruction = instruction;
    valid = previouslyValid;
    CleanupSharing separate = shareCleanupSuffixes(false);
    emitPlannedFailureCleanup(separate);
    finishBodyVerification();
  }

  void appendFailureControlFlow(
      MirInstructionId producerInstruction,
      MirDropObligationId successResultDrop,
      const std::vector<MirDropObligationId> &consumedDrops = {},
      const std::vector<MirLoanId> &endedLoans = {},
      const std::vector<MirLoanId> &producerEndedLoans = {}) {
    const MirBlockId producerBlock = current;
    const MirBlock *producer = currentBlock();
    if (producer == nullptr || producer->instructions.empty() ||
        producer->instructions.back().id != producerInstruction) {
      valid = false;
      return;
    }

    const MirBlockId normalBlock = appendBlock();
    const MirBlockId failureBlock = appendBlock();
    const MirFailureRecordId failureRecord = output.failureRecords.size() + 1;
    output.failureRecords.push_back({.id = failureRecord,
                                     .producerBlock = producerBlock,
                                     .producerInstruction = producerInstruction,
                                     .parameterBlock = failureBlock});
    output.blocks[failureBlock - 1].failureParameter = failureRecord;
    output.blocks[failureBlock - 1].activeFailure = failureRecord;
    current = producerBlock;
    MirTerminator invoke{.kind = MirTerminatorKind::Invoke,
                         .invokeInstruction = producerInstruction,
                         .failureRecord = failureRecord,
                         .target = normalBlock,
                         .elseTarget = failureBlock};
    if (successResultDrop != 0) {
      invoke.successLifecycle.push_back(
          {.kind = MirLifecycleEventKind::Initialize,
           .target = successResultDrop});
    }
    terminate(std::move(invoke));

    current = failureBlock;
    // Endpoint evaluation may fail before publishing its sentinel. Restore
    // the cursor capability before draining any caller-owned cursor/owner:
    // its destructor is ordinary checked code and can use the borrowed owner.
    const auto *endpointProducerBlock = output.findBlock(producerBlock);
    const auto *endpointProducer =
        endpointProducerBlock && !endpointProducerBlock->instructions.empty()
            ? &endpointProducerBlock->instructions.back()
            : nullptr;
    if (parkedRangeEndpoint && endpointProducer &&
        endpointProducer->kind == MirInstructionKind::Call &&
        endpointProducer->hirValue == parkedRangeEndpoint->parking.endValue) {
      parkedRangeEndpoint->parking.endCall = producerInstruction;
      parkedRangeEndpoint->parking.failureRestores.push_back(
          appendInstruction({.kind = MirInstructionKind::RestoreRangeCursor,
                             .hirValue = parkedRangeEndpoint->parking.endValue,
                             .hirStatement = parkedRangeEndpoint->statement,
                             .loan = parkedRangeEndpoint->parking.cursor},
                            false));
    }
    if (shareFailureCleanup()) {
      planningFailureCleanup = plannedFailureCleanup.size();
      plannedFailureCleanup.push_back(
          {.record = failureRecord, .parameterBlock = failureBlock});
    }
    std::vector<MirLoanId> failureEndedLoans = endedLoans;
    // A failed producer publishes no owned carrier. Its provisional loan must
    // retire before the ancestor/resource drain, even when final semantic
    // metadata already names the binding that would receive its success.
    const auto *failedProducerBlock = output.findBlock(producerBlock);
    const auto *failedProducer =
        failedProducerBlock && !failedProducerBlock->instructions.empty()
            ? &failedProducerBlock->instructions.back()
            : nullptr;
    const auto *failedLoan = failedProducer && failedProducer->loan
                                 ? output.findLoan(*failedProducer->loan)
                                 : nullptr;
    if (failedLoan && failedProducer->kind == MirInstructionKind::Borrow &&
        !containsLoan(failureEndedLoans, failedLoan->id)) {
      // Checked place formation publishes no reference on failure. Retire its
      // provisional verifier capability before draining receiver ancestors or
      // callee locals; the runtime pointer is never read on this edge.
      (void)appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = failedLoan->id});
      failureEndedLoans.push_back(failedLoan->id);
    }
    if (failedLoan && failedLoan->lifecycleBound && !failedLoan->entry &&
        !failedLoan->escapes && failedProducer->result &&
        failedLoan->producedBy == failedProducer->hirValue &&
        !containsLoan(failureEndedLoans, failedLoan->id)) {
      (void)appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = failedLoan->id});
      failureEndedLoans.push_back(failedLoan->id);
    }
    // A fresh constructor argument has already transferred its owning slot to
    // the callee before this Invoke. On failure the callee has drained that
    // parameter, so its caller-local dependency must end before any external
    // parent is retired. It has no remaining temporary drop to trigger the
    // ordinary lifecycle endpoint. Borrowed/copied arguments do not transfer
    // this capability and must retain their existing cleanup schedule.
    std::vector<MirLoanId> consumedConstructedLoans;
    if (failedProducer &&
        failedProducer->kind == MirInstructionKind::Construct) {
      for (const auto &operand : failedProducer->operands) {
        const auto *stage =
            operand.kind == MirOperandKind::Value
                ? mir_detail::definitionFor(output, operand.value)
                : nullptr;
        if (!stage || stage->kind != MirInstructionKind::CallInput ||
            stage->callSite != failedProducer->callSite ||
            stage->callInputRole != MirCallInputRole::Argument ||
            stage->callInputKind != HirCallInputKind::MoveValue ||
            !stage->preparedParameterDrop || stage->operands.size() != 1 ||
            stage->operands.front().kind != MirOperandKind::Value ||
            !std::any_of(
                failedProducer->lifecycle.begin(),
                failedProducer->lifecycle.end(), [&](const auto &event) {
                  return event.kind == MirLifecycleEventKind::TransferOut &&
                         event.source == *stage->preparedParameterDrop;
                }))
          continue;
        const auto *argument =
            mir_detail::definitionFor(output, stage->operands.front().value);
        const auto *dependency = argument && argument->loan
                                     ? output.findLoan(*argument->loan)
                                     : nullptr;
        if (argument && argument->kind == MirInstructionKind::Construct &&
            dependency && dependency->kind == MirLoanKind::Stored &&
            dependency->lifecycleBound && !dependency->entry &&
            !dependency->escapes &&
            dependency->producedBy == argument->hirValue &&
            loanIsActive(scopes, dependency->id))
          consumedConstructedLoans.push_back(dependency->id);
      }
    }
    for (const auto loan : childFirstMirLoans(consumedConstructedLoans)) {
      if (!containsLoan(failureEndedLoans, loan)) {
        (void)appendInstruction(
            {.kind = MirInstructionKind::EndBorrow, .loan = loan});
        failureEndedLoans.push_back(loan);
      }
    }
    // Return-expression evaluation may already have formed its exported loan
    // when a later local destructor fails. No reference is published on that
    // edge, so retire a non-owning return capability before draining locals.
    // An owning cleanup-bound result instead retains its dependency until the
    // pending result itself has been destroyed on that failure edge.
    const auto *returnDependency = output.findLoan(pendingReturnLoan);
    if (pendingReturnLoan != 0 &&
        (returnDependency == nullptr || !returnDependency->lifecycleBound) &&
        !containsLoan(failureEndedLoans, pendingReturnLoan)) {
      (void)appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = pendingReturnLoan});
      failureEndedLoans.push_back(pendingReturnLoan);
    }
    for (const MirLoanId loan : producerEndedLoans) {
      if (containsLoan(failureEndedLoans, loan)) {
        continue;
      }
      (void)appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = loan});
      failureEndedLoans.push_back(loan);
    }
    const bool hasLifecycleLoans =
        std::any_of(scopes.begin(), scopes.end(), [&](const Scope &scope) {
          return std::any_of(scope.loans.begin(), scope.loans.end(),
                             [&](MirLoanId id) {
                               const MirLoan *loan = output.findLoan(id);
                               return loan != nullptr && loan->lifecycleBound;
                             });
        });
    std::vector<MirLoanId> deferredConstructorLoans;
    if (output.kind == MirBodyKind::Constructor ||
        output.kind == MirBodyKind::FieldInitializers || !hasLifecycleLoans) {
      emitOrderedFailureDrain(consumedDrops, failureEndedLoans,
                              deferredConstructorLoans);
    } else {
      const auto retained = retainedReborrowAncestors();
      for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        for (auto loan = scope->loans.rbegin(); loan != scope->loans.rend();
             ++loan) {
          const MirLoan *record = output.findLoan(*loan);
          if (record == nullptr) {
            valid = false;
            continue;
          }
          // A transient returned view/reference may still borrow an active
          // full-expression temporary when a later call fails. Retire that
          // capability before dropping its owner, in child-before-parent order.
          // Carried/lifecycle loans and ancestors retained by them still end
          // through their owning scope's ordinary cleanup schedule.
          const HirValue *produced = findValue(record->producedBy);
          const bool transientView =
              record->kind == MirLoanKind::Stored && produced != nullptr &&
              produced->info.type.kind == SemanticType::StringView;
          const bool transientResult =
              (record->kind == MirLoanKind::CallResult || transientView) &&
              !record->lifecycleBound && record->carriers.empty() &&
              record->storedField == 0 && !record->escapes &&
              !retained.contains(record->id);
          if ((record->kind != MirLoanKind::CallArgument && !transientResult) ||
              containsLoan(failureEndedLoans, *loan)) {
            continue;
          }
          (void)appendInstruction(
              {.kind = MirInstructionKind::EndBorrow, .loan = *loan});
          failureEndedLoans.push_back(*loan);
        }
      }
      auto failureTemporaries = temporaryDrops;
      if (pendingReturnDrop != 0) {
        failureTemporaries.push_back({.obligation = pendingReturnDrop});
      }
      emitFailureTemporaryCleanup(failureTemporaries, consumedDrops,
                                  failureEndedLoans);
      for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        emitFailureScopeCleanup(*scope, consumedDrops, failureEndedLoans);
      }
    }
    // A constructor publishes stored references into its result, not into a
    // normal local scope. Successful completion exports those dependencies;
    // failed construction exports nothing. Keep them through subobject
    // rollback, then retire every published capability before propagation.
    auto constructorFailureLoans = constructorPublishedLoans;
    constructorFailureLoans.insert(constructorFailureLoans.end(),
                                   deferredConstructorLoans.begin(),
                                   deferredConstructorLoans.end());
    for (const auto loan : childFirstMirLoans(constructorFailureLoans)) {
      if (!containsLoan(failureEndedLoans, loan)) {
        (void)appendInstruction(
            {.kind = MirInstructionKind::EndBorrow, .loan = loan});
      }
    }
    if (PlannedFailureCleanup *plan = planningCleanup()) {
      plan->residualLoans = residualCleanupLoans(*plan, failureEndedLoans);
      planningFailureCleanup.reset();
    } else {
      terminate({.kind = MirTerminatorKind::PropagateFailure,
                 .failureRecord = failureRecord});
    }
    current = normalBlock;
  }

  // A constructor failure edge drains live temporaries, scope bindings, and
  // armed ConstructionRollback obligations as one globally reverse
  // construction-ordered sequence behind a single failure boundary. Rollback
  // obligations are armed mid-prologue, so they interleave between the
  // parameter bindings seeded before them and the temporaries created after
  // them; a phase-ordered drain cannot satisfy the primary-chain global
  // ordering rule. Owning ordinary bodies without lifecycle-bound loans need
  // the same ordering when a lexical result is initialized before cleanup of
  // its initializer's older temporaries. Borrowed carriers retain their
  // established scope/loan schedule above.
  void
  emitOrderedFailureDrain(const std::vector<MirDropObligationId> &consumedDrops,
                          const std::vector<MirLoanId> &endedLoans,
                          std::vector<MirLoanId> &deferredConstructorLoans) {
    const auto retained = temporaryCarrierLoans(consumedDrops);
    auto allEnded = endedLoans;
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      for (auto loan = scope->loans.rbegin(); loan != scope->loans.rend();
           ++loan) {
        const MirLoan *record = output.findLoan(*loan);
        // A later initializer can publish an entry reference into a field,
        // changing its final loan record after this failure edge was built.
        // Keep constructor entry capabilities through rollback on every edge,
        // then retire them child-first with the published dependencies.
        if (output.kind == MirBodyKind::Constructor && record &&
            record->entry && !record->lifecycleBound &&
            !containsLoan(allEnded, *loan)) {
          deferredConstructorLoans.push_back(*loan);
          continue;
        }
        if (!containsLoan(allEnded, *loan) && !retained.contains(*loan) &&
            !containsLoan(constructorPublishedLoans, *loan) &&
            (record == nullptr || !record->lifecycleBound)) {
          (void)appendInstruction(
              {.kind = MirInstructionKind::EndBorrow, .loan = *loan});
          allEnded.push_back(*loan);
        }
      }
    }

    struct DrainEntry {
      MirDropObligationId obligation = 0;
      bool conditional = false;
    };
    std::vector<DrainEntry> entries;
    for (const TemporaryDrop &temporary : temporaryDrops) {
      if (!containsDrop(consumedDrops, temporary.obligation)) {
        entries.push_back({temporary.obligation, temporary.conditional});
      }
    }
    if (pendingReturnDrop != 0 &&
        !containsDrop(consumedDrops, pendingReturnDrop)) {
      entries.push_back({pendingReturnDrop, false});
    }
    for (const Scope &scope : scopes) {
      for (const MirPlaceId placeId : scope.drops) {
        const MirPlace *place = output.findPlace(placeId);
        const MirDropObligationId obligation =
            dropObligationForLocalPlace(place);
        if (obligation == 0) {
          valid = false;
          continue;
        }
        if (!containsDrop(consumedDrops, obligation)) {
          entries.push_back({obligation, false});
        }
      }
    }
    for (const MirDropObligationId armed : constructorRollback) {
      entries.push_back({armed, false});
    }

    std::sort(entries.begin(), entries.end(),
              [&](const DrainEntry &left, const DrainEntry &right) {
                const MirDropObligation *first =
                    output.findDropObligation(left.obligation);
                const MirDropObligation *second =
                    output.findDropObligation(right.obligation);
                return (first == nullptr ? 0 : first->constructionOrder) >
                       (second == nullptr ? 0 : second->constructionOrder);
              });

    std::vector<MirDropObligationId> cleanup;
    auto retired = consumedDrops;
    cleanup.reserve(entries.size());
    for (const DrainEntry &entry : entries) {
      const MirDropObligation *obligation =
          output.findDropObligation(entry.obligation);
      const MirPlace *place =
          obligation == nullptr ? nullptr : output.findPlace(obligation->place);
      if (obligation == nullptr || place == nullptr) {
        valid = false;
        continue;
      }
      const MirLoanId lifecycleLoan = lifecycleLoanForPlace(*place);
      (void)appendFailureCleanupDrop(
          entry.obligation, obligation->place,
          ExpressionInfo{.type = place->type,
                         .category = ValueCategory::Place,
                         .access = place->access,
                         .traits = place->traits},
          entry.conditional);
      endPendingReturnLoanAfterDrop(entry.obligation, allEnded);
      if (lifecycleLoan != 0 && !containsLoan(endedLoans, lifecycleLoan)) {
        (void)appendInstruction(
            {.kind = MirInstructionKind::EndBorrow, .loan = lifecycleLoan});
      }
      cleanup.push_back(entry.obligation);
      retired.push_back(entry.obligation);
      for (const auto loan : retiringTemporaryCarrierLoans(
               entry.obligation, retired, allEnded, true)) {
        if (containsLoan(deferredConstructorLoans, loan))
          continue;
        (void)appendInstruction(
            {.kind = MirInstructionKind::EndBorrow, .loan = loan});
        allEnded.push_back(loan);
      }
    }
    appendFailureCleanupBoundary(std::move(cleanup));
  }

  // Normal constructor completion transfers every armed subobject to the
  // caller: the constructed object owns them from here, so no rollback
  // obligation stays active across a normal exit edge.
  void retireConstructorRollback() {
    if ((output.kind != MirBodyKind::Constructor &&
         output.kind != MirBodyKind::FieldInitializers) ||
        constructorRollback.empty()) {
      return;
    }
    MirInstruction release{.kind = MirInstructionKind::Lifecycle};
    for (const MirDropObligationId armed : constructorRollback) {
      release.lifecycle.push_back(
          {.kind = MirLifecycleEventKind::TransferOut, .source = armed});
    }
    (void)appendInstruction(std::move(release));
    constructorRollback.clear();
  }

  // `propagation` is given when the drop was planned ahead of its emission
  // (shared exits); otherwise it is decided here, at the drop's own block.
  // A drop never names a constructor-initializer stage: only the stage's one
  // Initialize/InitializeBase publication does, even when the drop ends a
  // temporary of that initializer's full expression.
  MirInstructionId appendNormalDrop(
      MirDropObligationId obligation, MirPlaceId destination,
      ExpressionInfo info, std::vector<MirLifecycleEvent> lifecycle,
      const std::vector<MirDropObligationId> &consumedDrops,
      const std::vector<MirLoanId> &endedLoans, HirValueId hirValue = 0,
      HirStatementId hirStatement = 0, MirLoanId producerEndedLoan = 0,
      std::vector<MirLoanId> producerEndedLoans = {},
      std::optional<FailurePropagationKind> propagation = std::nullopt) {
    MirInstruction drop{.kind = MirInstructionKind::Drop,
                        .hirValue = hirValue,
                        .hirStatement = hirStatement,
                        .destination = destination,
                        .info = std::move(info),
                        .lifecycle = std::move(lifecycle)};
    drop.definedFailure.propagation =
        propagation ? *propagation
                    : normalizedDestructorPropagation(obligation);
    const bool mayRaise =
        drop.definedFailure.propagation == FailurePropagationKind::Destructor;
    const MirInstructionId instruction =
        appendInstruction(std::move(drop), true, false);
    if (instruction != 0 && routeFailureEdgesHere() && mayRaise) {
      if (producerEndedLoan != 0)
        producerEndedLoans.push_back(producerEndedLoan);
      appendFailureControlFlow(instruction, 0, consumedDrops, endedLoans,
                               producerEndedLoans);
    }
    return instruction;
  }

  [[nodiscard]] bool isFullExpressionRoot(HirValueId value) const {
    if (!fullExpressionRoots) {
      std::unordered_set<HirValueId> &roots = fullExpressionRoots.emplace();
      for (const HirFullExpression &expression : source.fullExpressions) {
        roots.insert(expression.roots.begin(), expression.roots.end());
      }
    }
    return value != 0 && fullExpressionRoots->contains(value);
  }

  [[nodiscard]] bool hasSingleArrayInitializerConsumer(HirValueId value) const {
    const HirValue *producer = findValue(value);
    const HirValue *consumer = nullptr;
    std::size_t uses = 0;
    for (const HirValue &candidate : source.values) {
      for (const HirValueId operand : candidate.operands) {
        if (operand != value) {
          continue;
        }
        ++uses;
        consumer = &candidate;
      }
    }
    return producer != nullptr && consumer != nullptr && uses == 1 &&
           consumer->kind == HirValueKind::ArrayInitializer &&
           consumer->info.type.kind == SemanticType::Array &&
           consumer->info.type.arguments.size() == 1 &&
           consumer->info.type.arrayLengthParameterId == 0 &&
           consumer->info.type.arrayLength == consumer->operands.size() &&
           consumer->info.type.arguments.front() == producer->info.type &&
           producer->fullExpression != 0 &&
           consumer->fullExpression == producer->fullExpression &&
           isFullExpressionRoot(consumer->id);
  }

  MirInstructionId appendInstruction(MirInstruction instruction,
                                     bool inheritSourceMetadata = true,
                                     bool routeFailureControlFlow = true) {
    if (PlannedFailureCleanup *plan = planningCleanup()) {
      // While a failure edge's cleanup is recorded only loan ends arrive
      // here; drops and boundary markers have their own recorders.
      if (instruction.kind != MirInstructionKind::EndBorrow ||
          !instruction.loan) {
        valid = false;
        return 0;
      }
      // Program verification justifies ending a lifecycle-bound loan by the
      // drop or failed producer before it on the same path, so such an end
      // and everything before it stay with their own failure edge.
      const MirLoan *loan = output.findLoan(*instruction.loan);
      plan->steps.push_back(
          {.kind = PlannedCleanupStep::Kind::EndBorrow,
           .loan = *instruction.loan,
           .contextFree = loan == nullptr || !loan->lifecycleBound});
      return 0;
    }
    MirBlock *block = currentBlock();
    if (block == nullptr || terminated()) {
      return 0;
    }
    if (inheritSourceMetadata && instruction.hirValue != 0) {
      if (const HirValue *source = findValue(instruction.hirValue)) {
        const bool neutralValueStage =
            instruction.kind == MirInstructionKind::CallInput &&
            instruction.callInputKind == HirCallInputKind::Value &&
            instruction.result && !instruction.destination &&
            !instruction.receiver && !instruction.loan &&
            instruction.intrinsic == IntrinsicKind::None &&
            instruction.operation == MirOperation::None &&
            instruction.operands.size() == 1 &&
            instruction.operands.front().kind == MirOperandKind::Value &&
            instruction.operands.front().value != 0 &&
            instruction.operands.front().place == 0 &&
            instruction.operands.front().loan == 0 &&
            instruction.operands.front().type == instruction.info.type &&
            instruction.lifecycle.empty() && !instruction.functionTarget &&
            !instruction.constructorTarget && !instruction.bodyTarget &&
            !instruction.nativeCallbackAdapter &&
            !instruction.threadTaskAdapter &&
            !instruction.preparedParameterDrop &&
            !instruction.retainedOwnerPlace && !instruction.successResultDrop &&
            !instruction.successResultDestination &&
            instruction.synchronization.kind ==
                SynchronizationOperationKind::None;
        // Object lifetime manipulation is one explicit call, not an assertion
        // by its surrounding full-expression/lifecycle bookkeeping markers.
        // A neutral Value stage likewise forwards an already computed SSA
        // value. The source Load/Compute/Call retains its unsafe operation;
        // preparation must not claim to execute that operation a second time.
        // Explicit stage annotations and all owning/borrowing stages remain.
        if (source->unsafeOperation != UnsafeOperationKind::None &&
            !neutralValueStage &&
            (source->unsafeOperation != UnsafeOperationKind::ObjectLifetime ||
             instruction.kind == MirInstructionKind::Call)) {
          instruction.unsafeOperation = source->unsafeOperation;
        }
        if (source->ownership) {
          instruction.ownership = source->ownership;
        }
      }
    }
    if (instruction.kind != MirInstructionKind::EnsureThreadLocal) {
      bool movedThreadLocalDetector = false;
      for (auto &origin : instruction.definedFailure.localOrigins) {
        const auto removed =
            std::erase_if(origin.outcomes, threadLocalGuardOutcome);
        movedThreadLocalDetector |= removed != 0;
      }
      std::erase_if(instruction.definedFailure.localOrigins,
                    [](const auto &origin) { return origin.outcomes.empty(); });
      if (movedThreadLocalDetector && instruction.definedFailure.propagation ==
                                          FailurePropagationKind::BodyCall)
        instruction.definedFailure.propagation = FailurePropagationKind::None;
    }
    instruction.localFailureSites.clear();
    if (instruction.callerLocation) {
      const auto site =
          failureMetadata.callerSiteFor(*instruction.callerLocation);
      if (!site) {
        valid = false;
      } else {
        instruction.callerLocationSite = *site;
      }
    }
    instruction.localFailureSites.reserve(
        instruction.definedFailure.localOrigins.size());
    for (const DefinedFailureOrigin &origin :
         instruction.definedFailure.localOrigins) {
      const std::optional<FailureSiteId> site = failureMetadata.siteFor(origin);
      if (!site) {
        valid = false;
        continue;
      }
      instruction.localFailureSites.push_back(*site);
    }
    const auto rawPlace = [&](MirPlaceId id) {
      const MirPlace *place = output.findPlace(id);
      return place != nullptr &&
             (place->foreignStorage ||
              std::any_of(place->projections.begin(), place->projections.end(),
                          [](const MirPlaceProjection &projection) {
                            return projection.kind ==
                                       MirProjectionKind::RawDereference ||
                                   projection.kind ==
                                       MirProjectionKind::RawIndex;
                          }));
    };
    const auto rawReceiver = [&]() {
      if (!instruction.receiver)
        return false;
      if (instruction.receiver->place != 0)
        return rawPlace(instruction.receiver->place);
      if (instruction.kind != MirInstructionKind::Call ||
          instruction.receiver->kind != MirOperandKind::Value)
        return false;
      const MirValue *value = output.findValue(instruction.receiver->value);
      const MirBlock *block =
          value == nullptr ? nullptr : output.findBlock(value->definitionBlock);
      if (block == nullptr)
        return false;
      for (const auto &stage : block->instructions)
        if (stage.id == value->definition &&
            stage.kind == MirInstructionKind::CallInput &&
            stage.callSite == instruction.callSite &&
            stage.callInputRole == MirCallInputRole::Receiver &&
            stage.operands.size() == 1)
          return rawPlace(stage.operands.front().place);
      return false;
    };
    instruction.rawMemoryAccess =
        (instruction.destination && rawPlace(*instruction.destination)) ||
        rawReceiver() ||
        std::any_of(instruction.operands.begin(), instruction.operands.end(),
                    [&](const MirOperand &operand) {
                      return operand.kind != MirOperandKind::Address &&
                             operand.place != 0 && rawPlace(operand.place);
                    });
    // Expanded checked updates retain the foreign load obligation even when
    // the read is synthesized separately from its source modify expression.
    if (instruction.kind == MirInstructionKind::Load &&
        instruction.unsafeOperation == UnsafeOperationKind::None &&
        std::any_of(instruction.operands.begin(), instruction.operands.end(),
                    [&](const MirOperand &operand) {
                      const auto *place = output.findPlace(operand.place);
                      return operand.kind != MirOperandKind::Address && place &&
                             place->foreignStorage;
                    }))
      instruction.unsafeOperation = UnsafeOperationKind::ForeignStorage;
    // An outer field selection has no new unsafe syntax of its own, but a
    // store through its raw prefix still executes that checked source access.
    // Preserve its operation marker alongside the raw-memory effect rather
    // than losing it on `pointer->inner.field` or `pointer[index].field`.
    if ((instruction.kind == MirInstructionKind::Assign ||
         instruction.kind == MirInstructionKind::Modify) &&
        instruction.destination && rawPlace(*instruction.destination) &&
        instruction.unsafeOperation == UnsafeOperationKind::None) {
      const auto *destination = output.findPlace(*instruction.destination);
      const auto raw = std::find_if(
          destination->projections.begin(), destination->projections.end(),
          [](const MirPlaceProjection &projection) {
            return projection.kind == MirProjectionKind::RawDereference ||
                   projection.kind == MirProjectionKind::RawIndex;
          });
      instruction.unsafeOperation =
          raw == destination->projections.end()
              ? UnsafeOperationKind::ForeignStorage
          : raw->kind == MirProjectionKind::RawIndex
              ? UnsafeOperationKind::RawIndex
              : (std::next(raw) == destination->projections.end()
                     ? UnsafeOperationKind::RawDereference
                     : UnsafeOperationKind::RawMember);
    }
    instruction.id = nextInstruction++;
    const MirInstructionId id = instruction.id;
    if (instruction.result) {
      if (*instruction.result == 0 ||
          *instruction.result > output.values.size()) {
        valid = false;
      } else {
        MirValue &result = output.values[*instruction.result - 1];
        if (result.definition != 0) {
          valid = false;
        } else {
          result.definitionBlock = block->id;
          result.definition = id;
        }
      }
    }
    block->instructions.push_back(std::move(instruction));
    const MirInstruction &appended = block->instructions.back();
    const bool neverReturns = appended.neverReturns;
    if (routeFailureControlFlow && routeFailureEdgesHere() &&
        requiresMirFailureControlFlow(appended)) {
      const MirDropObligationId successResultDrop =
          appended.successResultDrop.value_or(0);
      appendFailureControlFlow(id, successResultDrop);
      registerTemporary(successResultDrop);
    }
    if (neverReturns && !terminated()) {
      terminate({.kind = MirTerminatorKind::Unreachable});
    }
    return id;
  }

  [[nodiscard]] bool
  temporaryConditional(MirDropObligationId obligation) const {
    const auto found = temporaryDrop(obligation);
    return found != temporaryDrops.end() && found->conditional;
  }

  void transferTemporaryOut(MirInstruction &instruction,
                            HirValueId sourceValue) {
    const MirDropObligationId sourceObligation =
        dropObligationForValue(sourceValue);
    if (!temporaryIsActive(sourceObligation)) {
      return;
    }
    appendLifecycle(instruction, {.kind = MirLifecycleEventKind::TransferOut,
                                  .source = sourceObligation});
    (void)removeTemporary(sourceObligation);
    if ((output.kind == MirBodyKind::Constructor ||
         output.kind == MirBodyKind::FieldInitializers) &&
        instruction.kind == MirInstructionKind::Lifecycle) {
      constructorUnarmedTransfer = true;
    }
  }

  [[nodiscard]] bool routeFailureEdgesHere() const {
    return supportsMirFailureControlFlow(output.kind) &&
           ((output.kind != MirBodyKind::Constructor &&
             output.kind != MirBodyKind::FieldInitializers) ||
            !constructorUnarmedTransfer);
  }

  [[nodiscard]] MirDropObligationId
  initializeValueLifecycle(MirInstruction &instruction, const HirValue &value,
                           MirPlaceId place = 0) {
    const MirDropObligationId target = dropObligationForValue(value.id, place);
    if (target == 0) {
      return 0;
    }

    MirDropObligationId sourceObligation = 0;
    MirLifecycleEventKind transition = MirLifecycleEventKind::Initialize;
    bool sourceRemainsTemporary = false;
    if (value.kind == HirValueKind::Move && !value.operands.empty() &&
        temporarySlot(value.operands.front()) != 0) {
      // Moving a materialized temporary hands its value to the result. As for
      // a moved binding, the moved slot's obligation stays scheduled, and its
      // full-expression Drop retires it.
      sourceObligation = dropObligationForValue(value.operands.front());
      transition = MirLifecycleEventKind::Move;
      sourceRemainsTemporary = true;
    } else if (value.kind == HirValueKind::Move && !value.operands.empty()) {
      sourceObligation = exactBindingDropObligation(value.operands.front());
      transition = MirLifecycleEventKind::Move;
      sourceRemainsTemporary = temporaryIsActive(sourceObligation);
    } else if ((value.kind == HirValueKind::Grouping ||
                value.kind == HirValueKind::Conversion ||
                (value.kind == HirValueKind::Binary &&
                 value.operation == TokenKind::COMMA)) &&
               !value.operands.empty()) {
      sourceObligation = dropObligationForValue(value.operands.back());
      if (temporaryIsActive(sourceObligation)) {
        transition = MirLifecycleEventKind::Reparent;
      } else {
        sourceObligation = 0;
      }
    }

    bool conditional = false;
    if (sourceObligation != 0) {
      conditional = temporaryConditional(sourceObligation);
      if (transition == MirLifecycleEventKind::Reparent) {
        appendReparentOrTypedTransfer(instruction, sourceObligation, target);
      } else {
        appendLifecycle(
            instruction,
            {.kind = transition, .source = sourceObligation, .target = target});
      }
      if (!sourceRemainsTemporary) {
        (void)removeTemporary(sourceObligation);
      }
    } else {
      appendLifecycle(instruction, {.kind = MirLifecycleEventKind::Initialize,
                                    .target = target});
    }
    registerTemporary(target, conditional);
    return target;
  }

  [[nodiscard]] HirFullExpressionId
  publishFullExpression(const HirFullExpression &expression) {
    if (expression.id == 0 || expression.roots.empty()) {
      valid = false;
      return 0;
    }
    if (const auto found = fullExpressionIds.find(expression.id);
        found != fullExpressionIds.end()) {
      return found->second;
    }
    const HirFullExpressionId id = output.fullExpressions.size() + 1;
    output.fullExpressions.push_back(
        {.id = id,
         .hirExpression = expression.id,
         .statement = expression.statement,
         .constructorInitializer = expression.constructorInitializer,
         .roots = expression.roots});
    fullExpressionIds.emplace(expression.id, id);
    for (MirDropObligation &obligation : output.dropObligations) {
      if (obligation.hirFullExpression == expression.id) {
        obligation.fullExpression = id;
      }
    }
    return id;
  }

  void emitTemporaryDrops(const std::vector<TemporaryDrop> &baseline,
                          HirValueId hirValue = 0,
                          HirStatementId hirStatement = 0,
                          std::size_t constructorInitializer = 0,
                          bool earlyExit = false) {
    std::vector<MirDropObligationId> consumedDrops;
    for (auto candidate = temporaryDrops.rbegin();
         candidate != temporaryDrops.rend(); ++candidate) {
      if (std::any_of(baseline.begin(), baseline.end(),
                      [&](const TemporaryDrop &existing) {
                        return existing.obligation == candidate->obligation;
                      })) {
        continue;
      }
      const MirDropObligation *obligation =
          output.findDropObligation(candidate->obligation);
      if (obligation == nullptr || obligation->place == 0) {
        valid = false;
        continue;
      }
      const MirPlace *place = output.findPlace(obligation->place);
      if (place == nullptr) {
        valid = false;
        continue;
      }
      consumedDrops.push_back(candidate->obligation);
      const auto endedLoans =
          retiringTemporaryCarrierLoans(candidate->obligation, consumedDrops);
      (void)appendNormalDrop(candidate->obligation, obligation->place,
                             ExpressionInfo{.type = obligation->dropType.type,
                                            .category = ValueCategory::Place,
                                            .access = AccessMode::Mutable,
                                            .traits = place->traits},
                             {{.kind = MirLifecycleEventKind::Drop,
                               .source = candidate->obligation,
                               .conditional = candidate->conditional}},
                             consumedDrops, {}, hirValue, hirStatement, 0,
                             endedLoans);
      for (const auto loan : endedLoans) {
        (void)appendInstruction(
            {.kind = MirInstructionKind::EndBorrow, .loan = loan});
        removeLoanFromState(loan, scopes, bindingLoans);
      }
    }
    endUncarriedLifecycleLoans();
    temporaryDrops = baseline;
    const auto boundary = std::find_if(
        source.fullExpressions.begin(), source.fullExpressions.end(),
        [&](const HirFullExpression &expression) {
          if (constructorInitializer != 0) {
            return expression.constructorInitializer == constructorInitializer;
          }
          if (earlyExit) {
            const HirValue *exit = findValue(hirValue);
            return exit != nullptr && exit->fullExpression != 0 &&
                   expression.id == exit->fullExpression;
          }
          return hirStatement != 0 && expression.statement == hirStatement &&
                 hirValue != 0 &&
                 std::find(expression.roots.begin(), expression.roots.end(),
                           hirValue) != expression.roots.end();
        });
    if (boundary == source.fullExpressions.end()) {
      return;
    }
    const bool ambiguous = std::any_of(
        std::next(boundary), source.fullExpressions.end(),
        [&](const HirFullExpression &expression) {
          if (constructorInitializer != 0) {
            return expression.constructorInitializer == constructorInitializer;
          }
          if (earlyExit) {
            const HirValue *exit = findValue(hirValue);
            return exit != nullptr && exit->fullExpression != 0 &&
                   expression.id == exit->fullExpression;
          }
          return expression.statement == hirStatement &&
                 std::find(expression.roots.begin(), expression.roots.end(),
                           hirValue) != expression.roots.end();
        });
    if (ambiguous) {
      valid = false;
      return;
    }
    const HirFullExpressionId boundaryId = publishFullExpression(*boundary);
    if (boundaryId == 0) {
      return;
    }
    if (earlyExit && boundary->roots.empty()) {
      valid = false;
      return;
    }
    (void)appendInstruction(
        {.kind = MirInstructionKind::Lifecycle,
         .hirValue = earlyExit ? boundary->roots.front() : hirValue,
         .hirStatement = earlyExit ? boundary->statement : hirStatement,
         .fullExpressionEnd = boundaryId,
         .earlyExitFullExpressionEnd = earlyExit},
        output.kind != MirBodyKind::Module ||
            currentProgramInitializationStep == 0);
    if (!earlyExit) {
      fullExpressionTemporaryBaselines.erase(boundary->id);
      fullExpressionScopeBaselines.erase(boundary->id);
    }
  }

  void emitTemporaryTransfer(HirValueId value, HirStatementId statement = 0) {
    MirInstruction transfer{.kind = MirInstructionKind::Lifecycle,
                            .hirValue = value,
                            .hirStatement = statement};
    transferTemporaryOut(transfer, value);
    if (!transfer.lifecycle.empty()) {
      (void)appendInstruction(std::move(transfer));
    }
  }

  [[nodiscard]] std::optional<MirLoanDependency>
  lowerCompleteBorrowDependency(const SemanticLoanDependency &dependency) {
    if (!dependency.place.valid())
      return std::nullopt;
    MirLoanId parent = 0;
    MirPlaceId selected = 0;
    bool ambiguous = false;
    const auto consider = [&](MirPlaceId candidate, AccessMode authority) {
      const auto *place = output.findPlace(candidate);
      if (!place || place->key != dependency.place ||
          (dependency.access == AccessMode::Mutable &&
           authority != AccessMode::Mutable))
        return;
      if (selected != 0 && !sameBorrowSource(selected, candidate))
        ambiguous = true;
      else
        selected = candidate;
    };
    if (dependency.parent != 0) {
      const auto mapped = semanticLoans.find(dependency.parent);
      const auto *semantic = source.findLoan(dependency.parent);
      if (mapped == semanticLoans.end() || !semantic)
        return std::nullopt;
      parent = mapped->second;
      const auto *loan = output.findLoan(parent);
      if (!loan)
        return std::nullopt;
      if (loan->entry && loan->kind == MirLoanKind::Stored) {
        const auto base = loan->source;
        const auto project = [&](const auto &fields, const auto &conditions,
                                 AccessMode access) {
          auto external = semantic->place;
          for (const auto field : fields)
            external.projections.push_back(
                {.kind = PlaceProjectionKind::Field, .field = field});
          if (external == dependency.place)
            consider(projectBorrowFields(base, fields, conditions), access);
        };
        project(semantic->designatedFields, semantic->conditions,
                semantic->access);
        for (const auto &extra : semantic->additionalDependencies)
          project(extra.designatedFields, extra.conditions, extra.access);
      } else {
        consider(loan->source, loan->access);
        for (const auto &extra : loan->additionalDependencies)
          consider(extra.source, extra.access);
        // A reference formal's entry loan protects its whole referent; a
        // sub-place of that referent is named by its own evaluated place.
        if (selected == 0 && loan->entry &&
            loan->kind == MirLoanKind::Parameter)
          for (const auto &place : output.places)
            if (place.root == MirPlaceRootKind::Binding)
              consider(place.id, place.access);
      }
    } else {
      // A direct physical owner can be materialized from its existing typed
      // binding and field path. Dynamic projections must already have their
      // evaluated MIR place; a semantic overlap key is not an index value.
      if (const auto binding = localSymbols.find(dependency.place.root);
          !dependency.place.receiver && binding != localSymbols.end()) {
        const auto base = placeForBinding(binding->second);
        std::vector<SymbolId> fields;
        bool fieldPath = true;
        for (const auto &projection : dependency.place.projections) {
          if (projection.kind != PlaceProjectionKind::Field) {
            fieldPath = false;
            break;
          }
          fields.push_back(projection.field);
        }
        if (fieldPath) {
          const auto projected = projectBorrowFields(base, fields);
          if (const auto *place = output.findPlace(projected))
            consider(projected, place->access);
        }
      }
      if (selected == 0)
        for (const auto &place : output.places)
          if (place.root != MirPlaceRootKind::Value &&
              place.root != MirPlaceRootKind::Temporary)
            consider(place.id, place.access);
      // A complete dependency on global or static storage names that exact
      // program storage, which this body need not have mentioned itself.
      if (selected == 0 && !dependency.place.receiver)
        if (const MirPlaceId global = globalStoragePlace(
                {.root = dependency.place.root,
                 .projections = dependency.place.projections},
                dependency.place.domain))
          consider(global, output.places[global - 1].access);
    }
    if (selected == 0 || ambiguous)
      return std::nullopt;
    return MirLoanDependency{.source = selected,
                             .parent = parent,
                             .designatedFields = dependency.designatedFields,
                             .access = dependency.access,
                             .conditions = dependency.conditions};
  }

  MirValueId appendValue(HirValueId sourceValue, ExpressionInfo info) {
    const MirValueId id = output.values.size() + 1;
    output.values.push_back(
        {.id = id, .sourceValue = sourceValue, .info = std::move(info)});
    if (const auto *source = findValue(sourceValue);
        source && source->info.type == output.values.back().info.type &&
        source->borrowDependencies &&
        !(source->info.category == ValueCategory::Place &&
          output.values.back().info.category == ValueCategory::Value &&
          !output.values.back().info.traits.containsBorrowedState &&
          source->borrowDependencies->possibleAlternatives.empty() &&
          std::all_of(source->borrowDependencies->dependencies.begin(),
                      source->borrowDependencies->dependencies.end(),
                      [](const auto &dependency) {
                        return dependency.designatedFields.empty() &&
                               dependency.conditions.empty();
                      }))) {
      // Transport source-established complete sets, including known-empty
      // alternatives. MIR independently reconstructs their value/storage flow;
      // absence is never upgraded to an empty proof by lowering.
      // A load from a borrowed scalar place reads through its address loan;
      // the resulting dependency-free scalar does not retain that address.
      // Stored-field dependencies and alternative domains still transport.
      MirBorrowDependencySet complete{
          .possibleAlternatives =
              source->borrowDependencies->possibleAlternatives};
      for (const auto &dependency : source->borrowDependencies->dependencies) {
        const auto lowered = lowerCompleteBorrowDependency(dependency);
        if (!lowered) {
          // A borrowing call's result is appended before its inputs name
          // their places; its loan publication attaches this same set.
          if ((source->functionTarget || source->constructorTarget) &&
              source->borrowOrigin != BorrowOriginKind::None)
            return id;
          valid = false;
          return id;
        }
        complete.dependencies.push_back(*lowered);
      }
      output.values[id - 1].borrowDependencies = std::move(complete);
    }
    return id;
  }

  [[nodiscard]] bool completeEmptyBorrowValue(HirValueId source,
                                              const MirOperand &operand,
                                              const SemanticType &type) const {
    const auto *value = findValue(source);
    const auto *lowered = operand.kind == MirOperandKind::Value
                              ? output.findValue(operand.value)
                              : nullptr;
    // No loan is needed only when both representations preserve the same
    // complete, typed dependency-free alternative. A missing loan or missing
    // dependency set is not evidence of emptiness; the program verifier still
    // independently reconstructs the producer and storage flow.
    return (type.kind == SemanticType::Enum ||
            type.kind == SemanticType::Class) &&
           value && value->info.type == type && value->borrowDependencies &&
           value->borrowDependencies->dependencies.empty() &&
           !value->borrowDependencies->possibleAlternatives.empty() &&
           lowered && lowered->info.type == type &&
           lowered->borrowDependencies &&
           lowered->borrowDependencies->dependencies.empty() &&
           lowered->borrowDependencies->possibleAlternatives ==
               value->borrowDependencies->possibleAlternatives;
  }

  [[nodiscard]] MirValueId mirValueFor(const HirValue &value) {
    if (const auto found = loweredValues.find(value.id);
        found != loweredValues.end()) {
      return found->second;
    }
    const MirValueId id = appendValue(value.id, value.info);
    loweredValues.emplace(value.id, id);
    return id;
  }

  [[nodiscard]] MirValueId mirValueFor(HirValueId id) {
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      valid = false;
      return 0;
    }
    return mirValueFor(*value);
  }

  [[nodiscard]] std::optional<MirValueId> resultFor(const HirValue &value) {
    return value.info.type.kind == SemanticType::Void
               ? std::nullopt
               : std::optional<MirValueId>{mirValueFor(value)};
  }

  MirPlaceId appendPlace(MirPlace place) {
    if (place.root == MirPlaceRootKind::Symbol && place.capture == 0 &&
        std::any_of(program.threadLocalStorage().begin(),
                    program.threadLocalStorage().end(),
                    [&](const auto &storage) {
                      return storage.symbol == place.symbol;
                    }))
      place.initiallyAvailable = true;
    if (place.root == MirPlaceRootKind::Symbol && place.capture == 0 &&
        program.findForeignStorage(place.symbol) != nullptr) {
      place.foreignStorage = true;
      place.initiallyAvailable = true;
    }
    place.id = output.places.size() + 1;
    const MirPlaceId id = place.id;
    output.places.push_back(std::move(place));
    return id;
  }

  MirPlaceId appendPackElementPlace(MirPlace projected) {
    for (const MirLoan &loan : output.loans) {
      const MirPlace *place = output.findPlace(loan.source);
      if (loan.entry && place != nullptr &&
          place->binding == projected.binding &&
          place->root == projected.root && place->type == projected.type &&
          place->projections == projected.projections) {
        return place->id;
      }
    }
    // Every access to an owning parameter element must use the same lifetime
    // place, including non-consuming folds and repeated copy expansions.
    for (const MirDropObligation &drop : output.dropObligations) {
      const MirPlace *place = output.findPlace(drop.place);
      if (drop.kind == MirDropObligationKind::Binding &&
          drop.binding == projected.binding && place != nullptr &&
          place->root == projected.root && place->type == projected.type &&
          place->projections == projected.projections)
        return place->id;
    }
    return appendPlace(std::move(projected));
  }

  void attachPlaceIdentity(MirPlaceId id, const HirValue &value) {
    MirPlace *place =
        id == 0 || id > output.places.size() ? nullptr : &output.places[id - 1];
    if (place == nullptr || !value.place) {
      return;
    }
    place->key = value.place;
    const auto semanticIndex = std::find_if(
        value.place->projections.rbegin(), value.place->projections.rend(),
        [](const PlaceProjection &projection) {
          return projection.kind == PlaceProjectionKind::ConstantIndex ||
                 projection.kind == PlaceProjectionKind::DynamicIndex;
        });
    const auto mirIndex =
        std::find_if(place->projections.rbegin(), place->projections.rend(),
                     [](const MirPlaceProjection &projection) {
                       return projection.kind == MirProjectionKind::Index;
                     });
    if (semanticIndex == value.place->projections.rend() ||
        mirIndex == place->projections.rend()) {
      return;
    }
    if (semanticIndex->kind == PlaceProjectionKind::ConstantIndex) {
      mirIndex->constantIndex = semanticIndex->index;
      mirIndex->selection = 0;
    } else {
      mirIndex->constantIndex.reset();
      mirIndex->selection = semanticIndex->selection;
    }
  }

  [[nodiscard]] MirPlaceId placeForBinding(HirBindingId id) {
    if (const auto found = bindingPlaces.find(id);
        found != bindingPlaces.end()) {
      return found->second;
    }
    const HirBinding *binding = findBinding(id);
    if (binding == nullptr) {
      return 0;
    }
    std::optional<PlaceKey> key;
    if (binding->info.symbol != 0) {
      key =
          PlaceKey{.domain = output.placeDomain, .root = binding->info.symbol};
    }
    const MirPlaceId place = appendPlace(
        {.root = binding->info.threadLocal ? MirPlaceRootKind::Symbol
                                           : MirPlaceRootKind::Binding,
         .binding = binding->info.threadLocal ? 0 : id,
         .symbol = binding->info.symbol,
         .type = binding->info.type,
         .access = binding->info.access,
         .traits = binding->info.traits,
         .key = std::move(key),
         .initiallyAvailable =
             binding->parameter != nullptr || binding->info.threadLocal});
    bindingPlaces.emplace(id, place);
    return place;
  }

  [[nodiscard]] MirPlaceId placeForLoan(const HirLoan &loan,
                                        const HirValue *resultValue = nullptr) {
    if (const auto found = semanticLoanPlaces.find(loan.semanticLoan);
        found != semanticLoanPlaces.end()) {
      return found->second;
    }

    if (loan.referenceSelection) {
      const auto authority = loan.parent != 0
                                 ? loan.parent
                                 : loan.referenceSelection->transferredFrom;
      const auto *entry = source.findLoan(authority);
      const auto mapped = semanticLoans.find(authority);
      if (entry && entry->entry &&
          entry->kind == SemanticLoanKind::StoredValue) {
        if (mapped == semanticLoans.end()) {
          valid = false;
          return 0;
        }
        const auto *lowered = output.findLoan(mapped->second);
        if (!lowered) {
          valid = false;
          return 0;
        }
        const auto base = lowered->source;
        const auto project = [&](const auto &fields, const auto &conditions) {
          auto external = entry->place;
          for (const auto field : fields)
            external.projections.push_back(
                {.kind = PlaceProjectionKind::Field, .field = field});
          if (external != loan.place)
            return MirPlaceId{0};
          return projectBorrowFields(base, fields, conditions);
        };
        auto exact = project(entry->designatedFields, entry->conditions);
        for (const auto &dependency : entry->additionalDependencies)
          if (auto projected =
                  project(dependency.designatedFields, dependency.conditions)) {
            if (exact != 0 && exact != projected) {
              valid = false;
              return 0;
            }
            exact = projected;
          }
        const auto *place = output.findPlace(exact);
        if (!place || place->key != loan.place ||
            (resultValue && place->type != resultValue->info.type) ||
            (loan.access == AccessMode::Mutable &&
             place->access != AccessMode::Mutable)) {
          valid = false;
          return 0;
        }
        semanticLoanPlaces.emplace(loan.semanticLoan, exact);
        return exact;
      }
    }

    MirPlace lowered;
    std::size_t firstProjection = 0;
    if (loan.parent != 0) {
      const HirLoan *parentLoan = source.findLoan(loan.parent);
      const auto parent = semanticLoans.find(loan.parent);
      const bool sameRoot = parentLoan != nullptr &&
                            parent != semanticLoans.end() &&
                            parentLoan->place.domain == loan.place.domain &&
                            parentLoan->place.root == loan.place.root &&
                            parentLoan->place.receiver == loan.place.receiver &&
                            parentLoan->place.projections.size() <=
                                loan.place.projections.size() &&
                            std::equal(parentLoan->place.projections.begin(),
                                       parentLoan->place.projections.end(),
                                       loan.place.projections.begin());
      // A place rooted at the parent loan names its primary referent. A
      // reference selected under one of the parent's additional owners uses
      // the owner place the carrier already materialized for that dependency
      // (the referent of a reference-typed owner); the selection loan keeps
      // the exact parent link.
      if (!sameRoot && loan.referenceSelection && parent != semanticLoans.end())
        if (const MirLoan *carrier = output.findLoan(parent->second))
          for (const auto &dependency : carrier->additionalDependencies) {
            const MirPlace *owner = output.findPlace(dependency.source);
            if (owner == nullptr || owner->key != loan.place)
              continue;
            if (resultValue == nullptr ||
                owner->type == resultValue->info.type) {
              semanticLoanPlaces.emplace(loan.semanticLoan, dependency.source);
              return dependency.source;
            }
            if (owner->root == MirPlaceRootKind::Binding &&
                owner->projections.empty() &&
                owner->type.kind == SemanticType::Reference &&
                owner->type.arguments.size() == 1 &&
                owner->type.arguments.front() == resultValue->info.type) {
              MirPlace referent = *owner;
              referent.projections.push_back(
                  {.kind = MirProjectionKind::Dereference});
              referent.type = resultValue->info.type;
              referent.traits = resultValue->info.traits;
              referent.access = loan.access;
              referent.sourceValue = resultValue->id;
              const MirPlaceId id = appendPlace(std::move(referent));
              semanticLoanPlaces.emplace(loan.semanticLoan, id);
              return id;
            }
          }
      if (!sameRoot) {
        valid = false;
        return 0;
      }
      lowered.root = MirPlaceRootKind::Loan;
      lowered.loan = parent->second;
      lowered.access = loan.access;
      firstProjection = parentLoan->place.projections.size();
    } else if (loan.place.receiver) {
      lowered.root = MirPlaceRootKind::This;
      lowered.access = loan.access;
      if (function != nullptr && function->owner && *function->owner != 0 &&
          *function->owner <= program.classInstances().size()) {
        const auto &owner = program.classInstances()[*function->owner - 1];
        lowered.type = owner.type;
        lowered.traits = owner.traits;
        lowered.access = function->source != nullptr &&
                                 receiverAllowsMutation(
                                     function->source->receiverMutability())
                             ? AccessMode::Mutable
                             : AccessMode::ReadOnly;
      } else if (destructor != nullptr) {
        const auto *owner =
            destructor->owner != 0 &&
                    destructor->owner <= program.classInstances().size()
                ? &program.classInstances()[destructor->owner - 1]
                : nullptr;
        if (owner == nullptr) {
          valid = false;
          return 0;
        }
        lowered.type = owner->type;
        lowered.traits = owner->traits;
        lowered.access = AccessMode::Mutable;
      }
    } else if (const auto local = localSymbols.find(loan.place.root);
               local != localSymbols.end()) {
      const MirPlaceId base = placeForBinding(local->second);
      const MirPlace *place = output.findPlace(base);
      if (place == nullptr) {
        valid = false;
        return 0;
      }
      if (loan.place.projections.empty()) {
        semanticLoanPlaces.emplace(loan.semanticLoan, base);
        return base;
      }
      lowered = *place;
      lowered.id = 0;
      lowered.access = loan.access;
    } else if (loan.place.root != 0) {
      lowered.root = MirPlaceRootKind::Symbol;
      lowered.symbol = loan.place.root;
      lowered.access = loan.access;
      const auto capture = lambdaCaptures.find(loan.place.root);
      if (lambda != nullptr && capture != lambdaCaptures.end()) {
        const auto &field = lambda->captures[capture->second - 1];
        lowered.capture = capture->second;
        lowered.type = field.type;
        lowered.traits = field.traits;
        lowered.initiallyAvailable = true;
      }
    } else {
      valid = false;
      return 0;
    }
    lowered.key = loan.place;
    if (resultValue != nullptr) {
      lowered.type = resultValue->info.type;
      lowered.access = resultValue->info.access;
      lowered.traits = resultValue->info.traits;
      lowered.sourceValue = resultValue->id;
    }

    for (std::size_t index = firstProjection;
         index < loan.place.projections.size(); ++index) {
      const SemanticLoanPlaceProjection &projection =
          loan.place.projections[index];
      switch (projection.kind) {
      case SemanticLoanPlaceProjectionKind::Field:
        if (projection.field == 0) {
          valid = false;
          return 0;
        }
        lowered.projections.push_back(
            {.kind = MirProjectionKind::Field, .field = projection.field});
        break;
      case SemanticLoanPlaceProjectionKind::Dereference:
        lowered.projections.push_back({.kind = MirProjectionKind::Dereference});
        break;
      case SemanticLoanPlaceProjectionKind::ConstantIndex:
        if (lowered.type.kind == SemanticType::TypePack &&
            lowered.type.concretePack &&
            projection.index < lowered.type.arguments.size()) {
          const SemanticType element = lowered.type.arguments[projection.index];
          lowered.type = element;
          lowered.traits = resolvedTraits(element);
          lowered.access = AccessMode::ReadOnly;
          lowered.projections.push_back({.kind = MirProjectionKind::PackElement,
                                         .constantIndex = projection.index});
          break;
        }
        lowered.projections.push_back({.kind = MirProjectionKind::Index,
                                       .constantIndex = projection.index});
        break;
      case SemanticLoanPlaceProjectionKind::DynamicIndex:
        lowered.projections.push_back({.kind = MirProjectionKind::Index,
                                       .selection = projection.selection});
        break;
      }
    }

    const bool packElement =
        lowered.projections.size() == 1 &&
        lowered.projections.front().kind == MirProjectionKind::PackElement;
    const MirPlaceId place = packElement
                                 ? appendPackElementPlace(std::move(lowered))
                                 : appendPlace(std::move(lowered));
    semanticLoanPlaces.emplace(loan.semanticLoan, place);
    return place;
  }

  // The exact storage place of a global or static symbol along a field path,
  // typed by that storage itself rather than by a value borrowing it.
  [[nodiscard]] MirPlaceId globalStoragePlace(const BorrowOriginPlace &origin,
                                              const PlaceDomain &domain) {
    const auto &steps = program.programInitializationPlan().steps;
    const auto step =
        std::find_if(steps.begin(), steps.end(), [&](const auto &candidate) {
          return candidate.symbol == origin.root;
        });
    if (!origin.valid() || step == steps.end())
      return 0;
    const auto &bindings = program.module().bindings;
    const auto global = std::find_if(
        bindings.begin(), bindings.end(),
        [&](const HirBinding &binding) { return binding.id == step->binding; });
    std::vector<SymbolId> fields;
    for (const auto &projection : origin.projections) {
      if (projection.kind != PlaceProjectionKind::Field)
        return 0;
      fields.push_back(projection.field);
    }
    if (global == bindings.end())
      return 0;
    const PlaceKey key{.domain = domain, .root = origin.root};
    MirPlaceId root = 0;
    for (const auto &place : output.places)
      if (place.root == MirPlaceRootKind::Symbol &&
          place.symbol == origin.root && place.projections.empty() &&
          place.type == global->info.type && place.key == key)
        root = place.id;
    if (root == 0)
      root = appendPlace({.root = MirPlaceRootKind::Symbol,
                          .symbol = origin.root,
                          .type = global->info.type,
                          .access = global->info.access,
                          .traits = resolvedTraits(global->info.type),
                          .key = key});
    return fields.empty() ? root : projectBorrowFields(root, fields);
  }

  [[nodiscard]] MirPlaceId placeForBorrowOrigin(const BorrowOriginPlace &origin,
                                                const HirValue &value) {
    if (!origin.valid()) {
      return 0;
    }
    // A carrier whose complete contract names global storage borrows that
    // storage's own typed place, not a place typed as the carrier.
    if ((value.info.type.kind == SemanticType::Class ||
         value.info.type.kind == SemanticType::Enum) &&
        value.info.traits.containsBorrowedState && value.borrowDependencies)
      if (const MirPlaceId global =
              globalStoragePlace(origin, output.placeDomain))
        return global;
    MirPlace lowered{.root = MirPlaceRootKind::Symbol,
                     .symbol = origin.root,
                     .type = value.info.type,
                     .access = value.info.access,
                     .traits = value.info.traits,
                     .sourceValue = value.id,
                     .key = PlaceKey{.domain = output.placeDomain,
                                     .root = origin.root,
                                     .projections = origin.projections}};
    for (const PlaceProjection &projection : origin.projections) {
      switch (projection.kind) {
      case PlaceProjectionKind::Field:
        if (projection.field == 0) {
          valid = false;
          return 0;
        }
        lowered.projections.push_back(
            {.kind = MirProjectionKind::Field, .field = projection.field});
        break;
      case PlaceProjectionKind::Dereference:
        lowered.projections.push_back({.kind = MirProjectionKind::Dereference});
        break;
      case PlaceProjectionKind::ConstantIndex:
        lowered.projections.push_back({.kind = MirProjectionKind::Index,
                                       .constantIndex = projection.index});
        break;
      case PlaceProjectionKind::DynamicIndex:
        lowered.projections.push_back({.kind = MirProjectionKind::Index,
                                       .selection = projection.selection});
        break;
      }
    }
    return appendPlace(std::move(lowered));
  }

  [[nodiscard]] const HirLoan *loanForBinding(HirBindingId binding) const {
    const HirBinding *resolved = findBinding(binding);
    return resolved == nullptr || resolved->info.retainedLoan == 0
               ? nullptr
               : source.findLoan(resolved->info.retainedLoan);
  }

  [[nodiscard]] HirBindingId
  movedBorrowCarrierBinding(HirValueId valueId) const {
    const HirValue *move = findValue(valueId);
    if (move == nullptr || move->kind != HirValueKind::Move ||
        move->operands.size() != 1) {
      return 0;
    }
    const HirValue *sourceValue = findValue(move->operands.front());
    if (sourceValue == nullptr ||
        (sourceValue->kind != HirValueKind::Variable &&
         sourceValue->kind != HirValueKind::QualifiedName)) {
      return 0;
    }
    const auto local = localSymbols.find(sourceValue->symbol);
    return local == localSymbols.end() ? 0 : local->second;
  }

  // Semantics records a carrier with one stored reference path as a compact
  // sole-origin loan without a field map. MIR names that field, so a borrow
  // through it resolves to the loan's referent rather than the carrier shell.
  [[nodiscard]] std::vector<SymbolId>
  solePathDesignation(const MirLoan &lowered, const HirLoan &loan,
                      HirBindingId binding) const {
    std::vector<HirBindingId> carriers = lowered.carriers;
    if (binding != 0 &&
        std::find(carriers.begin(), carriers.end(), binding) == carriers.end())
      carriers.push_back(binding);
    if (loan.entry || loan.referenceSelection ||
        !loan.additionalDependencies.empty() ||
        lowered.kind != MirLoanKind::Stored || carriers.empty() ||
        !lowered.additionalDependencies.empty())
      return {};
    const auto *source = output.findPlace(lowered.source);
    const auto *carrier = findBinding(carriers.front());
    if (source == nullptr || carrier == nullptr)
      return {};
    for (const auto candidate : carriers) {
      const auto *other = findBinding(candidate);
      if (other == nullptr || other->info.type != carrier->info.type)
        return {};
    }
    const auto path = soleStoredReferencePath(carrier->info.type);
    return path && path->second == source->type ? path->first
                                                : std::vector<SymbolId>{};
  }

  void recordLoanIdentity(MirLoanId id, const HirLoan &loan,
                          HirBindingId binding = 0) {
    if (id == 0 || id > output.loans.size()) {
      valid = false;
      return;
    }
    if (const auto found = semanticLoans.find(loan.semanticLoan);
        found != semanticLoans.end() && found->second != id) {
      valid = false;
      return;
    }
    MirLoan &lowered = output.loans[id - 1];
    if (lowered.semanticLoan != 0 &&
        lowered.semanticLoan != loan.semanticLoan) {
      valid = false;
      return;
    }
    lowered.semanticLoan = loan.semanticLoan;
    if (loan.referenceSelection) {
      const auto &selection = *loan.referenceSelection;
      const auto *value = findValue(selection.value);
      const auto found = loweredValues.find(selection.value);
      const auto *resolved = found == loweredValues.end()
                                 ? nullptr
                                 : output.findValue(found->second);
      const auto *block = resolved == nullptr
                              ? nullptr
                              : output.findBlock(resolved->definitionBlock);
      const MirInstruction *producer = nullptr;
      if (block != nullptr)
        for (const auto &instruction : block->instructions)
          if (instruction.id == resolved->definition)
            producer = &instruction;
      if (value == nullptr || resolved == nullptr || producer == nullptr ||
          value->kind != HirValueKind::PayloadExtraction ||
          value->operands.size() != 1 ||
          value->operands.front() != selection.carrier ||
          producer->kind != MirInstructionKind::Compute ||
          producer->operation != MirOperation::PayloadExtract ||
          producer->hirValue != selection.value ||
          producer->result != resolved->id || producer->operands.size() != 1 ||
          output.findPlace(producer->operands.front().place) == nullptr ||
          !producer->payloadExtractionKind ||
          !isStoredReferenceExtraction(*producer->payloadExtractionKind)) {
        valid = false;
        return;
      }
      MirReferenceSelection selected{.value = resolved->id,
                                     .carrier =
                                         producer->operands.front().place,
                                     .fields = selection.fields,
                                     .conditions = selection.conditions,
                                     .reference = selection.reference};
      if (selection.transferredFrom != 0) {
        const auto source = semanticLoans.find(selection.transferredFrom);
        if (source == semanticLoans.end() || source->second == id ||
            output.findLoan(source->second) == nullptr ||
            *producer->payloadExtractionKind !=
                PayloadExtractionKind::TransferReference) {
          valid = false;
          return;
        }
        selected.transferredFrom = source->second;
      }
      if (lowered.referenceSelection &&
          *lowered.referenceSelection != selected) {
        valid = false;
        return;
      }
      lowered.referenceSelection = std::move(selected);
    } else if (lowered.referenceSelection) {
      valid = false;
      return;
    }
    if (loan.storedDependency != 0) {
      const auto dependency = semanticLoans.find(loan.storedDependency);
      if (dependency == semanticLoans.end() ||
          (lowered.storedDependency != 0 &&
           lowered.storedDependency != dependency->second)) {
        valid = false;
        return;
      }
      lowered.storedDependency = dependency->second;
    } else if (lowered.storedDependency != 0) {
      valid = false;
      return;
    }
    if (lowered.lifecycleBound && !loan.lifecycleBound) {
      valid = false;
      return;
    }
    lowered.lifecycleBound = loan.lifecycleBound;
    // A sole stored reference path is named only while every carrier is a
    // value of that one class. A carrier of another type, such as a view
    // copied out of the holder, leaves the compact single-origin loan.
    const auto solePath = solePathDesignation(lowered, loan, binding);
    const auto namedSolePath = [&] {
      // A newly produced carrier may not be registered in the MIR group
      // until after its identity is recorded. The binding being initialized
      // still names its exact sole reference path at this point.
      if (lowered.carriers.empty())
        return !solePath.empty() && solePath == lowered.designatedFields;
      const auto *carrier = findBinding(lowered.carriers.front());
      const auto path =
          carrier ? soleStoredReferencePath(carrier->info.type) : std::nullopt;
      return path && path->first == lowered.designatedFields;
    };
    if (!lowered.designatedFields.empty() &&
        lowered.designatedFields != loan.designatedFields &&
        (!loan.designatedFields.empty() || !namedSolePath())) {
      valid = false;
      return;
    }
    lowered.designatedFields =
        loan.designatedFields.empty() ? solePath : loan.designatedFields;
    if (!lowered.conditions.empty() && lowered.conditions != loan.conditions) {
      valid = false;
      return;
    }
    lowered.conditions = loan.conditions;
    const auto previousDependencies = lowered.additionalDependencies;
    std::erase_if(lowered.additionalDependencies, [](const auto &dependency) {
      return dependency.source == 0;
    });
    if (!loan.entry &&
        (!loan.designatedFields.empty() || loan.referenceSelection))
      lowered.additionalDependencies.clear();
    for (const auto &dependency : loan.additionalDependencies) {
      if (!dependency.designatedFields.empty() || loan.referenceSelection) {
        MirPlaceId concreteSource = 0;
        if (!loan.entry) {
          const auto matches = [&](MirPlaceId id) {
            const auto *place = output.findPlace(id);
            return place != nullptr && place->key == dependency.place;
          };
          if (matches(lowered.source))
            concreteSource = lowered.source;
          for (const auto &previous : previousDependencies)
            if (matches(previous.source)) {
              concreteSource = previous.source;
              break;
            }
          // A selected scalar reference can have several alternative owners
          // even though it no longer contains a reference-bearing field path.
          // Its carrier has already materialized every external origin. Map
          // those existing places; never use the selected runtime address as
          // a replacement for an omitted owner.
          if (concreteSource == 0 && loan.referenceSelection)
            for (const auto &place : output.places)
              if (place.root != MirPlaceRootKind::Value && matches(place.id)) {
                concreteSource = place.id;
                break;
              }
          if (concreteSource == 0) {
            valid = false;
            return;
          }
        }
        lowered.additionalDependencies.push_back(
            {.source = concreteSource,
             .parent = dependency.parent == 0 ? 0
                       : semanticLoans.contains(dependency.parent)
                           ? semanticLoans.at(dependency.parent)
                           : 0,
             .designatedFields = dependency.designatedFields,
             .access = dependency.access,
             .conditions = dependency.conditions});
        if (dependency.parent != 0 &&
            lowered.additionalDependencies.back().parent == 0)
          valid = false;
      }
    }
    if (lowered.access == AccessMode::ReadOnly &&
        loan.access == AccessMode::Mutable) {
      valid = false;
      return;
    }
    // A by-reference capture's entry loan is held by the closure environment.
    // Each local alias that semantics records as its carrier holds its own
    // loan below that entry instead (see referenceOperand).
    const bool capturedEntry = capturedReferenceEntry(lowered);
    for (const HirBindingId carrier : loan.carriers) {
      if (capturedEntry)
        break;
      // Semantic traversal carriers also express the element escape boundary.
      // Concrete cursor/proxy shells have their own producer-owned loans.
      // Re-recording an outer shared traversal for a nested loop must not
      // reattach those bindings to its coarse owner loan.
      if (isPhysicalProxyRangeProtocolBinding(carrier) &&
          std::any_of(source.statements.begin(), source.statements.end(),
                      [&](const auto &statement) {
                        return statement.rangeIteration &&
                               statement.rangeIteration->rangeLoan ==
                                   loan.semanticLoan;
                      }))
        continue;
      if (std::find(lowered.carriers.begin(), lowered.carriers.end(),
                    carrier) == lowered.carriers.end()) {
        lowered.carriers.push_back(carrier);
      }
    }
    if (binding != 0 && !capturedEntry &&
        std::find(lowered.carriers.begin(), lowered.carriers.end(), binding) ==
            lowered.carriers.end()) {
      lowered.carriers.push_back(binding);
    }
    semanticLoans.insert_or_assign(loan.semanticLoan, id);
  }

  [[nodiscard]] MirPlaceId clonePlace(MirPlaceId base, const HirValue &value) {
    const MirPlace *sourcePlace = output.findPlace(base);
    if (sourcePlace == nullptr) {
      return 0;
    }
    MirPlace result = *sourcePlace;
    result.id = 0;
    result.type = value.info.type;
    result.access = value.info.access;
    result.traits = value.info.traits;
    result.sourceValue = value.id;
    return appendPlace(std::move(result));
  }

  [[nodiscard]] MirPlaceId valueRootPlace(const HirValue &value) {
    return appendPlace({.root = MirPlaceRootKind::Value,
                        .value = mirValueFor(value),
                        .type = value.info.type,
                        .access = value.info.access,
                        .traits = value.info.traits,
                        .sourceValue = value.id});
  }

  // The value an already emitted operand reads. A call's reference result is
  // read through its call-result loan, so the operand is the loaded referent
  // rather than the call's own result value.
  [[nodiscard]] MirValueId emittedOperandValue(HirValueId id) {
    if (const auto found = materializedValues.find(id);
        found != materializedValues.end())
      return found->second;
    return mirValueFor(id);
  }

  [[nodiscard]] MirValueId operandValue(HirValueId id) {
    (void)valueOperand(id);
    return emittedOperandValue(id);
  }

  [[nodiscard]] MirPlaceId rawPointerPlace(HirValueId pointer,
                                           const HirValue &value,
                                           MirProjectionKind projection,
                                           MirValueId index = 0) {
    MirPlaceId place = appendPlace({.root = MirPlaceRootKind::Value,
                                    .value = operandValue(pointer),
                                    .type = value.info.type,
                                    .access = value.info.access,
                                    .traits = value.info.traits,
                                    .sourceValue = value.id});
    output.places[place - 1].projections.push_back(
        {.kind = projection, .index = index});
    return place;
  }

  void ensureThreadLocalValue(HirValueId id) {
    const auto *threadValue = findValue(id);
    if (threadValue != nullptr && threadValue->symbol != 0 &&
        !lambdaCaptures.contains(threadValue->symbol) &&
        ensuredThreadLocalValues.insert(id).second) {
      const auto storage = std::find_if(
          program.threadLocalStorage().begin(),
          program.threadLocalStorage().end(), [&](const auto &candidate) {
            return candidate.symbol == threadValue->symbol;
          });
      if (storage != program.threadLocalStorage().end()) {
        auto failure = threadValue->definedFailure;
        for (auto &origin : failure.localOrigins)
          std::erase_if(origin.outcomes, [](const auto outcome) {
            return !threadLocalGuardOutcome(outcome);
          });
        std::erase_if(failure.localOrigins, [](const auto &origin) {
          return origin.outcomes.empty();
        });
        failure.propagation = storage->dataInitializer
                                  ? FailurePropagationKind::None
                                  : FailurePropagationKind::BodyCall;
        (void)appendInstruction(
            {.kind = MirInstructionKind::EnsureThreadLocal,
             .threadLocalStorage = storage->symbol,
             .hirValue = id,
             .definedFailure = std::move(failure),
             .bodyTarget =
                 MirBodyAddress{.kind = MirBodyKind::ThreadLocalInitializer,
                                .owner = storage->symbol},
             .info = {.type = SemanticType::Void}},
            false);
      }
    }
  }

  [[nodiscard]] MirPlaceId placeForValue(HirValueId id) {
    ensureThreadLocalValue(id);
    if (const auto found = valuePlaces.find(id); found != valuePlaces.end()) {
      return found->second;
    }
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      return 0;
    }

    MirPlaceId place = 0;
    switch (value->kind) {
    case HirValueKind::Variable:
    case HirValueKind::QualifiedName: {
      const auto local = localSymbols.find(value->symbol);
      if (local != localSymbols.end()) {
        place = placeForBinding(local->second);
        const HirBinding *binding = findBinding(local->second);
        if (binding != nullptr &&
            binding->info.type.kind == SemanticType::Reference) {
          place = clonePlace(place, *value);
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Dereference});
        }
      } else {
        const auto capture = lambdaCaptures.find(value->symbol);
        place = appendPlace(
            {.root = MirPlaceRootKind::Symbol,
             .symbol = value->symbol,
             .capture = capture == lambdaCaptures.end() ? 0 : capture->second,
             .type = value->info.type,
             .access = value->info.access,
             .traits = value->info.traits,
             .sourceValue = value->id});
        if (lambda != nullptr && capture != lambdaCaptures.end() &&
            lambda->captures[capture->second - 1].type.kind ==
                SemanticType::Reference) {
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Dereference});
        }
      }
      break;
    }
    case HirValueKind::This:
      place = appendPlace({.root = MirPlaceRootKind::This,
                           .type = value->info.type,
                           .access = value->info.access,
                           .traits = value->info.traits,
                           .sourceValue = value->id});
      break;
    case HirValueKind::Grouping:
      if (!value->operands.empty()) {
        place = clonePlace(placeForValue(value->operands.front()), *value);
      }
      break;
    case HirValueKind::Binary:
      if (value->operation == TokenKind::COMMA && !value->operands.empty()) {
        place = clonePlace(placeForValue(value->operands.back()), *value);
      }
      break;
    case HirValueKind::MemberAccess:
      if (value->functionTarget) {
        place = loanOrValuePlace(*value);
        if (place != 0 && value->info.category == ValueCategory::Place &&
            value->symbol != 0) {
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Field, .field = value->symbol});
        }
      } else if (!value->operands.empty() &&
                 value->unsafeOperation == UnsafeOperationKind::RawMember) {
        place = rawPointerPlace(value->operands.front(), *value,
                                MirProjectionKind::RawDereference);
        output.places[place - 1].projections.push_back(
            {.kind = MirProjectionKind::Field, .field = value->symbol});
      } else if (!value->operands.empty()) {
        place = clonePlace(placeForValue(value->operands.front()), *value);
        // A class-valued temporary is an SSA value, not a pre-existing place.
        // Project from that value after emitPlaceDependencies() materializes
        // it. Rooting this place at the member result would make the following
        // Load read its own result before definition. An assignment's value
        // is the copy of its destination placed after the Assign.
        if (place == 0) {
          const HirValue *receiver = findValue(value->operands.front());
          if (receiver != nullptr &&
              receiver->info.category == ValueCategory::Value &&
              receiver->info.type.kind == SemanticType::Class) {
            const auto copy = assignmentSnapshots.find(receiver->id);
            place = appendPlace({.root = MirPlaceRootKind::Value,
                                 .value = copy == assignmentSnapshots.end()
                                              ? mirValueFor(*receiver)
                                              : copy->second,
                                 .type = value->info.type,
                                 .access = value->info.access,
                                 .traits = value->info.traits,
                                 .sourceValue = value->id});
          }
        }
        if (place != 0) {
          if (value->operation == TokenKind::ARROW) {
            output.places[place - 1].projections.push_back(
                {.kind = MirProjectionKind::Dereference});
          }
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Field, .field = value->symbol});
          if (value->storedReferenceAccess) {
            output.places[place - 1].projections.push_back(
                {.kind = MirProjectionKind::Dereference});
          }
        }
      }
      break;
    case HirValueKind::Index:
      if (value->functionTarget) {
        place = loanOrValuePlace(*value);
      } else if (value->operands.size() >= 2 &&
                 value->unsafeOperation == UnsafeOperationKind::RawIndex) {
        (void)valueOperand(value->operands[0]);
        const MirValueId index = operandValue(value->operands[1]);
        place = rawPointerPlace(value->operands[0], *value,
                                MirProjectionKind::RawIndex, index);
      } else if (value->operands.size() >= 2) {
        place = clonePlace(placeForValue(value->operands[0]), *value);
        // A call, move, or joined expression can produce an array value
        // without a source place. Its element is selected from the
        // materialized value, after emitPlaceDependencies() has evaluated
        // that producer and before the index is used.
        if (place == 0) {
          const HirValue *array = findValue(value->operands[0]);
          if (array != nullptr &&
              array->info.category == ValueCategory::Value &&
              array->info.type.kind == SemanticType::Array) {
            const auto copy = assignmentSnapshots.find(array->id);
            const MirPlaceId arrayRoot =
                appendPlace({.root = MirPlaceRootKind::Value,
                             .value = copy == assignmentSnapshots.end()
                                          ? mirValueFor(*array)
                                          : copy->second,
                             .type = array->info.type,
                             .access = array->info.access,
                             .traits = array->info.traits,
                             .sourceValue = array->id});
            place = clonePlace(arrayRoot, *value);
          }
        }
        if (place != 0) {
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Index,
               .index = emittedOperandValue(value->operands[1])});
        }
      }
      break;
    case HirValueKind::PackElement: {
      const auto *pack = value->operands.size() == 1
                             ? findValue(value->operands.front())
                             : nullptr;
      if (!value->packElement || !pack ||
          pack->info.type.kind != SemanticType::TypePack ||
          !pack->info.type.concretePack || value->symbol == 0 ||
          pack->symbol != value->symbol ||
          *value->packElement >= pack->info.type.arguments.size() ||
          pack->info.type.arguments[*value->packElement] != value->info.type ||
          value->info.category != ValueCategory::Place ||
          value->info.access != AccessMode::ReadOnly ||
          value->info.traits != resolvedTraits(value->info.type)) {
        valid = false;
        break;
      }
      const auto *sourcePlace = output.findPlace(placeForValue(pack->id));
      if (!sourcePlace || sourcePlace->root != MirPlaceRootKind::Binding ||
          sourcePlace->capture != 0 || !sourcePlace->projections.empty() ||
          sourcePlace->symbol != pack->symbol ||
          sourcePlace->type != pack->info.type) {
        valid = false;
        break;
      }
      MirPlace projected = *sourcePlace;
      projected.id = 0;
      projected.projections.push_back({.kind = MirProjectionKind::PackElement,
                                       .constantIndex = *value->packElement});
      projected.type = value->info.type;
      projected.traits = resolvedTraits(value->info.type);
      projected.access = AccessMode::ReadOnly;
      projected.sourceValue = 0;
      projected.key.reset();
      place = appendPackElementPlace(std::move(projected));
      break;
    }
    case HirValueKind::Call:
    case HirValueKind::Propagation:
    case HirValueKind::Conditional:
    case HirValueKind::ScopedResult:
      place = loanOrValuePlace(*value);
      break;
    case HirValueKind::Unary:
      if (value->operation == TokenKind::STAR && !value->operands.empty() &&
          value->unsafeOperation == UnsafeOperationKind::RawDereference) {
        place = rawPointerPlace(value->operands.front(), *value,
                                MirProjectionKind::RawDereference);
      } else {
        place = loanOrValuePlace(*value);
        if (place == 0 && value->operation == TokenKind::STAR &&
            !value->operands.empty()) {
          place = valueRootPlace(*value);
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Dereference});
        }
      }
      break;
    default:
      if (value->info.category == ValueCategory::Place) {
        place = valueRootPlace(*value);
      }
      break;
    }
    if (place == 0 && value->info.category == ValueCategory::Place) {
      place = valueRootPlace(*value);
    }
    if (place != 0) {
      attachPlaceIdentity(place, *value);
      valuePlaces.insert_or_assign(id, place);
    }
    return place;
  }

  [[nodiscard]] MirPlaceId loanOrValuePlace(const HirValue &value) {
    // A returned carrier is a class value whose loan protects a different
    // external owner. It is not a reference to that owner: field selection
    // must project from the materialized SSA shell, not from the loan source.
    if (value.info.category == ValueCategory::Value &&
        value.info.type.kind == SemanticType::Class &&
        value.info.traits.containsBorrowedState) {
      return 0;
    }
    const auto found = valueLoans.find(value.id);
    if (found != valueLoans.end()) {
      return appendPlace({.root = MirPlaceRootKind::Loan,
                          .loan = found->second,
                          .type = value.info.type,
                          .access = value.info.access,
                          .traits = value.info.traits,
                          .sourceValue = value.id});
    }
    return value.info.category == ValueCategory::Place ? valueRootPlace(value)
                                                       : 0;
  }

  [[nodiscard]] MirPlaceId destinationFor(const HirValue &value) {
    ensureThreadLocalValue(value.id);
    switch (value.kind) {
    case HirValueKind::PlaceSet:
      if (value.operands.size() == 2) {
        const HirValue *target = findValue(value.operands.front());
        return target == nullptr
                   ? 0
                   : clonePlace(placeForValue(target->id), *target);
      }
      return 0;
    case HirValueKind::Assignment: {
      const auto local = localSymbols.find(value.symbol);
      if (local != localSymbols.end()) {
        const MirPlaceId bindingPlace = placeForBinding(local->second);
        const HirBinding *binding = findBinding(local->second);
        if (binding != nullptr &&
            binding->info.type.kind == SemanticType::Reference) {
          const MirPlaceId referent = clonePlace(bindingPlace, value);
          if (referent != 0) {
            output.places[referent - 1].projections.push_back(
                {.kind = MirProjectionKind::Dereference});
          }
          return referent;
        }
        return bindingPlace;
      }
      const auto capture = lambdaCaptures.find(value.symbol);
      const auto *foreign = program.findForeignStorage(value.symbol);
      const auto place = appendPlace(
          {.root = MirPlaceRootKind::Symbol,
           .symbol = value.symbol,
           .capture = capture == lambdaCaptures.end() ? 0 : capture->second,
           .type = value.info.type,
           .access = foreign ? foreign->access : value.info.access,
           .traits = value.info.traits,
           .sourceValue = value.id});
      if (lambda != nullptr && capture != lambdaCaptures.end() &&
          lambda->captures[capture->second - 1].type.kind ==
              SemanticType::Reference) {
        output.places[place - 1].access =
            lambda->captures[capture->second - 1].type.referenceAccess;
        output.places[place - 1].projections.push_back(
            {.kind = MirProjectionKind::Dereference});
      }
      return place;
    }
    case HirValueKind::MemberSet:
      if (!value.operands.empty()) {
        MirPlaceId place =
            value.unsafeOperation == UnsafeOperationKind::RawMember
                ? rawPointerPlace(value.operands[0], value,
                                  MirProjectionKind::RawDereference)
                : clonePlace(placeForValue(value.operands[0]), value);
        if (place != 0) {
          if (output.places[place - 1].foreignStorage)
            output.places[place - 1].access = AccessMode::Mutable;
          if (value.operation == TokenKind::ARROW &&
              value.unsafeOperation != UnsafeOperationKind::RawMember) {
            output.places[place - 1].projections.push_back(
                {.kind = MirProjectionKind::Dereference});
          }
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Field, .field = value.symbol});
          if (value.storedReferenceAccess)
            output.places[place - 1].projections.push_back(
                {.kind = MirProjectionKind::Dereference});
        }
        return place;
      }
      break;
    case HirValueKind::IndexSet:
      if (value.operands.size() >= 2) {
        MirPlaceId place = 0;
        if (value.unsafeOperation == UnsafeOperationKind::RawIndex) {
          (void)valueOperand(value.operands[0]);
          const MirValueId index = operandValue(value.operands[1]);
          place = rawPointerPlace(value.operands[0], value,
                                  MirProjectionKind::RawIndex, index);
        } else {
          place = clonePlace(placeForValue(value.operands[0]), value);
        }
        if (place != 0) {
          if (output.places[place - 1].foreignStorage)
            output.places[place - 1].access = AccessMode::Mutable;
          if (value.unsafeOperation != UnsafeOperationKind::RawIndex) {
            output.places[place - 1].projections.push_back(
                {.kind = MirProjectionKind::Index,
                 .index = emittedOperandValue(value.operands[1])});
          }
        }
        return place;
      }
      break;
    case HirValueKind::DereferenceSet:
      if (!value.operands.empty()) {
        MirPlaceId place =
            value.unsafeOperation == UnsafeOperationKind::RawDereference
                ? rawPointerPlace(value.operands[0], value,
                                  MirProjectionKind::RawDereference)
                : valueRootPlace(value);
        if (value.unsafeOperation != UnsafeOperationKind::RawDereference) {
          output.places[place - 1].value = mirValueFor(value.operands[0]);
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Dereference});
        }
        return place;
      }
      break;
    default:
      break;
    }
    return 0;
  }

  void emitProgramConstantSubstitution(const HirValue &value) {
    if (emittedValues.contains(value.id)) {
      return;
    }
    if (!value.programConstantSubstitution || !value.constant) {
      valid = false;
      return;
    }
    const std::optional<Literal> literal =
        programConstantLiteral(*value.constant);
    if (!literal &&
        !std::holds_alternative<ConstantAggregateValue>(*value.constant)) {
      valid = false;
      return;
    }
    ExpressionInfo materializedInfo = value.info;
    materializedInfo.category = ValueCategory::Value;
    materializedInfo.access = AccessMode::ReadOnly;
    const auto *integer = std::get_if<ConstantInteger>(&*value.constant);
    const auto *enumerator = std::get_if<ConstantEnum>(&*value.constant);
    // A payload enum is an aggregate with an exact active alternative, not an
    // integral enumerator. Keep it on the ordinary aggregate literal path.
    const bool enumeration =
        value.info.type.kind == SemanticType::Enum &&
        !std::holds_alternative<ConstantAggregateValue>(*value.constant);
    const bool negativeInteger = integer != nullptr && integer->negative;
    if (enumeration) {
      const ConstantInteger *enumValue =
          enumerator == nullptr ? integer : &enumerator->value;
      if (enumValue == nullptr || value.info.type.enumId == 0 ||
          (enumerator != nullptr &&
           enumerator->enumId != value.info.type.enumId)) {
        valid = false;
        return;
      }
      const MirValueId result = appendValue(value.id, materializedInfo);
      loweredValues.insert_or_assign(value.id, result);
      (void)appendInstruction(
          {.kind = MirInstructionKind::Compute,
           .hirValue = value.id,
           .result = result,
           .operation = MirOperation::EnumConstant,
           .programConstantSubstitution = true,
           .enumOwner = value.info.type.enumId,
           .enumValue = EnumConstant{.negative = enumValue->negative,
                                     .magnitude = enumValue->magnitude},
           .info = materializedInfo});
      emittedValues.insert(value.id);
      return;
    }
    if (negativeInteger) {
      const MirValueId magnitude = appendValue(value.id, materializedInfo);
      (void)appendInstruction(
          {.kind = MirInstructionKind::Compute,
           .hirValue = value.id,
           .result = magnitude,
           .operation = MirOperation::Literal,
           .literal = literal,
           .literalProvenance = {.kind = MirLiteralProvenanceKind::Source},
           .info = materializedInfo});
      const MirValueId result = appendValue(value.id, materializedInfo);
      loweredValues.insert_or_assign(value.id, result);
      (void)appendInstruction({.kind = MirInstructionKind::Compute,
                               .hirValue = value.id,
                               .result = result,
                               .operands = {{.kind = MirOperandKind::Value,
                                             .value = magnitude,
                                             .type = value.info.type}},
                               .operation = MirOperation::Negate,
                               .programConstantSubstitution = true,
                               .info = materializedInfo});
      emittedValues.insert(value.id);
      return;
    }
    const MirValueId result = appendValue(value.id, materializedInfo);
    loweredValues.insert_or_assign(value.id, result);
    (void)appendInstruction(
        {.kind = MirInstructionKind::Compute,
         .hirValue = value.id,
         .result = result,
         .operation = MirOperation::Literal,
         .literal = literal,
         .literalProvenance = {.kind = MirLiteralProvenanceKind::Source},
         .programConstantSubstitution = true,
         .info = materializedInfo});
    emittedValues.insert(value.id);
  }

  [[nodiscard]] MirOperand valueOperand(HirValueId id) {
    if (terminated()) {
      return {};
    }
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      valid = false;
      return {};
    }
    if (value->programConstantSubstitution) {
      emitProgramConstantSubstitution(*value);
      return {.kind = MirOperandKind::Value,
              .value = mirValueFor(*value),
              .type = value->info.type};
    }
    // Consuming a class assignment's whole result consumes the copy of its
    // committed destination that a projection of the result reads. A
    // discarded assignment is emitted as a sequenced input, never here.
    if (hasAssignmentResultCopy(*value)) {
      const MirValueId result = assignmentResultValue(*value);
      if (result == 0)
        return {};
      return {.kind = MirOperandKind::Value,
              .value = result,
              .type = value->info.type};
    }
    if (emittedValues.contains(id)) {
      const MirLoanId loan = loanForValue(id);
      const MirLoan *borrow = output.findLoan(loan);
      if (value->info.category == ValueCategory::Place && borrow != nullptr &&
          borrow->kind == MirLoanKind::CallResult) {
        if (const auto found = materializedValues.find(id);
            found != materializedValues.end()) {
          return {.kind = MirOperandKind::Value,
                  .value = found->second,
                  .type = value->info.type};
        }
        ExpressionInfo loadedInfo = value->info;
        loadedInfo.category = ValueCategory::Value;
        loadedInfo.access = AccessMode::ReadOnly;
        const MirValueId loaded = appendValue(id, loadedInfo);
        const MirPlaceId place = placeForValue(id);
        (void)appendInstruction({.kind = MirInstructionKind::Load,
                                 .hirValue = id,
                                 .result = loaded,
                                 .operands = {{.kind = MirOperandKind::Copy,
                                               .place = place,
                                               .type = value->info.type}},
                                 .info = loadedInfo});
        materializedValues.emplace(id, loaded);
        endConsumedCallResultLoans({id}, *value);
        return {.kind = MirOperandKind::Value,
                .value = loaded,
                .type = value->info.type};
      }
      return {.kind = MirOperandKind::Value,
              .value = mirValueFor(*value),
              .type = value->info.type};
    }
    // Sequenced evaluations, such as a comma's left operand, complete before
    // the value they precede on every path that reads it.
    for (const HirValueId input : value->sequencedInputs) {
      emitValue(input);
      if (terminated()) {
        return {};
      }
    }
    if (value->kind == HirValueKind::Move && !value->operands.empty()) {
      emitPlaceDependencies(value->operands.front());
      MirPlaceId sourcePlace = placeForValue(value->operands.front());
      const HirValue *source = findValue(value->operands.front());
      if (sourcePlace == 0) {
        // A materialized temporary moves out of its value slot.
        sourcePlace = temporarySlot(value->operands.front());
      }
      if (source != nullptr && (source->kind == HirValueKind::Variable ||
                                source->kind == HirValueKind::QualifiedName)) {
        const auto local = localSymbols.find(source->symbol);
        const HirBinding *binding =
            local == localSymbols.end() ? nullptr : findBinding(local->second);
        if (binding != nullptr &&
            binding->info.type.kind == SemanticType::Reference) {
          sourcePlace = placeForBinding(binding->id);
        }
      }
      MirInstruction move{.kind = MirInstructionKind::Move,
                          .hirValue = value->id,
                          .result = resultFor(*value),
                          .operands = {{.kind = MirOperandKind::Move,
                                        .place = sourcePlace,
                                        .type = value->info.type}},
                          .intrinsic = value->intrinsic,
                          .definedFailure = localDefinedFailure(source),
                          .info = value->info};
      (void)initializeValueLifecycle(move, *value);
      (void)appendInstruction(std::move(move));
      if (const MirLoanId loan = loanForValue(value->operands.front());
          loan != 0) {
        valueLoans.insert_or_assign(value->id, loan);
      }
      emittedValues.insert(value->id);
      return {.kind = MirOperandKind::Value,
              .value = mirValueFor(*value),
              .type = value->info.type};
    }
    // Reading a resolved integral enumerator is an exact constant operation,
    // not a read of runtime global storage. Keep explicit place formation for
    // actual borrowing separate from value materialization.
    if (value->info.category == ValueCategory::Place &&
        isPlaceExpression(value->kind) &&
        computeOperation(*value) != MirOperation::EnumConstant) {
      // A class element is copied as a whole object, which its consumer
      // constructs from the element place. As for a copied argument, an
      // element selected at a run-time index completes its check at its
      // AddressOf before the copy.
      emitPlaceDependencies(id, value->info.type.kind == SemanticType::Class &&
                                    dynamicallySelectedElement(id));
      if (emittedValues.contains(id)) {
        return valueOperand(id);
      }
      const MirPlaceId place = placeForValue(id);
      (void)appendInstruction({.kind = MirInstructionKind::Load,
                               .hirValue = value->id,
                               .result = resultFor(*value),
                               .operands = {{.kind = MirOperandKind::Copy,
                                             .place = place,
                                             .type = value->info.type}},
                               .definedFailure = selectedPlaceFailure(value),
                               .info = value->info});
      emittedValues.insert(id);
      endConsumedCallResultLoans(value->operands, *value);
      return {.kind = MirOperandKind::Value,
              .value = mirValueFor(*value),
              .type = value->info.type};
    }
    emitValue(id);
    if (terminated()) {
      return {};
    }
    return {.kind = MirOperandKind::Value,
            .value = mirValueFor(*value),
            .type = value->info.type};
  }

  [[nodiscard]] MirOperand addressOperand(HirValueId id) {
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      valid = false;
      return {};
    }
    emitPlaceDependencies(id);
    if (terminated()) {
      return {};
    }
    const MirPlaceId place = placeForValue(id);
    if (place == 0) {
      valid = false;
    }
    return {.kind = MirOperandKind::Address,
            .place = place,
            .type = value->info.type};
  }

  // Forming an address evaluates its place without loading the referent. The
  // terminal fixed-array selection therefore must publish its own semantic
  // bounds origin at AddressOf, rather than disappear with that omitted Load.
  [[nodiscard]] DefinedFailureOperation addressDefinedFailure(HirValueId id) {
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      valid = false;
      return {};
    }
    if (value->kind == HirValueKind::Index &&
        !value->definedFailure.localOrigins.empty()) {
      return localDefinedFailure(value);
    }
    if ((value->kind == HirValueKind::Grouping ||
         value->kind == HirValueKind::MemberAccess) &&
        !value->operands.empty()) {
      return addressDefinedFailure(value->operands.front());
    }
    if (value->kind == HirValueKind::Binary &&
        value->operation == TokenKind::COMMA && !value->operands.empty())
      return addressDefinedFailure(value->operands.back());
    return {};
  }

  [[nodiscard]] static bool isPlaceExpression(HirValueKind kind) {
    switch (kind) {
    case HirValueKind::Call:
    case HirValueKind::Propagation:
    case HirValueKind::Conditional:
    case HirValueKind::ScopedResult:
    case HirValueKind::Grouping:
    case HirValueKind::Index:
    case HirValueKind::PackElement:
    case HirValueKind::MemberAccess:
    case HirValueKind::QualifiedName:
    case HirValueKind::This:
    case HirValueKind::Unary:
    case HirValueKind::Variable:
      return true;
    default:
      return false;
    }
  }

  [[nodiscard]] static bool isCallLike(const HirValue &value) {
    return value.kind == HirValueKind::Call || value.functionTarget;
  }

  // Built-in stores, including a plain store through a selected accessor.
  // Each lowers to one MIR Assign, whose own result is expression
  // bookkeeping rather than an object.
  [[nodiscard]] static bool isAssignmentExpression(HirValueKind kind) {
    switch (kind) {
    case HirValueKind::Assignment:
    case HirValueKind::MemberSet:
    case HirValueKind::IndexSet:
    case HirValueKind::DereferenceSet:
    case HirValueKind::PlaceSet:
      return true;
    default:
      return false;
    }
  }

  // Semantic analysis gives a built-in class assignment a fresh value with
  // temporary-storage lifetime: a copy of the committed destination, not the
  // destination place. Reading the destination later would observe a write
  // made by a later operand of the same full-expression. MIR therefore states
  // that copy as one Load placed directly after the Assign, before any later
  // operand runs and before a selected accessor's store loan ends. Only a
  // copyable class without borrowed state or active cleanup has such a copy.
  [[nodiscard]] bool hasAssignmentResultCopy(const HirValue &value) const {
    if (!isAssignmentExpression(value.kind) ||
        value.info.type.kind != SemanticType::Class ||
        !value.info.traits.copyable || value.info.traits.containsBorrowedState)
      return false;
    const auto owner = std::find_if(program.classInstances().begin(),
                                    program.classInstances().end(),
                                    [&](const HirClassInstance &candidate) {
                                      return candidate.type == value.info.type;
                                    });
    return owner != program.classInstances().end() &&
           !owner->requiresActiveCleanup;
  }

  [[nodiscard]] MirValueId assignmentResultSnapshot(const HirValue &value) {
    if (const auto found = assignmentSnapshots.find(value.id);
        found != assignmentSnapshots.end())
      return found->second;
    if (!hasAssignmentResultCopy(value)) {
      reject(value.id, "assignment result has no copy of its destination");
      return 0;
    }
    if (emittedValues.contains(value.id)) {
      // The destination may have changed since the commit.
      reject(value.id, "assignment result was requested after its commit");
      return 0;
    }
    assignmentSnapshotRequests.insert(value.id);
    emitValue(value.id);
    assignmentSnapshotRequests.erase(value.id);
    const auto found = assignmentSnapshots.find(value.id);
    if (found == assignmentSnapshots.end()) {
      if (!terminated())
        reject(value.id, "assignment result copy was not materialized");
      return 0;
    }
    return found->second;
  }

  // The Load that assignmentResultSnapshot() requests. Projections of the
  // assignment result are rooted at its value, as for any class temporary.
  void emitAssignmentResultCopy(const HirValue &value, MirPlaceId destination) {
    if (destination == 0 || terminated()) {
      return;
    }
    ExpressionInfo info = value.info;
    info.category = ValueCategory::Value;
    info.access = AccessMode::ReadOnly;
    const MirValueId snapshot = appendValue(value.id, info);
    // The copy reads the committed destination and records that read; the
    // assignment's own reinitialization event stays on the Assign. A store
    // through a raw pointer is read back through that same raw place.
    const MirPlace *committed = output.findPlace(destination);
    std::optional<OwnershipEvent> read;
    if (committed != nullptr && committed->key)
      read = OwnershipEvent{.kind = OwnershipEventKind::Read,
                            .place = *committed->key};
    (void)appendInstruction({.kind = MirInstructionKind::Load,
                             .hirValue = value.id,
                             .unsafeOperation = value.unsafeOperation,
                             .result = snapshot,
                             .operands = {{.kind = MirOperandKind::Copy,
                                           .place = destination,
                                           .type = value.info.type}},
                             .info = info,
                             .ownership = read},
                            false);
    assignmentSnapshots.emplace(value.id, snapshot);
  }

  // Whether `id` is an element selected at a run-time index, seen through
  // groupings and field projections. A constant index keeps the static bound
  // proof of its place instead of a run-time check before the access.
  [[nodiscard]] bool dynamicallySelectedElement(HirValueId id) const {
    const HirValue *value = findValue(id);
    while (value != nullptr && !value->operands.empty() &&
           (value->kind == HirValueKind::Grouping ||
            (value->kind == HirValueKind::MemberAccess &&
             value->unsafeOperation != UnsafeOperationKind::RawMember)))
      value = findValue(value->operands.front());
    if (value == nullptr || value->kind != HirValueKind::Index || !value->place)
      return false;
    const auto index = std::find_if(
        value->place->projections.rbegin(), value->place->projections.rend(),
        [](const PlaceProjection &projection) {
          return projection.kind == PlaceProjectionKind::ConstantIndex ||
                 projection.kind == PlaceProjectionKind::DynamicIndex;
        });
    return index != value->place->projections.rend() &&
           index->kind == PlaceProjectionKind::DynamicIndex;
  }

  // The local failure of an access to `value`'s place, without the bound its
  // completed AddressOf prefix already checked. Membership is recorded only
  // after emitting this exact Index's AddressOf with its local failure.
  // Retain every other origin/propagation family rather than erasing
  // arbitrary operation failures when this source shape grows.
  [[nodiscard]] DefinedFailureOperation
  selectedPlaceFailure(const HirValue *value) {
    DefinedFailureOperation failure = localDefinedFailure(value);
    if (value != nullptr && checkedPlacePrefixes.contains(value->id))
      std::erase_if(failure.localOrigins, [](const auto &origin) {
        return origin.outcomes == std::vector<DefinedFailureOutcome>{
                                      {DefinedFailureCode::IndexOutOfBounds,
                                       DefinedFailureDetail::FixedArray}};
      });
    return failure;
  }

  // The whole result is that copy. When HIR planned a value obligation for
  // the result (a borrowed receiver keeps it alive), the copy initializes a
  // hidden temporary carrying that obligation, exactly as a value
  // conditional's join publishes an arm, and the result leaves it; the
  // borrow then reads a live temporary rather than the destination.
  [[nodiscard]] MirValueId assignmentResultValue(const HirValue &value) {
    if (const auto found = assignmentResults.find(value.id);
        found != assignmentResults.end())
      return found->second;
    const MirValueId copy = assignmentResultSnapshot(value);
    if (copy == 0 || terminated())
      return 0;
    if (!value.dropObligation)
      return copy;
    const MirPlaceId temporary = conditionalTemporary(value);
    const MirDropObligationId obligation =
        dropObligationForValue(value.id, temporary);
    if (obligation == 0) {
      valid = false;
      return 0;
    }
    MirInstruction initialize{
        .kind = MirInstructionKind::Initialize,
        .hirValue = value.id,
        .destination = temporary,
        .operands = {{.kind = MirOperandKind::Value,
                      .value = copy,
                      .type = value.info.type}},
        .info = ExpressionInfo{.type = value.info.type,
                               .category = ValueCategory::Place,
                               .access = AccessMode::Mutable,
                               .traits = value.info.traits}};
    appendLifecycle(initialize, {.kind = MirLifecycleEventKind::Initialize,
                                 .target = obligation});
    registerTemporary(obligation);
    (void)appendInstruction(std::move(initialize), false);
    const bool moveResult = value.info.traits.drop != DropKind::Trivial;
    ExpressionInfo info = value.info;
    info.category = ValueCategory::Value;
    const MirValueId result = appendValue(value.id, info);
    (void)appendInstruction(
        {.kind =
             moveResult ? MirInstructionKind::Move : MirInstructionKind::Load,
         .hirValue = value.id,
         .result = result,
         .operands = {{.kind = moveResult ? MirOperandKind::Move
                                          : MirOperandKind::Copy,
                       .place = temporary,
                       .type = value.info.type}},
         .info = info},
        false);
    assignmentResults.emplace(value.id, result);
    return result;
  }

  // HIR stages a class assignment's result passed by value as a copy. The
  // copy reads the committed destination itself, staged directly after the
  // Assign and before any later operand runs, exactly as a copied local
  // argument is. A store through a selected accessor has no destination left
  // to read once its loan ends.
  [[nodiscard]] MirOperand
  assignmentResultCopyOperand(const HirValue &value,
                              const SemanticType &parameter) {
    if (emittedValues.contains(value.id)) {
      reject(value.id, "assignment result was requested after its commit");
      return {};
    }
    assignmentDestinationRequests.insert(value.id);
    emitValue(value.id);
    assignmentDestinationRequests.erase(value.id);
    if (terminated())
      return {};
    const auto found = assignmentDestinations.find(value.id);
    const MirPlaceId place = found == assignmentDestinations.end()
                                 ? 0
                                 : clonePlace(found->second, value);
    if (place == 0) {
      reject(value.id, "assignment result has no destination to copy");
      return {};
    }
    return {.kind = MirOperandKind::Copy, .place = place, .type = parameter};
  }

  // A projected array used as another index's base must finish its checked
  // place formation before that outer index starts. Checking only at the
  // eventual load/store/address would run later side effects after failure.
  // Address-only receiver/argument transport also uses this stage because
  // it has no terminal load to perform that selection's check.
  void emitCheckedPlacePrefix(const HirValue &value) {
    if (checkedPlacePrefixes.contains(value.id) || terminated() ||
        value.kind != HirValueKind::Index || value.functionTarget ||
        value.unsafeOperation == UnsafeOperationKind::RawIndex ||
        value.operands.size() != 2)
      return;
    const auto *base = findValue(value.operands.front());
    if (base == nullptr || base->info.type.kind != SemanticType::Array ||
        value.definedFailure.localOrigins.empty())
      return;
    const MirPlaceId place = placeForValue(value.id);
    const auto *selected = output.findPlace(place);
    if (selected == nullptr) {
      valid = false;
      return;
    }
    const auto pointer =
        SemanticType::rawPointerTo(selected->type, selected->access);
    ExpressionInfo info{.type = pointer,
                        .category = ValueCategory::Value,
                        .access = AccessMode::ReadOnly,
                        .traits = semanticTraits(pointer)};
    (void)appendInstruction({.kind = MirInstructionKind::Compute,
                             .hirValue = value.id,
                             .result = appendValue(value.id, info),
                             .operands = {{.kind = MirOperandKind::Address,
                                           .place = place,
                                           .type = selected->type}},
                             .operation = MirOperation::AddressOf,
                             .definedFailure = localDefinedFailure(&value),
                             .info = info});
    checkedPlacePrefixes.insert(value.id);
  }

  void emitPlaceDependencies(HirValueId id, bool checkedPrefix = false) {
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      valid = false;
      return;
    }
    for (const HirValueId input : value->sequencedInputs) {
      emitValue(input);
      if (terminated()) {
        return;
      }
    }
    // An assignment, including a store through a selected accessor, is
    // evaluated here as the projected object; the projection then reads its
    // result copy rather than the destination.
    if (isAssignmentExpression(value->kind)) {
      (void)assignmentResultSnapshot(*value);
      return;
    }
    // A direct-initialized class temporary is a fresh SSA value like a call
    // result: materialize it before a projection roots a place at it. So is
    // a value moved out of a place: the Move leaves its source moved-from and
    // the projection reads the moved temporary, not the source place.
    if (isCallLike(*value) || value->kind == HirValueKind::DirectInitializer ||
        value->kind == HirValueKind::Move ||
        value->kind == HirValueKind::Propagation ||
        value->kind == HirValueKind::Conditional ||
        value->kind == HirValueKind::ScopedResult) {
      emitValue(id);
      return;
    }
    switch (value->kind) {
    case HirValueKind::Binary:
      if (value->operation == TokenKind::COMMA && !value->operands.empty()) {
        for (auto operand = value->operands.begin();
             operand != std::prev(value->operands.end()); ++operand) {
          // A discarded assignment commits without copying its result.
          const HirValue *discarded = findValue(*operand);
          if (discarded != nullptr && isAssignmentExpression(discarded->kind))
            emitValue(*operand);
          else
            (void)valueOperand(*operand);
        }
        emitPlaceDependencies(value->operands.back(), checkedPrefix);
      }
      break;
    case HirValueKind::Grouping:
    case HirValueKind::PackElement:
      if (!value->operands.empty()) {
        emitPlaceDependencies(value->operands.front(), checkedPrefix);
      }
      break;
    case HirValueKind::MemberAccess:
      if (!value->operands.empty()) {
        if (value->unsafeOperation == UnsafeOperationKind::RawMember) {
          (void)valueOperand(value->operands.front());
        } else {
          // A field projection performs no selection check, so an element
          // selected at a run-time index completes its check at its
          // AddressOf before any access through the field.
          emitPlaceDependencies(value->operands.front(),
                                checkedPrefix ||
                                    dynamicallySelectedElement(id));
        }
      }
      break;
    case HirValueKind::Index:
      if (!value->operands.empty()) {
        if (value->unsafeOperation == UnsafeOperationKind::RawIndex) {
          (void)valueOperand(value->operands.front());
        } else {
          emitPlaceDependencies(value->operands.front(), true);
        }
      }
      if (value->operands.size() >= 2) {
        (void)valueOperand(value->operands[1]);
      }
      if (checkedPrefix)
        emitCheckedPlacePrefix(*value);
      break;
    case HirValueKind::Unary:
      if (value->operation == TokenKind::STAR && !value->operands.empty()) {
        (void)valueOperand(value->operands.front());
      }
      break;
    default:
      break;
    }
  }

  // The not-yet-emitted borrow-forming access call (for example a mutable
  // `operator[]` returning a checked element reference) that roots a
  // projected store target's place chain. A plain field store through such a
  // call defers the call, and the exclusive receiver-tied loan it begins,
  // to the commit, so the walk crosses only evaluation-free projections
  // (groupings and ordinary member accesses) on the way to the root.
  [[nodiscard]] const HirValue *
  pendingProjectedBorrowStoreRoot(HirValueId id) const {
    const HirValue *value = findValue(id);
    if (value == nullptr || emittedValues.contains(id)) {
      return nullptr;
    }
    if (isCallLike(*value)) {
      return value->functionTarget &&
                     value->borrowOrigin != BorrowOriginKind::None
                 ? value
                 : nullptr;
    }
    switch (value->kind) {
    case HirValueKind::Grouping:
      return value->operands.empty()
                 ? nullptr
                 : pendingProjectedBorrowStoreRoot(value->operands.front());
    case HirValueKind::MemberAccess:
      return value->operands.empty() ||
                     value->unsafeOperation == UnsafeOperationKind::RawMember
                 ? nullptr
                 : pendingProjectedBorrowStoreRoot(value->operands.front());
    default:
      return nullptr;
    }
  }

  // Target-first operand evaluation for a deferred borrow-forming access
  // call: the receiver place and the argument values are formed now, before
  // the store's right operand, while the checked call itself, and the
  // exclusive loan it begins, waits for the commit. Each branch performs
  // exactly the emission side of the operand handling emitCall() will repeat,
  // so the later call finds every operand already materialized.
  void emitDeferredBorrowRootOperands(const HirValue &call) {
    if (call.callPlan) {
      if (call.callPlan->receiver) {
        const HirCallReceiver &receiver = *call.callPlan->receiver;
        if (receiver.kind == HirCallInputKind::ReadTemporaryBorrow ||
            receiver.kind == HirCallInputKind::MutableTemporaryBorrow) {
          (void)valueOperand(receiver.value);
        } else {
          emitPlaceDependencies(receiver.value);
        }
      }
      for (const HirCallArgument &argument : call.callPlan->arguments) {
        switch (argument.kind) {
        case HirCallInputKind::ReadBorrow:
        case HirCallInputKind::MutableBorrow:
        case HirCallInputKind::CopyValue:
          emitPlaceDependencies(argument.value);
          break;
        default:
          (void)valueOperand(argument.value);
          break;
        }
      }
      return;
    }
    if (const std::optional<HirValueId> receiver = receiverValue(call)) {
      emitPlaceDependencies(*receiver);
    }
    const std::vector<HirValueId> arguments = callArgumentValues(call);
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      if (index < call.parameterTypes.size() &&
          call.parameterTypes[index].kind == SemanticType::Reference) {
        emitPlaceDependencies(arguments[index]);
      } else {
        (void)valueOperand(arguments[index]);
      }
    }
  }

  // The entry loan through which a lambda body holds one capture: the borrow
  // a by-reference capture makes of its referent, or the retained authority
  // of a by-value borrowed-state carrier. A capture is the lambda body's
  // formal, so its places resolve through this loan as a binding's do through
  // its binding loan.
  [[nodiscard]] MirLoanId captureEntryLoan(SymbolId symbol) const {
    for (const MirLoanKind kind : {MirLoanKind::Parameter, MirLoanKind::Stored})
      for (const MirLoan &loan : output.loans) {
        const MirPlace *origin = output.findPlace(loan.source);
        if (loan.entry && loan.kind == kind && origin &&
            origin->root == MirPlaceRootKind::Symbol && origin->capture != 0 &&
            origin->symbol == symbol)
          return loan.id;
      }
    return 0;
  }

  // A lambda body's entry loan for one by-reference capture.
  [[nodiscard]] bool capturedReferenceEntry(const MirLoan &loan) const {
    const MirPlace *slot = output.findPlace(loan.source);
    return loan.entry && loan.kind == MirLoanKind::Parameter &&
           loan.parent == 0 && slot != nullptr &&
           slot->root == MirPlaceRootKind::Symbol && slot->capture != 0 &&
           slot->projections.empty() &&
           slot->type.kind == SemanticType::Reference;
  }

  // Whether `loan` is a local physical borrow below one by-reference
  // capture's entry loan: a chain of active, non-escaping accessor-result or
  // local links that reaches that entry and grants `access`.
  [[nodiscard]] bool capturedEntryDescendant(MirLoanId loan, MirLoanId entry,
                                             AccessMode access) const {
    std::unordered_set<MirLoanId> visited;
    MirLoanId link = loan;
    while (link != entry) {
      const MirLoan *record = output.findLoan(link);
      if (record == nullptr || !visited.insert(link).second || record->entry ||
          record->escapes ||
          (record->kind != MirLoanKind::CallResult &&
           record->kind != MirLoanKind::Local) ||
          !loanIsActive(scopes, link) ||
          (access == AccessMode::Mutable &&
           record->access != AccessMode::Mutable))
        return false;
      link = record->parent;
    }
    return loan != entry && loan != 0;
  }

  // The exact place of a value read through a by-reference capture whose
  // entry loan names `slot`: the capture slot dereferenced.
  [[nodiscard]] MirPlaceId captureReferentPlace(HirValueId id,
                                                MirPlaceId slotId) {
    const MirPlace *slot = output.findPlace(slotId);
    const MirLoan *entry = output.findLoan(loanForValue(id));
    if (slot == nullptr || entry == nullptr || entry->source != slotId ||
        !entry->entry || entry->kind != MirLoanKind::Parameter ||
        slot->root != MirPlaceRootKind::Symbol || slot->capture == 0 ||
        !slot->projections.empty() ||
        slot->type.kind != SemanticType::Reference)
      return 0;
    const MirPlaceId placeId = placeForValue(id);
    const MirPlace *place = output.findPlace(placeId);
    return place != nullptr && place->root == MirPlaceRootKind::Symbol &&
                   place->symbol == slot->symbol &&
                   place->capture == slot->capture &&
                   !place->projections.empty() &&
                   place->projections.front().kind ==
                       MirProjectionKind::Dereference
               ? placeId
               : 0;
  }

  [[nodiscard]] MirLoanId loanForValue(HirValueId id) const {
    if (const auto direct = valueLoans.find(id); direct != valueLoans.end()) {
      return direct->second;
    }
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      return 0;
    }
    if (value->kind == HirValueKind::Variable ||
        value->kind == HirValueKind::QualifiedName) {
      const auto local = localSymbols.find(value->symbol);
      if (local != localSymbols.end()) {
        const auto loan = bindingLoans.find(local->second);
        return loan == bindingLoans.end() ? 0 : loan->second;
      }
      if (const MirLoanId captured = captureEntryLoan(value->symbol))
        return captured;
      return 0;
    }
    if ((value->kind == HirValueKind::Grouping ||
         value->kind == HirValueKind::Move) &&
        !value->operands.empty()) {
      return loanForValue(value->operands.front());
    }
    if (value->kind == HirValueKind::Binary &&
        value->operation == TokenKind::COMMA && !value->operands.empty()) {
      return loanForValue(value->operands.back());
    }
    if (value->kind == HirValueKind::MemberAccess &&
        (value->storedReferenceAccess ||
         value->info.traits.containsBorrowedState) &&
        !value->operands.empty()) {
      return loanForValue(value->operands.front());
    }
    return 0;
  }

  // A binding-rooted place behind a stored reference field of its carrier
  // (`holder.box`, `teller.account.balance`) lies in that field's external
  // referent, as a `this` field place does. Returns the weakest access of the
  // stored references it crosses, or nothing when it crosses none. A carrier
  // alias's own access never reaches through a read-only stored reference.
  [[nodiscard]] std::optional<AccessMode>
  storedReferenceAccessFor(const MirPlace &place) const {
    // The root object is a binding, or the referent a call returned by
    // reference (a loan-rooted place selects within that referent).
    SemanticType type;
    if (place.root == MirPlaceRootKind::Binding) {
      const HirBinding *binding = findBinding(place.binding);
      if (binding == nullptr)
        return std::nullopt;
      type = binding->info.type;
    } else if (place.root == MirPlaceRootKind::Loan) {
      const MirLoan *loan = output.findLoan(place.loan);
      const HirValue *referent =
          loan != nullptr && loan->kind == MirLoanKind::CallResult
              ? findValue(loan->producedBy)
              : nullptr;
      if (referent == nullptr ||
          referent->info.category != ValueCategory::Place)
        return std::nullopt;
      type = referent->info.type;
    } else {
      return std::nullopt;
    }
    bool storedReference = false;
    std::optional<AccessMode> crossed;
    for (const auto &projection : place.projections) {
      if (projection.kind == MirProjectionKind::Dereference) {
        if (type.kind != SemanticType::Reference || type.arguments.size() != 1)
          return std::nullopt;
        if (storedReference)
          crossed = !crossed || *crossed == AccessMode::Mutable
                        ? type.referenceAccess
                        : AccessMode::ReadOnly;
        // Copy the referent first: assigning a type from its own argument
        // would read storage that the assignment is replacing.
        SemanticType referent = type.arguments.front();
        type = std::move(referent);
        storedReference = false;
        continue;
      }
      if (projection.kind != MirProjectionKind::Field)
        return std::nullopt;
      const auto field = borrowedFieldType(type, projection.field);
      if (!field)
        return std::nullopt;
      type = *field;
      storedReference = type.kind == SemanticType::Reference;
    }
    return crossed;
  }

  // The text field place a by-value text argument reads through a reference
  // formal, when `value` relays that argument as its text result.
  [[nodiscard]] MirPlaceId formalTextFieldPlace(const HirValue &value,
                                                HirValueId argument) {
    const auto text = [](const SemanticType &type) {
      return type.kind == SemanticType::StringView ||
             type.kind == SemanticType::CString;
    };
    if (!text(value.info.type) || value.info.category != ValueCategory::Value ||
        !value.borrowFields.empty())
      return 0;
    // Only an argument read from an already lowered place qualifies: never
    // materialize a place here, which an unused place would leave behind.
    const MirLoan *loan = output.findLoan(loanForValue(argument));
    if (loan == nullptr || loan->kind != MirLoanKind::Parameter || !loan->entry)
      return 0;
    const auto found = valuePlaces.find(argument);
    const MirPlaceId id = found == valuePlaces.end() ? 0 : found->second;
    const MirPlace *place = output.findPlace(id);
    if (place == nullptr || !text(place->type) ||
        place->root != MirPlaceRootKind::Binding ||
        place->projections.empty() ||
        place->projections.back().kind != MirProjectionKind::Field)
      return 0;
    return id;
  }

  [[nodiscard]] MirPlaceId borrowedSourcePlace(HirValueId id) const {
    const MirLoanId loan = loanForValue(id);
    const MirLoan *resolved = output.findLoan(loan);
    return resolved == nullptr ? 0 : resolved->source;
  }

  // Whether class `derived` has `base` among its class and interface bases.
  [[nodiscard]] bool hasBase(const SemanticType &derived,
                             const SemanticType &base,
                             std::size_t depth = 0) const {
    if (depth > 64 || derived.kind != SemanticType::Class)
      return false;
    for (const HirClassInstance &instance : program.classInstances())
      if (instance.type == derived)
        for (const HirBaseInstance &candidate : instance.bases)
          if (candidate.type == base ||
              hasBase(candidate.type, base, depth + 1))
            return true;
    return false;
  }

  [[nodiscard]] MirOperand argumentOperand(HirValueId id,
                                           const SemanticType &parameter) {
    if (parameter.kind != SemanticType::Reference) {
      return valueOperand(id);
    }
    emitPlaceDependencies(id, true);
    if (terminated()) {
      return {};
    }
    if (const MirLoanId existing = loanForValue(id); existing != 0) {
      // A borrow-carrying value (for example a read-only stored-reference
      // iterator) already has an owner loan, but a reference parameter needs
      // the runtime address of the carrier object rather than the address of
      // that carrier's referent.  Retain the owner loan as provenance and,
      // when the HIR place is an exact referent-typed place, carry that place
      // alongside it for backend publication.
      MirPlaceId runtimePlace = placeForValue(id);
      const MirPlace *resolved = output.findPlace(runtimePlace);
      // A reference of a base or interface type to a borrowed carrier
      // designates the same carrier object.
      const bool upcastCarrier =
          parameter.arguments.size() == 1 && resolved != nullptr &&
          resolved->type != parameter.arguments.front() &&
          resolved->traits.containsBorrowedState &&
          hasBase(resolved->type, parameter.arguments.front());
      if (!upcastCarrier &&
          (parameter.arguments.size() != 1 || resolved == nullptr ||
           resolved->type != parameter.arguments.front())) {
        runtimePlace = 0;
      }
      if (const MirLoan *loan = output.findLoan(existing);
          loan != nullptr && loan->kind == MirLoanKind::Stored &&
          runtimePlace != 0 && resolved->traits.containsBorrowedState) {
        // Pass the shell's checked address. Its binding retains the separate
        // Stored authority; passing that loan as the reference itself would
        // substitute the external referent for the source object.
        return {.kind = parameter.referenceAccess == AccessMode::Mutable
                            ? MirOperandKind::BorrowWrite
                            : MirOperandKind::BorrowRead,
                .place = runtimePlace,
                .type = parameter};
      }
      return {.kind = MirOperandKind::Loan,
              .place = upcastCarrier ? 0 : runtimePlace,
              .loan = existing,
              .type = parameter};
    }
    const MirPlaceId place = placeForValue(id);
    return {.kind = parameter.referenceAccess == AccessMode::Mutable
                        ? MirOperandKind::BorrowWrite
                        : MirOperandKind::BorrowRead,
            .place = place,
            .type = parameter};
  }

  [[nodiscard]] MirOperand copyArgumentOperand(HirValueId id,
                                               const SemanticType &parameter) {
    if (const HirValue *assignment = findValue(id);
        assignment != nullptr && isAssignmentExpression(assignment->kind))
      return assignmentResultCopyOperand(*assignment, parameter);
    emitPlaceDependencies(id, true);
    if (terminated()) {
      return {};
    }
    const HirValue *value = findValue(id);
    MirPlaceId place = placeForValue(id);
    const MirPlace *resolved = output.findPlace(place);
    if (value == nullptr || resolved == nullptr) {
      valid = false;
      return {.kind = MirOperandKind::Copy, .type = parameter};
    }
    if (resolved->sourceValue != id) {
      place = clonePlace(place, *value);
    }
    if (place == 0) {
      valid = false;
    }
    return {.kind = MirOperandKind::Copy, .place = place, .type = parameter};
  }

  [[nodiscard]] std::vector<HirValueId>
  callArgumentValues(const HirValue &value) const {
    if (value.kind == HirValueKind::IndexSet && value.functionTarget &&
        value.operands.size() >= 3 && value.parameterTypes.size() == 1) {
      // An overloaded subscript assignment carries object, index, and stored
      // value as HIR operands. Only the index belongs to operator[]; the value
      // is committed through the returned mutable loan after the call
      // succeeds.
      return {value.operands[1]};
    }
    if (value.callPlan) {
      std::vector<HirValueId> result;
      result.reserve(value.callPlan->arguments.size());
      for (const HirCallArgument &argument : value.callPlan->arguments) {
        result.push_back(argument.value);
      }
      return result;
    }
    std::size_t argumentCount = value.parameterTypes.size();
    if (value.kind == HirValueKind::Call) {
      if (const auto *call = exprAs<Call>(value.source)) {
        argumentCount =
            call->arguments().size() + value.defaultArguments.size();
      }
    }
    if (argumentCount > value.operands.size()) {
      return value.operands;
    }
    return std::vector<HirValueId>(
        value.operands.end() - static_cast<std::ptrdiff_t>(argumentCount),
        value.operands.end());
  }

  [[nodiscard]] std::optional<HirValueId>
  receiverValue(const HirValue &value) const {
    if (value.kind == HirValueKind::Call && value.receiver) {
      return value.receiver;
    }
    if (value.functionTarget && value.kind != HirValueKind::Call &&
        !value.operands.empty()) {
      return value.operands.front();
    }
    return std::nullopt;
  }

  [[nodiscard]] AccessMode
  receiverAccess(HirFunctionInstanceId targetId) const {
    if (targetId == 0) {
      return AccessMode::ReadOnly;
    }
    const HirFunctionInstance *target = program.findFunctionInstance(targetId);
    return target != nullptr && target->source != nullptr &&
                   receiverAllowsMutation(target->source->receiverMutability())
               ? AccessMode::Mutable
               : AccessMode::ReadOnly;
  }

  [[nodiscard]] AccessMode receiverAccess(const HirValue &value) const {
    if (value.callableInvocation)
      return *value.callableInvocation == CallableInvocationCapability::Read
                 ? AccessMode::ReadOnly
                 : AccessMode::Mutable;
    return value.functionTarget ? receiverAccess(*value.functionTarget)
                                : AccessMode::ReadOnly;
  }

  // The exact slot of a materialized temporary whose obligation is still
  // active, including a join's existing hidden slot, or zero.
  [[nodiscard]] MirPlaceId temporarySlot(HirValueId id) {
    const HirValue *temporary = findValue(id);
    const MirDropObligationId obligation =
        temporary == nullptr || temporary->info.category != ValueCategory::Value
            ? 0
            : dropObligationForValue(id);
    const MirDropObligation *drop =
        obligation == 0 ? nullptr : output.findDropObligation(obligation);
    const MirPlace *slot =
        drop == nullptr ? nullptr : output.findPlace(drop->place);
    if (drop == nullptr || !temporaryIsActive(obligation) ||
        drop->kind != MirDropObligationKind::Value || drop->value != id ||
        slot == nullptr ||
        (slot->root != MirPlaceRootKind::Value &&
         !(slot->root == MirPlaceRootKind::Temporary &&
           findMirTemporaryMaterialization(output, *slot, id))) ||
        !slot->projections.empty()) {
      return 0;
    }
    return slot->id;
  }

  [[nodiscard]] MirOperand
  temporaryBorrowOperand(HirValueId id, AccessMode access,
                         const SemanticType &stagedType,
                         bool allowBorrowedCarrier = false) {
    const HirValue *temporary = findValue(id);
    // Semantics admits fresh class, struct and payload-enum values here.
    if (temporary == nullptr ||
        (temporary->info.type.kind != SemanticType::Class &&
         temporary->info.type.kind != SemanticType::Enum &&
         temporary->info.type.kind != SemanticType::Lambda &&
         temporary->info.type.kind != SemanticType::FunctionItem) ||
        temporary->info.category != ValueCategory::Value ||
        (temporary->info.traits.containsBorrowedState &&
         !allowBorrowedCarrier &&
         temporary->info.type.kind != SemanticType::Class)) {
      valid = false;
      return {};
    }

    const MirOperand produced = valueOperand(id);
    if (produced.kind != MirOperandKind::Value || produced.value == 0) {
      valid = false;
      return {};
    }

    MirPlaceId place = 0;
    const MirDropObligationId obligation = dropObligationForValue(id);
    if (obligation != 0) {
      const MirDropObligation *drop = output.findDropObligation(obligation);
      if (drop == nullptr || !temporaryIsActive(obligation) ||
          drop->kind != MirDropObligationKind::Value || drop->value != id) {
        valid = false;
        return {};
      }
      place = drop->place;
    } else {
      const auto existing = valuePlaces.find(id);
      if (existing != valuePlaces.end()) {
        place = existing->second;
      } else {
        place = appendPlace({.root = MirPlaceRootKind::Value,
                             .value = produced.value,
                             .type = temporary->info.type,
                             .access = AccessMode::Mutable,
                             .traits = temporary->info.traits,
                             .sourceValue = temporary->id});
        valuePlaces.insert_or_assign(id, place);
      }
    }
    const MirPlace *materialized = output.findPlace(place);
    // A conditional result's lifetime is rooted in its join temporary, and an
    // assignment result's in the temporary holding its copy. The final Move
    // is a transport identity, not a second materialization.
    const bool conditionalResult =
        (temporary->kind == HirValueKind::Conditional ||
         temporary->kind == HirValueKind::ScopedResult ||
         isAssignmentExpression(temporary->kind)) &&
        materialized != nullptr &&
        materialized->root == MirPlaceRootKind::Temporary &&
        std::any_of(
            output.blocks.begin(), output.blocks.end(),
            [&](const MirBlock &block) {
              return std::any_of(
                  block.instructions.begin(), block.instructions.end(),
                  [&](const MirInstruction &instruction) {
                    return (instruction.kind == MirInstructionKind::Move ||
                            instruction.kind == MirInstructionKind::Load) &&
                           instruction.result == produced.value &&
                           instruction.operands.size() == 1 &&
                           instruction.operands.front().place == place &&
                           instruction.info.type == temporary->info.type &&
                           instruction.lifecycle.empty();
                  });
            });
    if (materialized == nullptr ||
        (!conditionalResult && (materialized->root != MirPlaceRootKind::Value ||
                                materialized->value != produced.value)) ||
        materialized->type != temporary->info.type ||
        materialized->access != AccessMode::Mutable ||
        materialized->sourceValue != temporary->id ||
        !materialized->projections.empty() ||
        (temporary->info.traits.containsBorrowedState &&
         !findMirTemporaryCarrierDependency(output, *materialized))) {
      valid = false;
      return {};
    }
    return {.kind = access == AccessMode::Mutable ? MirOperandKind::BorrowWrite
                                                  : MirOperandKind::BorrowRead,
            .place = place,
            .type = stagedType};
  }

  [[nodiscard]] MirOperand receiverOperand(HirValueId id, AccessMode access) {
    const HirValue *receiver = findValue(id);
    if (receiver == nullptr) {
      valid = false;
      return {};
    }
    // A raw-pointer method receiver (`pointer->method()`) is the pointee
    // itself. A field reached through the pointer (`pointer->field.method()`)
    // is an ordinary place below that pointee and keeps its projections.
    if (receiver->kind == HirValueKind::MemberAccess &&
        receiver->unsafeOperation == UnsafeOperationKind::RawMember &&
        receiver->info.category != ValueCategory::Place &&
        !receiver->operands.empty()) {
      const HirValueId pointerId = receiver->operands.front();
      const HirValue *pointer = findValue(pointerId);
      if (pointer == nullptr ||
          pointer->info.type.kind != SemanticType::RawPointer ||
          pointer->info.type.arguments.size() != 1) {
        valid = false;
        return {};
      }
      const MirValueId pointerValue = operandValue(pointerId);
      if (terminated()) {
        return {};
      }
      const SemanticType pointee = pointer->info.type.arguments.front();
      const AccessMode pointeeAccess = pointer->info.type.pointerAccess;
      const MirPlaceId place = appendPlace({.root = MirPlaceRootKind::Value,
                                            .value = pointerValue,
                                            .type = pointee,
                                            .access = pointeeAccess,
                                            .traits = semanticTraits(pointee),
                                            .sourceValue = receiver->id});
      output.places[place - 1].projections.push_back(
          {.kind = MirProjectionKind::RawDereference});
      return {.kind = access == AccessMode::Mutable
                          ? MirOperandKind::BorrowWrite
                          : MirOperandKind::BorrowRead,
              .place = place,
              .type = pointee};
    }
    if (receiver->info.category == ValueCategory::Place) {
      emitPlaceDependencies(id, true);
      if (terminated()) {
        return {};
      }
      const MirPlaceId place = placeForValue(id);
      if (place != 0) {
        return {.kind = access == AccessMode::Mutable
                            ? MirOperandKind::BorrowWrite
                            : MirOperandKind::BorrowRead,
                .place = place,
                .type = receiver->info.type};
      }
    }
    return valueOperand(id);
  }

  [[nodiscard]] static MirOperation binaryOperation(TokenKind operation) {
    switch (operation) {
    case TokenKind::COMMA:
      return MirOperation::Comma;
    case TokenKind::PLUS:
      return MirOperation::Add;
    case TokenKind::MINUS:
      return MirOperation::Subtract;
    case TokenKind::STAR:
      return MirOperation::Multiply;
    case TokenKind::SLASH:
      return MirOperation::Divide;
    case TokenKind::PERCENT:
      return MirOperation::Remainder;
    case TokenKind::AMPERSAND:
      return MirOperation::BitwiseAnd;
    case TokenKind::PIPE:
      return MirOperation::BitwiseOr;
    case TokenKind::CARET:
      return MirOperation::BitwiseXor;
    case TokenKind::SHIFT_LEFT:
      return MirOperation::ShiftLeft;
    case TokenKind::SHIFT_RIGHT:
      return MirOperation::ShiftRight;
    case TokenKind::EQUAL_EQUAL:
      return MirOperation::Equal;
    case TokenKind::BANG_EQUAL:
      return MirOperation::NotEqual;
    case TokenKind::LESS:
      return MirOperation::Less;
    case TokenKind::LESS_EQUAL:
      return MirOperation::LessEqual;
    case TokenKind::GREATER:
      return MirOperation::Greater;
    case TokenKind::GREATER_EQUAL:
      return MirOperation::GreaterEqual;
    default:
      return MirOperation::None;
    }
  }

  [[nodiscard]] static MirOperation unaryOperation(TokenKind operation) {
    switch (operation) {
    case TokenKind::PLUS:
      return MirOperation::Positive;
    case TokenKind::MINUS:
      return MirOperation::Negate;
    case TokenKind::BANG:
      return MirOperation::LogicalNot;
    case TokenKind::TILDE:
      return MirOperation::BitwiseNot;
    default:
      return MirOperation::None;
    }
  }

  [[nodiscard]] static MirOperation assignmentOperation(TokenKind operation) {
    switch (operation) {
    case TokenKind::EQUAL:
      return MirOperation::Assign;
    case TokenKind::PLUS_EQUAL:
      return MirOperation::AddAssign;
    case TokenKind::MINUS_EQUAL:
      return MirOperation::SubtractAssign;
    case TokenKind::STAR_EQUAL:
      return MirOperation::MultiplyAssign;
    case TokenKind::SLASH_EQUAL:
      return MirOperation::DivideAssign;
    case TokenKind::PERCENT_EQUAL:
      return MirOperation::RemainderAssign;
    case TokenKind::AMPERSAND_EQUAL:
      return MirOperation::BitwiseAndAssign;
    case TokenKind::PIPE_EQUAL:
      return MirOperation::BitwiseOrAssign;
    case TokenKind::CARET_EQUAL:
      return MirOperation::BitwiseXorAssign;
    case TokenKind::SHIFT_LEFT_EQUAL:
      return MirOperation::ShiftLeftAssign;
    case TokenKind::SHIFT_RIGHT_EQUAL:
      return MirOperation::ShiftRightAssign;
    default:
      return MirOperation::None;
    }
  }

  [[nodiscard]] static bool plainOverloadedBorrowStore(const HirValue &value) {
    return value.operation &&
           assignmentOperation(*value.operation) == MirOperation::Assign;
  }

  [[nodiscard]] bool stableReservationOrigin(MirPlaceId id) const {
    std::unordered_set<MirPlaceId> visited;
    while (visited.insert(id).second) {
      const auto *place = output.findPlace(id);
      if (place == nullptr)
        return false;
      MirLoanId through =
          place->root == MirPlaceRootKind::Loan ? place->loan : 0;
      if (place->root == MirPlaceRootKind::Binding &&
          !place->projections.empty() &&
          place->projections.front().kind == MirProjectionKind::Dereference) {
        const auto found = bindingLoans.find(place->binding);
        if (found != bindingLoans.end())
          through = found->second;
      }
      if (through != 0) {
        const auto *loan = output.findLoan(through);
        if (loan == nullptr)
          return false;
        if (loan->source != id) {
          id = loan->source;
          continue;
        }
      }
      return place->root != MirPlaceRootKind::Symbol &&
             place->root != MirPlaceRootKind::Temporary &&
             place->root != MirPlaceRootKind::Value;
    }
    return false;
  }

  [[nodiscard]] static MirOperation
  orderedCompoundArithmetic(MirOperation assignment) {
    switch (assignment) {
    case MirOperation::AddAssign:
      return MirOperation::Add;
    case MirOperation::SubtractAssign:
      return MirOperation::Subtract;
    case MirOperation::MultiplyAssign:
      return MirOperation::Multiply;
    case MirOperation::DivideAssign:
      return MirOperation::Divide;
    case MirOperation::RemainderAssign:
      return MirOperation::Remainder;
    case MirOperation::BitwiseAndAssign:
      return MirOperation::BitwiseAnd;
    case MirOperation::BitwiseOrAssign:
      return MirOperation::BitwiseOr;
    case MirOperation::BitwiseXorAssign:
      return MirOperation::BitwiseXor;
    case MirOperation::ShiftLeftAssign:
      return MirOperation::ShiftLeft;
    case MirOperation::ShiftRightAssign:
      return MirOperation::ShiftRight;
    default:
      return MirOperation::None;
    }
  }

  [[nodiscard]] static MirOperation modifierOperation(const HirValue &value) {
    if (!value.operation) {
      return MirOperation::None;
    }
    switch (*value.operation) {
    case TokenKind::PLUS_PLUS:
      return value.kind == HirValueKind::Postfix ? MirOperation::PostIncrement
                                                 : MirOperation::PreIncrement;
    case TokenKind::MINUS_MINUS:
      return value.kind == HirValueKind::Postfix ? MirOperation::PostDecrement
                                                 : MirOperation::PreDecrement;
    default:
      return MirOperation::None;
    }
  }

  [[nodiscard]] static MirOperation computeOperation(const HirValue &value) {
    if (value.kind == HirValueKind::Unary &&
        value.operation == TokenKind::AMPERSAND) {
      return MirOperation::AddressOf;
    }
    if (value.unsafeOperation == UnsafeOperationKind::PointerArithmetic &&
        value.kind == HirValueKind::Binary) {
      if (value.info.type.kind != SemanticType::RawPointer) {
        return MirOperation::PointerDifference;
      }
      return value.operation == TokenKind::MINUS ? MirOperation::PointerSubtract
                                                 : MirOperation::PointerAdd;
    }
    switch (value.kind) {
    case HirValueKind::ArrayInitializer:
      return MirOperation::Aggregate;
    case HirValueKind::Binary:
      return value.operation ? binaryOperation(*value.operation)
                             : MirOperation::None;
    case HirValueKind::Conversion:
      return MirOperation::Convert;
    case HirValueKind::Grouping:
      return MirOperation::Identity;
    case HirValueKind::Index:
      return MirOperation::Index;
    case HirValueKind::Lambda:
      return MirOperation::Closure;
    case HirValueKind::NativeCallback:
      return MirOperation::NativeCallback;
    case HirValueKind::FunctionItem:
      return MirOperation::FunctionItem;
    case HirValueKind::LayoutQuery:
      return MirOperation::Literal;
    case HirValueKind::Literal:
      return MirOperation::Literal;
    case HirValueKind::PackFold:
    case HirValueKind::PackElement:
      return MirOperation::None;
    case HirValueKind::PackExpansion:
      // Calls lower concrete expansions directly into projected element
      // places and ordered CallInput instructions. No pack SSA operation
      // survives into MIR.
      return MirOperation::None;
    case HirValueKind::PayloadConstruction:
      return MirOperation::PayloadConstruct;
    case HirValueKind::PayloadExtraction:
      return MirOperation::PayloadExtract;
    case HirValueKind::QualifiedName:
      return value.enumOwner && value.enumValue ? MirOperation::EnumConstant
                                                : MirOperation::None;
    case HirValueKind::Unary:
      return value.operation ? unaryOperation(*value.operation)
                             : MirOperation::None;
    default:
      return MirOperation::None;
    }
  }

  [[nodiscard]] MirPlaceId canonicalBorrowSource(MirPlaceId placeId) {
    const auto resolve = [&](const auto &self, MirPlaceId current,
                             std::size_t depth) -> MirPlaceId {
      const MirPlace *place = output.findPlace(current);
      if (place == nullptr || depth > output.loans.size() + 1) {
        return 0;
      }

      const MirLoan *loan = nullptr;
      std::size_t projectionOffset = 0;
      if (place->root == MirPlaceRootKind::Loan) {
        loan = output.findLoan(place->loan);
      } else if (place->root == MirPlaceRootKind::Binding &&
                 !place->projections.empty() &&
                 place->projections.front().kind ==
                     MirProjectionKind::Dereference) {
        const HirBinding *binding = findBinding(place->binding);
        const auto semantic =
            binding == nullptr || binding->info.retainedLoan == 0
                ? semanticLoans.end()
                : semanticLoans.find(binding->info.retainedLoan);
        const auto found = bindingLoans.find(place->binding);
        loan = semantic != semanticLoans.end()
                   ? output.findLoan(semantic->second)
                   : (found == bindingLoans.end()
                          ? nullptr
                          : output.findLoan(found->second));
        projectionOffset = loan == nullptr ? 0 : 1;
        if (referenceAccessorBindings.contains(place->binding)) {
          // The binding captured an exact returned/projected address while
          // retaining its coarser owner loan. Dereference and fields belong
          // to that referent, not to the protected enclosing owner or range.
          return current;
        }
      } else if (place->root == MirPlaceRootKind::Value) {
        const MirValue *value = output.findValue(place->value);
        const MirLoanId loanId =
            value == nullptr ? 0 : loanForValue(value->sourceValue);
        loan = output.findLoan(loanId);
      }
      if (loan == nullptr || loan->source == current) {
        return current;
      }
      if (loan->kind == MirLoanKind::CallResult &&
          projectionOffset < place->projections.size()) {
        // A call's dependency origin is not its returned referent. In
        // particular, operator-> borrows an owner but returns its pointee.
        // Keep projections on the returned address instead of appending them
        // to the owner place. The parent loan retains the dependency chain.
        return current;
      }

      const MirPlaceId baseId = self(self, loan->source, depth + 1);
      const MirPlace *base = output.findPlace(baseId);
      place = output.findPlace(current);
      if (base == nullptr || place == nullptr) {
        return 0;
      }
      if (projectionOffset == place->projections.size()) {
        return baseId;
      }
      MirPlace composed = *base;
      composed.id = 0;
      composed.type = place->type;
      composed.access = place->access;
      composed.traits = place->traits;
      composed.sourceValue = place->sourceValue;
      const auto remaining = place->projections.begin() +
                             static_cast<std::ptrdiff_t>(projectionOffset);
      if (base->type.kind == SemanticType::Reference &&
          remaining->kind != MirProjectionKind::Dereference) {
        // The loan is anchored at a reference carrier place (for example a
        // reference parameter's entry loan), not at a body-local referent
        // place, so re-rooting does not absorb the indirection. Spell the
        // dereference explicitly: composing a field or index projection
        // directly onto the reference-typed carrier would produce a place no
        // later stage can key to one exact concrete class instance.
        composed.projections.push_back(
            {.kind = MirProjectionKind::Dereference});
      }
      composed.projections.insert(composed.projections.end(), remaining,
                                  place->projections.end());
      return appendPlace(std::move(composed));
    };
    return resolve(resolve, placeId, 0);
  }

  [[nodiscard]] bool sameBorrowSource(MirPlaceId left, MirPlaceId right) {
    left = canonicalBorrowSource(left);
    right = canonicalBorrowSource(right);
    const MirPlace *lhs = output.findPlace(left);
    const MirPlace *rhs = output.findPlace(right);
    if (lhs == nullptr || rhs == nullptr || lhs->root != rhs->root ||
        lhs->binding != rhs->binding || lhs->symbol != rhs->symbol ||
        lhs->temporary != rhs->temporary || lhs->value != rhs->value ||
        lhs->loan != rhs->loan ||
        lhs->projections.size() != rhs->projections.size()) {
      return false;
    }
    return std::equal(lhs->projections.begin(), lhs->projections.end(),
                      rhs->projections.begin(),
                      [](const MirPlaceProjection &leftProjection,
                         const MirPlaceProjection &rightProjection) {
                        return leftProjection.kind == rightProjection.kind &&
                               leftProjection.field == rightProjection.field &&
                               leftProjection.index == rightProjection.index &&
                               leftProjection.constantIndex ==
                                   rightProjection.constantIndex &&
                               leftProjection.selection ==
                                   rightProjection.selection;
                      });
  }

  // A direct text field of the receiver holds the caller's text: ownerless,
  // or relayed under the method's field contract (L-OWN-02). A reference to
  // it needs no stored dependency in this body.
  [[nodiscard]] bool receiverTextFieldPlace(MirPlaceId id) const {
    const MirPlace *place = output.findPlace(id);
    return place != nullptr && place->root == MirPlaceRootKind::This &&
           (place->type.kind == SemanticType::StringView ||
            place->type.kind == SemanticType::CString) &&
           place->projections.size() == 1 &&
           place->projections.front().kind == MirProjectionKind::Field;
  }

  [[nodiscard]] MirLoanId storedDependencyForPlace(MirPlaceId id) const {
    const MirPlace *place = output.findPlace(id);
    if (place == nullptr)
      return 0;
    if (place->root == MirPlaceRootKind::Loan) {
      const auto *loan = output.findLoan(place->loan);
      return loan == nullptr ? 0 : loan->storedDependency;
    }
    if (place->root == MirPlaceRootKind::Value ||
        place->root == MirPlaceRootKind::Temporary) {
      const auto temporary = findMirTemporaryCarrierDependency(output, *place);
      return temporary ? temporary.loan->id : 0;
    }
    MirLoanId result = 0;
    for (const auto &loan : output.loans) {
      const auto *source = output.findPlace(loan.source);
      const bool exactAbstract =
          loan.entry && loan.kind == MirLoanKind::Stored && source != nullptr &&
          source->root == place->root && source->binding == place->binding &&
          source->symbol == place->symbol &&
          source->capture == place->capture && source->projections.empty() &&
          (source->root == MirPlaceRootKind::This ||
           (source->root == MirPlaceRootKind::Symbol && source->capture != 0) ||
           source->type.kind == SemanticType::Reference);
      const bool carried =
          place->root == MirPlaceRootKind::Binding &&
          (std::find(loan.carriers.begin(), loan.carriers.end(),
                     place->binding) != loan.carriers.end() ||
           (loan.kind == MirLoanKind::Stored && loan.lifecycleBound &&
            mir_detail::bindingReceivedStoredLoan(output, place->binding,
                                                  loan.id)));
      if (!exactAbstract && !carried)
        continue;
      const MirLoanId dependency =
          loan.kind == MirLoanKind::Stored ? loan.id : loan.storedDependency;
      if (dependency == 0)
        continue;
      if (result != 0 && result != dependency)
        return 0;
      result = dependency;
    }
    return result;
  }

  [[nodiscard]] std::optional<SemanticType>
  borrowedFieldType(const SemanticType &type, SymbolId field,
                    std::optional<std::size_t> variant = std::nullopt) const {
    if (type.kind == SemanticType::Enum) {
      // An abstract entry path describes a possible active alternative, not
      // permission to search every alternative for a convenient field.
      if (!variant)
        return std::nullopt;
      for (const auto &owner : program.enumDeclarations())
        if (owner.type == type && owner.payload)
          for (const auto &alternative : owner.enumerators)
            if (alternative.variantIndex == *variant &&
                alternative.payloadTypes.size() ==
                    alternative.payloadSymbols.size())
              for (std::size_t index = 0;
                   index < alternative.payloadSymbols.size(); ++index)
                if (alternative.payloadSymbols[index] == field)
                  return alternative.payloadTypes[index];
      return std::nullopt;
    }
    if (variant)
      return std::nullopt;
    if (type.kind == SemanticType::Lambda) {
      for (const auto &owner : program.lambdaInstances())
        if (owner.type == type)
          for (const auto &capture : owner.captures)
            if (capture.bindingSymbol == field)
              return capture.type;
      return std::nullopt;
    }
    for (const auto &owner : program.classInstances())
      if (owner.type == type)
        for (const auto &member : owner.fields)
          if (member.info.symbol == field)
            return member.info.type;
    return std::nullopt;
  }

  [[nodiscard]] MirPlaceId projectBorrowFields(
      MirPlaceId base, const std::vector<SymbolId> &fields,
      std::span<const BorrowDependencyCondition> conditions = {}) {
    const auto *origin = output.findPlace(base);
    if (origin == nullptr)
      return 0;
    MirPlace projected = *origin;
    projected.id = 0;
    // Preserve the semantic overlap identity while adding executable typed
    // projections. Reference-field dereference is transparent in that key;
    // losing it makes two mapped fields sharing an entry owner impossible to
    // reconcile with their semantic dependencies at local publication.
    const auto intern = [&]() {
      projected.traits = resolvedTraits(projected.type);
      for (const auto &place : output.places)
        if (place.root == projected.root &&
            place.binding == projected.binding &&
            place.symbol == projected.symbol && place.loan == projected.loan &&
            place.value == projected.value &&
            place.temporary == projected.temporary &&
            place.projections == projected.projections &&
            place.type == projected.type) {
          if (projected.key && !place.key)
            output.places[place.id - 1].key = projected.key;
          return place.id;
        }
      return appendPlace(projected);
    };
    const auto dereference = [&] {
      if (projected.type.kind == SemanticType::Reference &&
          projected.type.arguments.size() == 1) {
        projected.access = projected.type.referenceAccess;
        SemanticType referent = projected.type.arguments.front();
        projected.type = std::move(referent);
        projected.projections.push_back(
            {.kind = MirProjectionKind::Dereference});
      }
    };
    dereference();
    std::vector<SymbolId> prefix;
    std::size_t usedConditions = 0;
    for (const auto field : fields) {
      std::optional<std::size_t> variant;
      for (const auto &condition : conditions)
        if (condition.subobject == prefix) {
          if (variant || condition.enumeration != projected.type ||
              projected.type.kind != SemanticType::Enum)
            return 0;
          variant = condition.variant;
          ++usedConditions;
        }
      const auto member = borrowedFieldType(projected.type, field, variant);
      if (!member)
        return 0;
      projected.projections.push_back(
          {.kind = MirProjectionKind::Field, .field = field});
      if (projected.key)
        projected.key->projections.push_back(
            {.kind = PlaceProjectionKind::Field, .field = field});
      projected.type = *member;
      prefix.push_back(field);
      dereference();
      // Keep the typed prefix as well as the final referent. Independent
      // field-origin verification composes the carrier's stored reference
      // with the remainder of an accessor path without reconstructing types.
      (void)intern();
    }
    if (usedConditions != conditions.size())
      return 0;
    return intern();
  }

  // A stored loan whose designated fields are exactly its carriers' one
  // stored reference path, which its source's type is the referent of.
  [[nodiscard]] bool solePathLoan(const MirLoan &loan) const {
    if (loan.kind != MirLoanKind::Stored || loan.designatedFields.empty() ||
        loan.carriers.empty() || !loan.additionalDependencies.empty())
      return false;
    const auto *source = output.findPlace(loan.source);
    if (source == nullptr)
      return false;
    for (const auto carrier : loan.carriers) {
      const auto *binding = findBinding(carrier);
      const auto path =
          binding ? soleStoredReferencePath(binding->info.type) : std::nullopt;
      if (!path || path->first != loan.designatedFields ||
          path->second != source->type)
        return false;
    }
    return true;
  }

  // The one stored reference field path of a class without bases, through
  // its owned class fields, with that reference's referent type.
  [[nodiscard]] std::optional<std::pair<std::vector<SymbolId>, SemanticType>>
  soleStoredReferencePath(const SemanticType &type) const {
    std::vector<std::pair<std::vector<SymbolId>, SemanticType>> paths;
    std::vector<SymbolId> prefix;
    std::vector<SemanticType> visiting;
    bool exact = true;
    const auto visit = [&](const auto &self, const SemanticType &current) {
      if (std::find(visiting.begin(), visiting.end(), current) !=
          visiting.end()) {
        exact = false;
        return;
      }
      const auto instance = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const HirClassInstance &candidate) {
            return candidate.type == current;
          });
      if (instance == program.classInstances().end() ||
          !instance->bases.empty()) {
        exact = false;
        return;
      }
      visiting.push_back(current);
      for (const auto &field : instance->fields) {
        const SemanticType &fieldType = field.info.type;
        prefix.push_back(field.info.symbol);
        if (fieldType.kind == SemanticType::Reference &&
            fieldType.arguments.size() == 1)
          paths.emplace_back(prefix, fieldType.arguments.front());
        else if (fieldType.kind == SemanticType::Class &&
                 field.info.traits.containsBorrowedState)
          self(self, fieldType);
        else if (field.info.traits.containsBorrowedState)
          exact = false;
        prefix.pop_back();
      }
      visiting.pop_back();
    };
    visit(visit, type);
    if (!exact || paths.size() != 1)
      return std::nullopt;
    return paths.front();
  }

  [[nodiscard]] std::optional<std::size_t>
  storedReferencePrefix(SemanticType type,
                        const std::vector<SymbolId> &fields) const {
    if (type.kind == SemanticType::Reference && type.arguments.size() == 1)
      type = SemanticType(type.arguments.front());
    for (std::size_t index = 0; index < fields.size(); ++index) {
      const auto field = borrowedFieldType(type, fields[index]);
      if (!field)
        return std::nullopt;
      type = *field;
      if (type.kind == SemanticType::Reference)
        return index + 1;
    }
    return std::nullopt;
  }

  [[nodiscard]] bool borrowsCarrierStorage(const HirValue &value) const {
    if (value.info.category != ValueCategory::Place)
      return false;
    if (value.info.type.kind == SemanticType::StringView ||
        value.info.type.kind == SemanticType::CString ||
        ((value.info.type.kind == SemanticType::Class ||
          value.info.type.kind == SemanticType::Lambda) &&
         value.info.traits.containsBorrowedState))
      return true;
    // Erasure removes the visible referent layout, not the addressable
    // shell's lifetime or its inherited owners. Preserve the selected
    // borrowed carrier just as for a reference with a visible layout.
    if (value.info.type.kind == SemanticType::Class &&
        value.borrowOrigin == BorrowOriginKind::Argument &&
        value.borrowArgument < value.parameterTypes.size() &&
        value.parameterTypes[value.borrowArgument].kind ==
            SemanticType::Reference &&
        std::any_of(
            program.classInstances().begin(), program.classInstances().end(),
            [&](const HirClassInstance &owner) {
              return owner.type == value.info.type &&
                     owner.compilerCapability ==
                         CompilerCapabilityTypeKind::ErasedObject;
            })) {
      const auto arguments = callArgumentValues(value);
      const auto *input = value.borrowArgument < arguments.size()
                              ? findValue(arguments[value.borrowArgument])
                              : nullptr;
      if (input && input->info.traits.containsBorrowedState)
        return true;
    }
    // A base or interface view of a borrowed carrier hides the carrier's
    // stored references, not its addressable shell: a call relaying such a
    // reference argument borrows that carrier as one of its own type does.
    if (value.info.type.kind == SemanticType::Class &&
        value.borrowOrigin == BorrowOriginKind::Argument &&
        value.borrowArgument < value.parameterTypes.size() &&
        value.parameterTypes[value.borrowArgument].kind ==
            SemanticType::Reference) {
      const auto arguments = callArgumentValues(value);
      const auto *input = value.borrowArgument < arguments.size()
                              ? findValue(arguments[value.borrowArgument])
                              : nullptr;
      if (input && input->info.traits.containsBorrowedState &&
          hasBase(input->info.type, value.info.type))
        return true;
    }
    // A reference returned from an ordinary receiver borrows its storage
    // unless semantics selected an exact stored-reference field instead.
    // The referent need not itself contain a borrow (for example an int field
    // or an element in an owned allocation beside the resource dependency).
    const auto *target =
        value.functionTarget
            ? program.findFunctionInstance(*value.functionTarget)
            : nullptr;
    if (value.intrinsic != IntrinsicKind::None || target == nullptr ||
        target->returnType.kind != SemanticType::Reference ||
        value.borrowOrigin != BorrowOriginKind::Receiver ||
        !value.borrowFields.empty())
      return false;
    const auto receiver = receiverValue(value);
    const auto *source = receiver ? findValue(*receiver) : nullptr;
    const auto *retained =
        receiver ? output.findLoan(loanForValue(*receiver)) : nullptr;
    if (retained != nullptr && ((retained->kind == MirLoanKind::Local &&
                                 retained->storedDependency == 0) ||
                                ((retained->kind == MirLoanKind::Local ||
                                  retained->kind == MirLoanKind::CallResult) &&
                                 receiver && isRangeIteratorValue(*receiver))))
      return false; // An opaque range cursor carries its checked range loan.
    return source != nullptr && source->info.traits.containsBorrowedState;
  }

  [[nodiscard]] bool isRangeIteratorBinding(HirBindingId binding) const {
    return binding != 0 &&
           std::any_of(source.statements.begin(), source.statements.end(),
                       [&](const auto &statement) {
                         return statement.rangeIteration &&
                                statement.rangeIteration->iterator == binding;
                       });
  }

  [[nodiscard]] bool
  isPhysicalProxyRangeProtocolBinding(HirBindingId binding) const {
    return binding != 0 &&
           std::any_of(
               source.statements.begin(), source.statements.end(),
               [&](const auto &statement) {
                 const auto &range = statement.rangeIteration;
                 const auto *selection =
                     range ? findValue(range->elementAccessValue) : nullptr;
                 return range &&
                        range->elementType.kind == SemanticType::Class &&
                        resolvedTraits(range->elementType)
                            .containsBorrowedState &&
                        selection != nullptr &&
                        selection->info.category == ValueCategory::Place &&
                        selection->borrowOrigin == BorrowOriginKind::Receiver &&
                        selection->borrowFields.empty() &&
                        (range->iterator == binding ||
                         range->element == binding ||
                         range->elementOwner == binding);
               });
  }

  [[nodiscard]] bool exactReadonlyRangeElementAlias(HirBindingId binding,
                                                    const HirValue &initializer,
                                                    const MirLoan &loan) const {
    if (initializer.kind != HirValueKind::Variable ||
        loan.kind != MirLoanKind::CallResult || loan.entry || loan.escapes ||
        loan.access != AccessMode::ReadOnly || !loanIsActive(scopes, loan.id))
      return false;
    for (const auto &statement : source.statements) {
      if (!statement.rangeIteration)
        continue;
      const auto &range = *statement.rangeIteration;
      const auto *staging = findBinding(range.elementOwner);
      const auto stagedLoan = bindingLoans.find(range.elementOwner);
      const auto cursorLoan = bindingLoans.find(range.iterator);
      const auto parent = semanticLoans.find(range.rangeLoan);
      const auto *cursor = cursorLoan == bindingLoans.end()
                               ? nullptr
                               : output.findLoan(cursorLoan->second);
      if (!range.materializesElementValue && range.element == binding &&
          range.elementOwner != 0 && staging &&
          staging->info.symbol == initializer.symbol &&
          stagedLoan != bindingLoans.end() && stagedLoan->second == loan.id &&
          loan.producedBy == range.elementAccessValue && cursor &&
          loan.storedDependency == cursor->id &&
          cursor->kind == MirLoanKind::Stored &&
          parent != semanticLoans.end() && cursor->parent == parent->second &&
          loanIsActive(scopes, cursor->id) &&
          std::find(loan.carriers.begin(), loan.carriers.end(),
                    range.elementOwner) != loan.carriers.end())
        return true;
    }
    return false;
  }

  [[nodiscard]] bool isRangeIteratorValue(HirValueId value) const {
    const auto *input = findValue(value);
    const auto binding = input == nullptr ? localSymbols.end()
                                          : localSymbols.find(input->symbol);
    return binding != localSymbols.end() &&
           isRangeIteratorBinding(binding->second);
  }

  [[nodiscard]] bool storesCarrierReference(const HirValue &value) const {
    if (value.intrinsic != IntrinsicKind::None ||
        (!value.functionTarget && !value.constructorTarget) ||
        value.info.category != ValueCategory::Value ||
        value.info.type.kind != SemanticType::Class ||
        !value.info.traits.containsBorrowedState ||
        !value.borrowArguments.empty())
      return false;
    const bool receiverResult =
        value.borrowOrigin == BorrowOriginKind::Receiver;
    const auto arguments = callArgumentValues(value);
    const auto selected =
        receiverResult ? receiverValue(value)
        : value.borrowOrigin == BorrowOriginKind::Argument &&
                value.borrowArgument < arguments.size()
            ? std::optional<HirValueId>{arguments[value.borrowArgument]}
            : std::nullopt;
    const auto *input = selected ? findValue(*selected) : nullptr;
    if (input == nullptr ||
        (!input->info.traits.containsBorrowedState &&
         input->info.type.kind != SemanticType::StringView &&
         input->info.type.kind != SemanticType::CString))
      return false;
    if (receiverResult
            ? !value.borrowFields.empty()
            : value.borrowArgument >= value.parameterTypes.size() ||
                  value.parameterTypes[value.borrowArgument].kind !=
                      SemanticType::Reference ||
                  value.borrowFields.size() > 1 ||
                  (!value.borrowFields.empty() &&
                   (!value.borrowFields.front().sourceFields.empty() ||
                    value.borrowFields.front().origin !=
                        BorrowOriginKind::Argument ||
                    value.borrowFields.front().parameter !=
                        value.borrowArgument)))
      return false;
    // The compact receiver contract represents one stored reference. A
    // multi-origin carrier instead forwards its existing external dependencies
    // (for example a subview of two independent buffers), not its own shell.
    std::vector<SemanticType> visiting;
    std::optional<SemanticType> reference;
    const auto references = [&](const auto &self,
                                const SemanticType &type) -> unsigned {
      if (type.kind == SemanticType::Reference) {
        reference = type;
        return 1;
      }
      if (std::find(visiting.begin(), visiting.end(), type) != visiting.end())
        return 2;
      const auto owner = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const auto &candidate) { return candidate.type == type; });
      if (owner == program.classInstances().end())
        return 2;
      if (owner->kind == ClassKind::Union || owner->cAbiRecord ||
          std::any_of(owner->bases.begin(), owner->bases.end(),
                      [](const auto &base) { return !base.interface; }))
        return 2;
      visiting.push_back(type);
      unsigned count = 0;
      for (const auto &field : owner->fields) {
        if (field.info.type.kind != SemanticType::Reference &&
            !field.info.traits.containsBorrowedState)
          continue;
        count += self(self, field.info.type);
        if (count > 1)
          break;
      }
      visiting.pop_back();
      return count;
    };
    return references(references, value.info.type) == 1 &&
           (receiverResult ||
            (reference &&
             reference->arguments ==
                 value.parameterTypes[value.borrowArgument].arguments));
  }

  // Whether a reference argument names exactly the referent an accessor call
  // returned under `accessor`: that call's own result place, or a reference
  // binding initialized from it. A reference binding is never reseated, so
  // its dereference stays that referent for the binding's whole lifetime.
  [[nodiscard]] bool
  designatesAccessorReferent(const MirOperand &argument,
                             const MirLoan &accessor,
                             const SemanticType &parameter) const {
    const MirPlace *place = output.findPlace(argument.place);
    const HirValue *result = findValue(accessor.producedBy);
    if (accessor.kind != MirLoanKind::CallResult || place == nullptr ||
        result == nullptr || parameter.kind != SemanticType::Reference ||
        parameter.arguments.size() != 1 ||
        result->info.category != ValueCategory::Place ||
        result->info.type != parameter.arguments.front() ||
        place->type != result->info.type ||
        !accessor.designatedFields.empty() ||
        !accessor.additionalDependencies.empty())
      return false;
    if (place->root == MirPlaceRootKind::Loan)
      return place->loan == accessor.id && place->projections.empty();
    return place->root == MirPlaceRootKind::Binding &&
           place->projections.size() == 1 &&
           place->projections.front().kind == MirProjectionKind::Dereference &&
           std::find(accessor.carriers.begin(), accessor.carriers.end(),
                     place->binding) != accessor.carriers.end();
  }

  [[nodiscard]] MirPlaceId borrowOriginPlace(const HirValue &value,
                                             const MirInstruction &call) {
    if (!value.borrowFields.empty() &&
        !value.borrowFields.front().sourceFields.empty()) {
      const auto &mapping = value.borrowFields.front();
      std::optional<HirValueId> sourceValue;
      if (mapping.origin == BorrowOriginKind::Receiver)
        sourceValue = receiverValue(value);
      else if (mapping.origin == BorrowOriginKind::Argument) {
        const auto arguments = callArgumentValues(value);
        if (mapping.parameter < arguments.size())
          sourceValue = arguments[mapping.parameter];
      }
      const auto *loan =
          sourceValue ? output.findLoan(loanForValue(*sourceValue)) : nullptr;
      // The synthesized iterator retains the selected physical range owner.
      // A separate stored dependency keeps that owner's resource alive; it is
      // not the storage containing the range elements.
      if (loan != nullptr && sourceValue &&
          isRangeIteratorValue(*sourceValue) &&
          (loan->kind == MirLoanKind::Local ||
           loan->kind == MirLoanKind::CallResult) &&
          loan->designatedFields.empty() &&
          loan->additionalDependencies.empty())
        return loan->source;
      if (loan != nullptr && loan->kind != MirLoanKind::Stored &&
          loan->storedDependency != 0)
        loan = output.findLoan(loan->storedDependency);
      if (loan != nullptr) {
        // A member receiver's contract is relative to that member, whereas
        // concrete stored-loan paths are relative to the complete carrier.
        // Compose the exact receiver prefix before selecting its external
        // referent; otherwise two nested views of different owners collapse
        // onto the address of the stored reference in the carrier shell.
        auto sourceFields = mapping.sourceFields;
        const auto *sourcePlace =
            sourceValue ? output.findPlace(placeForValue(*sourceValue))
                        : nullptr;
        if (!loan->entry && sourcePlace != nullptr &&
            sourcePlace->root == MirPlaceRootKind::Binding &&
            std::find(loan->carriers.begin(), loan->carriers.end(),
                      sourcePlace->binding) != loan->carriers.end() &&
            std::all_of(sourcePlace->projections.begin(),
                        sourcePlace->projections.end(),
                        [](const auto &projection) {
                          return projection.kind == MirProjectionKind::Field;
                        })) {
          std::vector<SymbolId> prefix;
          for (const auto &projection : sourcePlace->projections)
            prefix.push_back(projection.field);
          sourceFields.insert(sourceFields.begin(), prefix.begin(),
                              prefix.end());
        }
        const auto *input = sourceValue ? findValue(*sourceValue) : nullptr;
        if (loan->entry && loan->kind == MirLoanKind::Stored &&
            loan->access == AccessMode::ReadOnly &&
            value.info.category == ValueCategory::Value &&
            value.info.type.kind == SemanticType::Class &&
            value.info.traits.containsBorrowedState &&
            value.info.traits.copyable &&
            loan->additionalDependencies.empty() &&
            loan->designatedFields == mapping.sourceFields)
          // Compactness belongs to this selected input dependency, not the
          // number of fields in the result. Composing several such inputs
          // must preserve each opaque entry identity independently.
          return loan->source;
        if (loan->entry && loan->kind == MirLoanKind::Stored)
          return projectBorrowFields(loan->source, mapping.sourceFields,
                                     mapping.sourceConditions);
        // A sole-path carrier may still use the compact single-origin loan
        // representation. Its selected field already denotes that source;
        // projecting the field onto the external owner would change meaning.
        // A loan that names its carrier's one stored reference path is the
        // same single origin: the input, the carrier or the subobject of it
        // that holds that reference, reaches only that referent.
        if (loan->kind == MirLoanKind::Stored &&
            (loan->designatedFields.empty() || solePathLoan(*loan)) &&
            loan->additionalDependencies.empty()) {
          const auto prefix = input
                                  ? storedReferencePrefix(input->info.type,
                                                          mapping.sourceFields)
                                  : std::nullopt;
          if (prefix && *prefix < mapping.sourceFields.size())
            return projectBorrowFields(loan->source,
                                       {mapping.sourceFields.begin() + *prefix,
                                        mapping.sourceFields.end()});
          return loan->source;
        }
        const auto select =
            [&](MirPlaceId source,
                const std::vector<SymbolId> &path) -> MirPlaceId {
          if (path.size() > sourceFields.size() ||
              !std::equal(path.begin(), path.end(), sourceFields.begin()))
            return 0;
          if (path.size() == sourceFields.size())
            return source;
          return projectBorrowFields(
              source, {sourceFields.begin() + path.size(), sourceFields.end()});
        };
        if (const auto source = select(loan->source, loan->designatedFields))
          return source;
        for (const auto &dependency : loan->additionalDependencies)
          if (dependency.source != 0)
            if (const auto source =
                    select(dependency.source, dependency.designatedFields))
              return source;
      }
      if (sourceValue)
        return projectBorrowFields(placeForValue(*sourceValue),
                                   mapping.sourceFields,
                                   mapping.sourceConditions);
    }
    // A returned reference protects the addressable carrier shell. A
    // by-value carrier instead forwards its external stored referents.
    if (borrowsCarrierStorage(value) || storesCarrierReference(value)) {
      if (value.borrowOrigin == BorrowOriginKind::Receiver) {
        if (const auto receiver = receiverValue(value)) {
          const auto *input = findValue(*receiver);
          if (input != nullptr &&
              input->info.category == ValueCategory::Value &&
              input->info.type.kind == SemanticType::Class &&
              input->info.traits.containsBorrowedState) {
            // Borrowed-result operators retain their ordinary Value receiver
            // stage. Its physical shell nevertheless has the same exact
            // producer/drop authority as an ephemeral receiver. Do not
            // canonicalize this place through its external Stored loan: an
            // operator may return an inline field of the temporary itself.
            const auto shell = temporaryBorrowOperand(
                *receiver, receiverAccess(value), input->info.type, true);
            if (shell.place != 0)
              valuePlaces.insert_or_assign(*receiver, shell.place);
            return shell.place;
          }
          return canonicalBorrowSource(placeForValue(*receiver));
        }
      } else if (value.borrowOrigin == BorrowOriginKind::Argument) {
        const auto arguments = callArgumentValues(value);
        if (value.borrowArgument < arguments.size())
          return canonicalBorrowSource(
              placeForValue(arguments[value.borrowArgument]));
      }
    }
    if (value.borrowOrigin == BorrowOriginKind::Receiver && call.receiver) {
      if (const std::optional<HirValueId> receiver = receiverValue(value)) {
        // A receiver behind a stored reference field keeps its selected
        // place: the carrier's loan source would name the object holding
        // the reference rather than its referent.
        if (const MirPlace *selected = output.findPlace(call.receiver->place);
            selected != nullptr && storedReferenceAccessFor(*selected))
          return call.receiver->place;
        if (const MirPlaceId source = borrowedSourcePlace(*receiver);
            source != 0) {
          // A by-reference capture's entry loan names the capture slot. A
          // receiver reached through it is the referent place itself, as a
          // reference formal's dereferenced receiver is.
          if (const MirPlaceId referent =
                  captureReferentPlace(*receiver, source))
            return referent;
          return canonicalBorrowSource(source);
        }
        if (call.receiver->place != 0) {
          return canonicalBorrowSource(call.receiver->place);
        }
        if (call.receiver->kind == MirOperandKind::Loan) {
          const MirLoan *loan = output.findLoan(call.receiver->loan);
          return loan == nullptr ? 0 : canonicalBorrowSource(loan->source);
        }
        if (const HirValue *receiverValue = findValue(*receiver);
            receiverValue != nullptr) {
          const MirPlaceId source = valueRootPlace(*receiverValue);
          valuePlaces.insert_or_assign(*receiver, source);
          return source;
        }
      }
      return canonicalBorrowSource(call.receiver->place);
    }
    if (value.borrowOrigin == BorrowOriginKind::Argument &&
        value.borrowArgument < call.operands.size()) {
      const std::vector<HirValueId> arguments = callArgumentValues(value);
      if (value.borrowArgument < arguments.size()) {
        // A text view relayed from a reference formal's text field holds the
        // text of that field, which the function's field contract tracks
        // (L-OWN-02), as a receiver's text field's does: keep the field
        // place rather than the formal's entry loan on the whole referent.
        if (const MirPlaceId field =
                formalTextFieldPlace(value, arguments[value.borrowArgument]))
          return canonicalBorrowSource(field);
        if (const MirPlaceId source =
                borrowedSourcePlace(arguments[value.borrowArgument]);
            source != 0) {
          return canonicalBorrowSource(source);
        }
      }
      const MirOperand &argument = call.operands[value.borrowArgument];
      if (argument.kind == MirOperandKind::Loan) {
        const MirLoan *loan = output.findLoan(argument.loan);
        return loan == nullptr ? 0 : canonicalBorrowSource(loan->source);
      }
      if (argument.place != 0) {
        return canonicalBorrowSource(argument.place);
      }
      if (value.borrowArgument < arguments.size()) {
        return canonicalBorrowSource(
            placeForValue(arguments[value.borrowArgument]));
      }
    }
    if (value.borrowOrigin == BorrowOriginKind::Global && value.borrowPlace) {
      return placeForBorrowOrigin(*value.borrowPlace, value);
    }
    return 0;
  }

  [[nodiscard]] MirLoanId
  createLoan(MirLoanKind kind, MirPlaceId sourcePlace, AccessMode access,
             HirValueId producedBy = 0, HirBindingId binding = 0,
             SymbolId storedField = 0, MirLoanId parent = 0,
             bool twoPhase = false) {
    const MirLoanId id = output.loans.size() + 1;
    const HirValue *producer =
        producedBy == 0 ? nullptr : findValue(producedBy);
    const HirBinding *carrier = binding == 0 ? nullptr : findBinding(binding);
    // A shell/argument/reference loan never acquires its producer's cleanup.
    // Only an owning stored value or an owning return carries that obligation.
    const bool lifecycleBound =
        (kind == MirLoanKind::Stored ||
         (kind == MirLoanKind::Return &&
          isLifecycleBoundBorrowCarrier(output.returnType))) &&
        ((producer != nullptr &&
          isLifecycleBoundBorrowCarrier(producer->info.type)) ||
         (carrier != nullptr &&
          isLifecycleBoundBorrowCarrier(carrier->info.type)));
    output.loans.push_back(
        {.id = id,
         .parent = parent,
         .kind = kind,
         .source = sourcePlace,
         .access = access,
         .producedBy = producedBy,
         .carriers = binding == 0 ? std::vector<HirBindingId>{}
                                  : std::vector<HirBindingId>{binding},
         .storedField = storedField,
         .lifecycleBound = lifecycleBound,
         .twoPhase = twoPhase});
    return id;
  }

  [[nodiscard]] bool exactReadonlyCleanupParent(const HirValue &value,
                                                MirLoanId origin) const {
    const auto contract = retainedInitializerLoans.find(value.id);
    const auto *retained =
        contract == retainedInitializerLoans.end() || contract->second == 0
            ? nullptr
            : source.findLoan(contract->second);
    const auto *selected = output.findLoan(origin);
    if (!retained || !retained->lifecycleBound || retained->parent == 0 ||
        retained->access != AccessMode::ReadOnly ||
        value.borrowAccess != AccessMode::ReadOnly || !selected ||
        selected->access != AccessMode::ReadOnly)
      return false;
    const auto parent = semanticLoans.find(retained->parent);
    return parent != semanticLoans.end() && parent->second == origin;
  }

  [[nodiscard]] static DefinedFailureOperation
  localDefinedFailure(const HirValue *value) {
    DefinedFailureOperation operation;
    if (value != nullptr) {
      operation.localOrigins = value->definedFailure.localOrigins;
    }
    return operation;
  }

  [[nodiscard]] PreparedCallInput
  prepareCallInput(HirValueId callSite, HirValueId sourceValue,
                   const SemanticType &type, HirCallInputKind inputKind,
                   MirCallInputRole role, std::size_t index, MirOperand operand,
                   bool stageOwningParameter, bool defaultArgument = false,
                   MirDropObligationId movedBindingDrop = 0,
                   std::size_t constructorInitializer = 0) {
    ExpressionInfo info{.type = type,
                        .category = ValueCategory::Value,
                        .access = AccessMode::ReadOnly,
                        .traits = resolvedTraits(type)};
    const HirValue *sourceValueInfo = findValue(sourceValue);
    if (sourceValueInfo != nullptr && sourceValueInfo->info.type == type) {
      info.traits = sourceValueInfo->info.traits;
    }
    const MirValueId result = appendValue(sourceValue, info);
    MirInstruction input{.kind = MirInstructionKind::CallInput,
                         .hirValue = sourceValue,
                         .callSite = callSite,
                         .constructorInitializer = constructorInitializer,
                         .callInputRole = role,
                         .callInputIndex = index,
                         .callInputKind = inputKind,
                         .defaultArgument = defaultArgument,
                         .result = result,
                         .operands = {std::move(operand)},
                         .info = info};
    if (inputKind == HirCallInputKind::RetainedReceiver &&
        sourceValueInfo != nullptr && sourceValueInfo->retainedReceiverOwner) {
      const MirPlaceId anchor =
          placeForValue(*sourceValueInfo->retainedReceiverOwner);
      if (anchor == 0) {
        valid = false;
      } else {
        input.retainedOwnerPlace = anchor;
      }
    }
    MirLoanId inputLoan = 0;
    if (role == MirCallInputRole::Receiver &&
        info.traits.containsBorrowedState &&
        (inputKind == HirCallInputKind::ReadTemporaryBorrow ||
         inputKind == HirCallInputKind::MutableTemporaryBorrow)) {
      const MirPlace *shell = output.findPlace(input.operands.front().place);
      const auto dependency =
          shell == nullptr ? MirTemporaryCarrierDependency{}
                           : findMirTemporaryCarrierDependency(output, *shell);
      const AccessMode access =
          inputKind == HirCallInputKind::MutableTemporaryBorrow
              ? AccessMode::Mutable
              : AccessMode::ReadOnly;
      if (!dependency || shell->type != type || scopes.empty()) {
        valid = false;
      } else {
        const MirLoanId stored = dependency.loan->id;
        inputLoan =
            createLoan(MirLoanKind::CallArgument, shell->id, access, callSite);
        output.loans[inputLoan - 1].storedDependency = stored;
        input.loan = inputLoan;
        scopes.back().loans.push_back(inputLoan);
      }
    }
    if (role == MirCallInputRole::Argument &&
        inputKind == HirCallInputKind::ReadTemporaryBorrow) {
      const MirPlace *source = output.findPlace(input.operands.front().place);
      auto materialization =
          source ? findMirTemporaryMaterialization(output, *source, sourceValue)
                 : MirTemporaryMaterialization{};
      if (!materialization && source)
        materialization =
            findMirMovedTemporaryMaterialization(output, *source, sourceValue);
      if (type.kind != SemanticType::Reference ||
          type.referenceAccess != AccessMode::ReadOnly ||
          type.arguments.size() != 1 ||
          (type.arguments.front().kind != SemanticType::Class &&
           type.arguments.front().kind != SemanticType::Enum &&
           type.arguments.front().kind != SemanticType::Lambda &&
           type.arguments.front().kind != SemanticType::FunctionItem) ||
          input.operands.front().kind != MirOperandKind::BorrowRead ||
          input.operands.front().type != type || source == nullptr ||
          !materialization || !source->projections.empty() ||
          source->type != type.arguments.front() ||
          source->access != AccessMode::Mutable ||
          source->sourceValue != sourceValue || scopes.empty()) {
        valid = false;
      } else {
        inputLoan = createLoan(MirLoanKind::CallArgument, source->id,
                               AccessMode::ReadOnly, callSite);
        if (source->traits.containsBorrowedState) {
          // A fresh borrowed carrier keeps its producer's stored dependency
          // live through the call, as a capturing lambda keeps its captures.
          const auto dependency =
              findMirTemporaryCarrierDependency(output, *source);
          if (!dependency || (source->type.kind != SemanticType::Lambda &&
                              source->type.kind != SemanticType::Class))
            valid = false;
          else
            output.loans[inputLoan - 1].storedDependency = dependency.loan->id;
        }
        input.loan = inputLoan;
        scopes.back().loans.push_back(inputLoan);
      }
    }
    const bool materializesCheckedPlace =
        inputKind == HirCallInputKind::CopyValue ||
        ((inputKind == HirCallInputKind::ReadBorrow ||
          inputKind == HirCallInputKind::MutableBorrow) &&
         input.operands.front().kind != MirOperandKind::Loan);
    // The source selection has already completed at its checked AddressOf
    // prefix, before this prepared copy/borrow is published. Do not attach
    // that same selection failure to a second operation with no Invoke edge.
    // A call-like place is already materialized by emitPlaceDependencies.
    // Its own instruction/Invoke performed the check before this stage copies
    // or borrows the resulting place. Local effects belong to that producer,
    // not a second check attached to the prepared argument.
    const bool evaluatedCallPlace = sourceValueInfo != nullptr &&
                                    isCallLike(*sourceValueInfo) &&
                                    emittedValues.contains(sourceValue);
    if (materializesCheckedPlace && !evaluatedCallPlace) {
      input.definedFailure = selectedPlaceFailure(sourceValueInfo);
    }
    MirDropObligationId parameterDrop = 0;
    const std::optional<MirDropType> dropType =
        stageOwningParameter ? preparedParameterDropType(type) : std::nullopt;
    if (dropType && (inputKind == HirCallInputKind::CopyValue ||
                     inputKind == HirCallInputKind::MoveValue ||
                     inputKind == HirCallInputKind::MoveBorrowedTemporary)) {
      parameterDrop = appendPreparedParameterDrop(callSite, sourceValue, result,
                                                  info, *dropType);
      const MirDropObligation *prepared =
          output.findDropObligation(parameterDrop);
      if (prepared == nullptr) {
        valid = false;
      } else {
        input.preparedParameterDrop = parameterDrop;
        input.destination = prepared->place;
        if (inputKind == HirCallInputKind::CopyValue) {
          appendLifecycle(input, {.kind = MirLifecycleEventKind::Initialize,
                                  .target = parameterDrop});
        } else if (movedBindingDrop != 0) {
          appendLifecycle(input, {.kind = MirLifecycleEventKind::Move,
                                  .source = movedBindingDrop,
                                  .target = parameterDrop});
        } else {
          const MirDropObligationId sourceDrop =
              dropObligationForValue(sourceValue);
          if (temporaryIsActive(sourceDrop)) {
            appendReparentOrTypedTransfer(input, sourceDrop, parameterDrop);
            (void)removeTemporary(sourceDrop);
          } else {
            appendLifecycle(input, {.kind = MirLifecycleEventKind::Initialize,
                                    .target = parameterDrop});
          }
        }
      }
    } else if ((inputKind == HirCallInputKind::MoveValue &&
                role != MirCallInputRole::Receiver) ||
               inputKind == HirCallInputKind::MoveBorrowedTemporary) {
      transferTemporaryOut(input, sourceValue);
    }
    (void)appendInstruction(std::move(input));
    registerTemporary(parameterDrop);
    return {.operand = {.kind = MirOperandKind::Value,
                        .value = result,
                        .type = type},
            .parameterDrop = parameterDrop,
            .loan = inputLoan};
  }

  void emitCall(const HirValue &value, bool detachedBorrowResult = false,
                bool reserveBorrowResult = false, bool construction = false) {
    if (value.kind == HirValueKind::Call && value.functionTarget &&
        !value.operands.empty()) {
      const HirValue *callee = findValue(value.operands.front());
      if (callee != nullptr && callee->info.type.hasFunctionItemShape()) {
        // The selected free function needs no receiver, but evaluating the
        // expression that produces its stateless identity can have effects.
        // Keep those effects before argument evaluation in the MIR schedule.
        (void)valueOperand(callee->id);
      }
    }
    if (terminated()) {
      return;
    }
    if (value.intrinsic == IntrinsicKind::RawPointerConversion) {
      const auto arguments = callArgumentValues(value);
      if (arguments.size() != 1 || !value.definedFailure.empty()) {
        valid = false;
        return;
      }
      auto operand = valueOperand(arguments.front());
      (void)appendInstruction({.kind = MirInstructionKind::Compute,
                               .hirValue = value.id,
                               .result = resultFor(value),
                               .operands = {std::move(operand)},
                               .operation = MirOperation::Convert,
                               .info = value.info});
      return;
    }
    if (value.intrinsic == IntrinsicKind::ExactTypeParameterConstruction) {
      std::vector<MirOperand> operands;
      for (const HirValueId id : callArgumentValues(value)) {
        const HirValue *argument = findValue(id);
        if (argument == nullptr) {
          valid = false;
          return;
        }
        if (argument->kind != HirValueKind::PackExpansion) {
          operands.push_back(argumentOperand(id, value.info.type));
          continue;
        }
        const auto local = localSymbols.find(argument->symbol);
        const MirPlace *root = output.findPlace(
            local == localSymbols.end() ? 0 : placeForBinding(local->second));
        if (root == nullptr || root->type.kind != SemanticType::TypePack ||
            !root->type.concretePack || root->type.arguments.size() > 1) {
          valid = false;
          return;
        }
        if (root->type.arguments.empty()) {
          continue;
        }
        MirPlace element = *root;
        element.id = 0;
        element.projections.push_back(
            {.kind = MirProjectionKind::PackElement, .constantIndex = 0});
        element.type = root->type.arguments.front();
        element.traits = resolvedTraits(element.type);
        element.sourceValue = id;
        element.key.reset();
        const MirPlaceId place = appendPlace(std::move(element));
        const MirValueId loaded = appendValue(id, value.info);
        (void)appendInstruction({.kind = MirInstructionKind::Load,
                                 .hirValue = id,
                                 .result = loaded,
                                 .operands = {{.kind = MirOperandKind::Copy,
                                               .place = place,
                                               .type = value.info.type}},
                                 .info = value.info});
        operands.push_back({.kind = MirOperandKind::Value,
                            .value = loaded,
                            .type = value.info.type});
      }
      if (operands.size() != 1 || operands.front().type != value.info.type ||
          !value.definedFailure.empty()) {
        valid = false;
        return;
      }
      (void)appendInstruction({.kind = MirInstructionKind::Compute,
                               .hirValue = value.id,
                               .result = resultFor(value),
                               .operands = std::move(operands),
                               .operation = MirOperation::Identity,
                               .info = value.info});
      return;
    }
    DefinedFailureOperation failure = value.definedFailure;
    if (detachedBorrowResult) {
      failure.localOrigins.clear();
      failure.propagation = value.dispatch == CallDispatch::Virtual
                                ? FailurePropagationKind::VirtualCall
                                : FailurePropagationKind::DirectCall;
    }
    if (value.intrinsic == IntrinsicKind::ObjectConstruct &&
        failure.propagation == FailurePropagationKind::Constructor &&
        value.constructorTarget) {
      // Semantics selected this constructor protocol from its exact signature.
      // HIR argument types may still group a concrete pack; normalization must
      // not depend on the later operand/parameter flattening below.
      failure.propagation = normalizedConstructorPropagation(
          failure.propagation, value.constructorTarget);
    } else if (failure.localOrigins.empty()) {
      failure.propagation =
          construction
              ? normalizedConstructorPropagation(failure.propagation,
                                                 value.constructorTarget)
              : normalizedCallPropagation(failure.propagation, value.dispatch,
                                          value.functionTarget);
    }
    if (reserveBorrowResult && !detachedBorrowResult) {
      valid = false;
      return;
    }
    if (detachedBorrowResult &&
        (!value.functionTarget ||
         value.borrowOrigin == BorrowOriginKind::None)) {
      valid = false;
      return;
    }
    MirInstruction call{
        .kind = construction ? MirInstructionKind::Construct
                             : MirInstructionKind::Call,
        .hirValue = value.id,
        .callSite = value.callPlan ? value.id : 0,
        .intrinsic = value.intrinsic,
        .objectLayoutType = value.objectLayoutType,
        .neverReturns = value.neverReturns,
        .noNormalBorrowResult = value.noNormalBorrowResultProof.has_value(),
        .tracksCaller = value.tracksCaller,
        .callerLocation = value.callerLocation,
        .forwardsCallerLocation =
            value.tracksCaller && output.hasCallerLocation,
        .sourceLocation = value.sourceLocation,
        .synchronization = value.synchronization,
        .definedFailure = std::move(failure),
        .dispatch = value.dispatch,
        .dispatchOwner = value.dispatchOwner,
        .functionTarget = value.functionTarget,
        .functionPointerFunctionTarget = value.functionPointerFunctionTarget,
        .functionPointerLambdaTarget = value.functionPointerLambdaTarget,
        .nativeCalleeType = value.nativeCalleeType,
        .threadTaskAdapter = value.threadTaskAdapter,
        .constructorTarget = value.constructorTarget,
        .constructorKind = value.constructorKind,
        .lambdaTarget = value.lambdaTarget,
        .callableArguments = value.callableArguments,
        .callableBoundary = value.callableBoundary,
        .callableInvocation = value.callableInvocation,
        .info = value.info};

    const std::vector<HirValueId> arguments = callArgumentValues(value);
    const bool expandsPack = std::any_of(
        arguments.begin(), arguments.end(), [&](HirValueId argument) {
          const HirValue *input = findValue(argument);
          return input != nullptr && input->kind == HirValueKind::PackExpansion;
        });
    if (expandsPack) {
      call.callSite = value.id;
    }
    std::optional<MirOperand> sourceReceiver;
    std::optional<HirValueId> sourceReceiverValue;
    std::vector<MirOperand> sourceArguments;
    std::vector<MirLoanId> argumentDependencies;
    std::vector<MirDropObligationId> preparedParameterDrops;
    std::vector<MirLoanId> temporaryArgumentLoans;
    sourceArguments.reserve(arguments.size());
    preparedParameterDrops.reserve(arguments.size() + 1);

    if (value.callPlan) {
      if (value.callPlan->receiver) {
        const HirCallReceiver &receiver = *value.callPlan->receiver;
        sourceReceiverValue = receiver.value;
        const AccessMode access =
            receiver.kind == HirCallInputKind::RetainedReceiver ||
                    receiver.kind == HirCallInputKind::MutableBorrow ||
                    receiver.kind == HirCallInputKind::MutableTemporaryBorrow
                ? AccessMode::Mutable
                : AccessMode::ReadOnly;
        const bool temporaryReceiver =
            receiver.kind == HirCallInputKind::ReadTemporaryBorrow ||
            receiver.kind == HirCallInputKind::MutableTemporaryBorrow;
        const HirValue *receiverSource = findValue(receiver.value);
        const bool retainedReceiver =
            receiver.kind == HirCallInputKind::RetainedReceiver;
        if (retainedReceiver && (receiverSource == nullptr ||
                                 !receiverSource->retainedReceiverProjection ||
                                 receiverSource->kind != HirValueKind::Move ||
                                 receiverSource->operands.size() != 1)) {
          valid = false;
          return;
        }
        sourceReceiver =
            value.nativeCalleeType ? valueOperand(receiver.value)
            : retainedReceiver
                ? receiverOperand(receiverSource->operands.front(), access)
            : temporaryReceiver ? temporaryBorrowOperand(receiver.value, access,
                                                         receiver.type, true)
                                : receiverOperand(receiver.value, access);
        if (terminated()) {
          return;
        }
        PreparedCallInput prepared = prepareCallInput(
            value.id, receiver.value, receiver.type, receiver.kind,
            MirCallInputRole::Receiver, 0, *sourceReceiver,
            !retainedReceiver && !temporaryReceiver &&
                !(receiver.kind == HirCallInputKind::MoveValue &&
                  dropObligationForValue(receiver.value) != 0));
        call.receiver = std::move(prepared.operand);
        if (prepared.parameterDrop != 0) {
          preparedParameterDrops.push_back(prepared.parameterDrop);
        }
        if (prepared.loan != 0) {
          temporaryArgumentLoans.push_back(prepared.loan);
        }
      }
    } else if (const std::optional<HirValueId> receiver =
                   receiverValue(value)) {
      sourceReceiverValue = *receiver;
      const AccessMode access = receiverAccess(value);
      const HirValue *receiverSource = findValue(*receiver);
      while (receiverSource != nullptr &&
             receiverSource->retainedReceiverProjection &&
             receiverSource->kind == HirValueKind::Grouping &&
             receiverSource->operands.size() == 1) {
        receiverSource = findValue(receiverSource->operands.front());
      }
      const bool retainedReceiver = receiverSource != nullptr &&
                                    receiverSource->retainedReceiverProjection;
      if (retainedReceiver &&
          (!expandsPack || receiverSource->kind != HirValueKind::Move ||
           receiverSource->operands.size() != 1)) {
        valid = false;
        return;
      }
      MirOperand operand =
          value.nativeCalleeType ? valueOperand(*receiver)
          : retainedReceiver ? receiverOperand(receiverSource->operands.front(),
                                               AccessMode::Mutable)
                             : receiverOperand(*receiver, access);
      if (expandsPack) {
        const HirValue *input = findValue(*receiver);
        // An owned environment has a retained full-expression obligation.
        // Cleanup-free closure identities keep their ordinary value stage;
        // the preceding Move still carries source consumption in either case.
        const bool movedClosure =
            input != nullptr && input->kind == HirValueKind::Move &&
            input->info.type.kind == SemanticType::Lambda &&
            dropObligationForValue(*receiver) != 0;
        const HirCallInputKind kind =
            value.nativeCalleeType ? HirCallInputKind::Value
            : retainedReceiver     ? HirCallInputKind::RetainedReceiver
            : movedClosure         ? HirCallInputKind::MoveValue
            : input != nullptr && input->info.category == ValueCategory::Place
                ? (access == AccessMode::Mutable
                       ? HirCallInputKind::MutableBorrow
                       : HirCallInputKind::ReadBorrow)
                : HirCallInputKind::Value;
        sourceReceiver = operand;
        PreparedCallInput prepared = prepareCallInput(
            value.id, retainedReceiver ? receiverSource->id : *receiver,
            input == nullptr ? SemanticType::Unknown : input->info.type, kind,
            MirCallInputRole::Receiver, 0, operand,
            !movedClosure && !retainedReceiver);
        call.receiver = std::move(prepared.operand);
        if (prepared.parameterDrop != 0) {
          preparedParameterDrops.push_back(prepared.parameterDrop);
        }
      } else {
        call.receiver = std::move(operand);
      }
    }
    if (sourceReceiverValue) {
      if (const HirValue *source = findValue(*sourceReceiverValue);
          source != nullptr &&
          source->unsafeOperation == UnsafeOperationKind::RawMember &&
          source->info.category != ValueCategory::Place) {
        // Only a direct `pointer->method()` uses the raw-pointee receiver
        // contract. A field selected through that pointer is already an
        // exact projected place, and its ordinary method/operator call must
        // preserve the field projection. appendInstruction independently
        // retains the raw-memory effect from that receiver place.
        call.unsafeOperation = UnsafeOperationKind::RawMember;
      }
    }

    if (!value.callPlan && !expandsPack && value.lambdaTarget &&
        !value.operands.empty() && !value.functionTarget) {
      const std::size_t argumentCount = arguments.size();
      if (value.operands.size() > argumentCount) {
        call.receiver = valueOperand(value.operands.front());
      }
    }

    if (value.callPlan) {
      for (const HirCallArgument &argument : value.callPlan->arguments) {
        call.parameterTypes.push_back(argument.parameterType);
        MirOperand sourceArgument =
            argument.kind == HirCallInputKind::ReadTemporaryBorrow
                ? temporaryBorrowOperand(
                      argument.value, AccessMode::ReadOnly,
                      argument.parameterType,
                      argument.parameterType.kind == SemanticType::Reference &&
                          argument.parameterType.arguments.size() == 1 &&
                          argument.parameterType.arguments.front().kind ==
                              SemanticType::Lambda)
            : argument.kind == HirCallInputKind::CopyValue
                ? copyArgumentOperand(argument.value, argument.parameterType)
                : argumentOperand(argument.value, argument.parameterType);
        if (terminated()) {
          return;
        }
        sourceArguments.push_back(std::move(sourceArgument));
        PreparedCallInput prepared = prepareCallInput(
            value.id, argument.value, argument.parameterType, argument.kind,
            MirCallInputRole::Argument, argument.parameterIndex,
            sourceArguments.back(),
            argument.kind != HirCallInputKind::ReadTemporaryBorrow,
            argument.defaultArgument);
        call.operands.push_back(std::move(prepared.operand));
        argumentDependencies.push_back(loanForValue(argument.value));
        if (prepared.loan != 0) {
          temporaryArgumentLoans.push_back(prepared.loan);
        }
        if (prepared.parameterDrop != 0) {
          preparedParameterDrops.push_back(prepared.parameterDrop);
        }
      }
    } else if (expandsPack) {
      const auto inputKindFor = [&](const HirValue &input,
                                    const SemanticType &parameter)
          -> std::optional<HirCallInputKind> {
        if (parameter.kind == SemanticType::Reference) {
          return parameter.referenceAccess == AccessMode::Mutable
                     ? HirCallInputKind::MutableBorrow
                     : HirCallInputKind::ReadBorrow;
        }
        switch (parameter.kind) {
        case SemanticType::Int8:
        case SemanticType::Int16:
        case SemanticType::Int32:
        case SemanticType::Int64:
        case SemanticType::UInt8:
        case SemanticType::UInt16:
        case SemanticType::UInt32:
        case SemanticType::UInt64:
        case SemanticType::Float:
        case SemanticType::Double:
        case SemanticType::Bool:
        case SemanticType::Char:
        case SemanticType::StringView:
        case SemanticType::CString:
        case SemanticType::NullPtr:
        case SemanticType::RawPointer:
        case SemanticType::Enum:
          return HirCallInputKind::Value;
        case SemanticType::NativeFunction:
          return parameter.hasNativeFunctionShape()
                     ? std::optional{HirCallInputKind::Value}
                     : std::nullopt;
        case SemanticType::FunctionItem:
          return parameter.hasFunctionItemShape()
                     ? std::optional{HirCallInputKind::Value}
                     : std::nullopt;
        case SemanticType::Array: {
          const SemanticType *leaf = &parameter;
          while (leaf->kind == SemanticType::Array) {
            if (leaf->arguments.size() != 1 || leaf->arrayLength == 0 ||
                leaf->arrayLengthParameterId != 0)
              return std::nullopt;
            leaf = &leaf->arguments.front();
          }
          // Match HIR's passive by-value array plan. The semantic traits and
          // concrete leaf lifecycle remain authoritative; no array decay or
          // borrowed/owning element transfer is synthesized here.
          const auto cleanup = preparedParameterDropType(*leaf);
          if (input.info.type != parameter || !input.info.traits.copyable ||
              input.info.traits.containsBorrowedState ||
              (cleanup && cleanup->requiresActiveCleanup))
            return std::nullopt;
          return HirCallInputKind::Value;
        }
        case SemanticType::Class:
          if (input.info.category == ValueCategory::Place &&
              input.info.traits.copyable) {
            return HirCallInputKind::CopyValue;
          }
          if (input.info.category == ValueCategory::Value &&
              input.info.traits.movable) {
            return HirCallInputKind::MoveValue;
          }
          return std::nullopt;
        default:
          return std::nullopt;
        }
      };
      const auto appendPreparedArgument =
          [&](HirValueId sourceValue, const SemanticType &parameter,
              HirCallInputKind kind, MirOperand sourceOperand,
              MirDropObligationId movedBindingDrop = 0,
              MirLoanId dependency = 0, bool defaultArgument = false) {
            const std::size_t parameterIndex = call.operands.size();
            sourceArguments.push_back(sourceOperand);
            PreparedCallInput prepared =
                prepareCallInput(value.id, sourceValue, parameter, kind,
                                 MirCallInputRole::Argument, parameterIndex,
                                 std::move(sourceOperand), true,
                                 defaultArgument, movedBindingDrop);
            call.parameterTypes.push_back(parameter);
            call.operands.push_back(std::move(prepared.operand));
            argumentDependencies.push_back(dependency);
            if (prepared.parameterDrop != 0) {
              preparedParameterDrops.push_back(prepared.parameterDrop);
            }
          };

      for (std::size_t index = 0; index < arguments.size(); ++index) {
        const HirValue *input = findValue(arguments[index]);
        if (input == nullptr) {
          valid = false;
          continue;
        }
        if (input->kind != HirValueKind::PackExpansion) {
          const std::size_t parameterIndex = call.operands.size();
          const SemanticType parameter =
              parameterIndex < value.parameterTypes.size()
                  ? value.parameterTypes[parameterIndex]
                  : input->info.type;
          const bool mutableStorageInput =
              index == 0 && input->info.category == ValueCategory::Place &&
              input->info.type == parameter &&
              value.intrinsic == IntrinsicKind::OptionalStorageConstruct &&
              parameter.kind == SemanticType::Class;
          const std::optional<HirCallInputKind> kind =
              mutableStorageInput
                  ? std::optional<
                        HirCallInputKind>{HirCallInputKind::MutableBorrow}
                  : inputKindFor(*input, parameter);
          if (!kind) {
            valid = false;
            continue;
          }
          MirOperand sourceArgument =
              *kind == HirCallInputKind::CopyValue
                  ? copyArgumentOperand(input->id, parameter)
              : mutableStorageInput
                  ? receiverOperand(input->id, AccessMode::Mutable)
                  : argumentOperand(input->id, parameter);
          appendPreparedArgument(
              input->id, parameter, *kind, std::move(sourceArgument), 0,
              loanForValue(input->id),
              std::find(value.defaultArguments.begin(),
                        value.defaultArguments.end(),
                        input->id) != value.defaultArguments.end());
          continue;
        }

        const auto local = localSymbols.find(input->symbol);
        const MirPlaceId packPlaceId =
            local == localSymbols.end() ? 0 : placeForBinding(local->second);
        const MirPlace *resolvedPackPlace = output.findPlace(packPlaceId);
        const std::vector<SemanticType> &elements = input->info.type.arguments;
        const std::size_t parameterIndex = call.operands.size();
        const bool followedByDefaults =
            arguments.size() - index - 1 == value.defaultArguments.size() &&
            std::equal(arguments.begin() + index + 1, arguments.end(),
                       value.defaultArguments.begin());
        const bool retainedPackParameter =
            parameterIndex + 1 == value.parameterTypes.size() &&
            value.defaultArguments.empty() &&
            value.parameterTypes[parameterIndex].kind ==
                SemanticType::TypePack &&
            value.parameterTypes[parameterIndex].concretePack &&
            value.parameterTypes[parameterIndex].arguments == elements;
        const bool flattenedPackParameters =
            value.parameterTypes.size() == parameterIndex + elements.size() +
                                               value.defaultArguments.size() &&
            std::equal(
                elements.begin(), elements.end(),
                value.parameterTypes.begin() + parameterIndex,
                [](const SemanticType &element, const SemanticType &parameter) {
                  return mir_detail::validCallInputType(
                             element, parameter, HirCallInputKind::Value) ||
                         (parameter.kind == SemanticType::Reference &&
                          parameter.referenceAccess == AccessMode::ReadOnly &&
                          parameter.arguments.size() == 1 &&
                          parameter.arguments.front() == element);
                });
        if (!followedByDefaults ||
            input->info.type.kind != SemanticType::TypePack ||
            !input->info.type.concretePack || resolvedPackPlace == nullptr ||
            resolvedPackPlace->root != MirPlaceRootKind::Binding ||
            !resolvedPackPlace->projections.empty() ||
            resolvedPackPlace->symbol != input->symbol ||
            resolvedPackPlace->type.kind != SemanticType::TypePack ||
            !resolvedPackPlace->type.concretePack ||
            (!retainedPackParameter && !flattenedPackParameters) ||
            resolvedPackPlace->type.arguments != elements ||
            input->packExpansionElements.size() != elements.size() ||
            !resolvedPackPlace->key) {
          valid = false;
          continue;
        }
        const MirPlace packPlace = *resolvedPackPlace;
        for (std::size_t elementIndex = 0; elementIndex < elements.size();
             ++elementIndex) {
          const SemanticType &element = elements[elementIndex];
          const SemanticType &parameter =
              retainedPackParameter
                  ? element
                  : value.parameterTypes[parameterIndex + elementIndex];
          // The selected signature, not the source pack's ownership mode,
          // decides whether this invocation borrows an element. Its owning
          // parameter remains alive until the enclosing body's cleanup.
          const bool borrowed = parameter.kind == SemanticType::Reference;
          const HirPackExpansionElement &expansion =
              input->packExpansionElements[elementIndex];
          if (expansion.type != element ||
              (expansion.kind != HirCallInputKind::Value &&
               expansion.kind != HirCallInputKind::CopyValue &&
               expansion.kind != HirCallInputKind::MoveValue &&
               !((expansion.kind == HirCallInputKind::ReadBorrow ||
                  expansion.kind == HirCallInputKind::MutableBorrow) &&
                 element.kind == SemanticType::Reference &&
                 (expansion.kind == HirCallInputKind::MutableBorrow) ==
                     (element.referenceAccess == AccessMode::Mutable) &&
                 parameter == element))) {
            valid = false;
            continue;
          }
          PlaceKey elementKey = *packPlace.key;
          elementKey.projections.push_back(
              {.kind = PlaceProjectionKind::ConstantIndex,
               .index = elementIndex});
          MirPlace projected = packPlace;
          projected.id = 0;
          projected.projections.push_back(
              {.kind = MirProjectionKind::PackElement,
               .constantIndex = elementIndex});
          projected.type = element;
          projected.access = AccessMode::ReadOnly;
          projected.traits = expansion.traits;
          projected.sourceValue = input->id;
          projected.key = elementKey;
          const MirPlaceId elementPlace =
              appendPackElementPlace(std::move(projected));

          MirOperand sourceArgument;
          if (borrowed && parameter.referenceAccess == AccessMode::Mutable) {
            const auto loan = packEntryLoans.find(elementPlace);
            if (loan == packEntryLoans.end()) {
              valid = false;
              continue;
            }
            sourceArgument = {.kind = MirOperandKind::Loan,
                              .loan = loan->second,
                              .type = parameter};
          } else if (borrowed) {
            sourceArgument = {.kind = MirOperandKind::BorrowRead,
                              .place = elementPlace,
                              .type = parameter};
          } else if (expansion.kind == HirCallInputKind::MoveValue) {
            ExpressionInfo movedInfo{.type = element,
                                     .category = ValueCategory::Value,
                                     .access = AccessMode::ReadOnly,
                                     .traits = expansion.traits};
            const MirValueId moved = appendValue(input->id, movedInfo);
            MirInstruction move{.kind = MirInstructionKind::Move,
                                .hirValue = input->id,
                                .result = moved,
                                .operands = {{.kind = MirOperandKind::Move,
                                              .place = elementPlace,
                                              .type = element}},
                                .info = movedInfo,
                                .ownership = OwnershipEvent{
                                    .kind = OwnershipEventKind::Move,
                                    .place = elementKey,
                                    .before = OwnershipStateSet::Available,
                                    .after = OwnershipStateSet::Moved}};
            (void)appendInstruction(std::move(move));
            sourceArgument = {
                .kind = MirOperandKind::Value, .value = moved, .type = element};
          } else if (expansion.kind == HirCallInputKind::CopyValue) {
            sourceArgument = {.kind = MirOperandKind::Copy,
                              .place = elementPlace,
                              .type = element};
          } else {
            ExpressionInfo loadedInfo{.type = element,
                                      .category = ValueCategory::Value,
                                      .access = AccessMode::ReadOnly,
                                      .traits = expansion.traits};
            const MirValueId loaded = appendValue(input->id, loadedInfo);
            (void)appendInstruction({.kind = MirInstructionKind::Load,
                                     .hirValue = input->id,
                                     .result = loaded,
                                     .operands = {{.kind = MirOperandKind::Copy,
                                                   .place = elementPlace,
                                                   .type = element}},
                                     .info = loadedInfo});
            sourceArgument = {.kind = MirOperandKind::Value,
                              .value = loaded,
                              .type = element};
          }
          appendPreparedArgument(
              input->id, parameter,
              borrowed ? (parameter.referenceAccess == AccessMode::Mutable
                              ? HirCallInputKind::MutableBorrow
                              : HirCallInputKind::ReadBorrow)
                       : expansion.kind,
              std::move(sourceArgument),
              !borrowed && expansion.kind == HirCallInputKind::MoveValue
                  ? dropObligationForLocalPlace(output.findPlace(elementPlace))
                  : 0,
              packEntryLoans.contains(elementPlace)
                  ? packEntryLoans.at(elementPlace)
                  : 0);
        }
        emittedValues.insert(input->id);
      }
    } else {
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        const SemanticType parameter =
            index < value.parameterTypes.size()
                ? value.parameterTypes[index]
                : (findValue(arguments[index]) == nullptr
                       ? SemanticType::Unknown
                       : findValue(arguments[index])->info.type);
        call.parameterTypes.push_back(parameter);
        call.operands.push_back(argumentOperand(arguments[index], parameter));
        argumentDependencies.push_back(loanForValue(arguments[index]));
        if (terminated()) {
          return;
        }
      }
    }

    if (terminated()) {
      return;
    }
    call.result =
        call.noNormalBorrowResult ? std::nullopt
        : detachedBorrowResult
            ? std::optional<MirValueId>{appendValue(value.id, value.info)}
            : resultFor(value);

    const auto operandLoan = [&](const MirOperand &operand) {
      if (operand.kind == MirOperandKind::Loan) {
        return operand.loan;
      }
      const MirPlace *place = output.findPlace(operand.place);
      if (place == nullptr) {
        return MirLoanId{0};
      }
      if (place->root == MirPlaceRootKind::Loan) {
        return place->loan;
      }
      if (place->root == MirPlaceRootKind::Binding) {
        const auto found = bindingLoans.find(place->binding);
        if (found != bindingLoans.end()) {
          return found->second;
        }
      }
      if (place->root == MirPlaceRootKind::Symbol && place->capture != 0)
        return captureEntryLoan(place->symbol);
      if (place->root == MirPlaceRootKind::Value) {
        const MirValue *resolved = output.findValue(place->value);
        return resolved == nullptr ? MirLoanId{0}
                                   : loanForValue(resolved->sourceValue);
      }
      return MirLoanId{0};
    };
    MirLoanId originLoan = 0;
    if (value.borrowOrigin == BorrowOriginKind::Receiver) {
      const std::optional<MirOperand> &receiver =
          sourceReceiver ? sourceReceiver : call.receiver;
      if (receiver) {
        originLoan = operandLoan(*receiver);
      }
      // A consuming receiver moved out of a binding relays that binding's
      // retained loan, as a moved argument does.
      if (originLoan == 0)
        if (const auto source = receiverValue(value)) {
          const HirBindingId transferred = movedBorrowCarrierBinding(*source);
          const auto found = bindingLoans.find(transferred);
          if (transferred != 0 && found != bindingLoans.end())
            originLoan = found->second;
        }
    } else if (value.borrowOrigin == BorrowOriginKind::Argument &&
               value.borrowArgument < arguments.size() &&
               value.borrowArgument < call.operands.size()) {
      const MirOperand &argument = value.borrowArgument < sourceArguments.size()
                                       ? sourceArguments[value.borrowArgument]
                                       : call.operands[value.borrowArgument];
      originLoan = operandLoan(argument);
      if (originLoan == 0) {
        const HirBindingId transferred =
            movedBorrowCarrierBinding(arguments[value.borrowArgument]);
        const auto found = bindingLoans.find(transferred);
        if (transferred != 0 && found != bindingLoans.end()) {
          originLoan = found->second;
        }
      }
    }
    // A mapped reference-field result borrows the selected external referent,
    // not the address of a reference alias's carrier shell. Match the source
    // selection in borrowOriginPlace while retaining shell ancestry for
    // ordinary references into owned carrier storage.
    if (!value.borrowFields.empty() &&
        !value.borrowFields.front().sourceFields.empty()) {
      const auto &mapping = value.borrowFields.front();
      const auto mappedValue =
          mapping.origin == BorrowOriginKind::Receiver ? receiverValue(value)
          : mapping.origin == BorrowOriginKind::Argument &&
                  mapping.parameter < arguments.size()
              ? std::optional<HirValueId>{arguments[mapping.parameter]}
              : std::nullopt;
      const auto *alias = output.findLoan(originLoan);
      if (alias != nullptr && alias->kind != MirLoanKind::Stored &&
          alias->storedDependency != 0 &&
          !(mappedValue && isRangeIteratorValue(*mappedValue)))
        originLoan = alias->storedDependency;
    }
    MirInstruction originCall = call;
    if (sourceReceiver) {
      originCall.receiver = sourceReceiver;
    }
    if (!sourceArguments.empty()) {
      originCall.operands = sourceArguments;
    }
    MirPlaceId origin =
        call.noNormalBorrowResult ? 0 : borrowOriginPlace(value, originCall);
    // A reference field of `this` has an abstract entry Stored authority,
    // even though its receiver is not a local reference binding. Preserve
    // that authority when a checked accessor transforms the referent type.
    // The exact projected source remains separate from the parent's shell.
    // Only a referent-transforming accessor (checked restoration) takes that
    // abstract authority as its parent: an ordinary declared-origin call on
    // such a field keeps publishing a root result, as its caller-side
    // return-tail and accessor-chain rules expect.
    if (originLoan == 0 && value.borrowOrigin == BorrowOriginKind::Argument &&
        value.borrowArgument < arguments.size() &&
        (value.restoresReferenceResult ||
         value.intrinsic == IntrinsicKind::RestoreReference ||
         value.intrinsic == IntrinsicKind::RestoreReferenceMut)) {
      const auto *argument = findValue(arguments[value.borrowArgument]);
      if (argument && argument->storedReferenceAccess)
        originLoan = storedDependencyForPlace(origin);
    }
    const bool exactCarrierRelay =
        value.info.type.kind == SemanticType::Class &&
        value.info.traits.containsBorrowedState &&
        !value.info.traits.copyable &&
        value.borrowOrigin == BorrowOriginKind::Argument &&
        value.borrowArgument < value.parameterTypes.size() &&
        value.parameterTypes[value.borrowArgument] == value.info.type;
    std::vector<MirLoanId> movedArgumentLoans;
    // A copied borrow carrier shares its stored group with its source. Moving
    // one copy transfers only that carrier: the group remains active while
    // another binding still carries it, and its own endpoint retires it.
    std::unordered_map<MirLoanId, HirBindingId> sharedMovedCarriers;
    const auto recordMovedCarrier = [&](HirBindingId moved, MirLoanId loan) {
      if (std::find(movedArgumentLoans.begin(), movedArgumentLoans.end(),
                    loan) != movedArgumentLoans.end())
        return;
      movedArgumentLoans.push_back(loan);
      if (std::any_of(bindingLoans.begin(), bindingLoans.end(),
                      [&](const auto &entry) {
                        return entry.first != moved && entry.second == loan;
                      }))
        sharedMovedCarriers.emplace(loan, moved);
    };
    for (std::size_t index = 0;
         !construction && !value.constructorTarget &&
         (!value.borrowFields.empty() || exactCarrierRelay ||
          value.borrowDependencies.has_value()) &&
         index < arguments.size() && index < call.parameterTypes.size();
         ++index) {
      if (call.parameterTypes[index].kind == SemanticType::Reference)
        continue;
      const auto *argument = findValue(arguments[index]);
      if (argument == nullptr)
        continue;
      const auto moved = movedBorrowCarrierBinding(arguments[index]);
      const auto found = bindingLoans.find(moved);
      const auto *loan = found == bindingLoans.end()
                             ? nullptr
                             : output.findLoan(found->second);
      if (moved != 0 && loan != nullptr && loan->kind == MirLoanKind::Stored)
        recordMovedCarrier(moved, loan->id);
    }
    // A consuming method transfers its receiver by value just like an owned
    // formal. Its external references can survive, but the returned capability
    // must not remain a child of the consumed receiver's stored group.
    if (!construction && value.borrowDependencies && value.functionTarget) {
      const auto *target = program.findFunctionInstance(*value.functionTarget);
      const auto receiver = receiverValue(value);
      if (target && target->source && receiver &&
          target->source->receiverMutability() ==
              ReceiverMutability::Consuming) {
        const auto moved = movedBorrowCarrierBinding(*receiver);
        const auto found = bindingLoans.find(moved);
        const auto *loan = found == bindingLoans.end()
                               ? nullptr
                               : output.findLoan(found->second);
        if (moved && loan && loan->kind == MirLoanKind::Stored)
          recordMovedCarrier(moved, loan->id);
      }
    }
    call.borrowOrigin = value.borrowOrigin;
    call.borrowArgument = value.borrowArgument;
    call.borrowAccess = value.borrowAccess;
    call.borrowArguments = value.borrowArguments;
    call.borrowFields = value.borrowFields;
    call.dependentTextPublication = value.dependentTextPublication;
    call.provisionalTextRelays = value.provisionalTextRelays;
    call.borrowPlace = value.borrowPlace;
    call.restoresReferenceResult = value.restoresReferenceResult;
    call.restoredBorrowFields = value.restoredBorrowFields;
    // A text field result (L-OWN-02) holds what its origin holds in that
    // field: the retained loan of a dependent carrier that designates it, or
    // no dependency here at all (ownerless text, another field's owner, or a
    // formal's field whose text is the caller's).
    const bool textFieldResult =
        mir_detail::textFieldResult(value.info.type, value.borrowOrigin,
                                    value.borrowArgument, value.borrowFields);
    // A temporary argument holds its dependency as the Stored loan that its
    // publishing construction produced, the loan borrowOriginPlace selected.
    if (textFieldResult && originLoan == 0 &&
        value.borrowOrigin == BorrowOriginKind::Argument &&
        value.borrowArgument < arguments.size())
      originLoan = loanForValue(arguments[value.borrowArgument]);
    const bool heldTextField =
        textFieldResult && originLoan != 0 &&
        originLoan <= output.loans.size() &&
        output.loans[originLoan - 1].kind == MirLoanKind::Stored &&
        output.loans[originLoan - 1].designatedFields ==
            value.borrowFields.front().sourceFields;
    if (value.borrowOrigin != BorrowOriginKind::None && origin != 0 &&
        (!textFieldResult || heldTextField) &&
        (!value.borrowDependencies ||
         !value.borrowDependencies->dependencies.empty())) {
      const bool shellReference = borrowsCarrierStorage(value);
      const MirLoanKind kind =
          value.info.traits.containsBorrowedState && !shellReference
              ? MirLoanKind::Stored
              : MirLoanKind::CallResult;
      MirLoanId parent = 0;
      // A carrier alias's access reaches a receiver behind one of its stored
      // references only as far as that reference allows. Through a read-only
      // reference the alias cannot parent an exclusive carrier; the
      // reference's own stored entry is that carrier's authority instead.
      const MirOperand *selectedReceiver =
          value.borrowOrigin != BorrowOriginKind::Receiver ? nullptr
          : sourceReceiver                                 ? &*sourceReceiver
          : call.receiver                                  ? &*call.receiver
                                                           : nullptr;
      const MirPlace *selectedReceiverPlace =
          selectedReceiver ? output.findPlace(selectedReceiver->place)
                           : nullptr;
      const auto crossedAccess =
          selectedReceiverPlace
              ? storedReferenceAccessFor(*selectedReceiverPlace)
              : std::nullopt;
      if (originLoan != 0 && originLoan <= output.loans.size() &&
          std::find(movedArgumentLoans.begin(), movedArgumentLoans.end(),
                    originLoan) == movedArgumentLoans.end()) {
        const AccessMode parentAccess =
            output.loans[originLoan - 1].kind != MirLoanKind::Stored &&
                    crossedAccess
                ? (*crossedAccess == AccessMode::Mutable
                       ? output.loans[originLoan - 1].access
                       : AccessMode::ReadOnly)
                : output.loans[originLoan - 1].access;
        const bool derivedCallResult =
            kind == MirLoanKind::CallResult &&
            (parentAccess == AccessMode::Mutable ||
             value.borrowAccess == AccessMode::ReadOnly ||
             !value.borrowFields.empty());
        const bool derivedOriginCarrier =
            kind == MirLoanKind::Stored &&
            (parentAccess == AccessMode::Mutable ||
             exactReadonlyCleanupParent(value, originLoan) ||
             (output.loans[originLoan - 1].kind == MirLoanKind::Stored &&
              isLifecycleBoundBorrowCarrier(value.info.type)) ||
             (output.loans[originLoan - 1].kind == MirLoanKind::Stored &&
              output.loans[originLoan - 1].storedDependency != 0 &&
              value.info.traits.copyable &&
              value.borrowOrigin == BorrowOriginKind::Argument &&
              value.borrowArgument < value.parameterTypes.size() &&
              value.parameterTypes[value.borrowArgument].kind !=
                  SemanticType::Reference) ||
             // A read-only formal's entry loan parents only a read-only
             // carrier. An exclusive borrow taken through shared access
             // (synchronized storage) is a root loan, as it is when the
             // formal has no entry loan at all.
             (output.loans[originLoan - 1].kind == MirLoanKind::Parameter &&
              isLifecycleBoundBorrowCarrier(value.info.type) &&
              value.borrowArguments.empty() && value.borrowFields.size() <= 1 &&
              value.borrowAccess == AccessMode::ReadOnly) ||
             (output.loans[originLoan - 1].kind == MirLoanKind::CallResult &&
              value.borrowAccess == AccessMode::ReadOnly));
        if ((derivedCallResult || derivedOriginCarrier) &&
            !(shellReference &&
              output.loans[originLoan - 1].kind == MirLoanKind::Stored)) {
          parent = originLoan;
        }
      }
      // Moving an owned carrier into a new carrier forwards its external
      // ancestry; it does not borrow the consumed shell. In particular a
      // cleanup-bound result keeps the shared resource parameter alive.
      if (kind == MirLoanKind::Stored && originLoan != 0 &&
          originLoan <= output.loans.size() &&
          output.loans[originLoan - 1].kind == MirLoanKind::Stored &&
          value.borrowOrigin == BorrowOriginKind::Argument &&
          value.borrowArgument < arguments.size() &&
          value.borrowArgument < value.parameterTypes.size() &&
          value.parameterTypes[value.borrowArgument].kind !=
              SemanticType::Reference &&
          ((output.loans[originLoan - 1].lifecycleBound &&
            isLifecycleBoundBorrowCarrier(value.info.type)) ||
           exactCarrierRelay) &&
          movedBorrowCarrierBinding(arguments[value.borrowArgument]) != 0) {
        parent = output.loans[originLoan - 1].parent;
      }
      if (parent == 0 && kind == MirLoanKind::Stored &&
          isLifecycleBoundBorrowCarrier(value.info.type)) {
        const auto *dependency =
            output.findLoan(storedDependencyForPlace(origin));
        if (dependency != nullptr && dependency->entry &&
            dependency->kind == MirLoanKind::Stored)
          parent = dependency->id;
      }
      const auto retainedContract = retainedInitializerLoans.find(value.id);
      const HirLoan *shellContract =
          retainedContract == retainedInitializerLoans.end() ||
                  retainedContract->second == 0
              ? nullptr
              : source.findLoan(retainedContract->second);
      const HirValue *shellArgument =
          (value.constructorTarget || value.functionTarget) &&
                  value.borrowOrigin == BorrowOriginKind::Argument &&
                  value.borrowArgument < arguments.size()
              ? findValue(arguments[value.borrowArgument])
          : storesCarrierReference(value) && receiverValue(value)
              ? findValue(*receiverValue(value))
              : nullptr;
      const bool shellReferenceInput =
          storesCarrierReference(value) ||
          (value.borrowOrigin == BorrowOriginKind::Argument &&
           value.borrowArgument < value.parameterTypes.size() &&
           value.parameterTypes[value.borrowArgument].kind ==
               SemanticType::Reference);
      if (kind != MirLoanKind::Stored || shellContract == nullptr ||
          shellContract->kind != SemanticLoanKind::StoredValue ||
          shellContract->storedDependency == 0 || shellArgument == nullptr ||
          !shellArgument->info.traits.containsBorrowedState ||
          !shellReferenceInput)
        shellContract = nullptr;
      if (shellContract != nullptr) {
        origin = placeForLoan(*shellContract, shellArgument);
        parent = 0;
        if (shellContract->parent != 0) {
          const auto found = semanticLoans.find(shellContract->parent);
          if (found == semanticLoans.end())
            valid = false;
          else
            parent = found->second;
        }
      }
      // A fresh adapter over a local dynamic text view has no binding
      // contract. Like that contract, it borrows the exact view shell and
      // retains the shell's own owner loan; flattening the shell onto its
      // external text owner would lose the referent the adapter stores. A
      // parameter's entry already names its caller-owned authority.
      MirLoanId textShellOwner = 0;
      if (shellContract == nullptr && kind == MirLoanKind::Stored &&
          shellArgument != nullptr && shellReferenceInput &&
          (shellArgument->info.type.kind == SemanticType::StringView ||
           shellArgument->info.type.kind == SemanticType::CString) &&
          shellArgument->info.traits.containsBorrowedState) {
        const MirLoanId owner = loanForValue(shellArgument->id);
        const MirLoan *ownerLoan = output.findLoan(owner);
        const MirPlaceId shell = placeForValue(shellArgument->id);
        const MirPlace *shellPlace = output.findPlace(shell);
        if (ownerLoan != nullptr && ownerLoan->kind == MirLoanKind::Stored &&
            !ownerLoan->entry && shellPlace != nullptr &&
            shellPlace->root == MirPlaceRootKind::Binding &&
            shellPlace->projections.empty()) {
          origin = shell;
          parent = 0;
          textShellOwner = owner;
        }
      }
      // A carrier that stores a reference to the view an accessor returned
      // borrows that exact returned referent, whether the call result is
      // consumed directly (`b.get().begin()`) or first bound to a reference
      // (`std::string_view& view = b.get(); view.begin()`). The accessor's
      // result loan is its parent, and the carrier holds exactly that loan's
      // stored dependency. The accessor's owner, which the semantic contract
      // names for a temporary result, is not where the stored reference
      // points.
      MirLoanId accessorResult = 0;
      if (kind == MirLoanKind::Stored && textShellOwner == 0 &&
          value.borrowOrigin == BorrowOriginKind::Argument &&
          value.borrowArgument < arguments.size() &&
          value.borrowArgument < call.operands.size() &&
          storesCarrierReference(value)) {
        const MirOperand &argument =
            value.borrowArgument < sourceArguments.size()
                ? sourceArguments[value.borrowArgument]
                : call.operands[value.borrowArgument];
        const MirLoan *accessor = output.findLoan(originLoan);
        if (accessor != nullptr &&
            designatesAccessorReferent(
                argument, *accessor,
                value.parameterTypes[value.borrowArgument])) {
          const MirPlace *selected = output.findPlace(origin);
          if (selected == nullptr || selected->root != MirPlaceRootKind::Loan ||
              selected->loan != accessor->id ||
              !selected->projections.empty()) {
            const MirPlace &referent = *output.findPlace(argument.place);
            origin = referent.root == MirPlaceRootKind::Loan
                         ? argument.place
                         : appendPlace({.root = MirPlaceRootKind::Loan,
                                        .loan = accessor->id,
                                        .type = referent.type,
                                        .access = referent.access,
                                        .traits = referent.traits,
                                        .sourceValue =
                                            arguments[value.borrowArgument]});
          }
          parent = accessor->id;
          accessorResult = accessor->id;
        }
      }
      const bool twoPhase =
          reserveBorrowResult && stableReservationOrigin(origin);
      const MirLoanId loan = createLoan(kind, origin, value.borrowAccess,
                                        value.id, 0, 0, parent, twoPhase);
      if (shellContract != nullptr) {
        recordLoanIdentity(loan, *shellContract);
      } else if (textShellOwner != 0) {
        output.loans[loan - 1].storedDependency = textShellOwner;
      } else if (kind == MirLoanKind::Stored && shellArgument != nullptr &&
                 (shellArgument->info.traits.containsBorrowedState ||
                  storesCarrierReference(value)) &&
                 shellReferenceInput && value.borrowArguments.empty() &&
                 value.borrowFields.size() <= 1 &&
                 (value.borrowFields.empty() ||
                  value.borrowFields.front().sourceFields.empty())) {
        // A directly returned adapter has no local binding's retained-loan
        // record. Its exact reference-parameter contract still keeps the
        // referenced carrier's external dependencies alive, separately from
        // the physical field address stored by the adapter.
        output.loans[loan - 1].storedDependency =
            storedDependencyForPlace(origin);
      } else if (shellReference) {
        output.loans[loan - 1].storedDependency =
            storedDependencyForPlace(origin);
        if (output.loans[loan - 1].storedDependency == 0 &&
            value.restoresReferenceResult &&
            !value.restoredBorrowFields.empty())
          output.loans[loan - 1].storedDependency = originLoan;
        // A text reference may select a literal-backed local. The program
        // verifier independently proves that case from its initialization;
        // every dynamic text source still requires its exact owner witness.
        if (output.loans[loan - 1].storedDependency == 0 &&
            value.info.type.kind != SemanticType::StringView &&
            value.info.type.kind != SemanticType::CString)
          valid = false;
      } else if (parent != 0) {
        // A referent returned through an existing carrier reference remains
        // a child of that physical reference. Preserve its already-verified
        // external dependency too (for example iterator-reference operator*).
        // A derived stored iterator also retains this external dependency
        // while its parent protects the borrowed receiver shell.
        output.loans[loan - 1].storedDependency =
            output.loans[parent - 1].storedDependency;
      }
      if (accessorResult != 0 &&
          output.loans[loan - 1].storedDependency !=
              output.loans[accessorResult - 1].storedDependency)
        valid = false;
      const MirLoanId dependencyLoan =
          originLoan != 0 ? originLoan
          : value.borrowOrigin == BorrowOriginKind::Argument &&
                  value.borrowArgument < arguments.size()
              ? loanForValue(arguments[value.borrowArgument])
              : 0;
      // A physical carrier borrow retains its external field set through the
      // separately verified stored-dependency edge. Copying those sources
      // into the shell loan would mix physical storage and external authority
      // (and attach the input's field paths to a differently shaped cursor).
      // Mapped result fields below still publish their exact explicit paths.
      // A receiver behind one stored reference field depends only on that
      // field's referent, which its source already names; the carrier's other
      // owners are not dependencies of the result.
      if (!shellReference && !crossedAccess &&
          output.loans[loan - 1].storedDependency == 0 && dependencyLoan != 0 &&
          dependencyLoan <= output.loans.size()) {
        for (const auto &dependency :
             output.loans[dependencyLoan - 1].additionalDependencies)
          if (dependency.source != 0)
            output.loans[loan - 1].additionalDependencies.push_back(dependency);
      }
      for (const std::size_t parameter : value.borrowArguments) {
        if (parameter >= originCall.operands.size()) {
          valid = false;
          continue;
        }
        HirValue dependency = value;
        dependency.borrowArgument = parameter;
        const MirPlaceId source = borrowOriginPlace(dependency, originCall);
        const MirLoanId inherited = operandLoan(originCall.operands[parameter]);
        const AccessMode access =
            parameter < value.parameterTypes.size() &&
                    value.parameterTypes[parameter].kind ==
                        SemanticType::Reference
                ? value.parameterTypes[parameter].referenceAccess
            : inherited != 0 && inherited <= output.loans.size()
                ? output.loans[inherited - 1].access
                : AccessMode::ReadOnly;
        if (source != 0 && source != origin) {
          output.loans[loan - 1].additionalDependencies.push_back(
              {.source = source, .access = access});
        }
        if (inherited != 0 && inherited <= output.loans.size()) {
          const auto inheritedDependencies =
              output.loans[inherited - 1].additionalDependencies;
          for (const auto &additional : inheritedDependencies) {
            auto &dependencies = output.loans[loan - 1].additionalDependencies;
            if (additional.source != 0 && additional.source != origin &&
                std::find(dependencies.begin(), dependencies.end(),
                          additional) == dependencies.end()) {
              dependencies.push_back(additional);
            }
          }
        }
      }
      if (!value.borrowFields.empty()) {
        auto &published = output.loans[loan - 1];
        published.designatedFields = value.borrowFields.front().resultFields;
        published.additionalDependencies.clear();
        for (std::size_t index = 1; index < value.borrowFields.size();
             ++index) {
          const auto &mapping = value.borrowFields[index];
          HirValue selected = value;
          selected.borrowOrigin = mapping.origin;
          selected.borrowArgument = mapping.parameter;
          selected.borrowFields = {mapping};
          const auto source = borrowOriginPlace(selected, originCall);
          if (source == 0)
            valid = false;
          published.additionalDependencies.push_back(
              {.source = source,
               .designatedFields = mapping.resultFields,
               .access = mapping.access});
        }
      }
      if (value.borrowDependencies) {
        std::vector<MirLoanDependency> complete;
        for (const auto &dependency : value.borrowDependencies->dependencies) {
          const auto lowered = lowerCompleteBorrowDependency(dependency);
          if (!lowered) {
            valid = false;
            return;
          }
          auto selected = *lowered;
          // A consumed carrier's complete external permission is transferred
          // through the call. The result must not remain a child of the group
          // that the successful invocation retires below.
          if (std::find(movedArgumentLoans.begin(), movedArgumentLoans.end(),
                        selected.parent) != movedArgumentLoans.end()) {
            const auto *previous = output.findLoan(selected.parent);
            if (!previous) {
              valid = false;
              return;
            }
            std::optional<MirLoanId> externalParent;
            bool ambiguous = false;
            const auto consider = [&](MirPlaceId source, AccessMode access,
                                      MirLoanId parent) {
              if (!sameBorrowSource(source, selected.source) ||
                  (selected.access == AccessMode::Mutable &&
                   access != AccessMode::Mutable))
                return;
              if (externalParent && *externalParent != parent)
                ambiguous = true;
              else
                externalParent = parent;
            };
            consider(previous->source, previous->access, previous->parent);
            for (const auto &extra : previous->additionalDependencies)
              consider(extra.source, extra.access, extra.parent);
            // A parameter's entry group names its caller-owned referent only
            // through its designated reference path. Transferring that group
            // keeps the caller's permission, which has no MIR parent here.
            const auto entryReferent = [&]() {
              const MirPlace *group = output.findPlace(previous->source);
              const MirPlace *referent = output.findPlace(selected.source);
              if (!previous->entry || previous->kind != MirLoanKind::Stored ||
                  previous->designatedFields.empty() || group == nullptr ||
                  referent == nullptr || group->root != referent->root ||
                  group->binding != referent->binding ||
                  group->symbol != referent->symbol ||
                  referent->projections.size() !=
                      group->projections.size() +
                          previous->designatedFields.size() + 1 ||
                  referent->projections.back().kind !=
                      MirProjectionKind::Dereference)
                return false;
              for (std::size_t index = 0; index < group->projections.size();
                   ++index)
                if (referent->projections[index].kind !=
                        group->projections[index].kind ||
                    referent->projections[index].field !=
                        group->projections[index].field)
                  return false;
              for (std::size_t index = 0;
                   index < previous->designatedFields.size(); ++index) {
                const auto &projection =
                    referent->projections[group->projections.size() + index];
                if (projection.kind != MirProjectionKind::Field ||
                    projection.field != previous->designatedFields[index])
                  return false;
              }
              return true;
            };
            if (!externalParent && !ambiguous && entryReferent())
              externalParent = previous->parent;
            if (!externalParent || ambiguous) {
              valid = false;
              return;
            }
            if (kind == MirLoanKind::CallResult) {
              auto &transfers = callResultTransferredFrom[loan];
              if (std::find(transfers.begin(), transfers.end(), previous->id) ==
                  transfers.end())
                transfers.push_back(previous->id);
            }
            selected.parent = *externalParent;
          }
          complete.push_back(std::move(selected));
        }
        if (complete.empty()) {
          valid = false; // No origin may be invented for a borrowed result.
          return;
        }
        // A reference input read through a call result's loan is a child of
        // that accessor result, not of the accessor's own parent named by
        // the semantic contract. Keep the accessor in the published ancestry.
        std::vector<const MirLoan *> accessors;
        for (std::size_t index = 0; index < call.operands.size(); ++index) {
          const MirOperand &input = index < sourceArguments.size()
                                        ? sourceArguments[index]
                                        : call.operands[index];
          const auto *accessor = output.findLoan(operandLoan(input));
          if (accessor && accessor->kind == MirLoanKind::CallResult &&
              index < call.parameterTypes.size() &&
              call.parameterTypes[index].kind == SemanticType::Reference)
            accessors.push_back(accessor);
        }
        for (auto &dependency : complete) {
          const MirLoan *selected = nullptr;
          bool ambiguous = false;
          for (const auto *accessor : accessors)
            if (accessor->id != dependency.parent &&
                accessor->parent == dependency.parent &&
                sameBorrowSource(accessor->source, dependency.source)) {
              ambiguous |= selected && selected != accessor;
              selected = accessor;
            }
          if (selected && !ambiguous) {
            dependency.parent = selected->id;
            dependency.source = selected->source;
          }
        }
        // A parentless referent is named by the exact place through which
        // the call's mapped input supplies it, not by an earlier evaluation
        // of the same reference formal: the verifier reconstructs this call's
        // own inputs. A reference input reads its referent's place; a carrier
        // passed by value supplies the sources of its own Stored group.
        const auto stagedOperand =
            [&](const MirOperand &input) -> const MirOperand * {
          const MirOperand *cursor = &input;
          while (cursor->kind == MirOperandKind::Value) {
            const MirValue *staged = output.findValue(cursor->value);
            if (!staged || staged->definitionBlock == 0 ||
                staged->definitionBlock > output.blocks.size())
              return nullptr;
            const MirInstruction *stage = nullptr;
            for (const auto &instruction :
                 output.blocks[staged->definitionBlock - 1].instructions)
              if (instruction.id == staged->definition)
                stage = &instruction;
            if (!stage || stage->operands.size() != 1 ||
                (stage->kind != MirInstructionKind::CallInput &&
                 stage->kind != MirInstructionKind::Move))
              return nullptr;
            cursor = &stage->operands.front();
          }
          return cursor;
        };
        std::vector<MirPlaceId> argumentPlaces;
        const auto addPlace = [&](MirPlaceId place) {
          if (place != 0 &&
              std::find(argumentPlaces.begin(), argumentPlaces.end(), place) ==
                  argumentPlaces.end())
            argumentPlaces.push_back(place);
        };
        std::vector<std::size_t> mappedParameters = value.borrowArguments;
        if (value.borrowOrigin == BorrowOriginKind::Argument)
          mappedParameters.push_back(value.borrowArgument);
        for (const auto &mapping : value.borrowFields)
          if (mapping.origin == BorrowOriginKind::Argument)
            mappedParameters.push_back(mapping.parameter);
        for (const std::size_t parameter : mappedParameters) {
          if (parameter >= call.operands.size() ||
              parameter >= call.parameterTypes.size())
            continue;
          const MirOperand *input = stagedOperand(
              parameter < sourceArguments.size() ? sourceArguments[parameter]
                                                 : call.operands[parameter]);
          const MirPlace *place =
              input ? output.findPlace(input->place) : nullptr;
          if (!place)
            continue;
          if (call.parameterTypes[parameter].kind == SemanticType::Reference) {
            addPlace(place->id);
            continue;
          }
          const MirLoan *group =
              place->root == MirPlaceRootKind::Binding &&
                      place->projections.empty()
                  ? (bindingLoans.contains(place->binding)
                         ? output.findLoan(bindingLoans.at(place->binding))
                         : nullptr)
              : place->root == MirPlaceRootKind::Value ||
                      place->root == MirPlaceRootKind::Temporary
                  ? output.findLoan(loanForValue(place->sourceValue))
                  : nullptr;
          if (!group || group->kind != MirLoanKind::Stored)
            continue;
          addPlace(group->source);
          for (const auto &extra : group->additionalDependencies)
            addPlace(extra.source);
        }
        for (auto &dependency : complete) {
          if (dependency.parent != 0)
            continue;
          MirPlaceId selected = 0;
          bool ambiguous = false;
          for (const auto place : argumentPlaces)
            if (place != dependency.source &&
                sameBorrowSource(place, dependency.source)) {
              ambiguous |= selected != 0;
              selected = place;
            }
          if (selected != 0 && !ambiguous &&
              std::find(argumentPlaces.begin(), argumentPlaces.end(),
                        dependency.source) == argumentPlaces.end())
            dependency.source = selected;
        }
        const auto &primary = complete.front();
        auto &published = output.loans[loan - 1];
        published.source = primary.source;
        published.access = primary.access;
        published.parent = primary.parent;
        published.designatedFields = primary.designatedFields;
        published.conditions = primary.conditions;
        published.additionalDependencies.assign(complete.begin() + 1,
                                                complete.end());
        if (!call.result) {
          valid = false;
          return;
        }
        // The result value's transport may precede the places its call's
        // inputs materialize; publish the same source-established set now.
        auto &resultDependencies =
            output.values[*call.result - 1].borrowDependencies;
        if (!resultDependencies)
          resultDependencies = MirBorrowDependencySet{
              .possibleAlternatives =
                  value.borrowDependencies->possibleAlternatives};
        resultDependencies->dependencies = std::move(complete);
      }
      call.loan = loan;
      valueLoans.insert_or_assign(value.id, loan);
      if (!scopes.empty()) {
        scopes.back().loans.push_back(loan);
      }
    }
    if ((value.intrinsic == IntrinsicKind::ObjectDestroy ||
         value.intrinsic == IntrinsicKind::OptionalStorageReset) &&
        !call.operands.empty()) {
      const SemanticType &parameter = call.operands.front().type;
      const SemanticType &storage = parameter.kind == SemanticType::Reference &&
                                            parameter.arguments.size() == 1
                                        ? parameter.arguments.front()
                                        : parameter;
      if (storage.arguments.size() == 1) {
        std::unordered_set<HirClassInstanceId> visiting;
        call.elementCleanup =
            uniqueCleanupType(storage.arguments.front(), visiting);
        if (call.elementCleanup) {
          if (mirDropHasPolymorphicCleanup(*call.elementCleanup))
            call.definedFailure.propagation =
                FailurePropagationKind::Destructor;
          for (const auto target :
               mirDropDestructorTargets(*call.elementCleanup)) {
            if (target == 0 ||
                target > definedFailureEffects.destructors.size() ||
                definedFailureEffects.destructors[target - 1]) {
              call.definedFailure.propagation =
                  FailurePropagationKind::Destructor;
              break;
            }
          }
        }
      }
    }
    if (value.intrinsic == IntrinsicKind::AllocateUniqueOwner &&
        value.info.type.kind == SemanticType::UniqueOwner &&
        value.info.type.arguments.size() == 1 &&
        value.info.type.arguments.front().kind == SemanticType::Class &&
        ((value.constructorTarget &&
          value.constructorKind == ConstructorKind::Ordinary) ||
         mir_detail::generatedUniqueConstruction(call))) {
      // Prepared parameters remain caller-owned until allocation succeeds.
      // The second instruction transfers them into the selected constructor;
      // its failure path owns only the unconstructed allocation, not T.
      call.objectAllocation = appendObjectAllocation(value);
      call.definedFailure.localOrigins.clear();
      call.definedFailure.propagation =
          value.constructorTarget ? normalizedConstructorPropagation(
                                        FailurePropagationKind::Constructor,
                                        value.constructorTarget)
                                  : FailurePropagationKind::None;
    }
    if (value.intrinsic == IntrinsicKind::ThreadScope) {
      if (value.scopedThreadTasks.size() != call.operands.size() ||
          argumentDependencies.size() != call.operands.size()) {
        valid = false;
      } else {
        for (std::size_t index = 0; index < call.operands.size(); ++index) {
          const auto *adapter =
              program.findThreadTaskAdapter(value.scopedThreadTasks[index]);
          const auto loan =
              adapter != nullptr && !adapter->scopeDependencies.empty()
                  ? argumentDependencies[index]
                  : MirLoanId{0};
          if (adapter == nullptr || !adapter->scoped ||
              (!adapter->scopeDependencies.empty() && loan == 0))
            valid = false;
          if (const auto *entry = output.findLoan(loan);
              entry != nullptr && entry->entry &&
              entry->kind == MirLoanKind::Stored) {
            // Entry contracts name reference-field paths relative to the
            // owned carrier (including an exact pack element). Materialize
            // those typed places so verification can replay each referent.
            const auto contract = *entry;
            (void)projectBorrowFields(contract.source,
                                      contract.designatedFields,
                                      contract.conditions);
            for (const auto &dependency : contract.additionalDependencies)
              (void)projectBorrowFields(contract.source,
                                        dependency.designatedFields,
                                        dependency.conditions);
          }
          call.scopedThreadTasks.push_back(
              {.adapter = value.scopedThreadTasks[index], .dependency = loan});
        }
      }
    }
    if (!value.callPlan && !expandsPack) {
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        const SemanticType parameter =
            index < value.parameterTypes.size()
                ? value.parameterTypes[index]
                : (findValue(arguments[index]) == nullptr
                       ? SemanticType::Unknown
                       : findValue(arguments[index])->info.type);
        if (parameter.kind != SemanticType::Reference) {
          transferTemporaryOut(call, arguments[index]);
        }
      }
    }
    for (const MirDropObligationId parameterDrop : preparedParameterDrops) {
      if (!removeTemporary(parameterDrop)) {
        valid = false;
      }
      appendLifecycle(call, {.kind = MirLifecycleEventKind::TransferOut,
                             .source = parameterDrop});
    }
    const MirDropObligationId resultDrop =
        detachedBorrowResult ? 0 : dropObligationForValue(value.id);
    const bool transferableBorrowedResult =
        call.loan &&
        (value.info.type.kind == SemanticType::Class ||
         value.info.type.kind == SemanticType::Enum) &&
        value.info.category == ValueCategory::Value &&
        value.info.traits.movable && value.info.traits.containsBorrowedState;
    // A cleanup-owning result exists only after a failure-capable call
    // succeeds, regardless of whether it is the full-expression root or a
    // nested input to another call. Arm its value obligation on the Invoke
    // success edge; the normal successor then owns the temporary until a
    // later CallInput reparents it or full-expression cleanup drops it.
    const bool successEdgeResult =
        resultDrop != 0 && routeFailureEdgesHere() &&
        call.definedFailure.mayUnwind() && !call.destination &&
        (!call.loan || transferableBorrowedResult ||
         borrowedTemporaryTransfers.contains(value.id)) &&
        !value.ownership;
    if (successEdgeResult) {
      call.successResultDrop = resultDrop;
    } else if (!detachedBorrowResult) {
      (void)initializeValueLifecycle(call, value);
    }
    const MirDropObligationId allocation = call.objectAllocation;
    if ((construction ||
         value.intrinsic == IntrinsicKind::DefaultTypeParameterConstruction) &&
        call.operands.empty()) {
      call.callSite = 0;
    }
    const MirLoanId publishedLoan = call.loan.value_or(0);
    (void)appendInstruction(std::move(call));
    // A by-value move transfers the shell and every dependency to the callee.
    // A returned carrier publishes its own checked dependency set; it is not
    // a child reborrow of the now-consumed caller shell.
    for (const auto loan : movedArgumentLoans) {
      if (const auto shared = sharedMovedCarriers.find(loan);
          shared != sharedMovedCarriers.end()) {
        bindingLoans.erase(shared->second);
        sharedTransferredGroups.insert(loan);
        continue;
      }
      const auto *previous = output.findLoan(loan);
      const auto *published = output.findLoan(publishedLoan);
      const auto resultIdentity = retainedInitializerLoans.find(value.id);
      if (exactCarrierRelay && loan == originLoan && previous != nullptr &&
          published != nullptr && published->kind == MirLoanKind::Stored &&
          previous->access == published->access &&
          previous->parent == published->parent &&
          previous->lifecycleBound == published->lifecycleBound &&
          previous->storedDependency == published->storedDependency &&
          previous->additionalDependencies ==
              published->additionalDependencies &&
          sameBorrowSource(previous->source, published->source)) {
        // Semantic carrier membership describes the completed source flow;
        // intermediate moved locals can have no retained-loan annotation.
        // Transport a preserved identity at the exact consuming call, not by
        // later resurrecting an already retired formal's runtime capability.
        // A distinct retained result has its own semantic endpoint: redirecting
        // the input identity would let the input's last use retire that result.
        // An initializer entry with zero identity still retains the carrier
        // in a named binding and must continue the input identity. Absence of
        // an entry instead identifies an unretained temporary, with its own
        // full-expression endpoint, even when relayed through another call.
        // Its physical dependencies still travel with the value; the input's
        // semantic acknowledgement stays
        // on the consumed loan instead of following that temporary.
        bool transported = false;
        for (auto &[semantic, currentLoan] : semanticLoans) {
          if (currentLoan == loan &&
              resultIdentity != retainedInitializerLoans.end() &&
              (resultIdentity->second == 0 ||
               resultIdentity->second == semantic)) {
            currentLoan = publishedLoan;
            transported = true;
          }
        }
        if (transported)
          output.loans[loan - 1].semanticLoan = 0;
      }
      (void)appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = loan});
      removeLoanFromState(loan, scopes, bindingLoans);
      // HIR can also end this consumed capability at a later last-use statement
      // when a result retains its external origins under a new identity.
      // The call has already emitted its exact executable endpoint; remember
      // only that statement's acknowledgement rather than accepting arbitrary
      // inactive loans (or emitting a second EndBorrow).
      const auto expression = std::find_if(
          source.fullExpressions.begin(), source.fullExpressions.end(),
          [&](const auto &candidate) {
            return candidate.id == value.fullExpression;
          });
      if (expression != source.fullExpressions.end() &&
          expression->statement != 0) {
        HirStatementId endpoint = expression->statement;
        const HirStatement *owner = findStatement(endpoint);
        const bool endsHere =
            owner != nullptr &&
            std::any_of(owner->endedLoans.begin(), owner->endedLoans.end(),
                        [&](SemanticLoanId semantic) {
                          const auto found = semanticLoans.find(semantic);
                          return found != semanticLoans.end() &&
                                 found->second == loan;
                        });
        if (!endsHere) {
          const auto exact =
              previous == nullptr
                  ? uniqueSemanticLoanEndpoints.end()
                  : uniqueSemanticLoanEndpoints.find(previous->semanticLoan);
          if (exact != uniqueSemanticLoanEndpoints.end() && exact->second != 0)
            endpoint = exact->second;
          else {
            const auto enclosing = enclosingTransferredLoanEndpoints.find(loan);
            if (enclosing != enclosingTransferredLoanEndpoints.end())
              endpoint = enclosing->second;
          }
        }
        callTransferredLoanEndpoints.insert_or_assign(loan, endpoint);
      }
    }
    if (allocation != 0) {
      // appendInstruction leaves us on the construction-success continuation.
      // No failure edge may relinquish unconstructed storage to the result.
      (void)appendInstruction(
          {.kind = MirInstructionKind::Lifecycle,
           .hirValue = value.id,
           .objectAllocation = allocation,
           .lifecycle = {{.kind = MirLifecycleEventKind::TransferOut,
                          .source = allocation}}});
      if (!removeTemporary(allocation)) {
        valid = false;
      }
    }
    for (auto loan = temporaryArgumentLoans.rbegin();
         loan != temporaryArgumentLoans.rend(); ++loan) {
      (void)appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = *loan});
      removeLoanFromState(*loan, scopes, bindingLoans);
    }
    if (sourceReceiverValue) {
      endConsumedCallResultLoans({*sourceReceiverValue}, value);
    }
  }

  void emitPackFold(const HirValue &value) {
    if (value.info.type != SemanticType::Void || value.packFoldSymbol == 0 ||
        value.packFoldParameter == 0 || value.packFoldFunction == 0 ||
        value.operands.size() !=
            value.packFoldArgument + 1 + value.packFoldElements.size()) {
      valid = false;
      return;
    }

    const auto *pack = findValue(value.operands[value.packFoldArgument]);
    if (!pack || pack->symbol != value.packFoldSymbol ||
        pack->info.type.kind != SemanticType::TypePack ||
        !pack->info.type.concretePack ||
        pack->info.type.genericParameterId != value.packFoldParameter ||
        pack->info.type.arguments.size() != value.packFoldElements.size()) {
      valid = false;
      return;
    }
    for (std::size_t elementIndex = 0;
         elementIndex < value.packFoldElements.size(); ++elementIndex) {
      const HirPackFoldElement &element = value.packFoldElements[elementIndex];
      const auto *call = findValue(element.call);
      if (element.functionTarget == 0 ||
          element.elementType != pack->info.type.arguments[elementIndex] ||
          element.parameterTypes.size() != value.packFoldArgument + 1 ||
          !call || call->kind != HirValueKind::Call ||
          call->functionTarget != element.functionTarget ||
          call->parameterTypes != element.parameterTypes ||
          value.operands[value.packFoldArgument + 1 + elementIndex] !=
              element.call ||
          !call->callPlan) {
        valid = false;
        return;
      }

      // HIR has already expanded each concrete element into its own
      // ordinary invocation. Reuse its canonical left-to-right CallInput
      // schedule, including protection across later TLS initialization.
      emitValue(element.call);
      if (terminated()) {
        emittedValues.insert(value.id);
        return;
      }
    }

    emittedValues.insert(value.id);
  }

  void emitSpecialMemberPackConstruction(const HirValue &value,
                                         const HirValue &pack) {
    const bool moved = value.constructorKind == ConstructorKind::Move;
    const auto local = localSymbols.find(pack.symbol);
    const MirPlace *root = output.findPlace(
        local == localSymbols.end() ? 0 : placeForBinding(local->second));
    const bool selectedSpecialMember =
        moved || value.constructorKind == ConstructorKind::Copy;
    if (!selectedSpecialMember || value.constructorTarget || value.callPlan ||
        value.info.type.kind != SemanticType::Class ||
        value.info.traits.containsBorrowedState ||
        !value.definedFailure.empty() ||
        pack.kind != HirValueKind::PackExpansion ||
        pack.info.type.kind != SemanticType::TypePack ||
        !pack.info.type.concretePack || pack.info.type.arguments.size() != 1 ||
        pack.info.type.arguments.front() != value.info.type ||
        pack.packExpansionElements.size() != 1 || root == nullptr ||
        root->root != MirPlaceRootKind::Binding || !root->projections.empty() ||
        root->type != pack.info.type || root->symbol != pack.symbol ||
        !root->key || value.parameterTypes.size() != 1 ||
        value.parameterTypes.front().kind != SemanticType::Reference ||
        value.parameterTypes.front().arguments.size() != 1 ||
        value.parameterTypes.front().arguments.front() != value.info.type) {
      valid = false;
      return;
    }
    const HirPackExpansionElement &element = pack.packExpansionElements.front();
    if (element.type != value.info.type ||
        element.kind != (moved ? HirCallInputKind::MoveValue
                               : HirCallInputKind::CopyValue) ||
        (moved ? !element.traits.movable : !element.traits.copyable)) {
      valid = false;
      return;
    }
    // A generated special member transfers one exact element; it has no
    // ordinary constructor body or by-value parameter to prepare. Use the
    // existing copy/move operation and canonical pack-element lifetime.
    PlaceKey key = *root->key;
    key.projections.push_back(
        {.kind = PlaceProjectionKind::ConstantIndex, .index = 0});
    MirPlace projected = *root;
    projected.id = 0;
    projected.projections.push_back(
        {.kind = MirProjectionKind::PackElement, .constantIndex = 0});
    projected.type = element.type;
    projected.traits = element.traits;
    projected.sourceValue = pack.id;
    projected.key = key;
    const MirPlaceId place = appendPackElementPlace(std::move(projected));
    MirInstruction transfer{.kind = moved ? MirInstructionKind::Move
                                          : MirInstructionKind::Load,
                            .hirValue = value.id,
                            .result = resultFor(value),
                            .operands = {{.kind = moved ? MirOperandKind::Move
                                                        : MirOperandKind::Copy,
                                          .place = place,
                                          .type = element.type}},
                            .sourceLocation = value.sourceLocation,
                            .info = value.info};
    if (moved) {
      transfer.ownership =
          OwnershipEvent{.kind = OwnershipEventKind::Move,
                         .place = key,
                         .before = OwnershipStateSet::Available,
                         .after = OwnershipStateSet::Moved};
      const MirDropObligationId sourceDrop =
          dropObligationForLocalPlace(output.findPlace(place));
      const MirDropObligationId targetDrop = dropObligationForValue(value.id);
      if (targetDrop != 0) {
        if (sourceDrop == 0) {
          valid = false;
          return;
        }
        appendLifecycle(transfer, {.kind = MirLifecycleEventKind::Move,
                                   .source = sourceDrop,
                                   .target = targetDrop});
        registerTemporary(targetDrop);
      } else if (sourceDrop != 0 && output.findDropObligation(sourceDrop)
                                        ->dropType.requiresActiveCleanup) {
        valid = false;
        return;
      }
    } else {
      (void)initializeValueLifecycle(transfer, value);
    }
    (void)appendInstruction(std::move(transfer), false);
  }

  void emitConstruct(const HirValue &value) {
    if (value.callPlan && value.constructorKind == ConstructorKind::Ordinary) {
      // Ordinary constructors own their parameters just like functions.
      // Prepare each argument before evaluating its successor, retain its
      // rollback obligation, then transfer only at the invocation boundary.
      // Do not hand off a prvalue while later arguments can still fail.
      emitCall(value, false, false, true);
      return;
    }
    const std::vector<HirValueId> arguments = callArgumentValues(value);
    if (value.constructorKind != ConstructorKind::Ordinary &&
        arguments.size() == 1) {
      const HirValue *input = findValue(arguments.front());
      if (input != nullptr && input->kind == HirValueKind::PackExpansion) {
        emitSpecialMemberPackConstruction(value, *input);
        return;
      }
    }
    if (std::any_of(arguments.begin(), arguments.end(),
                    [&](HirValueId argument) {
                      const HirValue *input = findValue(argument);
                      return input != nullptr &&
                             input->kind == HirValueKind::PackExpansion;
                    })) {
      // Pack element preparation, ownership transfer and rollback are the same
      // call-input protocol for functions and selected constructors.
      emitCall(value, false, false, true);
      return;
    }
    DefinedFailureOperation failure = value.definedFailure;
    if (failure.localOrigins.empty()) {
      failure.propagation = normalizedConstructorPropagation(
          failure.propagation, value.constructorTarget);
    }
    MirInstruction construct{.kind = MirInstructionKind::Construct,
                             .hirValue = value.id,
                             .callSite = value.callPlan ? value.id : 0,
                             .result = resultFor(value),
                             .definedFailure = std::move(failure),
                             .constructorTarget = value.constructorTarget,
                             .constructorKind = value.constructorKind,
                             .info = value.info};
    std::vector<MirOperand> sourceArguments;
    sourceArguments.reserve(arguments.size());
    if (value.callPlan) {
      if (value.callPlan->receiver ||
          value.callPlan->arguments.size() != arguments.size()) {
        valid = false;
        return;
      }
      for (const HirCallArgument &argument : value.callPlan->arguments) {
        construct.parameterTypes.push_back(argument.parameterType);
        sourceArguments.push_back(
            argument.kind == HirCallInputKind::CopyValue
                ? copyArgumentOperand(argument.value, argument.parameterType)
                : argumentOperand(argument.value, argument.parameterType));
        construct.operands.push_back(
            prepareCallInput(value.id, argument.value, argument.parameterType,
                             argument.kind, MirCallInputRole::Argument,
                             argument.parameterIndex, sourceArguments.back(),
                             false, argument.defaultArgument)
                .operand);
      }
    } else {
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        const SemanticType parameter =
            index < value.parameterTypes.size()
                ? value.parameterTypes[index]
                : (findValue(arguments[index]) == nullptr
                       ? SemanticType::Unknown
                       : findValue(arguments[index])->info.type);
        construct.parameterTypes.push_back(parameter);
        construct.operands.push_back(
            value.constructorKind != ConstructorKind::Move
                ? argumentOperand(arguments[index], parameter)
                : valueOperand(arguments[index]));
      }
    }
    MirInstruction originConstruct = construct;
    if (!sourceArguments.empty()) {
      originConstruct.operands = sourceArguments;
    }
    const auto operandLoan = [&](const MirOperand &operand) {
      if (operand.kind == MirOperandKind::Loan) {
        return operand.loan;
      }
      if (operand.kind == MirOperandKind::Value) {
        const MirValue *resolved = output.findValue(operand.value);
        return resolved == nullptr ? MirLoanId{0}
                                   : loanForValue(resolved->sourceValue);
      }
      const MirPlace *place = output.findPlace(operand.place);
      if (place == nullptr) {
        return MirLoanId{0};
      }
      if (place->root == MirPlaceRootKind::Loan) {
        return place->loan;
      }
      if (place->root == MirPlaceRootKind::Binding) {
        const auto found = bindingLoans.find(place->binding);
        return found == bindingLoans.end() ? MirLoanId{0} : found->second;
      }
      if (place->root == MirPlaceRootKind::Symbol && place->capture != 0)
        return captureEntryLoan(place->symbol);
      if (place->root == MirPlaceRootKind::Value) {
        const MirValue *resolved = output.findValue(place->value);
        return resolved == nullptr ? MirLoanId{0}
                                   : loanForValue(resolved->sourceValue);
      }
      return MirLoanId{0};
    };
    MirLoanId originLoan = 0;
    if (value.borrowOrigin == BorrowOriginKind::Argument &&
        value.borrowArgument < construct.operands.size()) {
      const MirOperand &argument =
          value.borrowArgument < sourceArguments.size()
              ? sourceArguments[value.borrowArgument]
              : construct.operands[value.borrowArgument];
      originLoan = operandLoan(argument);
    }
    // A generated move transfers an already checked value, not a new borrow
    // of the nominal reference formal used to select the special member.
    // Preserve the entire stored authority, including mixed access modes and
    // nested field paths, instead of degrading it to that formal's access.
    const bool generatedMove =
        !value.constructorTarget && !value.callPlan &&
        value.constructorKind == ConstructorKind::Move &&
        arguments.size() == 1 && construct.operands.size() == 1 &&
        construct.operands.front().kind == MirOperandKind::Value &&
        construct.operands.front().type == value.info.type;
    const MirLoan *inputLoan = output.findLoan(originLoan);
    const std::optional<MirLoan> transferredLoan =
        generatedMove && inputLoan != nullptr &&
                inputLoan->kind == MirLoanKind::Stored &&
                value.borrowOrigin == BorrowOriginKind::Argument &&
                value.borrowArgument == 0 && value.borrowArguments.empty() &&
                value.borrowFields.empty()
            ? std::optional<MirLoan>{*inputLoan}
            : std::nullopt;
    const MirPlaceId origin = borrowOriginPlace(value, originConstruct);
    construct.borrowOrigin = value.borrowOrigin;
    construct.borrowArgument = value.borrowArgument;
    construct.borrowAccess = value.borrowAccess;
    construct.borrowArguments = value.borrowArguments;
    construct.borrowFields = value.borrowFields;
    construct.dependentTextPublication = value.dependentTextPublication;
    if (value.borrowOrigin != BorrowOriginKind::None && origin != 0) {
      MirLoanId parent = 0;
      // A cleanup-bound adapter constructed through a checked readonly
      // reference keeps that exact parent alive until its own drop. This is
      // the semantic retained-loan contract, not permission to invent ancestry
      // from an overlapping source place (or from the constructor spelling).
      if (originLoan != 0 && originLoan <= output.loans.size() &&
          (output.loans[originLoan - 1].access == AccessMode::Mutable ||
           exactReadonlyCleanupParent(value, originLoan) ||
           (output.loans[originLoan - 1].kind == MirLoanKind::Stored &&
            output.loans[originLoan - 1].entry &&
            isLifecycleBoundBorrowCarrier(value.info.type)))) {
        parent = originLoan;
      }
      if (parent == 0 && isLifecycleBoundBorrowCarrier(value.info.type)) {
        const auto *dependency =
            output.findLoan(storedDependencyForPlace(origin));
        if (dependency != nullptr && dependency->entry &&
            dependency->kind == MirLoanKind::Stored)
          parent = dependency->id;
      }
      if (transferredLoan)
        parent = transferredLoan->parent;
      const MirLoanId loan = createLoan(
          MirLoanKind::Stored, origin,
          transferredLoan ? transferredLoan->access : value.borrowAccess,
          value.id, 0, 0, parent);
      if (parent != 0)
        output.loans[loan - 1].storedDependency =
            output.loans[parent - 1].storedDependency;
      std::vector<MirLoanDependency> additionalDependencies;
      const auto addDependency = [&](MirLoanDependency dependency) {
        if (dependency.source != 0 && dependency.source != origin &&
            std::find(additionalDependencies.begin(),
                      additionalDependencies.end(),
                      dependency) == additionalDependencies.end()) {
          additionalDependencies.push_back(std::move(dependency));
        }
      };
      if (originLoan != 0 && originLoan <= output.loans.size()) {
        for (const auto &dependency :
             output.loans[originLoan - 1].additionalDependencies) {
          addDependency(dependency);
        }
      }
      for (const std::size_t parameter : value.borrowArguments) {
        if (parameter >= originConstruct.operands.size()) {
          valid = false;
          continue;
        }
        HirValue dependency = value;
        dependency.borrowArgument = parameter;
        const MirLoanId inherited =
            operandLoan(originConstruct.operands[parameter]);
        const AccessMode access =
            parameter < value.parameterTypes.size() &&
                    value.parameterTypes[parameter].kind ==
                        SemanticType::Reference
                ? value.parameterTypes[parameter].referenceAccess
            : inherited != 0 && inherited <= output.loans.size()
                ? output.loans[inherited - 1].access
                : AccessMode::ReadOnly;
        addDependency({.source = borrowOriginPlace(dependency, originConstruct),
                       .access = access});
        if (inherited != 0 && inherited <= output.loans.size()) {
          for (const auto &dependency :
               output.loans[inherited - 1].additionalDependencies) {
            addDependency(dependency);
          }
        }
      }
      output.loans[loan - 1].additionalDependencies =
          std::move(additionalDependencies);
      if (!value.borrowFields.empty()) {
        output.loans[loan - 1].designatedFields =
            value.borrowFields.front().resultFields;
        output.loans[loan - 1].additionalDependencies.clear();
        for (std::size_t index = 1; index < value.borrowFields.size();
             ++index) {
          const auto &mapping = value.borrowFields[index];
          HirValue selected = value;
          selected.borrowOrigin = mapping.origin;
          selected.borrowArgument = mapping.parameter;
          selected.borrowFields = {mapping};
          const auto dependency = borrowOriginPlace(selected, originConstruct);
          if (dependency == 0)
            valid = false;
          output.loans[loan - 1].additionalDependencies.push_back(
              {.source = dependency,
               .designatedFields = mapping.resultFields,
               .access = mapping.access});
        }
      }
      // A carrier with one stored reference path names that field even when
      // semantics records no field map: the reference holds exactly the
      // selected referent. Naming it lets a later borrow through the field
      // resolve to that referent rather than to the carrier shell.
      if (value.borrowFields.empty() &&
          output.loans[loan - 1].kind == MirLoanKind::Stored &&
          output.loans[loan - 1].designatedFields.empty() &&
          output.loans[loan - 1].additionalDependencies.empty())
        if (const auto *source =
                output.findPlace(output.loans[loan - 1].source))
          if (const auto path = soleStoredReferencePath(value.info.type);
              path && path->second == source->type)
            output.loans[loan - 1].designatedFields = path->first;
      if (transferredLoan) {
        construct.borrowAccess = transferredLoan->access;
        output.loans[loan - 1].storedDependency =
            transferredLoan->storedDependency;
        output.loans[loan - 1].designatedFields =
            transferredLoan->designatedFields;
        output.loans[loan - 1].additionalDependencies =
            transferredLoan->additionalDependencies;
      }
      construct.loan = loan;
      valueLoans.insert_or_assign(value.id, loan);
      if (!scopes.empty()) {
        scopes.back().loans.push_back(loan);
      }
    }
    if (!value.callPlan) {
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        const SemanticType parameter =
            index < value.parameterTypes.size()
                ? value.parameterTypes[index]
                : (findValue(arguments[index]) == nullptr
                       ? SemanticType::Unknown
                       : findValue(arguments[index])->info.type);
        if (parameter.kind != SemanticType::Reference || generatedMove) {
          transferTemporaryOut(construct, arguments[index]);
        }
      }
    }
    // Mirror the ordinary-call result contract: construction publishes a
    // cleanup-owning value only on its successful control-flow edge.
    const MirDropObligationId resultDrop = dropObligationForValue(value.id);
    const bool successEdgeResult = resultDrop != 0 && routeFailureEdgesHere() &&
                                   !construct.definedFailure.empty() &&
                                   !construct.destination && !construct.loan &&
                                   !value.ownership;
    if (successEdgeResult) {
      construct.successResultDrop = resultDrop;
    } else {
      (void)initializeValueLifecycle(construct, value);
    }
    (void)appendInstruction(std::move(construct));
  }

  void routeDirectResultInitialization(const MirOperand &operand,
                                       MirPlaceId destination) {
    if (!routeFailureEdgesHere() || operand.kind != MirOperandKind::Value ||
        operand.value == 0 || destination == 0) {
      return;
    }
    const MirValue *result = output.findValue(operand.value);
    MirPlace *destinationPlace = destination > output.places.size()
                                     ? nullptr
                                     : &output.places[destination - 1];
    MirBlock *definitionBlock =
        result == nullptr || result->definitionBlock == 0 ||
                result->definitionBlock > output.blocks.size()
            ? nullptr
            : &output.blocks[result->definitionBlock - 1];
    if (definitionBlock == nullptr || destinationPlace == nullptr) {
      return;
    }
    const auto producerIt =
        std::find_if(definitionBlock->instructions.begin(),
                     definitionBlock->instructions.end(),
                     [&](const MirInstruction &instruction) {
                       return instruction.id == result->definition;
                     });
    if (producerIt == definitionBlock->instructions.end()) {
      return;
    }
    MirInstruction &producer = *producerIt;
    const bool directProducer =
        (producer.kind == MirInstructionKind::Call ||
         producer.kind == MirInstructionKind::Construct) &&
        producer.id == result->definition && producer.result == operand.value;
    if (!directProducer || producer.destination || producer.loan ||
        producer.successResultDrop || producer.successResultDestination ||
        producer.definedFailure.empty() ||
        producer.info.traits.drop != DropKind::Lexical ||
        destinationPlace->root != MirPlaceRootKind::Binding ||
        !destinationPlace->projections.empty() ||
        destinationPlace->type != producer.info.type) {
      return;
    }
    producer.successResultDestination = destination;
    const MirInstructionId producerId = producer.id;
    if (!requiresMirFailureControlFlow(producer)) {
      producer.successResultDestination.reset();
      return;
    }
    const bool existingInvoke =
        definitionBlock->terminator.kind == MirTerminatorKind::Invoke &&
        definitionBlock->terminator.invokeInstruction == producerId &&
        definitionBlock->terminator.target == current &&
        definitionBlock->instructions.back().id == producerId &&
        definitionBlock->terminator.successLifecycle.empty();
    if (existingInvoke) {
      return;
    }
    if (definitionBlock->id != current || terminated() ||
        definitionBlock->instructions.back().id != producerId) {
      producer.successResultDestination.reset();
      return;
    }
    appendFailureControlFlow(producerId, 0);
  }

  [[nodiscard]] MirOperand conditionOperand(HirValueId id) {
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      valid = false;
      return {};
    }
    if (value->info.type != SemanticType::Bool) {
      valid = false;
      return {};
    }
    return valueOperand(id);
  }

  [[nodiscard]] MirPlaceId logicalTemporary(const HirValue &value) {
    return appendPlace({.root = MirPlaceRootKind::Temporary,
                        .temporary = nextTemporary++,
                        .type = SemanticType::Bool,
                        .access = AccessMode::Mutable,
                        .traits = value.info.traits,
                        .sourceValue = value.id});
  }

  [[nodiscard]] MirPlaceId conditionalTemporary(const HirValue &value) {
    return appendPlace({.root = MirPlaceRootKind::Temporary,
                        .temporary = nextTemporary++,
                        .type = value.info.type,
                        .access = AccessMode::Mutable,
                        .traits = value.info.traits,
                        .sourceValue = value.id});
  }

  void endAddedLoans(const std::vector<Scope> &incoming) {
    if (incoming.size() != scopes.size()) {
      valid = false;
      return;
    }
    // A consuming call in this branch may already have retired an incoming
    // group; joinConsumedBranches retires it on the other branches.
    std::vector<Scope> baseline = incoming;
    for (Scope &scope : baseline)
      std::erase_if(scope.loans, [&](MirLoanId loan) {
        return !loanIsActive(scopes, loan) &&
               callTransferredLoanEndpoints.contains(loan);
      });
    const auto retained = temporaryCarrierLoans();
    for (std::size_t depth = scopes.size(); depth > 0; --depth) {
      const std::vector<MirLoanId> &before = baseline[depth - 1].loans;
      const std::vector<MirLoanId> &after = scopes[depth - 1].loans;
      if (after.size() < before.size() ||
          !std::equal(before.begin(), before.end(), after.begin())) {
        valid = false;
        continue;
      }
      for (std::size_t index = after.size(); index > before.size(); --index) {
        if (retained.contains(after[index - 1]))
          continue;
        (void)appendInstruction(
            {.kind = MirInstructionKind::EndBorrow, .loan = after[index - 1]});
      }
    }
  }

  // Only drop-bound dependencies survive a branch join. The existing
  // conditional value obligation provides their path-sensitive lifetime;
  // arbitrary branch-local reference loans keep their old strict endpoint.
  void retainBranchCarrierLoans(const std::vector<Scope> &branch) {
    if (branch.size() != scopes.size()) {
      valid = false;
      return;
    }
    const auto retained = temporaryCarrierLoans();
    for (std::size_t index = 0; index < scopes.size(); ++index) {
      for (const auto loan : branch[index].loans) {
        if (retained.contains(loan) && !containsLoan(scopes[index].loans, loan))
          scopes[index].loans.push_back(loan);
      }
    }
  }

  // A reborrow is only as good as the loan it hangs from, so a parent must
  // outlive its child on every edge — the MIR verifier rejects ending a
  // suspended parent. Releasing loans by expression extent alone does not see
  // that: a transient link is uncarried and unescaping, yet a binding may
  // retain a reborrow of it. Collect every ancestor of a loan that outlives
  // the current full expression so the release keeps them, and let the
  // owning scope end the whole chain child-first.
  [[nodiscard]] std::unordered_set<MirLoanId> retainedReborrowAncestors(
      const std::vector<MirDropObligationId> &retired = {}) const {
    std::unordered_set<MirLoanId> ancestors = temporaryCarrierLoans(retired);
    for (const Scope &scope : scopes) {
      for (const MirLoanId live : scope.loans) {
        const MirLoan *record = output.findLoan(live);
        if (record == nullptr ||
            (record->carriers.empty() && !record->escapes)) {
          continue;
        }
        std::vector<MirLoanId> pending{record->parent,
                                       record->storedDependency};
        while (!pending.empty()) {
          const MirLoanId parent = pending.back();
          pending.pop_back();
          if (parent == 0 || !ancestors.insert(parent).second)
            continue;
          const MirLoan *ancestor = output.findLoan(parent);
          if (ancestor != nullptr) {
            pending.push_back(ancestor->parent);
            pending.push_back(ancestor->storedDependency);
          }
        }
      }
    }
    return ancestors;
  }

  // `outliving` names loans whose endpoint the caller schedules after this
  // full expression, such as a switch subject's place loans.
  void endFullExpressionLoans(const std::vector<Scope> &baseline,
                              const std::vector<MirLoanId> &outliving = {}) {
    if (baseline.size() != scopes.size()) {
      valid = false;
      return;
    }
    std::unordered_set<MirLoanId> retained = retainedReborrowAncestors();
    retained.insert(outliving.begin(), outliving.end());
    for (std::size_t depth = scopes.size(); depth > 0; --depth) {
      const std::vector<MirLoanId> &before = baseline[depth - 1].loans;
      std::vector<MirLoanId> &after = scopes[depth - 1].loans;
      // Calls can consume a carrier which existed at expression entry.
      // Its explicit EndBorrow removes it from the active state, so the
      // baseline is no longer necessarily a prefix of the surviving loans.
      // Identity, not the old vector length, determines which loans are new.
      for (std::size_t index = after.size(); index > 0; --index) {
        const MirLoan &loan = output.loans[after[index - 1] - 1];
        const bool added =
            std::find(before.begin(), before.end(), loan.id) == before.end();
        if (added && !loan.lifecycleBound && loan.carriers.empty() &&
            !loan.escapes && !retained.contains(loan.id)) {
          (void)appendInstruction(
              {.kind = MirInstructionKind::EndBorrow, .loan = loan.id});
        }
      }
      after.erase(std::remove_if(
                      after.begin(), after.end(),
                      [&](MirLoanId loan) {
                        const MirLoan &candidate = output.loans[loan - 1];
                        return std::find(before.begin(), before.end(), loan) ==
                                   before.end() &&
                               !candidate.lifecycleBound &&
                               candidate.carriers.empty() &&
                               !candidate.escapes && !retained.contains(loan);
                      }),
                  after.end());
      after.erase(std::remove_if(after.begin(), after.end(),
                                 [&](MirLoanId loan) {
                                   return output.loans[loan - 1].escapes;
                                 }),
                  after.end());
    }
  }

  void endReturnExpressionLoans(const std::vector<Scope> &baseline) {
    if (baseline.size() != scopes.size()) {
      valid = false;
      return;
    }
    const std::unordered_set<MirLoanId> retained = retainedReborrowAncestors();
    for (std::size_t depth = scopes.size(); depth > 0; --depth) {
      const std::vector<MirLoanId> &before = baseline[depth - 1].loans;
      std::vector<MirLoanId> &after = scopes[depth - 1].loans;
      // As with ordinary full-expression cleanup, nested call-result loans
      // must end child-first. A projected return may have materialized its
      // final address already while both intermediate accessor loans remain.
      for (std::size_t index = after.size(); index > 0; --index) {
        const auto candidate =
            after.begin() + static_cast<std::ptrdiff_t>(index - 1);
        const bool added =
            std::find(before.begin(), before.end(), *candidate) == before.end();
        const MirLoan *loan = output.findLoan(*candidate);
        if (added && loan != nullptr && !loan->lifecycleBound &&
            loan->carriers.empty() && !loan->escapes &&
            !retained.contains(*candidate)) {
          (void)appendInstruction(
              {.kind = MirInstructionKind::EndBorrow, .loan = loan->id});
          after.erase(candidate);
        }
      }
    }
  }

  [[nodiscard]] MirLoanId mirLoanForSemanticLoan(SemanticLoanId semanticLoan) {
    const auto found = semanticLoans.find(semanticLoan);
    if (found == semanticLoans.end() || found->second == 0 ||
        found->second > output.loans.size() ||
        output.loans[found->second - 1].escapes) {
      valid = false;
      return 0;
    }
    return found->second;
  }

  [[nodiscard]] static bool loanIsActive(const std::vector<Scope> &scopeState,
                                         MirLoanId loan) {
    return std::any_of(
        scopeState.begin(), scopeState.end(), [&](const Scope &scope) {
          return std::find(scope.loans.begin(), scope.loans.end(), loan) !=
                 scope.loans.end();
        });
  }

  // A retained binding may name a call result the expression reached through
  // more than one link: in `carrier.right.reach().write()` the receiver of
  // `write()` is itself a borrowed call result, so lowering builds a chain of
  // call-result reborrows before the binding retains the outermost one.
  //
  // Report whether every link from `outermost` up to — but excluding —
  // `ancestor` is such a reborrow, built by this expression for `valueId`
  // alone: a call result that carries nothing, does not escape, is still
  // active, and is at least as permissive as `access`. When it is, the
  // outermost link designates exactly what the retained binding names, and
  // the chain already bottoms out at `ancestor`, so retaining it preserves
  // that loan's suspension and reactivation.
  [[nodiscard]] bool retainableCallResultChain(MirLoanId outermost,
                                               MirLoanId ancestor,
                                               HirValueId valueId,
                                               AccessMode access) const {
    if (outermost == 0 || outermost == ancestor ||
        (ancestor != 0 && output.findLoan(ancestor) == nullptr)) {
      return false;
    }
    // Control-flow expressions may transparently forward one selected
    // checked call result. Use only aliases recorded after type/access/loan
    // verification by forwardBorrowResult, not an arbitrary equal source ID.
    std::unordered_set<HirValueId> aliases;
    for (;;) {
      const auto forwarded = forwardedBorrowValues.find(valueId);
      if (forwarded == forwardedBorrowValues.end())
        break;
      if (!aliases.insert(valueId).second)
        return false;
      valueId = forwarded->second;
    }
    std::unordered_set<MirLoanId> visited;
    MirLoanId link = outermost;
    while (link != ancestor) {
      const MirLoan *candidate = output.findLoan(link);
      if (candidate == nullptr || !visited.insert(link).second ||
          candidate->kind != MirLoanKind::CallResult ||
          !candidate->carriers.empty() || candidate->escapes ||
          !loanIsActive(scopes, link) ||
          (access != AccessMode::ReadOnly &&
           candidate->access != AccessMode::Mutable) ||
          (link == outermost ? candidate->producedBy != valueId
                             : candidate->producedBy == 0)) {
        return false;
      }
      link = candidate->parent;
    }
    return true;
  }

  static void removeLoanFromState(
      MirLoanId loan, std::vector<Scope> &scopeState,
      std::unordered_map<HirBindingId, MirLoanId> &bindingLoanState) {
    for (Scope &scope : scopeState) {
      std::erase(scope.loans, loan);
    }
    std::erase_if(bindingLoanState,
                  [&](const auto &entry) { return entry.second == loan; });
  }

  void endConsumedCallResultLoans(const std::vector<HirValueId> &operands,
                                  const HirValue &consumer) {
    if (consumer.info.type.kind == SemanticType::Reference ||
        consumer.info.traits.containsBorrowedState) {
      return;
    }
    std::unordered_set<MirLoanId> visited;
    for (const HirValueId operand : operands) {
      MirLoanId loan = loanForValue(operand);
      if (loan == 0) {
        const auto materialized = valuePlaces.find(operand);
        const auto *place = materialized == valuePlaces.end()
                                ? nullptr
                                : output.findPlace(materialized->second);
        // An owning member projection does not itself carry borrowed state,
        // but its address can still be rooted in a transient accessor result.
        // Once this non-borrowing consumer completes, retire that exact
        // address-producing loan just as for an unprojected receiver.
        if (place != nullptr && place->root == MirPlaceRootKind::Loan)
          loan = place->loan;
      }
      while (loan != 0 && loan <= output.loans.size() &&
             visited.insert(loan).second && loanIsActive(scopes, loan)) {
        const MirLoan &candidate = output.loans[loan - 1];
        if (candidate.kind != MirLoanKind::CallResult ||
            !candidate.carriers.empty() || candidate.escapes) {
          break;
        }
        const bool hasActiveChild = std::any_of(
            output.loans.begin(), output.loans.end(),
            [&](const MirLoan &other) {
              return other.parent == loan && loanIsActive(scopes, other.id);
            });
        if (hasActiveChild) {
          // `loanForValue` can name an inner expression's origin while a
          // later call-result reborrow is the receiver actually consumed by
          // this call. Leave that parent to the selected full-expression
          // endpoint; ending it here would violate child-before-parent order.
          break;
        }
        const MirLoanId parent = candidate.parent;
        (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                 .hirValue = consumer.id,
                                 .loan = loan});
        removeLoanFromState(loan, scopes, bindingLoans);
        loan = parent;
      }
    }
  }

  [[nodiscard]] std::vector<MirLoanId>
  childFirstLoans(const std::vector<SemanticLoanId> &loans,
                  const std::vector<Scope> *scopeState = nullptr) {
    std::vector<MirLoanId> result;
    result.reserve(loans.size());
    for (const SemanticLoanId semanticLoan : loans) {
      const MirLoanId loan = mirLoanForSemanticLoan(semanticLoan);
      if (loan != 0 &&
          std::find(result.begin(), result.end(), loan) == result.end()) {
        result.push_back(loan);
      }
    }
    const std::vector<Scope> &activeScopes =
        scopeState == nullptr ? scopes : *scopeState;
    const auto descendsFromSelected = [&](MirLoanId candidate) {
      std::unordered_set<MirLoanId> visited;
      std::vector<MirLoanId> pending{candidate};
      while (!pending.empty()) {
        const MirLoanId current = pending.back();
        pending.pop_back();
        const MirLoan *record = output.findLoan(current);
        if (record == nullptr || !visited.insert(current).second)
          continue;
        if ((record->parent != 0 &&
             std::find(result.begin(), result.end(), record->parent) !=
                 result.end()) ||
            (record->storedDependency != 0 &&
             std::find(result.begin(), result.end(),
                       record->storedDependency) != result.end())) {
          return true;
        }
        pending.push_back(record->parent);
        pending.push_back(record->storedDependency);
      }
      return false;
    };
    for (const MirLoan &loan : output.loans) {
      if (loanIsActive(activeScopes, loan.id) &&
          descendsFromSelected(loan.id) &&
          std::find(result.begin(), result.end(), loan.id) == result.end()) {
        result.push_back(loan.id);
      }
    }
    // A retained call-result reborrow outlives the transient links it was
    // built from — the outer call of `f(receiver.borrow())` designates what
    // the inner call returned — so those links stay live past their full
    // expression to keep every parent outliving its child. They carry
    // nothing themselves, so they end exactly when the last loan that
    // reborrows them does. Fold them into the same release; the child-first
    // order below still ends each parent after its children.
    for (std::size_t index = 0; index < result.size(); ++index) {
      MirLoanId ancestor = output.loans[result[index] - 1].parent;
      while (ancestor != 0 && ancestor <= output.loans.size() &&
             std::find(result.begin(), result.end(), ancestor) ==
                 result.end()) {
        const MirLoan &candidate = output.loans[ancestor - 1];
        // A by-reference capture's entry loan carries no binding because the
        // closure environment holds it for the whole call; it is not a
        // transient link.
        if (!candidate.carriers.empty() || candidate.escapes ||
            capturedReferenceEntry(candidate) ||
            !loanIsActive(activeScopes, ancestor)) {
          break;
        }
        // The external owner can also be retained by an independently live
        // carrier-shell alias. Finishing one child must not retire that
        // owner's entry loan while the alias remains usable.
        const bool reborrowedElsewhere =
            std::any_of(output.loans.begin(), output.loans.end(),
                        [&](const MirLoan &other) {
                          return (other.parent == ancestor ||
                                  other.storedDependency == ancestor) &&
                                 loanIsActive(activeScopes, other.id) &&
                                 std::find(result.begin(), result.end(),
                                           other.id) == result.end();
                        });
        if (reborrowedElsewhere) {
          break;
        }
        result.push_back(ancestor);
        ancestor = candidate.parent;
      }
    }
    std::stable_sort(result.begin(), result.end(),
                     [&](MirLoanId left, MirLoanId right) {
                       return loanDepth(left) > loanDepth(right);
                     });
    return result;
  }

  [[nodiscard]] std::size_t loanDepth(MirLoanId loan) const {
    std::unordered_set<MirLoanId> visiting;
    const auto visit = [&](const auto &self, MirLoanId current) -> std::size_t {
      const MirLoan *record = output.findLoan(current);
      if (record == nullptr || !visiting.insert(current).second) {
        return 0;
      }
      auto depth = std::max(self(self, record->parent),
                            self(self, record->storedDependency));
      for (const auto &dependency : record->additionalDependencies)
        depth = std::max(depth, self(self, dependency.parent));
      const auto result = 1 + depth;
      visiting.erase(current);
      return result;
    };
    return visit(visit, loan);
  }

  [[nodiscard]] std::vector<MirLoanId>
  childFirstMirLoans(const std::vector<MirLoanId> &loans) const {
    std::vector<MirLoanId> result;
    result.reserve(loans.size());
    for (const MirLoanId loan : loans) {
      if (loan != 0 && loan <= output.loans.size() &&
          std::find(result.begin(), result.end(), loan) == result.end()) {
        result.push_back(loan);
      }
    }
    std::stable_sort(result.begin(), result.end(),
                     [&](MirLoanId left, MirLoanId right) {
                       return loanDepth(left) > loanDepth(right);
                     });
    return result;
  }

  // The parameter an argument-origin reference result borrows from, for a
  // function or a lambda body; a lambda's contract lives on its instance.
  [[nodiscard]] std::optional<mir_detail::MirReturnOriginFormal>
  returnBorrowOriginFormal() const {
    if (function != nullptr) {
      // A text field result borrows nothing of its formal in this body.
      if (mir_detail::textFieldResult(
              function->returnType, function->returnBorrowOrigin,
              function->returnBorrowParameter, function->returnBorrowFields))
        return std::nullopt;
      return function->returnBorrowOrigin == BorrowOriginKind::Argument
                 ? mir_detail::returnOriginFormal(
                       function->parameterTypes, function->parameterBindings,
                       function->returnBorrowParameter)
                 : std::nullopt;
    }
    if (lambda != nullptr) {
      return lambda->returnBorrowOrigin == BorrowOriginKind::Argument
                 ? mir_detail::returnOriginFormal(lambda->parameterTypes,
                                                  lambda->parameterBindings,
                                                  lambda->returnBorrowParameter)
                 : std::nullopt;
    }
    return std::nullopt;
  }
  [[nodiscard]] std::optional<HirBindingId>
  returnBorrowParameterBinding() const {
    const auto formal = returnBorrowOriginFormal();
    return formal ? std::optional<HirBindingId>{formal->binding} : std::nullopt;
  }
  // Whether a loan's source place is exactly the summarized formal: the
  // whole binding, or the one pack element the contract names.
  [[nodiscard]] bool sourceIsReturnOriginFormal(const MirPlace &source) const {
    const auto formal = returnBorrowOriginFormal();
    if (!formal || source.root != MirPlaceRootKind::Binding ||
        source.binding != formal->binding)
      return false;
    if (source.projections.empty())
      return source.type.kind != SemanticType::TypePack;
    return source.projections.size() == 1 &&
           source.projections.front().kind == MirProjectionKind::PackElement &&
           source.projections.front().constantIndex &&
           static_cast<std::size_t>(
               *source.projections.front().constantIndex) ==
               formal->elementWithinBinding;
  }
  [[nodiscard]] BorrowOriginKind bodyReturnBorrowOrigin() const {
    return function != nullptr ? function->returnBorrowOrigin
           : lambda != nullptr ? lambda->returnBorrowOrigin
                               : BorrowOriginKind::None;
  }
  [[nodiscard]] AccessMode bodyReturnBorrowAccess() const {
    return function != nullptr ? function->returnBorrowAccess
           : lambda != nullptr ? lambda->returnBorrowAccess
                               : AccessMode::ReadOnly;
  }

  [[nodiscard]] bool mustReachBorrowedReturn(MirLoanId loan) const {
    const std::optional<HirBindingId> summarized =
        returnBorrowParameterBinding();
    if (loan == 0 || loan > output.loans.size() || !summarized) {
      return false;
    }
    const MirLoan &candidate = output.loans[loan - 1];
    const HirBindingId parameter = *summarized;
    return candidate.entry &&
           std::find(candidate.carriers.begin(), candidate.carriers.end(),
                     parameter) != candidate.carriers.end();
  }

  void endSemanticLoans(const std::vector<SemanticLoanId> &loans,
                        HirStatementId statement) {
    const auto retained = lifecycleRetainedLoans({});
    for (const MirLoanId loan : childFirstLoans(loans)) {
      if (mustReachBorrowedReturn(loan) || retained.contains(loan) ||
          containsLoan(constructorPublishedLoans, loan)) {
        continue;
      }
      if (!loanIsActive(scopes, loan)) {
        const auto transferred = callTransferredLoanEndpoints.find(loan);
        if (transferred != callTransferredLoanEndpoints.end() &&
            transferred->second == statement) {
          callTransferredLoanEndpoints.erase(transferred);
          continue;
        }
        valid = false;
        continue;
      }
      (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                               .hirStatement = statement,
                               .loan = loan});
      removeLoanFromState(loan, scopes, bindingLoans);
    }
  }

  void endSemanticLoansIfActive(const std::vector<SemanticLoanId> &loans,
                                HirStatementId statement) {
    const auto retained = lifecycleRetainedLoans({});
    for (const MirLoanId loan : childFirstLoans(loans)) {
      if (mustReachBorrowedReturn(loan) || retained.contains(loan) ||
          containsLoan(constructorPublishedLoans, loan)) {
        continue;
      }
      if (!loanIsActive(scopes, loan)) {
        continue;
      }
      (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                               .hirStatement = statement,
                               .loan = loan});
      removeLoanFromState(loan, scopes, bindingLoans);
    }
  }

  void normalizeSemanticLoanState(
      const std::vector<SemanticLoanId> &loans, std::vector<Scope> &scopeState,
      std::unordered_map<HirBindingId, MirLoanId> &bindingLoanState) {
    const auto retained = lifecycleRetainedLoans({}, &scopeState);
    for (const MirLoanId loan : childFirstLoans(loans, &scopeState)) {
      if (mustReachBorrowedReturn(loan) || retained.contains(loan) ||
          containsLoan(constructorPublishedLoans, loan)) {
        continue;
      }
      if (!loanIsActive(scopeState, loan)) {
        valid = false;
        continue;
      }
      removeLoanFromState(loan, scopeState, bindingLoanState);
    }
  }

  void endSemanticLoans(const HirStatement &statement) {
    endSemanticLoans(statement.endedLoans, statement.id);
  }

  [[nodiscard]] static std::unordered_map<MirLoanId, HirStatementId>
  intersectTransferredLoanEndpoints(
      std::unordered_map<MirLoanId, HirStatementId> left,
      const std::unordered_map<MirLoanId, HirStatementId> &right) {
    // An executable endpoint is acknowledged at a join only when every
    // normally reaching predecessor carries the same exact HIR endpoint.
    std::erase_if(left, [&](const auto &entry) {
      const auto found = right.find(entry.first);
      return found == right.end() || found->second != entry.second;
    });
    return left;
  }

  // A consuming call in one branch retires its carrier's stored group there,
  // and the carrier is then moved on some path, so nothing after the join can
  // use it. Branches that still hold the group retire it before the join so
  // every edge agrees. Only a cleanup-free group qualifies; a lifecycle-bound
  // group is still needed by its carrier's drop.
  struct OpenBranch {
    MirBlockId end = 0;
    std::vector<Scope> scopes;
    std::unordered_map<MirLoanId, HirStatementId> transfers;
  };

  struct ResultRegion {
    const HirValue *value = nullptr;
    MirPlaceId destination = 0;
    MirDropObligationId obligation = 0;
    std::vector<Scope> incomingScopes;
    std::vector<TemporaryDrop> incomingTemporaries;
    std::unordered_map<HirBindingId, MirLoanId> incomingBindings;
    std::unordered_map<MirLoanId, HirStatementId> incomingTransfers;
    std::vector<OpenBranch> exits;
    std::vector<std::vector<TemporaryDrop>> exitTemporaries;
    std::vector<std::unordered_map<HirBindingId, MirLoanId>> exitBindings;
  };

  void emitRegionReturn(const HirStatement &statement) {
    if (resultRegions.empty() ||
        statement.resultRegion != resultRegions.back().value->id) {
      valid = false;
      return;
    }
    // Evaluating a return expression may itself enter another result region,
    // so retain indices/identities, not references into the region stack.
    const std::size_t index = resultRegions.size() - 1;
    const auto *regionValue = resultRegions[index].value;
    const auto incomingScopes = scopes;
    if (regionValue->info.type != SemanticType::Void) {
      if (!statement.value) {
        valid = false;
        return;
      }
      const MirOperand operand = valueOperand(*statement.value);
      if (terminated())
        return;
      MirInstruction initialize{.kind = MirInstructionKind::Initialize,
                                .hirValue = regionValue->id,
                                .hirStatement = statement.id,
                                .destination = resultRegions[index].destination,
                                .operands = {operand},
                                .info = {.type = regionValue->info.type,
                                         .category = ValueCategory::Place,
                                         .access = AccessMode::Mutable,
                                         .traits = regionValue->info.traits}};
      const auto sourceDrop = dropObligationForValue(*statement.value);
      const auto resultDrop = resultRegions[index].obligation;
      if (resultDrop != 0) {
        if (temporaryIsActive(sourceDrop)) {
          appendReparentOrTypedTransfer(initialize, sourceDrop, resultDrop);
          (void)removeTemporary(sourceDrop);
        } else {
          appendLifecycle(initialize,
                          {.kind = MirLifecycleEventKind::Initialize,
                           .target = resultDrop});
        }
        registerTemporary(resultDrop);
      }
      (void)appendInstruction(std::move(initialize));
    } else if (statement.value) {
      emitValue(*statement.value);
      if (terminated())
        return;
    }
    endReturnExpressionLoans(incomingScopes);
    auto retained = resultRegions[index].incomingTemporaries;
    const auto resultDrop = resultRegions[index].obligation;
    for (const auto &temporary : temporaryDrops)
      if (temporary.obligation == resultDrop)
        retained.push_back(temporary);
    // The published result already has its outer full-expression lifetime.
    // Keep it in failure cleanup while destroying macro-local temporaries
    // and locals. A cleanup panic must still destroy it exactly once.
    emitTemporaryDrops(retained, statement.value.value_or(0), statement.id);
    emitScopeExit(resultRegions[index].incomingScopes.size());
    const MirBlockId bridge = appendBlock();
    terminate({.kind = MirTerminatorKind::Goto,
               .hirStatement = statement.id,
               .target = bridge});
    auto exitedScopes = scopes;
    exitedScopes.resize(resultRegions[index].incomingScopes.size());
    resultRegions[index].exits.push_back(
        {.end = bridge,
         .scopes = std::move(exitedScopes),
         .transfers = callTransferredLoanEndpoints});
    resultRegions[index].exitTemporaries.push_back(temporaryDrops);
    resultRegions[index].exitBindings.push_back(projectOuterBindingLoans(
        resultRegions[index].incomingBindings, bindingLoans));
  }

  void emitScopedResult(const HirValue &value) {
    if (!value.regionBody || value.info.category != ValueCategory::Value ||
        std::any_of(
            resultRegions.begin(), resultRegions.end(),
            [&](const auto &region) { return region.value == &value; }) ||
        value.info.traits.containsBorrowedState) {
      valid = false;
      return;
    }
    if ((value.info.type.kind == SemanticType::Class ||
         value.info.type.kind == SemanticType::Lambda) &&
        !value.dropObligation) {
      // Result-slot initialization is a language lifetime fact, including
      // passive objects, not a pattern for the backend to reconstruct.
      valid = false;
      return;
    }
    const MirBlockId merge = appendBlock();
    const bool hasResult = value.info.type != SemanticType::Void;
    const MirPlaceId destination = hasResult ? conditionalTemporary(value) : 0;
    resultRegions.push_back(
        {.value = &value,
         .destination = destination,
         .obligation =
             hasResult ? dropObligationForValue(value.id, destination) : 0,
         .incomingScopes = scopes,
         .incomingTemporaries = temporaryDrops,
         .incomingBindings = bindingLoans,
         .incomingTransfers = callTransferredLoanEndpoints});
    lowerStatement(*value.regionBody);
    if (!terminated()) {
      if (hasResult) {
        valid = false;
      } else {
        const MirBlockId bridge = appendBlock();
        terminate({.kind = MirTerminatorKind::Goto, .target = bridge});
        resultRegions.back().exits.push_back(
            {.end = bridge,
             .scopes = scopes,
             .transfers = callTransferredLoanEndpoints});
        resultRegions.back().exitTemporaries.push_back(temporaryDrops);
        resultRegions.back().exitBindings.push_back(projectOuterBindingLoans(
            resultRegions.back().incomingBindings, bindingLoans));
      }
    }
    auto region = std::move(resultRegions.back());
    resultRegions.pop_back();
    std::vector<OpenBranch *> branches;
    for (auto &exit : region.exits)
      branches.push_back(&exit);
    const auto consumed = joinConsumedBranches(branches, region.incomingScopes,
                                               region.incomingTransfers, merge);
    scopes = region.incomingScopes;
    for (const auto &[loan, endpoint] : consumed)
      removeLoanFromState(loan, scopes, bindingLoans);
    current = merge;
    callTransferredLoanEndpoints = region.incomingTransfers;
    temporaryDrops = region.incomingTemporaries;
    bindingLoans = region.incomingBindings;
    for (auto &bindings : region.exitBindings)
      for (const auto &[loan, endpoint] : consumed)
        std::erase_if(bindings,
                      [&](const auto &entry) { return entry.second == loan; });
    if (region.exits.empty()) {
      terminate({.kind = MirTerminatorKind::Unreachable});
      emittedValues.insert(value.id);
      return;
    }
    callTransferredLoanEndpoints = region.exits.front().transfers;
    temporaryDrops = region.exitTemporaries.front();
    bindingLoans = region.exitBindings.front();
    scopes = region.exits.front().scopes;
    for (std::size_t index = 1; index < region.exits.size(); ++index) {
      if (!sameScopeState(scopes, region.exits[index].scopes) ||
          bindingLoans != region.exitBindings[index])
        valid = false;
      callTransferredLoanEndpoints = intersectTransferredLoanEndpoints(
          callTransferredLoanEndpoints, region.exits[index].transfers);
      temporaryDrops =
          mergeTemporaryDrops(temporaryDrops, region.exitTemporaries[index]);
    }
    if (hasResult) {
      // A fresh object result uses an owned transport even when its generated
      // move is trivial. A Load denotes a copied place observation, which is
      // not the producer contract selected by a later by-value call stage.
      const bool move = value.info.type.kind == SemanticType::Class ||
                        value.info.type.kind == SemanticType::Lambda ||
                        value.info.traits.drop != DropKind::Trivial ||
                        !value.info.traits.copyable;
      (void)appendInstruction(
          {.kind = move ? MirInstructionKind::Move : MirInstructionKind::Load,
           .hirValue = value.id,
           .result = resultFor(value),
           .operands = {{.kind =
                             move ? MirOperandKind::Move : MirOperandKind::Copy,
                         .place = destination,
                         .type = value.info.type}},
           .info = value.info});
    }
    emittedValues.insert(value.id);
  }

  [[nodiscard]] std::unordered_map<MirLoanId, HirStatementId>
  joinConsumedBranches(
      std::vector<OpenBranch *> branches, const std::vector<Scope> &incoming,
      const std::unordered_map<MirLoanId, HirStatementId> &incomingTransfers,
      MirBlockId merge) {
    std::unordered_map<MirLoanId, HirStatementId> consumed;
    for (const OpenBranch *branch : branches)
      for (const Scope &scope : incoming)
        for (const MirLoanId loan : scope.loans) {
          const auto transfer = branch->transfers.find(loan);
          if (!loanIsActive(branch->scopes, loan) &&
              transfer != branch->transfers.end() &&
              !incomingTransfers.contains(loan))
            consumed.emplace(loan, transfer->second);
        }
    for (OpenBranch *branch : branches) {
      current = branch->end;
      scopes = branch->scopes;
      for (const auto &[loan, endpoint] : consumed) {
        if (!loanIsActive(scopes, loan))
          continue;
        if (output.loans[loan - 1].lifecycleBound) {
          valid = false;
          continue;
        }
        (void)appendInstruction(
            {.kind = MirInstructionKind::EndBorrow, .loan = loan});
        removeLoanFromState(loan, scopes, bindingLoans);
        branch->transfers.insert_or_assign(loan, endpoint);
      }
      branch->scopes = scopes;
      terminate({.kind = MirTerminatorKind::Goto, .target = merge});
    }
    return consumed;
  }

  void emitLogical(const HirValue &value) {
    if (value.operands.size() != 2 || !value.operation ||
        (*value.operation != TokenKind::AND &&
         *value.operation != TokenKind::OR)) {
      valid = false;
      return;
    }

    const MirOperand left = conditionOperand(value.operands[0]);
    const std::vector<TemporaryDrop> leftTemporaryDrops = temporaryDrops;
    const MirPlaceId temporary = logicalTemporary(value);
    const MirBlockId rightBlock = appendBlock();
    const MirBlockId shortCircuitBlock = appendBlock();
    const MirBlockId mergeBlock = appendBlock();
    const bool conjunction = *value.operation == TokenKind::AND;
    terminate({.kind = MirTerminatorKind::Branch,
               .hirValue = value.id,
               .value = left,
               .target = conjunction ? rightBlock : shortCircuitBlock,
               .elseTarget = conjunction ? shortCircuitBlock : rightBlock});

    const std::vector<Scope> incomingScopes = scopes;
    const auto incomingTransferEndpoints = callTransferredLoanEndpoints;
    current = shortCircuitBlock;
    scopes = incomingScopes;
    temporaryDrops = leftTemporaryDrops;
    (void)appendInstruction(
        {.kind = MirInstructionKind::Initialize,
         .hirValue = value.id,
         .destination = temporary,
         .operands = {{.kind = MirOperandKind::Constant,
                       .literal = Literal{!conjunction},
                       .type = SemanticType::Bool}},
         .info = ExpressionInfo{.type = SemanticType::Bool,
                                .category = ValueCategory::Place,
                                .access = AccessMode::Mutable,
                                .traits = value.info.traits}});
    OpenBranch shortCircuit{.end = current,
                            .scopes = scopes,
                            .transfers = incomingTransferEndpoints};
    const std::vector<TemporaryDrop> shortCircuitTemporaryDrops =
        temporaryDrops;

    current = rightBlock;
    scopes = incomingScopes;
    temporaryDrops = leftTemporaryDrops;
    callTransferredLoanEndpoints = incomingTransferEndpoints;
    const MirOperand right = conditionOperand(value.operands[1]);
    (void)appendInstruction(
        {.kind = MirInstructionKind::Initialize,
         .hirValue = value.id,
         .destination = temporary,
         .operands = {right},
         .info = ExpressionInfo{.type = SemanticType::Bool,
                                .category = ValueCategory::Place,
                                .access = AccessMode::Mutable,
                                .traits = value.info.traits}});
    endAddedLoans(incomingScopes);
    OpenBranch rightBranch{.end = current,
                           .scopes = scopes,
                           .transfers = callTransferredLoanEndpoints};
    const std::vector<TemporaryDrop> rightTemporaryDrops = temporaryDrops;
    const auto consumed =
        joinConsumedBranches({&shortCircuit, &rightBranch}, incomingScopes,
                             incomingTransferEndpoints, mergeBlock);
    const auto rightScopes = rightBranch.scopes;

    current = mergeBlock;
    callTransferredLoanEndpoints = intersectTransferredLoanEndpoints(
        shortCircuit.transfers, rightBranch.transfers);
    scopes = incomingScopes;
    for (const auto &[loan, endpoint] : consumed)
      removeLoanFromState(loan, scopes, bindingLoans);
    temporaryDrops =
        mergeTemporaryDrops(shortCircuitTemporaryDrops, rightTemporaryDrops);
    retainBranchCarrierLoans(rightScopes);
    (void)appendInstruction({.kind = MirInstructionKind::Load,
                             .hirValue = value.id,
                             .result = resultFor(value),
                             .operands = {{.kind = MirOperandKind::Copy,
                                           .place = temporary,
                                           .type = SemanticType::Bool}},
                             .info = value.info});
    emittedValues.insert(value.id);
  }

  void forwardBorrowResult(const HirValue &value, HirValueId source) {
    emitValue(source);
    const MirLoanId loan = loanForValue(source);
    const MirLoan *authority = output.findLoan(loan);
    const HirValue *producer = findValue(source);
    if (authority == nullptr || producer == nullptr ||
        value.info.category != ValueCategory::Place ||
        producer->info.category != ValueCategory::Place ||
        producer->info.type != value.info.type ||
        producer->info.access != value.info.access ||
        authority->access != value.info.access) {
      valid = false;
      return;
    }
    valueLoans.insert_or_assign(value.id, loan);
    forwardedBorrowValues.insert_or_assign(value.id, source);
    loweredValues.insert_or_assign(value.id, mirValueFor(source));
    valuePlaces.insert_or_assign(value.id,
                                 clonePlace(placeForValue(source), value));
  }

  void emitConditional(const HirValue &value) {
    if (value.operands.size() != 3) {
      valid = false;
      return;
    }

    const MirOperand condition = conditionOperand(value.operands[0]);
    const MirBlockId thenBlock = appendBlock();
    const MirBlockId elseBlock = appendBlock();
    const MirBlockId mergeBlock = appendBlock();
    terminate({.kind = MirTerminatorKind::Branch,
               .hirValue = value.id,
               .value = condition,
               .target = thenBlock,
               .elseTarget = elseBlock});

    const std::vector<Scope> incomingScopes = scopes;
    const std::vector<TemporaryDrop> incomingTemporaryDrops = temporaryDrops;
    const auto incomingTransferEndpoints = callTransferredLoanEndpoints;
    const bool hasResult = value.info.type.kind != SemanticType::Void;
    const bool borrowedResult =
        hasResult && value.info.category == ValueCategory::Place;
    // A reference join currently has exactly one normally completing arm.
    // The other is terminal, so no pointer phi or lifetime union is needed.
    if (borrowedResult &&
        (!findValue(value.operands[1]) || !findValue(value.operands[2]) ||
         findValue(value.operands[1])->neverReturns ==
             findValue(value.operands[2])->neverReturns)) {
      valid = false;
      return;
    }
    const MirPlaceId temporary = hasResult && !borrowedResult
                                     ? conditionalTemporary(value)
                                     : MirPlaceId{0};
    const MirDropObligationId resultObligation =
        hasResult && !borrowedResult
            ? dropObligationForValue(value.id, temporary)
            : 0;
    std::vector<TemporaryDrop> thenTemporaryDrops;
    std::vector<TemporaryDrop> elseTemporaryDrops;
    OpenBranch thenOpen;
    OpenBranch elseOpen;
    const auto emitArm = [&](MirBlockId block, HirValueId arm,
                             bool &completesNormally, OpenBranch &open) {
      current = block;
      scopes = incomingScopes;
      temporaryDrops = incomingTemporaryDrops;
      callTransferredLoanEndpoints = incomingTransferEndpoints;
      const HirValue *armValue = findValue(arm);
      if (armValue == nullptr) {
        valid = false;
        completesNormally = false;
        return temporaryDrops;
      }
      if (armValue->neverReturns) {
        emitValue(arm);
        completesNormally = false;
        if (!terminated()) {
          valid = false;
        }
        return temporaryDrops;
      }
      if (borrowedResult) {
        forwardBorrowResult(value, arm);
      } else if (hasResult) {
        const MirOperand operand = valueOperand(arm);
        if (terminated()) {
          completesNormally = false;
          return temporaryDrops;
        }
        MirInstruction initialize{
            .kind = MirInstructionKind::Initialize,
            .hirValue = value.id,
            .destination = temporary,
            .operands = {operand},
            .info = ExpressionInfo{.type = value.info.type,
                                   .category = ValueCategory::Place,
                                   .access = AccessMode::Mutable,
                                   .traits = value.info.traits}};
        const MirDropObligationId sourceObligation =
            dropObligationForValue(arm);
        if (resultObligation != 0) {
          if (temporaryIsActive(sourceObligation)) {
            appendReparentOrTypedTransfer(initialize, sourceObligation,
                                          resultObligation);
            (void)removeTemporary(sourceObligation);
          } else {
            appendLifecycle(initialize,
                            {.kind = MirLifecycleEventKind::Initialize,
                             .target = resultObligation});
          }
          registerTemporary(resultObligation);
        }
        (void)appendInstruction(std::move(initialize));
      } else {
        emitValue(arm);
      }
      completesNormally = !terminated();
      if (!completesNormally) {
        return temporaryDrops;
      }
      if (!borrowedResult)
        endAddedLoans(incomingScopes);
      open = {.end = current,
              .scopes = scopes,
              .transfers = callTransferredLoanEndpoints};
      return temporaryDrops;
    };

    bool thenCompletes = false;
    bool elseCompletes = false;
    thenTemporaryDrops =
        emitArm(thenBlock, value.operands[1], thenCompletes, thenOpen);
    elseTemporaryDrops =
        emitArm(elseBlock, value.operands[2], elseCompletes, elseOpen);
    std::vector<OpenBranch *> completing;
    if (thenCompletes)
      completing.push_back(&thenOpen);
    if (elseCompletes)
      completing.push_back(&elseOpen);
    const auto consumed = joinConsumedBranches(
        completing, incomingScopes, incomingTransferEndpoints, mergeBlock);
    const auto thenScopes = thenOpen.scopes;
    const auto thenTransferEndpoints = thenOpen.transfers;
    const auto elseScopes = elseOpen.scopes;
    const auto elseTransferEndpoints = elseOpen.transfers;

    current = mergeBlock;
    scopes = incomingScopes;
    for (const auto &[loan, endpoint] : consumed)
      removeLoanFromState(loan, scopes, bindingLoans);
    if (!thenCompletes && !elseCompletes) {
      callTransferredLoanEndpoints = incomingTransferEndpoints;
      temporaryDrops = incomingTemporaryDrops;
      terminate({.kind = MirTerminatorKind::Unreachable});
      emittedValues.insert(value.id);
      return;
    }
    callTransferredLoanEndpoints =
        thenCompletes && elseCompletes
            ? intersectTransferredLoanEndpoints(thenTransferEndpoints,
                                                elseTransferEndpoints)
        : thenCompletes ? thenTransferEndpoints
                        : elseTransferEndpoints;
    temporaryDrops =
        thenCompletes && elseCompletes
            ? mergeTemporaryDrops(thenTemporaryDrops, elseTemporaryDrops)
        : thenCompletes ? thenTemporaryDrops
                        : elseTemporaryDrops;
    if (thenCompletes) {
      retainBranchCarrierLoans(thenScopes);
    }
    if (elseCompletes) {
      retainBranchCarrierLoans(elseScopes);
    }
    if (borrowedResult) {
      scopes = thenCompletes ? thenScopes : elseScopes;
    } else if (hasResult) {
      const bool moveResult = value.info.traits.drop != DropKind::Trivial ||
                              !value.info.traits.copyable;
      (void)appendInstruction(
          {.kind =
               moveResult ? MirInstructionKind::Move : MirInstructionKind::Load,
           .hirValue = value.id,
           .result = resultFor(value),
           .operands = {{.kind = moveResult ? MirOperandKind::Move
                                            : MirOperandKind::Copy,
                         .place = temporary,
                         .type = value.info.type}},
           .info = value.info});
    }
    emittedValues.insert(value.id);
  }

  // Prefix `?` through `std::tryable`: the operand and its block's has_value
  // call complete on the incoming path, then one branch selects either the
  // take_value call or the residual exit. The exit evaluates take_residual
  // and, unless the residual already is the return type, the return type's
  // implicit construction; it ends the full expression's temporaries created
  // so far (newest first), performs lexical return cleanup and returns. Every
  // step is an ordinary call or construction verified like any other.
  void emitPropagation(const HirValue &value) {
    if (!value.propagation || value.info.type == SemanticType::Unknown) {
      valid = false;
      return;
    }
    const HirPropagationPlan &plan = *value.propagation;
    const HirValue *test = findValue(plan.test);
    const HirValue *success = findValue(plan.output);
    const HirValueId returnedValue =
        plan.residualReturn != 0 ? plan.residualReturn : plan.residual;
    const HirValue *returned = findValue(returnedValue);
    if (test == nullptr || success == nullptr || returned == nullptr ||
        test->info.type != SemanticType::Bool ||
        success->info.type != value.info.type ||
        returned->info.type != output.returnType) {
      valid = false;
      return;
    }
    const auto temporaryBaseline =
        fullExpressionTemporaryBaselines.find(value.fullExpression);
    const auto scopeBaseline =
        fullExpressionScopeBaselines.find(value.fullExpression);
    if (value.fullExpression == 0 ||
        temporaryBaseline == fullExpressionTemporaryBaselines.end() ||
        scopeBaseline == fullExpressionScopeBaselines.end()) {
      valid = false;
      return;
    }
    // Recursive operand emission may add nested result-region expressions and
    // rehash the baseline maps. Keep the entry state, not invalidated
    // iterators.
    const auto initialTemporaryDrops = temporaryBaseline->second;
    const auto initialScopes = scopeBaseline->second;
    // A named operand that the consuming steps take by value, such as an
    // enum without cleanup, is read once before the branch, so both steps
    // receive that one dominating value; a copied class instead copies at
    // each step's own input.
    if (!plan.consumesOperand && success->callPlan &&
        success->callPlan->arguments.size() == 1 &&
        success->callPlan->arguments.front().kind == HirCallInputKind::Value) {
      (void)valueOperand(plan.operand);
      if (terminated()) {
        emittedValues.insert(value.id);
        return;
      }
    }
    const MirOperand condition = valueOperand(plan.test);
    if (terminated()) {
      emittedValues.insert(value.id);
      return;
    }
    const std::vector<Scope> evaluatedScopes = scopes;
    const std::vector<TemporaryDrop> evaluatedTemporaryDrops = temporaryDrops;
    const auto evaluatedBindingLoans = bindingLoans;
    const MirBlockId successBlock = appendBlock();
    const MirBlockId residualBlock = appendBlock();
    terminate({.kind = MirTerminatorKind::Branch,
               .hirValue = value.id,
               .value = condition,
               .target = successBlock,
               .elseTarget = residualBlock});

    current = residualBlock;
    scopes = evaluatedScopes;
    temporaryDrops = evaluatedTemporaryDrops;
    const MirOperand result = valueOperand(returnedValue);
    if (!terminated()) {
      const MirDropObligationId enclosingPendingReturnDrop = pendingReturnDrop;
      prepareReturnOwnership(returnedValue, result);
      endReturnExpressionLoans(initialScopes);
      emitTemporaryDrops(initialTemporaryDrops, value.id, 0, 0, true);
      emitScopeExit(0);
      publishReturnOwnership(returnedValue);
      pendingReturnDrop = enclosingPendingReturnDrop;
      terminate({.kind = MirTerminatorKind::Return,
                 .hirValue = value.id,
                 .value = result});
    }

    current = successBlock;
    scopes = evaluatedScopes;
    temporaryDrops = evaluatedTemporaryDrops;
    // Cleanup on the residual path must not retire the success path's
    // carrier-to-owner mapping, just as it must not retire its scope loans.
    bindingLoans = evaluatedBindingLoans;
    if (value.info.category == ValueCategory::Place) {
      forwardBorrowResult(value, plan.output);
      emittedValues.insert(value.id);
      return;
    }
    // The result is the take_value call's result itself, carrying the
    // propagation's own obligation, so consumers see an ordinary call result
    // rather than a join temporary that a class without a default
    // constructor could not declare.
    HirValue call = *success;
    call.id = value.id;
    call.fullExpression = value.fullExpression;
    call.dropObligation = value.dropObligation;
    emitCall(call);
    if (!terminated()) {
      endAddedLoans(evaluatedScopes);
    }
    emittedValues.insert(value.id);
  }

  // An ordered compound update reads its place once, checks the arithmetic,
  // and commits the result. Representing those stages explicitly keeps the
  // schedule inside MIR's primitive vocabulary instead of one opaque
  // compound instruction a backend would have to re-derive.
  //
  // Semantics records the arithmetic domain separately from the destination
  // and gives arithmetic and checked narrowing independent source origins.
  // MIR preserves those facts as read -> compute -> convert -> commit.
  [[nodiscard]] static bool fixedWidthIntegerType(const SemanticType &type) {
    switch (type.kind) {
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64:
      return true;
    default:
      return false;
    }
  }

  // A fixed-width integer result makes the arithmetic operations below carry
  // a mandatory checked-outcome contract, so the decomposed operation must
  // inherit exactly one source origin. Bitwise updates carry no contract and
  // may be unchecked.
  [[nodiscard]] static bool
  orderedCompoundContractBearing(MirOperation operation) {
    switch (operation) {
    case MirOperation::Add:
    case MirOperation::Subtract:
    case MirOperation::Multiply:
    case MirOperation::Divide:
    case MirOperation::Remainder:
    case MirOperation::ShiftLeft:
    case MirOperation::ShiftRight:
      return true;
    default:
      return false;
    }
  }

  [[nodiscard]] static bool
  orderedCompoundOutcomes(const DefinedFailureOperation &failure,
                          MirOperation operation) {
    if (failure.propagation != FailurePropagationKind::None ||
        failure.localOrigins.size() > 1) {
      return false;
    }
    if (orderedCompoundContractBearing(operation) !=
        (failure.localOrigins.size() == 1)) {
      return false;
    }
    for (const DefinedFailureOrigin &origin : failure.localOrigins) {
      for (const DefinedFailureOutcome &outcome : origin.outcomes) {
        if (outcome.code == DefinedFailureCode::NumericConversionOutOfRange) {
          return false;
        }
      }
    }
    return true;
  }

  struct OrderedCompoundSchedule {
    DefinedFailureOperation readFailure;
    DefinedFailureOperation arithmeticFailure;
    DefinedFailureOperation conversionFailure;
    SemanticType arithmeticType = SemanticType::Unknown;
  };

  [[nodiscard]] static bool samePlaceRoot(const MirPlace &left,
                                          const MirPlace &right) {
    return left.root == right.root && left.binding == right.binding &&
           left.symbol == right.symbol && left.capture == right.capture &&
           left.temporary == right.temporary && left.value == right.value &&
           left.loan == right.loan;
  }

  [[nodiscard]] bool fixedArrayElementPlace(const MirPlace &place) const {
    if (place.projections.empty() ||
        place.projections.back().kind != MirProjectionKind::Index) {
      return false;
    }
    return std::any_of(output.places.begin(), output.places.end(),
                       [&](const MirPlace &candidate) {
                         return candidate.id != place.id &&
                                samePlaceRoot(candidate, place) &&
                                candidate.projections.size() + 1 ==
                                    place.projections.size() &&
                                std::equal(candidate.projections.begin(),
                                           candidate.projections.end(),
                                           place.projections.begin()) &&
                                candidate.type.kind == SemanticType::Array &&
                                candidate.type.arguments.size() == 1 &&
                                candidate.type.arguments.front() == place.type;
                       });
  }

  [[nodiscard]] std::optional<OrderedCompoundSchedule>
  orderedCompoundSchedule(const HirValue &value, MirPlaceId destination,
                          const SemanticType &operandType,
                          MirOperation operation) const {
    const MirPlace *place = output.findPlace(destination);
    const bool arrayElement =
        place != nullptr && fixedArrayElementPlace(*place);
    // A class subscript projection still denotes a selected user call, not a
    // physical element address. Splitting it into a Load and Assign would
    // invoke the accessor twice (and may require an unselected overload).
    // Keep that closed compound boundary until its call result is staged as
    // one exact reference. Ordinary field/reference paths remain primitive.
    const bool calledSubscript =
        place != nullptr && !arrayElement &&
        std::any_of(place->projections.begin(), place->projections.end(),
                    [](const MirPlaceProjection &projection) {
                      return projection.kind == MirProjectionKind::Index;
                    });
    const bool floating = semanticFloatFormat(value.info.type).has_value();
    const SemanticType arithmeticType = value.info.updateArithmeticType;
    if (place == nullptr || calledSubscript ||
        (!fixedWidthIntegerType(value.info.type) && !floating) ||
        (!fixedWidthIntegerType(arithmeticType) && !floating) ||
        place->type != value.info.type ||
        (!fixedWidthIntegerType(operandType) && !floating) ||
        value.info.category != ValueCategory::Value ||
        value.info.traits.drop != DropKind::Trivial ||
        value.info.traits.containsBorrowedState || value.ownership ||
        value.unsafeOperation != UnsafeOperationKind::None ||
        (value.definedFailure.propagation != FailurePropagationKind::None &&
         !((value.kind == HirValueKind::IndexSet ||
            value.kind == HirValueKind::DereferenceSet) &&
           value.functionTarget &&
           value.borrowOrigin != BorrowOriginKind::None))) {
      return std::nullopt;
    }

    OrderedCompoundSchedule schedule;
    schedule.arithmeticType = arithmeticType;
    for (const DefinedFailureOrigin &origin :
         value.definedFailure.localOrigins) {
      const bool bounds =
          arrayElement && !origin.outcomes.empty() &&
          std::all_of(origin.outcomes.begin(), origin.outcomes.end(),
                      [](const DefinedFailureOutcome &outcome) {
                        return outcome.code ==
                               DefinedFailureCode::IndexOutOfBounds;
                      });
      const bool conversion =
          origin.outcomes ==
          std::vector<DefinedFailureOutcome>{
              {.code = DefinedFailureCode::NumericConversionOutOfRange,
               .detail = DefinedFailureDetail::NumericCast}};
      (bounds       ? schedule.readFailure.localOrigins
       : conversion ? schedule.conversionFailure.localOrigins
                    : schedule.arithmeticFailure.localOrigins)
          .push_back(origin);
    }
    const bool arithmeticValid =
        floating
            ? schedule.arithmeticFailure.empty()
            : orderedCompoundOutcomes(schedule.arithmeticFailure, operation);
    if (schedule.readFailure.localOrigins.size() > 1 ||
        schedule.conversionFailure.localOrigins.size() > 1 ||
        !arithmeticValid) {
      return std::nullopt;
    }
    return schedule;
  }

  // Emits read -> checked operation -> commit for one eligible place update.
  // `result` names the MIR value the HIR value denotes: the pre-update read
  // for a postfix form, the committed operand otherwise.
  void emitOrderedCompoundSchedule(
      const HirValue &value, MirPlaceId destination, MirOperation operation,
      MirOperand sourceOperand, bool postfix, bool commitResult,
      const OrderedCompoundSchedule &schedule,
      UnsafeOperationKind unsafeOperation = UnsafeOperationKind::None,
      MirValueId preparedRead = 0) {
    const MirValueId canonical = mirValueFor(value);
    const MirValueId readValue = preparedRead != 0 ? preparedRead
                                 : postfix ? canonical
                                           : appendValue(value.id, value.info);
    if (preparedRead == 0)
      (void)appendInstruction({.kind = MirInstructionKind::Load,
                               .hirValue = value.id,
                               .result = readValue,
                               .operands = {{.kind = MirOperandKind::Copy,
                                             .place = destination,
                                             .type = value.info.type}},
                               .definedFailure = schedule.readFailure,
                               .info = value.info},
                              false);
    ExpressionInfo arithmeticInfo = value.info;
    arithmeticInfo.type = schedule.arithmeticType == SemanticType::Unknown
                              ? value.info.type
                              : schedule.arithmeticType;
    arithmeticInfo.traits = semanticTraits(arithmeticInfo.type);
    const bool conversion = arithmeticInfo.type != value.info.type;
    const MirValueId computedValue = postfix || commitResult || conversion
                                         ? appendValue(value.id, arithmeticInfo)
                                         : canonical;
    (void)appendInstruction({.kind = MirInstructionKind::Compute,
                             .hirValue = value.id,
                             .unsafeOperation = unsafeOperation,
                             .result = computedValue,
                             .operands = {{.kind = MirOperandKind::Value,
                                           .value = readValue,
                                           .type = value.info.type},
                                          std::move(sourceOperand)},
                             .operation = operation,
                             .definedFailure = schedule.arithmeticFailure,
                             .info = arithmeticInfo},
                            false);
    MirValueId committedValue = computedValue;
    if (conversion) {
      committedValue = postfix || commitResult
                           ? appendValue(value.id, value.info)
                           : canonical;
      (void)appendInstruction({.kind = MirInstructionKind::Compute,
                               .hirValue = value.id,
                               .result = committedValue,
                               .operands = {{.kind = MirOperandKind::Value,
                                             .value = computedValue,
                                             .type = arithmeticInfo.type}},
                               .operation = MirOperation::Convert,
                               .definedFailure = schedule.conversionFailure,
                               .info = value.info},
                              false);
    }
    // The commit keeps assignment's ordinary result contract; a prefix or
    // compound form denotes it, and a postfix form discards it in favor of
    // the pre-update read.
    (void)appendInstruction(
        {.kind = MirInstructionKind::Assign,
         .hirValue = value.id,
         .result = commitResult ? canonical : appendValue(value.id, value.info),
         .destination = destination,
         .operands = {{.kind = MirOperandKind::Value,
                       .value = committedValue,
                       .type = value.info.type}},
         .operation = MirOperation::Assign,
         .info = value.info});
    emittedValues.insert(value.id);
  }

  [[nodiscard]] bool
  rawPointerModifyScheduleEligible(const HirValue &value,
                                   MirPlaceId destination) const {
    const MirPlace *place = output.findPlace(destination);
    return place != nullptr &&
           value.info.type.kind == SemanticType::RawPointer &&
           value.info.type.arguments.size() == 1 &&
           place->type == value.info.type &&
           value.info.category == ValueCategory::Value &&
           value.info.traits.drop == DropKind::Trivial &&
           !value.info.traits.containsBorrowedState && !value.ownership &&
           value.unsafeOperation == UnsafeOperationKind::PointerArithmetic &&
           value.definedFailure.empty();
  }

  [[nodiscard]] std::optional<OrderedCompoundSchedule>
  rawPointerCompoundSchedule(const HirValue &value, MirPlaceId destination,
                             const SemanticType &operandType,
                             MirOperation operation) const {
    const MirPlace *place = output.findPlace(destination);
    const bool arrayElement =
        place != nullptr && fixedArrayElementPlace(*place);
    const bool unsafePointerUpdate =
        value.unsafeOperation == UnsafeOperationKind::PointerArithmetic ||
        value.unsafeOperation == UnsafeOperationKind::RawMember ||
        value.unsafeOperation == UnsafeOperationKind::RawIndex ||
        value.unsafeOperation == UnsafeOperationKind::RawDereference;
    if (place == nullptr ||
        (operation != MirOperation::PointerAdd &&
         operation != MirOperation::PointerSubtract) ||
        value.info.type.kind != SemanticType::RawPointer ||
        value.info.type.arguments.size() != 1 ||
        place->type != value.info.type || !fixedWidthIntegerType(operandType) ||
        value.info.category != ValueCategory::Value ||
        value.info.traits.drop != DropKind::Trivial ||
        value.info.traits.containsBorrowedState || value.ownership ||
        !unsafePointerUpdate ||
        value.definedFailure.propagation != FailurePropagationKind::None) {
      return std::nullopt;
    }

    OrderedCompoundSchedule schedule;
    for (const DefinedFailureOrigin &origin :
         value.definedFailure.localOrigins) {
      const bool bounds =
          arrayElement && !origin.outcomes.empty() &&
          std::all_of(origin.outcomes.begin(), origin.outcomes.end(),
                      [](const DefinedFailureOutcome &outcome) {
                        return outcome.code ==
                               DefinedFailureCode::IndexOutOfBounds;
                      });
      if (!bounds) {
        return std::nullopt;
      }
      schedule.readFailure.localOrigins.push_back(origin);
    }
    if (schedule.readFailure.localOrigins.size() > 1) {
      return std::nullopt;
    }
    return schedule;
  }

  void emitModify(const HirValue &value) {
    if (value.operands.size() != 1) {
      valid = false;
      return;
    }
    emitPlaceDependencies(value.operands.front());
    const MirOperation operation = modifierOperation(value);
    const MirPlaceId destination = placeForValue(value.operands.front());
    if (operation == MirOperation::None || destination == 0) {
      valid = false;
      return;
    }
    const bool increment = operation == MirOperation::PreIncrement ||
                           operation == MirOperation::PostIncrement;
    const bool postfix = operation == MirOperation::PostIncrement ||
                         operation == MirOperation::PostDecrement;
    const MirOperation arithmetic =
        increment ? MirOperation::Add : MirOperation::Subtract;
    if (rawPointerModifyScheduleEligible(value, destination)) {
      emitOrderedCompoundSchedule(
          value, destination,
          increment ? MirOperation::PointerAdd : MirOperation::PointerSubtract,
          {.kind = MirOperandKind::Constant,
           .literal = Literal{std::uint64_t{1}},
           .type = SemanticType::Int32},
          postfix, false, {}, UnsafeOperationKind::PointerArithmetic);
      return;
    }
    if (const std::optional<OrderedCompoundSchedule> schedule =
            orderedCompoundSchedule(value, destination, value.info.type,
                                    arithmetic)) {
      Literal one = std::uint64_t{1};
      if (const std::optional<BinaryFloatFormat> format =
              semanticFloatFormat(value.info.type)) {
        one = BinaryFloat{.bits = *format == BinaryFloatFormat::Binary64
                                      ? 0x3ff0000000000000ULL
                                      : 0x3f800000U,
                          .format = *format};
      }
      emitOrderedCompoundSchedule(value, destination, arithmetic,
                                  {.kind = MirOperandKind::Constant,
                                   .literal = std::move(one),
                                   .type = value.info.type},
                                  postfix, false, *schedule);
      return;
    }
    (void)appendInstruction({.kind = MirInstructionKind::Modify,
                             .hirValue = value.id,
                             .result = resultFor(value),
                             .destination = destination,
                             .operation = operation,
                             .definedFailure = value.definedFailure,
                             .info = value.info});
    emittedValues.insert(value.id);
  }

  // A terminal operand never supplies a value to its enclosing expression.
  // Keep the primary full-expression bookkeeping on the impossible normal
  // continuation, independently of the already-routed failure cleanup. This
  // also covers grouped/nested calls and a conditional with two terminal arms.
  void finishTerminalFullExpression(HirValueId id) {
    MirBlock *block = currentBlock();
    if (block == nullptr ||
        block->terminator.kind != MirTerminatorKind::Unreachable ||
        !isFullExpressionRoot(id)) {
      return;
    }
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      return;
    }
    const auto boundary = std::find_if(
        source.fullExpressions.begin(), source.fullExpressions.end(),
        [&](const HirFullExpression &item) {
          return item.id == value->fullExpression;
        });
    if (boundary == source.fullExpressions.end()) {
      return;
    }
    const auto temporary = fullExpressionTemporaryBaselines.find(boundary->id);
    const auto scope = fullExpressionScopeBaselines.find(boundary->id);
    if (temporary == fullExpressionTemporaryBaselines.end() ||
        scope == fullExpressionScopeBaselines.end()) {
      return;
    }
    const auto temporaryBaseline = temporary->second;
    const auto scopeBaseline = scope->second;
    const MirTerminator endpoint = block->terminator;
    block->terminator = {};
    endFullExpressionLoans(scopeBaseline);
    emitTemporaryDrops(temporaryBaseline, id, boundary->statement,
                       boundary->constructorInitializer);
    terminate(endpoint);
  }

  void emitValue(HirValueId id) {
    if (terminated()) {
      return;
    }
    emitValueImpl(id);
    finishTerminalFullExpression(id);
  }

  void emitValueImpl(HirValueId id) {
    if (id == 0 || emittedValues.contains(id)) {
      return;
    }
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      valid = false;
      return;
    }

    if (value->fullExpression != 0 &&
        !fullExpressionTemporaryBaselines.contains(value->fullExpression)) {
      fullExpressionTemporaryBaselines.emplace(value->fullExpression,
                                               temporaryDrops);
      fullExpressionScopeBaselines.emplace(value->fullExpression, scopes);
    }

    if (value->programConstantSubstitution) {
      emitProgramConstantSubstitution(*value);
      return;
    }

    for (const HirValueId input : value->sequencedInputs) {
      emitValue(input);
      if (terminated()) {
        emittedValues.insert(id);
        return;
      }
    }

    if (value->kind == HirValueKind::Propagation) {
      emitPropagation(*value);
      return;
    }

    if (value->kind == HirValueKind::Move) {
      (void)valueOperand(id);
      return;
    }
    if (value->kind == HirValueKind::Assignment ||
        value->kind == HirValueKind::MemberSet ||
        value->kind == HirValueKind::IndexSet ||
        value->kind == HirValueKind::DereferenceSet ||
        value->kind == HirValueKind::PlaceSet) {
      const bool overloadedBorrowStore =
          (value->kind == HirValueKind::IndexSet ||
           value->kind == HirValueKind::DereferenceSet) &&
          value->functionTarget &&
          value->borrowOrigin != BorrowOriginKind::None;
      bool reservedBorrowStore =
          overloadedBorrowStore && plainOverloadedBorrowStore(*value);
      if (value->kind == HirValueKind::PlaceSet && !value->operands.empty()) {
        emitPlaceDependencies(value->operands.front());
      } else if (value->kind == HirValueKind::MemberSet &&
                 !value->operands.empty()) {
        if (value->unsafeOperation == UnsafeOperationKind::RawMember) {
          (void)valueOperand(value->operands.front());
        } else {
          // A plain field store whose base place is an element reached
          // through a checked borrow-forming access call (for example
          // `points[i].x = ...` over an overloaded subscript) follows the
          // commit-time loan contract of the element store itself: the
          // call's receiver place and argument values are formed
          // target-first, the right operand then completes and its transient
          // element loans end, and only the checked access call that begins
          // the exclusive element loan, plus the field projection and the
          // committed write, remain for the commit. Without this deferral
          // the destination call's exclusive loan is active while the right
          // operand reads the same container, and loan verification rejects
          // a program semantic analysis accepted. A compound store instead
          // reads the stabilized prior value through the once-formed target
          // before its right operand, so it keeps the formed-target-first
          // schedule.
          const bool plainStore =
              !value->operation ||
              assignmentOperation(*value->operation) == MirOperation::Assign;
          const HirValue *deferredRoot =
              plainStore && value->operands.size() >= 2
                  ? pendingProjectedBorrowStoreRoot(value->operands.front())
                  : nullptr;
          if (deferredRoot != nullptr) {
            emitDeferredBorrowRootOperands(*deferredRoot);
            (void)valueOperand(value->operands.back());
          }
          // As for a field read, an element selected at a run-time index
          // below the field completes its check before the store.
          emitPlaceDependencies(
              value->operands.front(),
              dynamicallySelectedElement(value->operands.front()));
        }
      } else if (value->kind == HirValueKind::IndexSet &&
                 value->operands.size() >= 2) {
        if (overloadedBorrowStore) {
          emitCall(*value, true, reservedBorrowStore);
        } else if (value->unsafeOperation == UnsafeOperationKind::RawIndex) {
          (void)valueOperand(value->operands[0]);
        } else {
          emitPlaceDependencies(value->operands[0], true);
        }
        if (!overloadedBorrowStore) {
          (void)valueOperand(value->operands[1]);
        }
      } else if (value->kind == HirValueKind::DereferenceSet &&
                 !value->operands.empty()) {
        if (overloadedBorrowStore) {
          emitCall(*value, true, reservedBorrowStore);
        } else {
          (void)valueOperand(value->operands.front());
        }
      }
      if (reservedBorrowStore) {
        const auto reserved = valueLoans.find(value->id);
        reservedBorrowStore = reserved != valueLoans.end() &&
                              reserved->second != 0 &&
                              reserved->second <= output.loans.size() &&
                              output.loans[reserved->second - 1].twoPhase;
      }
      MirPlaceId destination = 0;
      if (overloadedBorrowStore) {
        destination = loanOrValuePlace(*value);
        if (destination != 0) {
          valuePlaces.insert_or_assign(value->id, destination);
        }
      } else {
        destination = destinationFor(*value);
      }
      attachPlaceIdentity(destination, *value);
      const MirOperation assignOperation =
          value->operation ? assignmentOperation(*value->operation)
                           : MirOperation::None;
      const MirOperation compoundOperation =
          orderedCompoundArithmetic(assignOperation);
      std::optional<OrderedCompoundSchedule> compoundSchedule;
      MirValueId compoundRead = 0;
      if (compoundOperation != MirOperation::None && !value->operands.empty()) {
        const HirValue *right = findValue(value->operands.back());
        if (right != nullptr) {
          compoundSchedule = orderedCompoundSchedule(
              *value, destination, right->info.type, compoundOperation);
        }
        if (compoundSchedule) {
          compoundRead = appendValue(value->id, value->info);
          (void)appendInstruction(
              {.kind = MirInstructionKind::Load,
               .hirValue = value->id,
               .result = compoundRead,
               .operands = {{.kind = MirOperandKind::Copy,
                             .place = destination,
                             .type = value->info.type}},
               .definedFailure = compoundSchedule->readFailure,
               .info = value->info},
              false);
        }
      }
      std::vector<MirOperand> operands;
      MirDropObligationId sourceObligation = 0;
      if (!value->operands.empty()) {
        const HirValueId sourceValue = value->operands.back();
        operands.push_back(valueOperand(sourceValue));
        sourceObligation = dropObligationForValue(sourceValue);
      }
      const MirOperation pointerCompoundOperation =
          assignOperation == MirOperation::AddAssign
              ? MirOperation::PointerAdd
              : (assignOperation == MirOperation::SubtractAssign
                     ? MirOperation::PointerSubtract
                     : MirOperation::None);
      const std::optional<OrderedCompoundSchedule> pointerCompoundSchedule =
          pointerCompoundOperation == MirOperation::None ||
                  operands.size() != 1 || sourceObligation != 0
              ? std::nullopt
              : rawPointerCompoundSchedule(*value, destination,
                                           operands.front().type,
                                           pointerCompoundOperation);
      if (pointerCompoundSchedule) {
        emitOrderedCompoundSchedule(
            *value, destination, pointerCompoundOperation,
            std::move(operands.front()), false, true, *pointerCompoundSchedule,
            value->unsafeOperation);
        return;
      }
      if (compoundSchedule) {
        if (operands.size() != 1 || sourceObligation != 0) {
          valid = false;
          return;
        }
        emitOrderedCompoundSchedule(*value, destination, compoundOperation,
                                    std::move(operands.front()), false, true,
                                    *compoundSchedule,
                                    UnsafeOperationKind::None, compoundRead);
        return;
      }
      if (reservedBorrowStore) {
        const auto reserved = valueLoans.find(value->id);
        if (reserved == valueLoans.end()) {
          valid = false;
          return;
        }
        (void)appendInstruction({.kind = MirInstructionKind::ActivateBorrow,
                                 .hirValue = value->id,
                                 .loan = reserved->second});
      }
      MirInstruction assign{.kind = MirInstructionKind::Assign,
                            .hirValue = value->id,
                            .result = resultFor(*value),
                            .destination = destination,
                            .operands = std::move(operands),
                            .operation = assignOperation,
                            .definedFailure = overloadedBorrowStore
                                                  ? localDefinedFailure(value)
                                                  : value->definedFailure,
                            .info = value->info};
      MirInstructionId preparation = 0;
      const MirPlace *replacementPlace = output.findPlace(destination);
      // A thread-local destination's first-use outcomes belong to the
      // EnsureThreadLocal guard already emitted for that place; appending the
      // store moves them there. They do not make the replacement fallible.
      const bool replacementFallible = std::any_of(
          assign.definedFailure.localOrigins.begin(),
          assign.definedFailure.localOrigins.end(), [](const auto &origin) {
            return std::any_of(origin.outcomes.begin(), origin.outcomes.end(),
                               [](const DefinedFailureOutcome outcome) {
                                 return !threadLocalGuardOutcome(outcome);
                               });
          });
      const bool checkedReplacement =
          replacementPlace != nullptr &&
          !replacementPlace->projections.empty() &&
          replacementPlace->projections.back().kind ==
              MirProjectionKind::Index &&
          assign.definedFailure.localOrigins.size() == 1 &&
          assign.definedFailure.localOrigins.front().outcomes ==
              std::vector<DefinedFailureOutcome>{
                  {.code = DefinedFailureCode::IndexOutOfBounds,
                   .detail = DefinedFailureDetail::FixedArray}};
      if (replacementPlace != nullptr &&
          (assign.operation == MirOperation::Assign ||
           assign.operation == MirOperation::None) &&
          (!replacementFallible || checkedReplacement)) {
        std::unordered_set<HirClassInstanceId> visiting;
        auto cleanup = uniqueCleanupType(replacementPlace->type, visiting);
        if (cleanup) {
          MirInstruction prepare{.kind =
                                     MirInstructionKind::CleanupBeforeAssign,
                                 .hirValue = value->id,
                                 .destination = destination,
                                 .replacementCleanup = *cleanup,
                                 .info = value->info};
          if (checkedReplacement)
            prepare.definedFailure.localOrigins =
                std::exchange(assign.definedFailure.localOrigins, {});
          if (mirDropHasPolymorphicCleanup(*cleanup))
            prepare.definedFailure.propagation =
                FailurePropagationKind::Destructor;
          for (const auto target : mirDropDestructorTargets(*cleanup)) {
            if (target == 0 ||
                target > definedFailureEffects.destructors.size() ||
                definedFailureEffects.destructors[target - 1]) {
              prepare.definedFailure.propagation =
                  FailurePropagationKind::Destructor;
              break;
            }
          }
          preparation = appendInstruction(std::move(prepare), false);
          assign.replacementPair = preparation;
        }
      }
      if (temporaryIsActive(sourceObligation)) {
        const MirPlace *destinationPlace = output.findPlace(destination);
        const MirDropObligationId target =
            destinationPlace != nullptr &&
                    destinationPlace->root == MirPlaceRootKind::Binding &&
                    destinationPlace->projections.empty()
                ? dropObligationForBinding(destinationPlace->binding)
                : 0;
        const MirDropObligation *sourceDrop =
            output.findDropObligation(sourceObligation);
        if (sourceDrop != nullptr && destinationPlace != nullptr &&
            sourceDrop->dropType.type != destinationPlace->type) {
          // A conversion into a distinct owning wrapper transfers the source
          // object out of its own lifetime. The destination keeps its
          // obligation, which now owns the converted value, as for a
          // storage-free source.
          appendLifecycle(assign, {.kind = MirLifecycleEventKind::TransferOut,
                                   .source = sourceObligation});
          (void)removeTemporary(sourceObligation);
        } else {
          appendLifecycle(assign, {.kind = MirLifecycleEventKind::Replace,
                                   .source = sourceObligation,
                                   .target = target});
        }
      }
      const auto assignment = appendInstruction(std::move(assign));
      if (preparation != 0) {
        for (auto &block : output.blocks)
          for (auto &instruction : block.instructions)
            if (instruction.id == preparation)
              instruction.replacementPair = assignment;
      }
      if (assignmentSnapshotRequests.contains(id)) {
        emitAssignmentResultCopy(*value, destination);
      }
      if (assignmentDestinationRequests.contains(id) && !overloadedBorrowStore)
        assignmentDestinations.emplace(id, destination);
      emittedValues.insert(id);
      if (overloadedBorrowStore) {
        endConsumedCallResultLoans({value->id}, *value);
      }
      // Projections of the result read its copy, never the store's accessor
      // place, whose loan has now ended.
      if (assignmentSnapshots.contains(id)) {
        valuePlaces.erase(id);
      }
      return;
    }
    if (value->kind == HirValueKind::Call &&
        value->intrinsic != IntrinsicKind::None) {
      emitCall(*value);
      emittedValues.insert(id);
      return;
    }
    if (value->constructorTarget ||
        value->constructorKind != ConstructorKind::Ordinary ||
        value->kind == HirValueKind::DirectInitializer) {
      emitConstruct(*value);
      emittedValues.insert(id);
      return;
    }
    if (value->kind == HirValueKind::Call ||
        (value->functionTarget && value->kind != HirValueKind::FunctionItem)) {
      emitCall(*value);
      emittedValues.insert(id);
      return;
    }
    if ((value->kind == HirValueKind::Unary ||
         value->kind == HirValueKind::Postfix) &&
        modifierOperation(*value) != MirOperation::None) {
      emitModify(*value);
      return;
    }
    if (value->kind == HirValueKind::Logical) {
      emitLogical(*value);
      return;
    }
    if (value->kind == HirValueKind::Conditional) {
      emitConditional(*value);
      return;
    }
    if (value->kind == HirValueKind::ScopedResult) {
      emitScopedResult(*value);
      return;
    }
    if (value->kind == HirValueKind::PackFold) {
      emitPackFold(*value);
      return;
    }
    if (value->info.category == ValueCategory::Place &&
        isPlaceExpression(value->kind) &&
        computeOperation(*value) != MirOperation::EnumConstant) {
      (void)valueOperand(id);
      return;
    }
    // A comma's object-valued left operand (a class value) is a discarded
    // evaluation, not an input of the comma: it completes, keeping
    // its temporaries' full-expression lifetime, and the comma forwards its
    // right operand. A place operand evaluates only its place.
    if (value->kind == HirValueKind::Binary &&
        value->operation == TokenKind::COMMA && value->operands.size() == 2) {
      const HirValue *left = findValue(value->operands.front());
      if (left != nullptr && (left->info.type.kind == SemanticType::Class)) {
        if (left->info.category == ValueCategory::Place &&
            isPlaceExpression(left->kind))
          emitPlaceDependencies(left->id);
        else
          emitValue(left->id);
        if (terminated()) {
          return;
        }
        MirInstruction forward{
            .kind = MirInstructionKind::Compute,
            .hirValue = value->id,
            .operands = {valueOperand(value->operands.back())},
            .operation = MirOperation::Identity,
            .info = value->info};
        if (terminated()) {
          return;
        }
        forward.result = resultFor(*value);
        (void)initializeValueLifecycle(forward, *value);
        (void)appendInstruction(std::move(forward));
        endConsumedCallResultLoans({value->operands.back()}, *value);
        emittedValues.insert(id);
        return;
      }
    }

    MirInstruction instruction{
        .kind = MirInstructionKind::Compute,
        .hirValue = value->id,
        .operation = computeOperation(*value),
        .literal = value->literal,
        .enumOwner = value->enumOwner,
        .enumValue = value->enumValue,
        .enumVariant = value->enumVariant,
        .payloadIndex = value->payloadIndex,
        .payloadExtractionKind = value->payloadExtractionKind,
        .intrinsic = value->intrinsic,
        .definedFailure = value->definedFailure,
        .functionTarget = value->functionTarget,
        .nativeCallbackAdapter = value->nativeCallbackAdapter,
        .lambdaTarget = value->lambdaTarget,
        .info = value->info};
    if (instruction.operation == MirOperation::Literal) {
      instruction.literalProvenance.kind = MirLiteralProvenanceKind::Source;
    }
    if (instruction.operation == MirOperation::AddressOf &&
        value->operands.size() == 1) {
      instruction.definedFailure =
          addressDefinedFailure(value->operands.front());
    }
    if (instruction.operation == MirOperation::Closure &&
        instruction.lambdaTarget) {
      const HirLambda *lambda = program.findLambda(*instruction.lambdaTarget);
      if (lambda == nullptr) {
        valid = false;
      } else {
        instruction.closureCaptureTypes.reserve(lambda->captures.size());
        instruction.closureCaptureModes.reserve(lambda->captures.size());
        for (const LambdaCaptureInfo &capture : lambda->captures) {
          instruction.closureCaptureTypes.push_back(capture.type);
          instruction.closureCaptureModes.push_back(capture.mode);
        }
      }
    }
    if (instruction.operation == MirOperation::None) {
      valid = false;
    }
    std::vector<MirDropObligationId> payloadPreparedDrops;
    std::vector<MirLoanId> payloadTemporaryLoans;
    std::vector<MirLoanId> payloadConsumedGroups;
    const bool stagedPayload =
        value->kind == HirValueKind::PayloadConstruction && value->callPlan;
    if (stagedPayload) {
      if (value->callPlan->receiver ||
          value->callPlan->arguments.size() != value->operands.size()) {
        valid = false;
        return;
      }
      instruction.callSite = value->id;
      for (const auto &argument : value->callPlan->arguments) {
        auto operand =
            argument.kind == HirCallInputKind::CopyValue
                ? copyArgumentOperand(argument.value, argument.parameterType)
                : argumentOperand(argument.value, argument.parameterType);
        if (terminated())
          return;
        auto prepared = prepareCallInput(
            value->id, argument.value, argument.parameterType, argument.kind,
            MirCallInputRole::Argument, argument.parameterIndex,
            std::move(operand), true, argument.defaultArgument);
        instruction.parameterTypes.push_back(argument.parameterType);
        instruction.operands.push_back(std::move(prepared.operand));
        if (prepared.parameterDrop)
          payloadPreparedDrops.push_back(prepared.parameterDrop);
        if (prepared.loan)
          payloadTemporaryLoans.push_back(prepared.loan);
        const auto *source = findValue(argument.value);
        if (source && source->kind == HirValueKind::PayloadConstruction &&
            argument.kind == HirCallInputKind::Value &&
            source->info.traits.movable &&
            source->info.traits.containsBorrowedState) {
          const auto group = loanForValue(source->id);
          const auto *retained = output.findLoan(group);
          if (retained && retained->kind == MirLoanKind::Stored &&
              !retained->entry && !retained->escapes &&
              retained->producedBy == source->id) {
            if (retained->access == AccessMode::Mutable ||
                std::any_of(retained->additionalDependencies.begin(),
                            retained->additionalDependencies.end(),
                            [](const auto &dependency) {
                              return dependency.access == AccessMode::Mutable;
                            }))
              payloadConsumedGroups.push_back(group);
          } else
            valid = false;
        }
      }
    }
    for (std::size_t index = 0; index < value->operands.size(); ++index) {
      if (stagedPayload)
        break;
      const HirValueId operand = value->operands[index];
      if (instruction.operation == MirOperation::Closure &&
          index < instruction.closureCaptureModes.size() &&
          isBorrowingCapture(instruction.closureCaptureModes[index])) {
        instruction.operands.push_back(
            argumentOperand(operand, instruction.closureCaptureTypes[index]));
      } else if (instruction.operation == MirOperation::Closure &&
                 index < instruction.closureCaptureModes.size() &&
                 instruction.closureCaptureModes[index] ==
                     LambdaCaptureMode::Copy) {
        emitPlaceDependencies(operand);
        const HirValue *sourceValue = findValue(operand);
        instruction.operands.push_back({.kind = MirOperandKind::Copy,
                                        .place = placeForValue(operand),
                                        .type = sourceValue == nullptr
                                                    ? SemanticType::Unknown
                                                    : sourceValue->info.type});
      } else if (instruction.operation == MirOperation::PayloadExtract) {
        if (const auto subject = payloadSwitchSubjects.find(operand);
            subject != payloadSwitchSubjects.end()) {
          instruction.operands.push_back(subject->second);
          continue;
        }
        const HirValue *sourceValue = findValue(operand);
        const MirPlaceId sourcePlace = placeForValue(operand);
        if (sourceValue != nullptr && sourcePlace != 0 &&
            sourceValue->info.category == ValueCategory::Place &&
            sourceValue->info.type.kind == SemanticType::Enum &&
            (sourceValue->info.traits.drop == DropKind::Lexical ||
             value->info.category == ValueCategory::Place)) {
          emitPlaceDependencies(operand);
          instruction.operands.push_back({.kind = MirOperandKind::BorrowRead,
                                          .place = sourcePlace,
                                          .type = sourceValue->info.type});
        } else {
          instruction.operands.push_back(valueOperand(operand));
        }
      } else {
        instruction.operands.push_back(
            instruction.operation == MirOperation::LogicalNot
                ? conditionOperand(operand)
            : instruction.operation == MirOperation::AddressOf
                ? addressOperand(operand)
                : valueOperand(operand));
      }
      if (terminated()) {
        return;
      }
    }
    instruction.result = resultFor(*value);
    if (value->kind == HirValueKind::PayloadConstruction &&
        instruction.result && value->borrowDependencies) {
      // appendValue transports the complete semantic fact; independently
      // checked ordered inputs establish its origins at program verification.
      const auto &complete =
          output.values[*instruction.result - 1].borrowDependencies;
      if (!complete || (!complete->dependencies.empty() && !stagedPayload)) {
        valid = false;
        return;
      }
      if (!complete->dependencies.empty()) {
        const auto &primary = complete->dependencies.front();
        const auto loan =
            createLoan(MirLoanKind::Stored, primary.source, primary.access,
                       value->id, 0, 0, primary.parent);
        output.loans[loan - 1].designatedFields = primary.designatedFields;
        output.loans[loan - 1].conditions = primary.conditions;
        output.loans[loan - 1].additionalDependencies.assign(
            complete->dependencies.begin() + 1, complete->dependencies.end());
        instruction.loan = loan;
        valueLoans.insert_or_assign(value->id, loan);
        if (!scopes.empty())
          scopes.back().loans.push_back(loan);
      }
    }
    // A published text payload (L-OWN-02) holds its argument's owner, as a
    // constructor's published text field holds it; the retained loan, not a
    // complete set, carries it. The value cannot be reassigned, so the
    // payload symbol alone fixes its alternative.
    if (value->kind == HirValueKind::PayloadConstruction &&
        instruction.result && value->dependentTextPublication) {
      // The text argument's own loan names its owner place.
      const std::size_t parameter = value->borrowFields.empty()
                                        ? 0
                                        : value->borrowFields.front().parameter;
      const MirLoan *argumentLoan =
          value->borrowFields.size() == 1 && value->enumVariant &&
                  parameter < value->operands.size()
              ? output.findLoan(loanForValue(value->operands[parameter]))
              : nullptr;
      const MirPlaceId origin = argumentLoan ? argumentLoan->source : 0;
      if (origin == 0) {
        valid = false;
        return;
      }
      const auto loan = createLoan(MirLoanKind::Stored, origin,
                                   AccessMode::ReadOnly, value->id);
      output.loans[loan - 1].designatedFields =
          value->borrowFields.front().resultFields;
      // The loan names the payload; a payload construct carries no
      // call-style borrow summary.
      instruction.dependentTextPublication = true;
      instruction.loan = loan;
      valueLoans.insert_or_assign(value->id, loan);
      if (!scopes.empty())
        scopes.back().loans.push_back(loan);
    }
    if (value->kind == HirValueKind::ArrayInitializer ||
        (value->kind == HirValueKind::PayloadConstruction && !stagedPayload) ||
        value->kind == HirValueKind::Lambda) {
      for (std::size_t index = 0; index < value->operands.size(); ++index) {
        if (value->kind == HirValueKind::Lambda &&
            index < instruction.closureCaptureModes.size() &&
            instruction.closureCaptureModes[index] != LambdaCaptureMode::Move) {
          continue;
        }
        const HirValueId operand = value->operands[index];
        transferTemporaryOut(instruction, operand);
      }
    }
    for (const auto drop : payloadPreparedDrops) {
      if (!removeTemporary(drop))
        valid = false;
      appendLifecycle(instruction, {.kind = MirLifecycleEventKind::TransferOut,
                                    .source = drop});
    }
    if (value->kind != HirValueKind::PayloadExtraction ||
        value->info.category != ValueCategory::Place)
      (void)initializeValueLifecycle(instruction, *value);
    if (instruction.operation == MirOperation::Closure &&
        !value->captureBorrowDependencies.empty()) {
      const auto *target = program.findLambda(*instruction.lambdaTarget);
      std::vector<MirLoanDependency> dependencies;
      for (const auto &dependency : value->captureBorrowDependencies) {
        const auto capture = target == nullptr ? 0 : [&] {
          for (std::size_t index = 0; index < target->captures.size(); ++index)
            if (!dependency.designatedFields.empty() &&
                target->captures[index].bindingSymbol ==
                    dependency.designatedFields.front())
              return index + 1;
          return std::size_t{0};
        }();
        if (capture == 0 || capture > instruction.operands.size()) {
          valid = false;
          continue;
        }
        auto sourcePlace = instruction.operands[capture - 1].place;
        if (!isBorrowingCapture(target->captures[capture - 1].mode)) {
          const std::vector<SymbolId> fields(
              dependency.designatedFields.begin() + 1,
              dependency.designatedFields.end());
          const auto *retained =
              output.findLoan(loanForValue(value->operands[capture - 1]));
          if (retained && retained->kind != MirLoanKind::Stored &&
              retained->storedDependency != 0)
            retained = output.findLoan(retained->storedDependency);
          sourcePlace = 0;
          if (retained && retained->entry)
            sourcePlace = projectBorrowFields(retained->source, fields);
          else if (retained) {
            if (retained->designatedFields == fields)
              sourcePlace = retained->source;
            // Semantics expands a compact one-reference source into its exact
            // field path before prefixing the capture. Program verification
            // independently checks that the source has precisely that path.
            else if (retained->designatedFields.empty() &&
                     retained->additionalDependencies.empty() &&
                     !fields.empty())
              sourcePlace = retained->source;
            for (const auto &nested : retained->additionalDependencies)
              if (nested.designatedFields == fields)
                sourcePlace = nested.source;
          }
          if (sourcePlace == 0)
            sourcePlace = projectBorrowFields(
                instruction.operands[capture - 1].place, fields);
        }
        const auto *source = output.findPlace(sourcePlace);
        if (source == nullptr) {
          valid = false;
          continue;
        }
        if (source->key != dependency.place) {
          auto projected = *source;
          projected.id = 0;
          projected.key = dependency.place;
          sourcePlace = appendPlace(std::move(projected));
        }
        const auto parent = dependency.parent == 0 ? 0
                            : semanticLoans.contains(dependency.parent)
                                ? semanticLoans.at(dependency.parent)
                                : 0;
        if (dependency.parent != 0 && parent == 0)
          valid = false;
        dependencies.push_back({.source = sourcePlace,
                                .parent = parent,
                                .designatedFields = dependency.designatedFields,
                                .access = dependency.access});
      }
      if (!dependencies.empty()) {
        const auto &primary = dependencies.front();
        const auto loan =
            createLoan(MirLoanKind::Stored, primary.source, primary.access,
                       value->id, 0, 0, primary.parent);
        output.loans[loan - 1].designatedFields = primary.designatedFields;
        output.loans[loan - 1].additionalDependencies.assign(
            dependencies.begin() + 1, dependencies.end());
        instruction.loan = loan;
        instruction.borrowOrigin = BorrowOriginKind::Argument;
        for (std::size_t index = 0; index < target->captures.size(); ++index)
          if (target->captures[index].bindingSymbol ==
              primary.designatedFields.front())
            instruction.borrowArgument = index;
        valueLoans.insert_or_assign(value->id, loan);
        if (!scopes.empty())
          scopes.back().loans.push_back(loan);
      }
    }
    (void)appendInstruction(std::move(instruction));
    for (const auto group : payloadConsumedGroups) {
      (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                               .hirValue = value->id,
                               .loan = group});
      removeLoanFromState(group, scopes, bindingLoans);
    }
    for (auto loan = payloadTemporaryLoans.rbegin();
         loan != payloadTemporaryLoans.rend(); ++loan) {
      (void)appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = *loan});
      removeLoanFromState(*loan, scopes, bindingLoans);
    }
    // An extraction does not consume an observed switch subject: the switch
    // ends that subject's place loans once the arm has bound its payloads.
    std::vector<HirValueId> consumedOperands = value->operands;
    if (value->kind == HirValueKind::PayloadExtraction)
      std::erase_if(consumedOperands, [&](HirValueId operand) {
        return payloadSwitchSubjects.contains(operand);
      });
    endConsumedCallResultLoans(consumedOperands, *value);
    emittedValues.insert(id);
  }

  [[nodiscard]] MirOperand
  referenceOperand(HirValueId valueId, HirBindingId binding = 0,
                   std::optional<AccessMode> requestedAccess = {}) {
    const HirValue *value = findValue(valueId);
    if (value == nullptr) {
      return {};
    }
    ensureThreadLocalValue(valueId);
    emitPlaceDependencies(valueId);
    if (terminated()) {
      return {};
    }
    const MirLoanId existing = loanForValue(valueId);
    const HirLoan *desired = binding == 0 ? nullptr : loanForBinding(binding);
    if (desired != nullptr && value->kind == HirValueKind::PayloadExtraction &&
        value->info.category == ValueCategory::Place) {
      emitValue(valueId);
      const MirPlaceId field = placeForValue(valueId);
      if (field == 0) {
        valid = false;
        return {};
      }
      // Ordinary payload borrowing protects the enum's field storage. A
      // stored-reference extraction instead protects external owners and
      // retains its evaluated address separately in referenceSelection.
      const MirPlaceId origin =
          desired->referenceSelection ? placeForLoan(*desired, value) : field;
      if (origin == 0) {
        valid = false;
        return {};
      }
      if (!desired->referenceSelection)
        output.places[field - 1].key = desired->place;
      MirLoanId parent = 0;
      if (desired->parent != 0) {
        const auto found = semanticLoans.find(desired->parent);
        if (found == semanticLoans.end()) {
          valid = false;
          return {};
        }
        parent = found->second;
      }
      const MirLoanId loan =
          createLoan(MirLoanKind::Local, origin, desired->access, valueId,
                     binding, 0, parent);
      recordLoanIdentity(loan, *desired, binding);
      (void)appendInstruction(
          {.kind = MirInstructionKind::Borrow,
           .hirValue = valueId,
           .operands = {{.kind = parent != 0 ? MirOperandKind::Loan
                                 : desired->access == AccessMode::Mutable
                                     ? MirOperandKind::BorrowWrite
                                     : MirOperandKind::BorrowRead,
                         .place = field,
                         .loan = parent,
                         .type = value->info.type}},
           .loan = loan,
           .info = value->info});
      scopes.back().loans.push_back(loan);
      valueLoans.insert_or_assign(valueId, loan);
      bindingLoans.insert_or_assign(binding, loan);
      return {.kind = MirOperandKind::Loan,
              .place = field,
              .loan = loan,
              .type = value->info.type};
    }
    if (desired != nullptr) {
      if (const auto *alias = output.findLoan(existing);
          alias && desired->access == AccessMode::ReadOnly &&
          exactReadonlyRangeElementAlias(binding, *value, *alias))
        return {.kind = MirOperandKind::Loan,
                .loan = alias->id,
                .type = value->info.type};
      const bool exactRangeElement = std::any_of(
          source.statements.begin(), source.statements.end(),
          [&](const HirStatement &statement) {
            return statement.rangeIteration &&
                   statement.rangeIteration->elementLoan ==
                       desired->semanticLoan &&
                   statement.rangeIteration->elementAccessValue == valueId;
          });
      if (exactRangeElement && existing != 0 &&
          isPhysicalProxyRangeProtocolBinding(binding)) {
        const auto *selected = output.findLoan(existing);
        const auto range = std::find_if(
            source.statements.begin(), source.statements.end(),
            [&](const HirStatement &statement) {
              return statement.rangeIteration &&
                     (statement.rangeIteration->element == binding ||
                      statement.rangeIteration->elementOwner == binding) &&
                     statement.rangeIteration->elementAccessValue == valueId;
            });
        const auto iterator =
            range == source.statements.end()
                ? bindingLoans.end()
                : bindingLoans.find(range->rangeIteration->iterator);
        const auto *cursor = iterator == bindingLoans.end()
                                 ? nullptr
                                 : output.findLoan(iterator->second);
        const auto owner =
            range == source.statements.end()
                ? semanticLoans.end()
                : semanticLoans.find(range->rangeIteration->rangeLoan);
        if (selected != nullptr && cursor != nullptr &&
            selected->kind == MirLoanKind::CallResult &&
            selected->producedBy == valueId && !selected->escapes &&
            selected->carriers.empty() && selected->access == desired->access &&
            selected->storedDependency == cursor->id &&
            cursor->kind == MirLoanKind::Stored &&
            owner != semanticLoans.end() && cursor->parent == owner->second &&
            loanIsActive(scopes, selected->id) &&
            loanIsActive(scopes, cursor->id)) {
          // The range's semantic loan establishes the traversal boundary.
          // Dereference may instead return an inline cursor projection;
          // its selected call-result loan protects that physical shell and
          // separately retains the cursor's checked owner dependency.
          // Do not replace this producer identity with the range shell.
          std::erase(output.loans[owner->second - 1].carriers, binding);
          return {.kind = MirOperandKind::Loan,
                  .loan = existing,
                  .type = value->info.type};
        }
      }
      if (const auto found = semanticLoans.find(desired->semanticLoan);
          found != semanticLoans.end()) {
        // A by-reference capture's entry loan is held by the closure
        // environment, not by a binding, so a local alias never joins it as
        // a carrier; it carries a physical loan below that entry instead.
        if (const MirLoan *entry = output.findLoan(found->second);
            binding != 0 && entry != nullptr &&
            capturedReferenceEntry(*entry)) {
          const MirLoanId entryId = entry->id;
          // An accessor result or another alias already borrows below the
          // entry; this alias carries that same physical loan.
          if (existing != 0 &&
              capturedEntryDescendant(existing, entryId, desired->access))
            return {.kind = MirOperandKind::Loan,
                    .loan = existing,
                    .type = value->info.type};
          // Otherwise the initializer names a referent reached through the
          // capture itself, or through such an alias.
          const MirPlaceId referent =
              existing == 0 || existing == entryId ? placeForValue(valueId) : 0;
          const MirPlace *place = output.findPlace(referent);
          const MirPlace *slot = output.findPlace(entry->source);
          MirLoanId parent = 0;
          if (place != nullptr && slot != nullptr &&
              !place->projections.empty() &&
              place->projections.front().kind ==
                  MirProjectionKind::Dereference &&
              place->type == value->info.type) {
            if (place->root == MirPlaceRootKind::Symbol &&
                place->symbol == slot->symbol &&
                place->capture == slot->capture)
              parent = entryId;
            else if (const auto carried = bindingLoans.find(place->binding);
                     place->root == MirPlaceRootKind::Binding &&
                     carried != bindingLoans.end() &&
                     capturedEntryDescendant(carried->second, entryId,
                                             desired->access))
              parent = carried->second;
          }
          if (parent != 0 &&
              (desired->access == AccessMode::ReadOnly ||
               output.loans[parent - 1].access == AccessMode::Mutable)) {
            const MirLoanId loan =
                createLoan(MirLoanKind::Local, referent, desired->access,
                           valueId, 0, 0, parent);
            (void)appendInstruction({.kind = MirInstructionKind::Borrow,
                                     .hirValue = valueId,
                                     .operands = {{.kind = MirOperandKind::Loan,
                                                   .place = referent,
                                                   .loan = parent,
                                                   .type = value->info.type}},
                                     .loan = loan,
                                     .info = value->info});
            scopes.back().loans.push_back(loan);
            valueLoans.insert_or_assign(valueId, loan);
            return {.kind = MirOperandKind::Loan,
                    .place = referent,
                    .loan = loan,
                    .type = value->info.type};
          }
        }
        if (existing != 0 && existing != found->second) {
          const MirLoan *transient = output.findLoan(existing);
          const MirLoan *retained = output.findLoan(found->second);
          // Several shared aliases can retain one semantic owner identity
          // even when an accessor selects a different runtime referent. Keep
          // the owner loan here; reference initialization separately captures
          // the checked descendant call-result address before its loan ends.
          if (transient == nullptr || retained == nullptr ||
              desired->access != AccessMode::ReadOnly ||
              (!sameBorrowSource(transient->source, retained->source) &&
               !retainableCallResultChain(existing, found->second, valueId,
                                          desired->access))) {
            reject(valueId, "reference initializer's accessor result does not "
                            "retain the selected owner and access");
          }
        }
        recordLoanIdentity(found->second, *desired, binding);
        return {.kind = MirOperandKind::Loan,
                .loan = found->second,
                .type = value->info.type};
      }

      const auto desiredSourcePlace = [&]() {
        // Semantic places describe overlap, not every executable projection.
        // In particular a reference field's identity omits its dereference.
        // Use the exact expression place before materializing any parent-based
        // fallback: even an unused, incorrectly typed place fails verification.
        const MirPlaceId concrete = placeForValue(valueId);
        const MirPlace *place = output.findPlace(concrete);
        const auto *parent =
            desired->parent == 0 ? nullptr : source.findLoan(desired->parent);
        // A place behind a mapped carrier's stored reference field, such as
        // `pair.second.value`, is selected through that field the same way.
        const bool projectedStoredReference =
            value->kind == HirValueKind::MemberAccess &&
            (value->storedReferenceAccess ||
             (place != nullptr && storedReferenceAccessFor(*place))) &&
            parent != nullptr &&
            parent->kind == SemanticLoanKind::StoredValue &&
            !parent->designatedFields.empty();
        if (place != nullptr &&
            ((place->key && *place->key == desired->place) ||
             exactRangeElement || projectedStoredReference)) {
          if (projectedStoredReference)
            output.places[concrete - 1].key = desired->place;
          semanticLoanPlaces.insert_or_assign(desired->semanticLoan, concrete);
          return concrete;
        }
        return placeForLoan(*desired, value);
      };
      MirLoanId parent = 0;
      if (desired->parent != 0) {
        const auto transferred = callResultTransferredFrom.find(existing);
        const auto oldParent = semanticLoans.find(desired->parent);
        const auto *published = output.findLoan(existing);
        if (transferred != callResultTransferredFrom.end() &&
            oldParent != semanticLoans.end() &&
            std::find(transferred->second.begin(), transferred->second.end(),
                      oldParent->second) != transferred->second.end() &&
            published != nullptr &&
            (!loanIsActive(scopes, oldParent->second) ||
             sharedTransferredGroups.contains(oldParent->second)) &&
            retainableCallResultChain(existing, published->parent, valueId,
                                      desired->access)) {
          // This expression's checked consuming call published the exact
          // address, transferred its external ancestry and retired the old
          // group. Keep that executable publication rather than recreating a
          // child of the already-ended semantic input identity.
          recordLoanIdentity(existing, *desired, binding);
          return {.kind = MirOperandKind::Loan,
                  .loan = existing,
                  .type = value->info.type};
        }
        if (const auto retainedParent = semanticLoans.find(desired->parent);
            (!exactRangeElement || desired->access == AccessMode::ReadOnly) &&
            retainedParent != semanticLoans.end() &&
            retainableCallResultChain(existing, retainedParent->second, valueId,
                                      desired->access)) {
          // The call-result chain this expression built for this very value
          // already bottoms out at exactly the loan the retained semantic
          // loan names as its parent, so its outermost link is the exact
          // runtime referent — the same reason a parentless call-result chain
          // retains its own below. A reference carrier is what gives a
          // retained loan a semantic parent at all, so this is the route
          // every multi-link chain reached through a reference parameter or a
          // local reference binding takes.
          //
          // Retain that identity rather than ending the chain and recreating
          // a Local loan from the semantic place. That place states the
          // returned referent's type over a projection chain that stops at
          // the loan's coarser overlap identity — the parent plus the
          // receiver subobject, never the address the outer call returned. No
          // later stage can key it, and the backend cannot publish the
          // returned address from it. Deciding before `placeForLoan` also
          // keeps that place out of the body: materializing it and leaving it
          // unused would still fail the emitter's place-coherence scan, which
          // reads every place a body records.
          //
          // Read-only range elements use this same exact returned address.
          // Replacing their call result with a Local loan under the shared
          // range would lose the accessor relationship. Mutable range elements
          // still require their direct child loan and concrete selected place.
          // The links this reborrows stay live. A parent must outlive its
          // child reborrow on every edge, which the full-expression release
          // preserves for a retained chain, and the owning scope ends the
          // whole chain child-first.
          recordLoanIdentity(existing, *desired, binding);
          return {.kind = MirOperandKind::Loan,
                  .loan = existing,
                  .type = value->info.type};
        }
        // Mutable range elements have a concrete indexed/dereference place
        // even though their semantic overlap identity is deliberately rooted
        // at the protected owner. Use that concrete place for transient-chain
        // reconciliation as well as for the child loan below; materializing a
        // scalar place directly at the parent range loan would invent an
        // unspellable alias and lose the runtime element selection.
        const MirPlaceId desiredSource = desiredSourcePlace();
        if (desiredSource == 0) {
          reject(valueId, "reference initializer cannot materialize its "
                          "selected semantic origin");
          return {};
        }
        const auto found = semanticLoans.find(desired->parent);
        if (found == semanticLoans.end()) {
          valid = false;
          return {};
        }
        parent = found->second;
        if (existing != 0 && existing != parent) {
          const MirLoan *parentLoan = output.findLoan(parent);
          std::vector<MirLoanId> transientChain;
          std::unordered_set<MirLoanId> visited;
          MirLoanId transientId = existing;
          bool compatible = parentLoan != nullptr;
          while (compatible && transientId != parent) {
            const MirLoan *transient = output.findLoan(transientId);
            compatible =
                transient != nullptr && visited.insert(transientId).second &&
                transient->kind == MirLoanKind::CallResult &&
                (desired->access == AccessMode::ReadOnly ||
                 transient->access == AccessMode::Mutable) &&
                transient->carriers.empty() && !transient->escapes &&
                loanIsActive(scopes, transientId) &&
                (sameBorrowSource(transient->source, desiredSource) ||
                 exactRangeElement) &&
                (transientChain.empty() ? transient->producedBy == valueId
                                        : transient->producedBy != 0);
            if (!compatible) {
              break;
            }
            transientChain.push_back(transientId);
            transientId = transient->parent;
          }
          compatible =
              compatible && transientId == parent && !transientChain.empty();
          if (!compatible) {
            valid = false;
          } else {
            for (const MirLoanId transient : transientChain) {
              (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                       .hirValue = valueId,
                                       .loan = transient});
              removeLoanFromState(transient, scopes, bindingLoans);
            }
          }
        }
      } else if (existing != 0) {
        const MirLoan &sourceLoan = output.loans[existing - 1];
        if (sourceLoan.kind == MirLoanKind::CallResult) {
          // A root call-result loan is already the exact runtime referent.
          // Retain that identity instead of ending it and recreating a Local
          // loan from the (necessarily coarser) receiver/argument origin.
          // The latter is sufficient for overlap checking, but it cannot
          // recover the address of a returned subobject for backend
          // publication. Derived call-result chains still normalize through
          // their semantic parent below so parent suspension/reactivation is
          // preserved.
          if (sourceLoan.parent == 0) {
            recordLoanIdentity(existing, *desired, binding);
            return {.kind = MirOperandKind::Loan,
                    .loan = existing,
                    .type = value->info.type};
          }
          std::vector<MirLoanId> transientChain;
          std::unordered_set<MirLoanId> visited;
          MirLoanId transient = existing;
          bool compatible = true;
          while (transient != 0 && compatible &&
                 visited.insert(transient).second) {
            const MirLoan &candidate = output.loans[transient - 1];
            compatible = candidate.kind == MirLoanKind::CallResult &&
                         candidate.carriers.empty() && !candidate.escapes &&
                         loanIsActive(scopes, transient);
            if (compatible) {
              transientChain.push_back(transient);
              transient = candidate.parent;
            }
          }
          if (!compatible || transient != 0 || transientChain.empty()) {
            valid = false;
          } else if (desired->access == AccessMode::ReadOnly ||
                     sourceLoan.access == AccessMode::Mutable) {
            // The chain is a call-result reborrow chain the expression built
            // for this very value — a borrowed call result passed as the
            // argument (or receiver) of the call whose result is retained.
            // Its outermost link is the exact runtime referent for the same
            // reason the root case above is, so retain that identity rather
            // than ending the chain and recreating a Local loan from the
            // origin place: the recreated loan cannot name the address the
            // outer call returned, and its source place would record the
            // returned referent's type over a projection chain deriving the
            // origin's. The links it reborrows stay live; a parent must
            // outlive its child reborrow on every edge, which
            // `retainedReborrowAncestors` keeps true when the full
            // expression ends.
            recordLoanIdentity(existing, *desired, binding);
            return {.kind = MirOperandKind::Loan,
                    .loan = existing,
                    .type = value->info.type};
          } else {
            for (const MirLoanId ended : transientChain) {
              (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                       .hirValue = valueId,
                                       .loan = ended});
              removeLoanFromState(ended, scopes, bindingLoans);
            }
          }
        } else if (desired->storedDependency == 0 &&
                   (!sourceLoan.entry ||
                    sourceLoan.semanticLoan == desired->semanticLoan)) {
          recordLoanIdentity(existing, *desired, binding);
          return {.kind = MirOperandKind::Loan,
                  .loan = existing,
                  .type = value->info.type};
        }
      }

      // A field or element of a call's returned reference is a subobject of
      // that call's checked referent, whether the call is a method, an
      // overloaded `operator->` or an overloaded `operator*`. Retain the
      // root call-result loan, as a reference bound to the returned
      // reference itself does, and keep the projected place as the address.
      // A Local loan on the semantic origin could not name that address, and
      // one on the projected place would leave the call's owner unborrowed.
      // Through a reference carrier the call reborrows the same semantic
      // parent the binding names, so it retains that link too.
      if (existing == 0)
        if (const MirPlace *projected =
                output.findPlace(placeForValue(valueId));
            projected != nullptr && projected->root == MirPlaceRootKind::Loan &&
            !projected->projections.empty() &&
            std::all_of(projected->projections.begin(),
                        projected->projections.end(),
                        [](const MirPlaceProjection &projection) {
                          return projection.kind == MirProjectionKind::Field ||
                                 projection.kind == MirProjectionKind::Index;
                        })) {
          const MirLoan *selected = output.findLoan(projected->loan);
          if (selected != nullptr &&
              selected->kind == MirLoanKind::CallResult &&
              selected->parent == parent && selected->carriers.empty() &&
              !selected->escapes && selected->producedBy != 0 &&
              loanIsActive(scopes, selected->id) &&
              (desired->access == AccessMode::ReadOnly ||
               selected->access == AccessMode::Mutable)) {
            recordLoanIdentity(selected->id, *desired, binding);
            return {.kind = MirOperandKind::Loan,
                    .place = projected->id,
                    .loan = selected->id,
                    .type = value->info.type};
          }
        }

      // Prefer the expression's concrete MIR place when it carries the exact
      // semantic loan identity. In particular, a dynamic fixed-array element
      // records its index as a MIR value while the semantic PlaceKey records
      // only the stable selection identity. Reconstructing the place solely
      // from the semantic loan would lose that runtime index before backend
      // emission.
      const MirPlaceId sourcePlace = desiredSourcePlace();
      if (sourcePlace == 0) {
        return {};
      }
      const MirLoanId loan =
          createLoan(MirLoanKind::Local, sourcePlace, desired->access, valueId,
                     binding, 0, parent);
      recordLoanIdentity(loan, *desired, binding);
      MirOperand sourceOperand;
      if (parent != 0) {
        sourceOperand = {.kind = MirOperandKind::Loan,
                         .loan = parent,
                         .type = value->info.type};
      } else {
        sourceOperand = {.kind = desired->access == AccessMode::Mutable
                                     ? MirOperandKind::BorrowWrite
                                     : MirOperandKind::BorrowRead,
                         .place = sourcePlace,
                         .type = value->info.type};
      }
      (void)appendInstruction({.kind = MirInstructionKind::Borrow,
                               .hirValue = valueId,
                               .operands = {sourceOperand},
                               .loan = loan,
                               .definedFailure = localDefinedFailure(value),
                               .info = value->info});
      if (!scopes.empty()) {
        scopes.back().loans.push_back(loan);
      }
      valueLoans.insert_or_assign(valueId, loan);
      return {
          .kind = MirOperandKind::Loan, .loan = loan, .type = value->info.type};
    }

    const bool shellReference =
        value->info.category == ValueCategory::Place &&
        (value->info.type.kind == SemanticType::Class ||
         value->info.type.kind == SemanticType::Lambda ||
         value->info.type.kind == SemanticType::StringView ||
         value->info.type.kind == SemanticType::CString) &&
        value->info.traits.containsBorrowedState;
    if (existing != 0 && !(shellReference && output.loans[existing - 1].kind ==
                                                 MirLoanKind::Stored)) {
      std::vector<HirBindingId> &carriers = output.loans[existing - 1].carriers;
      if (binding != 0 && std::find(carriers.begin(), carriers.end(),
                                    binding) == carriers.end()) {
        carriers.push_back(binding);
      }
      return {.kind = MirOperandKind::Loan,
              .loan = existing,
              .type = value->info.type};
    }
    const MirPlaceId sourcePlace = placeForValue(valueId);
    // A mutable place can supply a shared reference. The selected reference
    // contract, not the maximum access of the source expression, determines
    // the capability published by a reference return.
    const AccessMode access = requestedAccess.value_or(value->info.access);
    const MirLoanId loan =
        createLoan(MirLoanKind::Local, sourcePlace, access, valueId, binding);
    if (shellReference) {
      output.loans[loan - 1].storedDependency =
          storedDependencyForPlace(sourcePlace);
      if (output.loans[loan - 1].storedDependency == 0 &&
          !receiverTextFieldPlace(sourcePlace))
        valid = false;
    }
    (void)appendInstruction(
        {.kind = MirInstructionKind::Borrow,
         .hirValue = valueId,
         .operands = {{.kind = access == AccessMode::Mutable
                                   ? MirOperandKind::BorrowWrite
                                   : MirOperandKind::BorrowRead,
                       .place = sourcePlace,
                       .type = value->info.type}},
         .loan = loan,
         .definedFailure = localDefinedFailure(value),
         .info = value->info});
    if (!scopes.empty()) {
      scopes.back().loans.push_back(loan);
    }
    return {
        .kind = MirOperandKind::Loan, .loan = loan, .type = value->info.type};
  }

  MirLoanId markStoredCarrierBorrow(HirValueId valueId, SymbolId field,
                                    AccessMode access) {
    const MirLoanId sourceLoan = loanForValue(valueId);
    const MirLoan *source = output.findLoan(sourceLoan);
    const HirValue *value = findValue(valueId);
    const bool constructed =
        source != nullptr && value != nullptr && !source->entry &&
        !source->escapes && source->producedBy == valueId &&
        (value->constructorTarget.has_value() ||
         (value->kind == HirValueKind::PayloadConstruction &&
          value->borrowDependencies.has_value()));
    if (source == nullptr || source->kind != MirLoanKind::Stored ||
        (!constructed &&
         (!source->entry || source->designatedFields.empty())) ||
        source->access != access || value == nullptr ||
        !value->info.traits.containsBorrowedState) {
      valid = false;
      return 0;
    }
    const auto dependencies = source->additionalDependencies;
    const auto designatedFields = source->designatedFields;
    const auto conditions = source->conditions;
    const MirLoanId published = createLoan(MirLoanKind::Stored, source->source,
                                           access, valueId, 0, field);
    output.loans[published - 1].additionalDependencies = dependencies;
    if (!conditions.empty()) {
      output.loans[published - 1].designatedFields = designatedFields;
      output.loans[published - 1].conditions = conditions;
    }
    (void)appendInstruction({.kind = MirInstructionKind::Borrow,
                             .hirValue = valueId,
                             .operands = {{.kind = MirOperandKind::Loan,
                                           .value = mirValueFor(valueId),
                                           .loan = sourceLoan,
                                           .type = value->info.type}},
                             .loan = published,
                             .info = value->info});
    (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                             .hirValue = valueId,
                             .loan = sourceLoan});
    removeLoanFromState(sourceLoan, scopes, bindingLoans);
    output.loans[published - 1].escapes = true;
    constructorPublishedLoans.push_back(published);
    return sourceLoan;
  }

  void markStoredBorrow(HirValueId valueId, SymbolId field, AccessMode access) {
    if (const MirLoanId sourceLoan = loanForValue(valueId); sourceLoan != 0) {
      MirLoan &source = output.loans[sourceLoan - 1];
      if (access == AccessMode::Mutable && source.entry &&
          source.kind == MirLoanKind::Parameter &&
          source.access == AccessMode::Mutable) {
        // A mutable reference parameter is already the unique capability for
        // its referent. Storing it in the constructed value transfers that
        // capability; creating a sibling BorrowWrite would incorrectly model
        // two simultaneous exclusive loans over the same place.
        source.kind = MirLoanKind::Stored;
        source.storedField = field;
        source.escapes = true;
        constructorPublishedLoans.push_back(sourceLoan);
        if (source.storedDependency != 0) {
          const auto dependency = source.storedDependency;
          constructorPublishedLoans.push_back(dependency);
        }
        return;
      }
      const MirPlaceId sourcePlace = output.loans[sourceLoan - 1].source;
      // Shared shell references may form a distinct stored loan, but their
      // caller-owned referent dependency still has to survive publication.
      // Capture the identity before createLoan can reallocate output.loans.
      const MirLoanId storedDependency = source.storedDependency;
      const bool transformedReference = source.kind == MirLoanKind::CallResult;
      const MirLoanId stored = createLoan(MirLoanKind::Stored, sourcePlace,
                                          access, valueId, 0, field);
      // This publishes a reference field, not ownership of the referent.
      // Cleanup-bound retention belongs to a constructed owning carrier and
      // must not be inferred from the referenced value's class.
      output.loans[stored - 1].lifecycleBound = false;
      output.loans[stored - 1].storedDependency = storedDependency;
      (void)appendInstruction(
          {.kind = MirInstructionKind::Borrow,
           .hirValue = valueId,
           .operands = {{.kind = transformedReference ? MirOperandKind::Loan
                                 : access == AccessMode::Mutable
                                     ? MirOperandKind::BorrowWrite
                                     : MirOperandKind::BorrowRead,
                         .place = transformedReference ? 0 : sourcePlace,
                         .loan = transformedReference ? sourceLoan : 0,
                         .type = findValue(valueId) == nullptr
                                     ? SemanticType::Unknown
                                     : findValue(valueId)->info.type}},
           .loan = stored,
           .definedFailure = localDefinedFailure(findValue(valueId)),
           .info = findValue(valueId) == nullptr ? ExpressionInfo{}
                                                 : findValue(valueId)->info});
      output.loans[stored - 1].escapes = true;
      constructorPublishedLoans.push_back(stored);
      if (transformedReference) {
        (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                 .hirValue = valueId,
                                 .loan = sourceLoan});
        removeLoanFromState(sourceLoan, scopes, bindingLoans);
      }
      if (storedDependency != 0)
        constructorPublishedLoans.push_back(storedDependency);
      return;
    }
    MirOperand borrow = referenceOperand(valueId);
    if (borrow.loan == 0 || borrow.loan > output.loans.size()) {
      valid = false;
      return;
    }
    MirLoan &loan = output.loans[borrow.loan - 1];
    loan.kind = MirLoanKind::Stored;
    loan.access = access;
    loan.storedField = field;
    loan.escapes = true;
    constructorPublishedLoans.push_back(loan.id);
    if (loan.storedDependency != 0)
      constructorPublishedLoans.push_back(loan.storedDependency);
  }

  void registerDrop(HirBindingId bindingId, MirPlaceId place) {
    if (scopes.empty() || nonLexicalBindings.contains(bindingId)) {
      return;
    }
    const HirBinding *binding = findBinding(bindingId);
    if (binding && !binding->info.threadLocal && !binding->info.staticStorage)
      registerSourceLocal(bindingId);
    if (binding != nullptr &&
        binding->info.type.kind != SemanticType::Reference &&
        binding->info.traits.drop == DropKind::Lexical &&
        !binding->info.threadLocal && !binding->info.staticStorage) {
      if (dropObligationForBinding(bindingId) == 0) {
        valid = false;
        return;
      }
      scopes.back().drops.push_back(place);
    }
  }

  [[nodiscard]] bool bodylessDefinition() const {
    const bool bodylessFunction =
        function != nullptr &&
        (function->source == nullptr || function->source->runtimeBinding() ||
         function->source->body() == nullptr);
    const bool bodylessConstructor = constructor != nullptr &&
                                     !constructor->generatedDefault &&
                                     (constructor->source == nullptr ||
                                      constructor->source->body() == nullptr);
    return bodylessFunction || bodylessConstructor;
  }

  void seedBodylessParameterPlaces() {
    const std::vector<HirBindingId> *parameters =
        function != nullptr      ? &function->parameterBindings
        : constructor != nullptr ? &constructor->parameterBindings
                                 : nullptr;
    if (parameters == nullptr) {
      valid = false;
      return;
    }
    for (const HirBindingId parameter : *parameters) {
      if (parameter == 0 || placeForBinding(parameter) == 0) {
        valid = false;
      }
    }
  }

  void seedParameterDrops() {
    for (const HirBinding &binding : source.bindings) {
      if (binding.parameter == nullptr) {
        continue;
      }
      if (binding.info.type.kind == SemanticType::TypePack &&
          binding.info.type.concretePack) {
        registerSourceLocal(binding.id);
        (void)placeForBinding(binding.id);
        for (const HirDropObligation &drop : source.dropObligations) {
          if (drop.kind != HirDropObligationKind::Binding ||
              drop.binding != binding.id || !drop.packElement)
            continue;
          const MirDropObligationId id = ensureDropObligation(drop);
          const MirDropObligation *lowered = output.findDropObligation(id);
          if (lowered != nullptr)
            scopes.back().drops.push_back(lowered->place);
        }
        continue;
      }
      registerDrop(binding.id, placeForBinding(binding.id));
    }
  }

  void seedEntryLoans() {
    if (scopes.empty()) {
      return;
    }
    for (const HirLoan &loan : source.loans) {
      if (!loan.entry) {
        continue;
      }
      const MirPlaceId sourcePlace = placeForLoan(loan);
      if (sourcePlace == 0) {
        continue;
      }
      const MirPlace *entryPlace = output.findPlace(sourcePlace);
      const bool textReference =
          entryPlace && entryPlace->root == MirPlaceRootKind::Binding &&
          entryPlace->projections.empty() &&
          entryPlace->type.kind == SemanticType::Reference &&
          entryPlace->type.arguments.size() == 1 &&
          (entryPlace->type.arguments.front().kind ==
               SemanticType::StringView ||
           entryPlace->type.arguments.front().kind == SemanticType::CString);
      const bool abstractStored =
          loan.kind == SemanticLoanKind::StoredValue &&
          (!loan.designatedFields.empty() || textReference);
      const bool capturedEntry = lambdaCaptures.contains(loan.place.root);
      if (loan.semanticLoan == 0 || loan.parent != 0 ||
          (loan.carriers.empty() && !abstractStored && !capturedEntry)) {
        valid = false;
        continue;
      }
      const MirLoanKind kind = loan.kind == SemanticLoanKind::StoredValue
                                   ? MirLoanKind::Stored
                                   : MirLoanKind::Parameter;
      const MirLoanId lowered = createLoan(kind, sourcePlace, loan.access);
      output.loans[lowered - 1].entry = true;
      recordLoanIdentity(lowered, loan);
      if (kind == MirLoanKind::Stored &&
          (!loan.conditions.empty() ||
           std::any_of(loan.additionalDependencies.begin(),
                       loan.additionalDependencies.end(), [](const auto &item) {
                         return !item.conditions.empty();
                       }))) {
        // Materialize every exact abstract external path, including later
        // fields, before a selected scalar loan needs it. The guarded path
        // includes explicit reference dereferences; these are not aliases of
        // the parameter's physical enum shell.
        if (projectBorrowFields(sourcePlace, loan.designatedFields,
                                loan.conditions) == 0)
          valid = false;
        for (const auto &dependency : loan.additionalDependencies)
          if (projectBorrowFields(sourcePlace, dependency.designatedFields,
                                  dependency.conditions) == 0)
            valid = false;
      }
      const MirPlace *place = output.findPlace(sourcePlace);
      if (kind == MirLoanKind::Stored && place &&
          place->root == MirPlaceRootKind::Binding &&
          place->projections.empty() && place->initiallyAvailable &&
          (place->type.kind == SemanticType::Class ||
           place->type.kind == SemanticType::Lambda ||
           place->type.kind == SemanticType::Enum)) {
        // Semantic carrier membership describes the completed flow and may
        // name only a later local. At entry the formal is the sole physical
        // carrier; lowering each subsequent transfer records its recipient.
        // Do not preseed a future local with the formal's retired authority.
        output.loans[lowered - 1].carriers = {place->binding};
      }
      if (place && place->projections.size() == 1 &&
          place->projections.front().kind == MirProjectionKind::PackElement) {
        packEntryLoans.emplace(sourcePlace, lowered);
      } else {
        for (const HirBindingId carrier : output.loans[lowered - 1].carriers) {
          bindingLoans.insert_or_assign(carrier, lowered);
        }
      }
      scopes.front().loans.push_back(lowered);
    }
  }

  void seedReturnBorrow() {
    if (function != nullptr &&
        (function->source == nullptr || function->source->body() == nullptr)) {
      return;
    }
    const std::optional<mir_detail::MirReturnOriginFormal> summarized =
        returnBorrowOriginFormal();
    if (!summarized) {
      return;
    }
    const HirBindingId binding = summarized->binding;
    if (bindingLoans.contains(binding)) {
      return;
    }
    MirPlaceId sourcePlace = placeForBinding(binding);
    const MirPlace *root =
        sourcePlace != 0 ? output.findPlace(sourcePlace) : nullptr;
    if (root != nullptr && root->type.kind == SemanticType::TypePack) {
      // The contract names one element of a concrete pack: seed the entry
      // loan on that element's place, as every other element access does.
      const std::size_t element = summarized->elementWithinBinding;
      if (!root->type.concretePack || !root->key ||
          element >= root->type.arguments.size()) {
        return;
      }
      MirPlace projected = *root;
      projected.id = 0;
      projected.projections.push_back(
          {.kind = MirProjectionKind::PackElement, .constantIndex = element});
      projected.key->projections.push_back(
          {.kind = PlaceProjectionKind::ConstantIndex, .index = element});
      projected.type = root->type.arguments[element];
      projected.traits = resolvedTraits(projected.type);
      sourcePlace = appendPackElementPlace(std::move(projected));
    }
    if (sourcePlace == 0 || scopes.empty()) {
      return;
    }
    const MirLoanId loan = createLoan(MirLoanKind::Parameter, sourcePlace,
                                      bodyReturnBorrowAccess(), 0, binding);
    output.loans[loan - 1].entry = true;
    bindingLoans.insert_or_assign(binding, loan);
    scopes.front().loans.push_back(loan);
  }

  // Scope exit.
  //
  // Leaving scopes ends the loans they still hold and drops their owners in
  // reverse construction order, innermost scope first. walkScopeExit is the
  // one definition of that sequence. An ordinary exit emits it as it is
  // walked. A shared exit (emitSharedExit) first walks it over a copy of the
  // scope state to plan its join points, then walks it again to emit, so the
  // plan cannot diverge from the emitted steps.
  struct ScopeExitState {
    std::vector<Scope> &scopes;
    std::unordered_map<HirBindingId, MirLoanId> *bindingLoans = nullptr;
    std::vector<MirDropObligationId> consumed;
    std::vector<MirLoanId> ended;
  };

  // The walk is a sequence of units: the loan ends a scope performs before
  // its drops, then each drop with the loan ends that follow it. The visitor
  // sees beginUnit before each unit (returning false stops the walk), endLoan
  // for every loan end, drop for the drop itself, dropped once its trailing
  // loan ends are in place, endUnit after the unit, and scopeEnd after a
  // scope's last unit.
  template <typename Visitor>
  void walkScopeExit(ScopeExitState &state, std::size_t keepScopes,
                     Visitor &visitor) {
    for (std::size_t depth = state.scopes.size(); depth > keepScopes; --depth) {
      // Loans ended by a scope's walk leave every scope's inventory once that
      // scope is complete, so the scope is walked from a copy.
      const Scope scope = state.scopes[depth - 1];
      if (!walkScope(state, scope, visitor)) {
        return;
      }
    }
  }

  template <typename Visitor>
  bool walkScope(ScopeExitState &state, const Scope &scope, Visitor &visitor) {
    const auto endLoans = [&](const std::vector<MirLoanId> &loans) {
      for (const MirLoanId loan : loans) {
        visitor.endLoan(loan);
        state.ended.push_back(loan);
      }
    };
    if (const std::vector<MirLoanId> loans =
            scopeLoansToEnd(scope, state.ended, false, &state.scopes);
        !loans.empty()) {
      if (!visitor.beginUnit(state, false)) {
        return false;
      }
      endLoans(loans);
      visitor.endUnit();
    }
    for (auto drop = scope.drops.rbegin(); drop != scope.drops.rend(); ++drop) {
      const MirPlace *place = output.findPlace(*drop);
      const MirDropObligationId obligation = dropObligationForLocalPlace(place);
      if (!visitor.beginUnit(state, true)) {
        return false;
      }
      if (obligation != 0 && !containsDrop(state.consumed, obligation)) {
        state.consumed.push_back(obligation);
      }
      const MirLoanId lifecycleLoan =
          place != nullptr ? lifecycleLoanForPlace(*place, state.scopes) : 0;
      visitor.drop(*drop, place, obligation, lifecycleLoan, state);
      if (lifecycleLoan != 0 && !containsLoan(state.ended, lifecycleLoan)) {
        endLoans({lifecycleLoan});
      }
      endLoans(scopeLoansToEnd(scope, state.ended, false, &state.scopes));
      visitor.dropped(obligation);
      visitor.endUnit();
    }
    visitor.scopeEnd();
    // A local may own an entry loan originally registered in an outer scope.
    // Its cleanup ends that loan on this path; popping only the local scope
    // would leave a stale outer entry and emit another EndBorrow at the join.
    for (const MirLoanId loan : state.ended) {
      for (Scope &remaining : state.scopes) {
        std::erase(remaining.loans, loan);
      }
      if (state.bindingLoans != nullptr) {
        std::erase_if(*state.bindingLoans,
                      [&](const auto &entry) { return entry.second == loan; });
      }
    }
    return true;
  }

  [[nodiscard]] static ExpressionInfo scopeDropInfo(const MirPlace *place) {
    return place == nullptr ? ExpressionInfo{}
                            : ExpressionInfo{.type = place->type,
                                             .category = ValueCategory::Place,
                                             .access = place->access,
                                             .traits = place->traits};
  }

  [[nodiscard]] static std::vector<MirLifecycleEvent>
  scopeDropLifecycle(MirDropObligationId obligation) {
    return obligation == 0 ? std::vector<MirLifecycleEvent>{}
                           : std::vector<MirLifecycleEvent>{
                                 {.kind = MirLifecycleEventKind::Drop,
                                  .source = obligation}};
  }

  void appendNormalCleanupBoundary(std::vector<MirDropObligationId> drops) {
    const std::size_t id = output.cleanupBoundaries.size() + 1;
    output.cleanupBoundaries.push_back(
        {.id = id, .obligations = std::move(drops)});
    (void)appendInstruction(
        {.kind = MirInstructionKind::Lifecycle, .cleanupBoundaryEnd = id});
  }

  // Emits the walk as it goes, one cleanup boundary per scope.
  struct GroupedScopeExitVisitor {
    MirBodyLowerer &self;
    std::vector<MirDropObligationId> group;

    bool beginUnit(const ScopeExitState &, bool) { return true; }
    void endLoan(MirLoanId loan) {
      (void)self.appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = loan});
    }
    void drop(MirPlaceId destination, const MirPlace *place,
              MirDropObligationId obligation, MirLoanId lifecycleLoan,
              const ScopeExitState &state) {
      (void)self.appendNormalDrop(obligation, destination, scopeDropInfo(place),
                                  scopeDropLifecycle(obligation),
                                  state.consumed, state.ended, 0, 0,
                                  lifecycleLoan);
    }
    void dropped(MirDropObligationId obligation) {
      if (obligation != 0) {
        group.push_back(obligation);
      }
    }
    void endUnit() {}
    void scopeEnd() {
      if (!group.empty()) {
        self.appendNormalCleanupBoundary(std::move(group));
        group.clear();
      }
    }
  };

  void emitScopeExit(std::size_t keepScopes) {
    recordSourceLocalEnds(keepScopes);
    ScopeExitState state{.scopes = scopes, .bindingLoans = &bindingLoans};
    GroupedScopeExitVisitor visitor{.self = *this};
    walkScopeExit(state, keepScopes, visitor);
  }

  // Shared normal-exit cleanup.
  //
  // Each normal exit of a function or lambda body destroys the owners live at
  // it, so a body with an exit after each of its owners has exit code growing
  // with exits times owners. Exits of the same body share that work. The
  // cleanup an exit still has to do is a function of its state: the drops not
  // yet performed and the loans not yet ended, per scope, plus the kind of
  // value it returns. Every step of a shared exit begins a block registered
  // under that state. A later exit that reaches a registered state jumps to
  // its block and stops. Exits of one body therefore form ladders whose rungs
  // are shared by every exit that passes them, and exit code grows with
  // distinct exit states instead.
  //
  // Returned values travel in one Temporary carrier per value type: an exit
  // stores its value before its cleanup, and the ladder's tail loads it and
  // returns it. Only values of block-parameter type (scalars,
  // enums, pointers) are carried; owning, class and borrowed results, exits
  // with a pending owning result or return loan, and exits with live
  // temporaries keep their own cleanup. So do bodies with a single exit.
  //
  // The lifecycle dataflow joins obligation states at a rung: exits sharing a
  // rung have the same drops left, so every obligation the rung drops is
  // active (or a moved-from shell) on each entering path and every obligation
  // already dropped is inactive on each. Loan flow requires equal loan state
  // on every edge into a block; exits sharing a rung have the same loans
  // left. A drop is shared only when its failure propagation is its
  // destructor's own summary: a drop proved to retire a moved-from shell
  // depends on the path back to the move, so it and every step before it stay
  // private to the exit. Each rung closes its own cleanup boundary, and a
  // fallible drop's Invoke continuation holds that boundary before the jump
  // to the next rung, so the verifier's boundary walk never crosses a join.
  // A fallible drop's failure edge is planned from the state at its rung,
  // which every exit entering the rung shares.
  struct SharedExitKey {
    MirPlaceId carrier = 0;
    // Per scope with work left, outermost first: the places still to drop
    // and the loans still to end.
    std::vector<std::pair<std::vector<MirPlaceId>, std::vector<MirLoanId>>>
        scopes;

    friend bool operator==(const SharedExitKey &left,
                           const SharedExitKey &right) {
      return left.carrier == right.carrier && left.scopes == right.scopes;
    }
    friend bool operator<(const SharedExitKey &left,
                          const SharedExitKey &right) {
      return std::tie(left.carrier, left.scopes) <
             std::tie(right.carrier, right.scopes);
    }
  };

  struct SharedExitCarrier {
    bool hasValue = false;
    SemanticType type = SemanticType::Unknown;
    SemanticTypeTraits traits{};
    MirOperand stored;
    // The HIR value the exit returns: the root of its full expression, which
    // the carrier's store realizes on this path.
    HirValueId source = 0;
    ExpressionInfo valueInfo;
  };

  struct SharedExitUnit {
    SharedExitKey key;
    bool drop = false;
    MirDropObligationId obligation = 0;
    FailurePropagationKind propagation = FailurePropagationKind::None;
    bool shareable = true;
  };

  struct SharedExitPlan {
    std::vector<SharedExitUnit> units;
    // Units before this index stay private to the exit.
    std::size_t firstShared = 0;
  };

  [[nodiscard]] SharedExitKey sharedExitKey(MirPlaceId carrier,
                                            const ScopeExitState &state) {
    SharedExitKey key{.carrier = carrier};
    for (const Scope &scope : state.scopes) {
      std::vector<MirPlaceId> drops;
      for (const MirPlaceId place : scope.drops) {
        if (!containsDrop(state.consumed,
                          dropObligationForLocalPlace(output.findPlace(place))))
          drops.push_back(place);
      }
      std::vector<MirLoanId> loans;
      for (const MirLoanId loan : scope.loans) {
        if (!containsLoan(state.ended, loan))
          loans.push_back(loan);
      }
      if (!drops.empty() || !loans.empty()) {
        key.scopes.emplace_back(std::move(drops), std::move(loans));
      }
    }
    return key;
  }

  // A drop is shared only when its propagation is its destructor's own
  // summary; see the moved-from shell rule above.
  [[nodiscard]] bool exitDropShareable(const MirPlace *place,
                                       MirDropObligationId obligation) const {
    return place != nullptr && obligation != 0 &&
           normalizedDestructorPropagation(obligation) ==
               (destructorMayRaise(obligation)
                    ? FailurePropagationKind::Destructor
                    : FailurePropagationKind::None);
  }

  // Whether an exit from the current scopes shares at least one drop: the
  // walk's last drop, the oldest owner, must be shareable, since every unit
  // after the last private one is shared.
  [[nodiscard]] bool sharedExitHasShareableDrop() {
    bool any = false;
    bool shareable = false;
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      for (auto drop = scope->drops.rbegin(); drop != scope->drops.rend();
           ++drop) {
        const MirPlace *place = output.findPlace(*drop);
        const MirDropObligationId obligation =
            dropObligationForLocalPlace(place);
        if (place == nullptr || obligation == 0) {
          return false;
        }
        any = true;
        shareable = exitDropShareable(place, obligation);
      }
    }
    return any && shareable;
  }

  // The state-independent conditions for sharing an exit's cleanup;
  // `temporariesAtExit` are the temporaries still live when cleanup begins.
  [[nodiscard]] bool
  sharedExitEligible(const std::vector<TemporaryDrop> &temporariesAtExit) {
    return shareExitCleanupEnabled && exitCount >= 2 &&
           (output.kind == MirBodyKind::Function ||
            output.kind == MirBodyKind::Lambda) &&
           temporariesAtExit.empty() && pendingReturnDrop == 0 &&
           pendingReturnLoan == 0 && constructorPublishedLoans.empty() &&
           constructorRollback.empty() && sharedExitHasShareableDrop();
  }

  [[nodiscard]] static ExpressionInfo loadedValueInfo(const HirValue &value) {
    ExpressionInfo info = value.info;
    info.category = ValueCategory::Value;
    info.access = AccessMode::ReadOnly;
    return info;
  }

  // The carrier for returning `value` from a shared exit, when it is a
  // scalar value of the return type.
  [[nodiscard]] std::optional<SharedExitCarrier>
  sharedValueCarrier(const HirValue &value) const {
    // A substituted program constant has exactly one MIR materialization,
    // so the tail's reload cannot name it: such an exit keeps its own cleanup.
    if (value.dropObligation || value.neverReturns ||
        value.programConstantSubstitution) {
      return std::nullopt;
    }
    if (!isMirBlockParameterType(value.info.type) ||
        value.info.type != output.returnType) {
      return std::nullopt;
    }
    return SharedExitCarrier{.hasValue = true,
                             .type = value.info.type,
                             .traits = value.info.traits,
                             .source = value.id,
                             .valueInfo = loadedValueInfo(value)};
  }

  // The carrier of a return statement when its exit may share cleanup, or
  // nullopt when the return keeps its own cleanup. Decided before the
  // operand is emitted.
  [[nodiscard]] std::optional<SharedExitCarrier>
  sharedReturnCarrier(const HirStatement &statement,
                      const std::vector<TemporaryDrop> &temporariesAtExit) {
    std::optional<SharedExitCarrier> carrier;
    if (!statement.value) {
      if (output.returnType.kind != SemanticType::Void) {
        return std::nullopt;
      }
      carrier = SharedExitCarrier{};
    } else {
      const HirValue *value = findValue(*statement.value);
      if (value == nullptr) {
        return std::nullopt;
      }
      if (output.returnType.kind == SemanticType::Void) {
        if (value->info.type != SemanticType::Void) {
          return std::nullopt;
        }
        carrier = SharedExitCarrier{};
      } else if (output.returnType.kind == SemanticType::Reference) {
        return std::nullopt;
      } else {
        carrier = sharedValueCarrier(*value);
      }
    }
    if (!carrier || !sharedExitEligible(temporariesAtExit)) {
      return std::nullopt;
    }
    return carrier;
  }

  [[nodiscard]] MirPlaceId exitCarrierPlace(const SharedExitCarrier &carrier) {
    for (const auto &[type, place] : exitCarriers) {
      if (type == carrier.type) {
        return place;
      }
    }
    const MirPlaceId place = appendPlace({.root = MirPlaceRootKind::Temporary,
                                          .temporary = nextTemporary++,
                                          .type = carrier.type,
                                          .access = AccessMode::Mutable,
                                          .traits = carrier.traits,
                                          .sourceValue = carrier.source});
    exitCarriers.emplace_back(carrier.type, place);
    return place;
  }

  // Records the walk's units with the state key before each one.
  struct SharedExitPlanner {
    MirBodyLowerer &self;
    MirPlaceId carrier = 0;
    SharedExitPlan plan;

    bool beginUnit(const ScopeExitState &state, bool drop) {
      plan.units.push_back(
          {.key = self.sharedExitKey(carrier, state), .drop = drop});
      return true;
    }
    void endLoan(MirLoanId) {}
    void drop(MirPlaceId, const MirPlace *place, MirDropObligationId obligation,
              MirLoanId, const ScopeExitState &) {
      SharedExitUnit &unit = plan.units.back();
      unit.obligation = obligation;
      unit.propagation = self.normalizedDestructorPropagation(obligation);
      unit.shareable = self.exitDropShareable(place, obligation);
    }
    void dropped(MirDropObligationId) {}
    void endUnit() {}
    void scopeEnd() {}
  };

  // Emits the walk against its plan: private units in place, each shared
  // unit in a block registered under its state, or a jump to the block an
  // earlier exit registered for that state.
  struct SharedExitEmitter {
    MirBodyLowerer &self;
    const SharedExitPlan &plan;
    MirPlaceId carrier = 0;
    std::size_t index = 0;
    bool joined = false;
    bool consistent = true;

    bool beginUnit(const ScopeExitState &state, bool drop) {
      if (index >= plan.units.size() || plan.units[index].drop != drop ||
          self.terminated()) {
        consistent = false;
        return false;
      }
      if (index < plan.firstShared) {
        return true;
      }
      const SharedExitUnit &unit = plan.units[index];
      if (self.sharedExitKey(carrier, state) != unit.key) {
        consistent = false;
        return false;
      }
      if (const auto found = self.sharedExitBlocks.find(unit.key);
          found != self.sharedExitBlocks.end()) {
        self.terminate(
            {.kind = MirTerminatorKind::Goto, .target = found->second});
        joined = true;
        return false;
      }
      const MirBlockId block = self.appendBlock();
      self.terminate({.kind = MirTerminatorKind::Goto, .target = block});
      self.current = block;
      self.sharedExitBlocks.emplace(unit.key, block);
      return true;
    }
    void endLoan(MirLoanId loan) {
      (void)self.appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = loan});
    }
    void drop(MirPlaceId destination, const MirPlace *place,
              MirDropObligationId obligation, MirLoanId lifecycleLoan,
              const ScopeExitState &state) {
      const SharedExitUnit &unit = plan.units[index];
      if (unit.obligation != obligation) {
        consistent = false;
      }
      (void)self.appendNormalDrop(obligation, destination, scopeDropInfo(place),
                                  scopeDropLifecycle(obligation),
                                  state.consumed, state.ended, 0, 0,
                                  lifecycleLoan, {}, unit.propagation);
    }
    void dropped(MirDropObligationId obligation) {
      if (obligation != 0) {
        self.appendNormalCleanupBoundary({obligation});
      }
    }
    void endUnit() { ++index; }
    void scopeEnd() {}
  };

  // Stores a shared exit's value into its carrier, before the exit's
  // full-expression cleanup: the store realizes the returned expression on
  // this path, so it precedes the expression's end marker as the verifier
  // requires of every root. Returns the carrier, or zero for no value.
  [[nodiscard]] MirPlaceId
  storeSharedExitValue(const SharedExitCarrier &carrier) {
    if (!carrier.hasValue) {
      return 0;
    }
    const MirPlaceId place = exitCarrierPlace(carrier);
    (void)appendInstruction(
        {.kind = MirInstructionKind::Initialize,
         .hirValue = carrier.source,
         .destination = place,
         .operands = {carrier.stored},
         .info = ExpressionInfo{.type = carrier.type,
                                .category = ValueCategory::Place,
                                .access = AccessMode::Mutable,
                                .traits = carrier.traits}},
        false, false);
    return place;
  }

  // Lowers the current exit's cleanup and return through the body's shared
  // exit ladders, once its value is stored in `place`. Returns false, having
  // emitted nothing, when the exit shares no drop.
  //
  // The tail belongs to every exit that reaches it, so its reload and return
  // name no source location, like the shared
  // failure cleanup: debug information maps them to generated code rather
  // than to whichever exit happened to emit them first. Each exit's own
  // carrier store keeps its source location.
  bool emitSharedExit(const SharedExitCarrier &carrier, MirPlaceId place) {
    std::vector<Scope> simulated = scopes;
    ScopeExitState planning{.scopes = simulated};
    SharedExitPlanner planner{.self = *this, .carrier = place};
    walkScopeExit(planning, 0, planner);
    SharedExitPlan plan = std::move(planner.plan);
    for (std::size_t index = 0; index < plan.units.size(); ++index) {
      if (!plan.units[index].shareable) {
        plan.firstShared = index + 1;
      }
    }
    if (std::none_of(plan.units.begin() +
                         static_cast<std::ptrdiff_t>(plan.firstShared),
                     plan.units.end(),
                     [](const SharedExitUnit &unit) { return unit.drop; })) {
      return false;
    }
    exitSharingUsed = true;
    recordSourceLocalEnds(0);
    ScopeExitState state{.scopes = scopes, .bindingLoans = &bindingLoans};
    SharedExitEmitter emitter{.self = *this, .plan = plan, .carrier = place};
    walkScopeExit(state, 0, emitter);
    if (!emitter.consistent ||
        (!emitter.joined && emitter.index != plan.units.size())) {
      valid = false;
      return true;
    }
    if (emitter.joined) {
      exitsJoined = true;
      return true;
    }
    std::optional<MirOperand> result;
    if (carrier.hasValue) {
      const MirValueId loaded = appendValue(carrier.source, carrier.valueInfo);
      (void)appendInstruction({.kind = MirInstructionKind::Load,
                               .result = loaded,
                               .operands = {{.kind = MirOperandKind::Copy,
                                             .place = place,
                                             .type = carrier.type}},
                               .info = carrier.valueInfo},
                              false, false);
      result = MirOperand{
          .kind = MirOperandKind::Value, .value = loaded, .type = carrier.type};
    }
    terminate({.kind = MirTerminatorKind::Return, .value = std::move(result)});
    return true;
  }

  void lowerProgramInitializationPlan() {
    if (output.kind != MirBodyKind::Module ||
        loweredProgramInitialization == nullptr) {
      valid = false;
      currentProgramInitializationStep = 0;
      output.entry = appendBlock();
      current = output.entry;
      return;
    }

    *loweredProgramInitialization = {};
    std::unordered_map<SourceUnitId, std::size_t> units;
    loweredProgramInitialization->units.reserve(
        programInitialization->unitOrder.size());
    for (const SourceUnitId sourceUnit : programInitialization->unitOrder) {
      const std::size_t index = loweredProgramInitialization->units.size();
      if (sourceUnit == 0 || !units.emplace(sourceUnit, index).second) {
        valid = false;
      }
      loweredProgramInitialization->units.push_back({.sourceUnit = sourceUnit});
    }

    std::vector<HirStatementId> expectedRoots;
    std::unordered_set<HirBindingId> plannedBindings;
    std::unordered_set<HirBindingId> initializerLocalBindings;
    std::vector<std::vector<HirValueId>> initializerRoots;
    initializerRoots.reserve(programInitialization->steps.size());
    for (const auto &step : programInitialization->steps)
      initializerRoots.push_back(step.initializer
                                     ? std::vector{*step.initializer}
                                     : std::vector<HirValueId>{});
    const auto initializerInventories =
        hirInitializerReachabilityBatch(source, initializerRoots);
    loweredProgramInitialization->steps.reserve(
        programInitialization->steps.size());
    for (std::size_t index = 0; index < programInitialization->steps.size();
         ++index) {
      const HirProgramInitializationStep &step =
          programInitialization->steps[index];
      if (step.id != index + 1 || (step.sourceUnit == 0 && !units.empty()) ||
          step.symbol == 0 || step.binding == 0 ||
          !plannedBindings.insert(step.binding).second) {
        valid = false;
      }
      const auto unit = units.find(step.sourceUnit);
      if (unit == units.end() && !(step.sourceUnit == 0 && units.empty())) {
        valid = false;
      } else if (unit != units.end()) {
        loweredProgramInitialization->units[unit->second].steps.push_back(
            step.id);
      }

      const MirBlockId predecessor = current;
      currentProgramInitializationStep = step.id;
      const MirBlockId entryBlock = appendBlock();
      if (output.entry == 0) {
        output.entry = entryBlock;
      } else if (predecessor == 0 || terminated()) {
        valid = false;
      } else {
        current = predecessor;
        terminate({.kind = MirTerminatorKind::Goto, .target = entryBlock});
      }
      current = entryBlock;

      const HirBinding *binding = findBinding(step.binding);
      const MirPlaceId storagePlace = placeForBinding(step.binding);
      MirProgramInitializationStep lowered{
          .id = step.id,
          .sourceUnit = step.sourceUnit,
          .storageKind = step.kind,
          .role = step.role,
          .symbol = step.symbol,
          .ownerClass = step.ownerClass,
          .requiresActiveCleanup = step.requiresActiveCleanup,
          .nativeRetirement =
              step.nativeRetirement
                  ? std::optional{lowerDropType(*step.nativeRetirement)}
                  : std::nullopt,
          .binding = step.binding,
          .storagePlace = storagePlace,
          .entryBlock = entryBlock};
      if (binding == nullptr || binding->info.symbol != step.symbol ||
          storagePlace == 0 ||
          step.requiresActiveCleanup != step.nativeRetirement.has_value()) {
        valid = false;
      }

      if (step.role == ProgramInitializationStepRole::DataOnly) {
        if (step.initializer || step.statement != 0) {
          valid = false;
        }
        lowered.dataInitialization =
            binding != nullptr && binding->info.constant
                ? MirProgramDataInitializationKind::Constant
                : MirProgramDataInitializationKind::ImplicitZero;
        if (binding != nullptr && binding->info.constant) {
          lowered.dataConstant = binding->info.constant;
        }
        lowered.storageInitialization = appendInstruction(
            {.kind = MirInstructionKind::Initialize,
             .destination = storagePlace,
             .info = binding == nullptr
                         ? ExpressionInfo{}
                         : ExpressionInfo{.type = binding->info.type,
                                          .category = ValueCategory::Place,
                                          .access = binding->info.access,
                                          .traits = binding->info.traits}});
      } else if (step.role == ProgramInitializationStepRole::Initializer) {
        const HirStatement *statement = findStatement(step.statement);
        const HirValue *initializer =
            step.initializer ? findValue(*step.initializer) : nullptr;
        if (!step.initializer || statement == nullptr ||
            initializer == nullptr ||
            statement->kind != HirStatementKind::Variable ||
            statement->binding != step.binding ||
            statement->value != step.initializer ||
            initializer->fullExpression == 0) {
          valid = false;
        }
        expectedRoots.push_back(step.statement);
        lowered.statement = step.statement;
        lowered.initializer = step.initializer.value_or(0);
        const auto &reachable = initializerInventories[index];
        if (!reachable.complete)
          valid = false;
        std::vector<HirBindingId> locals(reachable.bindings.begin(),
                                         reachable.bindings.end());
        std::sort(locals.begin(), locals.end());
        lowered.storageInitialization =
            statement == nullptr ? 0 : lowerVariable(*statement);
        for (const HirBindingId id : locals) {
          const HirBinding *local = findBinding(id);
          const MirPlaceId place = placeForBinding(id);
          if (local == nullptr || place == 0 ||
              nonLexicalBindings.contains(id) ||
              !initializerLocalBindings.insert(id).second) {
            valid = false;
            continue;
          }
          lowered.locals.push_back(
              {.binding = id, .symbol = local->info.symbol, .place = place});
        }
        const auto fullExpression =
            initializer == nullptr
                ? fullExpressionIds.end()
                : fullExpressionIds.find(initializer->fullExpression);
        if (fullExpression == fullExpressionIds.end()) {
          valid = false;
        } else {
          lowered.fullExpression = fullExpression->second;
        }
        for (const HirFullExpression &expression : source.fullExpressions) {
          if (!reachable.statements.contains(expression.statement))
            continue;
          const auto nested = fullExpressionIds.find(expression.id);
          if (nested == fullExpressionIds.end() ||
              nested->second == lowered.fullExpression) {
            valid = false;
            continue;
          }
          lowered.nestedFullExpressions.push_back(nested->second);
        }
      } else {
        valid = false;
      }
      if (lowered.storageInitialization == 0) {
        valid = false;
      }
      loweredProgramInitialization->steps.push_back(std::move(lowered));
    }

    if (programInitialization->steps.empty()) {
      currentProgramInitializationStep = 0;
      output.entry = appendBlock();
      current = output.entry;
    }
    plannedBindings.insert(initializerLocalBindings.begin(),
                           initializerLocalBindings.end());
    if (source.roots != expectedRoots ||
        source.bindings.size() != plannedBindings.size() ||
        std::any_of(source.bindings.begin(), source.bindings.end(),
                    [&](const HirBinding &binding) {
                      return !plannedBindings.contains(binding.id);
                    })) {
      valid = false;
    }
  }

  void lowerStatements(const std::vector<HirStatementId> &ids) {
    for (const HirStatementId id : ids) {
      if (terminated()) {
        return;
      }
      lowerStatement(id);
    }
  }

  void lowerNestedStatement(const std::optional<HirStatementId> &id) {
    if (id) {
      lowerStatement(*id);
    }
  }

  void lowerScopedStatement(const std::optional<HirStatementId> &id) {
    scopes.push_back({});
    lowerNestedStatement(id);
    if (!terminated()) {
      emitScopeExit(scopes.size() - 1);
    }
    scopes.pop_back();
  }

  void lowerStatement(HirStatementId id) {
    const HirStatement *statement = findStatement(id);
    if (statement == nullptr) {
      valid = false;
      return;
    }
    if (statement->kind != HirStatementKind::Return &&
        statement->resultRegion != 0) {
      valid = false;
      return;
    }
    switch (statement->kind) {
    case HirStatementKind::Block:
      scopes.push_back({});
      lowerStatements(statement->statements);
      if (!terminated()) {
        emitScopeExit(scopes.size() - 1);
      }
      scopes.pop_back();
      if (!terminated())
        endSemanticLoans(*statement);
      return;
    case HirStatementKind::CompileTimeBranch:
      lowerStatements(statement->statements);
      return;
    case HirStatementKind::Expression:
      if (statement->value) {
        const std::vector<Scope> incomingScopes = scopes;
        const std::vector<TemporaryDrop> incomingTemporaryDrops =
            temporaryDrops;
        emitValue(*statement->value);
        endFullExpressionLoans(incomingScopes);
        emitTemporaryDrops(incomingTemporaryDrops, *statement->value,
                           statement->id);
      }
      endSemanticLoans(*statement);
      return;
    case HirStatementKind::DoWhile:
      lowerDoWhile(*statement);
      return;
    case HirStatementKind::Variable:
      (void)lowerVariable(*statement);
      return;
    case HirStatementKind::StructuredBinding:
      lowerStructuredBinding(*statement);
      return;
    case HirStatementKind::If:
      lowerIf(*statement);
      return;
    case HirStatementKind::While:
      lowerWhile(*statement);
      return;
    case HirStatementKind::For:
      lowerFor(*statement);
      return;
    case HirStatementKind::RangeFor:
      lowerRangeFor(*statement);
      return;
    case HirStatementKind::Switch:
      lowerSwitch(*statement);
      return;
    case HirStatementKind::Break:
      if (breakContexts.empty()) {
        valid = false;
        return;
      }
      endSemanticLoansIfActive(breakContexts.back().exitLoans, id);
      emitScopeExit(breakContexts.back().keepScopes);
      terminate({.kind = MirTerminatorKind::Goto,
                 .hirStatement = id,
                 .target = breakContexts.back().target});
      return;
    case HirStatementKind::Continue:
      if (continueContexts.empty()) {
        valid = false;
        return;
      }
      emitScopeExit(continueContexts.back().keepScopes);
      terminate({.kind = MirTerminatorKind::Goto,
                 .hirStatement = id,
                 .target = continueContexts.back().target});
      return;
    case HirStatementKind::Return:
      if (!resultRegions.empty() || statement->resultRegion != 0)
        emitRegionReturn(*statement);
      else
        lowerReturn(*statement);
      return;
    case HirStatementKind::Empty:
      return;
    }
  }

  [[nodiscard]] MirInstructionId lowerVariable(const HirStatement &statement) {
    if (!statement.binding) {
      return 0;
    }
    if (const auto *binding = findBinding(*statement.binding);
        binding != nullptr && binding->info.threadLocal) {
      const auto storage = std::find_if(
          program.threadLocalStorage().begin(),
          program.threadLocalStorage().end(), [&](const auto &candidate) {
            return candidate.symbol == binding->info.symbol;
          });
      auto failure = storage == program.threadLocalStorage().end()
                         ? DefinedFailureOperation{}
                         : storage->ensureFailure;
      failure.propagation = storage != program.threadLocalStorage().end() &&
                                    storage->dataInitializer
                                ? FailurePropagationKind::None
                                : FailurePropagationKind::BodyCall;
      return appendInstruction(
          {.kind = MirInstructionKind::EnsureThreadLocal,
           .threadLocalStorage = binding->info.symbol,
           .hirStatement = statement.id,
           .definedFailure = std::move(failure),
           .bodyTarget =
               MirBodyAddress{.kind = MirBodyKind::ThreadLocalInitializer,
                              .owner = binding->info.symbol},
           .info = {.type = SemanticType::Void}},
          false);
    }
    if (statement.value) {
      const auto *initializer = findValue(*statement.value);
      if (initializer && initializer->neverReturns) {
        emitValue(*statement.value);
        if (!terminated())
          valid = false;
        return 0;
      }
    }
    const std::vector<Scope> incomingScopes = scopes;
    const std::vector<TemporaryDrop> incomingTemporaryDrops = temporaryDrops;
    const HirBinding *binding = findBinding(*statement.binding);
    const MirPlaceId destination = placeForBinding(*statement.binding);
    std::vector<MirOperand> operands;
    MirLoanId retainedLoan = 0;
    bool physicalRangeLoan = false;
    // Only a generated structural range endpoint may temporarily park its
    // freshly produced hidden cursor. Ordinary source calls never acquire
    // this capability, even when their spelling or type resembles a range.
    for (const auto &candidate : source.statements) {
      if (!candidate.rangeIteration ||
          candidate.rangeIteration->sentinel != *statement.binding)
        continue;
      const auto &range = *candidate.rangeIteration;
      if (range.strategy != RangeIterationStrategy::Structural ||
          range.elementAccess != AccessMode::Mutable ||
          !range.nonBorrowingEndpoint || !statement.value ||
          *statement.value != range.end || parkedRangeEndpoint)
        continue;
      const auto cursorBinding = bindingLoans.find(range.iterator);
      const auto parent = semanticLoans.find(range.rangeLoan);
      const auto *cursor = cursorBinding == bindingLoans.end()
                               ? nullptr
                               : output.findLoan(cursorBinding->second);
      const auto *endpoint = findValue(range.end);
      if (!cursor || parent == semanticLoans.end() || cursor->entry ||
          cursor->escapes || cursor->kind != MirLoanKind::Stored ||
          cursor->access != AccessMode::Mutable ||
          cursor->parent != parent->second ||
          cursor->producedBy != range.begin ||
          cursor->carriers != std::vector<HirBindingId>{range.iterator} ||
          !endpoint || endpoint->info.type.kind == SemanticType::Reference ||
          endpoint->info.traits.containsBorrowedState ||
          endpoint->borrowOrigin != BorrowOriginKind::None ||
          !endpoint->functionTarget ||
          endpoint->dispatch != CallDispatch::Static || !endpoint->callPlan ||
          !endpoint->callPlan->arguments.empty() ||
          !endpoint->callPlan->receiver ||
          endpoint->callPlan->receiver->kind != HirCallInputKind::ReadBorrow)
        continue;
      MirInstructionId beginCall = 0;
      for (const auto &block : output.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.kind == MirInstructionKind::Call &&
              instruction.hirValue == range.begin &&
              instruction.loan == cursor->id)
            beginCall = instruction.id;
      if (!beginCall)
        continue;
      parkedRangeEndpoint =
          ParkedRangeEndpoint{.statement = candidate.id,
                              .parking = {.cursor = cursor->id,
                                          .endValue = range.end,
                                          .beginCall = beginCall}};
      parkedRangeEndpoint->parking.park =
          appendInstruction({.kind = MirInstructionKind::ParkRangeCursor,
                             .hirValue = range.end,
                             .hirStatement = candidate.id,
                             .loan = cursor->id},
                            false);
      break;
    }
    if (statement.value) {
      if (binding != nullptr &&
          binding->info.type.kind == SemanticType::Reference) {
        MirOperand reference =
            referenceOperand(*statement.value, *statement.binding);
        // The loan is provenance authority; the place is the exact runtime
        // address source. Borrow-carrying values can intentionally make those
        // differ, so retain both rather than asking a backend to reconstruct
        // the source expression from carrier topology.
        reference.place = placeForValue(*statement.value);
        retainedLoan = reference.loan;
        const auto *address = output.findPlace(reference.place);
        if (address != nullptr &&
            ((!address->projections.empty() &&
              address->type == reference.type) ||
             (address->root == MirPlaceRootKind::Loan &&
              ((address->loan == retainedLoan &&
                output.findLoan(retainedLoan) != nullptr &&
                output.findLoan(retainedLoan)->kind ==
                    MirLoanKind::CallResult) ||
               retainableCallResultChain(
                   address->loan, retainedLoan, *statement.value,
                   binding->info.type.referenceAccess))) ||
             (address->root == MirPlaceRootKind::Binding &&
              address->projections.size() == 1 &&
              address->projections.front().kind ==
                  MirProjectionKind::Dereference &&
              referenceAccessorBindings.contains(address->binding))))
          referenceAccessorBindings.insert(*statement.binding);
        operands.push_back(std::move(reference));
      } else {
        MirOperand initializer = valueOperand(*statement.value);
        if (parkedRangeEndpoint) {
          auto &parking = parkedRangeEndpoint->parking;
          for (const auto &block : output.blocks)
            for (const auto &instruction : block.instructions) {
              if (instruction.kind == MirInstructionKind::Call &&
                  instruction.hirValue == parking.endValue)
                parking.endCall = instruction.id;
              if (instruction.kind == MirInstructionKind::CallInput &&
                  instruction.callSite == parking.endValue &&
                  instruction.callInputRole == MirCallInputRole::Receiver)
                parking.endReceiver = instruction.id;
            }
          parking.restore =
              appendInstruction({.kind = MirInstructionKind::RestoreRangeCursor,
                                 .hirValue = parking.endValue,
                                 .hirStatement = parkedRangeEndpoint->statement,
                                 .loan = parking.cursor},
                                false);
          rangeEndpointParking[parkedRangeEndpoint->statement] = parking;
          parkedRangeEndpoint.reset();
        }
        routeDirectResultInitialization(initializer, destination);
        operands.push_back(std::move(initializer));
        if (binding != nullptr && binding->info.traits.containsBorrowedState) {
          retainedLoan = loanForValue(*statement.value);
        }
      }
    }
    if (terminated()) {
      return 0;
    }
    // A readonly owning range can give its cursor a fresh semantic Stored
    // identity rather than reusing the traversal identity. Its selected begin
    // result still borrows the exact hidden range owner. Preserve that physical
    // parent independently of the semantic-ID collision path below.
    if (retainedLoan != 0 && isRangeIteratorBinding(*statement.binding) &&
        isPhysicalProxyRangeProtocolBinding(*statement.binding)) {
      for (const auto &candidateStatement : source.statements) {
        if (!candidateStatement.rangeIteration ||
            candidateStatement.rangeIteration->iterator != *statement.binding)
          continue;
        const auto &range = *candidateStatement.rangeIteration;
        const auto selected = semanticLoans.find(range.rangeLoan);
        auto &cursor = output.loans[retainedLoan - 1];
        const auto *parent = selected == semanticLoans.end()
                                 ? nullptr
                                 : output.findLoan(selected->second);
        const auto *origin = output.findPlace(cursor.source);
        const auto *carrier =
            output.findPlace(placeForBinding(range.rangeCarrier));
        const bool exactCarrier =
            origin && carrier && origin->root == MirPlaceRootKind::Binding &&
            origin->binding == carrier->binding &&
            origin->projections.size() == 1 &&
            origin->projections.front().kind == MirProjectionKind::Dereference;
        if (parent && cursor.kind == MirLoanKind::Stored && !cursor.entry &&
            !cursor.escapes && cursor.producedBy == range.begin &&
            cursor.access == parent->access &&
            cursor.storedDependency == parent->storedDependency &&
            (sameBorrowSource(cursor.source, parent->source) || exactCarrier)) {
          if (cursor.parent == 0)
            cursor.parent = parent->id;
          if (cursor.parent != parent->id)
            valid = false;
          physicalRangeLoan = true;
          std::erase(output.loans[parent->id - 1].carriers, *statement.binding);
        }
      }
    }
    if (retainedLoan != 0) {
      if (binding != nullptr && binding->info.retainedLoan != 0) {
        const SemanticLoanId semanticLoan = binding->info.retainedLoan;
        if (const auto found = semanticLoans.find(semanticLoan);
            found != semanticLoans.end() && found->second != retainedLoan) {
          const MirLoan *existing = output.findLoan(found->second);
          MirLoan *candidate =
              retainedLoan == 0 || retainedLoan > output.loans.size()
                  ? nullptr
                  : &output.loans[retainedLoan - 1];
          const auto *initializer =
              statement.value ? findValue(*statement.value) : nullptr;
          const auto arguments = initializer ? callArgumentValues(*initializer)
                                             : std::vector<HirValueId>{};
          const bool generatedMove =
              initializer != nullptr &&
              initializer->constructorKind == ConstructorKind::Move &&
              !initializer->constructorTarget && !initializer->callPlan &&
              arguments.size() == 1 &&
              initializer->info.type == binding->info.type;
          bool consumesExistingCarrier = false;
          if (existing != nullptr && candidate != nullptr &&
              initializer != nullptr &&
              (initializer->functionTarget || initializer->constructorTarget ||
               generatedMove) &&
              existing->lifecycleBound && candidate->lifecycleBound &&
              candidate->parent == existing->parent &&
              sameBorrowSource(candidate->source, existing->source)) {
            for (std::size_t index = 0;
                 index < arguments.size() &&
                 index < initializer->parameterTypes.size();
                 ++index) {
              if (!generatedMove && initializer->parameterTypes[index].kind !=
                                        SemanticType::Class)
                continue;
              const auto moved = movedBorrowCarrierBinding(arguments[index]);
              const auto source = bindingLoans.find(moved);
              if (moved != 0 && ((source != bindingLoans.end() &&
                                  source->second == existing->id) ||
                                 std::find(existing->carriers.begin(),
                                           existing->carriers.end(),
                                           moved) != existing->carriers.end()))
                consumesExistingCarrier = true;
            }
          }
          if (existing != nullptr && candidate != nullptr &&
              existing->access == candidate->access && initializer != nullptr &&
              (candidate->producedBy == initializer->id ||
               exactReadonlyRangeElementAlias(*statement.binding, *initializer,
                                              *candidate)) &&
              (candidate->kind == MirLoanKind::Stored ||
               candidate->kind == MirLoanKind::CallResult) &&
              isPhysicalProxyRangeProtocolBinding(*statement.binding)) {
            // The semantic range loan is the traversal's owner/escape
            // boundary. It is not the storage of a concrete cursor or an
            // inline proxy returned by dereference. Keep the selected call's
            // physical loan and external dependency exactly as ordinary
            // explicit cursor code does, rather than aliasing its shell to
            // the range owner and retiring it at initialization.
            physicalRangeLoan = true;
            std::erase(output.loans[existing->id - 1].carriers,
                       *statement.binding);
            const auto *origin = output.findPlace(candidate->source);
            const bool rangeCarrierReferent =
                origin != nullptr &&
                origin->root == MirPlaceRootKind::Binding &&
                !origin->projections.empty() &&
                origin->projections.front().kind ==
                    MirProjectionKind::Dereference &&
                std::find(existing->carriers.begin(), existing->carriers.end(),
                          origin->binding) != existing->carriers.end();
            if (isRangeIteratorBinding(*statement.binding) &&
                candidate->parent == 0 &&
                (sameBorrowSource(candidate->source, existing->source) ||
                 rangeCarrierReferent))
              candidate->parent = existing->id;
          } else if (existing != nullptr && candidate != nullptr &&
                     binding->info.type.kind == SemanticType::Reference &&
                     capturedReferenceEntry(*existing) &&
                     loanIsActive(scopes, existing->id) &&
                     capturedEntryDescendant(
                         candidate->id, existing->id,
                         binding->info.type.referenceAccess)) {
            // A by-reference capture's entry loan is held by the closure
            // environment, which is not a binding, so a local reference alias
            // cannot join it as a carrier the way a formal's alias joins the
            // formal's entry. The alias instead carries its own physical loan
            // below that entry (an accessor result's, a fresh local borrow's,
            // or another such alias's), as a range cursor keeps its call's
            // loan; the semantic identity stays on the entry loan.
            physicalRangeLoan = true;
          } else if (consumesExistingCarrier) {
            // The semantic identity protects the same external owner, but a
            // consuming call publishes a new physical cleanup obligation.
            // Retire the consumed loan after the call's verified transfer;
            // keep the result's loan until its own drop/return, rather than
            // collapsing both lifetimes into the old carrier's schedule.
            const auto consumed = existing->id;
            std::erase(output.loans[consumed - 1].carriers, *statement.binding);
            output.loans[consumed - 1].semanticLoan = 0;
            // An exact field-mapped by-value call already retires the old
            // carrier on its success edge. Compact calls retire it here.
            // Both publish a new lifetime for the returned physical owner.
            if (loanIsActive(scopes, consumed)) {
              (void)appendInstruction(
                  {.kind = MirInstructionKind::EndBorrow, .loan = consumed});
              removeLoanFromState(consumed, scopes, bindingLoans);
            }
            semanticLoans.insert_or_assign(semanticLoan, retainedLoan);
          } else if (existing != nullptr && candidate != nullptr &&
                     candidate->storedDependency == existing->id &&
                     existing->kind == MirLoanKind::Stored &&
                     binding->info.type.kind != SemanticType::Reference &&
                     loanIsActive(scopes, existing->id)) {
            // A copied value retains the external owners, not the address of
            // the reference's carrier shell. Do not rewrite the physical loan
            // or compare its independent shell permission with owner access.
            retainedLoan = existing->id;
          } else if (existing != nullptr && candidate != nullptr &&
                     loanIsActive(scopes, existing->id) &&
                     (existing->access == candidate->access ||
                      (existing->access == AccessMode::Mutable &&
                       candidate->access == AccessMode::ReadOnly))) {
            // A synthesized iterator retains the physical range loan, while
            // begin() may separately publish a view of that range's external
            // storage. Keep the call's exact source and parent intact: changing
            // its source to the range shell would forge its selected field
            // contract. The retained range loan still keeps the shell and its
            // backing dependency alive for iteration. A carrier of the referent
            // an accessor returned likewise keeps that exact referent below
            // the retained accessor result.
            const MirPlace *referent = output.findPlace(candidate->source);
            const bool accessorReferent =
                candidate->parent == existing->id && referent != nullptr &&
                referent->root == MirPlaceRootKind::Loan &&
                referent->loan == existing->id && referent->projections.empty();
            if (!isRangeIteratorBinding(*statement.binding) &&
                !accessorReferent)
              candidate->source = existing->source;
            retainedLoan = existing->id;
          } else {
            valid = false;
          }
        } else if (found == semanticLoans.end()) {
          semanticLoans.emplace(semanticLoan, retainedLoan);
        }
        const HirLoan *desired = source.findLoan(semanticLoan);
        if (physicalRangeLoan) {
          // Preserve the authoritative semantic-to-MIR owner mapping. The
          // separate producer-owned physical loan ends with its lexical
          // cursor/element binding, including each loop iteration's cleanup.
        } else if (desired == nullptr) {
          valid = false;
        } else if (desired->lifecycleBound ||
                   (desired->kind == SemanticLoanKind::StoredValue &&
                    (desired->access == AccessMode::Mutable ||
                     !desired->designatedFields.empty() ||
                     desired->parent != 0 || desired->storedDependency != 0))) {
          MirLoan &retained = output.loans[retainedLoan - 1];
          MirLoanId expectedParent = 0;
          if (desired->parent != 0) {
            const auto parent = semanticLoans.find(desired->parent);
            if (parent == semanticLoans.end()) {
              valid = false;
            } else {
              expectedParent = parent->second;
            }
          }
          const bool exactRuntimeTransferParent =
              desired->parent == 0 && retained.parent != 0 &&
              retained.parent <= output.loans.size() &&
              retained.kind == MirLoanKind::Stored &&
              retained.access == AccessMode::Mutable &&
              output.loans[retained.parent - 1].kind == MirLoanKind::Stored &&
              output.loans[retained.parent - 1].access == AccessMode::Mutable;
          // Reusing a shared identity for a later value copy does not create
          // a new accessor chain. Validate the retained loan's original
          // producer, not the latest binding's initializer.
          const HirValue *initializer =
              retained.producedBy != 0 ? findValue(retained.producedBy)
              : statement.value        ? findValue(*statement.value)
                                       : nullptr;
          const auto arguments = initializer != nullptr
                                     ? callArgumentValues(*initializer)
                                     : std::vector<HirValueId>{};
          MirLoanId accessorAncestor = expectedParent;
          if (accessorAncestor == 0 && desired->storedDependency != 0 &&
              desired->access == AccessMode::ReadOnly) {
            const auto dependency =
                semanticLoans.find(desired->storedDependency);
            MirLoanId ancestor = retained.parent;
            std::unordered_set<MirLoanId> visited;
            while (const auto *link = output.findLoan(ancestor)) {
              if (!visited.insert(ancestor).second)
                break;
              if (link->kind != MirLoanKind::CallResult) {
                if (link->entry && link->kind == MirLoanKind::Parameter &&
                    dependency != semanticLoans.end() &&
                    link->storedDependency == dependency->second &&
                    sameBorrowSource(link->source, retained.source) &&
                    loanIsActive(scopes, ancestor))
                  accessorAncestor = ancestor;
                break;
              }
              ancestor = link->parent;
            }
          }
          const bool exactAccessorParent =
              retained.kind == MirLoanKind::Stored && initializer != nullptr &&
              (initializer->constructorTarget ||
               storesCarrierReference(*initializer)) &&
              initializer->borrowOrigin == BorrowOriginKind::Argument &&
              initializer->borrowArgument < arguments.size() &&
              retained.parent ==
                  loanForValue(arguments[initializer->borrowArgument]) &&
              retainableCallResultChain(retained.parent, accessorAncestor,
                                        arguments[initializer->borrowArgument],
                                        desired->access);
          // A carrier returned by a method of an accessor's referent, such
          // as `owner->lock()`, borrows that referent through the accessor
          // result, exactly as a constructor argument does.
          const auto accessorReceiver = initializer != nullptr
                                            ? receiverValue(*initializer)
                                            : std::nullopt;
          const bool exactAccessorReceiverParent =
              retained.kind == MirLoanKind::Stored && accessorReceiver &&
              initializer->functionTarget &&
              initializer->intrinsic == IntrinsicKind::None &&
              initializer->borrowOrigin == BorrowOriginKind::Receiver &&
              initializer->borrowFields.empty() &&
              retained.parent == loanForValue(*accessorReceiver) &&
              retainableCallResultChain(retained.parent, accessorAncestor,
                                        *accessorReceiver, desired->access);
          // A stored reference may retain the exact accessor result passed
          // to its constructor or used as its producing receiver. Semantics
          // records the stable owner; MIR also keeps the transient
          // address-producing chain alive, child-first, for as long as the
          // carrier retains it.
          if (retained.parent != expectedParent &&
              !exactRuntimeTransferParent && !exactAccessorParent &&
              !exactAccessorReceiverParent) {
            valid = false;
          }
          recordLoanIdentity(retainedLoan, *desired, *statement.binding);
        } else {
          MirLoan &retained = output.loans[retainedLoan - 1];
          if (retained.semanticLoan != 0 &&
              retained.semanticLoan != semanticLoan) {
            valid = false;
          } else {
            retained.semanticLoan = semanticLoan;
          }
        }
      }
      bindingLoans.insert_or_assign(*statement.binding, retainedLoan);
      std::vector<HirBindingId> &carriers =
          output.loans[retainedLoan - 1].carriers;
      if (std::find(carriers.begin(), carriers.end(), *statement.binding) ==
          carriers.end()) {
        carriers.push_back(*statement.binding);
      }
      const MirLoan *retained = output.findLoan(retainedLoan);
      const HirBindingId transferred =
          statement.value ? movedBorrowCarrierBinding(*statement.value) : 0;
      if (retained != nullptr && retained->lifecycleBound && transferred != 0 &&
          transferred != *statement.binding) {
        // An entry loan's formal is its immutable provenance anchor, even
        // after the owned value moves into a local. Preserve that identity;
        // bindingLoans and lifecycle transfers track current availability.
        const auto *origin = output.findPlace(retained->source);
        if (!retained->entry || origin == nullptr ||
            origin->root != MirPlaceRootKind::Binding ||
            origin->binding != transferred)
          std::erase(output.loans[retainedLoan - 1].carriers, transferred);
        if (const auto sourceLoan = bindingLoans.find(transferred);
            sourceLoan != bindingLoans.end() &&
            sourceLoan->second == retainedLoan) {
          bindingLoans.erase(sourceLoan);
        }
      }
    } else if (binding != nullptr &&
               binding->info.traits.containsBorrowedState && statement.value &&
               !(operands.size() == 1 &&
                 completeEmptyBorrowValue(*statement.value, operands.front(),
                                          binding->info.type))) {
      valid = false;
    }
    MirInstruction initialize{
        .kind = MirInstructionKind::Initialize,
        .hirStatement = statement.id,
        .destination = destination,
        .operands = std::move(operands),
        .info = binding == nullptr
                    ? ExpressionInfo{}
                    : ExpressionInfo{.type = binding->info.type,
                                     .category = ValueCategory::Place,
                                     .access = binding->info.access,
                                     .traits = binding->info.traits}};
    const MirDropObligationId target =
        dropObligationForBinding(*statement.binding);
    const MirDropObligationId sourceObligation =
        statement.value ? dropObligationForValue(*statement.value) : 0;
    if (target != 0) {
      if (temporaryIsActive(sourceObligation)) {
        appendReparentOrTypedTransfer(initialize, sourceObligation, target);
        (void)removeTemporary(sourceObligation);
      } else {
        appendLifecycle(initialize, {.kind = MirLifecycleEventKind::Initialize,
                                     .target = target});
      }
    } else if (output.kind == MirBodyKind::FieldInitializers &&
               temporaryIsActive(sourceObligation)) {
      // A completed per-instance field arms rollback exactly like a
      // constructor stage: the object under construction owns it from
      // normal completion, and any later defined-failure edge in this body
      // must destroy it in reverse order.
      const MirPlace *destinationPlace = output.findPlace(destination);
      const MirDropObligation *sourceDrop =
          output.findDropObligation(sourceObligation);
      const bool exactField =
          fieldInitializerOwner != nullptr && destinationPlace != nullptr &&
          sourceDrop != nullptr && binding != nullptr &&
          std::any_of(fieldInitializerOwner->fields.begin(),
                      fieldInitializerOwner->fields.end(),
                      [&](const HirClassField &field) {
                        return field.binding == destinationPlace->binding &&
                               field.declaration == binding->variable &&
                               field.info.symbol == destinationPlace->symbol &&
                               field.info.type == destinationPlace->type &&
                               field.info.type == sourceDrop->dropType.type;
                      });
      if (destinationPlace != nullptr && sourceDrop != nullptr &&
          destinationPlace->root == MirPlaceRootKind::Binding &&
          destinationPlace->binding != 0 &&
          destinationPlace->projections.empty() && exactField) {
        // Passive aggregate temporaries still have an object-lifetime
        // obligation. Preserve its exact field transfer too: trivial cleanup
        // does not make the publication an unarmed ownership transfer.
        const MirDropObligationId rollback = output.dropObligations.size() + 1;
        output.dropObligations.push_back(
            {.id = rollback,
             .constructionOrder = rollback,
             .kind = MirDropObligationKind::ConstructionRollback,
             .place = destination,
             .dropType = sourceDrop->dropType});
        appendReparentOrTypedTransfer(initialize, sourceObligation, rollback);
        (void)removeTemporary(sourceObligation);
        constructorRollback.push_back(rollback);
      } else {
        // A lexical initializer-local must have its own Binding obligation.
        // Never reinterpret its missing obligation as a field subobject.
        valid = false;
        appendLifecycle(initialize, {.kind = MirLifecycleEventKind::TransferOut,
                                     .source = sourceObligation});
        (void)removeTemporary(sourceObligation);
        constructorUnarmedTransfer = true;
      }
    } else if (temporaryIsActive(sourceObligation) &&
               (output.kind == MirBodyKind::Module ||
                output.kind == MirBodyKind::StaticFieldInitializers)) {
      // Persistent storage is owned outside this body, so it has no local drop
      // obligation to reparent into. The Initialize still consumes the exact
      // source temporary and transfers that lifetime into the external owner.
      appendLifecycle(initialize, {.kind = MirLifecycleEventKind::TransferOut,
                                   .source = sourceObligation});
      (void)removeTemporary(sourceObligation);
    }
    const MirInstructionId initialization =
        appendInstruction(std::move(initialize));
    registerDrop(*statement.binding, destination);
    endFullExpressionLoans(incomingScopes);
    emitTemporaryDrops(incomingTemporaryDrops, statement.value.value_or(0),
                       statement.id);
    endSemanticLoans(statement);
    return initialization;
  }

  void lowerStructuredBinding(const HirStatement &statement) {
    if (!statement.binding) {
      valid = false;
      return;
    }
    const HirBinding *sourceBinding = findBinding(*statement.binding);
    const MirPlaceId sourcePlaceId = placeForBinding(*statement.binding);
    const MirPlace *sourcePlace = output.findPlace(sourcePlaceId);
    if (sourceBinding == nullptr || sourcePlace == nullptr ||
        !statement.value) {
      valid = false;
      return;
    }
    const MirPlace sourceSnapshot = *sourcePlace;
    const std::vector<Scope> incomingScopes = scopes;
    const std::vector<TemporaryDrop> incomingTemporaryDrops = temporaryDrops;

    MirInstruction initialize{.kind = MirInstructionKind::Initialize,
                              .hirStatement = statement.id,
                              .destination = sourcePlaceId,
                              .operands = {valueOperand(*statement.value)},
                              .info = {.type = sourceBinding->info.type,
                                       .category = ValueCategory::Place,
                                       .access = sourceBinding->info.access,
                                       .traits = sourceBinding->info.traits}};
    const MirDropObligationId target =
        dropObligationForBinding(*statement.binding);
    const MirDropObligationId sourceObligation =
        dropObligationForValue(*statement.value);
    if (target != 0) {
      if (temporaryIsActive(sourceObligation)) {
        appendReparentOrTypedTransfer(initialize, sourceObligation, target);
        (void)removeTemporary(sourceObligation);
      } else {
        appendLifecycle(initialize, {.kind = MirLifecycleEventKind::Initialize,
                                     .target = target});
      }
    }
    (void)appendInstruction(std::move(initialize));
    registerDrop(*statement.binding, sourcePlaceId);

    for (const HirStructuredBindingElement &element :
         statement.structuredBindings) {
      const HirBinding *binding = findBinding(element.binding);
      if (binding == nullptr) {
        valid = false;
        continue;
      }
      MirPlace projected = sourceSnapshot;
      projected.id = 0;
      projected.symbol = binding->info.symbol;
      projected.type = binding->info.type;
      projected.access = binding->info.access;
      projected.traits = binding->info.traits;
      projected.sourceValue = statement.value.value_or(0);
      if (element.projection == HirStructuredBindingProjectionKind::Field) {
        if (element.field == 0) {
          valid = false;
          continue;
        }
        projected.projections.push_back(
            {.kind = MirProjectionKind::Field, .field = element.field});
      } else {
        if (!element.index) {
          valid = false;
          continue;
        }
        emitValue(*element.index);
        const HirValue *indexValue = findValue(*element.index);
        const auto *constantIndex =
            indexValue != nullptr &&
                    indexValue->kind == HirValueKind::Literal &&
                    indexValue->literal
                ? std::get_if<std::uint64_t>(&*indexValue->literal)
                : nullptr;
        if (constantIndex == nullptr ||
            sourceSnapshot.type.kind != SemanticType::Array ||
            *constantIndex >= sourceSnapshot.type.arrayLength) {
          valid = false;
          continue;
        }
        projected.projections.push_back({.kind = MirProjectionKind::Index,
                                         .index = mirValueFor(*element.index),
                                         .constantIndex = *constantIndex});
      }
      bindingPlaces.insert_or_assign(element.binding,
                                     appendPlace(std::move(projected)));
    }
    endFullExpressionLoans(incomingScopes);
    emitTemporaryDrops(incomingTemporaryDrops, *statement.value, statement.id);
  }

  void lowerIf(const HirStatement &statement) {
    const auto outerTransferEndpoints = enclosingTransferredLoanEndpoints;
    // HIR, not branch lowering, chooses the semantic endpoint. Register its
    // exact identity while lowering nested branches; this grants no endpoint
    // acknowledgement until a call actually emits EndBorrow on that path.
    for (const SemanticLoanId semantic : statement.endedLoans) {
      const auto found = semanticLoans.find(semantic);
      if (found != semanticLoans.end() && loanIsActive(scopes, found->second))
        enclosingTransferredLoanEndpoints.insert_or_assign(found->second,
                                                           statement.id);
    }
    const std::vector<Scope> conditionScopes = scopes;
    const std::vector<TemporaryDrop> conditionTemporaryDrops = temporaryDrops;
    const MirOperand condition = statement.condition
                                     ? conditionOperand(*statement.condition)
                                     : MirOperand{};
    endFullExpressionLoans(conditionScopes);
    emitTemporaryDrops(conditionTemporaryDrops, statement.condition.value_or(0),
                       statement.id);
    const MirBlockId thenBlock = appendBlock();
    const MirBlockId elseBlock = appendBlock();
    const MirBlockId mergeBlock = appendBlock();
    terminate({.kind = MirTerminatorKind::Branch,
               .hirStatement = statement.id,
               .value = condition,
               .target = thenBlock,
               .elseTarget = elseBlock});

    const std::vector<Scope> incomingScopes = scopes;
    const std::vector<TemporaryDrop> incomingTemporaryDrops = temporaryDrops;
    const std::unordered_map<HirBindingId, MirLoanId> incomingBindingLoans =
        bindingLoans;
    const auto incomingTransferEndpoints = callTransferredLoanEndpoints;
    current = thenBlock;
    scopes = incomingScopes;
    temporaryDrops = incomingTemporaryDrops;
    bindingLoans = incomingBindingLoans;
    endSemanticLoans(statement.thenEntryEndedLoans, statement.id);
    lowerScopedStatement(statement.body);
    const bool thenFallsThrough = !terminated();
    OpenBranch thenOpen{.end = current,
                        .scopes = scopes,
                        .transfers = callTransferredLoanEndpoints};
    const std::vector<TemporaryDrop> thenTemporaryDrops = temporaryDrops;
    std::unordered_map<HirBindingId, MirLoanId> thenBindingLoans =
        projectOuterBindingLoans(incomingBindingLoans, bindingLoans);

    current = elseBlock;
    scopes = incomingScopes;
    temporaryDrops = incomingTemporaryDrops;
    bindingLoans = incomingBindingLoans;
    callTransferredLoanEndpoints = incomingTransferEndpoints;
    endSemanticLoans(statement.elseEntryEndedLoans, statement.id);
    lowerScopedStatement(statement.elseBranch);
    const bool elseFallsThrough = !terminated();
    OpenBranch elseOpen{.end = current,
                        .scopes = scopes,
                        .transfers = callTransferredLoanEndpoints};
    const std::vector<TemporaryDrop> elseTemporaryDrops = temporaryDrops;
    std::unordered_map<HirBindingId, MirLoanId> elseBindingLoans =
        projectOuterBindingLoans(incomingBindingLoans, bindingLoans);

    std::vector<OpenBranch *> completing;
    if (thenFallsThrough)
      completing.push_back(&thenOpen);
    if (elseFallsThrough)
      completing.push_back(&elseOpen);
    const auto consumed = joinConsumedBranches(
        completing, incomingScopes, incomingTransferEndpoints, mergeBlock);
    for (const auto &[loan, endpoint] : consumed) {
      std::erase_if(thenBindingLoans,
                    [&](const auto &entry) { return entry.second == loan; });
      std::erase_if(elseBindingLoans,
                    [&](const auto &entry) { return entry.second == loan; });
    }
    const std::vector<Scope> thenScopes = thenOpen.scopes;
    const auto thenTransferEndpoints = thenOpen.transfers;
    const std::vector<Scope> elseScopes = elseOpen.scopes;
    const auto elseTransferEndpoints = elseOpen.transfers;

    current = mergeBlock;
    if (thenFallsThrough && elseFallsThrough) {
      if (!sameScopeState(thenScopes, elseScopes) ||
          thenBindingLoans != elseBindingLoans ||
          thenTemporaryDrops != elseTemporaryDrops) {
        valid = false;
      }
      scopes = thenScopes;
      temporaryDrops = thenTemporaryDrops;
      bindingLoans = thenBindingLoans;
      callTransferredLoanEndpoints = intersectTransferredLoanEndpoints(
          thenTransferEndpoints, elseTransferEndpoints);
    } else if (thenFallsThrough) {
      scopes = thenScopes;
      temporaryDrops = thenTemporaryDrops;
      bindingLoans = thenBindingLoans;
      callTransferredLoanEndpoints = thenTransferEndpoints;
    } else if (elseFallsThrough) {
      scopes = elseScopes;
      temporaryDrops = elseTemporaryDrops;
      bindingLoans = elseBindingLoans;
      callTransferredLoanEndpoints = elseTransferEndpoints;
    } else {
      scopes = incomingScopes;
      temporaryDrops = incomingTemporaryDrops;
      bindingLoans = incomingBindingLoans;
      callTransferredLoanEndpoints = incomingTransferEndpoints;
      enclosingTransferredLoanEndpoints = outerTransferEndpoints;
      terminate({.kind = MirTerminatorKind::Unreachable,
                 .hirStatement = statement.id});
      return;
    }
    endSemanticLoans(statement);
    enclosingTransferredLoanEndpoints = outerTransferEndpoints;
  }

  void lowerWhile(const HirStatement &statement) {
    const MirBlockId conditionBlock = appendBlock();
    const MirBlockId bodyBlock = appendBlock();
    const MirBlockId exitBlock = appendBlock();
    const MirBlockId naturalExitBlock =
        statement.endedLoans.empty() ? exitBlock : appendBlock();
    terminate({.kind = MirTerminatorKind::Goto, .target = conditionBlock});

    current = conditionBlock;
    const std::vector<Scope> conditionScopes = scopes;
    const std::vector<TemporaryDrop> conditionTemporaryDrops = temporaryDrops;
    const MirOperand condition = statement.condition
                                     ? conditionOperand(*statement.condition)
                                     : MirOperand{};
    endFullExpressionLoans(conditionScopes);
    emitTemporaryDrops(conditionTemporaryDrops, statement.condition.value_or(0),
                       statement.id);
    terminate({.kind = MirTerminatorKind::Branch,
               .hirStatement = statement.id,
               .value = condition,
               .target = bodyBlock,
               .elseTarget = naturalExitBlock});

    const std::vector<Scope> loopScopes = scopes;
    const std::vector<TemporaryDrop> loopTemporaryDrops = temporaryDrops;
    const std::unordered_map<HirBindingId, MirLoanId> loopBindingLoans =
        bindingLoans;
    const auto loopTransferEndpoints = callTransferredLoanEndpoints;
    std::vector<Scope> exitScopes = loopScopes;
    std::unordered_map<HirBindingId, MirLoanId> exitBindingLoans =
        loopBindingLoans;
    normalizeSemanticLoanState(statement.endedLoans, exitScopes,
                               exitBindingLoans);
    if (naturalExitBlock != exitBlock) {
      current = naturalExitBlock;
      scopes = loopScopes;
      temporaryDrops = loopTemporaryDrops;
      bindingLoans = loopBindingLoans;
      endSemanticLoans(statement.endedLoans, statement.id);
      terminate({.kind = MirTerminatorKind::Goto, .target = exitBlock});
    }

    breakContexts.push_back({.target = exitBlock,
                             .keepScopes = loopScopes.size(),
                             .exitLoans = statement.endedLoans});
    continueContexts.push_back(
        {.target = conditionBlock, .keepScopes = loopScopes.size()});
    current = bodyBlock;
    scopes = loopScopes;
    temporaryDrops = loopTemporaryDrops;
    bindingLoans = loopBindingLoans;
    callTransferredLoanEndpoints = loopTransferEndpoints;
    lowerScopedStatement(statement.body);
    if (!terminated()) {
      if (temporaryDrops != loopTemporaryDrops) {
        valid = false;
      }
      terminate({.kind = MirTerminatorKind::Goto, .target = conditionBlock});
    }
    continueContexts.pop_back();
    breakContexts.pop_back();
    current = exitBlock;
    scopes = std::move(exitScopes);
    temporaryDrops = loopTemporaryDrops;
    bindingLoans = std::move(exitBindingLoans);
    // A possibly unexecuted loop cannot contribute a transfer proof at exit.
    callTransferredLoanEndpoints = loopTransferEndpoints;
  }

  void lowerDoWhile(const HirStatement &statement) {
    const MirBlockId bodyBlock = appendBlock();
    const MirBlockId conditionBlock = appendBlock();
    const MirBlockId exitBlock = appendBlock();
    const MirBlockId naturalExitBlock =
        statement.endedLoans.empty() ? exitBlock : appendBlock();
    terminate({.kind = MirTerminatorKind::Goto, .target = bodyBlock});

    const std::vector<Scope> loopScopes = scopes;
    const std::vector<TemporaryDrop> loopTemporaryDrops = temporaryDrops;
    const std::unordered_map<HirBindingId, MirLoanId> loopBindingLoans =
        bindingLoans;
    const auto loopTransferEndpoints = callTransferredLoanEndpoints;
    std::vector<Scope> exitScopes = loopScopes;
    std::unordered_map<HirBindingId, MirLoanId> exitBindingLoans =
        loopBindingLoans;
    normalizeSemanticLoanState(statement.endedLoans, exitScopes,
                               exitBindingLoans);
    breakContexts.push_back({.target = exitBlock,
                             .keepScopes = loopScopes.size(),
                             .exitLoans = statement.endedLoans});
    continueContexts.push_back(
        {.target = conditionBlock, .keepScopes = loopScopes.size()});
    current = bodyBlock;
    scopes = loopScopes;
    temporaryDrops = loopTemporaryDrops;
    bindingLoans = loopBindingLoans;
    lowerScopedStatement(statement.body);
    if (!terminated()) {
      if (temporaryDrops != loopTemporaryDrops) {
        valid = false;
      }
      terminate({.kind = MirTerminatorKind::Goto, .target = conditionBlock});
    }
    continueContexts.pop_back();
    breakContexts.pop_back();

    current = conditionBlock;
    scopes = loopScopes;
    temporaryDrops = loopTemporaryDrops;
    bindingLoans = loopBindingLoans;
    callTransferredLoanEndpoints = loopTransferEndpoints;
    const std::vector<Scope> conditionScopes = scopes;
    const std::vector<TemporaryDrop> conditionTemporaryDrops = temporaryDrops;
    const MirOperand condition = statement.condition
                                     ? conditionOperand(*statement.condition)
                                     : MirOperand{};
    endFullExpressionLoans(conditionScopes);
    emitTemporaryDrops(conditionTemporaryDrops, statement.condition.value_or(0),
                       statement.id);
    terminate({.kind = MirTerminatorKind::Branch,
               .hirStatement = statement.id,
               .value = condition,
               .target = bodyBlock,
               .elseTarget = naturalExitBlock});

    if (naturalExitBlock != exitBlock) {
      current = naturalExitBlock;
      scopes = loopScopes;
      temporaryDrops = loopTemporaryDrops;
      bindingLoans = loopBindingLoans;
      endSemanticLoans(statement.endedLoans, statement.id);
      terminate({.kind = MirTerminatorKind::Goto, .target = exitBlock});
    }

    current = exitBlock;
    scopes = std::move(exitScopes);
    temporaryDrops = loopTemporaryDrops;
    bindingLoans = std::move(exitBindingLoans);
    callTransferredLoanEndpoints = loopTransferEndpoints;
  }

  void lowerFor(const HirStatement &statement) {
    scopes.push_back({});
    lowerNestedStatement(statement.initializer);
    const MirBlockId conditionBlock = appendBlock();
    const MirBlockId bodyBlock = appendBlock();
    const MirBlockId incrementBlock = appendBlock();
    const MirBlockId cleanupBlock = appendBlock();
    const MirBlockId exitBlock = appendBlock();
    const MirBlockId naturalExitBlock =
        statement.condition && !statement.endedLoans.empty() ? appendBlock()
                                                             : cleanupBlock;
    MirBlockId comparisonBlock = conditionBlock;
    MirBlockId advanceBlock = incrementBlock;
    terminate({.kind = MirTerminatorKind::Goto, .target = conditionBlock});

    current = conditionBlock;
    if (statement.condition) {
      const std::vector<Scope> conditionScopes = scopes;
      const std::vector<TemporaryDrop> conditionTemporaryDrops = temporaryDrops;
      const MirOperand condition = conditionOperand(*statement.condition);
      endFullExpressionLoans(conditionScopes);
      emitTemporaryDrops(conditionTemporaryDrops, *statement.condition,
                         statement.id);
      comparisonBlock = current;
      terminate({.kind = MirTerminatorKind::Branch,
                 .hirStatement = statement.id,
                 .value = condition,
                 .target = bodyBlock,
                 .elseTarget = naturalExitBlock});
    } else {
      terminate({.kind = MirTerminatorKind::Goto, .target = bodyBlock});
    }

    const std::vector<Scope> loopScopes = scopes;
    const std::vector<TemporaryDrop> loopTemporaryDrops = temporaryDrops;
    const std::unordered_map<HirBindingId, MirLoanId> loopBindingLoans =
        bindingLoans;
    const auto loopTransferEndpoints = callTransferredLoanEndpoints;
    std::vector<Scope> cleanupScopes = loopScopes;
    std::unordered_map<HirBindingId, MirLoanId> cleanupBindingLoans =
        loopBindingLoans;
    normalizeSemanticLoanState(statement.endedLoans, cleanupScopes,
                               cleanupBindingLoans);
    if (naturalExitBlock != cleanupBlock) {
      current = naturalExitBlock;
      scopes = loopScopes;
      temporaryDrops = loopTemporaryDrops;
      bindingLoans = loopBindingLoans;
      endSemanticLoans(statement.endedLoans, statement.id);
      terminate({.kind = MirTerminatorKind::Goto, .target = cleanupBlock});
    }

    breakContexts.push_back({.target = cleanupBlock,
                             .keepScopes = loopScopes.size(),
                             .exitLoans = statement.endedLoans});
    continueContexts.push_back(
        {.target = incrementBlock, .keepScopes = loopScopes.size()});
    current = bodyBlock;
    scopes = loopScopes;
    temporaryDrops = loopTemporaryDrops;
    bindingLoans = loopBindingLoans;
    callTransferredLoanEndpoints = loopTransferEndpoints;
    lowerScopedStatement(statement.body);
    if (!terminated()) {
      if (temporaryDrops != loopTemporaryDrops) {
        valid = false;
      }
      terminate({.kind = MirTerminatorKind::Goto, .target = incrementBlock});
    }

    current = incrementBlock;
    scopes = loopScopes;
    temporaryDrops = loopTemporaryDrops;
    bindingLoans = loopBindingLoans;
    callTransferredLoanEndpoints = loopTransferEndpoints;
    if (statement.increment) {
      const std::vector<Scope> incrementScopes = scopes;
      const std::vector<TemporaryDrop> incrementTemporaryDrops = temporaryDrops;
      emitValue(*statement.increment);
      endFullExpressionLoans(incrementScopes);
      emitTemporaryDrops(incrementTemporaryDrops, *statement.increment,
                         statement.id);
    }
    advanceBlock = current;
    if (!terminated()) {
      terminate({.kind = MirTerminatorKind::Goto, .target = conditionBlock});
    }
    continueContexts.pop_back();
    breakContexts.pop_back();

    current = cleanupBlock;
    scopes = std::move(cleanupScopes);
    temporaryDrops = loopTemporaryDrops;
    bindingLoans = std::move(cleanupBindingLoans);
    callTransferredLoanEndpoints = loopTransferEndpoints;
    emitScopeExit(scopes.size() - 1);
    terminate({.kind = MirTerminatorKind::Goto, .target = exitBlock});
    scopes.pop_back();
    current = exitBlock;
    temporaryDrops = loopTemporaryDrops;
    loweredLoops.insert_or_assign(
        statement.id, LoweredLoopShape{.conditionEntry = conditionBlock,
                                       .condition = comparisonBlock,
                                       .body = bodyBlock,
                                       .increment = advanceBlock,
                                       .cleanup = cleanupBlock,
                                       .exit = exitBlock});
  }

  void lowerRangeFor(const HirStatement &statement) {
    lowerNestedStatement(statement.body);
    if (!statement.rangeIteration) {
      valid = false;
      return;
    }
    const HirRangeIteration &sourceRange = *statement.rangeIteration;
    const auto loop = loweredLoops.find(sourceRange.coreLoop);
    if (loop == loweredLoops.end()) {
      valid = false;
      return;
    }
    const auto loanFor = [&](SemanticLoanId semanticLoan,
                             bool required) -> MirLoanId {
      if (semanticLoan == 0) {
        if (required) {
          valid = false;
        }
        return 0;
      }
      const auto found = semanticLoans.find(semanticLoan);
      if (found == semanticLoans.end() || found->second == 0 ||
          found->second > output.loans.size()) {
        valid = false;
        return 0;
      }
      return found->second;
    };
    MirDropObligationId ownerDrop = 0;
    const HirBinding *ownerBinding = findBinding(sourceRange.owner);
    if (ownerBinding != nullptr && ownerBinding->dropObligation) {
      const auto found = dropObligations.find(*ownerBinding->dropObligation);
      if (found == dropObligations.end()) {
        valid = false;
      } else {
        ownerDrop = found->second;
      }
    }
    output.rangeIterations.push_back(
        {.id = output.rangeIterations.size() + 1,
         .hirStatement = statement.id,
         .coreLoop = sourceRange.coreLoop,
         .strategy = sourceRange.strategy,
         .rangeType = sourceRange.rangeType,
         .iteratorType = sourceRange.iteratorType,
         .sentinelType = sourceRange.sentinelType,
         .elementType = sourceRange.elementType,
         .elementAccess = sourceRange.elementAccess,
         .owner = placeForBinding(sourceRange.owner),
         .rangeCarrier = placeForBinding(sourceRange.rangeCarrier),
         .iterator = placeForBinding(sourceRange.iterator),
         .sentinel = placeForBinding(sourceRange.sentinel),
         .element = placeForBinding(sourceRange.element),
         .elementOwner = sourceRange.elementOwner == 0
                             ? MirPlaceId{0}
                             : placeForBinding(sourceRange.elementOwner),
         .elementOwnerStatement = sourceRange.elementOwnerStatement,
         .rangeLoan = loanFor(sourceRange.rangeLoan, true),
         .elementLoan =
             [&] {
               if (isPhysicalProxyRangeProtocolBinding(sourceRange.element)) {
                 MirLoanId selected = 0;
                 for (const auto &loan : output.loans)
                   if ((loan.kind == MirLoanKind::CallResult ||
                        loan.kind == MirLoanKind::Stored) &&
                       std::find(loan.carriers.begin(), loan.carriers.end(),
                                 sourceRange.element) != loan.carriers.end()) {
                     if (selected != 0) {
                       valid = false;
                       return MirLoanId{0};
                     }
                     selected = loan.id;
                   }
                 if (selected != 0)
                   return selected;
               }
               return loanFor(sourceRange.elementLoan,
                              sourceRange.elementLoan != 0);
             }(),
         .elementOwnerLoan =
             [&] {
               if (isPhysicalProxyRangeProtocolBinding(
                       sourceRange.elementOwner))
                 for (const auto &loan : output.loans)
                   if (loan.kind == MirLoanKind::CallResult &&
                       std::find(loan.carriers.begin(), loan.carriers.end(),
                                 sourceRange.elementOwner) !=
                           loan.carriers.end())
                     return loan.id;
               return loanFor(sourceRange.elementOwnerLoan,
                              sourceRange.elementOwnerLoan != 0);
             }(),
         .elementAccessValue = sourceRange.materializesElementValue
                                   ? mirValueFor(sourceRange.elementAccessValue)
                                   : MirValueId{0},
         .elementAccessSource = sourceRange.elementAccessValue,
         .dereferenceFunction = sourceRange.dereferenceFunction,
         .elementOwnerDrop =
             sourceRange.materializesElementValue
                 ? dropObligationForBinding(sourceRange.elementOwner)
                 : MirDropObligationId{0},
         .elementSourceDrop =
             sourceRange.materializesElementValue
                 ? dropObligationForValue(sourceRange.elementAccessValue)
                 : MirDropObligationId{0},
         .ownerDrop = ownerDrop,
         .conditionEntryBlock = loop->second.conditionEntry,
         .conditionBlock = loop->second.condition,
         .bodyBlock = loop->second.body,
         .incrementBlock = loop->second.increment,
         .cleanupBlock = loop->second.cleanup,
         .exitBlock = loop->second.exit,
         .arrayLength = sourceRange.arrayLength,
         .ownsRangeTemporary = sourceRange.ownsRangeTemporary,
         .materializesElementValue = sourceRange.materializesElementValue,
         .endpointParking =
             rangeEndpointParking.contains(statement.id)
                 ? std::optional{rangeEndpointParking.at(statement.id)}
                 : std::nullopt});
  }

  // A consumed payload-enum switch subject enters its hidden owner binding at
  // the end of the subject's full expression, exactly like a local declared
  // from that value: its temporary obligation reparents into the binding, and
  // the binding joins the enclosing scope's cleanup so a failure while the
  // subject's other temporaries drop still drops the enum once. Each arm
  // retires that owner at its entry; the scope never drops it normally.
  struct ConsumedSwitchSubject {
    HirBindingId binding = 0;
    MirPlaceId place = 0;
    MirDropObligationId obligation = 0;
    SemanticType type = SemanticType::Unknown;
    SemanticLoanId dependency = 0;
  };

  [[nodiscard]] std::optional<ConsumedSwitchSubject>
  initializeConsumedSwitchSubject(const HirStatement &statement) {
    const HirBinding *binding = findBinding(*statement.binding);
    const HirValue *value = findValue(*statement.value);
    if (binding == nullptr || value == nullptr ||
        binding->info.type.kind != SemanticType::Enum ||
        binding->info.type != value->info.type ||
        value->info.category != ValueCategory::Value || scopes.empty()) {
      valid = false;
      return std::nullopt;
    }
    const MirPlaceId destination = placeForBinding(*statement.binding);
    MirOperand initializer = valueOperand(*statement.value);
    if (terminated()) {
      return std::nullopt;
    }
    routeDirectResultInitialization(initializer, destination);
    MirInstruction initialize{.kind = MirInstructionKind::Initialize,
                              .hirStatement = statement.id,
                              .destination = destination,
                              .operands = {std::move(initializer)},
                              .info = {.type = binding->info.type,
                                       .category = ValueCategory::Place,
                                       .access = binding->info.access,
                                       .traits = binding->info.traits}};
    const MirDropObligationId target =
        dropObligationForBinding(*statement.binding);
    const MirDropObligationId sourceObligation =
        dropObligationForValue(*statement.value);
    if (target != 0) {
      if (temporaryIsActive(sourceObligation)) {
        appendReparentOrTypedTransfer(initialize, sourceObligation, target);
        (void)removeTemporary(sourceObligation);
      } else {
        appendLifecycle(initialize, {.kind = MirLifecycleEventKind::Initialize,
                                     .target = target});
      }
    } else if (binding->info.traits.drop == DropKind::Lexical &&
               !nonLexicalBindings.contains(*statement.binding)) {
      valid = false;
    }
    (void)appendInstruction(std::move(initialize));
    if (binding->info.retainedLoan != 0) {
      const auto mapped = semanticLoans.find(binding->info.retainedLoan);
      const auto retained = loanForValue(*statement.value);
      const auto *group = output.findLoan(retained);
      if (mapped == semanticLoans.end() || mapped->second != retained ||
          !group || group->kind != MirLoanKind::Stored ||
          !loanIsActive(scopes, retained)) {
        valid = false;
        return std::nullopt;
      }
      // This real value initialization acquires the existing dependency
      // group, exactly as a named carrier initialized by a move would. It
      // does not alter the formal's immutable external provenance or make
      // the new physical shell initially available at function entry.
      bindingLoans.insert_or_assign(*statement.binding, retained);
      auto &carriers = output.loans[retained - 1].carriers;
      if (std::find(carriers.begin(), carriers.end(), *statement.binding) ==
          carriers.end())
        carriers.push_back(*statement.binding);
    }
    registerDrop(*statement.binding, destination);
    return ConsumedSwitchSubject{.binding = *statement.binding,
                                 .place = destination,
                                 .obligation = target,
                                 .type = binding->info.type,
                                 .dependency = binding->info.retainedLoan};
  }

  // Every path leaving the subject's evaluation retires the hidden owner
  // explicitly, so it must not remain among the drops of the enclosing scope.
  static void forgetConsumedSwitchSubject(std::vector<Scope> &state,
                                          MirPlaceId place) {
    for (Scope &scope : state) {
      std::erase(scope.drops, place);
    }
  }

  // Retires the consumed subject at an arm's entry. A binding arm moves every
  // field of the active alternative into its by-value bindings; the final
  // extraction transfers the enum's obligation out, so the drained shell is
  // never dropped. Any other arm (tag-only labels or `default`) drops the
  // enum, and therefore its active payload, before the arm's first statement.
  void retireConsumedSwitchSubject(const HirStatement &statement,
                                   const HirSwitchArm &arm,
                                   const ConsumedSwitchSubject &consumed) {
    const auto retireDependency = [&] {
      if (consumed.dependency == 0)
        return;
      const auto found = semanticLoans.find(consumed.dependency);
      const auto *group = found == semanticLoans.end()
                              ? nullptr
                              : output.findLoan(found->second);
      if (!group || group->kind != MirLoanKind::Stored || group->escapes ||
          group->lifecycleBound ||
          std::find(group->carriers.begin(), group->carriers.end(),
                    consumed.binding) == group->carriers.end() ||
          !loanIsActive(scopes, group->id) ||
          lifecycleRetainedLoans({}).contains(group->id) ||
          containsLoan(constructorPublishedLoans, group->id)) {
        valid = false;
        return;
      }
      // The subject's physical owner has already been consumed. Its entry
      // group is not the capability returned to the caller: the exact arm
      // prefix publishes new scalar selections after retiring this group.
      // Ordinary endpoint heuristics retain summarized entry loans until
      // return and therefore cannot implement this explicit handoff.
      const auto id = group->id;
      (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                               .hirStatement = statement.id,
                               .loan = id});
      removeLoanFromState(id, scopes, bindingLoans);
    };
    const MirPlace *subjectPlace = output.findPlace(consumed.place);
    if (subjectPlace == nullptr) {
      valid = false;
      return;
    }
    if (arm.payloadBindings.empty()) {
      if (consumed.obligation != 0) {
        (void)appendNormalDrop(
            consumed.obligation, consumed.place, scopeDropInfo(subjectPlace),
            scopeDropLifecycle(consumed.obligation), {consumed.obligation}, {});
        appendNormalCleanupBoundary({consumed.obligation});
      }
      retireDependency();
      return;
    }
    std::vector<std::size_t> deferredBindings;
    const auto initializePayload = [&](std::size_t index) {
      const auto &payload = arm.payloadBindings[index];
      const auto *binding = findBinding(payload.binding);
      const auto *extraction = findValue(payload.value);
      const auto destination = placeForBinding(payload.binding);
      const bool referenceTransfer = extraction->payloadExtractionKind ==
                                     PayloadExtractionKind::TransferReference;
      MirInstruction initialize{
          .kind = MirInstructionKind::Initialize,
          .hirStatement = statement.id,
          .destination = destination,
          .operands = {referenceTransfer
                           ? referenceOperand(payload.value, payload.binding)
                           : valueOperand(payload.value)},
          .info = {.type = binding->info.type,
                   .category = ValueCategory::Place,
                   .access = binding->info.access,
                   .traits = binding->info.traits}};
      if (binding->info.traits.drop == DropKind::Lexical) {
        const auto obligation = dropObligationForBinding(payload.binding);
        if (obligation != 0)
          appendLifecycle(initialize,
                          {.kind = MirLifecycleEventKind::Initialize,
                           .target = obligation});
      }
      (void)appendInstruction(std::move(initialize));
      registerDrop(payload.binding, destination);
    };
    for (std::size_t index = 0; index < arm.payloadBindings.size(); ++index) {
      const HirSwitchArm::PayloadBinding &payload = arm.payloadBindings[index];
      const HirBinding *binding = findBinding(payload.binding);
      const HirValue *extraction = findValue(payload.value);
      const MirPlaceId destination = placeForBinding(payload.binding);
      const bool referenceTransfer =
          extraction != nullptr && extraction->payloadExtractionKind ==
                                       PayloadExtractionKind::TransferReference;
      const bool exactBinding =
          binding != nullptr && extraction != nullptr &&
          (referenceTransfer
               ? binding->info.type.kind == SemanticType::Reference &&
                     binding->info.type.arguments.size() == 1 &&
                     binding->info.type.arguments.front() ==
                         extraction->info.type &&
                     extraction->info.category == ValueCategory::Place &&
                     binding->info.type.referenceAccess ==
                         extraction->info.access
               : binding->info.type.kind != SemanticType::Reference &&
                     binding->info.type == extraction->info.type &&
                     extraction->info.category == ValueCategory::Value &&
                     extraction->payloadExtractionKind ==
                         PayloadExtractionKind::MoveValue);
      if (binding == nullptr || extraction == nullptr || destination == 0 ||
          extraction->kind != HirValueKind::PayloadExtraction ||
          !exactBinding || !extraction->enumOwner || !extraction->enumVariant ||
          !extraction->payloadIndex) {
        valid = false;
        continue;
      }
      if (referenceTransfer) {
        const auto *loan = source.findLoan(binding->info.retainedLoan);
        if (consumed.dependency == 0 || !loan || !loan->referenceSelection ||
            loan->referenceSelection->transferredFrom != consumed.dependency) {
          valid = false;
          continue;
        }
      }
      MirInstruction extract{.kind = MirInstructionKind::Compute,
                             .hirValue = extraction->id,
                             .operation = MirOperation::PayloadExtract,
                             .enumOwner = extraction->enumOwner,
                             .enumVariant = extraction->enumVariant,
                             .payloadIndex = extraction->payloadIndex,
                             .payloadExtractionKind =
                                 extraction->payloadExtractionKind,
                             .info = extraction->info};
      extract.operands.push_back({.kind = MirOperandKind::Move,
                                  .place = consumed.place,
                                  .type = consumed.type});
      extract.result = resultFor(*extraction);
      if (consumed.obligation != 0 && index + 1 == arm.payloadBindings.size()) {
        appendLifecycle(extract, {.kind = MirLifecycleEventKind::TransferOut,
                                  .source = consumed.obligation});
      }
      (void)appendInstruction(std::move(extract));
      emittedValues.insert(extraction->id);
      if (consumed.dependency != 0)
        deferredBindings.push_back(index);
      else
        initializePayload(index);
    }
    if (consumed.dependency != 0) {
      // Evaluate all exact field addresses while the old group is active,
      // retire it once, then publish its successor capabilities. No user
      // operation or potentially failing instruction belongs in this prefix.
      retireDependency();
      for (const auto index : deferredBindings)
        initializePayload(index);
    }
  }

  // A reference result names its referent through the producing call's loan.
  // A read-borrowed switch subject rooted at such a loan is observed after the
  // subject's full expression, by the `Switch` and then by every arm's payload
  // extractions, as a reference local's subject is through that local's loan.
  // Returns that uncarried loan and the uncarried accessor results it
  // reborrows, child first.
  [[nodiscard]] std::vector<MirLoanId>
  switchSubjectLoans(const MirOperand &subject,
                     const std::vector<Scope> &baseline) const {
    std::vector<MirLoanId> loans;
    const MirPlace *place = subject.kind == MirOperandKind::BorrowRead
                                ? output.findPlace(subject.place)
                                : nullptr;
    MirLoanId loan = place != nullptr && place->root == MirPlaceRootKind::Loan
                         ? place->loan
                         : 0;
    while (loan != 0 && !containsLoan(loans, loan) &&
           !loanIsActive(baseline, loan) && loanIsActive(scopes, loan)) {
      const MirLoan *record = output.findLoan(loan);
      if (record == nullptr || record->lifecycleBound || record->escapes ||
          !record->carriers.empty())
        break;
      loans.push_back(loan);
      loan = record->parent;
    }
    return loans;
  }

  // Subject loans an arm's live loans still depend on: those reborrowed or
  // stored-retained by a live loan, or rooting its source place. An ancestor
  // of a retained loan stays with it.
  [[nodiscard]] std::vector<MirLoanId>
  retainedSwitchSubjectLoans(const std::vector<MirLoanId> &subjectLoans) const {
    const auto retained = retainedReborrowAncestors();
    const auto kept = std::find_if(
        subjectLoans.begin(), subjectLoans.end(), [&](MirLoanId subject) {
          if (retained.contains(subject))
            return true;
          return std::any_of(
              scopes.begin(), scopes.end(), [&](const Scope &scope) {
                return std::any_of(
                    scope.loans.begin(), scope.loans.end(), [&](MirLoanId id) {
                      const MirLoan *loan = output.findLoan(id);
                      const MirPlace *source =
                          loan == nullptr ? nullptr
                                          : output.findPlace(loan->source);
                      return !containsLoan(subjectLoans, id) &&
                             source != nullptr &&
                             source->root == MirPlaceRootKind::Loan &&
                             source->loan == subject;
                    });
              });
        });
    return {kept, subjectLoans.end()};
  }

  // The semantic place a chain of call-result loans borrows from: the first
  // keyed source along the chain, a prefix of the selected referent.
  [[nodiscard]] std::optional<PlaceKey> loanOriginKey(MirLoanId loan) const {
    std::unordered_set<MirLoanId> visited;
    while (loan != 0 && visited.insert(loan).second) {
      const MirLoan *record = output.findLoan(loan);
      const MirPlace *source =
          record == nullptr ? nullptr : output.findPlace(record->source);
      if (source == nullptr)
        return std::nullopt;
      if (source->key)
        return source->key;
      loan = source->root == MirPlaceRootKind::Loan ? source->loan : 0;
    }
    return std::nullopt;
  }

  void endSwitchSubjectLoans(const std::vector<MirLoanId> &loans,
                             HirStatementId statement) {
    for (const MirLoanId loan : childFirstMirLoans(loans)) {
      if (!loanIsActive(scopes, loan))
        continue;
      (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                               .hirStatement = statement,
                               .loan = loan});
      removeLoanFromState(loan, scopes, bindingLoans);
    }
  }

  void lowerSwitch(const HirStatement &statement) {
    const std::vector<Scope> subjectScopes = scopes;
    const std::vector<TemporaryDrop> subjectTemporaryDrops = temporaryDrops;
    MirOperand subject;
    std::optional<ConsumedSwitchSubject> consumed;
    if (statement.binding) {
      if (!statement.value) {
        valid = false;
        return;
      }
      consumed = initializeConsumedSwitchSubject(statement);
      if (terminated()) {
        // A terminal subject leaves the whole switch unreachable.
        return;
      }
      if (!consumed) {
        valid = false;
        return;
      }
      subject = {.kind = MirOperandKind::BorrowRead,
                 .place = consumed->place,
                 .type = consumed->type};
    } else if (statement.value) {
      const HirValue *sourceValue = findValue(*statement.value);
      // A named subject is observed. An enum that owns cleanup, that cannot
      // be copied (a cleanup-free alternative may still hold a move-only
      // field), or that an arm binds by reference is read-borrowed in place;
      // only a copyable passive enum is read through a copy.
      const bool payloadPlace =
          sourceValue != nullptr &&
          sourceValue->info.category == ValueCategory::Place &&
          sourceValue->info.type.kind == SemanticType::Enum &&
          std::any_of(program.enumDeclarations().begin(),
                      program.enumDeclarations().end(),
                      [&](const HirEnum &enumeration) {
                        return enumeration.payload &&
                               enumeration.type == sourceValue->info.type;
                      });
      // A subject's place exists only once its producer has run: a reference
      // result is addressed through that call's loan, never a placeholder
      // created before the call. The read-borrow transports only the
      // subject's address, so no Load performs a selected fixed-array
      // element's bounds check. Finish that selection at its checked
      // AddressOf before the switch observes it.
      if (payloadPlace)
        emitPlaceDependencies(*statement.value, true);
      const MirPlaceId sourcePlace =
          payloadPlace ? placeForValue(*statement.value) : 0;
      if (payloadPlace && sourcePlace != 0) {
        // A reference result names no semantic place of its own. Its overlap
        // identity is the owner its producing loans borrow, the place an
        // arm's payload borrow also protects.
        if (const MirPlace *place = output.findPlace(sourcePlace);
            place != nullptr && place->root == MirPlaceRootKind::Loan &&
            !place->key)
          if (auto key = loanOriginKey(place->loan))
            output.places[sourcePlace - 1].key = std::move(key);
        // Matching observes the discriminant of a place; it does not copy
        // the complete enum merely because its payload happens to be passive.
        // Arm bindings still perform their independently selected copy/borrow.
        subject = {.kind = MirOperandKind::BorrowRead,
                   .place = sourcePlace,
                   .type = sourceValue->info.type};
      } else {
        subject = valueOperand(*statement.value);
      }
    }
    // Each arm ends these after binding its payloads; the unmatched edge ends
    // them before reaching the exit.
    const std::vector<MirLoanId> subjectLoans =
        consumed ? std::vector<MirLoanId>{}
                 : switchSubjectLoans(subject, subjectScopes);
    endFullExpressionLoans(subjectScopes, subjectLoans);
    if (statement.value && !consumed)
      payloadSwitchSubjects[*statement.value] = subject;
    emitTemporaryDrops(subjectTemporaryDrops, statement.value.value_or(0),
                       statement.id);
    const MirBlockId exitBlock = appendBlock();
    std::vector<MirBlockId> armBlocks;
    armBlocks.reserve(statement.switchArms.size());
    for (std::size_t index = 0; index < statement.switchArms.size(); ++index) {
      armBlocks.push_back(appendBlock());
    }
    const bool hasDefault =
        std::any_of(statement.switchArms.begin(), statement.switchArms.end(),
                    [](const HirSwitchArm &arm) {
                      return std::any_of(arm.labels.begin(), arm.labels.end(),
                                         [](const HirSwitchLabel &label) {
                                           return label.isDefault;
                                         });
                    });
    const bool coversEveryValue = hasDefault || statement.exhaustiveSwitch;
    const MirBlockId unmatchedBlock =
        statement.exhaustiveSwitch
            ? appendBlock()
            : (!hasDefault &&
                       (!statement.endedLoans.empty() || !subjectLoans.empty())
                   ? appendBlock()
                   : exitBlock);

    MirTerminator terminator{.kind = MirTerminatorKind::Switch,
                             .hirStatement = statement.id,
                             .value = subject,
                             .target = unmatchedBlock};
    for (std::size_t armIndex = 0; armIndex < statement.switchArms.size();
         ++armIndex) {
      for (const HirSwitchLabel &label :
           statement.switchArms[armIndex].labels) {
        if (label.isDefault) {
          terminator.target = armBlocks[armIndex];
        } else {
          terminator.switchTargets.push_back(
              {.value = label.constant, .target = armBlocks[armIndex]});
        }
      }
    }
    terminate(std::move(terminator));

    const std::vector<Scope> switchScopes = scopes;
    const std::vector<TemporaryDrop> switchTemporaryDrops = temporaryDrops;
    const std::unordered_map<HirBindingId, MirLoanId> switchBindingLoans =
        bindingLoans;
    const auto switchTransferEndpoints = callTransferredLoanEndpoints;
    std::vector<Scope> exitScopes = switchScopes;
    if (consumed) {
      forgetConsumedSwitchSubject(exitScopes, consumed->place);
    }
    std::unordered_map<HirBindingId, MirLoanId> exitBindingLoans =
        switchBindingLoans;
    for (const MirLoanId loan : subjectLoans)
      removeLoanFromState(loan, exitScopes, exitBindingLoans);
    normalizeSemanticLoanState(statement.endedLoans, exitScopes,
                               exitBindingLoans);
    if (coversEveryValue && !statement.switchArms.empty()) {
      std::vector<SemanticLoanId> endedOnEveryArm =
          statement.switchArms.front().entryEndedLoans;
      std::erase_if(endedOnEveryArm, [&](SemanticLoanId loan) {
        return std::any_of(
                   statement.switchArms.begin() + 1, statement.switchArms.end(),
                   [&](const HirSwitchArm &arm) {
                     return std::find(arm.entryEndedLoans.begin(),
                                      arm.entryEndedLoans.end(),
                                      loan) == arm.entryEndedLoans.end();
                   }) ||
               std::find(statement.endedLoans.begin(),
                         statement.endedLoans.end(),
                         loan) != statement.endedLoans.end();
      });
      for (const MirLoanId loan :
           childFirstLoans(endedOnEveryArm, &exitScopes)) {
        if (loanIsActive(exitScopes, loan)) {
          removeLoanFromState(loan, exitScopes, exitBindingLoans);
        }
      }
    }
    if (statement.exhaustiveSwitch) {
      current = unmatchedBlock;
      scopes = switchScopes;
      temporaryDrops = switchTemporaryDrops;
      bindingLoans = switchBindingLoans;
      terminate({.kind = MirTerminatorKind::Unreachable,
                 .hirStatement = statement.id});
    } else if (unmatchedBlock != exitBlock || consumed) {
      // A semantically valid consumed payload switch is exhaustive or has a
      // `default` arm, so it never reaches this unmatched edge.
      if (consumed) {
        valid = false;
      }
      current = unmatchedBlock;
      scopes = switchScopes;
      temporaryDrops = switchTemporaryDrops;
      bindingLoans = switchBindingLoans;
      endSwitchSubjectLoans(subjectLoans, statement.id);
      endSemanticLoans(statement.endedLoans, statement.id);
      terminate({.kind = MirTerminatorKind::Goto, .target = exitBlock});
    }

    breakContexts.push_back({.target = exitBlock,
                             .keepScopes = switchScopes.size(),
                             .exitLoans = statement.endedLoans});
    for (std::size_t armIndex = 0; armIndex < statement.switchArms.size();
         ++armIndex) {
      current = armBlocks[armIndex];
      scopes = switchScopes;
      temporaryDrops = switchTemporaryDrops;
      bindingLoans = switchBindingLoans;
      callTransferredLoanEndpoints = switchTransferEndpoints;
      if (consumed) {
        forgetConsumedSwitchSubject(scopes, consumed->place);
      }
      scopes.push_back({});
      // The arm owns the subject's place loans: a tag-only arm ends them at
      // entry, before loans they may reborrow; a binding arm ends them once
      // its payloads are bound, unless a reference binding still depends on
      // them, in which case its scope exit ends them after that binding.
      const auto &payloadBindings =
          statement.switchArms[armIndex].payloadBindings;
      for (const MirLoanId loan : subjectLoans) {
        removeLoanFromState(loan, scopes, bindingLoans);
        scopes.back().loans.push_back(loan);
      }
      if (payloadBindings.empty())
        endSwitchSubjectLoans(subjectLoans, statement.id);
      auto entryEnded = statement.switchArms[armIndex].entryEndedLoans;
      if (consumed && consumed->dependency != 0) {
        if (std::find(entryEnded.begin(), entryEnded.end(),
                      consumed->dependency) == entryEnded.end())
          valid = false;
        std::erase(entryEnded, consumed->dependency);
      }
      endSemanticLoans(entryEnded, statement.id);
      if (consumed) {
        retireConsumedSwitchSubject(statement, statement.switchArms[armIndex],
                                    *consumed);
      }
      // Copy payloads before publishing any exclusive payload loan. A
      // by-value extraction reads the enum subject, and that read would
      // conflict with a mutable sibling binding if the source order placed
      // the mutable binding first. Pattern bindings have no user evaluation
      // order; their values are selected from the same guarded alternative.
      std::vector<const HirSwitchArm::PayloadBinding *> orderedPayloads;
      orderedPayloads.reserve(payloadBindings.size());
      for (const auto &payload : payloadBindings)
        if (const HirBinding *binding = findBinding(payload.binding);
            binding != nullptr &&
            binding->info.type.kind != SemanticType::Reference)
          orderedPayloads.push_back(&payload);
      for (const auto &payload : payloadBindings)
        if (const HirBinding *binding = findBinding(payload.binding);
            binding == nullptr ||
            binding->info.type.kind == SemanticType::Reference)
          orderedPayloads.push_back(&payload);
      for (const HirSwitchArm::PayloadBinding *orderedPayload :
           orderedPayloads) {
        const HirSwitchArm::PayloadBinding &payload = *orderedPayload;
        if (consumed) {
          break;
        }
        const HirBinding *binding = findBinding(payload.binding);
        const MirPlaceId destination = placeForBinding(payload.binding);
        if (binding == nullptr || destination == 0 || payload.value == 0) {
          valid = false;
          continue;
        }
        MirInstruction initialize{
            .kind = MirInstructionKind::Initialize,
            .hirStatement = statement.id,
            .destination = destination,
            .operands = {binding->info.type.kind == SemanticType::Reference
                             ? referenceOperand(payload.value, payload.binding)
                             : valueOperand(payload.value)},
            .info = {.type = binding->info.type,
                     .category = ValueCategory::Place,
                     .access = binding->info.access,
                     .traits = binding->info.traits}};
        // A copied class payload binding owns its copy: arm the binding's
        // obligation at this Initialize so the arm-exit Drop ends an
        // active lifetime, exactly like an ordinary local initialization.
        if (binding->info.type.kind != SemanticType::Reference &&
            binding->info.traits.drop == DropKind::Lexical) {
          const MirDropObligationId obligation =
              dropObligationForBinding(payload.binding);
          if (obligation != 0) {
            appendLifecycle(initialize,
                            {.kind = MirLifecycleEventKind::Initialize,
                             .target = obligation});
          }
        }
        (void)appendInstruction(std::move(initialize));
        // A by-value binding of a dependent text payload (L-OWN-02) holds
        // the subject's owner text: it carries the subject's active Stored
        // loan, as a local initialized from a published field read does, so
        // a copy of the binding retains that owner too.
        if (binding->info.type.kind != SemanticType::Reference &&
            binding->info.retainedLoan != 0) {
          const auto mapped = semanticLoans.find(binding->info.retainedLoan);
          const MirLoan *retained = mapped == semanticLoans.end()
                                        ? nullptr
                                        : output.findLoan(mapped->second);
          if (!retained || retained->kind != MirLoanKind::Stored ||
              !loanIsActive(scopes, retained->id)) {
            valid = false;
          } else {
            bindingLoans.insert_or_assign(payload.binding, retained->id);
            auto &carriers = output.loans[retained->id - 1].carriers;
            if (std::find(carriers.begin(), carriers.end(), payload.binding) ==
                carriers.end())
              carriers.push_back(payload.binding);
          }
        }
        registerDrop(payload.binding, destination);
        // A by-value binding of a dependent text payload carries the
        // subject's owner loan (L-OWN-02), so a view copied from it does too.
        if (binding->info.type.kind != SemanticType::Reference &&
            binding->info.retainedLoan != 0)
          if (const auto found = semanticLoans.find(binding->info.retainedLoan);
              found != semanticLoans.end()) {
            bindingLoans.insert_or_assign(payload.binding, found->second);
            auto &carriers = output.loans[found->second - 1].carriers;
            if (std::find(carriers.begin(), carriers.end(), payload.binding) ==
                carriers.end())
              carriers.push_back(payload.binding);
          }
      }
      if (!payloadBindings.empty()) {
        const auto retained = retainedSwitchSubjectLoans(subjectLoans);
        std::vector<MirLoanId> released;
        for (const MirLoanId loan : subjectLoans)
          if (!containsLoan(retained, loan))
            released.push_back(loan);
        endSwitchSubjectLoans(released, statement.id);
      }
      lowerStatements(statement.switchArms[armIndex].statements);
      if (!terminated()) {
        if (temporaryDrops != switchTemporaryDrops) {
          valid = false;
        }
        endSemanticLoansIfActive(statement.endedLoans, statement.id);
        emitScopeExit(scopes.size() - 1);
        terminate({.kind = MirTerminatorKind::Goto, .target = exitBlock});
      }
      scopes.pop_back();
    }
    breakContexts.pop_back();
    current = exitBlock;
    scopes = std::move(exitScopes);
    temporaryDrops = switchTemporaryDrops;
    bindingLoans = std::move(exitBindingLoans);
    callTransferredLoanEndpoints = switchTransferEndpoints;
  }

  // A parameter loan describes the caller-owned dependency, not necessarily
  // the address returned by an accessor. Local reference bindings retain that
  // exact runtime address separately. Both plain and fallible reference
  // publication must forward it; matching referent types cannot recover it.
  [[nodiscard]] MirPlaceId referenceReturnRuntimeAlias(HirValueId valueId,
                                                       const MirLoan &loan) {
    const HirValue *value = findValue(valueId);
    if (value == nullptr || loan.kind != MirLoanKind::Parameter || !loan.entry)
      return 0;
    const MirPlaceId selectedPlace = placeForValue(valueId);
    const MirPlace *selected = output.findPlace(selectedPlace);
    const MirPlace *parameter = output.findPlace(loan.source);
    return selected != nullptr && parameter != nullptr &&
                   selected->root == MirPlaceRootKind::Binding &&
                   selected->binding != parameter->binding &&
                   selected->type == value->info.type
               ? selectedPlace
               : 0;
  }

  void lowerReturn(const HirStatement &statement) {
    if (statement.value) {
      const auto *value = findValue(*statement.value);
      if (value && value->neverReturns) {
        emitValue(*statement.value);
        if (!terminated())
          valid = false;
        return;
      }
    }
    const std::vector<Scope> incomingScopes = scopes;
    const std::vector<TemporaryDrop> incomingTemporaryDrops = temporaryDrops;
    std::optional<MirOperand> result;
    std::optional<MirLoanId> returnLoan;
    std::optional<SharedExitCarrier> shared =
        sharedReturnCarrier(statement, incomingTemporaryDrops);
    if (statement.value) {
      if (output.returnType.kind == SemanticType::Void) {
        const HirValue *value = findValue(*statement.value);
        if (value == nullptr || value->info.type != SemanticType::Void) {
          valid = false;
          return;
        }
        // A void expression is evaluated for its effects, not transported as
        // a runtime value. Its full-expression and lexical cleanup still run
        // before the operand-free return below.
        emitValue(*statement.value);
      } else if (output.returnType.kind == SemanticType::Reference) {
        MirOperand borrow = referenceOperand(*statement.value, 0,
                                             output.returnType.referenceAccess);
        if (borrow.loan != 0) {
          MirLoanId storedCarrierEntry = 0;
          MirLoanId referenceParameterEntry = 0;
          if (const std::optional<HirBindingId> summarized =
                  returnBorrowParameterBinding();
              summarized && borrow.loan <= output.loans.size()) {
            const HirBindingId parameter = *summarized;
            const MirLoan &candidate = output.loans[borrow.loan - 1];
            const MirPlace *source = output.findPlace(candidate.source);
            const bool carriedByParameter =
                source != nullptr && sourceIsReturnOriginFormal(*source) &&
                std::find(candidate.carriers.begin(), candidate.carriers.end(),
                          parameter) != candidate.carriers.end();
            if (candidate.kind == MirLoanKind::Stored && candidate.entry &&
                !candidate.designatedFields.empty() && carriedByParameter) {
              storedCarrierEntry = borrow.loan;
            } else if (candidate.kind == MirLoanKind::Parameter &&
                       candidate.entry && carriedByParameter) {
              referenceParameterEntry = borrow.loan;
            }
          }
          if (storedCarrierEntry != 0) {
            const HirValue *value = findValue(*statement.value);
            const MirPlaceId runtimePlace = placeForValue(*statement.value);
            const MirPlace *runtime = output.findPlace(runtimePlace);
            // An upcast return designates the same carrier as a base or
            // interface view of it.
            const auto designates = [&](const SemanticType &type) {
              return type == output.returnType.arguments.front() ||
                     hasBase(type, output.returnType.arguments.front());
            };
            const bool exactRuntimeReferent =
                value != nullptr && output.returnType.arguments.size() == 1 &&
                runtime != nullptr && designates(runtime->type) &&
                designates(value->info.type);
            if (!exactRuntimeReferent) {
              valid = false;
            } else {
              // The entry Stored loan names the caller-owned dependency,
              // while runtimePlace names the exact reference value returned
              // by this body. Publish both facts into a fresh Return loan and
              // retire the by-value carrier parameter's body-local entry loan
              // at the same instruction boundary.
              const MirLoan retainedLoan = output.loans[storedCarrierEntry - 1];
              const MirLoanId published =
                  createLoan(MirLoanKind::Return, retainedLoan.source,
                             retainedLoan.access, *statement.value);
              output.loans[published - 1].storedDependency =
                  retainedLoan.storedDependency;
              (void)appendInstruction(
                  {.kind = MirInstructionKind::Borrow,
                   .hirValue = *statement.value,
                   .operands = {{.kind = MirOperandKind::Loan,
                                 .place = runtimePlace,
                                 .loan = borrow.loan,
                                 .type = value->info.type}},
                   .loan = published,
                   .info = value->info});
              (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                       .hirValue = *statement.value,
                                       .loan = borrow.loan});
              removeLoanFromState(borrow.loan, scopes, bindingLoans);
              output.loans[published - 1].escapes = true;
              borrow = {.kind = MirOperandKind::Loan,
                        .place = runtimePlace,
                        .loan = published,
                        .type = value->info.type};
            }
          } else if (referenceParameterEntry != 0) {
            const HirValue *value = findValue(*statement.value);
            // An upcast return designates the same referent as a base or
            // interface view of it.
            if (value == nullptr || output.returnType.arguments.size() != 1 ||
                (value->info.type != output.returnType.arguments.front() &&
                 !hasBase(value->info.type,
                          output.returnType.arguments.front()))) {
              valid = false;
            } else {
              // A read-only reference parameter's entry loan is seeded at the
              // body entry and has no producing instruction, so its runtime
              // address lives only in the parameter's pointer carrier. The
              // fallback below reclassifies the returned loan in place, which
              // would leave this loan a Return loan for the whole body with
              // nothing that ever names the address it publishes. Publish a
              // fresh Return loan from the entry loan's source and retire the
              // entry loan at the same instruction boundary, exactly as the
              // by-value carrier and nested-call cases above do.
              const MirLoan retainedLoan =
                  output.loans[referenceParameterEntry - 1];
              const MirPlaceId runtimeAlias =
                  referenceReturnRuntimeAlias(*statement.value, retainedLoan);
              // A local reference may share the parameter's coarse owner
              // authority while storing a call's different runtime address.
              // Publish that exact address, not the parameter pointer. The
              // verifier independently proves its unique initialization.
              const MirLoanId published =
                  createLoan(MirLoanKind::Return, retainedLoan.source,
                             retainedLoan.access, *statement.value);
              output.loans[published - 1].storedDependency =
                  retainedLoan.storedDependency;
              (void)appendInstruction(
                  {.kind = MirInstructionKind::Borrow,
                   .hirValue = *statement.value,
                   .operands = {{.kind = MirOperandKind::Loan,
                                 .place = runtimeAlias,
                                 .loan = referenceParameterEntry,
                                 .type = value->info.type}},
                   .loan = published,
                   .info = value->info});
              (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                       .hirValue = *statement.value,
                                       .loan = referenceParameterEntry});
              removeLoanFromState(referenceParameterEntry, scopes,
                                  bindingLoans);
              output.loans[published - 1].escapes = true;
              borrow = {.kind = MirOperandKind::Loan,
                        .place = runtimeAlias,
                        .loan = published,
                        .type = value->info.type};
            }
          } else if (function != nullptr &&
                     bodyReturnBorrowOrigin() != BorrowOriginKind::None &&
                     borrow.loan <= output.loans.size() &&
                     output.loans[borrow.loan - 1].kind ==
                         MirLoanKind::CallResult) {
            const MirPlaceId sourcePlace = output.loans[borrow.loan - 1].source;
            std::vector<MirLoanId> transientChain;
            std::unordered_set<MirLoanId> visited;
            MirLoanId transient = borrow.loan;
            while (transient != 0 && transient <= output.loans.size() &&
                   visited.insert(transient).second) {
              const MirLoan &candidate = output.loans[transient - 1];
              const bool restoredAliases =
                  function->restoresReferenceResult &&
                  mir_detail::restoredReferenceReturnWitness(output, candidate);
              // A call result's carriers may be local reference aliases of
              // its exact address. This builder has an unfinished CFG;
              // independent verification proves unique alias initialization
              // and dominance on the final CFG.
              const bool referenceAliases = std::all_of(
                  candidate.carriers.begin(), candidate.carriers.end(),
                  [&](HirBindingId id) {
                    const auto *alias = findBinding(id);
                    return alias != nullptr &&
                           alias->info.type.kind == SemanticType::Reference;
                  });
              if (candidate.kind != MirLoanKind::CallResult ||
                  (!candidate.carriers.empty() && !restoredAliases &&
                   !referenceAliases) ||
                  candidate.escapes || !loanIsActive(scopes, transient)) {
                break;
              }
              transientChain.push_back(transient);
              transient = candidate.parent;
            }
            MirLoanId summarizedEntry = 0;
            if (const std::optional<HirBindingId> summarized =
                    returnBorrowParameterBinding()) {
              const auto found = bindingLoans.find(*summarized);
              summarizedEntry = found == bindingLoans.end() ? 0 : found->second;
            }
            MirLoanId mappedEntry = summarizedEntry;
            if (!function->returnBorrowFields.empty() &&
                !function->returnBorrowFields.front().sourceFields.empty()) {
              const auto *entry = output.findLoan(summarizedEntry);
              if (entry != nullptr && entry->storedDependency != 0)
                mappedEntry = entry->storedDependency;
              if (function->returnBorrowOrigin == BorrowOriginKind::Receiver) {
                const auto *tail = output.findLoan(transient);
                const auto *origin =
                    tail ? output.findPlace(tail->source) : nullptr;
                if (tail != nullptr && tail->entry &&
                    tail->kind == MirLoanKind::Stored && origin != nullptr &&
                    origin->root == MirPlaceRootKind::This)
                  mappedEntry = tail->id;
              }
            }
            // A consuming extraction already transferred the summarized
            // parameter's entry group into this exact call result and retired
            // it; that transfer stands in for the entry at this return.
            MirLoanId transferredEntry = 0;
            if (summarizedEntry == 0 &&
                function->returnBorrowOrigin == BorrowOriginKind::Argument &&
                function->returnBorrowParameter <
                    function->parameterBindings.size()) {
              const auto transfers =
                  callResultTransferredFrom.find(borrow.loan);
              if (transfers != callResultTransferredFrom.end())
                for (const MirLoanId id : transfers->second) {
                  const MirLoan *entry = output.findLoan(id);
                  const MirPlace *root = entry == nullptr
                                             ? nullptr
                                             : output.findPlace(entry->source);
                  if (entry != nullptr && entry->entry &&
                      entry->kind == MirLoanKind::Stored && root != nullptr &&
                      root->root == MirPlaceRootKind::Binding &&
                      root->projections.empty() &&
                      root->binding == function->parameterBindings
                                           [function->returnBorrowParameter])
                    transferredEntry = id;
                }
            }
            // A local carrier's group relays the summarized entry's caller
            // authority to the referent its stored reference designates. The
            // group and then the entry end on the ordinary scope-exit
            // schedule, after the independent Return capability publishes.
            bool relayedEntry = false;
            if (const auto *relay = output.findLoan(transient);
                relay != nullptr && !transientChain.empty() &&
                relay->kind == MirLoanKind::Stored && !relay->entry &&
                !relay->escapes && relay->parent != 0 &&
                relay->parent == mappedEntry &&
                loanIsActive(scopes, relay->id)) {
              transient = relay->parent;
              relayedEntry = true;
            }
            const bool validTail =
                function->returnBorrowOrigin == BorrowOriginKind::Receiver ||
                        function->returnBorrowOrigin == BorrowOriginKind::Global
                    ? transient == 0 ||
                          (mappedEntry != 0 && transient == mappedEntry)
                    : (summarizedEntry != 0 || transferredEntry != 0) &&
                          (transient == mappedEntry ||
                           (transient == 0 &&
                            output.loans[borrow.loan - 1].access ==
                                AccessMode::ReadOnly &&
                            output.findPlace(sourcePlace) != nullptr &&
                            output.findPlace(sourcePlace)->root ==
                                MirPlaceRootKind::Binding &&
                            output.findPlace(sourcePlace)->binding ==
                                function->parameterBindings
                                    [function->returnBorrowParameter]));
            if (!validTail || transientChain.empty()) {
              valid = false;
            } else {
              const HirValue *value = findValue(*statement.value);
              if (value == nullptr || output.returnType.arguments.size() != 1 ||
                  value->info.type != output.returnType.arguments.front()) {
                valid = false;
              } else {
                const HirValueId producer =
                    output.loans[borrow.loan - 1].producedBy;
                const MirLoanId normalized =
                    createLoan(MirLoanKind::Return, sourcePlace,
                               function->returnBorrowAccess, producer);
                output.loans[normalized - 1].storedDependency =
                    output.loans[borrow.loan - 1].storedDependency;
                // A nested reference-returning call carries the exact referent
                // in its CallResult loan while sourcePlace names the owner used
                // for semantic conflict checks. Publish the former into the
                // Return loan before retiring the body-local chain.
                (void)appendInstruction(
                    {.kind = MirInstructionKind::Borrow,
                     .hirValue = producer,
                     .operands = {{.kind = MirOperandKind::Loan,
                                   .loan = borrow.loan,
                                   .type = value->info.type}},
                     .loan = normalized,
                     .info = value->info});
                for (const MirLoanId ended : transientChain) {
                  (void)appendInstruction(
                      {.kind = MirInstructionKind::EndBorrow,
                       .hirValue = producer,
                       .loan = ended});
                  removeLoanFromState(ended, scopes, bindingLoans);
                }
                if (summarizedEntry != 0 && !relayedEntry &&
                    output.loans[normalized - 1].storedDependency !=
                        summarizedEntry &&
                    !temporaryCarrierLoans().contains(summarizedEntry)) {
                  (void)appendInstruction(
                      {.kind = MirInstructionKind::EndBorrow,
                       .hirValue = *statement.value,
                       .loan = summarizedEntry});
                  removeLoanFromState(summarizedEntry, scopes, bindingLoans);
                }
                // A fresh borrowed receiver may still need this parameter
                // authority while destroying its shell. In that case the
                // ordinary temporary-drop/scope-exit schedule retires the
                // Stored child and then the parameter, after publication of
                // the independent external return capability.
                borrow = {.kind = MirOperandKind::Loan,
                          .loan = normalized,
                          .type = value->info.type};
              }
            }
          }
          // A returned reference may relay incoming authority through local
          // aliases and accessor results. Keep each producing loan intact and
          // publish its evaluated address only at the return boundary.
          const auto callerOwnedReferenceChild = [&](const MirLoan &candidate) {
            const std::optional<HirBindingId> summarized =
                returnBorrowParameterBinding();
            if (candidate.kind != MirLoanKind::Local)
              return false;
            std::unordered_set<MirLoanId> seen;
            const MirLoan *parent = output.findLoan(candidate.parent);
            while (parent != nullptr && seen.insert(parent->id).second &&
                   (parent->kind == MirLoanKind::Local ||
                    parent->kind == MirLoanKind::CallResult) &&
                   !parent->entry && !parent->escapes) {
              parent = output.findLoan(parent->parent);
            }
            if (!seen.empty() && parent == nullptr &&
                bodyReturnBorrowOrigin() == BorrowOriginKind::Receiver)
              return true;
            return summarized && parent != nullptr &&
                   parent->kind == MirLoanKind::Parameter && parent->entry &&
                   std::find(parent->carriers.begin(), parent->carriers.end(),
                             *summarized) != parent->carriers.end();
          };
          if ((output.loans[borrow.loan - 1].kind == MirLoanKind::CallResult ||
               output.loans[borrow.loan - 1].kind == MirLoanKind::Stored ||
               output.loans[borrow.loan - 1].referenceSelection ||
               callerOwnedReferenceChild(output.loans[borrow.loan - 1])) &&
              output.loans[borrow.loan - 1].producedBy != 0) {
            // A returned reference binding may alias the producing call's
            // own result loan, or the Stored loan of the carrier it was
            // extracted from. That call keeps its loan; the return publishes
            // a fresh Return loan from it, exactly as a nested
            // reference-returning call and a returned carrier do above, and
            // retires the body-local capability at this boundary.
            // A payload selection, or a payload borrow through a reference
            // parameter, likewise retains its local activation and tag proof;
            // changing that loan into an escaping Return would erase the
            // distinction between selection and publication, and would leave
            // its producing borrow reading a parent the Return loan lacks.
            const MirLoan source = output.loans[borrow.loan - 1];
            const HirValue *returned = findValue(*statement.value);
            MirPlaceId publicationSource = source.source;
            std::unordered_set<MirLoanId> seenParents;
            const MirLoan *ancestor = output.findLoan(source.parent);
            const bool nestedReference =
                source.kind == MirLoanKind::Local && source.parent != 0 &&
                !source.referenceSelection && callerOwnedReferenceChild(source);
            while (nestedReference && ancestor != nullptr &&
                   seenParents.insert(ancestor->id).second &&
                   (ancestor->kind == MirLoanKind::Local ||
                    ancestor->kind == MirLoanKind::CallResult) &&
                   !ancestor->entry && !ancestor->escapes) {
              // Local aliases may hide the caller-visible owner behind
              // binding slots. Preserve the outer origin separately from the
              // exact runtime address carried by the returned child loan.
              publicationSource = ancestor->source;
              ancestor = output.findLoan(ancestor->parent);
            }
            const MirLoanId published =
                createLoan(MirLoanKind::Return, publicationSource,
                           source.access, source.producedBy);
            output.loans[published - 1].storedDependency =
                source.storedDependency;
            output.loans[published - 1].additionalDependencies =
                source.additionalDependencies;
            output.loans[published - 1].designatedFields =
                source.designatedFields;
            output.loans[published - 1].escapes = true;
            (void)appendInstruction({.kind = MirInstructionKind::Borrow,
                                     .hirValue = source.producedBy,
                                     .operands = {{.kind = MirOperandKind::Loan,
                                                   .loan = borrow.loan,
                                                   .type = borrow.type}},
                                     .loan = published,
                                     .info = returned != nullptr
                                                 ? returned->info
                                                 : ExpressionInfo{}});
            (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                     .hirValue = source.producedBy,
                                     .loan = borrow.loan});
            removeLoanFromState(borrow.loan, scopes, bindingLoans);
            borrow = {.kind = MirOperandKind::Loan,
                      .loan = published,
                      .type = borrow.type};
            returnLoan = published;
          } else {
            MirLoan &loan = output.loans[borrow.loan - 1];
            loan.kind = MirLoanKind::Return;
            loan.escapes = true;
            // A Return loan publishes the body's dependency to its caller;
            // it is never a child of a body-local loan, which ends here. A
            // parent that was a stored owner authority becomes the stored
            // dependency.
            if (loan.parent != 0) {
              const MirLoan &parent = output.loans[loan.parent - 1];
              if (loan.storedDependency == 0 &&
                  parent.kind == MirLoanKind::Stored) {
                loan.storedDependency = loan.parent;
              }
              loan.parent = 0;
            }
            for (Scope &scope : scopes) {
              std::erase(scope.loans, borrow.loan);
            }
            returnLoan = borrow.loan;
          }
        }
        result = borrow;
      } else {
        result = valueOperand(*statement.value);
        const HirValue *value = findValue(*statement.value);
        // A known dependency-free active alternative has no authority to
        // export. Preserve its complete typed fact for independent program
        // verification rather than inventing a loan or inferring emptiness
        // from loanForValue returning zero.
        const bool completeEmpty =
            result && completeEmptyBorrowValue(*statement.value, *result,
                                               output.returnType);
        if (value != nullptr && value->info.traits.containsBorrowedState &&
            !completeEmpty) {
          MirLoanId retained = loanForValue(*statement.value);
          if (retained == 0 &&
              (output.returnType.kind == SemanticType::Class ||
               output.returnType.kind == SemanticType::StringView ||
               output.returnType.kind == SemanticType::CString) &&
              value->info.traits.copyable) {
            // A copied nested member, or a text view read through a checked
            // text reference field, has no separate local carrier binding.
            // Its caller-owned receiver/reference entry supplies the external
            // authority; the exact returned Load remains executable evidence.
            retained =
                storedDependencyForPlace(placeForValue(*statement.value));
          }
          if (retained != 0 && retained <= output.loans.size() &&
              output.returnType.kind == SemanticType::Class &&
              value->info.traits.copyable &&
              output.loans[retained - 1].kind != MirLoanKind::Stored &&
              output.loans[retained - 1].storedDependency != 0) {
            const MirLoanId dependency =
                output.loans[retained - 1].storedDependency;
            const MirLoan *entry = output.findLoan(dependency);
            if (entry != nullptr && entry->kind == MirLoanKind::Stored &&
                entry->entry && entry->access == AccessMode::ReadOnly) {
              // Publish the external owners of a copy made through a shell
              // reference. An existing Stored result instead keeps its own
              // physical referent and independent resource dependency: it may
              // be a freshly constructed adapter, not a copy of that resource.
              // Verification proves the exact Load and reference-field paths.
              retained = dependency;
            }
          }
          if (retained == 0 || retained > output.loans.size()) {
            valid = false;
          } else {
            // A borrow-carrying value owns two temporally distinct
            // capabilities at a return: its live body-local/entry loan, then
            // the dependency exported by the returned object.  Reclassifying
            // the original loan globally would make it a Return loan from the
            // start of the body and incorrectly forbid mutation of an opaque
            // by-value cursor before this point.  Publish a fresh return-only
            // loan from the live provenance, then retire the body capability
            // at this exact instruction boundary.
            const MirLoan retainedLoan = output.loans[retained - 1];
            const MirLoanId published =
                createLoan(MirLoanKind::Return, retainedLoan.source,
                           retainedLoan.access, *statement.value);
            output.loans[published - 1].storedDependency =
                retainedLoan.storedDependency;
            output.loans[published - 1].additionalDependencies =
                retainedLoan.additionalDependencies;
            output.loans[published - 1].designatedFields =
                retainedLoan.designatedFields;
            output.loans[published - 1].conditions = retainedLoan.conditions;
            if (retainedLoan.entry &&
                retainedLoan.kind == MirLoanKind::Stored &&
                retainedLoan.additionalDependencies.empty() &&
                output.returnType.kind == SemanticType::Class &&
                function != nullptr &&
                function->returnBorrowFields.size() == 1) {
              const auto &mapping = function->returnBorrowFields.front();
              if (mapping.sourceFields == retainedLoan.designatedFields &&
                  mapping.access == retainedLoan.access) {
                // The entry path is relative to the enclosing carrier; the
                // returned copy publishes the same referent at its own path.
                output.loans[published - 1].designatedFields =
                    mapping.resultFields;
              }
            }
            if (!retainedLoan.entry &&
                retainedLoan.kind == MirLoanKind::Stored &&
                retainedLoan.designatedFields.empty() &&
                retainedLoan.additionalDependencies.empty() &&
                output.returnType.kind == SemanticType::Class &&
                function != nullptr &&
                function->returnBorrowFields.size() == 1) {
              // Compact call results retain one concrete owner without a
              // field label. A wrapper may expose that same owner through
              // its own explicit result-field contract. Preserve the source
              // unchanged and attach the resolved destination field; program
              // verification independently checks both ends of the mapping.
              output.loans[published - 1].designatedFields =
                  function->returnBorrowFields.front().resultFields;
            }
            const MirValueId returnedValue =
                result && result->kind == MirOperandKind::Value ? result->value
                                                                : 0;
            const bool exactStoredCarrierEntry =
                retainedLoan.kind == MirLoanKind::Stored &&
                retainedLoan.entry && !retainedLoan.designatedFields.empty();
            const bool exactDynamicTextEntry =
                retainedLoan.kind == MirLoanKind::Stored &&
                retainedLoan.entry && retainedLoan.designatedFields.empty() &&
                (value->info.type.kind == SemanticType::StringView ||
                 value->info.type.kind == SemanticType::CString) &&
                value->info.type.kind == output.returnType.kind;
            if ((exactStoredCarrierEntry || exactDynamicTextEntry) &&
                returnedValue == 0) {
              valid = false;
            }
            (void)appendInstruction(
                {.kind = MirInstructionKind::Borrow,
                 .hirValue = *statement.value,
                 .operands = {{.kind = MirOperandKind::Loan,
                               .value = exactStoredCarrierEntry ||
                                                exactDynamicTextEntry
                                            ? returnedValue
                                            : 0,
                               .loan = retained,
                               .type = value->info.type}},
                 .loan = published,
                 .info = value->info});
            const MirPlace *retainedSource =
                output.findPlace(retainedLoan.source);
            const bool abstractCopy =
                exactStoredCarrierEntry &&
                retainedLoan.access == AccessMode::ReadOnly &&
                output.returnType.kind == SemanticType::Class &&
                value->info.traits.copyable && retainedSource != nullptr &&
                retainedSource->projections.empty() &&
                (retainedSource->root == MirPlaceRootKind::This ||
                 (retainedSource->root == MirPlaceRootKind::Binding &&
                  retainedSource->type.kind == SemanticType::Reference));
            const bool liveShells =
                abstractCopy &&
                std::any_of(output.loans.begin(), output.loans.end(),
                            [&](const MirLoan &candidate) {
                              return candidate.storedDependency == retained &&
                                     loanIsActive(scopes, candidate.id);
                            });
            if (!liveShells) {
              (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                       .hirValue = *statement.value,
                                       .loan = retained});
              removeLoanFromState(retained, scopes, bindingLoans);
            }
            // For an abstract shared copy, existing scope cleanup retires the
            // physical references before their stored dependency. Unlike a
            // shell-reference return, no original entry escapes with the copy.
            output.loans[published - 1].escapes = true;
            returnLoan = published;
          }
        }
      }
    }
    if (terminated()) {
      return;
    }
    if (shared && (returnLoan || !sharedExitEligible(incomingTemporaryDrops))) {
      // Evaluating the return expression changed what the exit may share:
      // it moved an owner, whose drop now depends on this path, or formed a
      // loan the return exports. Finish on the ordinary path.
      shared.reset();
    }
    if (shared) {
      // A carried value has no obligation, so there is no result ownership
      // to prepare or publish around the cleanup.
      if (shared->hasValue) {
        if (!result) {
          valid = false;
          return;
        }
        shared->stored = *result;
      }
      const MirPlaceId carrier = storeSharedExitValue(*shared);
      endReturnExpressionLoans(incomingScopes);
      emitTemporaryDrops(incomingTemporaryDrops, statement.value.value_or(0),
                         statement.id);
      if (!emitSharedExit(*shared, carrier)) {
        // The candidate scan promised a shareable drop.
        valid = false;
      }
      return;
    }
    const MirDropObligationId enclosingPendingReturnDrop = pendingReturnDrop;
    if (statement.value && result &&
        output.returnType.kind != SemanticType::Reference &&
        output.returnType.kind != SemanticType::Void) {
      prepareReturnOwnership(*statement.value, *result, statement.id);
    }
    endReturnExpressionLoans(incomingScopes);
    const MirLoanId enclosingPendingReturnLoan = pendingReturnLoan;
    pendingReturnLoan = returnLoan.value_or(0);
    emitTemporaryDrops(incomingTemporaryDrops, statement.value.value_or(0),
                       statement.id);
    emitScopeExit(0);
    pendingReturnLoan = enclosingPendingReturnLoan;
    publishReturnOwnership(statement.value.value_or(0), statement.id);
    pendingReturnDrop = enclosingPendingReturnDrop;
    terminate({.kind = MirTerminatorKind::Return,
               .hirStatement = statement.id,
               .value = std::move(result),
               .returnLoan = returnLoan});
  }

  void prepareReturnOwnership(HirValueId value, const MirOperand &result,
                              HirStatementId statement = 0) {
    const MirDropObligationId sourceDrop = dropObligationForValue(value);
    const bool fallibleReturnCleanup =
        std::any_of(temporaryDrops.begin(), temporaryDrops.end(),
                    [&](const auto &drop) {
                      return drop.obligation != sourceDrop &&
                             normalizedDestructorPropagation(drop.obligation) ==
                                 FailurePropagationKind::Destructor;
                    }) ||
        std::any_of(scopes.begin(), scopes.end(), [&](const auto &scope) {
          return std::any_of(
              scope.drops.begin(), scope.drops.end(), [&](const auto place) {
                return normalizedDestructorPropagation(
                           dropObligationForLocalPlace(output.findPlace(
                               place))) == FailurePropagationKind::Destructor;
              });
        });
    if (temporaryIsActive(sourceDrop) && result.kind == MirOperandKind::Value &&
        fallibleReturnCleanup) {
      const MirDropObligation source = *output.findDropObligation(sourceDrop);
      pendingReturnDrop = output.dropObligations.size() + 1;
      output.dropObligations.push_back(
          {.id = pendingReturnDrop,
           .constructionOrder = pendingReturnDrop,
           .kind = MirDropObligationKind::ReturnResult,
           .place = source.place,
           .value = source.value,
           .dropType = source.dropType});
      (void)appendInstruction(
          {.kind = MirInstructionKind::Lifecycle,
           .hirValue = value,
           .hirStatement = statement,
           .lifecycle = {{.kind = MirLifecycleEventKind::Reparent,
                          .source = sourceDrop,
                          .target = pendingReturnDrop}}});
      (void)removeTemporary(sourceDrop);
    } else {
      // Without a potentially failing continuation no rollback lifetime is
      // needed: transfer and final publication cannot be separated by
      // failure.
      emitTemporaryTransfer(value, statement);
    }
  }

  void publishReturnOwnership(HirValueId value, HirStatementId statement = 0) {
    if (pendingReturnDrop != 0) {
      (void)appendInstruction(
          {.kind = MirInstructionKind::Lifecycle,
           .hirValue = value,
           .hirStatement = statement,
           .lifecycle = {{.kind = MirLifecycleEventKind::TransferOut,
                          .source = pendingReturnDrop}}});
    }
  }

  [[nodiscard]] bool validateSourceProvenance() const {
    return std::all_of(output.places.begin(), output.places.end(),
                       [&](const MirPlace &place) {
                         return place.sourceValue == 0 ||
                                findValue(place.sourceValue) != nullptr;
                       }) &&
           std::all_of(output.values.begin(), output.values.end(),
                       [&](const MirValue &value) {
                         return value.sourceValue != 0 &&
                                findValue(value.sourceValue) != nullptr;
                       });
  }

  const HirProgram &program;
  MirVerificationResult verification;
  std::optional<std::pair<HirValueId, std::string>> firstRejection;
  const FailureMetadata &failureMetadata;
  const HirBody &source;
  // Every root of the source body's full expressions, gathered on first use.
  mutable std::optional<std::unordered_set<HirValueId>> fullExpressionRoots;
  std::unordered_set<HirValueId> borrowedTemporaryTransfers;
  MirLoanId pendingReturnLoan = 0;
  MirDropObligationId pendingReturnDrop = 0;
  std::vector<MirLoanId> constructorPublishedLoans;
  // A zero identity records a retained borrowed binding without a distinct
  // semantic endpoint annotation; an absent entry is not a retained binding.
  std::unordered_map<HirValueId, SemanticLoanId> retainedInitializerLoans;
  const MirDefinedFailureEffects &definedFailureEffects;
  bool implicitZeroReturn = false;
  const HirFunctionInstance *function = nullptr;
  const HirConstructorInstance *constructor = nullptr;
  const HirDestructorInstance *destructor = nullptr;
  const HirLambda *lambda = nullptr;
  // The already-lowered class record is the authority for in-class field
  // initialization that completed before a source constructor body begins.
  // Constructor MIR copies only the resulting active rollback descriptors;
  // it never re-executes or interprets the initializer body.
  const MirClassInstance *constructorOwner = nullptr;
  // The exact HIR class catalog distinguishes its storage bindings from
  // lexical locals evaluated inside a field's initializer expression.
  const HirClassInstance *fieldInitializerOwner = nullptr;
  std::unordered_set<HirBindingId> nonLexicalBindings;
  const HirProgramInitializationPlan *programInitialization = nullptr;
  MirProgramInitializationPlan *loweredProgramInitialization = nullptr;
  MirBody output;
  // Invoke continuations of `output`, recorded as each terminator is set.
  MirInvokeContinuations invokeContinuations;
  MirBlockId current = 0;
  ProgramInitializationStepId currentProgramInitializationStep = 0;
  MirInstructionId nextInstruction = 1;
  MirTemporaryId nextTemporary = 1;
  bool valid = true;
  // Failure cleanup recorded per failure edge, emitted when the body is
  // complete; see shareFailureCleanup.
  std::vector<PlannedFailureCleanup> plannedFailureCleanup;
  std::optional<std::size_t> planningFailureCleanup;
  // Shared normal-exit cleanup; see emitSharedExit. The exit count is the
  // body's returns, `?` propagations and its reachable end; one carrier
  // place per type holds returned values; each shared block is
  // registered under the exit state it continues from.
  std::size_t exitCount = 0;
  std::vector<std::pair<SemanticType, MirPlaceId>> exitCarriers;
  std::map<SharedExitKey, MirBlockId> sharedExitBlocks;
  bool exitSharingUsed = false;
  bool exitsJoined = false;
  std::unordered_map<HirValueId, const HirValue *> values;
  std::unordered_map<HirStatementId, const HirStatement *> statements;
  std::unordered_map<SemanticLoanId, HirStatementId>
      uniqueSemanticLoanEndpoints;
  std::unordered_map<HirBindingId, const HirBinding *> bindings;
  std::unordered_map<SymbolId, HirBindingId> localSymbols;
  std::unordered_map<SymbolId, std::size_t> lambdaCaptures;
  std::unordered_map<HirBindingId, MirPlaceId> bindingPlaces;
  std::unordered_set<HirValueId> ensuredThreadLocalValues;
  std::unordered_map<HirDropObligationId, MirDropObligationId> dropObligations;
  std::unordered_map<HirFullExpressionId, HirFullExpressionId>
      fullExpressionIds;
  std::unordered_map<HirFullExpressionId, std::vector<TemporaryDrop>>
      fullExpressionTemporaryBaselines;
  std::unordered_map<HirFullExpressionId, std::vector<Scope>>
      fullExpressionScopeBaselines;
  std::unordered_map<HirValueId, MirValueId> loweredValues;
  std::unordered_map<HirValueId, MirValueId> materializedValues;
  std::unordered_map<HirValueId, MirPlaceId> valuePlaces;
  // Class assignments whose result is consumed, and the explicit copy of the
  // committed destination that represents that result.
  std::unordered_set<HirValueId> assignmentSnapshotRequests;
  std::unordered_map<HirValueId, MirValueId> assignmentSnapshots;
  std::unordered_map<HirValueId, MirValueId> assignmentResults;
  std::unordered_set<HirValueId> assignmentDestinationRequests;
  std::unordered_map<HirValueId, MirPlaceId> assignmentDestinations;
  // Every extraction observes the same subject representation that selected
  // its arm, including mixed copied/borrowed bindings of passive enums.
  std::unordered_map<HirValueId, MirOperand> payloadSwitchSubjects;
  std::unordered_map<HirValueId, MirLoanId> valueLoans;
  std::unordered_map<HirValueId, HirValueId> forwardedBorrowValues;
  std::unordered_map<HirBindingId, MirLoanId> bindingLoans;
  std::unordered_set<HirBindingId> referenceAccessorBindings;
  std::unordered_map<MirLoanId, std::vector<MirLoanId>>
      callResultTransferredFrom;
  // Stored groups a consuming call transferred from one carrier while another
  // binding still carries them, so they stay active past the call.
  std::unordered_set<MirLoanId> sharedTransferredGroups;
  std::unordered_map<MirPlaceId, MirLoanId> packEntryLoans;
  std::unordered_map<SemanticLoanId, MirLoanId> semanticLoans;
  std::unordered_map<MirLoanId, HirStatementId> callTransferredLoanEndpoints;
  std::unordered_map<MirLoanId, HirStatementId>
      enclosingTransferredLoanEndpoints;
  std::unordered_map<SemanticLoanId, MirPlaceId> semanticLoanPlaces;
  std::unordered_set<HirValueId> emittedValues;
  std::unordered_set<HirValueId> checkedPlacePrefixes;
  std::vector<Scope> scopes;
  std::vector<ResultRegion> resultRegions;
  std::vector<TemporaryDrop> temporaryDrops;
  std::vector<BreakContext> breakContexts;
  std::vector<ContinueContext> continueContexts;
  std::unordered_map<HirStatementId, LoweredLoopShape> loweredLoops;
  struct ParkedRangeEndpoint {
    HirStatementId statement = 0;
    MirRangeEndpointParking parking;
  };
  std::optional<ParkedRangeEndpoint> parkedRangeEndpoint;
  std::unordered_map<HirStatementId, MirRangeEndpointParking>
      rangeEndpointParking;
};

thread_local bool MirLowerer::shareFailureCleanup_ = true;

MirBody MirLowerer::lowerBody(
    const HirProgram &program, const FailureMetadata &failureMetadata,
    const HirBody &body, MirBodyKind kind, SemanticType returnType,
    const std::vector<HirValueId> &prologueValues,
    const MirDefinedFailureEffects &definedFailureEffects, bool &valid,
    std::vector<MirLoweringError> &errors, std::size_t owner,
    bool implicitZeroReturn,
    const std::vector<HirConstructorInitializer> *initializers,
    const HirFunctionInstance *function,
    const HirConstructorInstance *constructor, const HirLambda *lambda,
    const HirProgramInitializationPlan *programInitialization,
    MirProgramInitializationPlan *loweredProgramInitialization,
    bool compilerTrusted, const MirClassInstance *constructorOwner,
    const HirDestructorInstance *destructor) {
  const auto lowerWith = [&](bool shareExitCleanup) {
    auto lowerer = std::make_unique<MirBodyLowerer>(
        program, failureMetadata, body, kind, returnType, definedFailureEffects,
        implicitZeroReturn, function, constructor, lambda,
        programInitialization, loweredProgramInitialization, constructorOwner,
        destructor);
    lowerer->shareFailureCleanupEnabled = shareFailureCleanup_;
    lowerer->shareExitCleanupEnabled = shareExitCleanup;
    MirBody lowered = lowerer->lower(prologueValues, initializers);
    return std::pair{std::move(lowerer), std::move(lowered)};
  };
  auto [lowerer, result] = lowerWith(shareExitCleanup_);
  if (!lowerer->isValid() && lowerer->usedExitSharing()) {
    // Should a body's shared exit schedule not verify, lower the body again
    // with each exit destroying its own owners, its established form.
    ++cleanupSharingStatistics_.exitSharingBodyFallbacks;
    cleanupSharingStatistics_.lastExitSharingBodyFallback =
        "body kind " + std::to_string(static_cast<int>(kind)) + " owner " +
        std::to_string(owner) +
        (function != nullptr && function->source != nullptr
             ? " (" + std::string(function->source->name().lexeme) + ")"
             : std::string()) +
        ": " +
        (lowerer->rejection() ? lowerer->rejection()->second
         : !lowerer->proof().errors.empty()
             ? lowerer->proof().errors.front().message
             : std::string("rejected before verification"));
    auto unshared = lowerWith(false);
    lowerer = std::move(unshared.first);
    result = std::move(unshared.second);
  }
  if (lowerer->joinedExits()) {
    ++cleanupSharingStatistics_.exitSharingBodies;
  }
  result.compilerTrustedSynchronization =
      compilerTrusted &&
      std::any_of(result.blocks.begin(), result.blocks.end(),
                  [](const MirBlock &block) {
                    return std::any_of(
                        block.instructions.begin(), block.instructions.end(),
                        [](const MirInstruction &instruction) {
                          return instruction.synchronization.kind !=
                                 SynchronizationOperationKind::None;
                        });
                  });
  if (!lowerer->isValid()) {
    const auto sourceFor = [&](HirValueId value) -> std::optional<SourceSpan> {
      if (value == 0) {
        if (function && function->source)
          return function->source->extent();
        if (constructor && constructor->source)
          return constructor->source->extent();
        if (destructor && destructor->source)
          return destructor->source->extent();
        if (lambda && lambda->source)
          return tokenSpan(lambda->source->bracket());
      }
      std::optional<SourceSpan> selected;
      for (const auto &statement : body.statements) {
        if (!statement.source || !statement.source->extent())
          continue;
        bool contains = false;
        std::vector<HirValueId> pending;
        if (statement.value)
          pending.push_back(*statement.value);
        if (statement.condition)
          pending.push_back(*statement.condition);
        std::unordered_set<HirValueId> visited;
        while (!pending.empty() && !contains) {
          const auto next = pending.back();
          pending.pop_back();
          if (!visited.insert(next).second)
            continue;
          contains = next == value;
          if (const auto *expression = body.findValue(next))
            pending.insert(pending.end(), expression->operands.begin(),
                           expression->operands.end());
        }
        const auto &span = *statement.source->extent();
        if (contains && (!selected || span.end - span.start <
                                          selected->end - selected->start))
          selected = span;
      }
      return selected;
    };
    if (const auto &rejection = lowerer->rejection(); rejection)
      errors.push_back({.body = {kind, owner},
                        .source = sourceFor(rejection->first),
                        .message = rejection->second});
    for (const auto &error : lowerer->proof().errors) {
      HirValueId value = 0;
      std::optional<SourceSpan> anchor;
      if (const auto *block = result.findBlock(error.block))
        for (const auto &instruction : block->instructions)
          if (instruction.id == error.instruction) {
            value = instruction.hirValue != 0 ? instruction.hirValue
                                              : instruction.callSite;
            if (const auto *statement =
                    body.findStatement(instruction.hirStatement);
                statement && statement->source)
              anchor = statement->source->extent();
          }
      errors.push_back({.body = {kind, owner},
                        .block = error.block,
                        .instruction = error.instruction,
                        .source = anchor ? anchor : sourceFor(value),
                        .message = error.message});
    }
    if (!lowerer->rejection() && lowerer->proof().valid())
      errors.push_back({.body = {kind, owner},
                        .source = sourceFor(0),
                        .message = "body lowering rejected its checked HIR "
                                   "before program initialization"});
  }
  valid = valid && lowerer->isValid();
  return result;
}

} // namespace lang
