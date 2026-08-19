#include "gti/mir.h"

namespace lang {

namespace {

[[nodiscard]] bool isMirFixedWidthIntegerType(const SemanticType &type) {
  return type == SemanticType::Int8 || type == SemanticType::Int16 ||
         type == SemanticType::Int32 || type == SemanticType::Int64 ||
         type == SemanticType::UInt8 || type == SemanticType::UInt16 ||
         type == SemanticType::UInt32 || type == SemanticType::UInt64;
}

[[nodiscard]] std::optional<Literal>
programConstantLiteral(const ConstantValue &constant) {
  if (const auto *integer = std::get_if<ConstantInteger>(&constant)) {
    return Literal{integer->magnitude};
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
      MirProgramInitializationPlan *loweredProgramInitialization = nullptr)
      : program(program), failureMetadata(failureMetadata), source(source),
        definedFailureEffects(definedFailureEffects),
        implicitZeroReturn(implicitZeroReturn), function(function),
        constructor(constructor), programInitialization(programInitialization),
        loweredProgramInitialization(loweredProgramInitialization) {
    output.kind = kind;
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
    for (const HirStatement &statement : source.statements) {
      statements.emplace(statement.id, &statement);
    }
    for (const HirBinding &binding : source.bindings) {
      bindings.emplace(binding.id, &binding);
      if (binding.info.symbol != 0) {
        localSymbols.insert_or_assign(binding.info.symbol, binding.id);
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
              initializer.kind == ConstructorInitializerTargetKind::Field &&
              !initializer.storesReference && !initializer.generatedDefault &&
              initializer.arguments.size() == 1 &&
              (initializer.targetType.kind == SemanticType::Class ||
               initializer.targetType.kind == SemanticType::UniqueOwner);
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
          const std::vector<Scope> incomingScopes = scopes;
          const std::vector<TemporaryDrop> incomingTemporaryDrops =
              temporaryDrops;
          for (const HirValueId argument : initializer.arguments) {
            (void)emitValue(argument);
          }
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
          const bool explicitScalarFieldStage =
              constructorOwner != nullptr &&
              field != constructorOwner->fields.end() &&
              initializer.kind == ConstructorInitializerTargetKind::Field &&
              !initializer.storesReference && !initializer.constructorTarget &&
              !initializer.generatedDefault &&
              initializer.arguments.size() == 1 &&
              field->info.type == initializer.targetType &&
              (isMirFixedWidthIntegerType(initializer.targetType) ||
               initializer.targetType == SemanticType::Bool ||
               initializer.targetType == SemanticType::Char) &&
              field->info.traits.drop == DropKind::Trivial &&
              !field->info.traits.containsBorrowedState;
          if (explicitScalarFieldStage) {
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
          const MirDropObligationId classStageSource =
              constructorOwner != nullptr &&
                      field != constructorOwner->fields.end() &&
                      initializer.kind ==
                          ConstructorInitializerTargetKind::Field &&
                      !initializer.storesReference &&
                      !initializer.generatedDefault &&
                      initializer.arguments.size() == 1 &&
                      field->info.type == initializer.targetType &&
                      (initializer.targetType.kind == SemanticType::Class ||
                       initializer.targetType.kind ==
                           SemanticType::UniqueOwner) &&
                      !field->info.traits.containsBorrowedState
                  ? dropObligationForValue(initializer.arguments.front())
                  : 0;
          const bool explicitClassFieldStage =
              classStageSource != 0 && temporaryIsActive(classStageSource);
          if (explicitClassFieldStage) {
            const MirPlaceId destination =
                appendPlace({.root = MirPlaceRootKind::This,
                             .projections = {{.kind = MirProjectionKind::Field,
                                              .field = initializer.field}},
                             .type = initializer.targetType,
                             .access = AccessMode::Mutable,
                             .traits = field->info.traits,
                             .sourceValue = initializer.arguments.front()});
            const MirDropObligation *sourceDrop =
                output.findDropObligation(classStageSource);
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
            appendReparentOrTypedTransfer(initialize, classStageSource,
                                          rollback);
            (void)removeTemporary(classStageSource);
            (void)appendInstruction(std::move(initialize));
            constructorRollback.push_back(rollback);
          }
          if (initializer.storesReference &&
              initializer.arguments.size() == 1) {
            markStoredBorrow(initializer.arguments.front(), initializer.field,
                             initializer.borrowAccess);
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
            if (explicitClassFieldStage && index == 0) {
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
    valid = verifyMirBody(output).valid() && valid;
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

  struct PreparedCallInput {
    MirOperand operand;
    MirDropObligationId parameterDrop = 0;
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
    const MirDropObligation *drop = output.findDropObligation(obligation);
    if (drop == nullptr || !drop->dropType.destructor ||
        *drop->dropType.destructor == 0) {
      return FailurePropagationKind::None;
    }
    if (*drop->dropType.destructor > definedFailureEffects.destructors.size()) {
      return FailurePropagationKind::Destructor;
    }
    return definedFailureEffects.destructors[*drop->dropType.destructor - 1]
               ? FailurePropagationKind::Destructor
               : FailurePropagationKind::None;
  }

  [[nodiscard]] static MirDropType lowerDropType(const HirDropType &type) {
    return {.type = type.type,
            .classInstance = type.classInstance,
            .lambdaInstance = type.lambdaInstance,
            .destructor = type.destructor,
            .requiresActiveCleanup = type.requiresActiveCleanup};
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
  dropObligationForValue(HirValueId valueId, MirPlaceId place = 0) {
    const HirValue *value = findValue(valueId);
    if (value == nullptr || !value->dropObligation) {
      return 0;
    }
    const HirDropObligation *obligation =
        source.findDropObligation(*value->dropObligation);
    return obligation == nullptr ? 0 : ensureDropObligation(*obligation, place);
  }

  [[nodiscard]] std::optional<MirDropType>
  preparedParameterDropType(const SemanticType &type) const {
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
    return MirDropType{.type = type,
                       .classInstance = instance->id,
                       .destructor = instance->destructor,
                       .requiresActiveCleanup =
                           instance->requiresActiveCleanup};
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
        if (!initializesObligation ||
            (definition->destination &&
             *definition->destination != obligation->place)) {
          valid = false;
          continue;
        }
        // A branch-only owning value needs executable storage that remains
        // nameable after the CFG merge. Retarget its existing producer to the
        // hidden result slot; this is direct materialization, not an extra
        // observable move.
        definition->destination = obligation->place;
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
    const MirInstructionId instruction =
        appendInstruction(std::move(drop), true, false);
    if (instruction != 0 && routeFailureEdgesHere() &&
        normalizedDestructorPropagation(obligation) ==
            FailurePropagationKind::Destructor) {
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

  void emitFailureTemporaryCleanup(
      const std::vector<TemporaryDrop> &activeTemporaries,
      const std::vector<MirDropObligationId> &consumedDrops) {
    std::vector<MirDropObligationId> cleanup;
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
      cleanup.push_back(candidate->obligation);
    }
    appendFailureCleanupBoundary(std::move(cleanup));
  }

  void
  emitFailureScopeCleanup(const Scope &scope,
                          const std::vector<MirDropObligationId> &consumedDrops,
                          const std::vector<MirLoanId> &endedLoans) {
    for (auto loan = scope.loans.rbegin(); loan != scope.loans.rend(); ++loan) {
      if (containsLoan(endedLoans, *loan)) {
        continue;
      }
      (void)appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = *loan});
    }
    std::vector<MirDropObligationId> cleanup;
    cleanup.reserve(scope.drops.size());
    for (auto drop = scope.drops.rbegin(); drop != scope.drops.rend(); ++drop) {
      const MirPlace *place = output.findPlace(*drop);
      const MirDropObligationId obligation =
          place != nullptr && place->root == MirPlaceRootKind::Binding
              ? dropObligationForBinding(place->binding)
              : 0;
      if (containsDrop(consumedDrops, obligation)) {
        continue;
      }
      if (place == nullptr || obligation == 0) {
        valid = false;
        continue;
      }
      (void)appendFailureCleanupDrop(
          obligation, *drop,
          ExpressionInfo{.type = place->type,
                         .category = ValueCategory::Place,
                         .access = place->access,
                         .traits = place->traits});
      cleanup.push_back(obligation);
    }
    appendFailureCleanupBoundary(std::move(cleanup));
  }

  void appendFailureControlFlow(
      MirInstructionId producerInstruction,
      MirDropObligationId successResultDrop,
      const std::vector<MirDropObligationId> &consumedDrops = {},
      const std::vector<MirLoanId> &endedLoans = {}) {
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
    if (output.kind == MirBodyKind::Constructor ||
        output.kind == MirBodyKind::FieldInitializers) {
      emitConstructorFailureDrain(consumedDrops, endedLoans);
    } else {
      emitFailureTemporaryCleanup(temporaryDrops, consumedDrops);
      for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        emitFailureScopeCleanup(*scope, consumedDrops, endedLoans);
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
  // ordering rule.
  void emitConstructorFailureDrain(
      const std::vector<MirDropObligationId> &consumedDrops,
      const std::vector<MirLoanId> &endedLoans) {
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
      for (auto loan = scope->loans.rbegin(); loan != scope->loans.rend();
           ++loan) {
        if (!containsLoan(endedLoans, *loan)) {
          (void)appendInstruction(
              {.kind = MirInstructionKind::EndBorrow, .loan = *loan});
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
    for (const Scope &scope : scopes) {
      for (const MirPlaceId placeId : scope.drops) {
        const MirPlace *place = output.findPlace(placeId);
        const MirDropObligationId obligation =
            place != nullptr && place->root == MirPlaceRootKind::Binding
                ? dropObligationForBinding(place->binding)
                : 0;
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
      (void)appendFailureCleanupDrop(
          entry.obligation, obligation->place,
          ExpressionInfo{.type = place->type,
                         .category = ValueCategory::Place,
                         .access = place->access,
                         .traits = place->traits},
          entry.conditional);
      cleanup.push_back(entry.obligation);
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
      HirStatementId hirStatement = 0, std::size_t constructorInitializer = 0) {
    MirInstruction drop{.kind = MirInstructionKind::Drop,
                        .hirValue = hirValue,
                        .hirStatement = hirStatement,
                        .constructorInitializer = constructorInitializer,
                        .destination = destination,
                        .info = std::move(info),
                        .lifecycle = std::move(lifecycle)};
    drop.definedFailure.propagation =
        normalizedDestructorPropagation(obligation);
    const MirInstructionId instruction =
        appendInstruction(std::move(drop), true, false);
    if (instruction != 0 && routeFailureEdgesHere() &&
        normalizedDestructorPropagation(obligation) ==
            FailurePropagationKind::Destructor) {
      appendFailureControlFlow(instruction, 0, consumedDrops, endedLoans);
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

  MirInstructionId appendInstruction(MirInstruction instruction,
                                     bool inheritSourceMetadata = true,
                                     bool routeFailureControlFlow = true) {
    MirBlock *block = currentBlock();
    if (block == nullptr || terminated()) {
      return 0;
    }
    if (inheritSourceMetadata && instruction.hirValue != 0) {
      if (const HirValue *source = findValue(instruction.hirValue)) {
        if (source->unsafeOperation != UnsafeOperationKind::None) {
          instruction.unsafeOperation = source->unsafeOperation;
        }
        if (source->ownership) {
          instruction.ownership = source->ownership;
        }
      }
    }
    instruction.localFailureSites.clear();
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
                          std::size_t constructorInitializer = 0) {
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
      (void)appendNormalDrop(candidate->obligation, obligation->place,
                             ExpressionInfo{.type = obligation->dropType.type,
                                            .category = ValueCategory::Place,
                                            .access = AccessMode::Mutable,
                                            .traits = place->traits},
                             {{.kind = MirLifecycleEventKind::Drop,
                               .source = candidate->obligation,
                               .conditional = candidate->conditional}},
                             consumedDrops, {}, hirValue, hirStatement,
                             constructorInitializer);
    }
    temporaryDrops = baseline;
    const auto boundary = std::find_if(
        source.fullExpressions.begin(), source.fullExpressions.end(),
        [&](const HirFullExpression &expression) {
          if (constructorInitializer != 0) {
            return expression.constructorInitializer == constructorInitializer;
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
    (void)appendInstruction({.kind = MirInstructionKind::Lifecycle,
                             .hirValue = hirValue,
                             .hirStatement = hirStatement,
                             .fullExpressionEnd = boundaryId},
                            output.kind != MirBodyKind::Module ||
                                currentProgramInitializationStep == 0);
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

  [[nodiscard]] MirPlaceId placeForLoan(const HirLoan &loan) {
    if (const auto found = semanticLoanPlaces.find(loan.semanticLoan);
        found != semanticLoanPlaces.end()) {
      return found->second;
    }

    MirPlace lowered;
    if (loan.place.receiver) {
      lowered.root = MirPlaceRootKind::This;
      lowered.access = loan.access;
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

    for (const SemanticLoanPlaceProjection &projection :
         loan.place.projections) {
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
    const bool enumeration = value.info.type.kind == SemanticType::Enum;
    const bool negativeInteger = integer != nullptr && integer->negative;
    if (enumeration) {
      if (integer == nullptr || value.info.type.enumId == 0) {
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
           .enumValue = EnumConstant{.negative = integer->negative,
                                     .magnitude = integer->magnitude},
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
    if (isCallLike(*value)) {
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
        value->storedReferenceAccess && !value->operands.empty()) {
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
      return {
          .kind = MirOperandKind::Loan, .loan = existing, .type = parameter};
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
    std::size_t argumentCount = value.parameterTypes.size();
    if (value.kind == HirValueKind::Call) {
      if (const auto *call = dynamic_cast<const Call *>(value.source)) {
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
    if (value.kind == HirValueKind::Call && !value.operands.empty()) {
      const HirValue *callee = findValue(value.operands.front());
      if (callee != nullptr && callee->kind == HirValueKind::MemberAccess &&
          !callee->operands.empty()) {
        return callee->unsafeOperation == UnsafeOperationKind::RawMember
                   ? std::optional<HirValueId>{callee->id}
                   : std::optional<HirValueId>{callee->operands.front()};
      }
      if (value.receiver) {
        return value.receiver;
      }
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
    if (value.unsafeOperation == UnsafeOperationKind::AddressOf) {
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
    case HirValueKind::LayoutQuery:
      return MirOperation::Literal;
    case HirValueKind::Literal:
      return MirOperation::Literal;
    case HirValueKind::PackFold:
      return MirOperation::PackFold;
    case HirValueKind::PackExpansion:
      return MirOperation::PackExpansion;
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
      composed.projections.insert(
          composed.projections.end(),
          place->projections.begin() +
              static_cast<std::ptrdiff_t>(projectionOffset),
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

  [[nodiscard]] MirPlaceId borrowOriginPlace(const HirValue &value,
                                             const MirInstruction &call) {
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
             SymbolId storedField = 0, MirLoanId parent = 0) {
    const MirLoanId id = output.loans.size() + 1;
    output.loans.push_back(
        {.id = id,
         .parent = parent,
         .kind = kind,
         .source = sourcePlace,
         .access = access,
         .producedBy = producedBy,
         .carriers = binding == 0 ? std::vector<HirBindingId>{}
                                  : std::vector<HirBindingId>{binding},
         .storedField = storedField});
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
                   bool stageOwningParameter) {
    ExpressionInfo info{.type = type,
                        .category = ValueCategory::Value,
                        .access = AccessMode::ReadOnly,
                        .traits = semanticTraits(type)};
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
                         .result = result,
                         .operands = {std::move(operand)},
                         .info = info};
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
                     inputKind == HirCallInputKind::MoveValue)) {
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
    } else if (inputKind == HirCallInputKind::MoveValue) {
      transferTemporaryOut(input, sourceValue);
    }
    (void)appendInstruction(std::move(input));
    registerTemporary(parameterDrop);
    return {.operand = {.kind = MirOperandKind::Value,
                        .value = result,
                        .type = type},
            .parameterDrop = parameterDrop};
  }

  void emitCall(const HirValue &value) {
    DefinedFailureOperation failure = value.definedFailure;
    if (failure.localOrigins.empty()) {
      failure.propagation = normalizedCallPropagation(
          failure.propagation, value.dispatch, value.functionTarget);
    }
    MirInstruction call{.kind = MirInstructionKind::Call,
                        .hirValue = value.id,
                        .callSite = value.callPlan ? value.id : 0,
                        .result = resultFor(value),
                        .intrinsic = value.intrinsic,
                        .synchronization = value.synchronization,
                        .definedFailure = std::move(failure),
                        .dispatch = value.dispatch,
                        .dispatchOwner = value.dispatchOwner,
                        .functionTarget = value.functionTarget,
                        .constructorTarget = value.constructorTarget,
                        .constructorKind = value.constructorKind,
                        .lambdaTarget = value.lambdaTarget,
                        .callableArguments = value.callableArguments,
                        .callableBoundary = value.callableBoundary,
                        .callableInvocation = value.callableInvocation,
                        .info = value.info};

    const std::vector<HirValueId> arguments = callArgumentValues(value);
    std::optional<MirOperand> sourceReceiver;
    std::vector<MirOperand> sourceArguments;
    std::vector<MirDropObligationId> preparedParameterDrops;
    sourceArguments.reserve(arguments.size());
    preparedParameterDrops.reserve(arguments.size() + 1);

    if (value.callPlan) {
      if (value.callPlan->receiver) {
        const HirCallReceiver &receiver = *value.callPlan->receiver;
        const AccessMode access =
            receiver.kind == HirCallInputKind::MutableBorrow
                ? AccessMode::Mutable
                : AccessMode::ReadOnly;
        sourceReceiver = receiverOperand(receiver.value, access);
        PreparedCallInput prepared = prepareCallInput(
            value.id, receiver.value, receiver.type, receiver.kind,
            MirCallInputRole::Receiver, 0, *sourceReceiver, true);
        call.receiver = std::move(prepared.operand);
        if (prepared.parameterDrop != 0) {
          preparedParameterDrops.push_back(prepared.parameterDrop);
        }
      }
    } else if (const std::optional<HirValueId> receiver =
                   receiverValue(value)) {
      const AccessMode access = receiverAccess(value);
      call.receiver = receiverOperand(*receiver, access);
      if (const HirValue *source = findValue(*receiver);
          source != nullptr &&
          source->unsafeOperation == UnsafeOperationKind::RawMember) {
        call.unsafeOperation = UnsafeOperationKind::RawMember;
      }
    }

    if (!value.callPlan && value.lambdaTarget && !value.operands.empty() &&
        !value.functionTarget) {
      const std::size_t argumentCount = arguments.size();
      if (value.operands.size() > argumentCount) {
        call.receiver = valueOperand(value.operands.front());
      }
    }

    if (value.callPlan) {
      for (const HirCallArgument &argument : value.callPlan->arguments) {
        call.parameterTypes.push_back(argument.parameterType);
        const std::optional<PreparedCallArgumentFailure> enclosingFailure =
            preparedCallArgumentFailure;
        if (!preparedParameterDrops.empty()) {
          preparedCallArgumentFailure =
              PreparedCallArgumentFailure{.argument = argument.value};
        }
        MirOperand sourceArgument =
            argument.kind == HirCallInputKind::CopyValue
                ? copyArgumentOperand(argument.value, argument.parameterType)
                : argumentOperand(argument.value, argument.parameterType);
        preparedCallArgumentFailure = enclosingFailure;
        sourceArguments.push_back(std::move(sourceArgument));
        PreparedCallInput prepared = prepareCallInput(
            value.id, argument.value, argument.parameterType, argument.kind,
            MirCallInputRole::Argument, argument.parameterIndex,
            sourceArguments.back(), true);
        call.operands.push_back(std::move(prepared.operand));
        if (prepared.parameterDrop != 0) {
          preparedParameterDrops.push_back(prepared.parameterDrop);
        }
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
      }
    }

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
    }
    MirInstruction originCall = call;
    if (sourceReceiver) {
      originCall.receiver = sourceReceiver;
    }
    if (!sourceArguments.empty()) {
      originCall.operands = sourceArguments;
    }
    const MirPlaceId origin = borrowOriginPlace(value, originCall);
    call.borrowOrigin = value.borrowOrigin;
    call.borrowArgument = value.borrowArgument;
    call.borrowAccess = value.borrowAccess;
    call.borrowPlace = value.borrowPlace;
    if (value.borrowOrigin != BorrowOriginKind::None && origin != 0) {
      const MirLoanKind kind = value.info.traits.containsBorrowedState
                                   ? MirLoanKind::Stored
                                   : MirLoanKind::CallResult;
      MirLoanId parent = 0;
      if (kind == MirLoanKind::CallResult && originLoan != 0 &&
          originLoan <= output.loans.size()) {
        const AccessMode parentAccess = output.loans[originLoan - 1].access;
        if (parentAccess == AccessMode::Mutable ||
            value.borrowAccess == AccessMode::ReadOnly) {
          parent = originLoan;
        }
      }
      const MirLoanId loan =
          createLoan(kind, origin, value.borrowAccess, value.id, 0, 0, parent);
      call.loan = loan;
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
    const MirDropObligationId resultDrop = dropObligationForValue(value.id);
    // The success-edge result form stays bounded to full-expression roots: a
    // nested owning result must be re-homed by initializeValueLifecycle into
    // a temporary place so a conditional join can drop it without using an
    // arm-local value, and that re-homed Initialize lifecycle then keeps the
    // nested call outside the routed family until the remaining owning-result
    // materialization work lands.
    const bool successEdgeResult =
        resultDrop != 0 && routeFailureEdgesHere() &&
        isFullExpressionRoot(value.id) && !call.definedFailure.empty() &&
        !call.destination && !call.loan && !value.ownership;
    if (successEdgeResult) {
      call.successResultDrop = resultDrop;
    } else {
      (void)initializeValueLifecycle(call, value);
    }
    (void)appendInstruction(std::move(call));
  }

  void emitConstruct(const HirValue &value) {
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
    const std::vector<HirValueId> arguments = callArgumentValues(value);
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
                             false)
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
            value.constructorKind == ConstructorKind::Ordinary
                ? argumentOperand(arguments[index], parameter)
                : valueOperand(arguments[index]));
      }
    }
    MirInstruction originConstruct = construct;
    if (!sourceArguments.empty()) {
      originConstruct.operands = sourceArguments;
    }
    const MirPlaceId origin = borrowOriginPlace(value, originConstruct);
    construct.borrowOrigin = value.borrowOrigin;
    construct.borrowArgument = value.borrowArgument;
    construct.borrowAccess = value.borrowAccess;
    if (value.borrowOrigin != BorrowOriginKind::None && origin != 0) {
      const MirLoanId loan =
          createLoan(MirLoanKind::Stored, origin, value.borrowAccess, value.id);
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
        if (parameter.kind != SemanticType::Reference) {
          transferTemporaryOut(construct, arguments[index]);
        }
      }
    }
    // Mirror the ordinary-call result contract: a full-expression-root
    // construction that may raise initializes its cleanup-owning result only
    // on the success edge, so failure cleanup can never drop an object whose
    // construction did not complete. A nested construction keeps the direct
    // initialize lifecycle, because its result would otherwise be an
    // arm-local value at a conditional join.
    const MirDropObligationId resultDrop = dropObligationForValue(value.id);
    const bool successEdgeResult =
        resultDrop != 0 && routeFailureEdgesHere() &&
        isFullExpressionRoot(value.id) && !construct.definedFailure.empty() &&
        !construct.destination && !construct.loan && !value.ownership;
    if (successEdgeResult) {
      construct.successResultDrop = resultDrop;
    } else {
      (void)initializeValueLifecycle(construct, value);
    }
    (void)appendInstruction(std::move(construct));
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
    } else if (value->contextualBoolTarget) {
      const AccessMode access = receiverAccess(*value->contextualBoolTarget);
      const FailurePropagationKind propagation = normalizedCallPropagation(
          value->dispatch == CallDispatch::Virtual
              ? FailurePropagationKind::VirtualCall
              : FailurePropagationKind::DirectCall,
          value->dispatch, value->contextualBoolTarget);
      (void)appendInstruction({.kind = MirInstructionKind::Call,
                               .hirValue = id,
                               .result = result,
                               .receiver = receiverOperand(id, access),
                               .definedFailure = {.propagation = propagation},
                               .dispatch = value->dispatch,
                               .dispatchOwner = value->dispatchOwner,
                               .functionTarget = value->contextualBoolTarget,
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
    for (std::size_t depth = scopes.size(); depth > 0; --depth) {
      const std::vector<MirLoanId> &before = baseline[depth - 1].loans;
      const std::vector<MirLoanId> &after = scopes[depth - 1].loans;
      if (after.size() < before.size() ||
          !std::equal(before.begin(), before.end(), after.begin())) {
        valid = false;
        continue;
      }
      for (std::size_t index = after.size(); index > before.size(); --index) {
        (void)appendInstruction(
            {.kind = MirInstructionKind::EndBorrow, .loan = after[index - 1]});
      }
    }
  }

  void endFullExpressionLoans(const std::vector<Scope> &baseline) {
    if (baseline.size() != scopes.size()) {
      valid = false;
      return;
    }
    for (std::size_t depth = scopes.size(); depth > 0; --depth) {
      const std::vector<MirLoanId> &before = baseline[depth - 1].loans;
      std::vector<MirLoanId> &after = scopes[depth - 1].loans;
      if (after.size() < before.size() ||
          !std::equal(before.begin(), before.end(), after.begin())) {
        valid = false;
        continue;
      }
      for (std::size_t index = after.size(); index > before.size(); --index) {
        const MirLoan &loan = output.loans[after[index - 1] - 1];
        if (loan.carriers.empty() && !loan.escapes) {
          (void)appendInstruction(
              {.kind = MirInstructionKind::EndBorrow, .loan = loan.id});
        }
      }
      after.erase(std::remove_if(after.begin() +
                                     static_cast<std::ptrdiff_t>(before.size()),
                                 after.end(),
                                 [&](MirLoanId loan) {
                                   const MirLoan &candidate =
                                       output.loans[loan - 1];
                                   return candidate.carriers.empty() &&
                                          !candidate.escapes;
                                 }),
                  after.end());
    }
  }

  void endReturnExpressionLoans(const std::vector<Scope> &baseline) {
    if (baseline.size() != scopes.size()) {
      valid = false;
      return;
    }
    for (std::size_t depth = scopes.size(); depth > 0; --depth) {
      const std::vector<MirLoanId> &before = baseline[depth - 1].loans;
      std::vector<MirLoanId> &after = scopes[depth - 1].loans;
      for (auto candidate = after.begin(); candidate != after.end();) {
        const bool added =
            std::find(before.begin(), before.end(), *candidate) == before.end();
        const MirLoan *loan = output.findLoan(*candidate);
        if (added && loan != nullptr && loan->carriers.empty() &&
            !loan->escapes) {
          (void)appendInstruction(
              {.kind = MirInstructionKind::EndBorrow, .loan = loan->id});
          candidate = after.erase(candidate);
        } else {
          ++candidate;
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
      while (loan != 0 && loan <= output.loans.size() &&
             visited.insert(loan).second && loanIsActive(scopes, loan)) {
        const MirLoan &candidate = output.loans[loan - 1];
        if (candidate.kind != MirLoanKind::CallResult ||
            !candidate.carriers.empty() || candidate.escapes) {
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
  childFirstLoans(const std::vector<SemanticLoanId> &loans) {
    std::vector<MirLoanId> result;
    result.reserve(loans.size());
    for (const SemanticLoanId semanticLoan : loans) {
      const MirLoanId loan = mirLoanForSemanticLoan(semanticLoan);
      if (loan != 0 &&
          std::find(result.begin(), result.end(), loan) == result.end()) {
        result.push_back(loan);
      }
    }
    const auto depth = [&](MirLoanId loan) {
      std::size_t result = 0;
      std::unordered_set<MirLoanId> visited;
      while (loan != 0 && loan <= output.loans.size() &&
             visited.insert(loan).second) {
        ++result;
        loan = output.loans[loan - 1].parent;
      }
      return result;
    };
    std::stable_sort(result.begin(), result.end(),
                     [&](MirLoanId left, MirLoanId right) {
                       return depth(left) > depth(right);
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
      if (mustReachBorrowedReturn(loan)) {
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
      if (mustReachBorrowedReturn(loan)) {
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
    for (const MirLoanId loan : childFirstLoans(loans)) {
      if (mustReachBorrowedReturn(loan)) {
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

    current = mergeBlock;
    scopes = incomingScopes;
    temporaryDrops =
        mergeTemporaryDrops(shortCircuitTemporaryDrops, rightTemporaryDrops);
    (void)appendInstruction({.kind = MirInstructionKind::Load,
                             .hirValue = value.id,
                             .result = resultFor(value),
                             .operands = {{.kind = MirOperandKind::Copy,
                                           .place = temporary,
                                           .type = SemanticType::Bool}},
                             .info = value.info});
    emittedValues.insert(value.id);
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
    const bool hasResult = value.info.type.kind != SemanticType::Void;
    const MirPlaceId temporary =
        hasResult ? conditionalTemporary(value) : MirPlaceId{0};
    const MirDropObligationId resultObligation =
        hasResult ? dropObligationForValue(value.id, temporary) : 0;
    std::vector<TemporaryDrop> thenTemporaryDrops;
    std::vector<TemporaryDrop> elseTemporaryDrops;
    const auto emitArm = [&](MirBlockId block, HirValueId arm) {
      current = block;
      scopes = incomingScopes;
      temporaryDrops = incomingTemporaryDrops;
      if (hasResult) {
        MirInstruction initialize{
            .kind = MirInstructionKind::Initialize,
            .hirValue = value.id,
            .destination = temporary,
            .operands = {valueOperand(arm)},
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
      endAddedLoans(incomingScopes);
      terminate({.kind = MirTerminatorKind::Goto, .target = mergeBlock});
      return temporaryDrops;
    };

    thenTemporaryDrops = emitArm(thenBlock, value.operands[1]);
    elseTemporaryDrops = emitArm(elseBlock, value.operands[2]);

    current = mergeBlock;
    scopes = incomingScopes;
    temporaryDrops =
        mergeTemporaryDrops(thenTemporaryDrops, elseTemporaryDrops);
    if (hasResult) {
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

  // An ordered compound update reads its place once, checks the arithmetic,
  // and commits the result. Representing those stages explicitly keeps the
  // schedule inside MIR's primitive vocabulary instead of one opaque
  // compound instruction a backend would have to re-derive.
  //
  // The bounded family requires the read, the operation, and the commit to
  // share one fixed-width integer domain. A narrowing form folds a checked
  // conversion into the same HIR-authored failure origin, and MIR may not
  // split that origin across stages, so it stays on the compound instruction
  // until semantics gives each stage its own origin.
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

  [[nodiscard]] bool orderedCompoundEligible(const HirValue &value,
                                             MirPlaceId destination,
                                             const SemanticType &operandType,
                                             MirOperation operation) {
    const MirPlace *place = output.findPlace(destination);
    return place != nullptr && place->projections.size() <= 1 &&
           fixedWidthIntegerType(value.info.type) &&
           place->type == value.info.type && operandType == value.info.type &&
           value.info.category == ValueCategory::Value &&
           value.info.traits.drop == DropKind::Trivial &&
           !value.info.traits.containsBorrowedState && !value.ownership &&
           value.unsafeOperation == UnsafeOperationKind::None &&
           orderedCompoundOutcomes(value.definedFailure, operation);
  }

  // Emits read -> checked operation -> commit for one eligible place update.
  // `result` names the MIR value the HIR value denotes: the pre-update read
  // for a postfix form, the committed operand otherwise.
  void emitOrderedCompoundSchedule(const HirValue &value,
                                   MirPlaceId destination,
                                   MirOperation operation,
                                   MirOperand sourceOperand, bool postfix,
                                   bool commitResult) {
    const MirValueId canonical = mirValueFor(value);
    const MirValueId readValue =
        postfix ? canonical : appendValue(value.id, value.info);
    (void)appendInstruction({.kind = MirInstructionKind::Load,
                             .hirValue = value.id,
                             .result = readValue,
                             .operands = {{.kind = MirOperandKind::Copy,
                                           .place = destination,
                                           .type = value.info.type}},
                             .info = value.info},
                            false);
    const MirValueId computedValue =
        postfix || commitResult ? appendValue(value.id, value.info) : canonical;
    (void)appendInstruction({.kind = MirInstructionKind::Compute,
                             .hirValue = value.id,
                             .result = computedValue,
                             .operands = {{.kind = MirOperandKind::Value,
                                           .value = readValue,
                                           .type = value.info.type},
                                          std::move(sourceOperand)},
                             .operation = operation,
                             .definedFailure = value.definedFailure,
                             .info = value.info},
                            false);
    // The commit keeps assignment's ordinary result contract; a prefix or
    // compound form denotes it, and a postfix form discards it in favor of
    // the pre-update read.
    (void)appendInstruction(
        {.kind = MirInstructionKind::Assign,
         .hirValue = value.id,
         .result = commitResult ? canonical : appendValue(value.id, value.info),
         .destination = destination,
         .operands = {{.kind = MirOperandKind::Value,
                       .value = computedValue,
                       .type = value.info.type}},
         .operation = MirOperation::Assign,
         .info = value.info});
    emittedValues.insert(value.id);
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
    if (orderedCompoundEligible(value, destination, value.info.type,
                                arithmetic)) {
      emitOrderedCompoundSchedule(value, destination, arithmetic,
                                  {.kind = MirOperandKind::Constant,
                                   .literal = Literal{std::uint64_t{1}},
                                   .type = value.info.type},
                                  postfix, false);
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

  void emitValue(HirValueId id) {
    if (id == 0 || emittedValues.contains(id)) {
      return;
    }
    const HirValue *value = findValue(id);
    if (value == nullptr) {
      valid = false;
      return;
    }

    if (value->programConstantSubstitution) {
      emitProgramConstantSubstitution(*value);
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
      if (value->kind == HirValueKind::MemberSet && !value->operands.empty()) {
        if (value->unsafeOperation == UnsafeOperationKind::RawMember) {
          (void)valueOperand(value->operands.front());
        } else {
          emitPlaceDependencies(value->operands.front());
        }
      } else if (value->kind == HirValueKind::IndexSet &&
                 value->operands.size() >= 2) {
        if (value->unsafeOperation == UnsafeOperationKind::RawIndex) {
          (void)valueOperand(value->operands[0]);
        } else {
          emitPlaceDependencies(value->operands[0]);
        }
        (void)valueOperand(value->operands[1]);
      } else if (value->kind == HirValueKind::DereferenceSet &&
                 !value->operands.empty()) {
        (void)valueOperand(value->operands.front());
      }
      const MirPlaceId destination = destinationFor(*value);
      attachPlaceIdentity(destination, *value);
      std::vector<MirOperand> operands;
      MirDropObligationId sourceObligation = 0;
      if (!value->operands.empty()) {
        const HirValueId sourceValue = value->operands.back();
        operands.push_back(valueOperand(sourceValue));
        sourceObligation = dropObligationForValue(sourceValue);
      }
      const MirOperation assignOperation =
          value->operation ? assignmentOperation(*value->operation)
                           : MirOperation::None;
      const MirOperation compoundOperation =
          orderedCompoundArithmetic(assignOperation);
      if (compoundOperation != MirOperation::None && operands.size() == 1 &&
          sourceObligation == 0 &&
          orderedCompoundEligible(*value, destination, operands.front().type,
                                  compoundOperation)) {
        emitOrderedCompoundSchedule(*value, destination, compoundOperation,
                                    std::move(operands.front()), false, true);
        return;
      }
      MirInstruction assign{.kind = MirInstructionKind::Assign,
                            .hirValue = value->id,
                            .result = resultFor(*value),
                            .destination = destination,
                            .operands = std::move(operands),
                            .operation = assignOperation,
                            .definedFailure = value->definedFailure,
                            .info = value->info};
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
      (void)appendInstruction(std::move(assign));
      emittedValues.insert(id);
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
    if (value->kind == HirValueKind::Call || value->functionTarget) {
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
    if (value->info.category == ValueCategory::Place &&
        isPlaceExpression(value->kind)) {
      (void)valueOperand(id);
      return;
    }

    MirInstruction instruction{.kind = MirInstructionKind::Compute,
                               .hirValue = value->id,
                               .result = resultFor(*value),
                               .operation = computeOperation(*value),
                               .literal = value->literal,
                               .enumOwner = value->enumOwner,
                               .enumValue = value->enumValue,
                               .enumVariant = value->enumVariant,
                               .payloadIndex = value->payloadIndex,
                               .intrinsic = value->intrinsic,
                               .definedFailure = value->definedFailure,
                               .lambdaTarget = value->lambdaTarget,
                               .info = value->info};
    if (instruction.operation == MirOperation::Literal) {
      instruction.literalProvenance.kind = MirLiteralProvenanceKind::Source;
    }
    if (instruction.operation == MirOperation::PackFold) {
      instruction.packFoldSymbol = value->packFoldSymbol;
      instruction.packFoldParameter = value->packFoldParameter;
      instruction.packFoldFunction = value->packFoldFunction;
      instruction.packFoldArgument = value->packFoldArgument;
      if (value->operands.size() != value->packFoldArgument + 1) {
        valid = false;
      } else {
        instruction.packFoldFixedPlaces.reserve(value->packFoldArgument);
        for (std::size_t index = 0; index < value->packFoldArgument; ++index) {
          emitPlaceDependencies(value->operands[index]);
          const MirPlaceId place = placeForValue(value->operands[index]);
          if (place == 0) {
            valid = false;
          }
          instruction.packFoldFixedPlaces.push_back(place);
        }
      }
      instruction.packFoldElements.reserve(value->packFoldElements.size());
      for (const HirPackFoldElement &element : value->packFoldElements) {
        instruction.packFoldElements.push_back(
            {.elementType = element.elementType,
             .functionTarget = element.functionTarget,
             .parameterTypes = element.parameterTypes});
      }
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
    for (std::size_t index = 0;
         instruction.operation != MirOperation::PackFold &&
         index < value->operands.size();
         ++index) {
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
      } else {
        instruction.operands.push_back(
            instruction.operation == MirOperation::LogicalNot
                ? conditionOperand(operand)
            : instruction.operation == MirOperation::AddressOf
                ? addressOperand(operand)
                : valueOperand(operand));
      }
    }
    if (value->kind == HirValueKind::ArrayInitializer ||
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
    (void)initializeValueLifecycle(instruction, *value);
    (void)appendInstruction(std::move(instruction));
    endConsumedCallResultLoans(value->operands, *value);
    emittedValues.insert(id);
  }

  [[nodiscard]] MirOperand referenceOperand(HirValueId valueId,
                                            HirBindingId binding = 0) {
    const HirValue *value = findValue(valueId);
    if (value == nullptr) {
      return {};
    }
    emitPlaceDependencies(valueId);
    const MirLoanId existing = loanForValue(valueId);
    const HirLoan *desired = binding == 0 ? nullptr : loanForBinding(binding);
    if (desired != nullptr) {
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

      MirLoanId parent = 0;
      if (desired->parent != 0) {
        const MirPlaceId desiredSource = placeForLoan(*desired);
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
                sameBorrowSource(transient->source, desiredSource) &&
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
          } else {
            for (const MirLoanId ended : transientChain) {
              (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                       .hirValue = valueId,
                                       .loan = ended});
              removeLoanFromState(ended, scopes, bindingLoans);
            }
          }
        } else if (!sourceLoan.entry ||
                   sourceLoan.semanticLoan == desired->semanticLoan) {
          recordLoanIdentity(existing, *desired, binding);
          return {.kind = MirOperandKind::Loan,
                  .loan = existing,
                  .type = value->info.type};
        }
      }

      const MirPlaceId sourcePlace = placeForLoan(*desired);
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

    if (existing != 0) {
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
    const MirLoanId loan = createLoan(MirLoanKind::Local, sourcePlace,
                                      value->info.access, valueId, binding);
    (void)appendInstruction(
        {.kind = MirInstructionKind::Borrow,
         .hirValue = valueId,
         .operands = {{.kind = value->info.access == AccessMode::Mutable
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

  void markStoredBorrow(HirValueId valueId, SymbolId field, AccessMode access) {
    if (const MirLoanId sourceLoan = loanForValue(valueId); sourceLoan != 0) {
      const MirPlaceId sourcePlace = output.loans[sourceLoan - 1].source;
      const MirLoanId stored = createLoan(MirLoanKind::Stored, sourcePlace,
                                          access, valueId, 0, field);
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
    for (Scope &scope : scopes) {
      std::erase(scope.loans, borrow.loan);
    }
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
    const bool bodylessConstructor =
        constructor != nullptr && (constructor->source == nullptr ||
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
      if (loan.semanticLoan == 0 || loan.parent != 0 || loan.carriers.empty()) {
        valid = false;
        continue;
      }
      const MirPlaceId sourcePlace = placeForLoan(loan);
      if (sourcePlace == 0) {
        continue;
      }
      const MirLoanId lowered =
          createLoan(MirLoanKind::Parameter, sourcePlace, loan.access);
      output.loans[lowered - 1].entry = true;
      recordLoanIdentity(lowered, loan);
      for (const HirBindingId carrier : loan.carriers) {
        bindingLoans.insert_or_assign(carrier, lowered);
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
    for (auto loan = scope.loans.rbegin(); loan != scope.loans.rend(); ++loan) {
      if (!containsLoan(endedLoans, *loan)) {
        endedLoans.push_back(*loan);
      }
      (void)appendInstruction(
          {.kind = MirInstructionKind::EndBorrow, .loan = *loan});
    }
    std::vector<MirDropObligationId> cleanup;
    cleanup.reserve(scope.drops.size());
    for (auto drop = scope.drops.rbegin(); drop != scope.drops.rend(); ++drop) {
      const MirPlace *place = output.findPlace(*drop);
      const MirDropObligationId obligation =
          place != nullptr && place->root == MirPlaceRootKind::Binding
              ? dropObligationForBinding(place->binding)
              : 0;
      if (obligation != 0) {
        if (!containsDrop(consumedDrops, obligation)) {
          consumedDrops.push_back(obligation);
        }
        cleanup.push_back(obligation);
      }
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
          consumedDrops, endedLoans);
    }
    if (!cleanup.empty()) {
      const std::size_t id = output.cleanupBoundaries.size() + 1;
      output.cleanupBoundaries.push_back(
          {.id = id, .obligations = std::move(cleanup)});
      (void)appendInstruction(
          {.kind = MirInstructionKind::Lifecycle, .cleanupBoundaryEnd = id});
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
      lowerNestedStatement(statement->body);
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
      if (binding != nullptr &&
          binding->info.type.kind == SemanticType::Reference) {
        MirOperand reference =
            referenceOperand(*statement.value, *statement.binding);
        retainedLoan = reference.loan;
        operands.push_back(std::move(reference));
      } else {
        operands.push_back(valueOperand(*statement.value));
        if (binding != nullptr && binding->info.traits.containsBorrowedState) {
          retainedLoan = loanForValue(*statement.value);
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
          if (existing != nullptr && candidate != nullptr &&
              loanIsActive(scopes, existing->id) &&
              (existing->access == candidate->access ||
               (existing->access == AccessMode::Mutable &&
                candidate->access == AccessMode::ReadOnly))) {
            candidate->source = existing->source;
            retainedLoan = existing->id;
          } else {
            valid = false;
          }
        } else if (found == semanticLoans.end()) {
          semanticLoans.emplace(semanticLoan, retainedLoan);
        }
        MirLoan &retained = output.loans[retainedLoan - 1];
        if (retained.semanticLoan != 0 &&
            retained.semanticLoan != semanticLoan) {
          valid = false;
        } else {
          retained.semanticLoan = semanticLoan;
        }
      }
      bindingLoans.insert_or_assign(*statement.binding, retainedLoan);
      std::vector<HirBindingId> &carriers =
          output.loans[retainedLoan - 1].carriers;
      if (std::find(carriers.begin(), carriers.end(), *statement.binding) ==
          carriers.end()) {
        carriers.push_back(*statement.binding);
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
    terminate({.kind = MirTerminatorKind::Goto, .target = conditionBlock});

    current = conditionBlock;
    if (statement.condition) {
      const std::vector<Scope> conditionScopes = scopes;
      const std::vector<TemporaryDrop> conditionTemporaryDrops = temporaryDrops;
      const MirOperand condition = conditionOperand(*statement.condition);
      endFullExpressionLoans(conditionScopes);
      emitTemporaryDrops(conditionTemporaryDrops, *statement.condition,
                         statement.id);
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
  }

  void lowerSwitch(const HirStatement &statement) {
    const std::vector<Scope> subjectScopes = scopes;
    const std::vector<TemporaryDrop> subjectTemporaryDrops = temporaryDrops;
    const MirOperand subject =
        statement.value ? valueOperand(*statement.value) : MirOperand{};
    endFullExpressionLoans(subjectScopes);
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
      for (const MirLoanId loan : childFirstLoans(endedOnEveryArm)) {
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
        (void)appendInstruction({.kind = MirInstructionKind::Initialize,
                                 .hirStatement = statement.id,
                                 .destination = destination,
                                 .operands = {valueOperand(payload.value)},
                                 .info = {.type = binding->info.type,
                                          .category = ValueCategory::Place,
                                          .access = binding->info.access,
                                          .traits = binding->info.traits}});
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
      if (output.returnType.kind == SemanticType::Reference) {
        MirOperand borrow = referenceOperand(*statement.value);
        if (borrow.loan != 0) {
          if (function != nullptr &&
              function->returnBorrowOrigin != BorrowOriginKind::None &&
              borrow.loan <= output.loans.size() &&
              output.loans[borrow.loan - 1].kind == MirLoanKind::CallResult) {
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
            const bool validTail =
                function->returnBorrowOrigin == BorrowOriginKind::Receiver ||
                        function->returnBorrowOrigin == BorrowOriginKind::Global
                    ? transient == 0
                    : summarizedEntry != 0 && transient == summarizedEntry;
            if (!validTail || transientChain.empty()) {
              valid = false;
            } else {
              for (const MirLoanId ended : transientChain) {
                (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                         .hirValue = *statement.value,
                                         .loan = ended});
                removeLoanFromState(ended, scopes, bindingLoans);
              }
              if (summarizedEntry != 0) {
                (void)appendInstruction({.kind = MirInstructionKind::EndBorrow,
                                         .hirValue = *statement.value,
                                         .loan = summarizedEntry});
                removeLoanFromState(summarizedEntry, scopes, bindingLoans);
              }
              const HirValue *value = findValue(*statement.value);
              const MirLoanId normalized =
                  createLoan(MirLoanKind::Return, sourcePlace,
                             function->returnBorrowAccess, *statement.value);
              (void)appendInstruction(
                  {.kind = MirInstructionKind::Borrow,
                   .hirValue = *statement.value,
                   .operands = {{.kind = function->returnBorrowAccess ==
                                                 AccessMode::Mutable
                                             ? MirOperandKind::BorrowWrite
                                             : MirOperandKind::BorrowRead,
                                 .place = sourcePlace,
                                 .type = output.returnType}},
                   .loan = normalized,
                   .info = value == nullptr ? ExpressionInfo{} : value->info});
              borrow.loan = normalized;
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
          const MirLoanId retained = loanForValue(*statement.value);
          if (retained == 0 || retained > output.loans.size()) {
            valid = false;
          } else {
            MirLoan &loan = output.loans[retained - 1];
            loan.kind = MirLoanKind::Return;
            loan.escapes = true;
            for (Scope &scope : scopes) {
              std::erase(scope.loans, retained);
            }
            returnLoan = retained;
          }
        }
      }
    }
    if (statement.value && output.returnType.kind != SemanticType::Reference) {
      emitTemporaryTransfer(*statement.value, statement.id);
    }
    endReturnExpressionLoans(incomingScopes);
    emitTemporaryDrops(incomingTemporaryDrops, statement.value.value_or(0),
                       statement.id);
    emitScopeExit(0);
    terminate({.kind = MirTerminatorKind::Return,
               .hirStatement = statement.id,
               .value = std::move(result),
               .returnLoan = returnLoan});
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
  const MirDefinedFailureEffects &definedFailureEffects;
  bool implicitZeroReturn = false;
  const HirFunctionInstance *function = nullptr;
  const HirConstructorInstance *constructor = nullptr;
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
  std::unordered_map<HirValueId, MirValueId> loweredValues;
  std::unordered_map<HirValueId, MirValueId> contextualValues;
  std::unordered_map<HirValueId, MirValueId> materializedValues;
  std::unordered_map<HirValueId, MirPlaceId> valuePlaces;
  std::unordered_map<HirValueId, MirLoanId> valueLoans;
  std::unordered_map<HirBindingId, MirLoanId> bindingLoans;
  std::unordered_map<SemanticLoanId, MirLoanId> semanticLoans;
  std::unordered_map<SemanticLoanId, MirPlaceId> semanticLoanPlaces;
  std::unordered_set<HirValueId> emittedValues;
  std::vector<Scope> scopes;
  std::vector<TemporaryDrop> temporaryDrops;
  std::optional<PreparedCallArgumentFailure> preparedCallArgumentFailure;
  std::vector<BreakContext> breakContexts;
  std::vector<ContinueContext> continueContexts;
};

MirLoweringResult
MirLowerer::lower(const HirProgram &source,
                  const FailureMetadata &failureMetadata) const {
  MirLoweringResult provisional =
      lowerProgram(source, failureMetadata, MirDefinedFailureEffects{}, false);
  if (!provisional.valid()) {
    return provisional;
  }
  MirProgram provisionalVerification = provisional.program;
  for (MirFunctionInstance &function : provisionalVerification.functions) {
    if (function.entryKind == ProgramEntryKind::None) {
      continue;
    }
    function.entryKind = ProgramEntryKind::None;
    function.entryArgumentAppendTarget.reset();
  }
  if (!verifyMirProgram(provisionalVerification).valid()) {
    provisional.program.valid_ = false;
    return provisional;
  }
  // A function-level proof reads the lowered shape, and lowering with a
  // proved-false callee or destructor removes the failure edges that hid the
  // next proof. Re-lower until the derived result is its own fixed point.
  // Effects only ever move from conservative true toward proved false, so this
  // terminates; the bound keeps a non-converging program fail-closed below.
  constexpr int maximumEffectRounds = 8;
  MirDefinedFailureEffects effects =
      deriveMirDefinedFailureEffects(provisional.program);
  MirLoweringResult result =
      lowerProgram(source, failureMetadata, effects, true);
  MirDefinedFailureEffects finalEffects =
      deriveMirDefinedFailureEffects(result.program);
  for (int round = 0; round < maximumEffectRounds && effects != finalEffects;
       ++round) {
    effects = finalEffects;
    result = lowerProgram(source, failureMetadata, effects, true);
    finalEffects = deriveMirDefinedFailureEffects(result.program);
  }
  const bool exactSummaries =
      effects == finalEffects &&
      effects.functions.size() == result.program.functionInstances().size() &&
      effects.constructors.size() ==
          result.program.constructorInstances().size() &&
      effects.destructors.size() ==
          result.program.destructorInstances().size() &&
      std::equal(effects.functions.begin(), effects.functions.end(),
                 result.program.functionInstances().begin(),
                 [](bool mayRaise, const MirFunctionInstance &function) {
                   return mayRaise == function.mayRaiseDefinedFailure;
                 }) &&
      std::equal(effects.constructors.begin(), effects.constructors.end(),
                 result.program.constructorInstances().begin(),
                 [](bool mayRaise, const MirConstructorInstance &constructor) {
                   return mayRaise == constructor.mayRaiseDefinedFailure;
                 }) &&
      std::equal(effects.destructors.begin(), effects.destructors.end(),
                 result.program.destructorInstances().begin(),
                 [](bool mayRaise, const MirDestructorInstance &destructor) {
                   return mayRaise == destructor.mayRaiseDefinedFailure;
                 });
  result.program.valid_ = result.program.valid_ && exactSummaries &&
                          verifyMirProgram(result.program).valid();
  return result;
}

MirLoweringResult
MirLowerer::lowerProgram(const HirProgram &source,
                         const FailureMetadata &failureMetadata,
                         const MirDefinedFailureEffects &definedFailureEffects,
                         bool includeProgramInitialization) {
  MirLoweringResult result;
  if (!source.valid()) {
    result.program.valid_ = false;
    return result;
  }

  bool valid = verifyFailureMetadata(failureMetadata).valid();
  result.program.executionProfile_ = source.executionProfile();
  result.program.failureMetadata_ = failureMetadata;
  if (includeProgramInitialization) {
    result.program.moduleBody = lowerBody(
        source, failureMetadata, source.module(), MirBodyKind::Module,
        SemanticType::Void, {}, definedFailureEffects, valid, false, nullptr,
        nullptr, nullptr, nullptr, &source.programInitializationPlan(),
        &result.program.programInitialization);
  } else {
    HirBody provisionalModule;
    provisionalModule.placeDomain = source.module().placeDomain;
    result.program.moduleBody = lowerBody(
        source, failureMetadata, provisionalModule, MirBodyKind::Module,
        SemanticType::Void, {}, definedFailureEffects, valid);
  }

  result.program.classes.reserve(source.classInstances().size());
  for (const HirClassInstance &instance : source.classInstances()) {
    MirClassInstance lowered{
        .id = instance.id,
        .declaration = instance.declaration,
        .type = instance.type,
        .kind = instance.kind,
        .bases = instance.bases,
        .structuralBases = instance.bases,
        .abstract = instance.abstract,
        .polymorphic = instance.polymorphic,
        .cAbiRecord = instance.cAbiRecord,
        .cAbiLayout = instance.cAbiLayout,
        .unionLayout = instance.unionLayout,
        .destructor = instance.destructor,
        .requiresActiveDropState = instance.requiresActiveDropState,
        .requiresActiveCleanup = instance.requiresActiveCleanup};
    lowered.fields.reserve(instance.fields.size());
    for (const HirClassField &field : instance.fields) {
      lowered.declaredFields.push_back(
          {.field = field.binding,
           .symbol = field.info.symbol,
           .type = field.info.type,
           .dropKind = field.info.traits.drop,
           .requiresActiveCleanup = field.requiresActiveCleanup});
      if (field.info.traits.drop != DropKind::Lexical) {
        continue;
      }
      lowered.fields.push_back(
          {.field = field.binding,
           .symbol = field.info.symbol,
           .type = field.info.type,
           .dropKind = field.info.traits.drop,
           .requiresActiveCleanup = field.requiresActiveCleanup});
    }
    lowered.fieldInitializers =
        lowerBody(source, failureMetadata, instance.fieldInitializers,
                  MirBodyKind::FieldInitializers, SemanticType::Void, {},
                  definedFailureEffects, valid);
    lowered.staticFieldInitializers =
        lowerBody(source, failureMetadata, instance.staticFieldInitializers,
                  MirBodyKind::StaticFieldInitializers, SemanticType::Void, {},
                  definedFailureEffects, valid);
    for (auto field = instance.fields.rbegin(); field != instance.fields.rend();
         ++field) {
      if (field->info.traits.drop == DropKind::Lexical) {
        lowered.fieldDropOrder.push_back(
            {.field = field->binding,
             .symbol = field->info.symbol,
             .type = field->info.type,
             .requiresActiveCleanup = field->requiresActiveCleanup});
      }
    }
    result.program.classes.push_back(std::move(lowered));
  }

  result.program.functions.reserve(source.functionInstances().size());
  for (const HirFunctionInstance &instance : source.functionInstances()) {
    const bool implicitZeroReturn = !instance.owner &&
                                    instance.source != nullptr &&
                                    instance.source->name().lexeme == "main";
    std::vector<MirCallableParameter> callableParameters;
    callableParameters.reserve(instance.callableParameters.size());
    for (const HirCallableParameter &parameter : instance.callableParameters) {
      MirCallableParameter lowered{.parameterIndex = parameter.parameterIndex,
                                   .callableType = parameter.callableType,
                                   .access = parameter.access,
                                   .boundary = parameter.boundary,
                                   .ownedTransport = parameter.ownedTransport};
      lowered.signatures.reserve(parameter.signatures.size());
      for (const HirCallableSignature &signature : parameter.signatures) {
        lowered.signatures.push_back(
            {.returnType = signature.returnType,
             .parameterTypes = signature.parameterTypes,
             .requiredCapability = signature.requiredCapability,
             .selectedCapability = signature.selectedCapability,
             .functionTarget = signature.functionTarget,
             .lambdaTarget = signature.lambdaTarget});
      }
      lowered.forwardings.reserve(parameter.forwardings.size());
      for (const HirCallableForwarding &forwarding : parameter.forwardings) {
        lowered.forwardings.push_back(
            {.parameterIndex = forwarding.parameterIndex,
             .functionTarget = forwarding.functionTarget});
      }
      callableParameters.emplace_back(std::move(lowered));
    }
    result.program.functions.push_back(
        {.id = instance.id,
         .declaration = instance.declaration,
         .typeArguments = instance.typeArguments,
         .owner = instance.owner,
         .returnType = instance.returnType,
         .parameterTypes = instance.parameterTypes,
         .parameterBindings = instance.parameterBindings,
         .entryKind = instance.entryKind,
         .entryArgumentAppendTarget = instance.entryArgumentAppendTarget,
         .staticMember = instance.staticMember,
         .receiverMutability = instance.source == nullptr
                                   ? ReceiverMutability::ReadOnly
                                   : instance.source->receiverMutability(),
         .overloadedOperator =
             instance.source == nullptr || !instance.source->operatorName()
                 ? std::nullopt
                 : std::optional{instance.source->operatorName()->kind},
         .constexprFunction = instance.constexprFunction,
         .returnBorrowOrigin = instance.returnBorrowOrigin,
         .returnBorrowParameter = instance.returnBorrowParameter,
         .returnBorrowAccess = instance.returnBorrowAccess,
         .returnBorrowPlace = instance.returnBorrowPlace,
         .linkage = instance.linkage,
         .externalSymbol = instance.externalSymbol,
         .virtualMethod = instance.virtualMethod,
         .pureVirtual = instance.pureVirtual,
         .overrideMethod = instance.overrideMethod,
         .virtualRoots = instance.virtualRoots,
         .callableParameters = std::move(callableParameters),
         .definitionKind =
             instance.source == nullptr
                 ? MirFunctionInstance::DefinitionKind::Declaration
             : instance.source->runtimeBinding()
                 ? MirFunctionInstance::DefinitionKind::RuntimeBinding
             : instance.source->body() != nullptr
                 ? MirFunctionInstance::DefinitionKind::Source
                 : MirFunctionInstance::DefinitionKind::Declaration,
         .mayRaiseDefinedFailure =
             definedFailureEffects.functions.empty() || instance.id == 0 ||
             instance.id > definedFailureEffects.functions.size() ||
             definedFailureEffects.functions[instance.id - 1],
         .body = lowerBody(source, failureMetadata, instance.body,
                           MirBodyKind::Function, instance.returnType, {},
                           definedFailureEffects, valid, implicitZeroReturn,
                           nullptr, &instance, nullptr)});
  }

  result.program.constructors.reserve(source.constructorInstances().size());
  for (const HirConstructorInstance &instance : source.constructorInstances()) {
    std::vector<MirConstructorInitializer> initializers;
    initializers.reserve(instance.initializers.size());
    for (const HirConstructorInitializer &initializer : instance.initializers) {
      initializers.push_back(
          {.kind = initializer.kind,
           .targetType = initializer.targetType,
           .field = initializer.field,
           .base = initializer.base,
           .constructorTarget = initializer.constructorTarget,
           .arguments = initializer.arguments,
           .storesReference = initializer.storesReference,
           .borrowAccess = initializer.borrowAccess,
           .generatedDefault = initializer.generatedDefault,
           .ownedParameter = initializer.ownedParameter});
    }
    result.program.constructors.push_back(
        {.id = instance.id,
         .owner = instance.owner,
         .parameterTypes = instance.parameterTypes,
         .parameterBindings = instance.parameterBindings,
         .borrowOrigin = instance.borrowOrigin,
         .borrowParameter = instance.borrowParameter,
         .borrowAccess = instance.borrowAccess,
         .definitionKind =
             instance.source != nullptr && instance.source->body() != nullptr
                 ? MirDefinitionKind::Source
                 : MirDefinitionKind::Declaration,
         .mayRaiseDefinedFailure =
             definedFailureEffects.constructors.empty() || instance.id == 0 ||
             instance.id > definedFailureEffects.constructors.size() ||
             definedFailureEffects.constructors[instance.id - 1],
         .initializers = std::move(initializers),
         .body =
             lowerBody(source, failureMetadata, instance.body,
                       MirBodyKind::Constructor, SemanticType::Void,
                       instance.initializerValues, definedFailureEffects, valid,
                       false, &instance.initializers, nullptr, &instance)});
  }

  result.program.destructors.reserve(source.destructorInstances().size());
  for (const HirDestructorInstance &instance : source.destructorInstances()) {
    result.program.destructors.push_back(
        {.id = instance.id,
         .owner = instance.owner,
         .definitionKind =
             instance.source != nullptr && instance.source->body() != nullptr
                 ? MirDefinitionKind::Source
                 : MirDefinitionKind::Declaration,
         .mayRaiseDefinedFailure =
             definedFailureEffects.destructors.empty() || instance.id == 0 ||
             instance.id > definedFailureEffects.destructors.size() ||
             definedFailureEffects.destructors[instance.id - 1],
         .body = lowerBody(source, failureMetadata, instance.body,
                           MirBodyKind::Destructor, SemanticType::Void, {},
                           definedFailureEffects, valid)});
  }

  result.program.lambdas.reserve(source.lambdaInstances().size());
  for (const HirLambda &instance : source.lambdaInstances()) {
    MirLambdaInstance lowered{.id = instance.id,
                              .declaration = instance.declaration,
                              .type = instance.type,
                              .returnType = instance.returnType,
                              .parameterTypes = instance.parameterTypes,
                              .parameterBindings = instance.parameterBindings};
    lowered.captureTypes.reserve(instance.captures.size());
    lowered.captureModes.reserve(instance.captures.size());
    lowered.captureSymbols.reserve(instance.captures.size());
    lowered.captureRequiresActiveCleanup.reserve(instance.captures.size());
    for (std::size_t capture = 0; capture < instance.captures.size();
         ++capture) {
      lowered.captureTypes.push_back(instance.captures[capture].type);
      lowered.captureModes.push_back(instance.captures[capture].mode);
      lowered.captureSymbols.push_back(
          instance.captures[capture].bindingSymbol);
      lowered.captureRequiresActiveCleanup.push_back(
          capture < instance.captureRequiresActiveCleanup.size()
              ? instance.captureRequiresActiveCleanup[capture]
              : false);
    }
    lowered.body =
        lowerBody(source, failureMetadata, instance.body, MirBodyKind::Lambda,
                  instance.returnType, {}, definedFailureEffects, valid, false,
                  nullptr, nullptr, nullptr, &instance);
    result.program.lambdas.push_back(std::move(lowered));
  }
  if (includeProgramInitialization) {
    valid =
        lowerHostedStartup(source, failureMetadata, result.program) && valid;
  }
  result.program.valid_ = valid;
  return result;
}

bool MirLowerer::lowerHostedStartup(const HirProgram &source,
                                    const FailureMetadata &failureMetadata,
                                    MirProgram &program) {
  const std::optional<HirHostedProgramEntryPlan> &hirPlan =
      source.hostedProgramEntryPlan();
  if (!hirPlan) {
    program.hostedStartupPlan_.reset();
    program.hostedStartupBody.reset();
    return true;
  }

  const MirFunctionInstance *entry =
      program.findFunctionInstance(hirPlan->entry);
  const MirFunctionInstance *append =
      program.findFunctionInstance(hirPlan->appendFunction);
  const MirConstructorInstance *vectorConstructor =
      program.findConstructorInstance(hirPlan->vectorConstructor);
  const MirConstructorInstance *stringConstructor =
      program.findConstructorInstance(hirPlan->stringConstructor);
  bool valid = entry != nullptr && entry->entryKind == hirPlan->kind &&
               entry->returnType == SemanticType::Int32 &&
               hirPlan->sourceUnit != 0 &&
               hirPlan->mainAnchor.end >= hirPlan->mainAnchor.start &&
               hirPlan->mainAnchor.line >= 1;
  const bool callsProgramInitialization = std::any_of(
      program.programInitializationPlan().steps.begin(),
      program.programInitializationPlan().steps.end(), [](const auto &step) {
        return step.role == ProgramInitializationStepRole::Initializer;
      });

  MirHostedStartupPlan plan;
  plan.kind = hirPlan->kind;
  plan.entry = hirPlan->entry;
  plan.appendFunction = hirPlan->appendFunction;
  plan.vectorConstructor = hirPlan->vectorConstructor;
  plan.stringConstructor = hirPlan->stringConstructor;
  plan.sourceAnchor = {.sourceUnit = hirPlan->sourceUnit,
                       .start = hirPlan->mainAnchor.start,
                       .end = hirPlan->mainAnchor.end,
                       .line = hirPlan->mainAnchor.line};
  plan.programInitializationTarget = {.kind = MirBodyKind::Module};
  plan.exitPolicy = MirHostedStartupExitPolicy::ImmediateExit70;

  MirBody body;
  body.kind = MirBodyKind::HostedStartup;
  body.returnType = SemanticType::Int32;
  std::size_t maximumBodyDomain = 0;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    if (const MirBody *candidate = findMirBody(program, address)) {
      maximumBodyDomain =
          std::max(maximumBodyDomain, candidate->placeDomain.body);
    }
  }
  body.placeDomain = {.snapshot = program.module().placeDomain.snapshot,
                      .body = maximumBodyDomain + 1};
  valid = valid && body.placeDomain.snapshot != 0 && body.placeDomain.body != 0;

  MirInstructionId nextInstruction = 1;
  MirTemporaryId nextTemporary = 1;
  // The hosted schedule is a closed, bounded graph. Reserve for every normal,
  // primary-cleanup, and first-secondary block before retaining references.
  body.blocks.reserve(64);
  const auto appendBlock = [&](MirFailureRecordId activeFailure =
                                   0) -> MirBlock & {
    MirBlock block;
    block.id = body.blocks.size() + 1;
    block.activeFailure = activeFailure;
    body.blocks.push_back(std::move(block));
    return body.blocks.back();
  };
  const auto beginOperation = [&](MirHostedStartupOperationKind kind,
                                  MirHostedStartupFailureBehavior behavior,
                                  MirBlockId block) {
    const MirHostedStartupOperationId id = plan.operations.size() + 1;
    plan.operations.push_back(
        {.id = id, .kind = kind, .failureBehavior = behavior, .block = block});
    return id;
  };
  const auto appendValue = [&](MirHostedStartupOperationId operation,
                               MirBlockId block, MirInstructionId instruction,
                               ExpressionInfo info) {
    const MirValueId id = body.values.size() + 1;
    body.values.push_back({.id = id,
                           .hostedStartupOperation = operation,
                           .sourceValue = 0,
                           .info = std::move(info),
                           .definitionBlock = block,
                           .definition = instruction});
    body.valueUses.emplace_back();
    MirHostedStartupOperation &row = plan.operations[operation - 1];
    if (row.value != 0) {
      valid = false;
    }
    row.value = id;
    return id;
  };
  const auto appendPlace = [&](MirHostedStartupOperationId operation,
                               MirPlace place) {
    const MirPlaceId id = body.places.size() + 1;
    place.id = id;
    place.hostedStartupOperation = operation;
    body.places.push_back(std::move(place));
    MirHostedStartupOperation &row = plan.operations[operation - 1];
    if (row.place != 0) {
      valid = false;
    }
    row.place = id;
    return id;
  };
  const auto appendDrop = [&](MirHostedStartupOperationId operation,
                              MirDropObligation drop) {
    const MirDropObligationId id = body.dropObligations.size() + 1;
    drop.id = id;
    drop.hostedStartupOperation = operation;
    drop.constructionOrder = id;
    body.dropObligations.push_back(std::move(drop));
    MirHostedStartupOperation &row = plan.operations[operation - 1];
    if (row.dropObligation != 0) {
      valid = false;
    }
    row.dropObligation = id;
    return id;
  };
  const auto appendInstruction = [&](MirHostedStartupOperationId operation,
                                     MirBlock &block,
                                     MirInstruction instruction) {
    instruction.id = nextInstruction++;
    instruction.hostedStartupOperation = operation;
    block.instructions.push_back(std::move(instruction));
    MirHostedStartupOperation &row = plan.operations[operation - 1];
    if (row.instruction != 0 || row.terminator) {
      valid = false;
    }
    row.instruction = block.instructions.back().id;
    return block.instructions.back().id;
  };
  const auto terminate = [&](MirHostedStartupOperationId operation,
                             MirBlock &block, MirTerminator terminator) {
    terminator.hostedStartupOperation = operation;
    block.terminator = std::move(terminator);
    MirHostedStartupOperation &row = plan.operations[operation - 1];
    if (row.instruction != 0 || row.terminator) {
      valid = false;
    }
    row.terminator = true;
  };
  const auto generatedInfo = [](const SemanticType &type,
                                ValueCategory category = ValueCategory::Value,
                                AccessMode access = AccessMode::ReadOnly) {
    return ExpressionInfo{.type = type,
                          .category = category,
                          .access = access,
                          .traits = semanticTraits(type)};
  };
  const auto exactLocalFailure = [&](DefinedFailureOperation operation) {
    std::vector<FailureSiteId> sites;
    sites.reserve(operation.localOrigins.size());
    for (const DefinedFailureOrigin &origin : operation.localOrigins) {
      const std::optional<FailureSiteId> site = failureMetadata.siteFor(origin);
      if (!site || *site == 0) {
        valid = false;
        sites.push_back(0);
      } else {
        sites.push_back(*site);
      }
    }
    return std::pair{std::move(operation), std::move(sites)};
  };
  const auto propagation = [](FailurePropagationKind kind, bool mayRaise) {
    DefinedFailureOperation result;
    result.propagation = mayRaise ? kind : FailurePropagationKind::None;
    return result;
  };
  const auto failureBehavior = [](bool mayRaise) {
    return mayRaise ? MirHostedStartupFailureBehavior::Propagate
                    : MirHostedStartupFailureBehavior::None;
  };
  const bool moduleMayRaise = std::any_of(
      program.module().blocks.begin(), program.module().blocks.end(),
      [](const MirBlock &block) {
        return std::any_of(block.instructions.begin(), block.instructions.end(),
                           [](const MirInstruction &instruction) {
                             return !instruction.definedFailure.empty();
                           });
      });
  const auto generatedDropType = [&](const SemanticType &type,
                                     HirClassInstanceId owner) {
    const MirClassInstance *instance = program.findClassInstance(owner);
    if (instance == nullptr || instance->type != type ||
        semanticTraits(type).drop != DropKind::Lexical ||
        !instance->requiresActiveCleanup) {
      valid = false;
    }
    return MirDropType{
        .type = type,
        .classInstance = owner == 0 ? std::nullopt : std::optional{owner},
        .destructor = instance == nullptr ? std::nullopt : instance->destructor,
        .requiresActiveCleanup =
            instance != nullptr && instance->requiresActiveCleanup};
  };
  const auto appendFailureRecord = [&](MirHostedStartupOperationId operation,
                                       MirFailureRecord record) {
    const MirFailureRecordId id = body.failureRecords.size() + 1;
    record.id = id;
    record.hostedStartupOperation = operation;
    body.failureRecords.push_back(std::move(record));
    MirHostedStartupOperation &row = plan.operations[operation - 1];
    if (row.failureRecord != 0) {
      valid = false;
    }
    row.failureRecord = id;
    return id;
  };
  const auto appendCleanupBoundary =
      [&](MirHostedStartupOperationId operation,
          std::vector<MirDropObligationId> obligations) {
        const std::size_t id = body.cleanupBoundaries.size() + 1;
        body.cleanupBoundaries.push_back(
            {.id = id,
             .hostedStartupOperation = operation,
             .kind = MirCleanupBoundaryKind::Failure,
             .obligations = std::move(obligations)});
        MirHostedStartupOperation &row = plan.operations[operation - 1];
        if (row.cleanupBoundary != 0) {
          valid = false;
        }
        row.cleanupBoundary = id;
        return id;
      };
  const auto destructorMayRaise = [&](MirDropObligationId obligation) {
    const MirDropObligation *drop = body.findDropObligation(obligation);
    const MirDestructorInstance *destructor =
        drop != nullptr && drop->dropType.destructor
            ? program.findDestructorInstance(*drop->dropType.destructor)
            : nullptr;
    return destructor != nullptr && destructor->mayRaiseDefinedFailure;
  };
  const auto emitFailureCleanup =
      [&](MirBlock &parameterBlock, MirFailureRecordId primary,
          const std::vector<MirDropObligationId> &activeDrops) {
        MirBlock *cleanup = &parameterBlock;
        std::vector<MirDropObligationId> boundaryDrops;
        boundaryDrops.reserve(activeDrops.size());
        for (auto candidate = activeDrops.rbegin();
             candidate != activeDrops.rend(); ++candidate) {
          const MirDropObligation *drop = body.findDropObligation(*candidate);
          const MirPlace *place =
              drop == nullptr ? nullptr : body.findPlace(drop->place);
          if (drop == nullptr || place == nullptr) {
            valid = false;
            continue;
          }
          const bool mayRaise = destructorMayRaise(*candidate);
          const MirHostedStartupOperationId dropOperation =
              beginOperation(MirHostedStartupOperationKind::DropFailureCleanup,
                             failureBehavior(mayRaise), cleanup->id);
          MirInstruction cleanupDrop;
          cleanupDrop.kind = MirInstructionKind::Drop;
          cleanupDrop.destination = drop->place;
          cleanupDrop.definedFailure =
              propagation(FailurePropagationKind::Destructor, mayRaise);
          cleanupDrop.info = generatedInfo(
              drop->dropType.type, ValueCategory::Place, AccessMode::Mutable);
          cleanupDrop.lifecycle = {{.kind = MirLifecycleEventKind::Drop,
                                    .source = *candidate,
                                    .failureCleanup = true}};
          const MirInstructionId dropInstruction = appendInstruction(
              dropOperation, *cleanup, std::move(cleanupDrop));
          boundaryDrops.push_back(*candidate);

          if (!mayRaise) {
            continue;
          }
          const MirBlockId producerBlock = cleanup->id;
          MirBlock &normal = appendBlock(primary);
          MirBlock &secondaryBlock = appendBlock(primary);
          const MirHostedStartupOperationId routeOperation = beginOperation(
              MirHostedStartupOperationKind::RouteCleanupFailure,
              MirHostedStartupFailureBehavior::None, producerBlock);
          const MirFailureRecordId secondary = appendFailureRecord(
              routeOperation, {.producerBlock = producerBlock,
                               .producerInstruction = dropInstruction,
                               .parameterBlock = secondaryBlock.id});
          secondaryBlock.failureParameter = secondary;
          terminate(routeOperation, *cleanup,
                    {.kind = MirTerminatorKind::Invoke,
                     .invokeInstruction = dropInstruction,
                     .failureRecord = secondary,
                     .target = normal.id,
                     .elseTarget = secondaryBlock.id});

          const MirHostedStartupOperationId terminateOperation = beginOperation(
              MirHostedStartupOperationKind::TerminateCleanupFailure,
              MirHostedStartupFailureBehavior::None, secondaryBlock.id);
          terminate(terminateOperation, secondaryBlock,
                    {.kind = MirTerminatorKind::TerminateCleanupFailure,
                     .failureRecord = secondary});
          cleanup = &normal;
        }

        if (!boundaryDrops.empty()) {
          const MirHostedStartupOperationId boundaryOperation = beginOperation(
              MirHostedStartupOperationKind::EndFailureCleanup,
              MirHostedStartupFailureBehavior::None, cleanup->id);
          const std::size_t boundary = appendCleanupBoundary(
              boundaryOperation, std::move(boundaryDrops));
          appendInstruction(boundaryOperation, *cleanup,
                            {.kind = MirInstructionKind::Lifecycle,
                             .cleanupBoundaryEnd = boundary});
        }
        const MirHostedStartupOperationId containOperation =
            beginOperation(MirHostedStartupOperationKind::ContainFailure,
                           MirHostedStartupFailureBehavior::None, cleanup->id);
        terminate(containOperation, *cleanup,
                  {.kind = MirTerminatorKind::ContainFailure,
                   .failureRecord = primary});
      };
  const auto routeOperationFailure =
      [&](MirBlock *&producer, MirInstructionId instruction,
          const std::vector<MirDropObligationId> &activeDrops,
          MirDropObligationId successDrop = 0) {
        if (producer == nullptr || producer->instructions.empty() ||
            producer->instructions.back().id != instruction) {
          valid = false;
          return;
        }
        const MirBlockId producerBlock = producer->id;
        MirBlock &normal = appendBlock();
        MirBlock &failureBlock = appendBlock();
        const MirHostedStartupOperationId routeOperation = beginOperation(
            MirHostedStartupOperationKind::RouteOperationFailure,
            MirHostedStartupFailureBehavior::None, producerBlock);
        const MirFailureRecordId primary = appendFailureRecord(
            routeOperation, {.producerBlock = producerBlock,
                             .producerInstruction = instruction,
                             .parameterBlock = failureBlock.id});
        failureBlock.failureParameter = primary;
        failureBlock.activeFailure = primary;
        MirTerminator invoke{.kind = MirTerminatorKind::Invoke,
                             .invokeInstruction = instruction,
                             .failureRecord = primary,
                             .target = normal.id,
                             .elseTarget = failureBlock.id};
        if (successDrop != 0) {
          invoke.successLifecycle = {{.kind = MirLifecycleEventKind::Initialize,
                                      .target = successDrop}};
        }
        terminate(routeOperation, *producer, std::move(invoke));
        emitFailureCleanup(failureBlock, primary, activeDrops);
        producer = &normal;
      };

  MirBlock &entryBlock = appendBlock();
  body.entry = entryBlock.id;

  if (hirPlan->kind == ProgramEntryKind::NoArguments) {
    MirBlock *startupBlock = &entryBlock;
    valid = valid && entry != nullptr && entry->parameterTypes.empty() &&
            hirPlan->appendFunction == 0 && hirPlan->vectorConstructor == 0 &&
            hirPlan->stringConstructor == 0 && hirPlan->validateCount.empty() &&
            hirPlan->convertCount.empty();

    if (callsProgramInitialization) {
      const MirHostedStartupOperationId bodyCallOperation = beginOperation(
          MirHostedStartupOperationKind::CallProgramInitialization,
          failureBehavior(moduleMayRaise), startupBlock->id);
      MirInstruction bodyCall;
      bodyCall.kind = MirInstructionKind::CallBody;
      bodyCall.bodyTarget = plan.programInitializationTarget;
      bodyCall.definedFailure =
          propagation(FailurePropagationKind::BodyCall, moduleMayRaise);
      bodyCall.info = generatedInfo(SemanticType::Void);
      const MirInstructionId bodyCallInstruction = appendInstruction(
          bodyCallOperation, *startupBlock, std::move(bodyCall));
      if (moduleMayRaise) {
        routeOperationFailure(startupBlock, bodyCallInstruction, {});
      }
    }

    const bool entryMayRaise =
        entry != nullptr && entry->mayRaiseDefinedFailure;
    const MirHostedStartupOperationId entryCallOperation =
        beginOperation(MirHostedStartupOperationKind::CallEntry,
                       failureBehavior(entryMayRaise), startupBlock->id);
    const MirInstructionId entryCallInstruction = nextInstruction;
    const MirValueId entryResult =
        appendValue(entryCallOperation, startupBlock->id, entryCallInstruction,
                    generatedInfo(SemanticType::Int32));
    MirInstruction entryCall;
    entryCall.kind = MirInstructionKind::Call;
    entryCall.result = entryResult;
    entryCall.functionTarget = hirPlan->entry;
    entryCall.definedFailure =
        propagation(FailurePropagationKind::DirectCall, entryMayRaise);
    entryCall.info = generatedInfo(SemanticType::Int32);
    appendInstruction(entryCallOperation, *startupBlock, std::move(entryCall));
    if (entryMayRaise) {
      routeOperationFailure(startupBlock, entryCallInstruction, {});
    }
    plan.entryResult = entryResult;

    const MirHostedStartupOperationId returnOperation =
        beginOperation(MirHostedStartupOperationKind::ReturnEntry,
                       MirHostedStartupFailureBehavior::None, startupBlock->id);
    MirTerminator returnEntry;
    returnEntry.kind = MirTerminatorKind::Return;
    returnEntry.value = MirOperand{.kind = MirOperandKind::Value,
                                   .value = entryResult,
                                   .type = SemanticType::Int32};
    terminate(returnOperation, *startupBlock, std::move(returnEntry));
  } else {
    MirBlock *setupBlock = &entryBlock;
    const bool exactOwnedShape =
        hirPlan->kind == ProgramEntryKind::OwnedArguments && entry != nullptr &&
        entry->parameterTypes.size() == 2 &&
        entry->parameterTypes.front() == SemanticType::Int32 &&
        entry->parameterTypes.back().kind == SemanticType::Class &&
        entry->parameterTypes.back().arguments.size() == 1 &&
        append != nullptr && vectorConstructor != nullptr &&
        stringConstructor != nullptr &&
        hirPlan->appendFunction == entry->entryArgumentAppendTarget.value_or(0);
    valid = valid && exactOwnedShape && !hirPlan->validateCount.empty() &&
            !hirPlan->convertCount.empty();
    const SemanticType vectorType =
        exactOwnedShape ? entry->parameterTypes.back() : SemanticType::Unknown;
    const SemanticType stringType =
        exactOwnedShape ? vectorType.arguments.front() : SemanticType::Unknown;

    const auto [validateFailure, validateSites] =
        exactLocalFailure(hirPlan->validateCount);
    const MirHostedStartupOperationId validateOperation =
        beginOperation(MirHostedStartupOperationKind::ValidateArgumentCount,
                       MirHostedStartupFailureBehavior::Detect, setupBlock->id);
    const MirInstructionId validateInstruction = nextInstruction;
    const MirValueId validatedCount =
        appendValue(validateOperation, setupBlock->id, validateInstruction,
                    generatedInfo(SemanticType::Int64));
    MirInstruction validate;
    validate.kind = MirInstructionKind::Load;
    validate.result = validatedCount;
    validate.definedFailure = validateFailure;
    validate.localFailureSites = validateSites;
    validate.info = generatedInfo(SemanticType::Int64);
    appendInstruction(validateOperation, *setupBlock, std::move(validate));
    routeOperationFailure(setupBlock, validateInstruction, {});

    const auto [convertFailure, convertSites] =
        exactLocalFailure(hirPlan->convertCount);
    const MirHostedStartupOperationId convertOperation =
        beginOperation(MirHostedStartupOperationKind::ConvertArgumentCount,
                       MirHostedStartupFailureBehavior::Detect, setupBlock->id);
    const MirInstructionId convertInstruction = nextInstruction;
    const MirValueId stabilizedCount =
        appendValue(convertOperation, setupBlock->id, convertInstruction,
                    generatedInfo(SemanticType::Int32));
    MirInstruction convert;
    convert.kind = MirInstructionKind::Compute;
    convert.result = stabilizedCount;
    convert.operands = {{.kind = MirOperandKind::Value,
                         .value = validatedCount,
                         .type = SemanticType::Int64}};
    // Native argc conversion is a sealed hosted-boundary operation rather
    // than an ordinary source-language numeric conversion.
    convert.operation = MirOperation::None;
    convert.definedFailure = convertFailure;
    convert.localFailureSites = convertSites;
    convert.info = generatedInfo(SemanticType::Int32);
    appendInstruction(convertOperation, *setupBlock, std::move(convert));
    routeOperationFailure(setupBlock, convertInstruction, {});
    plan.stabilizedCount = stabilizedCount;

    if (callsProgramInitialization) {
      const MirHostedStartupOperationId bodyCallOperation = beginOperation(
          MirHostedStartupOperationKind::CallProgramInitialization,
          failureBehavior(moduleMayRaise), setupBlock->id);
      MirInstruction bodyCall;
      bodyCall.kind = MirInstructionKind::CallBody;
      bodyCall.bodyTarget = plan.programInitializationTarget;
      bodyCall.definedFailure =
          propagation(FailurePropagationKind::BodyCall, moduleMayRaise);
      bodyCall.info = generatedInfo(SemanticType::Void);
      const MirInstructionId bodyCallInstruction = appendInstruction(
          bodyCallOperation, *setupBlock, std::move(bodyCall));
      if (moduleMayRaise) {
        routeOperationFailure(setupBlock, bodyCallInstruction, {});
      }
    }

    const bool vectorMayRaise = vectorConstructor != nullptr &&
                                vectorConstructor->mayRaiseDefinedFailure;
    const MirHostedStartupOperationId vectorOperation =
        beginOperation(MirHostedStartupOperationKind::ConstructArgumentVector,
                       failureBehavior(vectorMayRaise), setupBlock->id);
    const MirInstructionId vectorInstruction = nextInstruction;
    const MirValueId vectorValue =
        appendValue(vectorOperation, setupBlock->id, vectorInstruction,
                    generatedInfo(vectorType));
    const MirPlaceId vectorPlace =
        appendPlace(vectorOperation, {.root = MirPlaceRootKind::Value,
                                      .value = vectorValue,
                                      .type = vectorType,
                                      .access = AccessMode::Mutable,
                                      .traits = semanticTraits(vectorType)});
    const MirDropObligationId vectorDrop = appendDrop(
        vectorOperation,
        {.kind = MirDropObligationKind::Value,
         .place = vectorPlace,
         .generatedValue = vectorValue,
         .dropType = generatedDropType(
             vectorType,
             vectorConstructor == nullptr ? 0 : vectorConstructor->owner)});
    MirInstruction constructVector;
    constructVector.kind = MirInstructionKind::Construct;
    constructVector.result = vectorValue;
    constructVector.constructorTarget = hirPlan->vectorConstructor;
    constructVector.definedFailure =
        propagation(FailurePropagationKind::Constructor, vectorMayRaise);
    constructVector.info = generatedInfo(vectorType);
    if (vectorMayRaise) {
      constructVector.successResultDrop = vectorDrop;
    } else {
      constructVector.lifecycle = {
          {.kind = MirLifecycleEventKind::Initialize, .target = vectorDrop}};
    }
    appendInstruction(vectorOperation, *setupBlock, std::move(constructVector));
    if (vectorMayRaise) {
      routeOperationFailure(setupBlock, vectorInstruction, {}, vectorDrop);
    }
    plan.argumentVector = vectorValue;
    plan.argumentVectorPlace = vectorPlace;

    const MirHostedStartupOperationId initializeIndexOperation =
        beginOperation(MirHostedStartupOperationKind::InitializeArgumentIndex,
                       MirHostedStartupFailureBehavior::None, setupBlock->id);
    const MirPlaceId indexPlace =
        appendPlace(initializeIndexOperation,
                    {.root = MirPlaceRootKind::Temporary,
                     .temporary = nextTemporary++,
                     .type = SemanticType::Int32,
                     .access = AccessMode::Mutable,
                     .traits = semanticTraits(SemanticType::Int32)});
    MirInstruction initializeIndex;
    initializeIndex.kind = MirInstructionKind::Initialize;
    initializeIndex.destination = indexPlace;
    initializeIndex.operands = {{.kind = MirOperandKind::Constant,
                                 .literal = Literal{std::uint64_t{0}},
                                 .type = SemanticType::Int32}};
    initializeIndex.info = generatedInfo(
        SemanticType::Int32, ValueCategory::Place, AccessMode::Mutable);
    appendInstruction(initializeIndexOperation, *setupBlock,
                      std::move(initializeIndex));
    plan.argumentIndexPlace = indexPlace;

    MirBlock &loopHeader = appendBlock();
    const MirHostedStartupOperationId enterLoopOperation =
        beginOperation(MirHostedStartupOperationKind::EnterArgumentLoop,
                       MirHostedStartupFailureBehavior::None, setupBlock->id);
    MirTerminator enterLoop;
    enterLoop.kind = MirTerminatorKind::Goto;
    enterLoop.target = loopHeader.id;
    terminate(enterLoopOperation, *setupBlock, std::move(enterLoop));

    const MirHostedStartupOperationId loadIndexOperation =
        beginOperation(MirHostedStartupOperationKind::LoadArgumentIndex,
                       MirHostedStartupFailureBehavior::None, loopHeader.id);
    const MirInstructionId loadIndexInstruction = nextInstruction;
    const MirValueId indexValue =
        appendValue(loadIndexOperation, loopHeader.id, loadIndexInstruction,
                    generatedInfo(SemanticType::Int32));
    MirInstruction loadIndex;
    loadIndex.kind = MirInstructionKind::Load;
    loadIndex.result = indexValue;
    loadIndex.operands = {{.kind = MirOperandKind::Copy,
                           .place = indexPlace,
                           .type = SemanticType::Int32}};
    loadIndex.info = generatedInfo(SemanticType::Int32);
    appendInstruction(loadIndexOperation, loopHeader, std::move(loadIndex));

    const MirHostedStartupOperationId testIndexOperation =
        beginOperation(MirHostedStartupOperationKind::TestArgumentIndex,
                       MirHostedStartupFailureBehavior::None, loopHeader.id);
    const MirInstructionId testIndexInstruction = nextInstruction;
    const MirValueId hasArgument =
        appendValue(testIndexOperation, loopHeader.id, testIndexInstruction,
                    generatedInfo(SemanticType::Bool));
    MirInstruction testIndex;
    testIndex.kind = MirInstructionKind::Compute;
    testIndex.result = hasArgument;
    testIndex.operands = {{.kind = MirOperandKind::Value,
                           .value = indexValue,
                           .type = SemanticType::Int32},
                          {.kind = MirOperandKind::Value,
                           .value = stabilizedCount,
                           .type = SemanticType::Int32}};
    testIndex.operation = MirOperation::Less;
    testIndex.info = generatedInfo(SemanticType::Bool);
    appendInstruction(testIndexOperation, loopHeader, std::move(testIndex));

    MirBlock &loopBody = appendBlock();
    MirBlock &entryCallBlock = appendBlock();
    MirBlock *iterationBlock = &loopBody;
    const MirHostedStartupOperationId branchLoopOperation =
        beginOperation(MirHostedStartupOperationKind::BranchArgumentLoop,
                       MirHostedStartupFailureBehavior::None, loopHeader.id);
    MirTerminator branchLoop;
    branchLoop.kind = MirTerminatorKind::Branch;
    branchLoop.value = MirOperand{.kind = MirOperandKind::Value,
                                  .value = hasArgument,
                                  .type = SemanticType::Bool};
    branchLoop.target = loopBody.id;
    branchLoop.elseTarget = entryCallBlock.id;
    terminate(branchLoopOperation, loopHeader, std::move(branchLoop));

    const MirHostedStartupOperationId viewOperation = beginOperation(
        MirHostedStartupOperationKind::ReadArgumentView,
        MirHostedStartupFailureBehavior::None, iterationBlock->id);
    const MirInstructionId viewInstruction = nextInstruction;
    const MirValueId argumentView =
        appendValue(viewOperation, iterationBlock->id, viewInstruction,
                    generatedInfo(SemanticType::StringView));
    MirInstruction readView;
    readView.kind = MirInstructionKind::Compute;
    readView.result = argumentView;
    readView.operands = {{.kind = MirOperandKind::Value,
                          .value = indexValue,
                          .type = SemanticType::Int32}};
    readView.operation = MirOperation::None;
    readView.info = generatedInfo(SemanticType::StringView);
    appendInstruction(viewOperation, *iterationBlock, std::move(readView));

    const MirHostedStartupOperationId stringInputOperation = beginOperation(
        MirHostedStartupOperationKind::PrepareStringConstructorArgument,
        MirHostedStartupFailureBehavior::None, iterationBlock->id);
    const MirInstructionId stringInputInstruction = nextInstruction;
    const MirValueId preparedView = appendValue(
        stringInputOperation, iterationBlock->id, stringInputInstruction,
        generatedInfo(SemanticType::StringView));
    MirInstruction prepareView;
    prepareView.kind = MirInstructionKind::CallInput;
    prepareView.callInputRole = MirCallInputRole::Argument;
    prepareView.callInputKind = HirCallInputKind::Value;
    prepareView.result = preparedView;
    prepareView.operands = {{.kind = MirOperandKind::Value,
                             .value = argumentView,
                             .type = SemanticType::StringView}};
    prepareView.info = generatedInfo(SemanticType::StringView);
    appendInstruction(stringInputOperation, *iterationBlock,
                      std::move(prepareView));

    const bool stringMayRaise = stringConstructor != nullptr &&
                                stringConstructor->mayRaiseDefinedFailure;
    const MirHostedStartupOperationId stringOperation =
        beginOperation(MirHostedStartupOperationKind::ConstructArgumentString,
                       failureBehavior(stringMayRaise), iterationBlock->id);
    const MirInstructionId stringInstruction = nextInstruction;
    const MirValueId stringValue =
        appendValue(stringOperation, iterationBlock->id, stringInstruction,
                    generatedInfo(stringType));
    const MirPlaceId stringPlace =
        appendPlace(stringOperation, {.root = MirPlaceRootKind::Value,
                                      .value = stringValue,
                                      .type = stringType,
                                      .access = AccessMode::Mutable,
                                      .traits = semanticTraits(stringType)});
    const MirDropObligationId stringDrop = appendDrop(
        stringOperation,
        {.kind = MirDropObligationKind::Value,
         .place = stringPlace,
         .generatedValue = stringValue,
         .dropType = generatedDropType(
             stringType,
             stringConstructor == nullptr ? 0 : stringConstructor->owner)});
    MirInstruction constructString;
    constructString.kind = MirInstructionKind::Construct;
    constructString.result = stringValue;
    constructString.operands = {{.kind = MirOperandKind::Value,
                                 .value = preparedView,
                                 .type = SemanticType::StringView}};
    constructString.parameterTypes = {SemanticType::StringView};
    constructString.constructorTarget = hirPlan->stringConstructor;
    constructString.definedFailure =
        propagation(FailurePropagationKind::Constructor, stringMayRaise);
    constructString.info = generatedInfo(stringType);
    if (stringMayRaise) {
      constructString.successResultDrop = stringDrop;
    } else {
      constructString.lifecycle = {
          {.kind = MirLifecycleEventKind::Initialize, .target = stringDrop}};
    }
    appendInstruction(stringOperation, *iterationBlock,
                      std::move(constructString));
    if (stringMayRaise) {
      routeOperationFailure(iterationBlock, stringInstruction, {vectorDrop},
                            stringDrop);
    }

    const MirHostedStartupOperationId appendReceiverOperation = beginOperation(
        MirHostedStartupOperationKind::PrepareAppendReceiver,
        MirHostedStartupFailureBehavior::None, iterationBlock->id);
    const MirInstructionId appendReceiverInstruction = nextInstruction;
    const MirValueId preparedReceiver =
        appendValue(appendReceiverOperation, iterationBlock->id,
                    appendReceiverInstruction, generatedInfo(vectorType));
    MirInstruction prepareReceiver;
    prepareReceiver.kind = MirInstructionKind::CallInput;
    prepareReceiver.callInputRole = MirCallInputRole::Receiver;
    prepareReceiver.callInputKind = HirCallInputKind::MutableBorrow;
    prepareReceiver.result = preparedReceiver;
    prepareReceiver.operands = {{.kind = MirOperandKind::BorrowWrite,
                                 .place = vectorPlace,
                                 .type = vectorType}};
    prepareReceiver.info = generatedInfo(vectorType);
    appendInstruction(appendReceiverOperation, *iterationBlock,
                      std::move(prepareReceiver));

    const MirHostedStartupOperationId appendArgumentOperation = beginOperation(
        MirHostedStartupOperationKind::PrepareAppendArgumentMove,
        MirHostedStartupFailureBehavior::None, iterationBlock->id);
    const MirInstructionId appendArgumentInstruction = nextInstruction;
    const MirValueId preparedString =
        appendValue(appendArgumentOperation, iterationBlock->id,
                    appendArgumentInstruction, generatedInfo(stringType));
    const MirPlaceId preparedStringPlace = appendPlace(
        appendArgumentOperation, {.root = MirPlaceRootKind::Temporary,
                                  .temporary = nextTemporary++,
                                  .type = stringType,
                                  .access = AccessMode::Mutable,
                                  .traits = semanticTraits(stringType)});
    const MirDropObligationId preparedStringDrop = appendDrop(
        appendArgumentOperation,
        {.kind = MirDropObligationKind::PreparedParameter,
         .place = preparedStringPlace,
         .generatedValue = preparedString,
         .dropType = generatedDropType(
             stringType,
             stringConstructor == nullptr ? 0 : stringConstructor->owner)});
    MirInstruction prepareString;
    prepareString.kind = MirInstructionKind::CallInput;
    prepareString.callInputRole = MirCallInputRole::Argument;
    prepareString.callInputKind = HirCallInputKind::MoveValue;
    prepareString.preparedParameterDrop = preparedStringDrop;
    prepareString.result = preparedString;
    prepareString.destination = preparedStringPlace;
    prepareString.operands = {{.kind = MirOperandKind::Value,
                               .value = stringValue,
                               .type = stringType}};
    prepareString.info = generatedInfo(stringType);
    prepareString.lifecycle = {{.kind = MirLifecycleEventKind::Reparent,
                                .source = stringDrop,
                                .target = preparedStringDrop}};
    appendInstruction(appendArgumentOperation, *iterationBlock,
                      std::move(prepareString));

    const bool appendMayRaise =
        append != nullptr && append->mayRaiseDefinedFailure;
    const MirHostedStartupOperationId appendCallOperation =
        beginOperation(MirHostedStartupOperationKind::CallAppend,
                       failureBehavior(appendMayRaise), iterationBlock->id);
    MirInstruction appendCall;
    appendCall.kind = MirInstructionKind::Call;
    appendCall.receiver = MirOperand{.kind = MirOperandKind::Value,
                                     .value = preparedReceiver,
                                     .type = vectorType};
    appendCall.operands = {{.kind = MirOperandKind::Value,
                            .value = preparedString,
                            .type = stringType}};
    appendCall.parameterTypes = {stringType};
    appendCall.functionTarget = hirPlan->appendFunction;
    appendCall.definedFailure =
        propagation(FailurePropagationKind::DirectCall, appendMayRaise);
    appendCall.info = generatedInfo(SemanticType::Void);
    appendCall.lifecycle = {{.kind = MirLifecycleEventKind::TransferOut,
                             .source = preparedStringDrop}};
    const MirInstructionId appendCallInstruction = appendInstruction(
        appendCallOperation, *iterationBlock, std::move(appendCall));
    if (appendMayRaise) {
      routeOperationFailure(iterationBlock, appendCallInstruction,
                            {vectorDrop});
    }

    const MirHostedStartupOperationId advanceOperation = beginOperation(
        MirHostedStartupOperationKind::AdvanceArgumentIndex,
        MirHostedStartupFailureBehavior::None, iterationBlock->id);
    const MirInstructionId advanceInstruction = nextInstruction;
    const MirValueId advancedIndex =
        appendValue(advanceOperation, iterationBlock->id, advanceInstruction,
                    generatedInfo(SemanticType::Int32, ValueCategory::Place,
                                  AccessMode::Mutable));
    MirInstruction advance;
    advance.kind = MirInstructionKind::Modify;
    advance.result = advancedIndex;
    advance.destination = indexPlace;
    advance.operation = MirOperation::PreIncrement;
    advance.info = generatedInfo(SemanticType::Int32, ValueCategory::Place,
                                 AccessMode::Mutable);
    appendInstruction(advanceOperation, *iterationBlock, std::move(advance));

    const MirHostedStartupOperationId continueOperation = beginOperation(
        MirHostedStartupOperationKind::ContinueArgumentLoop,
        MirHostedStartupFailureBehavior::None, iterationBlock->id);
    MirTerminator continueLoop;
    continueLoop.kind = MirTerminatorKind::Goto;
    continueLoop.target = loopHeader.id;
    terminate(continueOperation, *iterationBlock, std::move(continueLoop));

    MirBlock *finalBlock = &entryCallBlock;
    const MirHostedStartupOperationId entryCountOperation =
        beginOperation(MirHostedStartupOperationKind::PrepareEntryCount,
                       MirHostedStartupFailureBehavior::None, finalBlock->id);
    const MirInstructionId entryCountInstruction = nextInstruction;
    const MirValueId preparedCount =
        appendValue(entryCountOperation, finalBlock->id, entryCountInstruction,
                    generatedInfo(SemanticType::Int32));
    MirInstruction prepareCount;
    prepareCount.kind = MirInstructionKind::CallInput;
    prepareCount.callInputRole = MirCallInputRole::Argument;
    prepareCount.callInputKind = HirCallInputKind::Value;
    prepareCount.result = preparedCount;
    prepareCount.operands = {{.kind = MirOperandKind::Value,
                              .value = stabilizedCount,
                              .type = SemanticType::Int32}};
    prepareCount.info = generatedInfo(SemanticType::Int32);
    appendInstruction(entryCountOperation, *finalBlock,
                      std::move(prepareCount));

    const MirHostedStartupOperationId entryArgumentsOperation =
        beginOperation(MirHostedStartupOperationKind::PrepareEntryArgumentsMove,
                       MirHostedStartupFailureBehavior::None, finalBlock->id);
    const MirInstructionId entryArgumentsInstruction = nextInstruction;
    const MirValueId preparedArguments =
        appendValue(entryArgumentsOperation, finalBlock->id,
                    entryArgumentsInstruction, generatedInfo(vectorType));
    const MirPlaceId preparedArgumentsPlace = appendPlace(
        entryArgumentsOperation, {.root = MirPlaceRootKind::Temporary,
                                  .temporary = nextTemporary++,
                                  .type = vectorType,
                                  .access = AccessMode::Mutable,
                                  .traits = semanticTraits(vectorType)});
    const MirDropObligationId preparedArgumentsDrop = appendDrop(
        entryArgumentsOperation,
        {.kind = MirDropObligationKind::PreparedParameter,
         .place = preparedArgumentsPlace,
         .generatedValue = preparedArguments,
         .dropType = generatedDropType(
             vectorType,
             vectorConstructor == nullptr ? 0 : vectorConstructor->owner)});
    MirInstruction prepareArguments;
    prepareArguments.kind = MirInstructionKind::CallInput;
    prepareArguments.callInputRole = MirCallInputRole::Argument;
    prepareArguments.callInputIndex = 1;
    prepareArguments.callInputKind = HirCallInputKind::MoveValue;
    prepareArguments.preparedParameterDrop = preparedArgumentsDrop;
    prepareArguments.result = preparedArguments;
    prepareArguments.destination = preparedArgumentsPlace;
    prepareArguments.operands = {{.kind = MirOperandKind::Value,
                                  .value = vectorValue,
                                  .type = vectorType}};
    prepareArguments.info = generatedInfo(vectorType);
    prepareArguments.lifecycle = {{.kind = MirLifecycleEventKind::Reparent,
                                   .source = vectorDrop,
                                   .target = preparedArgumentsDrop}};
    appendInstruction(entryArgumentsOperation, *finalBlock,
                      std::move(prepareArguments));

    const bool entryMayRaise =
        entry != nullptr && entry->mayRaiseDefinedFailure;
    const MirHostedStartupOperationId entryCallOperation =
        beginOperation(MirHostedStartupOperationKind::CallEntry,
                       failureBehavior(entryMayRaise), finalBlock->id);
    const MirInstructionId entryCallInstruction = nextInstruction;
    const MirValueId entryResult =
        appendValue(entryCallOperation, finalBlock->id, entryCallInstruction,
                    generatedInfo(SemanticType::Int32));
    MirInstruction entryCall;
    entryCall.kind = MirInstructionKind::Call;
    entryCall.result = entryResult;
    entryCall.operands = {{.kind = MirOperandKind::Value,
                           .value = preparedCount,
                           .type = SemanticType::Int32},
                          {.kind = MirOperandKind::Value,
                           .value = preparedArguments,
                           .type = vectorType}};
    entryCall.parameterTypes = {SemanticType::Int32, vectorType};
    entryCall.functionTarget = hirPlan->entry;
    entryCall.definedFailure =
        propagation(FailurePropagationKind::DirectCall, entryMayRaise);
    entryCall.info = generatedInfo(SemanticType::Int32);
    entryCall.lifecycle = {{.kind = MirLifecycleEventKind::TransferOut,
                            .source = preparedArgumentsDrop}};
    appendInstruction(entryCallOperation, *finalBlock, std::move(entryCall));
    if (entryMayRaise) {
      routeOperationFailure(finalBlock, entryCallInstruction, {});
    }
    plan.entryResult = entryResult;

    const MirHostedStartupOperationId returnOperation =
        beginOperation(MirHostedStartupOperationKind::ReturnEntry,
                       MirHostedStartupFailureBehavior::None, finalBlock->id);
    MirTerminator returnEntry;
    returnEntry.kind = MirTerminatorKind::Return;
    returnEntry.value = MirOperand{.kind = MirOperandKind::Value,
                                   .value = entryResult,
                                   .type = SemanticType::Int32};
    terminate(returnOperation, *finalBlock, std::move(returnEntry));
  }

  rebuildMirReachability(body);
  valid = rebuildMirValueUses(body) && valid;
  program.hostedStartupPlan_ = std::move(plan);
  program.hostedStartupBody = std::move(body);
  return valid;
}

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
    MirProgramInitializationPlan *loweredProgramInitialization) {
  MirBodyLowerer lowerer(program, failureMetadata, body, kind,
                         std::move(returnType), definedFailureEffects,
                         implicitZeroReturn, function, constructor, lambda,
                         programInitialization, loweredProgramInitialization);
  MirBody result = lowerer.lower(prologueValues, initializers);
  valid = valid && lowerer.isValid();
  return result;
}

} // namespace lang
