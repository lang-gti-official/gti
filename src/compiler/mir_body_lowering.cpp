#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_lowering.h"
#include "gti/mir_verifier.h"
#include "mir_verification_queries.h"
#include "mir_verification_support.h"

namespace lang {

namespace {

[[nodiscard]] bool isMirFixedWidthIntegerType(const SemanticType &type) {
  return type.kind == SemanticType::Int8 || type.kind == SemanticType::Int16 ||
         type.kind == SemanticType::Int32 || type.kind == SemanticType::Int64 ||
         type.kind == SemanticType::UInt8 ||
         type.kind == SemanticType::UInt16 ||
         type.kind == SemanticType::UInt32 || type.kind == SemanticType::UInt64;
}

[[nodiscard]] std::optional<SemanticType>
mirFallibleBorrowReferent(const SemanticType &type) {
  if (type.kind != SemanticType::Expected || type.arguments.size() != 2 ||
      type.arguments.front().kind != SemanticType::Reference ||
      type.arguments.front().referenceAccess != AccessMode::ReadOnly ||
      type.arguments.front().arguments.size() != 1) {
    return std::nullopt;
  }
  return type.arguments.front().arguments.front();
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
        constructor(constructor), destructor(destructor),
        constructorOwner(constructorOwner),
        programInitialization(programInitialization),
        loweredProgramInitialization(loweredProgramInitialization) {
    output.kind = kind;
    output.hasCallerLocation = function != nullptr && function->tracksCaller;
    output.placeDomain = source.placeDomain;
    output.returnType = std::move(returnType);
    for (const HirValue &value : source.values) {
      values.emplace(value.id, &value);
      if (value.programConstantSubstitution) {
        if (!value.constant ||
            std::holds_alternative<ConstantCheckedIntegerResult>(
                *value.constant)) {
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
        if (binding != nullptr && binding->info.retainedLoan != 0)
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
        // rollback or stay inactive, so they never transfer unarmed.
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
          for (const HirValueId argument : initializer.arguments) {
            (void)emitValue(argument);
          }
          stageBaseInitializer(initializer, initializerIndex);
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
               (initializer.targetType.kind == SemanticType::Enum &&
                std::any_of(program.enumDeclarations().begin(),
                            program.enumDeclarations().end(),
                            [&](const HirEnum &enumeration) {
                              return enumeration.declaration ==
                                         initializer.targetType.enumId &&
                                     !enumeration.payload;
                            }))) &&
              (field->info.traits.drop == DropKind::Trivial ||
               ((initializer.targetType.kind == SemanticType::Class ||
                 initializer.targetType.kind == SemanticType::Array) &&
                !field->requiresActiveCleanup)) &&
              (!field->info.traits.containsBorrowedState ||
               (constructor != nullptr &&
                constructor->borrowOrigin == BorrowOriginKind::Argument &&
                initializer.targetType.kind == SemanticType::Class));
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
            appendReparentOrTypedTransfer(initialize, owningStageSource,
                                          rollback);
            (void)removeTemporary(owningStageSource);
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
                     initializer.targetType.kind == SemanticType::Class &&
                     initializer.arguments.size() == 1) {
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
    rebuildMirReachability(output);
    valid = rebuildMirValueUses(output) && valid;
    valid = validateSourceProvenance() && valid;
    const MirVerificationResult verification = verifyMirBody(output);
    valid = verification.valid() && valid;
    return std::move(output);
  }

  [[nodiscard]] bool isValid() const { return valid; }

private:
  struct Scope {
    std::vector<MirPlaceId> drops;
    std::vector<MirLoanId> loans;
  };

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

  void stageBaseInitializer(const HirConstructorInitializer &initializer,
                            std::size_t initializerIndex) {
    // The first base-failure family has no constructor parameters or owning
    // derived fields. In particular there are no completed default fields at
    // entry that could be incorrectly unwound before the base has begun.
    if (constructor == nullptr || constructorOwner == nullptr ||
        constructorUnarmedTransfer || !constructor->parameterTypes.empty() ||
        constructorOwner->destructor ||
        constructorOwner->requiresActiveDropState ||
        initializer.kind != ConstructorInitializerTargetKind::Base ||
        !initializer.constructorTarget || !initializer.arguments.empty() ||
        initializer.generatedDefault ||
        std::any_of(constructorOwner->declaredFields.begin(),
                    constructorOwner->declaredFields.end(),
                    [](const MirClassFieldInfo &field) {
                      return field.dropKind != DropKind::Trivial ||
                             field.requiresActiveCleanup;
                    }) ||
        normalizedConstructorPropagation(FailurePropagationKind::Constructor,
                                         initializer.constructorTarget) !=
            FailurePropagationKind::Constructor) {
      return;
    }
    const auto base = std::find_if(
        program.classInstances().begin(), program.classInstances().end(),
        [&](const HirClassInstance &candidate) {
          return candidate.id == initializer.base &&
                 candidate.type == initializer.targetType;
        });
    if (base == program.classInstances().end() || base->destructor ||
        base->requiresActiveDropState) {
      return;
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
    (void)appendInstruction(
        {.kind = MirInstructionKind::InitializeBase,
         .constructorInitializer = initializerIndex + 1,
         .successResultDrop = rollback,
         .destination = destination,
         .definedFailure = {.propagation = FailurePropagationKind::Constructor},
         .constructorTarget = initializer.constructorTarget,
         .info = {.type = base->type,
                  .category = ValueCategory::Place,
                  .access = AccessMode::Mutable,
                  .traits = traits}});
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

  struct PreparedCallArgumentFailure {
    HirValueId argument = 0;
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

  [[nodiscard]] FailurePropagationKind
  normalizedDestructorPropagation(MirDropObligationId obligation) const {
    const MirBlock *block = output.findBlock(current);
    if (block != nullptr && mirDropFollowsDefiniteMove(*block, obligation)) {
      return FailurePropagationKind::None;
    }
    const MirDropObligation *drop = output.findDropObligation(obligation);
    if (drop == nullptr) {
      return FailurePropagationKind::None;
    }
    if (mirDropHasPolymorphicCleanup(drop->dropType))
      return FailurePropagationKind::Destructor;
    for (const auto target : mirDropDestructorTargets(drop->dropType)) {
      if (target == 0 || target > definedFailureEffects.destructors.size() ||
          definedFailureEffects.destructors[target - 1]) {
        return FailurePropagationKind::Destructor;
      }
    }
    return FailurePropagationKind::None;
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
          [&](const auto &candidate) {
            return candidate.declaration == type.enumId;
          });
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
    if ((type.kind == SemanticType::Expected && type.arguments.size() == 2) ||
        (type.kind == SemanticType::Unexpected && type.arguments.size() == 1)) {
      MirDropType result{.type = type};
      for (const auto &alternative : type.arguments) {
        auto child = uniqueCleanupType(alternative, visiting);
        if (!child) {
          const bool passive =
              alternative == SemanticType::Void ||
              (alternative.kind >= SemanticType::Int8 &&
               alternative.kind <= SemanticType::Char) ||
              alternative.kind == SemanticType::RawPointer ||
              alternative.kind == SemanticType::NativeFunction ||
              alternative.kind == SemanticType::NullPtr ||
              alternative.kind == SemanticType::CString ||
              alternative.kind == SemanticType::StringView ||
              (alternative.kind == SemanticType::Enum &&
               std::none_of(program.enumDeclarations().begin(),
                            program.enumDeclarations().end(),
                            [&](const auto &item) {
                              return item.declaration == alternative.enumId &&
                                     item.payload;
                            }));
          if (passive) {
            child = MirDropType{.type = alternative};
          } else {
            const auto owner = std::find_if(
                program.classInstances().begin(),
                program.classInstances().end(), [&](const auto &candidate) {
                  return candidate.type == alternative;
                });
            if (alternative.kind != SemanticType::Class ||
                owner == program.classInstances().end() ||
                owner->requiresActiveCleanup)
              return std::nullopt;
            child =
                MirDropType{.type = alternative, .classInstance = owner->id};
          }
        }
        result.requiresActiveCleanup |= child->requiresActiveCleanup;
        result.aggregate.push_back(*child);
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
    if ((type.kind == SemanticType::UniqueOwner ||
         type.kind == SemanticType::PrefixStorage) &&
        type.arguments.size() == 1) {
      const auto &value = type.arguments.front();
      if ((value.kind >= SemanticType::Int8 &&
           value.kind <= SemanticType::Char) ||
          value.kind == SemanticType::RawPointer ||
          value.kind == SemanticType::NativeFunction ||
          value.kind == SemanticType::NullPtr ||
          value.kind == SemanticType::CString ||
          value.kind == SemanticType::StringView ||
          (value.kind == SemanticType::Enum &&
           std::none_of(
               program.enumDeclarations().begin(),
               program.enumDeclarations().end(), [&](const auto &item) {
                 return item.declaration == value.enumId && item.payload;
               }))) {
        return MirDropType{.type = type, .requiresActiveCleanup = true};
      }
      if (type.kind == SemanticType::PrefixStorage) {
        auto element = uniqueCleanupType(value, visiting);
        if (!element)
          return std::nullopt;
        return MirDropType{.type = type,
                           .aggregate = {*element},
                           .requiresActiveCleanup = true};
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
      if (!payload || !payload->requiresActiveCleanup)
        return std::nullopt;
      return MirDropType{.type = type,
                         .classInstance = owner->id,
                         .aggregate = {*payload},
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
          [&](const HirEnum &candidate) {
            return candidate.declaration == type.enumId;
          });
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
    const HirValue *call = findValue(callSite);
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
    (void)appendInstruction({.kind = MirInstructionKind::AllocateObject,
                             .hirValue = call.id,
                             .successResultDrop = id,
                             .destination = place,
                             .definedFailure = std::move(failure),
                             .objectAllocation = id,
                             .info = call.info});
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
    }
  }

  void
  appendFailureCleanupBoundary(std::vector<MirDropObligationId> obligations) {
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
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
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
    const auto retained = failureCleanup ? temporaryCarrierLoans(retired)
                                         : retainedReborrowAncestors(retired);
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

  [[nodiscard]] std::unordered_set<MirLoanId>
  lifecycleRetainedLoans(const std::vector<MirLoanId> &ended) const {
    std::unordered_set<MirLoanId> retained;
    std::vector<MirLoanId> pending;
    for (const Scope &scope : scopes)
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
      }
    }
    return retained;
  }

  void endScopeLoansBeforeCleanup(const Scope &scope,
                                  std::vector<MirLoanId> &ended,
                                  bool failureCleanup) {
    const auto retained = lifecycleRetainedLoans(ended);
    for (const auto id : childFirstMirLoans(scope.loans)) {
      const auto *returned = output.findLoan(pendingReturnLoan);
      if (containsLoan(ended, id) || retained.contains(id) ||
          containsLoan(constructorPublishedLoans, id) ||
          (!failureCleanup && returned != nullptr &&
           returned->storedDependency == id))
        continue;
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
      const MirLoanId lifecycleLoan =
          place->root == MirPlaceRootKind::Binding
              ? lifecycleLoanForBinding(place->binding)
              : 0;
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
    if (failedLoan && failedLoan->lifecycleBound && !failedLoan->entry &&
        !failedLoan->escapes && failedProducer->result &&
        failedLoan->producedBy == failedProducer->hirValue &&
        !containsLoan(failureEndedLoans, failedLoan->id)) {
      (void)appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = failedLoan->id});
      failureEndedLoans.push_back(failedLoan->id);
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
    if (output.kind == MirBodyKind::Constructor ||
        output.kind == MirBodyKind::FieldInitializers || !hasLifecycleLoans) {
      emitOrderedFailureDrain(consumedDrops, failureEndedLoans);
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
    for (const auto loan : childFirstMirLoans(constructorPublishedLoans)) {
      if (!containsLoan(failureEndedLoans, loan)) {
        (void)appendInstruction(
            {.kind = MirInstructionKind::EndBorrow, .loan = loan});
      }
    }
    terminate({.kind = MirTerminatorKind::PropagateFailure,
               .failureRecord = failureRecord});
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
                          const std::vector<MirLoanId> &endedLoans) {
    const auto retained = temporaryCarrierLoans(consumedDrops);
    auto allEnded = endedLoans;
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      for (auto loan = scope->loans.rbegin(); loan != scope->loans.rend();
           ++loan) {
        const MirLoan *record = output.findLoan(*loan);
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
      const MirLoanId lifecycleLoan =
          place->root == MirPlaceRootKind::Binding
              ? lifecycleLoanForBinding(place->binding)
              : 0;
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

  MirInstructionId appendNormalDrop(
      MirDropObligationId obligation, MirPlaceId destination,
      ExpressionInfo info, std::vector<MirLifecycleEvent> lifecycle,
      const std::vector<MirDropObligationId> &consumedDrops,
      const std::vector<MirLoanId> &endedLoans, HirValueId hirValue = 0,
      HirStatementId hirStatement = 0, std::size_t constructorInitializer = 0,
      MirLoanId producerEndedLoan = 0,
      std::vector<MirLoanId> producerEndedLoans = {}) {
    MirInstruction drop{.kind = MirInstructionKind::Drop,
                        .hirValue = hirValue,
                        .hirStatement = hirStatement,
                        .constructorInitializer = constructorInitializer,
                        .destination = destination,
                        .info = std::move(info),
                        .lifecycle = std::move(lifecycle)};
    drop.definedFailure.propagation =
        normalizedDestructorPropagation(obligation);
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
    return value != 0 &&
           std::any_of(source.fullExpressions.begin(),
                       source.fullExpressions.end(),
                       [&](const HirFullExpression &expression) {
                         return std::find(expression.roots.begin(),
                                          expression.roots.end(),
                                          value) != expression.roots.end();
                       });
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
    MirBlock *block = currentBlock();
    if (block == nullptr || terminated()) {
      return 0;
    }
    if (inheritSourceMetadata && instruction.hirValue != 0) {
      if (const HirValue *source = findValue(instruction.hirValue)) {
        // Object lifetime manipulation is one explicit call, not an assertion
        // by its surrounding full-expression/lifecycle bookkeeping markers.
        if (source->unsafeOperation != UnsafeOperationKind::None &&
            (source->unsafeOperation != UnsafeOperationKind::ObjectLifetime ||
             instruction.kind == MirInstructionKind::Call)) {
          instruction.unsafeOperation = source->unsafeOperation;
        }
        if (source->ownership) {
          instruction.ownership = source->ownership;
        }
      }
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
             std::any_of(place->projections.begin(), place->projections.end(),
                         [](const MirPlaceProjection &projection) {
                           return projection.kind ==
                                      MirProjectionKind::RawDereference ||
                                  projection.kind ==
                                      MirProjectionKind::RawIndex;
                         });
    };
    instruction.rawMemoryAccess =
        (instruction.destination && rawPlace(*instruction.destination)) ||
        (instruction.receiver && instruction.receiver->place != 0 &&
         rawPlace(instruction.receiver->place)) ||
        std::any_of(instruction.operands.begin(), instruction.operands.end(),
                    [&](const MirOperand &operand) {
                      return operand.kind != MirOperandKind::Address &&
                             operand.place != 0 && rawPlace(operand.place);
                    });
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
    const bool fullExpressionRoot = isFullExpressionRoot(appended.hirValue);
    const bool preparedCallArgumentRoot =
        preparedCallArgumentFailure &&
        preparedCallArgumentFailure->argument == appended.hirValue;
    const MirFailureControlFlowPosition failurePosition =
        fullExpressionRoot ? MirFailureControlFlowPosition::FullExpressionRoot
        : preparedCallArgumentRoot
            ? MirFailureControlFlowPosition::PreparedCallArgumentRoot
            : MirFailureControlFlowPosition::None;
    if (routeFailureControlFlow && routeFailureEdgesHere() &&
        requiresMirFailureControlFlow(appended, failurePosition)) {
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
    if (value.kind == HirValueKind::Move && !value.operands.empty()) {
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
                             consumedDrops, {}, hirValue, hirStatement,
                             constructorInitializer, 0, endedLoans);
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

  MirValueId appendValue(HirValueId sourceValue, ExpressionInfo info) {
    const MirValueId id = output.values.size() + 1;
    output.values.push_back(
        {.id = id, .sourceValue = sourceValue, .info = std::move(info)});
    return id;
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
    const MirPlaceId place =
        appendPlace({.root = MirPlaceRootKind::Binding,
                     .binding = id,
                     .symbol = binding->info.symbol,
                     .type = binding->info.type,
                     .access = binding->info.access,
                     .traits = binding->info.traits,
                     .key = std::move(key),
                     .initiallyAvailable = binding->parameter != nullptr});
    bindingPlaces.emplace(id, place);
    return place;
  }

  [[nodiscard]] MirPlaceId placeForLoan(const HirLoan &loan,
                                        const HirValue *resultValue = nullptr) {
    if (const auto found = semanticLoanPlaces.find(loan.semanticLoan);
        found != semanticLoanPlaces.end()) {
      return found->second;
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

    const MirPlaceId place = appendPlace(std::move(lowered));
    semanticLoanPlaces.emplace(loan.semanticLoan, place);
    return place;
  }

  [[nodiscard]] MirPlaceId placeForBorrowOrigin(const BorrowOriginPlace &origin,
                                                const HirValue &value) {
    if (!origin.valid()) {
      return 0;
    }
    const std::optional<SemanticType> fallibleReferent =
        mirFallibleBorrowReferent(value.info.type);
    const SemanticType originType =
        fallibleReferent ? *fallibleReferent : value.info.type;
    MirPlace lowered{.root = MirPlaceRootKind::Symbol,
                     .symbol = origin.root,
                     .type = originType,
                     .access = fallibleReferent ? AccessMode::ReadOnly
                                                : value.info.access,
                     .traits = fallibleReferent ? semanticTraits(originType)
                                                : value.info.traits,
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
    if (!lowered.designatedFields.empty() &&
        lowered.designatedFields != loan.designatedFields) {
      valid = false;
      return;
    }
    lowered.designatedFields = loan.designatedFields;
    const auto previousDependencies = lowered.additionalDependencies;
    std::erase_if(lowered.additionalDependencies, [](const auto &dependency) {
      return dependency.source == 0;
    });
    if (!loan.entry && !loan.designatedFields.empty())
      lowered.additionalDependencies.clear();
    for (const auto &dependency : loan.additionalDependencies) {
      if (!dependency.designatedFields.empty()) {
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
          if (concreteSource == 0) {
            valid = false;
            return;
          }
        }
        lowered.additionalDependencies.push_back(
            {.source = concreteSource,
             .designatedFields = dependency.designatedFields,
             .access = dependency.access});
      }
    }
    if (lowered.access == AccessMode::ReadOnly &&
        loan.access == AccessMode::Mutable) {
      valid = false;
      return;
    }
    for (const HirBindingId carrier : loan.carriers) {
      if (std::find(lowered.carriers.begin(), lowered.carriers.end(),
                    carrier) == lowered.carriers.end()) {
        lowered.carriers.push_back(carrier);
      }
    }
    if (binding != 0 &&
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

  [[nodiscard]] MirPlaceId rawPointerPlace(HirValueId pointer,
                                           const HirValue &value,
                                           MirProjectionKind projection,
                                           MirValueId index = 0) {
    (void)valueOperand(pointer);
    MirPlaceId place = appendPlace({.root = MirPlaceRootKind::Value,
                                    .value = mirValueFor(pointer),
                                    .type = value.info.type,
                                    .access = value.info.access,
                                    .traits = value.info.traits,
                                    .sourceValue = value.id});
    output.places[place - 1].projections.push_back(
        {.kind = projection, .index = index});
    return place;
  }

  [[nodiscard]] MirPlaceId placeForValue(HirValueId id) {
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
        // Load read its own result before definition.
        if (place == 0) {
          const HirValue *receiver = findValue(value->operands.front());
          if (receiver != nullptr &&
              receiver->info.category == ValueCategory::Value &&
              receiver->info.type.kind == SemanticType::Class) {
            place = appendPlace({.root = MirPlaceRootKind::Value,
                                 .value = mirValueFor(*receiver),
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
        (void)valueOperand(value->operands[1]);
        place = rawPointerPlace(value->operands[0], *value,
                                MirProjectionKind::RawIndex,
                                mirValueFor(value->operands[1]));
      } else if (value->operands.size() >= 2) {
        place = clonePlace(placeForValue(value->operands[0]), *value);
        if (place != 0) {
          output.places[place - 1].projections.push_back(
              {.kind = MirProjectionKind::Index,
               .index = mirValueFor(value->operands[1])});
        }
      }
      break;
    case HirValueKind::Call:
    case HirValueKind::ExpectedPropagation:
    case HirValueKind::Conditional:
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
    switch (value.kind) {
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
      return appendPlace({.root = MirPlaceRootKind::Symbol,
                          .symbol = value.symbol,
                          .type = value.info.type,
                          .access = value.info.access,
                          .traits = value.info.traits,
                          .sourceValue = value.id});
    }
    case HirValueKind::MemberSet:
      if (!value.operands.empty()) {
        MirPlaceId place =
            value.unsafeOperation == UnsafeOperationKind::RawMember
                ? rawPointerPlace(value.operands[0], value,
                                  MirProjectionKind::RawDereference)
                : clonePlace(placeForValue(value.operands[0]), value);
        if (place != 0) {
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
          (void)valueOperand(value.operands[1]);
          place = rawPointerPlace(value.operands[0], value,
                                  MirProjectionKind::RawIndex,
                                  mirValueFor(value.operands[1]));
        } else {
          place = clonePlace(placeForValue(value.operands[0]), value);
        }
        if (place != 0) {
          if (value.unsafeOperation != UnsafeOperationKind::RawIndex) {
            output.places[place - 1].projections.push_back(
                {.kind = MirProjectionKind::Index,
                 .index = mirValueFor(value.operands[1])});
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
    if (!literal) {
      valid = false;
      return;
    }
    ExpressionInfo materializedInfo = value.info;
    materializedInfo.category = ValueCategory::Value;
    materializedInfo.access = AccessMode::ReadOnly;
    const auto *integer = std::get_if<ConstantInteger>(&*value.constant);
    const auto *enumerator = std::get_if<ConstantEnum>(&*value.constant);
    const bool enumeration = value.info.type.kind == SemanticType::Enum;
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
    if (value->kind == HirValueKind::Move && !value->operands.empty()) {
      emitPlaceDependencies(value->operands.front());
      MirPlaceId sourcePlace = placeForValue(value->operands.front());
      const HirValue *source = findValue(value->operands.front());
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
    if (value->info.category == ValueCategory::Place &&
        isPlaceExpression(value->kind)) {
      emitPlaceDependencies(id);
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
                               .definedFailure = localDefinedFailure(value),
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
    const MirPlaceId place = placeForValue(id);
    if (place == 0) {
      valid = false;
    }
    return {.kind = MirOperandKind::Address,
            .place = place,
            .type = value->info.type};
  }

  [[nodiscard]] static bool isPlaceExpression(HirValueKind kind) {
    switch (kind) {
    case HirValueKind::Call:
    case HirValueKind::ExpectedPropagation:
    case HirValueKind::Conditional:
    case HirValueKind::Grouping:
    case HirValueKind::Index:
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

  void emitPlaceDependencies(HirValueId id) {
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      valid = false;
      return;
    }
    if (isCallLike(*value) ||
        value->kind == HirValueKind::ExpectedPropagation ||
        value->kind == HirValueKind::Conditional) {
      emitValue(id);
      return;
    }
    switch (value->kind) {
    case HirValueKind::Binary:
      if (value->operation == TokenKind::COMMA && !value->operands.empty()) {
        for (auto operand = value->operands.begin();
             operand != std::prev(value->operands.end()); ++operand) {
          (void)valueOperand(*operand);
        }
        emitPlaceDependencies(value->operands.back());
      }
      break;
    case HirValueKind::Grouping:
      if (!value->operands.empty()) {
        emitPlaceDependencies(value->operands.front());
      }
      break;
    case HirValueKind::MemberAccess:
      if (!value->operands.empty()) {
        if (value->unsafeOperation == UnsafeOperationKind::RawMember) {
          (void)valueOperand(value->operands.front());
        } else {
          emitPlaceDependencies(value->operands.front());
        }
      }
      break;
    case HirValueKind::Index:
      if (!value->operands.empty()) {
        if (value->unsafeOperation == UnsafeOperationKind::RawIndex) {
          (void)valueOperand(value->operands.front());
        } else {
          emitPlaceDependencies(value->operands.front());
        }
      }
      if (value->operands.size() >= 2) {
        (void)valueOperand(value->operands[1]);
      }
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

  [[nodiscard]] MirPlaceId borrowedSourcePlace(HirValueId id) const {
    const MirLoanId loan = loanForValue(id);
    const MirLoan *resolved = output.findLoan(loan);
    return resolved == nullptr ? 0 : resolved->source;
  }

  [[nodiscard]] MirOperand argumentOperand(HirValueId id,
                                           const SemanticType &parameter) {
    if (parameter.kind != SemanticType::Reference) {
      return valueOperand(id);
    }
    emitPlaceDependencies(id);
    if (const MirLoanId existing = loanForValue(id); existing != 0) {
      // A borrow-carrying value (for example a read-only stored-reference
      // iterator) already has an owner loan, but a reference parameter needs
      // the runtime address of the carrier object rather than the address of
      // that carrier's referent.  Retain the owner loan as provenance and,
      // when the HIR place is an exact referent-typed place, carry that place
      // alongside it for backend publication.
      MirPlaceId runtimePlace = placeForValue(id);
      const MirPlace *resolved = output.findPlace(runtimePlace);
      if (parameter.arguments.size() != 1 || resolved == nullptr ||
          resolved->type != parameter.arguments.front()) {
        runtimePlace = 0;
      }
      if (const MirLoan *loan = output.findLoan(existing);
          loan != nullptr && loan->kind == MirLoanKind::Stored &&
          runtimePlace != 0 && resolved->type.kind == SemanticType::Class &&
          resolved->traits.containsBorrowedState) {
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
              .place = runtimePlace,
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
    emitPlaceDependencies(id);
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
        argumentCount = call->arguments().size();
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
    return value.functionTarget ? receiverAccess(*value.functionTarget)
                                : AccessMode::ReadOnly;
  }

  [[nodiscard]] bool isExpectedPropagationExtraction(HirValueId valueId) const {
    return std::any_of(
        source.values.begin(), source.values.end(),
        [&](const HirValue &candidate) {
          return candidate.expectedPropagation &&
                 (candidate.expectedPropagation->successExtraction == valueId ||
                  candidate.expectedPropagation->errorExtraction == valueId);
        });
  }

  [[nodiscard]] MirOperand temporaryBorrowOperand(
      HirValueId id, AccessMode access, const SemanticType &stagedType,
      bool allowExpected = false, bool allowBorrowedCarrier = false) {
    const HirValue *temporary = findValue(id);
    if (temporary == nullptr ||
        (temporary->info.type.kind != SemanticType::Class &&
         (!allowExpected ||
          temporary->info.type.kind != SemanticType::Expected)) ||
        temporary->info.category != ValueCategory::Value ||
        (temporary->info.traits.containsBorrowedState &&
         !allowBorrowedCarrier)) {
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
    // A conditional result's lifetime is rooted in its join temporary. Its
    // final Move is a transport identity, not a second materialization.
    const bool conditionalResult =
        temporary->kind == HirValueKind::Conditional &&
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
         !(mirFallibleBorrowReferent(temporary->info.type) &&
           loanForValue(id) != 0) &&
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
    if (receiver->kind == HirValueKind::MemberAccess &&
        receiver->unsafeOperation == UnsafeOperationKind::RawMember &&
        !receiver->operands.empty()) {
      const HirValueId pointerId = receiver->operands.front();
      const HirValue *pointer = findValue(pointerId);
      if (pointer == nullptr ||
          pointer->info.type.kind != SemanticType::RawPointer ||
          pointer->info.type.arguments.size() != 1) {
        valid = false;
        return {};
      }
      (void)valueOperand(pointerId);
      const SemanticType pointee = pointer->info.type.arguments.front();
      const AccessMode pointeeAccess = pointer->info.type.pointerAccess;
      const MirPlaceId place = appendPlace({.root = MirPlaceRootKind::Value,
                                            .value = mirValueFor(pointerId),
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
      emitPlaceDependencies(id);
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
    case HirValueKind::Unexpected:
      return MirOperation::Unexpected;
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
          source->projections.empty() &&
          (source->root == MirPlaceRootKind::This ||
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

  [[nodiscard]] MirPlaceId
  projectBorrowFields(MirPlaceId base, const std::vector<SymbolId> &fields) {
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
    for (const auto field : fields) {
      const auto owner = std::find_if(program.classInstances().begin(),
                                      program.classInstances().end(),
                                      [&](const auto &candidate) {
                                        return candidate.type == projected.type;
                                      });
      if (owner == program.classInstances().end())
        return 0;
      const auto member =
          std::find_if(owner->fields.begin(), owner->fields.end(),
                       [&](const auto &candidate) {
                         return candidate.info.symbol == field;
                       });
      if (member == owner->fields.end())
        return 0;
      projected.projections.push_back(
          {.kind = MirProjectionKind::Field, .field = field});
      if (projected.key)
        projected.key->projections.push_back(
            {.kind = PlaceProjectionKind::Field, .field = field});
      projected.type = member->info.type;
      dereference();
      // Keep the typed prefix as well as the final referent. Independent
      // field-origin verification composes the carrier's stored reference
      // with the remainder of an accessor path without reconstructing types.
      (void)intern();
    }
    return intern();
  }

  [[nodiscard]] std::optional<std::size_t>
  storedReferencePrefix(SemanticType type,
                        const std::vector<SymbolId> &fields) const {
    if (type.kind == SemanticType::Reference && type.arguments.size() == 1)
      type = SemanticType(type.arguments.front());
    for (std::size_t index = 0; index < fields.size(); ++index) {
      const auto owner = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const auto &candidate) { return candidate.type == type; });
      if (owner == program.classInstances().end())
        return std::nullopt;
      const auto field =
          std::find_if(owner->fields.begin(), owner->fields.end(),
                       [&](const auto &candidate) {
                         return candidate.info.symbol == fields[index];
                       });
      if (field == owner->fields.end())
        return std::nullopt;
      type = field->info.type;
      if (type.kind == SemanticType::Reference)
        return index + 1;
    }
    return std::nullopt;
  }

  [[nodiscard]] bool borrowsCarrierStorage(const HirValue &value) const {
    if (value.info.category != ValueCategory::Place)
      return false;
    if (value.info.type.kind == SemanticType::Class &&
        value.info.traits.containsBorrowedState)
      return true;
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
    if (retained != nullptr && retained->kind == MirLoanKind::Local &&
        (retained->storedDependency == 0 ||
         (receiver && isRangeIteratorValue(*receiver))))
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

  [[nodiscard]] bool isRangeIteratorValue(HirValueId value) const {
    const auto *input = findValue(value);
    const auto binding = input == nullptr ? localSymbols.end()
                                          : localSymbols.find(input->symbol);
    return binding != localSymbols.end() &&
           isRangeIteratorBinding(binding->second);
  }

  [[nodiscard]] bool
  storesCarrierReceiverReference(const HirValue &value) const {
    if (value.intrinsic != IntrinsicKind::None || !value.functionTarget ||
        value.info.category != ValueCategory::Value ||
        value.info.type.kind != SemanticType::Class ||
        !value.info.traits.containsBorrowedState ||
        value.borrowOrigin != BorrowOriginKind::Receiver ||
        !value.borrowFields.empty() || !value.borrowArguments.empty())
      return false;
    const auto receiver = receiverValue(value);
    const auto *input = receiver ? findValue(*receiver) : nullptr;
    if (input == nullptr || !input->info.traits.containsBorrowedState)
      return false;
    // The compact receiver contract represents one stored reference. A
    // multi-origin carrier instead forwards its existing external dependencies
    // (for example a subview of two independent buffers), not its own shell.
    std::vector<SemanticType> visiting;
    const auto references = [&](const auto &self,
                                const SemanticType &type) -> unsigned {
      if (type.kind == SemanticType::Reference)
        return 1;
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
    return references(references, value.info.type) == 1;
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
            value.info.traits.copyable && value.borrowFields.size() == 1 &&
            loan->additionalDependencies.empty() &&
            loan->designatedFields == mapping.sourceFields)
          return loan->source;
        if (loan->entry && loan->kind == MirLoanKind::Stored)
          return projectBorrowFields(loan->source, mapping.sourceFields);
        // A sole-path carrier may still use the compact single-origin loan
        // representation. Its selected field already denotes that source;
        // projecting the field onto the external owner would change meaning.
        if (loan->kind == MirLoanKind::Stored &&
            loan->designatedFields.empty() &&
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
                                   mapping.sourceFields);
    }
    // A returned reference protects the addressable carrier shell. A
    // by-value carrier instead forwards its external stored referents.
    if (borrowsCarrierStorage(value) || storesCarrierReceiverReference(value)) {
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
            const auto shell =
                temporaryBorrowOperand(*receiver, receiverAccess(value),
                                       input->info.type, false, true);
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
        if (const MirPlaceId source = borrowedSourcePlace(*receiver);
            source != 0) {
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
                   MirDropObligationId movedBindingDrop = 0) {
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
      if (type.kind != SemanticType::Reference ||
          type.referenceAccess != AccessMode::ReadOnly ||
          type.arguments.size() != 1 ||
          type.arguments.front().kind != SemanticType::Class ||
          input.operands.front().kind != MirOperandKind::BorrowRead ||
          input.operands.front().type != type || source == nullptr ||
          source->root != MirPlaceRootKind::Value ||
          !source->projections.empty() ||
          source->type != type.arguments.front() ||
          source->access != AccessMode::Mutable ||
          source->sourceValue != sourceValue || scopes.empty()) {
        valid = false;
      } else {
        inputLoan = createLoan(MirLoanKind::CallArgument, source->id,
                               AccessMode::ReadOnly, callSite);
        input.loan = inputLoan;
        scopes.back().loans.push_back(inputLoan);
      }
    }
    const bool materializesCheckedPlace =
        inputKind == HirCallInputKind::CopyValue ||
        ((inputKind == HirCallInputKind::ReadBorrow ||
          inputKind == HirCallInputKind::MutableBorrow) &&
         input.operands.front().kind != MirOperandKind::Loan);
    if (materializesCheckedPlace) {
      input.definedFailure = localDefinedFailure(sourceValueInfo);
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
    if (value.intrinsic == IntrinsicKind::ExpectedHasValue) {
      const std::optional<HirValueId> receiver = receiverValue(value);
      const HirValue *source = receiver ? findValue(*receiver) : nullptr;
      if (detachedBorrowResult || reserveBorrowResult || !receiver ||
          source == nullptr || !callArgumentValues(value).empty() ||
          source->info.type.kind != SemanticType::Expected ||
          source->info.type.arguments.size() != 2 ||
          value.info.type != SemanticType::Bool) {
        valid = false;
        return;
      }
      (void)appendInstruction({
          .kind = MirInstructionKind::Compute,
          .hirValue = value.id,
          .result = resultFor(value),
          .operands = {receiverOperand(*receiver, AccessMode::ReadOnly)},
          .operation = MirOperation::ExpectedHasValue,
          .info = value.info,
      });
      return;
    }
    DefinedFailureOperation failure = value.definedFailure;
    if (detachedBorrowResult) {
      failure.localOrigins.clear();
      failure.propagation = value.dispatch == CallDispatch::Virtual
                                ? FailurePropagationKind::VirtualCall
                                : FailurePropagationKind::DirectCall;
    }
    if ((value.intrinsic == IntrinsicKind::ObjectConstruct ||
         value.intrinsic == IntrinsicKind::PrefixStorageAppend) &&
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
    MirInstruction call{.kind = construction ? MirInstructionKind::Construct
                                             : MirInstructionKind::Call,
                        .hirValue = value.id,
                        .callSite = value.callPlan ? value.id : 0,
                        .intrinsic = value.intrinsic,
                        .objectLayoutType = value.objectLayoutType,
                        .neverReturns = value.neverReturns,
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
            : temporaryReceiver
                ? temporaryBorrowOperand(receiver.value, access, receiver.type,
                                         false, true)
                : receiverOperand(receiver.value, access);
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
      const bool expectedTemporaryExtraction =
          (isExpectedPropagationExtraction(value.id) ||
           (value.source == nullptr && value.definedFailure.empty())) &&
          receiverSource != nullptr &&
          receiverSource->info.category == ValueCategory::Value &&
          receiverSource->info.type.kind == SemanticType::Expected &&
          (value.intrinsic == IntrinsicKind::ExpectedValue ||
           value.intrinsic == IntrinsicKind::ExpectedError);
      MirOperand operand =
          value.nativeCalleeType ? valueOperand(*receiver)
          : retainedReceiver ? receiverOperand(receiverSource->operands.front(),
                                               AccessMode::Mutable)
          : expectedTemporaryExtraction
              ? temporaryBorrowOperand(
                    *receiver, AccessMode::ReadOnly, receiverSource->info.type,
                    true,
                    mirFallibleBorrowReferent(receiverSource->info.type)
                        .has_value())
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
      if (const HirValue *source = findValue(*receiver);
          source != nullptr &&
          source->unsafeOperation == UnsafeOperationKind::RawMember) {
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

    const auto beginPreparedArgumentFailure =
        [&](HirValueId argument, HirCallInputKind kind,
            const SemanticType &parameter) {
          const auto enclosingFailure = preparedCallArgumentFailure;
          if (!preparedParameterDrops.empty() ||
              !temporaryArgumentLoans.empty() ||
              (kind == HirCallInputKind::MoveValue &&
               parameter.kind == SemanticType::Class)) {
            preparedCallArgumentFailure =
                PreparedCallArgumentFailure{.argument = argument};
          }
          return enclosingFailure;
        };
    if (value.callPlan) {
      for (const HirCallArgument &argument : value.callPlan->arguments) {
        call.parameterTypes.push_back(argument.parameterType);
        const auto enclosingFailure = beginPreparedArgumentFailure(
            argument.value, argument.kind, argument.parameterType);
        MirOperand sourceArgument =
            value.intrinsic == IntrinsicKind::PrefixStorageAppend &&
                    argument.parameterIndex == 0 &&
                    argument.kind == HirCallInputKind::MutableBorrow &&
                    argument.parameterType.kind == SemanticType::PrefixStorage
                ? receiverOperand(argument.value, AccessMode::Mutable)
            : argument.kind == HirCallInputKind::ReadTemporaryBorrow
                ? temporaryBorrowOperand(argument.value, AccessMode::ReadOnly,
                                         argument.parameterType)
            : argument.kind == HirCallInputKind::CopyValue
                ? copyArgumentOperand(argument.value, argument.parameterType)
                : argumentOperand(argument.value, argument.parameterType);
        preparedCallArgumentFailure = enclosingFailure;
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
              MirDropObligationId movedBindingDrop = 0) {
            const std::size_t parameterIndex = call.operands.size();
            sourceArguments.push_back(sourceOperand);
            PreparedCallInput prepared = prepareCallInput(
                value.id, sourceValue, parameter, kind,
                MirCallInputRole::Argument, parameterIndex,
                std::move(sourceOperand), true, false, movedBindingDrop);
            call.parameterTypes.push_back(parameter);
            call.operands.push_back(std::move(prepared.operand));
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
          const SemanticType parameter = index < value.parameterTypes.size()
                                             ? value.parameterTypes[index]
                                             : input->info.type;
          const bool mutableStorageInput =
              index == 0 && input->info.category == ValueCategory::Place &&
              input->info.type == parameter &&
              (value.intrinsic == IntrinsicKind::PrefixStorageAppend ||
               value.intrinsic == IntrinsicKind::PrefixStorageInsert ||
               value.intrinsic == IntrinsicKind::OptionalStorageConstruct) &&
              (parameter.kind == SemanticType::PrefixStorage ||
               parameter.kind == SemanticType::Class);
          const std::optional<HirCallInputKind> kind =
              mutableStorageInput
                  ? std::optional<
                        HirCallInputKind>{HirCallInputKind::MutableBorrow}
                  : inputKindFor(*input, parameter);
          if (!kind) {
            valid = false;
            continue;
          }
          // Fixed expressions before a final pack expansion have the same
          // failure-before-publication contract as ordinary call arguments.
          const auto enclosingFailure =
              beginPreparedArgumentFailure(input->id, *kind, parameter);
          MirOperand sourceArgument =
              *kind == HirCallInputKind::CopyValue
                  ? copyArgumentOperand(input->id, parameter)
              : mutableStorageInput
                  ? receiverOperand(input->id, AccessMode::Mutable)
                  : argumentOperand(input->id, parameter);
          preparedCallArgumentFailure = enclosingFailure;
          appendPreparedArgument(input->id, parameter, *kind,
                                 std::move(sourceArgument));
          continue;
        }

        const auto local = localSymbols.find(input->symbol);
        const MirPlaceId packPlaceId =
            local == localSymbols.end() ? 0 : placeForBinding(local->second);
        const MirPlace *resolvedPackPlace = output.findPlace(packPlaceId);
        const std::vector<SemanticType> &elements = input->info.type.arguments;
        const bool retainedPackParameter =
            index + 1 == value.parameterTypes.size() &&
            value.parameterTypes[index].kind == SemanticType::TypePack &&
            value.parameterTypes[index].concretePack &&
            value.parameterTypes[index].arguments == elements;
        const bool flattenedPackParameters =
            value.parameterTypes.size() == index + elements.size() &&
            std::equal(
                elements.begin(), elements.end(),
                value.parameterTypes.begin() + index,
                [](const SemanticType &element, const SemanticType &parameter) {
                  return mir_detail::validCallInputType(
                             element, parameter, HirCallInputKind::Value) ||
                         (parameter.kind == SemanticType::Reference &&
                          parameter.referenceAccess == AccessMode::ReadOnly &&
                          parameter.arguments.size() == 1 &&
                          parameter.arguments.front() == element);
                });
        if (index + 1 != arguments.size() ||
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
                  : value.parameterTypes[index + elementIndex];
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
        if (terminated()) {
          return;
        }
      }
    }

    if (terminated()) {
      return;
    }
    call.result =
        detachedBorrowResult
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
    MirPlaceId origin = borrowOriginPlace(value, originCall);
    std::vector<MirLoanId> movedArgumentLoans;
    for (std::size_t index = 0;
         !construction && !value.constructorTarget &&
         !value.borrowFields.empty() && index < arguments.size() &&
         index < value.parameterTypes.size();
         ++index) {
      if (value.parameterTypes[index].kind == SemanticType::Reference)
        continue;
      const auto *argument = findValue(arguments[index]);
      if (argument == nullptr || argument->info.traits.copyable)
        continue;
      const auto moved = movedBorrowCarrierBinding(arguments[index]);
      const auto found = bindingLoans.find(moved);
      const auto *loan = found == bindingLoans.end()
                             ? nullptr
                             : output.findLoan(found->second);
      if (moved != 0 && loan != nullptr && loan->kind == MirLoanKind::Stored &&
          std::find(movedArgumentLoans.begin(), movedArgumentLoans.end(),
                    loan->id) == movedArgumentLoans.end())
        movedArgumentLoans.push_back(loan->id);
    }
    call.borrowOrigin = value.borrowOrigin;
    call.borrowArgument = value.borrowArgument;
    call.borrowAccess = value.borrowAccess;
    call.borrowArguments = value.borrowArguments;
    call.borrowFields = value.borrowFields;
    call.borrowPlace = value.borrowPlace;
    if (value.borrowOrigin != BorrowOriginKind::None && origin != 0) {
      const bool shellReference = borrowsCarrierStorage(value);
      const MirLoanKind kind =
          value.info.traits.containsBorrowedState && !shellReference
              ? MirLoanKind::Stored
              : MirLoanKind::CallResult;
      MirLoanId parent = 0;
      if (originLoan != 0 && originLoan <= output.loans.size() &&
          std::find(movedArgumentLoans.begin(), movedArgumentLoans.end(),
                    originLoan) == movedArgumentLoans.end()) {
        const AccessMode parentAccess = output.loans[originLoan - 1].access;
        const bool derivedCallResult =
            kind == MirLoanKind::CallResult &&
            (parentAccess == AccessMode::Mutable ||
             value.borrowAccess == AccessMode::ReadOnly ||
             !value.borrowFields.empty());
        const bool derivedOriginCarrier =
            kind == MirLoanKind::Stored &&
            (parentAccess == AccessMode::Mutable ||
             (output.loans[originLoan - 1].kind == MirLoanKind::Stored &&
              isLifecycleBoundBorrowCarrier(value.info.type)) ||
             (output.loans[originLoan - 1].kind == MirLoanKind::Stored &&
              output.loans[originLoan - 1].storedDependency != 0 &&
              value.info.traits.copyable &&
              value.borrowOrigin == BorrowOriginKind::Argument &&
              value.borrowArgument < value.parameterTypes.size() &&
              value.parameterTypes[value.borrowArgument].kind !=
                  SemanticType::Reference) ||
             (output.loans[originLoan - 1].kind == MirLoanKind::Parameter &&
              isLifecycleBoundBorrowCarrier(value.info.type) &&
              value.borrowArguments.empty() &&
              value.borrowFields.size() <= 1) ||
             (output.loans[originLoan - 1].kind == MirLoanKind::CallResult &&
              value.borrowAccess == AccessMode::ReadOnly));
        if ((derivedCallResult || derivedOriginCarrier) &&
            !(shellReference &&
              output.loans[originLoan - 1].kind == MirLoanKind::Stored) &&
            !(value.intrinsic == IntrinsicKind::ExpectedValue &&
              sourceReceiverValue &&
              mirFallibleBorrowReferent(
                  findValue(*sourceReceiverValue)->info.type))) {
          // A fallible reference stores an external pointer, not an inline
          // referent. Extraction shares that owner's authority independently
          // of the carrier shell, which may end at this full expression.
          parent = originLoan;
        }
      }
      // Moving an owned carrier into a new carrier forwards its external
      // ancestry; it does not borrow the consumed shell. In particular a
      // cleanup-bound result keeps the shared resource parameter alive.
      if (kind == MirLoanKind::Stored && originLoan != 0 &&
          originLoan <= output.loans.size() &&
          output.loans[originLoan - 1].kind == MirLoanKind::Stored &&
          output.loans[originLoan - 1].lifecycleBound &&
          isLifecycleBoundBorrowCarrier(value.info.type) &&
          value.borrowOrigin == BorrowOriginKind::Argument &&
          value.borrowArgument < arguments.size() &&
          value.borrowArgument < value.parameterTypes.size() &&
          value.parameterTypes[value.borrowArgument].kind !=
              SemanticType::Reference &&
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
          retainedContract == retainedInitializerLoans.end()
              ? nullptr
              : source.findLoan(retainedContract->second);
      const HirValue *shellArgument =
          (value.constructorTarget || value.functionTarget) &&
                  value.borrowOrigin == BorrowOriginKind::Argument &&
                  value.borrowArgument < arguments.size()
              ? findValue(arguments[value.borrowArgument])
          : storesCarrierReceiverReference(value) && receiverValue(value)
              ? findValue(*receiverValue(value))
              : nullptr;
      const bool shellReferenceInput =
          storesCarrierReceiverReference(value) ||
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
      const MirPlace *originPlace = output.findPlace(origin);
      const bool twoPhase = reserveBorrowResult && originPlace != nullptr &&
                            originPlace->root != MirPlaceRootKind::Symbol;
      const MirLoanId loan = createLoan(kind, origin, value.borrowAccess,
                                        value.id, 0, 0, parent, twoPhase);
      if (shellContract != nullptr) {
        recordLoanIdentity(loan, *shellContract);
      } else if (kind == MirLoanKind::Stored && shellArgument != nullptr &&
                 shellArgument->info.traits.containsBorrowedState &&
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
        if (output.loans[loan - 1].storedDependency == 0)
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
      const MirLoanId dependencyLoan =
          originLoan != 0 ? originLoan
          : value.borrowOrigin == BorrowOriginKind::Argument &&
                  value.borrowArgument < arguments.size()
              ? loanForValue(arguments[value.borrowArgument])
              : 0;
      if (!shellReference && dependencyLoan != 0 &&
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
      call.loan = loan;
      valueLoans.insert_or_assign(value.id, loan);
      if (!scopes.empty()) {
        scopes.back().loans.push_back(loan);
      }
    }
    if ((value.intrinsic == IntrinsicKind::ObjectDestroy ||
         value.intrinsic == IntrinsicKind::PrefixStoragePop ||
         value.intrinsic == IntrinsicKind::PrefixStorageErase ||
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
        value.constructorTarget &&
        value.constructorKind == ConstructorKind::Ordinary) {
      // Prepared parameters remain caller-owned until allocation succeeds.
      // The second instruction transfers them into the selected constructor;
      // its failure path owns only the unconstructed allocation, not T.
      call.objectAllocation = appendObjectAllocation(value);
      call.definedFailure.localOrigins.clear();
      call.definedFailure.propagation = normalizedConstructorPropagation(
          FailurePropagationKind::Constructor, value.constructorTarget);
    }
    if (value.intrinsic == IntrinsicKind::PrefixStorageAppend &&
        value.definedFailure.propagation ==
            FailurePropagationKind::Constructor &&
        value.constructorTarget &&
        value.constructorKind == ConstructorKind::Ordinary) {
      const std::size_t preflightOperands = 1;
      if (call.operands.size() < preflightOperands || call.callSite == 0) {
        valid = false;
        return;
      }
      DefinedFailureOperation preflightFailure;
      for (auto &origin : call.definedFailure.localOrigins) {
        DefinedFailureOrigin check = origin;
        const auto isPreflightOutcome = [](DefinedFailureOutcome outcome) {
          return outcome == DefinedFailureOutcome{
                                DefinedFailureCode::InvalidStorageState,
                                DefinedFailureDetail::RelocationCapacity};
        };
        std::erase_if(check.outcomes, [&](DefinedFailureOutcome outcome) {
          return !isPreflightOutcome(outcome);
        });
        if (!check.outcomes.empty())
          preflightFailure.localOrigins.push_back(std::move(check));
        std::erase_if(origin.outcomes, isPreflightOutcome);
      }
      std::erase_if(call.definedFailure.localOrigins,
                    [](const DefinedFailureOrigin &origin) {
                      return origin.outcomes.empty();
                    });
      // No parameter has transferred yet. Failure here drains every prepared
      // owner; only the success edge reaches the constructor handoff below.
      call.storagePreflight = appendInstruction(
          {.kind = MirInstructionKind::StorageConstructPreflight,
           .hirValue = value.id,
           .callSite = call.callSite,
           .operands = {call.operands.begin(),
                        call.operands.begin() + preflightOperands},
           .definedFailure = std::move(preflightFailure),
           .info = {.type = SemanticType::Void}},
          false);
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
         mirFallibleBorrowReferent(value.info.type).has_value()) &&
        value.info.category == ValueCategory::Value &&
        value.info.traits.movable && value.info.traits.containsBorrowedState;
    // A cleanup-owning result exists only after a failure-capable call
    // succeeds, regardless of whether it is the full-expression root or a
    // nested input to another call. Arm its value obligation on the Invoke
    // success edge; the normal successor then owns the temporary until a
    // later CallInput reparents it or full-expression cleanup drops it.
    const bool successEdgeResult =
        resultDrop != 0 && routeFailureEdgesHere() &&
        !call.definedFailure.empty() && !call.destination &&
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
    (void)appendInstruction(std::move(call));
    // A by-value move transfers the shell and every dependency to the callee.
    // A returned carrier publishes its own checked dependency set; it is not
    // a child reborrow of the now-consumed caller shell.
    for (const auto loan : movedArgumentLoans) {
      (void)appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = loan});
      removeLoanFromState(loan, scopes, bindingLoans);
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
        value.operands.size() != value.packFoldArgument + 1) {
      valid = false;
      return;
    }

    const HirValueId packValue = value.operands.back();
    emitPlaceDependencies(packValue);
    const MirPlaceId packPlaceId = placeForValue(packValue);
    const MirPlace *resolvedPackPlace = output.findPlace(packPlaceId);
    if (resolvedPackPlace == nullptr ||
        resolvedPackPlace->root != MirPlaceRootKind::Binding ||
        !resolvedPackPlace->projections.empty() ||
        resolvedPackPlace->symbol != value.packFoldSymbol ||
        resolvedPackPlace->type.kind != SemanticType::TypePack ||
        !resolvedPackPlace->type.concretePack ||
        resolvedPackPlace->type.genericParameterId != value.packFoldParameter ||
        resolvedPackPlace->type.arguments.size() !=
            value.packFoldElements.size()) {
      valid = false;
      return;
    }
    // Appending projected places can reallocate output.places. Keep the
    // verified entry-place descriptor by value for the whole expansion.
    const MirPlace packPlace = *resolvedPackPlace;

    for (std::size_t elementIndex = 0;
         elementIndex < value.packFoldElements.size(); ++elementIndex) {
      const HirPackFoldElement &element = value.packFoldElements[elementIndex];
      if (element.functionTarget == 0 ||
          element.elementType != packPlace.type.arguments[elementIndex] ||
          element.parameterTypes.size() != value.packFoldArgument + 1) {
        valid = false;
        return;
      }

      MirInstruction call{
          .kind = MirInstructionKind::Call,
          .hirValue = value.id,
          .definedFailure = {.propagation = normalizedCallPropagation(
                                 FailurePropagationKind::DirectCall,
                                 CallDispatch::Static, element.functionTarget)},
          .dispatch = CallDispatch::Static,
          .functionTarget = element.functionTarget,
          .info = value.info,
      };
      call.parameterTypes = element.parameterTypes;
      call.operands.reserve(element.parameterTypes.size());
      for (std::size_t argument = 0; argument < value.packFoldArgument;
           ++argument) {
        call.operands.push_back(argumentOperand(
            value.operands[argument], element.parameterTypes[argument]));
      }

      const SemanticType &elementParameter = element.parameterTypes.back();
      if (elementParameter.kind != SemanticType::Reference ||
          elementParameter.referenceAccess != AccessMode::ReadOnly ||
          elementParameter.arguments.size() != 1 ||
          elementParameter.arguments.front() != element.elementType) {
        valid = false;
        return;
      }
      MirPlace projected = packPlace;
      projected.id = 0;
      projected.projections.push_back({.kind = MirProjectionKind::PackElement,
                                       .constantIndex = elementIndex});
      projected.type = element.elementType;
      projected.access = AccessMode::ReadOnly;
      projected.traits = resolvedTraits(element.elementType);
      projected.sourceValue = 0;
      projected.key.reset();
      const MirPlaceId elementPlace =
          appendPackElementPlace(std::move(projected));
      call.operands.push_back({.kind = MirOperandKind::BorrowRead,
                               .place = elementPlace,
                               .type = elementParameter});
      (void)appendInstruction(std::move(call));
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
    if (value.borrowOrigin != BorrowOriginKind::None && origin != 0) {
      MirLoanId parent = 0;
      if (originLoan != 0 && originLoan <= output.loans.size() &&
          (output.loans[originLoan - 1].access == AccessMode::Mutable ||
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
    if (!requiresMirFailureControlFlow(
            producer, MirFailureControlFlowPosition::FullExpressionRoot)) {
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
    if (value->info.type == SemanticType::Bool) {
      return valueOperand(id);
    }
    if (const auto found = contextualValues.find(id);
        found != contextualValues.end()) {
      return {.kind = MirOperandKind::Value,
              .value = found->second,
              .type = SemanticType::Bool};
    }

    const ExpressionInfo info{.type = SemanticType::Bool,
                              .category = ValueCategory::Value,
                              .access = AccessMode::ReadOnly,
                              .traits = semanticTraits(SemanticType::Bool)};
    const MirValueId result = appendValue(id, info);
    if (value->info.type.kind == SemanticType::Expected) {
      (void)appendInstruction({.kind = MirInstructionKind::Compute,
                               .hirValue = id,
                               .result = result,
                               .operands = {valueOperand(id)},
                               .operation = MirOperation::ExpectedHasValue,
                               .info = info});
    } else {
      valid = false;
    }
    contextualValues.emplace(id, result);
    return {.kind = MirOperandKind::Value,
            .value = result,
            .type = SemanticType::Bool};
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

  void endAddedLoans(const std::vector<Scope> &baseline) {
    if (baseline.size() != scopes.size()) {
      valid = false;
      return;
    }
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

  void endFullExpressionLoans(const std::vector<Scope> &baseline) {
    if (baseline.size() != scopes.size()) {
      valid = false;
      return;
    }
    const std::unordered_set<MirLoanId> retained = retainedReborrowAncestors();
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
        if (!candidate.carriers.empty() || candidate.escapes ||
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
      const auto result = 1 + std::max(self(self, record->parent),
                                       self(self, record->storedDependency));
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

  [[nodiscard]] bool mustReachBorrowedReturn(MirLoanId loan) const {
    if (loan == 0 || loan > output.loans.size() || function == nullptr ||
        function->returnBorrowOrigin != BorrowOriginKind::Argument ||
        function->returnBorrowParameter >= function->parameterBindings.size()) {
      return false;
    }
    const MirLoan &candidate = output.loans[loan - 1];
    const HirBindingId parameter =
        function->parameterBindings[function->returnBorrowParameter];
    return candidate.entry &&
           std::find(candidate.carriers.begin(), candidate.carriers.end(),
                     parameter) != candidate.carriers.end();
  }

  void endSemanticLoans(const std::vector<SemanticLoanId> &loans,
                        HirStatementId statement) {
    for (const MirLoanId loan : childFirstLoans(loans)) {
      if (mustReachBorrowedReturn(loan) ||
          containsLoan(constructorPublishedLoans, loan)) {
        continue;
      }
      if (!loanIsActive(scopes, loan)) {
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
    for (const MirLoanId loan : childFirstLoans(loans)) {
      if (mustReachBorrowedReturn(loan) ||
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
    for (const MirLoanId loan : childFirstLoans(loans, &scopeState)) {
      if (mustReachBorrowedReturn(loan) ||
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
    terminate({.kind = MirTerminatorKind::Goto, .target = mergeBlock});
    const std::vector<TemporaryDrop> shortCircuitTemporaryDrops =
        temporaryDrops;

    current = rightBlock;
    scopes = incomingScopes;
    temporaryDrops = leftTemporaryDrops;
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
    terminate({.kind = MirTerminatorKind::Goto, .target = mergeBlock});
    const std::vector<TemporaryDrop> rightTemporaryDrops = temporaryDrops;
    const auto rightScopes = scopes;

    current = mergeBlock;
    scopes = incomingScopes;
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
    if (loan == 0 || value.info.category != ValueCategory::Place ||
        value.info.access != AccessMode::ReadOnly) {
      valid = false;
      return;
    }
    valueLoans.insert_or_assign(value.id, loan);
    loweredValues.insert_or_assign(value.id, mirValueFor(source));
    valuePlaces.insert_or_assign(value.id,
                                 clonePlace(placeForValue(source), value));
  }

  void emitConditional(const HirValue &value) {
    if (value.operands.size() != 3) {
      valid = false;
      return;
    }

    for (const HirValueId input : value.conditionalInputs) {
      emitValue(input);
      const HirValue *eager = findValue(input);
      // A cleanup-free closure still needs stable environment identity when
      // its invocation is in a later branch. Owning closures already have a
      // value-rooted place from their full-expression drop obligation.
      if (eager != nullptr && eager->kind == HirValueKind::Lambda &&
          eager->info.traits.drop == DropKind::Trivial &&
          !valuePlaces.contains(input)) {
        valuePlaces.emplace(input, valueRootPlace(*eager));
      }
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
    const auto emitArm = [&](MirBlockId block, HirValueId arm,
                             bool &completesNormally) {
      current = block;
      scopes = incomingScopes;
      temporaryDrops = incomingTemporaryDrops;
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
      terminate({.kind = MirTerminatorKind::Goto, .target = mergeBlock});
      return temporaryDrops;
    };

    bool thenCompletes = false;
    bool elseCompletes = false;
    thenTemporaryDrops = emitArm(thenBlock, value.operands[1], thenCompletes);
    const auto thenScopes = scopes;
    elseTemporaryDrops = emitArm(elseBlock, value.operands[2], elseCompletes);
    const auto elseScopes = scopes;

    current = mergeBlock;
    scopes = incomingScopes;
    if (!thenCompletes && !elseCompletes) {
      temporaryDrops = incomingTemporaryDrops;
      terminate({.kind = MirTerminatorKind::Unreachable});
      emittedValues.insert(value.id);
      return;
    }
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

  void emitExpectedPropagation(const HirValue &value) {
    if (!value.expectedPropagation ||
        value.info.type == SemanticType::Unknown) {
      valid = false;
      return;
    }
    const HirExpectedPropagationPlan &plan = *value.expectedPropagation;
    const HirValue *operand = findValue(plan.operand);
    const HirValue *successExtraction = findValue(plan.successExtraction);
    const HirValue *errorExtraction = findValue(plan.errorExtraction);
    const HirValue *unexpected = findValue(plan.unexpected);
    if (operand == nullptr || successExtraction == nullptr ||
        errorExtraction == nullptr || unexpected == nullptr ||
        operand->info.type.kind != SemanticType::Expected ||
        operand->info.type.arguments.size() != 2 ||
        (operand->info.type.arguments[0] != value.info.type &&
         mirFallibleBorrowReferent(operand->info.type) !=
             std::optional<SemanticType>{value.info.type}) ||
        successExtraction->intrinsic != IntrinsicKind::ExpectedValue ||
        errorExtraction->intrinsic != IntrinsicKind::ExpectedError ||
        unexpected->kind != HirValueKind::Unexpected ||
        unexpected->info.type.kind != SemanticType::Unexpected ||
        unexpected->info.type.arguments.size() != 1 ||
        unexpected->info.type.arguments.front() !=
            operand->info.type.arguments[1]) {
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
    const MirOperand condition = conditionOperand(plan.operand);
    const std::vector<Scope> evaluatedScopes = scopes;
    const std::vector<TemporaryDrop> evaluatedTemporaryDrops = temporaryDrops;
    const auto evaluatedBindingLoans = bindingLoans;
    const MirBlockId successBlock = appendBlock();
    const MirBlockId errorBlock = appendBlock();
    terminate({.kind = MirTerminatorKind::Branch,
               .hirValue = value.id,
               .value = condition,
               .target = successBlock,
               .elseTarget = errorBlock});

    current = errorBlock;
    scopes = evaluatedScopes;
    temporaryDrops = evaluatedTemporaryDrops;
    const MirOperand propagatedError = valueOperand(plan.unexpected);
    const MirDropObligationId enclosingPendingReturnDrop = pendingReturnDrop;
    prepareReturnOwnership(plan.unexpected, propagatedError);
    endReturnExpressionLoans(scopeBaseline->second);
    emitTemporaryDrops(temporaryBaseline->second, value.id, 0, 0, true);
    emitScopeExit(0);
    publishReturnOwnership(plan.unexpected);
    pendingReturnDrop = enclosingPendingReturnDrop;
    terminate({.kind = MirTerminatorKind::Return,
               .hirValue = value.id,
               .value = propagatedError});

    current = successBlock;
    scopes = evaluatedScopes;
    temporaryDrops = evaluatedTemporaryDrops;
    // Cleanup on the propagated-error path must not retire the success path's
    // carrier-to-owner mapping, just as it must not retire its scope loans.
    bindingLoans = evaluatedBindingLoans;
    if (value.info.type == SemanticType::Void) {
      emitValue(plan.successExtraction);
      endAddedLoans(evaluatedScopes);
      emittedValues.insert(value.id);
      return;
    }
    if (plan.successValue == 0 || findValue(plan.successValue) == nullptr) {
      valid = false;
      return;
    }

    if (value.info.category == ValueCategory::Place) {
      forwardBorrowResult(value, plan.successValue);
      emittedValues.insert(value.id);
      return;
    }

    const MirPlaceId temporary = conditionalTemporary(value);
    MirInstruction initialize{.kind = MirInstructionKind::Initialize,
                              .hirValue = value.id,
                              .destination = temporary,
                              .operands = {valueOperand(plan.successValue)},
                              .info = {.type = value.info.type,
                                       .category = ValueCategory::Place,
                                       .access = AccessMode::Mutable,
                                       .traits = value.info.traits}};
    const MirDropObligationId resultObligation =
        dropObligationForValue(value.id, temporary);
    const MirDropObligationId sourceObligation =
        dropObligationForValue(plan.successValue);
    if (resultObligation != 0) {
      if (temporaryIsActive(sourceObligation)) {
        appendReparentOrTypedTransfer(initialize, sourceObligation,
                                      resultObligation);
        (void)removeTemporary(sourceObligation);
      } else {
        appendLifecycle(initialize, {.kind = MirLifecycleEventKind::Initialize,
                                     .target = resultObligation});
      }
      registerTemporary(resultObligation);
    }
    (void)appendInstruction(std::move(initialize));
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
    endAddedLoans(evaluatedScopes);
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
        value.definedFailure.propagation != FailurePropagationKind::None) {
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

    if (value->kind == HirValueKind::ExpectedPropagation) {
      emitExpectedPropagation(*value);
      return;
    }

    if (value->kind == HirValueKind::Move) {
      (void)valueOperand(id);
      return;
    }
    if (value->kind == HirValueKind::Assignment ||
        value->kind == HirValueKind::MemberSet ||
        value->kind == HirValueKind::IndexSet ||
        value->kind == HirValueKind::DereferenceSet) {
      const bool overloadedBorrowStore =
          (value->kind == HirValueKind::IndexSet ||
           value->kind == HirValueKind::DereferenceSet) &&
          value->functionTarget &&
          value->borrowOrigin != BorrowOriginKind::None;
      bool reservedBorrowStore =
          overloadedBorrowStore && plainOverloadedBorrowStore(*value);
      if (value->kind == HirValueKind::MemberSet && !value->operands.empty()) {
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
          emitPlaceDependencies(value->operands.front());
        }
      } else if (value->kind == HirValueKind::IndexSet &&
                 value->operands.size() >= 2) {
        if (overloadedBorrowStore) {
          emitCall(*value, true, reservedBorrowStore);
        } else if (value->unsafeOperation == UnsafeOperationKind::RawIndex) {
          (void)valueOperand(value->operands[0]);
        } else {
          emitPlaceDependencies(value->operands[0]);
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
          (assign.definedFailure.localOrigins.empty() || checkedReplacement)) {
        std::unordered_set<HirClassInstanceId> visiting;
        auto cleanup = uniqueCleanupType(replacementPlace->type, visiting);
        if (cleanup) {
          MirInstruction prepare{.kind =
                                     MirInstructionKind::CleanupBeforeAssign,
                                 .hirValue = value->id,
                                 .destination = destination,
                                 .replacementCleanup = *cleanup,
                                 .info = value->info};
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
        appendLifecycle(assign, {.kind = MirLifecycleEventKind::Replace,
                                 .source = sourceObligation,
                                 .target = target});
      }
      const auto assignment = appendInstruction(std::move(assign));
      if (preparation != 0) {
        for (auto &block : output.blocks)
          for (auto &instruction : block.instructions)
            if (instruction.id == preparation)
              instruction.replacementPair = assignment;
      }
      emittedValues.insert(id);
      if (overloadedBorrowStore) {
        endConsumedCallResultLoans({value->id}, *value);
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
    if (value->kind == HirValueKind::PackFold) {
      emitPackFold(*value);
      return;
    }
    if (value->info.category == ValueCategory::Place &&
        isPlaceExpression(value->kind)) {
      (void)valueOperand(id);
      return;
    }

    MirInstruction instruction{.kind = MirInstructionKind::Compute,
                               .hirValue = value->id,
                               .operation = computeOperation(*value),
                               .literal = value->literal,
                               .enumOwner = value->enumOwner,
                               .enumValue = value->enumValue,
                               .enumVariant = value->enumVariant,
                               .payloadIndex = value->payloadIndex,
                               .intrinsic = value->intrinsic,
                               .definedFailure = value->definedFailure,
                               .functionTarget = value->functionTarget,
                               .nativeCallbackAdapter =
                                   value->nativeCallbackAdapter,
                               .lambdaTarget = value->lambdaTarget,
                               .info = value->info};
    if (instruction.operation == MirOperation::Literal) {
      instruction.literalProvenance.kind = MirLiteralProvenanceKind::Source;
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
    for (std::size_t index = 0; index < value->operands.size(); ++index) {
      const HirValueId operand = value->operands[index];
      if (instruction.operation == MirOperation::Closure &&
          index < instruction.closureCaptureModes.size() &&
          instruction.closureCaptureModes[index] == LambdaCaptureMode::Copy) {
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
    if (value->kind == HirValueKind::ArrayInitializer ||
        value->kind == HirValueKind::PayloadConstruction ||
        value->kind == HirValueKind::Unexpected ||
        value->kind == HirValueKind::Lambda) {
      for (std::size_t index = 0; index < value->operands.size(); ++index) {
        if (value->kind == HirValueKind::Lambda &&
            index < instruction.closureCaptureModes.size() &&
            instruction.closureCaptureModes[index] == LambdaCaptureMode::Copy) {
          continue;
        }
        const HirValueId operand = value->operands[index];
        transferTemporaryOut(instruction, operand);
      }
    }
    if (value->kind != HirValueKind::PayloadExtraction ||
        value->info.category != ValueCategory::Place)
      (void)initializeValueLifecycle(instruction, *value);
    (void)appendInstruction(std::move(instruction));
    endConsumedCallResultLoans(value->operands, *value);
    emittedValues.insert(id);
  }

  [[nodiscard]] MirOperand
  referenceOperand(HirValueId valueId, HirBindingId binding = 0,
                   std::optional<AccessMode> requestedAccess = {}) {
    const HirValue *value = findValue(valueId);
    if (value == nullptr) {
      return {};
    }
    emitPlaceDependencies(valueId);
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
          createLoan(MirLoanKind::Local, field, desired->access, valueId,
                     binding, 0, parent);
      recordLoanIdentity(loan, *desired, binding);
      (void)appendInstruction(
          {.kind = MirInstructionKind::Borrow,
           .hirValue = valueId,
           .operands = {{.kind = desired->access == AccessMode::Mutable
                                     ? MirOperandKind::BorrowWrite
                                     : MirOperandKind::BorrowRead,
                         .place = field,
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
      const bool exactRangeElement = std::any_of(
          source.statements.begin(), source.statements.end(),
          [&](const HirStatement &statement) {
            return statement.rangeIteration &&
                   statement.rangeIteration->elementLoan ==
                       desired->semanticLoan &&
                   statement.rangeIteration->elementAccessValue == valueId;
          });
      if (const auto found = semanticLoans.find(desired->semanticLoan);
          found != semanticLoans.end()) {
        if (existing != 0 && existing != found->second) {
          const MirLoan *transient = output.findLoan(existing);
          const MirLoan *retained = output.findLoan(found->second);
          if (transient == nullptr || retained == nullptr ||
              desired->access != AccessMode::ReadOnly ||
              !sameBorrowSource(transient->source, retained->source)) {
            valid = false;
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
        const bool projectedStoredReference =
            value->kind == HirValueKind::MemberAccess &&
            value->storedReferenceAccess && parent != nullptr &&
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

    const bool shellReference = value->info.category == ValueCategory::Place &&
                                value->info.type.kind == SemanticType::Class &&
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
      if (output.loans[loan - 1].storedDependency == 0)
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
    const bool constructed = source != nullptr && value != nullptr &&
                             !source->entry && !source->escapes &&
                             source->producedBy == valueId &&
                             value->constructorTarget.has_value();
    if (source == nullptr || source->kind != MirLoanKind::Stored ||
        (!constructed &&
         (!source->entry || source->designatedFields.empty())) ||
        source->access != access || value == nullptr ||
        !value->info.traits.containsBorrowedState) {
      valid = false;
      return 0;
    }
    const auto dependencies = source->additionalDependencies;
    const MirLoanId published = createLoan(MirLoanKind::Stored, source->source,
                                           access, valueId, 0, field);
    output.loans[published - 1].additionalDependencies = dependencies;
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
           .operands = {{.kind = access == AccessMode::Mutable
                                     ? MirOperandKind::BorrowWrite
                                     : MirOperandKind::BorrowRead,
                         .place = sourcePlace,
                         .type = findValue(valueId) == nullptr
                                     ? SemanticType::Unknown
                                     : findValue(valueId)->info.type}},
           .loan = stored,
           .definedFailure = localDefinedFailure(findValue(valueId)),
           .info = findValue(valueId) == nullptr ? ExpressionInfo{}
                                                 : findValue(valueId)->info});
      output.loans[stored - 1].escapes = true;
      constructorPublishedLoans.push_back(stored);
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
    if (scopes.empty() || !tracksLocalDrops()) {
      return;
    }
    const HirBinding *binding = findBinding(bindingId);
    if (binding != nullptr &&
        binding->info.type.kind != SemanticType::Reference &&
        binding->info.traits.drop == DropKind::Lexical &&
        !binding->info.staticStorage) {
      if (dropObligationForBinding(bindingId) == 0) {
        valid = false;
        return;
      }
      scopes.back().drops.push_back(place);
    }
  }

  [[nodiscard]] bool tracksLocalDrops() const {
    return output.kind == MirBodyKind::Function ||
           output.kind == MirBodyKind::Constructor ||
           output.kind == MirBodyKind::Destructor ||
           output.kind == MirBodyKind::Lambda;
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
    if (!tracksLocalDrops()) {
      return;
    }
    for (const HirBinding &binding : source.bindings) {
      if (binding.parameter == nullptr) {
        continue;
      }
      if (binding.info.type.kind == SemanticType::TypePack &&
          binding.info.type.concretePack) {
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
      const bool abstractStored = loan.kind == SemanticLoanKind::StoredValue &&
                                  !loan.designatedFields.empty();
      if (loan.semanticLoan == 0 || loan.parent != 0 ||
          (loan.carriers.empty() && !abstractStored)) {
        valid = false;
        continue;
      }
      const MirPlaceId sourcePlace = placeForLoan(loan);
      if (sourcePlace == 0) {
        continue;
      }
      const MirLoanKind kind = loan.kind == SemanticLoanKind::StoredValue
                                   ? MirLoanKind::Stored
                                   : MirLoanKind::Parameter;
      const MirLoanId lowered = createLoan(kind, sourcePlace, loan.access);
      output.loans[lowered - 1].entry = true;
      recordLoanIdentity(lowered, loan);
      const MirPlace *place = output.findPlace(sourcePlace);
      if (loan.lifecycleBound && place &&
          place->root == MirPlaceRootKind::Binding &&
          place->projections.empty() && place->initiallyAvailable &&
          place->type.kind == SemanticType::Class) {
        // Semantic carrier membership describes the completed flow and may
        // already omit a moved formal. MIR entry provenance must still name
        // the formal that supplied the initial ownership obligation.
        auto &carriers = output.loans[lowered - 1].carriers;
        if (std::find(carriers.begin(), carriers.end(), place->binding) ==
            carriers.end())
          carriers.push_back(place->binding);
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
    if (function == nullptr ||
        function->returnBorrowOrigin != BorrowOriginKind::Argument ||
        function->source == nullptr || function->source->body() == nullptr) {
      return;
    }
    if (function->returnBorrowParameter >= function->parameterBindings.size()) {
      return;
    }
    const HirBindingId binding =
        function->parameterBindings[function->returnBorrowParameter];
    if (bindingLoans.contains(binding)) {
      return;
    }
    const MirPlaceId sourcePlace = placeForBinding(binding);
    if (sourcePlace == 0 || scopes.empty()) {
      return;
    }
    const MirLoanId loan = createLoan(MirLoanKind::Parameter, sourcePlace,
                                      function->returnBorrowAccess, 0, binding);
    output.loans[loan - 1].entry = true;
    bindingLoans.insert_or_assign(binding, loan);
    scopes.front().loans.push_back(loan);
  }

  void
  emitScope(const Scope &scope,
            std::vector<MirDropObligationId> *sharedConsumedDrops = nullptr,
            std::vector<MirLoanId> *sharedEndedLoans = nullptr) {
    std::vector<MirDropObligationId> localConsumedDrops;
    std::vector<MirLoanId> localEndedLoans;
    std::vector<MirDropObligationId> &consumedDrops =
        sharedConsumedDrops == nullptr ? localConsumedDrops
                                       : *sharedConsumedDrops;
    std::vector<MirLoanId> &endedLoans =
        sharedEndedLoans == nullptr ? localEndedLoans : *sharedEndedLoans;
    endScopeLoansBeforeCleanup(scope, endedLoans, false);
    std::vector<MirDropObligationId> cleanup;
    cleanup.reserve(scope.drops.size());
    for (auto drop = scope.drops.rbegin(); drop != scope.drops.rend(); ++drop) {
      const MirPlace *place = output.findPlace(*drop);
      const MirDropObligationId obligation = dropObligationForLocalPlace(place);
      if (obligation != 0) {
        if (!containsDrop(consumedDrops, obligation)) {
          consumedDrops.push_back(obligation);
        }
        cleanup.push_back(obligation);
      }
      const MirLoanId lifecycleLoan =
          place != nullptr && place->root == MirPlaceRootKind::Binding
              ? lifecycleLoanForBinding(place->binding)
              : 0;
      (void)appendNormalDrop(
          obligation, *drop,
          place == nullptr ? ExpressionInfo{}
                           : ExpressionInfo{.type = place->type,
                                            .category = ValueCategory::Place,
                                            .access = place->access,
                                            .traits = place->traits},
          obligation == 0
              ? std::vector<MirLifecycleEvent>{}
              : std::vector<MirLifecycleEvent>{{.kind =
                                                    MirLifecycleEventKind::Drop,
                                                .source = obligation}},
          consumedDrops, endedLoans, 0, 0, 0, lifecycleLoan);
      if (lifecycleLoan != 0 && !containsLoan(endedLoans, lifecycleLoan)) {
        (void)appendInstruction(
            {.kind = MirInstructionKind::EndBorrow, .loan = lifecycleLoan});
        endedLoans.push_back(lifecycleLoan);
      }
      endScopeLoansBeforeCleanup(scope, endedLoans, false);
    }
    if (!cleanup.empty()) {
      const std::size_t id = output.cleanupBoundaries.size() + 1;
      output.cleanupBoundaries.push_back(
          {.id = id, .obligations = std::move(cleanup)});
      (void)appendInstruction(
          {.kind = MirInstructionKind::Lifecycle, .cleanupBoundaryEnd = id});
    }
    // A local may own an entry loan originally registered in an outer scope.
    // Its cleanup ends that loan on this path; popping only the local scope
    // would leave a stale outer entry and emit another EndBorrow at the join.
    for (const MirLoanId loan : endedLoans) {
      removeLoanFromState(loan, scopes, bindingLoans);
    }
  }

  void emitScopeExit(std::size_t keepScopes) {
    std::vector<MirDropObligationId> consumedDrops;
    std::vector<MirLoanId> endedLoans;
    for (std::size_t depth = scopes.size(); depth > keepScopes; --depth) {
      emitScope(scopes[depth - 1], &consumedDrops, &endedLoans);
    }
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
      MirProgramInitializationStep lowered{.id = step.id,
                                           .sourceUnit = step.sourceUnit,
                                           .storageKind = step.kind,
                                           .role = step.role,
                                           .symbol = step.symbol,
                                           .ownerClass = step.ownerClass,
                                           .requiresActiveCleanup =
                                               step.requiresActiveCleanup,
                                           .binding = step.binding,
                                           .storagePlace = storagePlace,
                                           .entryBlock = entryBlock};
      if (binding == nullptr || binding->info.symbol != step.symbol ||
          storagePlace == 0 || step.requiresActiveCleanup) {
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
        lowered.storageInitialization =
            statement == nullptr ? 0 : lowerVariable(*statement);
        const auto fullExpression =
            initializer == nullptr
                ? fullExpressionIds.end()
                : fullExpressionIds.find(initializer->fullExpression);
        if (fullExpression == fullExpressionIds.end()) {
          valid = false;
        } else {
          lowered.fullExpression = fullExpression->second;
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
      emitScope(scopes.back());
    }
    scopes.pop_back();
  }

  void lowerStatement(HirStatementId id) {
    const HirStatement *statement = findStatement(id);
    if (statement == nullptr) {
      valid = false;
      return;
    }
    switch (statement->kind) {
    case HirStatementKind::Block:
      scopes.push_back({});
      lowerStatements(statement->statements);
      if (!terminated()) {
        emitScope(scopes.back());
      }
      scopes.pop_back();
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
    const std::vector<Scope> incomingScopes = scopes;
    const std::vector<TemporaryDrop> incomingTemporaryDrops = temporaryDrops;
    const HirBinding *binding = findBinding(*statement.binding);
    const MirPlaceId destination = placeForBinding(*statement.binding);
    std::vector<MirOperand> operands;
    MirLoanId retainedLoan = 0;
    if (statement.value) {
      const HirValue *initializer = findValue(*statement.value);
      const std::optional<SemanticType> fallibleReferent =
          binding == nullptr ? std::nullopt
                             : mirFallibleBorrowReferent(binding->info.type);
      if (binding != nullptr && fallibleReferent && initializer != nullptr &&
          initializer->info.type == *fallibleReferent) {
        MirOperand reference =
            referenceOperand(*statement.value, *statement.binding);
        reference.place = placeForValue(*statement.value);
        retainedLoan = reference.loan;
        operands.push_back(std::move(reference));
      } else if (binding != nullptr &&
                 binding->info.type.kind == SemanticType::Reference) {
        MirOperand reference =
            referenceOperand(*statement.value, *statement.binding);
        // The loan is provenance authority; the place is the exact runtime
        // address source. Borrow-carrying values can intentionally make those
        // differ, so retain both rather than asking a backend to reconstruct
        // the source expression from carrier topology.
        reference.place = placeForValue(*statement.value);
        retainedLoan = reference.loan;
        operands.push_back(std::move(reference));
      } else {
        MirOperand initializer = valueOperand(*statement.value);
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
          if (consumesExistingCarrier) {
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
            // backing dependency alive for iteration.
            if (!isRangeIteratorBinding(*statement.binding))
              candidate->source = existing->source;
            retainedLoan = existing->id;
          } else {
            valid = false;
          }
        } else if (found == semanticLoans.end()) {
          semanticLoans.emplace(semanticLoan, retainedLoan);
        }
        const HirLoan *desired = source.findLoan(semanticLoan);
        if (desired == nullptr) {
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
          const HirValue *initializer =
              statement.value ? findValue(*statement.value) : nullptr;
          const auto arguments = initializer != nullptr
                                     ? callArgumentValues(*initializer)
                                     : std::vector<HirValueId>{};
          const bool exactAccessorParent =
              retained.kind == MirLoanKind::Stored && initializer != nullptr &&
              initializer->constructorTarget &&
              initializer->borrowOrigin == BorrowOriginKind::Argument &&
              initializer->borrowArgument < arguments.size() &&
              retained.parent ==
                  loanForValue(arguments[initializer->borrowArgument]) &&
              retainableCallResultChain(retained.parent, expectedParent,
                                        arguments[initializer->borrowArgument],
                                        desired->access);
          // A stored reference may retain the exact accessor result passed
          // to its constructor. Semantics records the stable owner; MIR also
          // keeps the transient address-producing chain alive, child-first,
          // for as long as the constructed adapter retains it.
          if (retained.parent != expectedParent &&
              !exactRuntimeTransferParent && !exactAccessorParent) {
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
               binding->info.traits.containsBorrowedState && statement.value) {
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
    } else if (temporaryIsActive(sourceObligation) &&
               output.kind == MirBodyKind::FieldInitializers) {
      // A completed per-instance field arms rollback exactly like a
      // constructor stage: the object under construction owns it from
      // normal completion, and any later defined-failure edge in this body
      // must destroy it in reverse order.
      const MirPlace *destinationPlace = output.findPlace(destination);
      const MirDropObligation *sourceDrop =
          output.findDropObligation(sourceObligation);
      if (destinationPlace != nullptr && sourceDrop != nullptr &&
          destinationPlace->root == MirPlaceRootKind::Binding &&
          destinationPlace->binding != 0 &&
          destinationPlace->projections.empty()) {
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
        projected.projections.push_back({.kind = MirProjectionKind::Index,
                                         .index = mirValueFor(*element.index)});
      }
      bindingPlaces.insert_or_assign(element.binding,
                                     appendPlace(std::move(projected)));
    }
    endFullExpressionLoans(incomingScopes);
    emitTemporaryDrops(incomingTemporaryDrops, *statement.value, statement.id);
  }

  void lowerIf(const HirStatement &statement) {
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
    current = thenBlock;
    scopes = incomingScopes;
    temporaryDrops = incomingTemporaryDrops;
    bindingLoans = incomingBindingLoans;
    endSemanticLoans(statement.thenEntryEndedLoans, statement.id);
    lowerScopedStatement(statement.body);
    const bool thenFallsThrough = !terminated();
    const std::vector<Scope> thenScopes = scopes;
    const std::vector<TemporaryDrop> thenTemporaryDrops = temporaryDrops;
    const std::unordered_map<HirBindingId, MirLoanId> thenBindingLoans =
        projectOuterBindingLoans(incomingBindingLoans, bindingLoans);
    if (thenFallsThrough) {
      terminate({.kind = MirTerminatorKind::Goto, .target = mergeBlock});
    }

    current = elseBlock;
    scopes = incomingScopes;
    temporaryDrops = incomingTemporaryDrops;
    bindingLoans = incomingBindingLoans;
    endSemanticLoans(statement.elseEntryEndedLoans, statement.id);
    lowerScopedStatement(statement.elseBranch);
    const bool elseFallsThrough = !terminated();
    const std::vector<Scope> elseScopes = scopes;
    const std::vector<TemporaryDrop> elseTemporaryDrops = temporaryDrops;
    const std::unordered_map<HirBindingId, MirLoanId> elseBindingLoans =
        projectOuterBindingLoans(incomingBindingLoans, bindingLoans);
    if (elseFallsThrough) {
      terminate({.kind = MirTerminatorKind::Goto, .target = mergeBlock});
    }

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
    } else if (thenFallsThrough) {
      scopes = thenScopes;
      temporaryDrops = thenTemporaryDrops;
      bindingLoans = thenBindingLoans;
    } else if (elseFallsThrough) {
      scopes = elseScopes;
      temporaryDrops = elseTemporaryDrops;
      bindingLoans = elseBindingLoans;
    } else {
      scopes = incomingScopes;
      temporaryDrops = incomingTemporaryDrops;
      bindingLoans = incomingBindingLoans;
      terminate({.kind = MirTerminatorKind::Unreachable,
                 .hirStatement = statement.id});
      return;
    }
    endSemanticLoans(statement);
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
    emitScope(scopes.back());
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
         .rangeLoan = loanFor(sourceRange.rangeLoan, true),
         .elementLoan =
             loanFor(sourceRange.elementLoan, sourceRange.elementLoan != 0),
         .expectedPayloadExtraction = sourceRange.expectedPayloadProjection
                                          ? mirValueFor(sourceRange.source)
                                          : MirValueId{0},
         .ownerDrop = ownerDrop,
         .conditionEntryBlock = loop->second.conditionEntry,
         .conditionBlock = loop->second.condition,
         .bodyBlock = loop->second.body,
         .incrementBlock = loop->second.increment,
         .cleanupBlock = loop->second.cleanup,
         .exitBlock = loop->second.exit,
         .arrayLength = sourceRange.arrayLength,
         .ownsRangeTemporary = sourceRange.ownsRangeTemporary,
         .expectedPayloadProjection = sourceRange.expectedPayloadProjection});
  }

  void lowerSwitch(const HirStatement &statement) {
    const std::vector<Scope> subjectScopes = scopes;
    const std::vector<TemporaryDrop> subjectTemporaryDrops = temporaryDrops;
    MirOperand subject;
    if (statement.value) {
      const HirValue *sourceValue = findValue(*statement.value);
      const MirPlaceId sourcePlace = placeForValue(*statement.value);
      if (sourceValue != nullptr && sourcePlace != 0 &&
          sourceValue->info.category == ValueCategory::Place &&
          sourceValue->info.type.kind == SemanticType::Enum &&
          (sourceValue->info.traits.drop == DropKind::Lexical ||
           std::any_of(
               statement.switchArms.begin(), statement.switchArms.end(),
               [&](const HirSwitchArm &arm) {
                 return std::any_of(
                     arm.payloadBindings.begin(), arm.payloadBindings.end(),
                     [&](const HirSwitchArm::PayloadBinding &payload) {
                       const HirBinding *binding = findBinding(payload.binding);
                       return binding != nullptr && binding->info.type.kind ==
                                                        SemanticType::Reference;
                     });
               }))) {
        emitPlaceDependencies(*statement.value);
        subject = {.kind = MirOperandKind::BorrowRead,
                   .place = sourcePlace,
                   .type = sourceValue->info.type};
      } else {
        subject = valueOperand(*statement.value);
      }
    }
    endFullExpressionLoans(subjectScopes);
    if (statement.value)
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
            : (!hasDefault && !statement.endedLoans.empty() ? appendBlock()
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
    std::vector<Scope> exitScopes = switchScopes;
    std::unordered_map<HirBindingId, MirLoanId> exitBindingLoans =
        switchBindingLoans;
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
    } else if (unmatchedBlock != exitBlock) {
      current = unmatchedBlock;
      scopes = switchScopes;
      temporaryDrops = switchTemporaryDrops;
      bindingLoans = switchBindingLoans;
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
      scopes.push_back({});
      endSemanticLoans(statement.switchArms[armIndex].entryEndedLoans,
                       statement.id);
      for (const HirSwitchArm::PayloadBinding &payload :
           statement.switchArms[armIndex].payloadBindings) {
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
        registerDrop(payload.binding, destination);
      }
      lowerStatements(statement.switchArms[armIndex].statements);
      if (!terminated()) {
        if (temporaryDrops != switchTemporaryDrops) {
          valid = false;
        }
        endSemanticLoansIfActive(statement.endedLoans, statement.id);
        emitScope(scopes.back());
        terminate({.kind = MirTerminatorKind::Goto, .target = exitBlock});
      }
      scopes.pop_back();
    }
    breakContexts.pop_back();
    current = exitBlock;
    scopes = std::move(exitScopes);
    temporaryDrops = switchTemporaryDrops;
    bindingLoans = std::move(exitBindingLoans);
  }

  void lowerReturn(const HirStatement &statement) {
    const std::vector<Scope> incomingScopes = scopes;
    const std::vector<TemporaryDrop> incomingTemporaryDrops = temporaryDrops;
    std::optional<MirOperand> result;
    std::optional<MirLoanId> returnLoan;
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
      } else if (const std::optional<SemanticType> fallibleReferent =
                     mirFallibleBorrowReferent(output.returnType);
                 fallibleReferent && findValue(*statement.value) != nullptr &&
                 findValue(*statement.value)->info.type == *fallibleReferent) {
        // A successful expected<T&, E> return transports the referent's
        // runtime address and the owner's semantic dependency separately.
        // Publish a fresh Return loan so body-local cleanup can retire the
        // source loan before the pointer is wrapped at the ABI boundary.
        MirOperand borrow =
            referenceOperand(*statement.value, 0, AccessMode::ReadOnly);
        if (borrow.loan == 0 || borrow.loan > output.loans.size()) {
          valid = false;
          return;
        }
        const MirLoan retainedLoan = output.loans[borrow.loan - 1];
        if (retainedLoan.kind == MirLoanKind::Stored && retainedLoan.entry &&
            !retainedLoan.designatedFields.empty()) {
          // A by-value carrier's loan protects its external owner, not its
          // shell. Preserve the exact designated runtime referent separately
          // just as for an ordinary reference return.
          borrow.place = placeForValue(*statement.value);
          const MirPlace *runtime = output.findPlace(borrow.place);
          if (runtime == nullptr || runtime->type != *fallibleReferent) {
            valid = false;
            return;
          }
        }
        const MirLoanId published =
            createLoan(MirLoanKind::Return, retainedLoan.source,
                       retainedLoan.access, *statement.value);
        (void)appendInstruction({.kind = MirInstructionKind::Borrow,
                                 .hirValue = *statement.value,
                                 .operands = {borrow},
                                 .loan = published,
                                 .info = findValue(*statement.value)->info});
        (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                 .hirValue = *statement.value,
                                 .loan = borrow.loan});
        removeLoanFromState(borrow.loan, scopes, bindingLoans);
        output.loans[published - 1].escapes = true;
        result = MirOperand{.kind = MirOperandKind::Loan,
                            .place = borrow.place,
                            .loan = published,
                            .type = *fallibleReferent};
        returnLoan = published;
      } else if (output.returnType.kind == SemanticType::Reference) {
        MirOperand borrow = referenceOperand(*statement.value, 0,
                                             output.returnType.referenceAccess);
        if (borrow.loan != 0) {
          MirLoanId storedCarrierEntry = 0;
          MirLoanId referenceParameterEntry = 0;
          if (function != nullptr &&
              function->returnBorrowOrigin == BorrowOriginKind::Argument &&
              function->returnBorrowParameter <
                  function->parameterBindings.size() &&
              borrow.loan <= output.loans.size()) {
            const HirBindingId parameter =
                function->parameterBindings[function->returnBorrowParameter];
            const MirLoan &candidate = output.loans[borrow.loan - 1];
            const MirPlace *source = output.findPlace(candidate.source);
            const bool carriedByParameter =
                source != nullptr &&
                source->root == MirPlaceRootKind::Binding &&
                source->binding == parameter && source->projections.empty() &&
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
            const bool exactRuntimeReferent =
                value != nullptr && output.returnType.arguments.size() == 1 &&
                runtime != nullptr &&
                runtime->type == output.returnType.arguments.front() &&
                value->info.type == output.returnType.arguments.front();
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
            if (value == nullptr || output.returnType.arguments.size() != 1 ||
                value->info.type != output.returnType.arguments.front()) {
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
              const MirPlaceId selectedPlace = placeForValue(*statement.value);
              const MirPlace *selected = output.findPlace(selectedPlace);
              const MirPlace *parameter = output.findPlace(retainedLoan.source);
              const MirPlaceId runtimeAlias =
                  selected != nullptr && parameter != nullptr &&
                          selected->root == MirPlaceRootKind::Binding &&
                          selected->binding != parameter->binding &&
                          selected->type == value->info.type
                      ? selectedPlace
                      : 0;
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
                     function->returnBorrowOrigin != BorrowOriginKind::None &&
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
              if (candidate.kind != MirLoanKind::CallResult ||
                  !candidate.carriers.empty() || candidate.escapes ||
                  !loanIsActive(scopes, transient)) {
                break;
              }
              transientChain.push_back(transient);
              transient = candidate.parent;
            }
            MirLoanId summarizedEntry = 0;
            if (function->returnBorrowOrigin == BorrowOriginKind::Argument &&
                function->returnBorrowParameter <
                    function->parameterBindings.size()) {
              const auto found = bindingLoans.find(
                  function->parameterBindings[function->returnBorrowParameter]);
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
            const bool validTail =
                function->returnBorrowOrigin == BorrowOriginKind::Receiver ||
                        function->returnBorrowOrigin == BorrowOriginKind::Global
                    ? transient == 0 ||
                          (mappedEntry != 0 && transient == mappedEntry)
                    : summarizedEntry != 0 &&
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
                if (summarizedEntry != 0) {
                  (void)appendInstruction(
                      {.kind = MirInstructionKind::EndBorrow,
                       .hirValue = *statement.value,
                       .loan = summarizedEntry});
                  removeLoanFromState(summarizedEntry, scopes, bindingLoans);
                }
                borrow = {.kind = MirOperandKind::Loan,
                          .loan = normalized,
                          .type = value->info.type};
              }
            }
          }
          MirLoan &loan = output.loans[borrow.loan - 1];
          loan.kind = MirLoanKind::Return;
          loan.escapes = true;
          for (Scope &scope : scopes) {
            std::erase(scope.loans, borrow.loan);
          }
          returnLoan = borrow.loan;
        }
        result = borrow;
      } else {
        result = valueOperand(*statement.value);
        const HirValue *value = findValue(*statement.value);
        if (value != nullptr && value->info.traits.containsBorrowedState) {
          MirLoanId retained = loanForValue(*statement.value);
          if (retained == 0 && output.returnType.kind == SemanticType::Class &&
              value->info.traits.copyable) {
            // A copied nested member has no separate local carrier binding.
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
  const FailureMetadata &failureMetadata;
  const HirBody &source;
  std::unordered_set<HirValueId> borrowedTemporaryTransfers;
  MirLoanId pendingReturnLoan = 0;
  MirDropObligationId pendingReturnDrop = 0;
  std::vector<MirLoanId> constructorPublishedLoans;
  std::unordered_map<HirValueId, SemanticLoanId> retainedInitializerLoans;
  const MirDefinedFailureEffects &definedFailureEffects;
  bool implicitZeroReturn = false;
  const HirFunctionInstance *function = nullptr;
  const HirConstructorInstance *constructor = nullptr;
  const HirDestructorInstance *destructor = nullptr;
  // The already-lowered class record is the authority for in-class field
  // initialization that completed before a source constructor body begins.
  // Constructor MIR copies only the resulting active rollback descriptors;
  // it never re-executes or interprets the initializer body.
  const MirClassInstance *constructorOwner = nullptr;
  const HirProgramInitializationPlan *programInitialization = nullptr;
  MirProgramInitializationPlan *loweredProgramInitialization = nullptr;
  MirBody output;
  MirBlockId current = 0;
  ProgramInitializationStepId currentProgramInitializationStep = 0;
  MirInstructionId nextInstruction = 1;
  MirTemporaryId nextTemporary = 1;
  bool valid = true;
  std::unordered_map<HirValueId, const HirValue *> values;
  std::unordered_map<HirStatementId, const HirStatement *> statements;
  std::unordered_map<HirBindingId, const HirBinding *> bindings;
  std::unordered_map<SymbolId, HirBindingId> localSymbols;
  std::unordered_map<SymbolId, std::size_t> lambdaCaptures;
  std::unordered_map<HirBindingId, MirPlaceId> bindingPlaces;
  std::unordered_map<HirDropObligationId, MirDropObligationId> dropObligations;
  std::unordered_map<HirFullExpressionId, HirFullExpressionId>
      fullExpressionIds;
  std::unordered_map<HirFullExpressionId, std::vector<TemporaryDrop>>
      fullExpressionTemporaryBaselines;
  std::unordered_map<HirFullExpressionId, std::vector<Scope>>
      fullExpressionScopeBaselines;
  std::unordered_map<HirValueId, MirValueId> loweredValues;
  std::unordered_map<HirValueId, MirValueId> contextualValues;
  std::unordered_map<HirValueId, MirValueId> materializedValues;
  std::unordered_map<HirValueId, MirPlaceId> valuePlaces;
  // Every extraction observes the same subject representation that selected
  // its arm, including mixed copied/borrowed bindings of passive enums.
  std::unordered_map<HirValueId, MirOperand> payloadSwitchSubjects;
  std::unordered_map<HirValueId, MirLoanId> valueLoans;
  std::unordered_map<HirBindingId, MirLoanId> bindingLoans;
  std::unordered_map<MirPlaceId, MirLoanId> packEntryLoans;
  std::unordered_map<SemanticLoanId, MirLoanId> semanticLoans;
  std::unordered_map<SemanticLoanId, MirPlaceId> semanticLoanPlaces;
  std::unordered_set<HirValueId> emittedValues;
  std::vector<Scope> scopes;
  std::vector<TemporaryDrop> temporaryDrops;
  std::optional<PreparedCallArgumentFailure> preparedCallArgumentFailure;
  std::vector<BreakContext> breakContexts;
  std::vector<ContinueContext> continueContexts;
  std::unordered_map<HirStatementId, LoweredLoopShape> loweredLoops;
};

MirBody MirLowerer::lowerBody(
    const HirProgram &program, const FailureMetadata &failureMetadata,
    const HirBody &body, MirBodyKind kind, SemanticType returnType,
    const std::vector<HirValueId> &prologueValues,
    const MirDefinedFailureEffects &definedFailureEffects, bool &valid,
    bool implicitZeroReturn,
    const std::vector<HirConstructorInitializer> *initializers,
    const HirFunctionInstance *function,
    const HirConstructorInstance *constructor, const HirLambda *lambda,
    const HirProgramInitializationPlan *programInitialization,
    MirProgramInitializationPlan *loweredProgramInitialization,
    bool compilerTrusted, const MirClassInstance *constructorOwner,
    const HirDestructorInstance *destructor) {
  MirBodyLowerer lowerer(program, failureMetadata, body, kind,
                         std::move(returnType), definedFailureEffects,
                         implicitZeroReturn, function, constructor, lambda,
                         programInitialization, loweredProgramInitialization,
                         constructorOwner, destructor);
  MirBody result = lowerer.lower(prologueValues, initializers);
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
  valid = valid && lowerer.isValid();
  return result;
}

} // namespace lang
