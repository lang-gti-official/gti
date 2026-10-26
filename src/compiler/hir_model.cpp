#include "gti/hir.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace lang {

const HirValue *HirBody::valueForSource(const Expr &source) const {
  for (auto value = values.rbegin(); value != values.rend(); ++value)
    if (value->source == &source)
      return &*value;
  for (auto alias = directInitializations.rbegin();
       alias != directInitializations.rend(); ++alias)
    if (alias->source == &source)
      return findValue(alias->value);
  for (auto alias = sourceAliases.rbegin(); alias != sourceAliases.rend();
       ++alias)
    if (alias->source == &source)
      return findValue(alias->value);
  return nullptr;
}

const HirValue *HirBody::initializerValueForSource(const Expr &source) const {
  for (const HirContextualConstruction &construction : contextualConstructions)
    if (construction.source == &source)
      return findValue(construction.value);
  return valueForSource(source);
}

namespace {

[[nodiscard]] bool sameSpan(const SourceSpan &left, const SourceSpan &right) {
  return left.source == right.source && left.start == right.start &&
         left.end == right.end && left.line == right.line;
}

[[nodiscard]] bool exactHostedFailure(const DefinedFailureOperation &operation,
                                      DefinedFailureCode code,
                                      DefinedFailureDetail detail,
                                      SourceUnitId sourceUnit,
                                      const SourceSpan &anchor) {
  if (operation.propagation != FailurePropagationKind::None ||
      operation.localOrigins.size() != 1) {
    return false;
  }
  const DefinedFailureOrigin &origin = operation.localOrigins.front();
  return origin.sourceUnit == sourceUnit && origin.start == anchor.start &&
         origin.end == anchor.end && origin.line == anchor.line &&
         origin.outcomes.size() == 1 &&
         origin.outcomes.front() ==
             DefinedFailureOutcome{.code = code, .detail = detail};
}

// The call a contextual conversion adds after the value it converts: both
// share one source, and the call's receiver is that value. The source's own
// records describe the converted value, not this call.
[[nodiscard]] bool contextualConversionCall(const HirBody &body,
                                            const HirValue &value,
                                            const SemanticModel &semantics) {
  if (value.kind != HirValueKind::Call || !value.functionTarget ||
      !value.receiver || value.source == nullptr ||
      semantics.findContextualConversion(*value.source) == nullptr) {
    return false;
  }
  const HirValue *converted = body.findValue(*value.receiver);
  return converted != nullptr && converted->source == value.source;
}

[[nodiscard]] bool
materializableProgramConstant(const std::optional<ConstantValue> &constant) {
  return constant && !std::holds_alternative<ConstantAggregateValue>(*constant);
}

} // namespace

// Initializer roots are values rather than body roots. A scoped result adds a
// statement subgraph at that value's evaluation position; only that subgraph,
// not arbitrary body statements, belongs to the selected initialization plan.
std::vector<HirInitializerReachability> hirInitializerReachabilityBatch(
    const HirBody &body,
    const std::vector<std::vector<HirValueId>> &rootGroups) {
  bool complete = true;
  std::unordered_map<HirValueId, const HirValue *> values;
  std::unordered_map<HirStatementId, const HirStatement *> statements;
  std::unordered_set<HirBindingId> bindings;
  values.reserve(body.values.size());
  statements.reserve(body.statements.size());
  bindings.reserve(body.bindings.size());
  for (const auto &value : body.values)
    complete &= values.emplace(value.id, &value).second;
  for (const auto &statement : body.statements)
    complete &= statements.emplace(statement.id, &statement).second;
  for (const auto &binding : body.bindings)
    complete &= bindings.insert(binding.id).second;
  std::vector<HirInitializerReachability> results;
  results.reserve(rootGroups.size());
  for (const auto &roots : rootGroups) {
    auto &reachable = results.emplace_back();
    reachable.complete = complete;
    std::vector<HirValueId> pendingValues = roots;
    std::vector<HirStatementId> pendingStatements;
    const auto binding = [&](HirBindingId id) {
      if (id != 0) {
        reachable.bindings.insert(id);
        reachable.complete &= bindings.contains(id);
      }
    };
    while (!pendingValues.empty() || !pendingStatements.empty()) {
      if (!pendingValues.empty()) {
        const auto id = pendingValues.back();
        pendingValues.pop_back();
        if (id == 0 || !reachable.values.insert(id).second)
          continue;
        const auto found = values.find(id);
        if (found == values.end()) {
          reachable.complete = false;
          continue;
        }
        const auto &value = *found->second;
        pendingValues.insert(pendingValues.end(), value.operands.begin(),
                             value.operands.end());
        pendingValues.insert(pendingValues.end(), value.sequencedInputs.begin(),
                             value.sequencedInputs.end());
        if (value.receiver)
          pendingValues.push_back(*value.receiver);
        if (value.regionBody && value.kind == HirValueKind::ScopedResult)
          pendingStatements.push_back(*value.regionBody);
        continue;
      }
      const auto id = pendingStatements.back();
      pendingStatements.pop_back();
      if (id == 0 || !reachable.statements.insert(id).second)
        continue;
      const auto found = statements.find(id);
      if (found == statements.end()) {
        reachable.complete = false;
        continue;
      }
      const auto &statement = *found->second;
      if (statement.binding)
        binding(*statement.binding);
      for (const auto input :
           {statement.value, statement.condition, statement.increment})
        if (input)
          pendingValues.push_back(*input);
      for (const auto child :
           {statement.initializer, statement.body, statement.elseBranch})
        if (child)
          pendingStatements.push_back(*child);
      pendingStatements.insert(pendingStatements.end(),
                               statement.statements.begin(),
                               statement.statements.end());
      for (const auto &element : statement.structuredBindings) {
        binding(element.binding);
        if (element.index)
          pendingValues.push_back(*element.index);
      }
      for (const auto &arm : statement.switchArms) {
        for (const auto &label : arm.labels)
          if (label.value)
            pendingValues.push_back(*label.value);
        for (const auto &payload : arm.payloadBindings) {
          binding(payload.binding);
          pendingValues.push_back(payload.value);
        }
        pendingStatements.insert(pendingStatements.end(),
                                 arm.statements.begin(), arm.statements.end());
      }
      if (const auto &range = statement.rangeIteration) {
        for (const auto id :
             {range->owner, range->rangeCarrier, range->iterator,
              range->sentinel, range->element, range->elementOwner})
          binding(id);
        for (const auto id :
             {range->source, range->begin, range->end, range->comparison,
              range->elementAccessValue, range->increment})
          pendingValues.push_back(id);
        pendingStatements.push_back(range->elementOwnerStatement);
        pendingStatements.push_back(range->coreLoop);
      }
    }
  }
  return results;
}

HirInitializerReachability
hirInitializerReachability(const HirBody &body,
                           const std::vector<HirValueId> &roots) {
  auto results = hirInitializerReachabilityBatch(body, {roots});
  return std::move(results.front());
}

namespace {

template <typename Nodes, typename Ids>
[[nodiscard]] bool exactInventory(const Nodes &nodes, const Ids &ids) {
  return nodes.size() == ids.size() &&
         std::all_of(nodes.begin(), nodes.end(),
                     [&](const auto &node) { return ids.contains(node.id); });
}

[[nodiscard]] bool containsUnresolvedGeneric(const CompileTimeValue &value) {
  return value.kind != CompileTimeValue::UInt64;
}

[[nodiscard]] bool containsUnresolvedGeneric(const SemanticType &type) {
  if (type.kind == SemanticType::TypeParameter ||
      (type.kind == SemanticType::TypePack && !type.concretePack) ||
      type.arrayLengthParameterId != 0 ||
      std::any_of(type.valueArguments.begin(), type.valueArguments.end(),
                  [](const CompileTimeValue &value) {
                    return containsUnresolvedGeneric(value);
                  }) ||
      std::any_of(type.lambdaEnclosingClassValues.begin(),
                  type.lambdaEnclosingClassValues.end(),
                  [](const CompileTimeValue &value) {
                    return containsUnresolvedGeneric(value);
                  }) ||
      std::any_of(type.lambdaEnclosingFunctionValues.begin(),
                  type.lambdaEnclosingFunctionValues.end(),
                  [](const CompileTimeValue &value) {
                    return containsUnresolvedGeneric(value);
                  })) {
    return true;
  }
  const auto containsType = [](const SemanticType &argument) {
    return containsUnresolvedGeneric(argument);
  };
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     containsType) ||
         std::any_of(type.lambdaEnclosingClassTypes.begin(),
                     type.lambdaEnclosingClassTypes.end(), containsType) ||
         std::any_of(type.lambdaEnclosingFunctionTypes.begin(),
                     type.lambdaEnclosingFunctionTypes.end(), containsType);
}

[[nodiscard]] SemanticType referentOrSelf(const SemanticType &type) {
  return type.kind == SemanticType::Reference && type.arguments.size() == 1
             ? type.arguments.front()
             : type;
}

} // namespace

const HirValue *HirBody::findValue(HirValueId id) const {
  const auto found =
      std::find_if(values.begin(), values.end(),
                   [id](const HirValue &value) { return value.id == id; });
  return found == values.end() ? nullptr : &*found;
}

const HirStatement *HirBody::findStatement(HirStatementId id) const {
  const auto found = std::find_if(
      statements.begin(), statements.end(),
      [id](const HirStatement &statement) { return statement.id == id; });
  return found == statements.end() ? nullptr : &*found;
}

const HirLoan *HirBody::findLoan(SemanticLoanId id) const {
  const auto found =
      std::find_if(loans.begin(), loans.end(), [id](const HirLoan &loan) {
        return loan.semanticLoan == id;
      });
  return found == loans.end() ? nullptr : &*found;
}

std::size_t HirProgram::valueCount() const {
  std::size_t count = moduleBody.values.size();
  for (const auto &storage : threadLocalStorage_)
    count += storage.initializer.values.size();
  for (const HirClassInstance &classInstance : classes) {
    count += classInstance.fieldInitializers.values.size();
    count += classInstance.staticFieldInitializers.values.size();
  }
  for (const HirFunctionInstance &function : functions) {
    count += function.body.values.size();
  }
  for (const HirConstructorInstance &constructor : constructors) {
    count += constructor.body.values.size();
  }
  for (const HirDestructorInstance &destructor : destructors) {
    count += destructor.body.values.size();
  }
  for (const HirLambda &lambda : lambdas) {
    count += lambda.body.values.size();
  }
  return count;
}

std::size_t HirProgram::statementCount() const {
  std::size_t count = moduleBody.statements.size();
  for (const auto &storage : threadLocalStorage_)
    count += storage.initializer.statements.size();
  for (const HirClassInstance &classInstance : classes) {
    count += classInstance.fieldInitializers.statements.size();
    count += classInstance.staticFieldInitializers.statements.size();
  }
  for (const HirFunctionInstance &function : functions) {
    count += function.body.statements.size();
  }
  for (const HirConstructorInstance &constructor : constructors) {
    count += constructor.body.statements.size();
  }
  for (const HirDestructorInstance &destructor : destructors) {
    count += destructor.body.statements.size();
  }
  for (const HirLambda &lambda : lambdas) {
    count += lambda.body.statements.size();
  }
  return count;
}

const std::vector<HirValueId> &
HirProgram::valueIdsForSource(const Expr &source) const {
  static const std::vector<HirValueId> empty;
  const auto found = sourceValueIds.find(&source);
  return found == sourceValueIds.end() ? empty : found->second;
}

HirProgramPlanVerificationResult
verifyHirProgramPlans(const SemanticModel &semantics,
                      const HirProgram &program) {
  HirProgramPlanVerificationResult result;
  for (const HirEnum &enumeration : program.enumDeclarations()) {
    if (!enumeration.payload)
      continue;
    const auto *source = semantics.findEnumType(enumeration.declaration);
    bool exact = source != nullptr && source->payload &&
                 source->enumerators.size() == enumeration.enumerators.size();
    std::unordered_set<SymbolId> fields;
    for (std::size_t variant = 0;
         exact && variant < enumeration.enumerators.size(); ++variant) {
      const auto &row = enumeration.enumerators[variant];
      exact =
          row.payloadSymbols == source->enumerators[variant].payloadSymbols &&
          row.payloadSymbols.size() == row.payloadTypes.size();
      for (const auto field : row.payloadSymbols)
        exact = exact && field != 0 && fields.insert(field).second;
    }
    if (!exact)
      result.errors.emplace_back(
          "payload enum lost its exact variant field declaration identities");
  }
  std::unordered_map<SymbolId, const SymbolRecord *> instanceSymbols;
  for (const SymbolRecord &symbol : program.instanceSymbols()) {
    if (symbol.id == 0 ||
        semantics.database().findSymbol(symbol.id) != nullptr ||
        !instanceSymbols.emplace(symbol.id, &symbol).second ||
        symbol.name.empty() || symbol.sourceUnit == 0 ||
        symbol.nameSpan.end <= symbol.nameSpan.start) {
      result.errors.emplace_back(
          "HIR concrete symbol inventory has invalid or overlapping identity");
    }
  }
  const auto verifyConcreteBindings = [&](const HirBody &body,
                                          const std::vector<HirValueId>
                                              &initializerValues = {}) {
    std::unordered_set<HirValueId> expandedCalls;
    std::unordered_set<HirValueId> expandedInputs;
    std::unordered_set<HirValueId> expandedElements;
    const auto exactNamedCopy = [](const HirValue &copy,
                                   const HirValue &source) {
      return source.kind == HirValueKind::Variable &&
             copy.kind == HirValueKind::Variable && copy.source == nullptr &&
             copy.id != source.id && copy.symbol == source.symbol &&
             copy.info == source.info && copy.place == source.place &&
             copy.ownership == source.ownership &&
             copy.unsafeOperation == source.unsafeOperation &&
             copy.definedFailure == source.definedFailure &&
             copy.constant == source.constant &&
             copy.programConstantSubstitution ==
                 source.programConstantSubstitution &&
             copy.borrowDependencies == source.borrowDependencies &&
             copy.borrowPlace == source.borrowPlace &&
             copy.borrowOrigin == source.borrowOrigin &&
             copy.borrowArgument == source.borrowArgument &&
             copy.borrowArguments == source.borrowArguments &&
             copy.borrowFields == source.borrowFields &&
             copy.borrowAccess == source.borrowAccess &&
             copy.operands.empty() && copy.sequencedInputs.empty() &&
             !copy.functionTarget && !copy.constructorTarget &&
             !copy.lambdaTarget && !copy.nativeCalleeType && !copy.callPlan &&
             !copy.packElement;
    };
    for (const auto &fold : body.values) {
      if (fold.kind != HirValueKind::PackFold)
        continue;
      const auto *source = exprAs<PackFold>(fold.source);
      const auto *pattern =
          source ? exprAs<Call>(source->pattern().get()) : nullptr;
      bool exact = pattern && fold.info.type == SemanticType::Void &&
                   fold.packFoldSymbol != 0 && fold.packFoldParameter != 0 &&
                   fold.packFoldFunction != 0 &&
                   fold.operands.size() ==
                       fold.packFoldArgument + 1 + fold.packFoldElements.size();
      const auto *pack =
          exact ? body.findValue(fold.operands[fold.packFoldArgument])
                : nullptr;
      const auto binding = std::find_if(
          body.bindings.begin(), body.bindings.end(), [&](const auto &entry) {
            return entry.info.symbol == fold.packFoldSymbol;
          });
      exact =
          exact && pack && pack->kind == HirValueKind::Variable &&
          pack->symbol == fold.packFoldSymbol &&
          pack->info.type.kind == SemanticType::TypePack &&
          pack->info.type.concretePack &&
          pack->info.type.genericParameterId == fold.packFoldParameter &&
          pack->info.type.arguments.size() == fold.packFoldElements.size() &&
          binding != body.bindings.end() && binding->parameter &&
          binding->info.type == pack->info.type;
      for (std::size_t index = 0; exact && index < fold.packFoldElements.size();
           ++index) {
        const auto &element = fold.packFoldElements[index];
        const auto *call = body.findValue(element.call);
        const auto *target =
            program.findFunctionInstance(element.functionTarget);
        exact =
            call && target && expandedCalls.insert(element.call).second &&
            fold.operands[fold.packFoldArgument + 1 + index] == element.call &&
            element.elementType == pack->info.type.arguments[index] &&
            element.parameterTypes.size() == fold.packFoldArgument + 1 &&
            target->declaration == fold.packFoldFunction && !target->owner &&
            target->returnType == SemanticType::Void &&
            target->parameterTypes == element.parameterTypes &&
            target->typeArguments ==
                std::vector<SemanticType>{element.elementType} &&
            target->valueArguments.empty() &&
            call->kind == HirValueKind::Call && call->source == pattern &&
            call->info == fold.info && call->dispatch == CallDispatch::Static &&
            call->functionTarget == element.functionTarget &&
            call->parameterTypes == element.parameterTypes &&
            call->operands.size() == fold.packFoldArgument + 1 &&
            call->callPlan && !call->callPlan->receiver && !call->receiver &&
            call->callPlan->arguments.size() == call->operands.size() &&
            call->sequencedInputs.empty();
        for (std::size_t argument = 0;
             exact && argument < call->operands.size(); ++argument) {
          const auto &input = call->callPlan->arguments[argument];
          const auto *value = body.findValue(call->operands[argument]);
          exact = value && input.value == value->id &&
                  input.parameterIndex == argument &&
                  input.parameterType == element.parameterTypes[argument] &&
                  !input.defaultArgument;
          if (!exact)
            break;
          if (argument < fold.packFoldArgument) {
            const auto *original = body.findValue(fold.operands[argument]);
            const auto &parameter = element.parameterTypes[argument];
            const auto expected =
                parameter.kind == SemanticType::Reference
                    ? (parameter.referenceAccess == AccessMode::Mutable
                           ? HirCallInputKind::MutableBorrow
                           : HirCallInputKind::ReadBorrow)
                : parameter.kind == SemanticType::Class ||
                        parameter.kind == SemanticType::Lambda ||
                        (parameter.kind == SemanticType::Enum && original &&
                         original->info.traits.drop == DropKind::Lexical)
                    ? HirCallInputKind::CopyValue
                    : HirCallInputKind::Value;
            exact = original && exactNamedCopy(*value, *original) &&
                    input.kind == expected &&
                    expandedInputs.insert(value->id).second;
          } else {
            const auto &parameter = element.parameterTypes[argument];
            exact = value->kind == HirValueKind::PackElement &&
                    value->packElement == index && value->source == nullptr &&
                    value->symbol == pack->symbol &&
                    value->operands == std::vector<HirValueId>{pack->id} &&
                    value->info.type == element.elementType &&
                    value->info.category == ValueCategory::Place &&
                    value->info.access == AccessMode::ReadOnly &&
                    value->definedFailure.empty() &&
                    value->unsafeOperation == UnsafeOperationKind::None &&
                    !value->functionTarget && !value->constructorTarget &&
                    !value->lambdaTarget && !value->callPlan && !value->place &&
                    !value->ownership && value->sequencedInputs.empty() &&
                    parameter.kind == SemanticType::Reference &&
                    parameter.referenceAccess == AccessMode::ReadOnly &&
                    parameter.arguments ==
                        std::vector<SemanticType>{element.elementType} &&
                    input.kind == HirCallInputKind::ReadBorrow &&
                    expandedElements.insert(value->id).second;
          }
        }
      }
      if (!exact)
        result.errors.emplace_back(
            "HIR fold lost its exact ordered call, named input or concrete "
            "pack-element projection");
    }
    for (const auto &value : body.values)
      if ((value.kind == HirValueKind::PackElement) !=
              value.packElement.has_value() ||
          (value.kind == HirValueKind::PackElement &&
           !expandedElements.contains(value.id)))
        result.errors.emplace_back(
            "HIR pack-element place has no unique typed expansion owner");
    const bool hasResultRegion =
        std::any_of(body.values.begin(), body.values.end(),
                    [](const auto &value) {
                      return value.kind == HirValueKind::ScopedResult ||
                             value.regionBody;
                    }) ||
        std::any_of(
            body.statements.begin(), body.statements.end(),
            [](const auto &statement) { return statement.resultRegion != 0; });
    if (hasResultRegion) {
      std::unordered_map<HirValueId, const HirValue *> values;
      std::unordered_map<HirStatementId, const HirStatement *> statements;
      values.reserve(body.values.size());
      statements.reserve(body.statements.size());
      for (const auto &node : body.values)
        if (!values.emplace(node.id, &node).second)
          result.errors.emplace_back(
              "HIR result region repeats a value identity");
      for (const auto &node : body.statements)
        if (!statements.emplace(node.id, &node).second)
          result.errors.emplace_back(
              "HIR result region repeats a statement identity");
      std::unordered_map<HirStatementId, HirValueId> statementRegions;
      std::unordered_set<HirValueId> checkedValues;
      std::unordered_set<HirValueId> activeValues;
      std::unordered_set<HirStatementId> activeStatements;
      std::function<void(HirStatementId, HirValueId)> statement;
      std::function<void(HirValueId, HirValueId)> value;
      value = [&](HirValueId id, HirValueId region) {
        const auto found = values.find(id);
        const auto *node = found == values.end() ? nullptr : found->second;
        if (!node || activeValues.contains(id)) {
          result.errors.emplace_back(
              "HIR result region has a missing/cyclic value");
          return;
        }
        if (!checkedValues.insert(id).second)
          return;
        activeValues.insert(id);
        if (node->kind == HirValueKind::ScopedResult) {
          if (!node->regionBody || !exprAs<Call>(node->source) ||
              node->info.category != ValueCategory::Value ||
              node->info.traits.containsBorrowedState ||
              !node->operands.empty() || node->functionTarget ||
              node->intrinsic != IntrinsicKind::None) {
            result.errors.emplace_back(
                "HIR scoped result has an invalid region contract");
          } else {
            statement(*node->regionBody, node->id);
          }
        } else if (node->regionBody) {
          result.errors.emplace_back(
              "HIR ordinary value cannot own a result region");
        }
        for (const auto operand : node->operands)
          value(operand, region);
        for (const auto input : node->sequencedInputs)
          value(input, region);
        if (node->receiver)
          value(*node->receiver, region);
        activeValues.erase(id);
      };
      statement = [&](HirStatementId id, HirValueId region) {
        if (activeStatements.contains(id)) {
          result.errors.emplace_back(
              "HIR result region has a cyclic statement");
          return;
        }
        const auto entry = statements.find(id);
        const auto *node = entry == statements.end() ? nullptr : entry->second;
        const auto [found, inserted] = statementRegions.emplace(id, region);
        if (!node || (!inserted && found->second != region)) {
          result.errors.emplace_back(
              "HIR statement crosses result-region boundaries");
          return;
        }
        if (!inserted)
          return;
        activeStatements.insert(id);
        if ((node->kind == HirStatementKind::Return &&
             node->resultRegion != region) ||
            (node->kind != HirStatementKind::Return && node->resultRegion != 0))
          result.errors.emplace_back(
              "HIR return has a missing or forged result-region target");
        for (const auto input : {node->value, node->condition, node->increment})
          if (input)
            value(*input, region);
        for (const auto child :
             {node->initializer, node->body, node->elseBranch})
          if (child)
            statement(*child, region);
        for (const auto child : node->statements)
          statement(child, region);
        for (const auto &arm : node->switchArms)
          for (const auto child : arm.statements)
            statement(child, region);
        activeStatements.erase(id);
      };
      for (const auto root : body.roots)
        statement(root, 0);
      for (const auto root : initializerValues)
        value(root, 0);
    }
    for (const HirBinding &binding : body.bindings) {
      const auto found = instanceSymbols.find(binding.info.symbol);
      if (found != instanceSymbols.end() &&
          (found->second->type != binding.info.type ||
           found->second->traits != binding.info.traits ||
           found->second->mutableBinding !=
               (binding.info.access == AccessMode::Mutable))) {
        result.errors.emplace_back(
            "HIR concrete binding differs from its semantic symbol record");
      }
    }
  };
  verifyConcreteBindings(program.module());
  for (const auto &storage : program.threadLocalStorage())
    verifyConcreteBindings(storage.initializer);
  for (const auto &instance : program.functionInstances())
    verifyConcreteBindings(instance.body);
  for (const auto &instance : program.constructorInstances())
    verifyConcreteBindings(instance.body, instance.initializerValues);
  for (const auto &instance : program.destructorInstances())
    verifyConcreteBindings(instance.body);
  for (const auto &instance : program.lambdaInstances())
    verifyConcreteBindings(instance.body);
  for (const auto &instance : program.classInstances()) {
    verifyConcreteBindings(instance.fieldInitializers);
    verifyConcreteBindings(instance.staticFieldInitializers);
    const ClassTypeInfo *owner = semantics.findClassType(instance.declaration);
    if (owner == nullptr ||
        owner->typeAliases.size() != instance.typeAliases.size()) {
      result.errors.emplace_back(
          "HIR class alias inventory differs from semantics");
      continue;
    }
    GenericSubstitution substitution;
    std::size_t typeIndex = 0, valueIndex = 0;
    for (const auto &parameter : owner->genericParameters) {
      if (!parameter.typeArgumentPath.empty()) {
        if (auto type = projectGenericTypeArgument(
                instance.typeArguments, parameter.typeArgumentPath,
                parameter.id, parameter.pack))
          substitution.types.emplace(parameter.id, std::move(*type));
      } else if (parameter.value) {
        if (valueIndex < instance.valueArguments.size())
          substitution.values.emplace(parameter.id,
                                      instance.valueArguments[valueIndex++]);
      } else if (parameter.pack) {
        std::vector<SemanticType> elements(
            instance.typeArguments.begin() +
                static_cast<std::ptrdiff_t>(typeIndex),
            instance.typeArguments.end());
        substitution.types.emplace(
            parameter.id,
            SemanticType::concreteTypePack(parameter.id, std::move(elements)));
        typeIndex = instance.typeArguments.size();
      } else if (typeIndex < instance.typeArguments.size()) {
        substitution.types.emplace(parameter.id,
                                   instance.typeArguments[typeIndex++]);
      }
    }
    if (instance.virtualDispatch.size() != owner->virtualDispatch.size())
      result.errors.emplace_back(
          "HIR virtual dispatch inventory differs from semantic selection");
    const auto exactDispatchPath = [&](const auto &path,
                                       HirClassInstanceId endpoint) {
      auto current = instance.id;
      std::unordered_set<HirClassInstanceId> seen{current};
      for (const auto next : path) {
        if (!current || current > program.classInstances().size() ||
            !seen.insert(next).second)
          return false;
        const auto &bases = program.classInstances()[current - 1].bases;
        const auto found =
            std::find_if(bases.begin(), bases.end(), [&](const auto &base) {
              return base.instance == next;
            });
        if (found == bases.end() || !next ||
            next > program.classInstances().size() ||
            program.classInstances()[next - 1].type != found->type)
          return false;
        current = next;
      }
      return current == endpoint && endpoint != 0 &&
             endpoint <= program.classInstances().size();
    };
    std::unordered_set<HirFunctionInstanceId> dispatchRoots;
    for (std::size_t index = 0; index < instance.virtualDispatch.size();
         ++index) {
      const auto &row = instance.virtualDispatch[index];
      const auto *root = program.findFunctionInstance(row.root);
      const auto *target = program.findFunctionInstance(row.target);
      const auto *selected = index < owner->virtualDispatch.size()
                                 ? &owner->virtualDispatch[index]
                                 : nullptr;
      if (!selected || !root || !target ||
          !dispatchRoots.insert(row.root).second ||
          root->declaration != selected->root ||
          target->declaration != selected->target ||
          root->owner != row.rootOwner || target->owner != row.targetOwner ||
          !exactDispatchPath(row.rootPath, row.rootOwner) ||
          !exactDispatchPath(row.targetPath, row.targetOwner) ||
          program.classInstances()[row.targetOwner - 1].type !=
              substituteSemanticType(selected->targetOwner, substitution,
                                     &semantics) ||
          !root->virtualMethod || !target->virtualMethod ||
          root->staticMember || target->staticMember ||
          root->parameterTypes != target->parameterTypes ||
          root->returnType != target->returnType ||
          (target->pureVirtual && !instance.abstract) ||
          (!target->pureVirtual &&
           (!target->source || !target->source->body())))
        result.errors.emplace_back(
            "HIR virtual dispatch differs from its exact semantic "
            "root/target/owner/path contract");
    }
    for (std::size_t index = 0; index < instance.typeAliases.size(); ++index) {
      const auto &alias = instance.typeAliases[index];
      const TypeAliasInfo *primary =
          semantics.findTypeAlias(*owner->typeAliases[index]);
      if (primary == nullptr || alias.source != owner->typeAliases[index] ||
          alias.declaration != primary->id || alias.declaration == 0 ||
          containsUnresolvedGeneric(alias.type) ||
          alias.type !=
              substituteSemanticType(primary->type, substitution, &semantics))
        result.errors.emplace_back(
            "HIR class alias differs from its exact semantic owner/type");
    }
  }
  if (program.analysisSeal() != semantics.analysisSeal()) {
    result.errors.emplace_back(
        "HIR semantic-analysis seal differs from the semantic model");
  }
  for (const auto &function : program.functionInstances()) {
    const bool textSignature =
        std::any_of(function.parameterTypes.begin(),
                    function.parameterTypes.end(), [](const auto &type) {
                      return type.kind == SemanticType::StringView ||
                             type.kind == SemanticType::CString;
                    });
    if (!textSignature && function.ownerlessTextParameters.empty() &&
        function.ownerlessTextFields.empty() &&
        function.publishedTextFields.empty())
      continue;
    const auto &proof = function.ownerlessTextContractProof;
    // HIR publishes a concrete instance's parameters with packs expanded, so
    // a semantic signature that still groups a concrete pack is compared in
    // that form. Required indices name source formals ahead of the pack,
    // which expansion leaves in place.
    std::vector<SemanticType> proofParameters;
    if (proof) {
      for (SemanticType parameter : proof->parameters())
        SemanticType::appendExpandedArgument(proofParameters,
                                             std::move(parameter));
    }
    if (!proof || proof->source() != function.source ||
        proof->function() != function.declaration ||
        proofParameters != function.parameterTypes ||
        proof->required() != function.ownerlessTextParameters ||
        proof->requiredFields() != function.ownerlessTextFields ||
        proof->published() != function.publishedTextFields ||
        proof->snapshot() != program.analysisSeal().programSnapshot)
      result.errors.emplace_back("HIR ownerless text contract differs from its "
                                 "exact semantic signature and proof");
  }
  if (program.programKind() != semantics.programKind()) {
    result.errors.emplace_back(
        "HIR program kind differs from the semantic model");
  }
  const std::optional<SemanticEmbeddingPlan> &semanticEmbedding =
      semantics.embeddingPlan();
  const std::optional<HirEmbeddingPlan> &embedding = program.embeddingPlan();
  if (semanticEmbedding.has_value() != embedding.has_value()) {
    result.errors.emplace_back(
        "embedding plan presence differs from semantics");
  } else if (semanticEmbedding && embedding) {
    if (program.programKind() != ProgramKind::Library ||
        embedding->abiVersion != embeddingAbiVersionV1 ||
        embedding->abiVersion != semanticEmbedding->abiVersion ||
        embedding->exports.empty() ||
        embedding->exports.size() != semanticEmbedding->exports.size()) {
      result.errors.emplace_back(
          "embedding HIR plan has invalid program or ABI provenance");
    }
    std::string previousSymbol;
    for (std::size_t index = 0; index < embedding->exports.size() &&
                                index < semanticEmbedding->exports.size();
         ++index) {
      const HirEmbeddingExport &lowered = embedding->exports[index];
      const SemanticEmbeddingExport &semantic =
          semanticEmbedding->exports[index];
      const HirFunctionInstance *instance =
          program.findFunctionInstance(lowered.function);
      const FunctionInfo *declaration =
          semantics.findFunction(lowered.semanticFunction);
      if ((index != 0 && previousSymbol >= lowered.symbol) ||
          lowered.semanticFunction != semantic.function ||
          lowered.qualifiedName != semantic.qualifiedName ||
          lowered.symbol != semantic.symbol ||
          lowered.returnType != semantic.returnType ||
          lowered.parameterTypes != semantic.parameterTypes ||
          !sameSpan(lowered.requestSource, semantic.requestSource) ||
          instance == nullptr || declaration == nullptr ||
          instance->id != lowered.function ||
          instance->declaration != lowered.semanticFunction ||
          instance->source != declaration->declaration || instance->owner ||
          !instance->typeArguments.empty() ||
          !instance->valueArguments.empty() ||
          instance->returnType != lowered.returnType ||
          instance->parameterTypes != lowered.parameterTypes ||
          instance->entryKind != ProgramEntryKind::None ||
          instance->linkage != LanguageLinkage::Gti) {
        result.errors.emplace_back(
            "embedding HIR export differs from its exact semantic function");
      }
      previousSymbol = lowered.symbol;
    }
  }
  if (program.programKind() == ProgramKind::Executable && embedding) {
    result.errors.emplace_back("executable HIR carries an embedding plan");
  }
  if (program.programKind() == ProgramKind::Library &&
      (!embedding || program.hostedProgramEntryPlan() ||
       !program.nativeCallbackAdapters().empty())) {
    result.errors.emplace_back(
        "library HIR lacks its export plan or retains an unsafe hosted root");
  }
  const auto verifyNativeInvocations = [&](const HirBody &body) {
    for (const auto &value : body.values) {
      if (!value.nativeCalleeType)
        continue;
      const auto &signature = *value.nativeCalleeType;
      const auto parameters = signature.nativeFunctionParameterTypes();
      const auto *callee =
          value.receiver ? body.findValue(*value.receiver) : nullptr;
      const bool expandedArguments =
          value.receiver && value.operands.size() > 1 &&
          value.operands.front() == *value.receiver &&
          std::any_of(value.operands.begin() + 1, value.operands.end(),
                      [&](HirValueId id) {
                        const HirValue *argument = body.findValue(id);
                        return argument &&
                               argument->kind == HirValueKind::PackExpansion &&
                               argument->info.type.kind ==
                                   SemanticType::TypePack &&
                               argument->info.type.concretePack;
                      });
      const bool orderedPlan =
          value.receiver && value.callPlan && value.callPlan->receiver &&
          value.callPlan->receiver->value == *value.receiver &&
          value.callPlan->receiver->type == signature &&
          value.callPlan->receiver->kind == HirCallInputKind::Value &&
          value.callPlan->arguments.size() == parameters.size();
      if (value.kind != HirValueKind::Call ||
          !signature.hasNativeFunctionShape() || !callee ||
          callee->info.type != signature || value.functionTarget ||
          value.constructorTarget || value.lambdaTarget ||
          value.nativeCallbackAdapter || (!orderedPlan && !expandedArguments) ||
          value.unsafeOperation != UnsafeOperationKind::ForeignPointerCall ||
          value.info.type != *signature.nativeFunctionReturnType() ||
          value.parameterTypes.size() != parameters.size() ||
          !std::equal(parameters.begin(), parameters.end(),
                      value.parameterTypes.begin())) {
        result.errors.emplace_back(
            "native indirect invocation lost its exact signature and callee "
            "plan");
      }
    }
  };
  verifyNativeInvocations(program.module());
  for (const auto &storage : program.threadLocalStorage())
    verifyNativeInvocations(storage.initializer);
  for (const auto &function : program.functionInstances())
    verifyNativeInvocations(function.body);
  for (const auto &constructor : program.constructorInstances())
    verifyNativeInvocations(constructor.body);
  for (const auto &destructor : program.destructorInstances())
    verifyNativeInvocations(destructor.body);
  for (const auto &lambda : program.lambdaInstances())
    verifyNativeInvocations(lambda.body);
  for (const auto &instance : program.functionInstances()) {
    const auto *declaration = semantics.findFunction(instance.declaration);
    if (declaration == nullptr ||
        instance.requiresUnsafe != declaration->requiresUnsafe ||
        instance.neverReturns != declaration->neverReturns ||
        instance.tracksCaller != declaration->tracksCaller ||
        instance.cThreadOperation != declaration->cThreadOperation ||
        instance.processMemoryOperation !=
            declaration->processMemoryOperation) {
      result.errors.emplace_back(
          "HIR function unsafe contract differs from its semantic declaration");
    }
  }
  for (const auto &instance : program.classInstances()) {
    const auto *lifecycle = instance.source != nullptr
                                ? semantics.findClassLifecycle(*instance.source)
                                : nullptr;
    const bool requiresSchedule =
        lifecycle != nullptr && lifecycle->generatedDefaultInitializers;
    if (instance.generatedDefaultRequiresSchedule != requiresSchedule ||
        (requiresSchedule &&
         instance.defaultConstructor != SpecialMemberStatus::Generated))
      result.errors.emplace_back(
          "HIR generated-default obligation differs from semantics");
  }
  for (const auto &instance : program.constructorInstances()) {
    const auto *declaration = instance.source != nullptr
                                  ? semantics.findConstructor(*instance.source)
                                  : nullptr;
    if (instance.requiresUnsafe !=
        (declaration != nullptr && declaration->requiresUnsafe))
      result.errors.emplace_back(
          "constructor unsafe contract differs from semantics");
    if ((instance.declaration == 0) != instance.generatedDefault ||
        (instance.source == nullptr) != instance.generatedDefault)
      result.errors.emplace_back(
          "constructor source identity disagrees with its generated status");
    if (!instance.generatedDefault)
      continue;
    const auto *owner =
        instance.owner != 0 && instance.owner <= program.classInstances().size()
            ? &program.classInstances()[instance.owner - 1]
            : nullptr;
    const auto *lifecycle = owner != nullptr && owner->source != nullptr
                                ? semantics.findClassLifecycle(*owner->source)
                                : nullptr;
    if (lifecycle != nullptr && lifecycle->generatedDefaultInitializers) {
      bool exact =
          instance.parameterTypes.empty() &&
          instance.parameterBindings.empty() &&
          instance.typeArguments.empty() && instance.valueArguments.empty() &&
          instance.body.roots.empty() &&
          lifecycle->defaultConstructor == SpecialMemberStatus::Generated;
      std::size_t position = 0;
      const auto base = std::find_if(owner->bases.begin(), owner->bases.end(),
                                     [](const HirBaseInstance &candidate) {
                                       return !candidate.interface;
                                     });
      if (base != owner->bases.end()) {
        const auto *baseOwner =
            base->instance != 0 &&
                    base->instance <= program.classInstances().size()
                ? &program.classInstances()[base->instance - 1]
                : nullptr;
        const auto *baseLifecycle =
            baseOwner != nullptr && baseOwner->source != nullptr
                ? semantics.findClassLifecycle(*baseOwner->source)
                : nullptr;
        const auto *initializer = instance.initializers.empty()
                                      ? nullptr
                                      : &instance.initializers.front();
        const auto *target =
            initializer != nullptr && initializer->constructorTarget
                ? program.findConstructorInstance(
                      *initializer->constructorTarget)
                : nullptr;
        const auto *selected = target != nullptr && target->source != nullptr
                                   ? semantics.findConstructor(*target->source)
                                   : nullptr;
        exact &= initializer != nullptr &&
                 initializer->kind == ConstructorInitializerTargetKind::Base &&
                 initializer->source == nullptr &&
                 initializer->targetType == base->type &&
                 initializer->base == base->instance &&
                 initializer->field == 0 &&
                 initializer->explicitArgumentCount == 0 &&
                 !initializer->storesReference &&
                 !initializer->ownedParameter && baseLifecycle != nullptr &&
                 initializer->generatedDefault ==
                     (baseLifecycle->defaultConstructor ==
                      SpecialMemberStatus::Generated);
        if (initializer != nullptr && target != nullptr)
          exact &=
              target->owner == base->instance &&
              initializer->generatedDefault == target->generatedDefault &&
              (target->generatedDefault ||
               (selected != nullptr && selected->requiredParameterCount == 0));
        else if (initializer != nullptr)
          exact &= initializer->generatedDefault &&
                   initializer->arguments.empty() && base->instance != 0 &&
                   base->instance <= program.classInstances().size() &&
                   !program.classInstances()[base->instance - 1]
                        .generatedDefaultRequiresSchedule;
        ++position;
      }
      std::vector<HirValueId> selectedValues;
      for (const auto &initializer : instance.initializers)
        selectedValues.insert(selectedValues.end(),
                              initializer.arguments.begin(),
                              initializer.arguments.end());
      exact &= selectedValues == instance.initializerValues &&
               instance.initializers.size() == position + owner->fields.size();
      const auto reachable =
          hirInitializerReachability(instance.body, selectedValues);
      exact &= reachable.complete &&
               exactInventory(instance.body.statements, reachable.statements) &&
               exactInventory(instance.body.values, reachable.values) &&
               exactInventory(instance.body.bindings, reachable.bindings);
      for (const auto &field : owner->fields) {
        const Expr *selectedSource =
            field.declaration == nullptr
                ? nullptr
                : (field.declaration->initializer()
                       ? field.declaration->initializer().get()
                       : semantics.implicitFieldInitializer(*field.declaration)
                             .get());
        const auto *initializer = position < instance.initializers.size()
                                      ? &instance.initializers[position]
                                      : nullptr;
        const auto *value =
            initializer != nullptr && initializer->arguments.size() == 1
                ? instance.body.findValue(initializer->arguments.front())
                : nullptr;
        exact &=
            field.declaration != nullptr && selectedSource != nullptr &&
            initializer != nullptr &&
            initializer->kind == ConstructorInitializerTargetKind::Field &&
            initializer->source == nullptr &&
            initializer->field == field.info.symbol &&
            initializer->targetType == field.info.type &&
            initializer->explicitArgumentCount == 1 && !initializer->base &&
            !initializer->constructorTarget && !initializer->generatedDefault &&
            !initializer->storesReference && !initializer->ownedParameter &&
            value != nullptr &&
            instance.body.initializerValueForSource(*selectedSource) == value &&
            (value->info.type == field.info.type ||
             isImplicitScalarInitializationConversion(field.info.type,
                                                      value->info.type));
        ++position;
      }
      if (!exact)
        result.errors.emplace_back(
            "generated constructor differs from its selected declaration "
            "initializers");
      continue;
    }
    result.errors.emplace_back(
        "generated constructor has no selected semantic initializer schedule");
  }
  const auto verifyDirectInitializations = [&](const HirBody &body) {
    std::unordered_set<const Expr *> seen;
    for (const auto &alias : body.directInitializations) {
      const auto *value = body.findValue(alias.value);
      const Expr *input = nullptr;
      const Binary *comma = alias.source == nullptr
                                ? nullptr
                                : semantics.builtinComma(*alias.source);
      if (const auto *group = exprAs<Grouping>(alias.source))
        input = group->expression().get();
      else if (comma != nullptr)
        input = comma->right().get();
      else if (const auto *call = exprAs<Call>(alias.source);
               call != nullptr && call->arguments().size() == 1)
        input = call->arguments().front().get();
      else if (const auto *direct = exprAs<DirectInitializer>(alias.source);
               direct != nullptr && direct->arguments().size() == 1)
        input = direct->arguments().front().get();
      const Expr *producer = alias.producer;
      for (std::size_t depth = 0; depth < body.directInitializations.size();
           ++depth) {
        const auto nested = std::find_if(
            body.directInitializations.begin(),
            body.directInitializations.end(),
            [&](const auto &entry) { return entry.source == producer; });
        if (nested == body.directInitializations.end())
          break;
        producer = nested->producer;
      }
      const auto *ids = alias.source == nullptr
                            ? nullptr
                            : &program.valueIdsForSource(*alias.source);
      const auto *selected = alias.source == nullptr
                                 ? nullptr
                                 : semantics.findConstruction(*alias.source);
      const bool grouping =
          exprAs<Grouping>(alias.source) != nullptr || comma != nullptr;
      // A comma alias's producer sequences exactly the comma's left operand
      // before it begins.
      const HirValue *discarded = comma == nullptr || comma->left() == nullptr
                                      ? nullptr
                                      : body.valueForSource(*comma->left());
      const bool sequencedLeft =
          comma == nullptr ||
          (value != nullptr && discarded != nullptr &&
           std::count(value->sequencedInputs.begin(),
                      value->sequencedInputs.end(), discarded->id) == 1);
      const auto &proof = alias.constructionProof;
      const bool exactConstruction =
          grouping ? !proof
                   : proof && proof->source() == alias.source &&
                         proof->producer() == alias.producer &&
                         proof->type() == alias.type &&
                         proof->snapshot() ==
                             semantics.analysisSeal().programSnapshot;
      const auto *sourceCall = exprAs<Call>(alias.source);
      const auto *call =
          sourceCall == nullptr ? nullptr : semantics.findCall(*sourceCall);
      const bool ordinaryCall =
          sourceCall != nullptr &&
          ((call != nullptr &&
            call->intrinsic != IntrinsicKind::ExactTypeParameterConstruction &&
            call->intrinsic !=
                IntrinsicKind::DefaultTypeParameterConstruction) ||
           semantics.findLambdaCall(*sourceCall) != nullptr ||
           semantics.findDeferredCallableCall(*sourceCall) != nullptr ||
           semantics.findOperator(*sourceCall) != nullptr);
      const bool executableSource = std::any_of(
          body.values.begin(), body.values.end(), [&](const auto &candidate) {
            return candidate.source == alias.source;
          });
      // A primary generic record may precede concrete reanalysis. When its
      // exact construction is retained, its direct-initialization decision
      // must agree; the concrete value/type and source identity are mandatory.
      const bool semanticMatch =
          !ordinaryCall && !executableSource && exactConstruction &&
          (selected == nullptr || selected->directInitialization || grouping);
      const bool fresh =
          value != nullptr && (value->kind == HirValueKind::Call ||
                               value->kind == HirValueKind::DirectInitializer ||
                               value->functionTarget.has_value());
      if (alias.source == nullptr || !seen.insert(alias.source).second ||
          input == nullptr || input != alias.producer || value == nullptr ||
          value->source != producer || alias.type.kind != SemanticType::Class ||
          !sequencedLeft || value->info.type != alias.type ||
          value->info.category != ValueCategory::Value ||
          (!grouping && (!fresh || value->intrinsic == IntrinsicKind::Move)) ||
          !semanticMatch || ids == nullptr ||
          std::find(ids->begin(), ids->end(), alias.value) == ids->end())
        result.errors.emplace_back(
            "object result alias lost its exact source/type, sequencing "
            "and shared value identity");
    }
  };
  const auto verifyGeneratedConstructionValues = [&](const HirBody &body) {
    verifyDirectInitializations(body);
    for (const auto &value : body.values) {
      if (value.intrinsic == IntrinsicKind::SourceExpressionText)
        result.errors.emplace_back(
            "written expression metadata was not normalized to a HIR literal");
      if (!value.defaultArguments.empty()) {
        const std::vector<Parameter> *parameters = nullptr;
        if (const auto *target =
                value.constructorTarget
                    ? program.findConstructorInstance(*value.constructorTarget)
                    : nullptr;
            target && target->source)
          parameters = &target->source->parameters();
        else if (const auto *target =
                     value.functionTarget
                         ? program.findFunctionInstance(*value.functionTarget)
                         : nullptr;
                 target) {
          // The canonical declaration owns defaults; source may instead be
          // an out-of-line executable definition that omits those defaults.
          const auto *declaration = semantics.findFunction(target->declaration);
          if (declaration && declaration->declaration)
            parameters = &declaration->declaration->parameters();
        }
        bool exact = parameters &&
                     value.defaultArguments.size() <= parameters->size() &&
                     value.defaultArguments.size() <= value.operands.size();
        if (exact) {
          const auto count = value.defaultArguments.size();
          exact = std::equal(value.defaultArguments.begin(),
                             value.defaultArguments.end(),
                             value.operands.end() - count);
          for (std::size_t index = 0; exact && index < count; ++index) {
            const auto &parameter =
                (*parameters)[parameters->size() - count + index];
            const auto *input = body.findValue(value.defaultArguments[index]);
            const auto *expression =
                parameter.defaultArgument
                    ? parameter.defaultArgument->expression.get()
                    : nullptr;
            exact = input && expression && input->source == expression &&
                    input->kind != HirValueKind::PackExpansion;
          }
        }
        if (!exact)
          result.errors.emplace_back(
              "caller-expanded defaults lost their exact declaration suffix");
      }
      for (std::size_t index = 0; index < value.operands.size(); ++index)
        if (const auto *input = body.findValue(value.operands[index]);
            input && input->kind == HirValueKind::PackExpansion &&
            index + 1 < value.operands.size() &&
            (value.defaultArguments.size() !=
                 value.operands.size() - index - 1 ||
             !std::equal(value.defaultArguments.begin(),
                         value.defaultArguments.end(),
                         value.operands.begin() + index + 1)))
          result.errors.emplace_back(
              "non-final argument pack lost its exact default suffix");
      if ((value.kind != HirValueKind::Call &&
           value.kind != HirValueKind::DirectInitializer) ||
          contextualConversionCall(body, value, semantics))
        continue;
      const auto *construction = value.source != nullptr
                                     ? semantics.findConstruction(*value.source)
                                     : nullptr;
      const auto *type =
          construction != nullptr && construction->generatedDefault
              ? semantics.findClassType(construction->constructedType.classId)
              : nullptr;
      const auto *lifecycle =
          type != nullptr && type->declaration != nullptr
              ? semantics.findClassLifecycle(*type->declaration)
              : nullptr;
      if (lifecycle == nullptr || !lifecycle->generatedDefaultInitializers)
        continue;
      const auto *target =
          value.constructorTarget
              ? program.findConstructorInstance(*value.constructorTarget)
              : nullptr;
      const auto *owner =
          target != nullptr && target->owner != 0 &&
                  target->owner <= program.classInstances().size()
              ? &program.classInstances()[target->owner - 1]
              : nullptr;
      const bool inPlace =
          value.intrinsic == IntrinsicKind::ObjectConstruct ||
          value.intrinsic == IntrinsicKind::OptionalStorageConstruct;
      bool exactResult = owner != nullptr && value.info.type == owner->type &&
                         value.intrinsic == IntrinsicKind::None;
      if (inPlace && owner != nullptr && !value.parameterTypes.empty()) {
        const auto &storage = value.parameterTypes.front();
        const auto *storageOwner =
            storage.kind == SemanticType::Class
                ? semantics.findClassType(storage.classId)
                : nullptr;
        exactResult =
            value.info.type == SemanticType::Void &&
            storage.arguments.size() == 1 &&
            storage.arguments.front() == owner->type &&
            (value.intrinsic == IntrinsicKind::ObjectConstruct
                 ? storage.kind == SemanticType::RawPointer
                 : storageOwner != nullptr &&
                       storageOwner->compilerCapability ==
                           CompilerCapabilityTypeKind::OptionalStorage);
      }
      const auto propagation =
          value.intrinsic == IntrinsicKind::OptionalStorageConstruct
              ? FailurePropagationKind::None
              : FailurePropagationKind::Constructor;
      // Concrete reanalysis owns generic argument substitution. The base
      // semantic record may still be the primary's C<T>; retain its exact
      // declaration identity and validate the concrete value/storage-to-owner
      // relationship instead of comparing C<T> with C<int>.
      const bool exactOwner =
          owner != nullptr &&
          (containsUnresolvedGeneric(construction->constructedType)
               ? owner->type.kind == SemanticType::Class &&
                     owner->type.classId ==
                         construction->constructedType.classId &&
                     !containsUnresolvedGeneric(owner->type)
               : owner->type == construction->constructedType);
      if (target == nullptr || !target->generatedDefault || owner == nullptr ||
          !exactOwner || !exactResult ||
          value.definedFailure.propagation != propagation)
        result.errors.emplace_back("generated construction value lost its "
                                   "selected constructor and failure edge");
    }
  };
  verifyGeneratedConstructionValues(program.module());
  for (const auto &storage : program.threadLocalStorage())
    verifyGeneratedConstructionValues(storage.initializer);
  for (const auto &owner : program.classInstances()) {
    verifyGeneratedConstructionValues(owner.fieldInitializers);
    verifyGeneratedConstructionValues(owner.staticFieldInitializers);
  }
  for (const auto &function : program.functionInstances())
    verifyGeneratedConstructionValues(function.body);
  for (const auto &constructor : program.constructorInstances())
    verifyGeneratedConstructionValues(constructor.body);
  for (const auto &destructor : program.destructorInstances())
    verifyGeneratedConstructionValues(destructor.body);
  for (const auto &lambda : program.lambdaInstances())
    verifyGeneratedConstructionValues(lambda.body);
  for (std::size_t index = 0; index < program.nativeCallbackAdapters().size();
       ++index) {
    const HirNativeCallbackAdapter &adapter =
        program.nativeCallbackAdapters()[index];
    const HirFunctionInstance *target =
        program.findFunctionInstance(adapter.target);
    const FunctionInfo *declaration =
        target == nullptr ? nullptr
                          : semantics.findFunction(target->declaration);
    const SemanticType *returnType = adapter.type.nativeFunctionReturnType();
    const std::span<const SemanticType> parameterTypes =
        adapter.type.nativeFunctionParameterTypes();
    if (adapter.id != index + 1 || target == nullptr ||
        target->owner.has_value() || target->source == nullptr ||
        target->requiresUnsafe || target->linkage != LanguageLinkage::Gti ||
        declaration == nullptr || declaration->ownerClass != 0 ||
        returnType == nullptr || target->returnType != *returnType ||
        target->parameterTypes.size() != parameterTypes.size() ||
        !std::equal(target->parameterTypes.begin(),
                    target->parameterTypes.end(), parameterTypes.begin()) ||
        adapter.cThreadCallback !=
            (program.executionProfile() == ExecutionProfile::Concurrent
                 ? (target->threadConfinedCallback
                        ? CThreadCallbackKind::ThreadConfined
                        : adapter.type.cThreadCallbackKind())
                 : CThreadCallbackKind::None) ||
        (program.executionProfile() == ExecutionProfile::Concurrent &&
         adapter.cThreadCallback == CThreadCallbackKind::None)) {
      result.errors.emplace_back(
          "native callback adapter has no exact free-function target");
    }
    if (target && declaration &&
        target->threadConfinedCallback != declaration->threadConfinedCallback) {
      result.errors.emplace_back(
          "native callback target differs from its semantic entry contract");
    }
  }
  const auto verifyNativeCallbackValues = [&](const HirBody &body) {
    for (const HirValue &value : body.values) {
      if (value.kind != HirValueKind::NativeCallback) {
        if (value.nativeCallbackAdapter) {
          result.errors.emplace_back(
              "non-callback HIR value carries callback adapter metadata");
        }
        continue;
      }
      const HirNativeCallbackAdapter *adapter =
          value.nativeCallbackAdapter
              ? program.findNativeCallbackAdapter(*value.nativeCallbackAdapter)
              : nullptr;
      const NativeFunctionConversionInfo *conversion =
          value.source == nullptr
              ? nullptr
              : semantics.findNativeFunctionConversion(*value.source);
      const HirFunctionInstance *target =
          adapter == nullptr ? nullptr
                             : program.findFunctionInstance(adapter->target);
      if (adapter == nullptr || conversion == nullptr || target == nullptr ||
          !value.operands.empty() || value.info.type != adapter->type ||
          conversion->type != adapter->type ||
          conversion->cThreadCallback != adapter->cThreadCallback ||
          conversion->function != target->declaration ||
          conversion->declaration != target->source) {
        result.errors.emplace_back(
            "native callback HIR value differs from semantic conversion");
      }
    }
  };
  verifyNativeCallbackValues(program.module());
  for (const auto &storage : program.threadLocalStorage())
    verifyNativeCallbackValues(storage.initializer);
  for (const HirClassInstance &instance : program.classInstances()) {
    verifyNativeCallbackValues(instance.fieldInitializers);
    verifyNativeCallbackValues(instance.staticFieldInitializers);
  }
  // A contextual construction is the declared value its initializer's own
  // value is consumed by: exactly the semantic selection, with no result
  // borrow, the initializer first and the selected defaults after it.
  const auto verifyContextualConstructions = [&](const HirBody &body) {
    std::unordered_set<HirValueId> listed;
    std::unordered_set<const ReturnStmt *> emptyReturns;
    for (const auto &entry : body.emptyReturnConstructions) {
      const auto &proof = entry.constructionProof;
      const auto *value = body.findValue(entry.value);
      const auto *target =
          value != nullptr && value->constructorTarget
              ? program.findConstructorInstance(*value->constructorTarget)
              : nullptr;
      const auto *owner =
          target != nullptr && target->owner != 0 &&
                  target->owner <= program.classInstances().size()
              ? &program.classInstances()[target->owner - 1]
              : nullptr;
      const auto *construction = proof ? &proof->construction() : nullptr;
      const auto statement = std::find_if(
          body.statements.begin(), body.statements.end(),
          [&](const auto &item) { return item.source == entry.source; });
      const auto fullExpression =
          value == nullptr
              ? body.fullExpressions.end()
              : std::find_if(body.fullExpressions.begin(),
                             body.fullExpressions.end(), [&](const auto &item) {
                               return item.id == value->fullExpression;
                             });
      bool valid =
          entry.source != nullptr && !entry.source->value() && proof &&
          proof->source() == entry.source &&
          proof->snapshot() == semantics.analysisSeal().programSnapshot &&
          emptyReturns.insert(entry.source).second &&
          listed.insert(entry.value).second && value != nullptr &&
          target != nullptr && owner != nullptr && construction != nullptr &&
          statement != body.statements.end() &&
          statement->kind == HirStatementKind::Return &&
          statement->value == entry.value &&
          fullExpression != body.fullExpressions.end() &&
          fullExpression->statement == statement->id &&
          fullExpression->roots == std::vector<HirValueId>{entry.value} &&
          value->kind == HirValueKind::DirectInitializer && !value->source &&
          construction->constructor != 0 && !construction->generatedDefault &&
          construction->kind == ConstructorKind::Ordinary &&
          construction->borrowOrigin == BorrowOriginKind::None &&
          construction->borrowFields.empty() &&
          target->declaration == construction->constructor &&
          target->typeArguments == construction->typeArguments &&
          target->valueArguments == construction->valueArguments &&
          target->requiresUnsafe == proof->unsafeConstruction() &&
          owner->type == construction->constructedType &&
          value->info == ExpressionInfo{.type = owner->type,
                                        .category = ValueCategory::Value,
                                        .access = AccessMode::ReadOnly,
                                        .traits = owner->traits} &&
          value->parameterTypes == construction->parameterTypes &&
          value->operands.size() == value->parameterTypes.size() &&
          value->operands.size() == construction->defaultArguments.size() &&
          value->defaultArguments == value->operands &&
          value->constructorKind == ConstructorKind::Ordinary &&
          value->definedFailure.propagation ==
              FailurePropagationKind::Constructor &&
          value->unsafeOperation ==
              (proof->unsafeConstruction()
                   ? UnsafeOperationKind::DeclaredUnsafeConstruction
                   : UnsafeOperationKind::None) &&
          value->borrowOrigin == BorrowOriginKind::None &&
          value->borrowFields.empty() && value->borrowArguments.empty() &&
          !value->borrowPlace && !value->borrowDependencies &&
          value->callPlan && !value->callPlan->receiver &&
          value->callPlan->arguments.size() == value->operands.size();
      if (valid) {
        for (std::size_t index = 0; index < value->operands.size(); ++index) {
          const auto *argument = body.findValue(value->operands[index]);
          const auto &input = value->callPlan->arguments[index];
          valid = valid && argument != nullptr &&
                  argument->source == construction->defaultArguments[index] &&
                  input.value == argument->id &&
                  input.parameterIndex == index &&
                  input.parameterType == construction->parameterTypes[index] &&
                  input.defaultArgument;
        }
      }
      if (!valid)
        result.errors.emplace_back("empty return construction HIR differs from "
                                   "its source-bound semantic proof");
    }
    for (const auto &statement : body.statements) {
      const auto *source = stmtAs<ReturnStmt>(statement.source);
      if (source != nullptr && !source->value() && statement.value &&
          std::none_of(
              body.emptyReturnConstructions.begin(),
              body.emptyReturnConstructions.end(), [&](const auto &entry) {
                return entry.source == source && entry.value == statement.value;
              }))
        result.errors.emplace_back(
            "bare return HIR value has no source-bound construction proof");
    }
    for (const HirContextualConstruction &entry :
         body.contextualConstructions) {
      const HirValue *value = body.findValue(entry.value);
      const HirValue *argument = value == nullptr || value->operands.empty()
                                     ? nullptr
                                     : body.findValue(value->operands.front());
      const auto &proof = entry.constructionProof;
      const ResolvedConstructionInfo *construction =
          proof && proof->source() == entry.source &&
                  proof->snapshot() == semantics.analysisSeal().programSnapshot
              ? &proof->construction()
              : nullptr;
      const HirConstructorInstance *target =
          value != nullptr && value->constructorTarget
              ? program.findConstructorInstance(*value->constructorTarget)
              : nullptr;
      const HirClassInstance *owner =
          target == nullptr || target->owner == 0 ||
                  target->owner > program.classInstances().size()
              ? nullptr
              : &program.classInstances()[target->owner - 1];
      auto expectedDependencies =
          construction ? construction->contextualBorrowDependencies
                       : std::nullopt;
      if (expectedDependencies)
        for (auto &dependency : expectedDependencies->dependencies)
          dependency.place.domain = body.placeDomain;
      // A built-in owning handle argument keeps its unstaged transport, as
      // the same constructor selected by explicit syntax does.
      const bool unstagedOwnerHandle =
          value != nullptr && !value->callPlan &&
          std::any_of(value->parameterTypes.begin(),
                      value->parameterTypes.end(),
                      [](const SemanticType &parameter) {
                        return parameter.kind == SemanticType::UniqueOwner ||
                               parameter.kind == SemanticType::SharedPointer;
                      });
      // A conversion publishing its argument's owner is a dependent value
      // (L-OWN-02): borrowed state, neither reassignable nor transferable.
      SemanticTypeTraits expectedTraits =
          owner != nullptr ? owner->traits : SemanticTypeTraits{};
      if (construction != nullptr && construction->dependentTextPublication) {
        expectedTraits.copyAssignable = false;
        expectedTraits.moveAssignable = false;
        expectedTraits.containsBorrowedState = true;
        expectedTraits.transferCapable = false;
        expectedTraits.shareCapable = false;
      }
      if (value == nullptr || construction == nullptr || target == nullptr ||
          owner == nullptr || !listed.insert(entry.value).second ||
          value->kind != HirValueKind::DirectInitializer ||
          value->source != nullptr || argument == nullptr ||
          // An initializer with an expression alias (a reflected expression)
          // lowers as the value of the expression it denotes.
          (argument->source != entry.source &&
           std::none_of(body.sourceAliases.begin(), body.sourceAliases.end(),
                        [&](const auto &alias) {
                          return alias.source == entry.source &&
                                 alias.value == argument->id;
                        })) ||
          body.valueForSource(*entry.source) != argument ||
          construction->constructor == 0 || construction->generatedDefault ||
          construction->kind != ConstructorKind::Ordinary ||
          (owner->traits.containsBorrowedState && !expectedDependencies) ||
          target->declaration != construction->constructor ||
          owner->type != construction->constructedType ||
          value->info != ExpressionInfo{.type = owner->type,
                                        .category = ValueCategory::Value,
                                        .access = AccessMode::ReadOnly,
                                        .traits = expectedTraits} ||
          value->dependentTextPublication !=
              construction->dependentTextPublication ||
          value->parameterTypes != construction->parameterTypes ||
          value->operands.size() != value->parameterTypes.size() ||
          value->operands.size() != 1 + construction->defaultArguments.size() ||
          value->defaultArguments.size() !=
              construction->defaultArguments.size() ||
          !std::equal(value->defaultArguments.begin(),
                      value->defaultArguments.end(),
                      value->operands.begin() + 1) ||
          value->constructorKind != ConstructorKind::Ordinary ||
          value->definedFailure.propagation !=
              FailurePropagationKind::Constructor ||
          value->borrowOrigin != construction->borrowOrigin ||
          value->borrowArgument != construction->borrowArgument ||
          value->borrowAccess != construction->borrowAccess ||
          value->borrowArguments != construction->borrowArguments ||
          value->borrowFields != construction->borrowFields ||
          value->borrowDependencies != expectedDependencies ||
          (!unstagedOwnerHandle &&
           (!value->callPlan || value->callPlan->receiver ||
            value->callPlan->arguments.size() != value->operands.size()))) {
        result.errors.emplace_back(
            "contextual construction HIR value differs from its semantic "
            "construction");
      }
    }
  };
  verifyContextualConstructions(program.module());
  for (const auto &storage : program.threadLocalStorage())
    verifyContextualConstructions(storage.initializer);
  for (const HirClassInstance &instance : program.classInstances()) {
    verifyContextualConstructions(instance.fieldInitializers);
    verifyContextualConstructions(instance.staticFieldInitializers);
  }
  for (const HirFunctionInstance &instance : program.functionInstances()) {
    verifyContextualConstructions(instance.body);
  }
  for (const HirConstructorInstance &instance :
       program.constructorInstances()) {
    verifyContextualConstructions(instance.body);
  }
  for (const HirDestructorInstance &instance : program.destructorInstances()) {
    verifyContextualConstructions(instance.body);
  }
  for (const HirFunctionInstance &instance : program.functionInstances()) {
    verifyNativeCallbackValues(instance.body);
  }
  for (const HirConstructorInstance &instance :
       program.constructorInstances()) {
    verifyNativeCallbackValues(instance.body);
  }
  for (const HirDestructorInstance &instance : program.destructorInstances()) {
    verifyNativeCallbackValues(instance.body);
  }
  for (const HirLambda &instance : program.lambdaInstances()) {
    verifyNativeCallbackValues(instance.body);
  }

  // Propagation through `std::tryable` keeps the semantic selection: each
  // step calls the selected block function on the one operand, in the same
  // full expression, and the residual return constructs through the selected
  // implicit constructor or is the residual itself. A generic instance's plan
  // lives in that instance's own analysis, so the program model supplies the
  // selected identities only for a non-generic body; the shape is checked for
  // both.
  const auto verifyPropagation = [&](const HirBody &body,
                                     const HirValue &value) {
    const auto *unary =
        value.source == nullptr ? nullptr : exprAs<Unary>(value.source);
    const ResolvedTryablePropagationInfo *selected =
        unary == nullptr ? nullptr : semantics.findTryablePropagation(*unary);
    if (value.kind != HirValueKind::Propagation || !value.propagation ||
        unary == nullptr || unary->oper().kind != TokenKind::QUESTION) {
      result.errors.emplace_back(
          "tryable propagation lost its selected semantic plan");
      return;
    }
    const HirPropagationPlan &plan = *value.propagation;
    const HirValue *operand = body.findValue(plan.operand);
    const HirValue *test = body.findValue(plan.test);
    const HirValue *output = body.findValue(plan.output);
    const HirValue *residual = body.findValue(plan.residual);
    const HirValue *residualReturn = plan.residualReturn == 0
                                         ? nullptr
                                         : body.findValue(plan.residualReturn);
    std::vector<HirValueId> operands{plan.operand, plan.test, plan.output,
                                     plan.residual};
    if (plan.residualReturn != 0) {
      operands.push_back(plan.residualReturn);
    }
    if (operand == nullptr || test == nullptr || output == nullptr ||
        residual == nullptr ||
        (plan.residualReturn != 0 && residualReturn == nullptr) ||
        value.operands != operands) {
      result.errors.emplace_back(
          "tryable propagation references missing protocol steps");
      return;
    }
    const bool movedTransport =
        plan.movedPlace != 0 && operand->kind == HirValueKind::Move &&
        operand->operands == std::vector<HirValueId>{plan.movedPlace};
    const auto movesPlace = [&](HirValueId id) {
      const HirValue *move = body.findValue(id);
      return move != nullptr && move->kind == HirValueKind::Move &&
             move->operands == std::vector<HirValueId>{plan.movedPlace} &&
             move->info.type == operand->info.type &&
             move->fullExpression == value.fullExpression;
    };
    const bool passiveEnumOperand =
        operand->info.type.kind == SemanticType::Enum &&
        std::any_of(program.enumDeclarations().begin(),
                    program.enumDeclarations().end(),
                    [&](const HirEnum &enumeration) {
                      return enumeration.type == operand->info.type &&
                             !enumeration.requiresActiveCleanup;
                    });
    const auto selectedCall = [&](const HirValue &call,
                                  const ResolvedCallInfo *step, bool consumes) {
      const HirFunctionInstance *target =
          call.functionTarget
              ? program.findFunctionInstance(*call.functionTarget)
              : nullptr;
      HirCallInputKind kind =
          !plan.consumesOperand
              ? (consumes ? HirCallInputKind::CopyValue
                          : HirCallInputKind::ReadBorrow)
              : (consumes ? HirCallInputKind::MoveValue
                          : HirCallInputKind::ReadTemporaryBorrow);
      // An enum without cleanup passes by value, as in any by-value call.
      if (consumes && passiveEnumOperand) {
        kind = HirCallInputKind::Value;
      }
      const auto movesTemporary = [&](HirValueId id) {
        const HirValue *move = body.findValue(id);
        return move != nullptr && move->kind == HirValueKind::Move &&
               move->operands == std::vector<HirValueId>{operand->id} &&
               move->info.type == operand->info.type &&
               move->fullExpression == value.fullExpression;
      };
      bool argument = call.operands.size() == 1 &&
                      (plan.consumesOperand && consumes
                           ? movesTemporary(call.operands.front())
                           : call.operands.front() == operand->id);
      if (movedTransport) {
        kind = !consumes            ? HirCallInputKind::ReadBorrow
               : passiveEnumOperand ? HirCallInputKind::Value
                                    : HirCallInputKind::MoveValue;
        argument = call.operands.size() == 1 &&
                   (consumes ? movesPlace(call.operands.front())
                             : call.operands.front() == plan.movedPlace);
      }
      if (consumes && operand->info.type.kind == SemanticType::Enum &&
          operand->info.traits.drop == DropKind::Trivial &&
          operand->info.traits.copyable &&
          !operand->info.traits.containsBorrowedState)
        kind = HirCallInputKind::Value;
      return call.kind == HirValueKind::Call && call.source == nullptr &&
             call.intrinsic == IntrinsicKind::None && target != nullptr &&
             (step == nullptr || target->declaration == step->function) &&
             argument && !call.receiver && call.callPlan &&
             !call.callPlan->receiver && call.callPlan->arguments.size() == 1 &&
             call.callPlan->arguments.front().value == call.operands.front() &&
             call.callPlan->arguments.front().kind == kind &&
             (step == nullptr ||
              (call.info.type ==
                   (step->returnType.kind == SemanticType::Reference
                        ? step->returnType.arguments.front()
                        : step->returnType) &&
               call.borrowOrigin == step->borrowOrigin &&
               call.borrowArgument == step->borrowArgument &&
               call.borrowAccess == step->borrowAccess &&
               call.borrowFields == step->borrowFields)) &&
             call.fullExpression == value.fullExpression;
    };
    const HirConstructorInstance *construction =
        residualReturn != nullptr && residualReturn->constructorTarget
            ? program.findConstructorInstance(
                  *residualReturn->constructorTarget)
            : nullptr;
    const bool constructsReturn = selected != nullptr
                                      ? selected->returnConstruction.has_value()
                                      : plan.residualReturn != 0;
    const bool returnsResidual =
        constructsReturn
            ? residualReturn != nullptr &&
                  residualReturn->kind == HirValueKind::DirectInitializer &&
                  residualReturn->source == nullptr &&
                  construction != nullptr &&
                  (selected == nullptr ||
                   construction->declaration ==
                       selected->returnConstruction->constructor) &&
                  residualReturn->operands ==
                      std::vector<HirValueId>{residual->id} &&
                  residualReturn->fullExpression == value.fullExpression &&
                  residualReturn->callPlan &&
                  !residualReturn->callPlan->receiver &&
                  residualReturn->callPlan->arguments.size() == 1
            : plan.residualReturn == 0;
    if ((selected != nullptr &&
         (plan.consumesOperand != selected->consumesOperand ||
          operand->info.type != selected->operandType ||
          value.info.type != selected->resultType ||
          residual->info.type != selected->residualType)) ||
        plan.consumesOperand !=
            (operand->info.category == ValueCategory::Value) ||
        (plan.movedPlace != 0 && !movedTransport) ||
        value.info.category != output->info.category ||
        !selectedCall(*test, selected ? &selected->test : nullptr, false) ||
        test->info.type != SemanticType::Bool ||
        !selectedCall(*output, selected ? &selected->output : nullptr, true) ||
        output->info.type != value.info.type ||
        !selectedCall(*residual, selected ? &selected->residual : nullptr,
                      true) ||
        !returnsResidual) {
      result.errors.emplace_back(
          "tryable propagation lost its selected block calls, operand "
          "transport, or residual return");
    }
  };

  const auto verifyCallableValues = [&](const HirBody &body) {
    for (const HirValue &value : body.values) {
      if (value.kind == HirValueKind::Propagation || value.propagation) {
        verifyPropagation(body, value);
      }
      const HirFunctionInstance *contractTarget =
          value.kind == HirValueKind::Call && value.functionTarget
              ? program.findFunctionInstance(*value.functionTarget)
              : nullptr;
      const bool expectedTerminal =
          (value.kind == HirValueKind::Call &&
           intrinsicTerminalFailureOutcome(value.intrinsic)) ||
          (contractTarget != nullptr && contractTarget->neverReturns) ||
          value.noNormalBorrowResultProof.has_value();
      if (const auto &proof = value.noNormalBorrowResultProof; proof) {
        const auto &selected = proof->call();
        if (value.kind != HirValueKind::Call ||
            value.intrinsic != IntrinsicKind::None || !contractTarget ||
            contractTarget->neverReturns ||
            value.dispatch != CallDispatch::Static ||
            proof->source() != value.source ||
            proof->snapshot() != semantics.analysisSeal().programSnapshot ||
            selected.function != contractTarget->declaration ||
            selected.returnType.kind != SemanticType::Reference ||
            selected.returnType != contractTarget->returnType ||
            selected.parameterTypes != value.parameterTypes ||
            selected.parameterTypes != contractTarget->parameterTypes ||
            selected.typeArguments != contractTarget->typeArguments ||
            selected.valueArguments != contractTarget->valueArguments ||
            selected.dispatchOwner != value.dispatchOwner ||
            !selected.noNormalBorrowResult || !selected.neverReturns ||
            value.borrowDependencies) {
          result.errors.emplace_back("HIR impossible normal result lost its "
                                     "exact semantic call proof");
        }
      }
      const bool expectedTracking =
          (value.kind == HirValueKind::Call &&
           intrinsicTracksCaller(value.intrinsic)) ||
          (contractTarget != nullptr && contractTarget->tracksCaller);
      const auto &callerProof = value.callerLocationProof;
      if (value.neverReturns != expectedTerminal ||
          value.tracksCaller != expectedTracking ||
          value.callerLocation.has_value() != expectedTracking ||
          callerProof.has_value() != expectedTracking ||
          (value.callerLocation &&
           (!callerProof || callerProof->source() != value.source ||
            callerProof->snapshot() !=
                semantics.analysisSeal().programSnapshot ||
            callerProof->origin() != *value.callerLocation))) {
        result.errors.emplace_back(
            "HIR call terminal/caller contract differs from its selected "
            "declaration or semantic source origin");
      }
      std::unordered_set<HirValueId> sequenced;
      for (const HirValueId inputId : value.sequencedInputs) {
        const HirValue *input = body.findValue(inputId);
        if (input == nullptr || input->neverReturns ||
            input->fullExpression != value.fullExpression ||
            value.programConstantSubstitution ||
            !sequenced.insert(inputId).second) {
          result.errors.emplace_back(
              "sequenced HIR inputs must be distinct normally completing "
              "evaluations in the same full expression");
        }
        std::unordered_set<HirValueId> visited;
        std::vector<HirValueId> pending{inputId};
        while (!pending.empty()) {
          const HirValueId id = pending.back();
          pending.pop_back();
          if (id == value.id) {
            result.errors.emplace_back("sequenced HIR inputs contain a cycle");
            break;
          }
          if (!visited.insert(id).second) {
            continue;
          }
          const HirValue *dependency = body.findValue(id);
          if (dependency == nullptr) {
            result.errors.emplace_back(
                "sequenced HIR input references a missing value");
            continue;
          }
          pending.insert(pending.end(), dependency->operands.begin(),
                         dependency->operands.end());
          pending.insert(pending.end(), dependency->sequencedInputs.begin(),
                         dependency->sequencedInputs.end());
          if (dependency->receiver) {
            pending.push_back(*dependency->receiver);
          }
        }
      }
      if (value.kind != HirValueKind::FunctionItem) {
        continue;
      }
      const FunctionItemSelectionInfo *selection =
          value.source == nullptr
              ? nullptr
              : semantics.findFunctionItemSelection(*value.source);
      const HirFunctionInstance *target =
          value.functionTarget
              ? program.findFunctionInstance(*value.functionTarget)
              : nullptr;
      const FunctionInfo *declaration =
          target == nullptr ? nullptr
                            : semantics.findFunction(target->declaration);
      const SemanticType *returnType = value.info.type.functionItemReturnType();
      const std::span<const SemanticType> parameters =
          value.info.type.functionItemParameterTypes();
      if (selection == nullptr || target == nullptr || declaration == nullptr ||
          !value.operands.empty() ||
          value.info.category != ValueCategory::Value ||
          value.info.type != selection->type ||
          selection->capability != CallableInvocationCapability::Read ||
          value.info.type.functionId != target->declaration ||
          selection->function != target->declaration ||
          selection->declaration != target->source || target->owner ||
          target->linkage != LanguageLinkage::Gti || target->requiresUnsafe ||
          returnType == nullptr || *returnType != target->returnType ||
          parameters.size() != target->parameterTypes.size() ||
          !std::equal(parameters.begin(), parameters.end(),
                      target->parameterTypes.begin())) {
        result.errors.emplace_back(
            "function-item HIR value has no exact free-function identity");
      }
    }
  };
  verifyCallableValues(program.module());
  for (const auto &storage : program.threadLocalStorage())
    verifyCallableValues(storage.initializer);
  for (const HirClassInstance &instance : program.classInstances()) {
    verifyCallableValues(instance.fieldInitializers);
    verifyCallableValues(instance.staticFieldInitializers);
  }
  for (const HirFunctionInstance &instance : program.functionInstances()) {
    verifyCallableValues(instance.body);
  }
  for (const HirConstructorInstance &instance :
       program.constructorInstances()) {
    verifyCallableValues(instance.body);
  }
  for (const HirDestructorInstance &instance : program.destructorInstances()) {
    verifyCallableValues(instance.body);
  }
  for (const HirLambda &instance : program.lambdaInstances()) {
    verifyCallableValues(instance.body);
  }

  std::vector<std::size_t> threadTaskRoots(program.threadTaskAdapters().size(),
                                           0);
  std::vector<SemanticType> threadTaskResults(
      program.threadTaskAdapters().size(), SemanticType::Unknown);
  const auto verifyThreadTaskValues = [&](const HirBody &body) {
    for (const HirValue &value : body.values) {
      if (value.intrinsic == IntrinsicKind::ThreadScope) {
        if (value.threadTaskAdapter ||
            value.scopedThreadTasks.size() != value.parameterTypes.size() ||
            value.info.type != SemanticType::Int32 ||
            value.synchronization.kind !=
                SynchronizationOperationKind::ThreadScope ||
            !validSynchronizationOperation(value.synchronization) ||
            program.executionProfile() != ExecutionProfile::Concurrent)
          result.errors.emplace_back(
              "parallel scope has an invalid HIR group contract");
        for (std::size_t index = 0; index < value.scopedThreadTasks.size();
             ++index) {
          const auto *adapter =
              program.findThreadTaskAdapter(value.scopedThreadTasks[index]);
          if (adapter == nullptr || !adapter->scoped ||
              adapter->spawnValue != value.id ||
              adapter->scopeArgument != index ||
              index >= value.parameterTypes.size() ||
              adapter->taskType != value.parameterTypes[index]) {
            result.errors.emplace_back(
                "parallel scope has no exact HIR task operand");
            continue;
          }
          for (const auto &dependency : adapter->scopeDependencies)
            if (dependency.place.domain != body.placeDomain ||
                (dependency.place.root == 0 && !dependency.place.receiver) ||
                dependency.designatedFields.empty())
              result.errors.emplace_back(
                  "parallel task has no exact body-local dependency");
          ++threadTaskRoots[adapter->id - 1];
          threadTaskResults[adapter->id - 1] = SemanticType::Void;
        }
        continue;
      }
      if (!value.scopedThreadTasks.empty())
        result.errors.emplace_back(
            "non-scope HIR value carries scoped task operands");
      if (value.intrinsic != IntrinsicKind::ThreadSpawn) {
        if (value.threadTaskAdapter) {
          result.errors.emplace_back(
              "non-spawn HIR value carries a thread task adapter");
        }
        continue;
      }
      const HirThreadTaskAdapter *adapter =
          value.threadTaskAdapter
              ? program.findThreadTaskAdapter(*value.threadTaskAdapter)
              : nullptr;
      if (adapter == nullptr || adapter->spawnValue != value.id ||
          adapter->scoped || !adapter->scopeDependencies.empty()) {
        result.errors.emplace_back(
            "managed thread spawn has no exact HIR task adapter");
        continue;
      }
      const SemanticType *handleType =
          value.parameterTypes.size() == 2 &&
                  value.parameterTypes.front().kind ==
                      SemanticType::Reference &&
                  value.parameterTypes.front().arguments.size() == 1
              ? &value.parameterTypes.front().arguments.front()
              : nullptr;
      if (handleType == nullptr || handleType->kind != SemanticType::Class ||
          handleType->arguments.size() > 1) {
        result.errors.emplace_back(
            "managed thread spawn has no exact result handle type");
        continue;
      }
      const SemanticType taskResult = handleType->arguments.empty()
                                          ? SemanticType::Void
                                          : handleType->arguments.front();
      SemanticType &recorded = threadTaskResults[adapter->id - 1];
      if (recorded != SemanticType::Unknown && recorded != taskResult) {
        result.errors.emplace_back(
            "managed thread task adapter has conflicting result types");
        continue;
      }
      recorded = taskResult;
      ++threadTaskRoots[adapter->id - 1];
    }
  };
  verifyThreadTaskValues(program.module());
  for (const auto &storage : program.threadLocalStorage())
    verifyThreadTaskValues(storage.initializer);
  for (const HirClassInstance &instance : program.classInstances()) {
    verifyThreadTaskValues(instance.fieldInitializers);
    verifyThreadTaskValues(instance.staticFieldInitializers);
  }
  for (const HirFunctionInstance &instance : program.functionInstances()) {
    verifyThreadTaskValues(instance.body);
  }
  for (const HirConstructorInstance &instance :
       program.constructorInstances()) {
    verifyThreadTaskValues(instance.body);
  }
  for (const HirDestructorInstance &instance : program.destructorInstances()) {
    verifyThreadTaskValues(instance.body);
  }
  for (const HirLambda &instance : program.lambdaInstances()) {
    verifyThreadTaskValues(instance.body);
  }

  for (std::size_t index = 0; index < program.threadTaskAdapters().size();
       ++index) {
    const HirThreadTaskAdapter &adapter = program.threadTaskAdapters()[index];
    const bool functionEntry = adapter.functionTarget.has_value();
    const bool lambdaEntry = adapter.lambdaTarget.has_value();
    bool exactEntry = functionEntry != lambdaEntry;
    if (functionEntry) {
      const HirFunctionInstance *target =
          program.findFunctionInstance(*adapter.functionTarget);
      const FunctionInfo *declaration =
          target == nullptr ? nullptr
                            : semantics.findFunction(target->declaration);
      const HirClassInstance *owner =
          target == nullptr || !target->owner ||
                  *target->owner > program.classInstances().size()
              ? nullptr
              : &program.classInstances()[*target->owner - 1];
      const bool functionItemEntry = adapter.taskType.hasFunctionItemShape();
      exactEntry =
          exactEntry && target != nullptr && declaration != nullptr &&
          target->source != nullptr &&
          declaration->declaration == target->source &&
          target->returnType == threadTaskResults[index] &&
          target->parameterTypes.empty() &&
          (functionItemEntry
               ? owner == nullptr &&
                     adapter.taskType.functionId == target->declaration &&
                     adapter.targetInvocation ==
                         CallableInvocationCapability::Read
               : owner != nullptr && owner->type == adapter.taskType &&
                     target->source->operatorName().has_value() &&
                     target->source->operatorName()->kind ==
                         OverloadedOperator::Call &&
                     adapter.targetInvocation ==
                         callableInvocationCapability(
                             target->source->receiverMutability()));
    }
    if (lambdaEntry) {
      const HirLambda *target = program.findLambda(*adapter.lambdaTarget);
      exactEntry =
          exactEntry && target != nullptr && target->type == adapter.taskType &&
          target->returnType == threadTaskResults[index] &&
          target->parameterTypes.empty() &&
          adapter.targetInvocation == CallableInvocationCapability::Read;
    }
    if (adapter.id != index + 1 || adapter.spawnValue == 0 ||
        adapter.taskType == SemanticType::Unknown ||
        adapter.taskType.kind == SemanticType::TypeParameter ||
        threadTaskResults[index] == SemanticType::Unknown ||
        adapter.invocation != CallableInvocationCapability::Once ||
        program.executionProfile() != ExecutionProfile::Concurrent ||
        threadTaskRoots[index] != 1 || !exactEntry) {
      result.errors.emplace_back(
          "managed thread HIR task adapter contract is invalid");
    }
  }

  // Statements inside a compile-time branch or expansion that the symbolic
  // primary deferred (a dependent `if constexpr` condition or `constexpr for`
  // range) have no primary semantic record: concrete reanalysis analyzed them
  // first. Their HIR plan is verified on its own concrete facts.
  const auto deferredPrimaryStatements = [&](const HirBody &body) {
    std::unordered_set<HirStatementId> deferred;
    const auto visit = [&](auto &self, HirStatementId id) -> void {
      const HirStatement *statement = body.findStatement(id);
      if (statement == nullptr || !deferred.insert(id).second) {
        return;
      }
      if (statement->initializer) {
        self(self, *statement->initializer);
      }
      if (statement->body) {
        self(self, *statement->body);
      }
      if (statement->elseBranch) {
        self(self, *statement->elseBranch);
      }
      for (const HirStatementId nested : statement->statements) {
        self(self, nested);
      }
      for (const HirSwitchArm &arm : statement->switchArms) {
        for (const HirStatementId nested : arm.statements) {
          self(self, nested);
        }
      }
    };
    for (const HirStatement &statement : body.statements) {
      if (statement.kind != HirStatementKind::CompileTimeBranch) {
        continue;
      }
      const auto *ifStatement = stmtAs<IfStmt>(statement.source);
      const auto *loop = stmtAs<ConstexprForStmt>(statement.source);
      const bool deferredInPrimary =
          (ifStatement != nullptr && ifStatement->isConstexpr() &&
           !semantics.findConstexprBranch(*ifStatement).has_value()) ||
          (loop != nullptr &&
           semantics.findConstexprForExpansion(*loop) == nullptr);
      if (deferredInPrimary) {
        visit(visit, statement.id);
      }
    }
    return deferred;
  };

  const auto verifyRangeIterations = [&](const HirBody &body,
                                         bool concreteReanalysis = false) {
    const std::unordered_set<HirStatementId> deferred =
        concreteReanalysis ? deferredPrimaryStatements(body)
                           : std::unordered_set<HirStatementId>{};
    for (const HirStatement &statement : body.statements) {
      const auto *source = stmtAs<RangeForStmt>(statement.source);
      if (statement.kind != HirStatementKind::RangeFor) {
        if (statement.rangeIteration) {
          result.errors.emplace_back(
              "non-range HIR statement carries range iteration metadata");
        }
        continue;
      }
      const ResolvedRangeIterationInfo *semantic =
          source == nullptr ? nullptr : semantics.findRangeIteration(*source);
      const bool primaryDeferred =
          semantic == nullptr && deferred.contains(statement.id);
      const HirRangeIteration *range =
          statement.rangeIteration ? &*statement.rangeIteration : nullptr;
      const auto bindingFor = [&](HirBindingId id) -> const HirBinding * {
        const auto found = std::find_if(
            body.bindings.begin(), body.bindings.end(),
            [&](const HirBinding &binding) { return binding.id == id; });
        return found == body.bindings.end() ? nullptr : &*found;
      };
      const HirBinding *owner =
          range == nullptr ? nullptr : bindingFor(range->owner);
      const auto exactBinding = [&](HirBindingId id,
                                    const VariableDecl *declaration) {
        const HirBinding *binding = bindingFor(id);
        return binding != nullptr && binding->variable == declaration;
      };
      const auto exactValue = [&](HirValueId id, const ExprPtr &expression) {
        const HirValue *value = body.findValue(id);
        return expression && value != nullptr &&
               body.valueForSource(*expression) == value;
      };
      const HirLoan *rangeLoan =
          range == nullptr ? nullptr : body.findLoan(range->rangeLoan);
      const HirLoan *elementLoan =
          range == nullptr ? nullptr : body.findLoan(range->elementLoan);
      const HirLoan *elementOwnerLoan =
          range == nullptr ? nullptr : body.findLoan(range->elementOwnerLoan);
      const bool referenceElement =
          source != nullptr && source->elementBinding() != nullptr &&
          source->elementBinding()->type().reference.has_value();
      const HirBinding *rangeCarrier =
          range == nullptr ? nullptr : bindingFor(range->rangeCarrier);
      const HirBinding *iterator =
          range == nullptr ? nullptr : bindingFor(range->iterator);
      const HirBinding *sentinel =
          range == nullptr ? nullptr : bindingFor(range->sentinel);
      const HirBinding *element =
          range == nullptr ? nullptr : bindingFor(range->element);
      const HirBinding *elementOwner =
          range == nullptr ? nullptr : bindingFor(range->elementOwner);
      const HirValue *elementAccessValue =
          range == nullptr ? nullptr
                           : body.findValue(range->elementAccessValue);
      const HirStatement *elementOwnerStatement =
          range == nullptr ? nullptr
                           : body.findStatement(range->elementOwnerStatement);
      const bool exactConcreteTypes =
          range != nullptr && rangeCarrier != nullptr && iterator != nullptr &&
          sentinel != nullptr && element != nullptr &&
          range->rangeType == referentOrSelf(rangeCarrier->info.type) &&
          range->iteratorType == iterator->info.type &&
          range->sentinelType == sentinel->info.type &&
          range->elementType == referentOrSelf(element->info.type) &&
          range->elementAccess ==
              (element->info.type.kind == SemanticType::Reference
                   ? element->info.type.referenceAccess
                   : AccessMode::ReadOnly) &&
          !containsUnresolvedGeneric(range->rangeType) &&
          !containsUnresolvedGeneric(range->iteratorType) &&
          !containsUnresolvedGeneric(range->sentinelType) &&
          !containsUnresolvedGeneric(range->elementType);
      const bool genericSemantic =
          semantic != nullptr &&
          (containsUnresolvedGeneric(semantic->rangeType) ||
           containsUnresolvedGeneric(semantic->iteratorType) ||
           containsUnresolvedGeneric(semantic->sentinelType) ||
           containsUnresolvedGeneric(semantic->elementType));
      const bool exactLoanProvenance =
          semantic != nullptr && range != nullptr &&
          (((genericSemantic || concreteReanalysis) &&
            (range->rangeLoan != 0) == (semantic->rangeLoan != 0) &&
            (range->elementLoan != 0) == (semantic->elementLoan != 0) &&
            (genericSemantic ||
             ((range->elementOwnerLoan != 0) ==
                  (semantic->elementOwnerLoan != 0) &&
              (range->rangeLoan == range->elementLoan) ==
                  (semantic->rangeLoan == semantic->elementLoan)))) ||
           (!genericSemantic && !concreteReanalysis &&
            range->rangeLoan == semantic->rangeLoan &&
            range->elementLoan == semantic->elementLoan &&
            range->elementOwnerLoan == semantic->elementOwnerLoan));
      const bool exactSemantic =
          (primaryDeferred && range != nullptr && exactConcreteTypes) ||
          (semantic != nullptr && range != nullptr && exactConcreteTypes &&
           (genericSemantic || range->strategy == semantic->strategy) &&
           (genericSemantic || (range->rangeType == semantic->rangeType &&
                                range->iteratorType == semantic->iteratorType &&
                                range->sentinelType == semantic->sentinelType &&
                                range->elementType == semantic->elementType &&
                                range->arrayLength == semantic->arrayLength)) &&
           range->elementAccess == semantic->elementAccess &&
           exactLoanProvenance &&
           (genericSemantic ||
            (range->beginFunction == semantic->beginFunction &&
             range->endFunction == semantic->endFunction &&
             range->compareFunction == semantic->compareFunction &&
             range->dereferenceFunction == semantic->dereferenceFunction &&
             range->incrementFunction == semantic->incrementFunction)) &&
           range->ownsRangeTemporary == semantic->ownsRangeTemporary &&
           (genericSemantic ||
            range->nonBorrowingEndpoint == semantic->nonBorrowingEndpoint) &&
           (genericSemantic || range->materializesElementValue ==
                                   semantic->materializesElementValue));
      const bool exactStructure =
          source != nullptr && range != nullptr && statement.body &&
          body.findStatement(*statement.body) != nullptr &&
          body.findStatement(range->coreLoop) != nullptr &&
          exactBinding(range->owner, source->ownerBinding()) &&
          exactBinding(range->rangeCarrier, source->rangeBinding()) &&
          exactBinding(range->iterator, source->iteratorBinding()) &&
          exactBinding(range->sentinel, source->sentinelBinding()) &&
          exactBinding(range->element, source->elementBinding()) &&
          (source->elementOwnerBinding() == nullptr
               ? range->elementOwner == 0
               : exactBinding(range->elementOwner,
                              source->elementOwnerBinding())) &&
          exactValue(range->source, source->ownerBinding()->initializer()) &&
          exactValue(range->begin, source->iteratorBinding()->initializer()) &&
          exactValue(range->end, source->sentinelBinding()->initializer()) &&
          exactValue(range->comparison, source->coreLoop()->condition()) &&
          exactValue(range->elementAccessValue,
                     source->elementOwnerBinding() == nullptr
                         ? source->elementBinding()->initializer()
                         : source->elementOwnerBinding()->initializer()) &&
          exactValue(range->increment, source->coreLoop()->increment());
      const bool exactLoans =
          rangeLoan != nullptr && rangeLoan->access == range->elementAccess &&
          std::find(rangeLoan->carriers.begin(), rangeLoan->carriers.end(),
                    range->rangeCarrier) != rangeLoan->carriers.end() &&
          (!referenceElement ||
           (elementLoan != nullptr &&
            elementLoan->access == range->elementAccess &&
            std::find(elementLoan->carriers.begin(),
                      elementLoan->carriers.end(),
                      range->element) != elementLoan->carriers.end() &&
            (range->elementAccess == AccessMode::ReadOnly ||
             (range->elementLoan != range->rangeLoan &&
              elementLoan->parent == range->rangeLoan)))) &&
          (range->elementAccess == AccessMode::ReadOnly ||
           range->strategy != RangeIterationStrategy::Structural ||
           std::find(rangeLoan->carriers.begin(), rangeLoan->carriers.end(),
                     range->iterator) != rangeLoan->carriers.end());
      const HirValue *writtenElementSource =
          source == nullptr || source->elementBinding() == nullptr
              ? nullptr
              : body.valueForSource(*source->elementBinding()->initializer());
      const bool exactElementStaging =
          range != nullptr && source != nullptr &&
          elementAccessValue != nullptr &&
          (source->elementOwnerBinding() == nullptr
               ? elementOwner == nullptr && range->elementOwnerStatement == 0 &&
                     range->elementOwnerLoan == 0 &&
                     !range->materializesElementValue
               : source->elementOwnerBinding()->isRangeElementOwner() &&
                     elementOwner != nullptr &&
                     elementOwnerStatement != nullptr &&
                     elementOwnerStatement->source ==
                         source->elementOwnerBinding() &&
                     elementOwnerStatement->binding == range->elementOwner &&
                     elementOwnerStatement->value ==
                         range->elementAccessValue &&
                     elementOwner->info.access == AccessMode::ReadOnly &&
                     referentOrSelf(elementOwner->info.type) ==
                         elementAccessValue->info.type &&
                     writtenElementSource != nullptr &&
                     writtenElementSource->kind == HirValueKind::Variable &&
                     writtenElementSource->symbol ==
                         elementOwner->info.symbol &&
                     (range->materializesElementValue
                          ? elementAccessValue->info.category ==
                                    ValueCategory::Value &&
                                elementAccessValue->info.type.kind !=
                                    SemanticType::Reference &&
                                elementAccessValue->info.type.kind !=
                                    SemanticType::StringView &&
                                elementAccessValue->info.type.kind !=
                                    SemanticType::CString &&
                                elementAccessValue->info.traits.copyable &&
                                !elementAccessValue->info.traits
                                     .containsBorrowedState &&
                                elementAccessValue->borrowOrigin ==
                                    BorrowOriginKind::None &&
                                elementOwner->info.type ==
                                    elementAccessValue->info.type &&
                                range->elementOwnerLoan == 0 &&
                                range->elementAccess == AccessMode::ReadOnly &&
                                (!referenceElement ||
                                 (elementLoan != nullptr &&
                                  elementLoan->parent == 0 &&
                                  elementLoan->place.root ==
                                      elementOwner->info.symbol &&
                                  elementLoan->place.projections.empty() &&
                                  !elementLoan->place.receiver))
                          : elementAccessValue->info.category ==
                                    ValueCategory::Place &&
                                elementOwner->info.type.kind ==
                                    SemanticType::Reference &&
                                elementOwnerLoan != nullptr &&
                                elementOwnerLoan->access ==
                                    AccessMode::ReadOnly &&
                                std::find(elementOwnerLoan->carriers.begin(),
                                          elementOwnerLoan->carriers.end(),
                                          elementOwner->id) !=
                                    elementOwnerLoan->carriers.end()));
      const bool exactStrategy =
          range != nullptr &&
          ((range->strategy == RangeIterationStrategy::FixedArray &&
            range->rangeType.kind == SemanticType::Array &&
            range->rangeType.arrayLengthParameterId == 0 &&
            range->rangeType.arrayLength == range->arrayLength &&
            range->arrayLength != 0 &&
            body.findValue(range->elementAccessValue) != nullptr &&
            body.findValue(range->elementAccessValue)->kind ==
                HirValueKind::Index &&
            range->beginFunction == 0 && range->endFunction == 0) ||
           (range->strategy == RangeIterationStrategy::Structural &&
            range->beginFunction != 0 && range->endFunction != 0 &&
            range->compareFunction != 0 && range->dereferenceFunction != 0 &&
            range->incrementFunction != 0));
      const bool exactOwner =
          owner != nullptr &&
          (range == nullptr ||
           range->ownsRangeTemporary ==
               (owner->info.type.kind != SemanticType::Reference));
      const auto *endpoint = range ? body.findValue(range->end) : nullptr;
      const bool exactEndpointResult =
          range && (!range->nonBorrowingEndpoint ||
                    (endpoint && endpoint->info.type == range->sentinelType &&
                     endpoint->info.type.kind != SemanticType::Reference &&
                     !endpoint->info.traits.containsBorrowedState &&
                     endpoint->borrowOrigin == BorrowOriginKind::None));
      if (!exactSemantic || !exactStructure || !exactLoans || !exactStrategy ||
          !exactOwner || range == nullptr || !exactEndpointResult ||
          !exactElementStaging) {
        result.errors.emplace_back(
            "range HIR plan differs from semantic owner/loan/protocol facts");
      }
    }
  };
  verifyRangeIterations(program.module());
  for (const auto &storage : program.threadLocalStorage())
    verifyRangeIterations(storage.initializer);
  for (const HirClassInstance &instance : program.classInstances()) {
    const bool reanalyzed =
        !instance.typeArguments.empty() || !instance.valueArguments.empty();
    verifyRangeIterations(instance.fieldInitializers, reanalyzed);
    verifyRangeIterations(instance.staticFieldInitializers, reanalyzed);
  }
  for (const HirFunctionInstance &instance : program.functionInstances()) {
    const FunctionInfo *function = semantics.findFunction(instance.declaration);
    const ClassTypeInfo *owner =
        function == nullptr || function->ownerClass == 0
            ? nullptr
            : semantics.findClassType(function->ownerClass);
    // Reanalysis starts a new body-local loan domain even when the range's
    // type happens not to mention any generic parameter. Compare the loan
    // relationship, not numeric identities allocated in the primary body.
    verifyRangeIterations(
        instance.body,
        function != nullptr &&
            (!function->genericParameters.empty() ||
             (owner != nullptr && !owner->genericParameters.empty())));
  }
  for (const HirConstructorInstance &instance :
       program.constructorInstances()) {
    verifyRangeIterations(instance.body, !instance.generatedDefault);
  }
  for (const HirDestructorInstance &instance : program.destructorInstances()) {
    const HirClassInstance *owner =
        instance.owner == 0 || instance.owner > program.classInstances().size()
            ? nullptr
            : &program.classInstances()[instance.owner - 1];
    verifyRangeIterations(instance.body,
                          owner != nullptr && (!owner->typeArguments.empty() ||
                                               !owner->valueArguments.empty()));
  }
  for (const HirLambda &instance : program.lambdaInstances()) {
    verifyRangeIterations(
        instance.body,
        !instance.type.lambdaEnclosingClassTypes.empty() ||
            !instance.type.lambdaEnclosingClassValues.empty() ||
            !instance.type.lambdaEnclosingFunctionTypes.empty() ||
            !instance.type.lambdaEnclosingFunctionValues.empty());
  }
  if (program.foreignStorageDeclarations() !=
      semantics.foreignStorageDeclarations())
    result.errors.emplace_back(
        "HIR foreign storage differs from semantic declarations");
  const auto &semanticTls = semantics.threadLocalInitializationPlan().steps;
  const auto &hirTls = program.threadLocalStorage();
  if (semanticTls.size() != hirTls.size())
    result.errors.emplace_back(
        "HIR thread-local inventory differs from semantic declarations");
  std::unordered_set<SymbolId> tlsSymbols;
  for (std::size_t index = 0;
       index < std::min(semanticTls.size(), hirTls.size()); ++index) {
    const auto &semantic = semanticTls[index];
    const auto &storage = hirTls[index];
    const auto *binding = semantic.declaration
                              ? semantics.findBinding(*semantic.declaration)
                              : nullptr;
    bool exact =
        binding && binding->threadLocal && storage.symbol != 0 &&
        tlsSymbols.insert(storage.symbol).second &&
        storage.symbol == semantic.symbol &&
        storage.sourceUnit == semantic.sourceUnit &&
        storage.source == semantic.declaration &&
        storage.type == binding->type && storage.traits == binding->traits &&
        storage.access == binding->access &&
        storage.requiresActiveCleanup == semantic.requiresActiveCleanup &&
        storage.ensureFailure == semantic.ensureFailure &&
        storage.dataInitializer.has_value() ==
            (semantic.role == ProgramInitializationStepRole::DataOnly) &&
        storage.initializer.roots.size() == 1;
    const auto *returned = storage.initializer.roots.size() == 1
                               ? storage.initializer.findStatement(
                                     storage.initializer.roots.front())
                               : nullptr;
    exact = exact && returned && returned->kind == HirStatementKind::Return &&
            returned->source == storage.source && returned->value.has_value();
    if (exact && storage.source->initializer()) {
      const auto *value = storage.initializer.initializerValueForSource(
          *storage.source->initializer());
      exact = value && *returned->value == value->id;
    }
    if (!exact)
      result.errors.emplace_back("HIR thread-local initializer loses its exact "
                                 "semantic storage proof");
  }
  const ProgramInitializationPlan &semanticInitialization =
      semantics.programInitializationPlan();
  const HirProgramInitializationPlan &initialization =
      program.programInitializationPlan();
  if (initialization.unitOrder != semanticInitialization.unitOrder) {
    result.errors.emplace_back(
        "program initialization source-unit order differs from semantics");
  }
  if (initialization.steps.size() != semanticInitialization.steps.size()) {
    result.errors.emplace_back(
        "program initialization step count differs from semantics");
  }
  std::vector<HirStatementId> expectedRoots;
  std::unordered_set<HirBindingId> expectedBindings;
  std::unordered_set<HirStatementId> expectedStatements;
  std::vector<HirValueId> initializerValues;
  const std::size_t stepCount = std::min(initialization.steps.size(),
                                         semanticInitialization.steps.size());
  for (std::size_t index = 0; index < stepCount; ++index) {
    const ProgramInitializationStep &semantic =
        semanticInitialization.steps[index];
    const HirProgramInitializationStep &step = initialization.steps[index];
    if (step.id != semantic.id || step.sourceUnit != semantic.sourceUnit ||
        step.kind != semantic.kind || step.role != semantic.role ||
        step.source != semantic.declaration || step.symbol != semantic.symbol ||
        step.requiresActiveCleanup != semantic.requiresActiveCleanup) {
      result.errors.emplace_back(
          "program initialization step identity differs from semantics");
    }
    const HirBinding *binding = nullptr;
    const auto bindingFound = std::find_if(
        program.module().bindings.begin(), program.module().bindings.end(),
        [&](const HirBinding &candidate) {
          return candidate.id == step.binding;
        });
    if (bindingFound != program.module().bindings.end()) {
      binding = &*bindingFound;
    }
    const BindingInfo *semanticBinding =
        semantic.declaration == nullptr
            ? nullptr
            : semantics.findBinding(*semantic.declaration);
    if (step.binding == 0 || binding == nullptr ||
        binding->variable != semantic.declaration ||
        semanticBinding == nullptr || binding->info != *semanticBinding ||
        binding->info.symbol != semantic.symbol ||
        !expectedBindings.insert(step.binding).second) {
      result.errors.emplace_back(
          "program initialization step has no exact module binding");
    }
    if (step.kind == ProgramStorageKind::NamespaceGlobal) {
      if (step.ownerClass != 0 || semantic.ownerClass != 0) {
        result.errors.emplace_back(
            "namespace-global initialization step has a class owner");
      }
    } else {
      const HirClassInstance *owner =
          step.ownerClass == 0 ||
                  step.ownerClass > program.classInstances().size()
              ? nullptr
              : &program.classInstances()[step.ownerClass - 1];
      if (owner == nullptr || owner->declaration != semantic.ownerClass) {
        result.errors.emplace_back(
            "static-field initialization step has the wrong class instance");
      } else {
        const auto field =
            std::find_if(owner->staticFields.begin(), owner->staticFields.end(),
                         [&](const HirClassField &candidate) {
                           return candidate.declaration == semantic.declaration;
                         });
        if (field == owner->staticFields.end() || semanticBinding == nullptr ||
            field->binding != step.binding ||
            field->initializer != step.initializer ||
            field->info != *semanticBinding ||
            field->requiresActiveCleanup != semantic.requiresActiveCleanup) {
          result.errors.emplace_back(
              "static-field metadata differs from its module plan step");
        }
        if (!owner->staticFieldInitializers.bindings.empty() ||
            !owner->staticFieldInitializers.values.empty() ||
            !owner->staticFieldInitializers.statements.empty() ||
            !owner->staticFieldInitializers.roots.empty()) {
          result.errors.emplace_back(
              "planned static field is duplicated in a class initializer body");
        }
        if (owner->staticFieldInitializers.placeDomain.snapshot !=
                program.module().placeDomain.snapshot ||
            owner->staticFieldInitializers.placeDomain.body == 0 ||
            owner->staticFieldInitializers.placeDomain ==
                program.module().placeDomain) {
          result.errors.emplace_back("planned static field has no distinct "
                                     "coherent empty-body domain");
        }
      }
    }
    if (step.role == ProgramInitializationStepRole::DataOnly) {
      if (step.initializer || step.statement != 0) {
        result.errors.emplace_back(
            "data-only program storage has an executable HIR root");
      }
      continue;
    }
    expectedRoots.push_back(step.statement);
    expectedStatements.insert(step.statement);
    if (step.initializer) {
      initializerValues.push_back(*step.initializer);
    }
    const HirStatement *statement =
        program.module().findStatement(step.statement);
    const HirValue *initializer =
        step.initializer ? program.module().findValue(*step.initializer)
                         : nullptr;
    if (!step.initializer || initializer == nullptr ||
        semantic.declaration->initializer() == nullptr ||
        program.module().initializerValueForSource(
            *semantic.declaration->initializer()) != initializer) {
      result.errors.emplace_back(
          "executable program initializer has no exact source HIR value");
    }
    if (step.statement == 0 || statement == nullptr ||
        statement->kind != HirStatementKind::Variable ||
        statement->source != semantic.declaration ||
        statement->binding != step.binding ||
        statement->value != step.initializer) {
      result.errors.emplace_back(
          "executable program initializer has no exact module statement");
    }
  }
  if (program.module().roots != expectedRoots) {
    result.errors.emplace_back(
        "module roots do not exactly match executable initialization steps");
  }
  const auto reachable =
      hirInitializerReachability(program.module(), initializerValues);
  // Plan roots remain exact and ordered. Only locals and statements nested
  // inside their initializer values are additional executable inventory.
  expectedBindings.insert(reachable.bindings.begin(), reachable.bindings.end());
  expectedStatements.insert(reachable.statements.begin(),
                            reachable.statements.end());
  if (!exactInventory(program.module().bindings, expectedBindings)) {
    result.errors.emplace_back(
        "module binding inventory contains unplanned program storage");
  }
  if (!exactInventory(program.module().statements, expectedStatements)) {
    result.errors.emplace_back(
        "module statement inventory contains an unplanned initializer");
  }
  const auto &reachableValues = reachable.values;
  if (!reachable.complete ||
      !exactInventory(program.module().values, reachableValues)) {
    result.errors.emplace_back(
        "module value inventory is not exactly reachable from planned "
        "initializer roots");
  }
  std::unordered_map<HirValueId, std::vector<const HirValue *>> valueParents;
  for (const HirValue &parent : program.module().values) {
    for (const HirValueId operand : parent.operands) {
      valueParents[operand].push_back(&parent);
    }
    for (const HirValueId input : parent.sequencedInputs) {
      valueParents[input].push_back(&parent);
    }
  }
  // The source-bearing ArrayInitializer rooting a (possibly nested)
  // empty-brace default expansion, reached by climbing the synthetic
  // levels. Every hop must be the exact single-consumer array shape.
  const auto defaultArrayRootInitializer =
      [&](const HirValue &value) -> const HirValue * {
    const HirValue *current = &value;
    for (std::size_t depth = 0; depth < program.module().values.size();
         ++depth) {
      const auto parents = valueParents.find(current->id);
      if (parents == valueParents.end() || parents->second.size() != 1) {
        return nullptr;
      }
      const HirValue &parent = *parents->second.front();
      if (parent.kind != HirValueKind::ArrayInitializer ||
          parent.info.type.kind != SemanticType::Array ||
          parent.info.type.arguments.size() != 1 ||
          parent.info.type.arguments.front() != current->info.type ||
          parent.info.type.arrayLengthParameterId != 0 ||
          parent.operands.size() != parent.info.type.arrayLength ||
          std::count(parent.operands.begin(), parent.operands.end(),
                     current->id) != 1) {
        return nullptr;
      }
      if (parent.source != nullptr) {
        return &parent;
      }
      current = &parent;
    }
    return nullptr;
  };
  const auto exactDefaultArrayElement = [&](const HirValue &value) {
    const auto parents = valueParents.find(value.id);
    if (value.source != nullptr ||
        value.kind != HirValueKind::DirectInitializer ||
        parents == valueParents.end() || parents->second.size() != 1) {
      return false;
    }
    const HirValue &parent = *parents->second.front();
    const HirValue *root = defaultArrayRootInitializer(value);
    const auto *initializer =
        root == nullptr ? nullptr : exprAs<ArrayInitializer>(root->source);
    const ResolvedConstructionInfo *construction =
        initializer == nullptr
            ? nullptr
            : semantics.findArrayElementConstruction(*initializer);
    const HirClassInstance *owner = nullptr;
    for (const HirClassInstance &candidate : program.classInstances()) {
      if (construction == nullptr ||
          candidate.type != construction->constructedType) {
        continue;
      }
      if (owner != nullptr) {
        return false;
      }
      owner = &candidate;
    }
    const HirConstructorInstance *target =
        value.constructorTarget
            ? program.findConstructorInstance(*value.constructorTarget)
            : nullptr;
    const HirClassInstance *targetOwner =
        target == nullptr || target->owner == 0 ||
                target->owner > program.classInstances().size()
            ? nullptr
            : &program.classInstances()[target->owner - 1];
    const bool exactTarget =
        construction != nullptr &&
        ((construction->generatedDefault && ([&] {
            const auto *lifecycle =
                owner != nullptr && owner->source != nullptr
                    ? semantics.findClassLifecycle(*owner->source)
                    : nullptr;
            return lifecycle != nullptr &&
                           lifecycle->generatedDefaultInitializers
                       ? target != nullptr && target->generatedDefault &&
                             targetOwner == owner
                       : !value.constructorTarget;
          }())) ||
         (!construction->generatedDefault && construction->constructor != 0 &&
          target != nullptr && targetOwner == owner &&
          target->declaration == construction->constructor));
    return construction != nullptr && owner != nullptr && exactTarget &&
           parent.kind == HirValueKind::ArrayInitializer &&
           parent.info.type.kind == SemanticType::Array &&
           parent.info.type.arguments.size() == 1 &&
           parent.info.type.arguments.front() == owner->type &&
           parent.info.type.arrayLengthParameterId == 0 &&
           parent.operands.size() == parent.info.type.arrayLength &&
           std::count(parent.operands.begin(), parent.operands.end(),
                      value.id) == 1 &&
           value.info == ExpressionInfo{.type = owner->type,
                                        .category = ValueCategory::Value,
                                        .access = AccessMode::ReadOnly,
                                        .traits = owner->traits} &&
           value.parameterTypes == construction->parameterTypes &&
           value.operands.size() == value.parameterTypes.size() &&
           value.constructorKind == construction->kind &&
           value.definedFailure.propagation ==
               ((construction->constructor != 0 &&
                 !construction->generatedDefault) ||
                        (target != nullptr && target->generatedDefault)
                    ? FailurePropagationKind::Constructor
                    : FailurePropagationKind::None);
  };
  const auto exactSyntheticCommon = [](const HirValue &value) {
    return value.source == nullptr &&
           value.unsafeOperation == UnsafeOperationKind::None &&
           value.operands.empty() && value.sequencedInputs.empty() &&
           value.parameterTypes.empty() && !value.operation &&
           !value.constant && !value.programConstantSubstitution &&
           value.intrinsic == IntrinsicKind::None &&
           value.synchronization == SynchronizationOperation{} &&
           value.definedFailure.empty() &&
           value.borrowOrigin == BorrowOriginKind::None &&
           value.borrowArgument == 0 &&
           value.borrowAccess == AccessMode::ReadOnly && !value.borrowPlace &&
           !value.storedReferenceAccess &&
           value.dispatch == CallDispatch::Static &&
           value.dispatchOwner == SemanticType::Unknown && !value.receiver &&
           !value.callPlan && value.packFoldSymbol == 0 &&
           value.packFoldParameter == 0 && value.packFoldFunction == 0 &&
           value.packFoldArgument == 0 && value.packFoldElements.empty() &&
           value.packExpansionElements.empty() && !value.functionTarget &&
           !value.nativeCallbackAdapter && !value.constructorTarget &&
           value.constructorKind == ConstructorKind::Ordinary &&
           !value.lambdaTarget && value.callableArguments.empty() &&
           !value.callableBoundary && !value.callableInvocation &&
           !value.enumOwner && !value.enumValue && !value.enumVariant &&
           !value.payloadIndex && !value.payloadExtractionKind &&
           !value.ownership && !value.dropObligation;
  };
  // A synthetic nested level of an empty-brace fixed-array expansion: an
  // exact fixed-array value whose elements recurse toward the recorded leaf
  // construction of its rooted source initializer.
  const auto exactDefaultArrayLevel = [&](const HirValue &value) {
    if (value.source != nullptr ||
        value.kind != HirValueKind::ArrayInitializer ||
        value.info.type.kind != SemanticType::Array ||
        value.info.type.arguments.size() != 1 ||
        value.info.type.arrayLengthParameterId != 0 ||
        value.info.type.arrayLength == 0 ||
        value.operands.size() != value.info.type.arrayLength ||
        value.info.category != ValueCategory::Value ||
        value.info.access != AccessMode::ReadOnly ||
        value.unsafeOperation != UnsafeOperationKind::None ||
        !value.parameterTypes.empty() || !value.sequencedInputs.empty() ||
        value.operation || value.constant ||
        value.programConstantSubstitution ||
        value.intrinsic != IntrinsicKind::None ||
        value.synchronization != SynchronizationOperation{} ||
        !value.definedFailure.empty() ||
        value.borrowOrigin != BorrowOriginKind::None ||
        value.borrowArgument != 0 ||
        value.borrowAccess != AccessMode::ReadOnly || value.borrowPlace ||
        value.storedReferenceAccess || value.dispatch != CallDispatch::Static ||
        value.dispatchOwner != SemanticType::Unknown || value.receiver ||
        value.callPlan || value.packFoldSymbol != 0 ||
        value.packFoldParameter != 0 || value.packFoldFunction != 0 ||
        value.packFoldArgument != 0 || !value.packFoldElements.empty() ||
        !value.packExpansionElements.empty() || value.functionTarget ||
        value.nativeCallbackAdapter || value.constructorTarget ||
        value.constructorKind != ConstructorKind::Ordinary ||
        value.lambdaTarget || !value.callableArguments.empty() ||
        value.callableBoundary || value.callableInvocation || value.enumOwner ||
        value.enumValue || value.enumVariant || value.payloadIndex ||
        value.payloadExtractionKind || value.symbol != 0 || value.literal ||
        value.place) {
      return false;
    }
    const HirValue *root = defaultArrayRootInitializer(value);
    const auto *initializer =
        root == nullptr ? nullptr : exprAs<ArrayInitializer>(root->source);
    return initializer != nullptr &&
           semantics.findArrayElementConstruction(*initializer) != nullptr;
  };
  const auto listedContextualConstruction = [&](const HirValue &value) {
    const auto &entries = program.module().contextualConstructions;
    return value.source == nullptr &&
           value.kind == HirValueKind::DirectInitializer &&
           std::any_of(entries.begin(), entries.end(),
                       [&](const HirContextualConstruction &entry) {
                         return entry.value == value.id;
                       });
  };
  std::unordered_map<HirBindingId, const HirBinding *> moduleBindings;
  for (const auto &binding : program.module().bindings)
    moduleBindings.emplace(binding.id, &binding);
  std::unordered_map<HirValueId, std::optional<std::uint64_t>>
      structuredIndices;
  for (const auto &statement : program.module().statements) {
    const auto *source = stmtAs<StructuredBindingDecl>(statement.source);
    const auto *decomposition =
        source ? semantics.findStructuredBinding(*source) : nullptr;
    for (std::size_t index = 0; index < statement.structuredBindings.size();
         ++index) {
      const auto &element = statement.structuredBindings[index];
      if (!element.index)
        continue;
      const auto [found, inserted] =
          structuredIndices.emplace(*element.index, std::nullopt);
      if (!inserted) {
        found->second.reset();
        continue;
      }
      if (statement.kind != HirStatementKind::StructuredBinding ||
          decomposition == nullptr ||
          decomposition->source.type.kind != SemanticType::Array ||
          decomposition->source.type.arrayLengthParameterId != 0 ||
          decomposition->elements.size() !=
              statement.structuredBindings.size() ||
          index >= decomposition->elements.size() ||
          element.projection !=
              HirStructuredBindingProjectionKind::ArrayElement ||
          element.field != 0)
        continue;
      const auto &selected = decomposition->elements[index];
      const auto binding = moduleBindings.find(element.binding);
      if (selected.projection ==
              StructuredBindingProjectionKind::ArrayElement &&
          selected.index < decomposition->source.type.arrayLength &&
          binding != moduleBindings.end() &&
          binding->second->variable == selected.declaration &&
          binding->second->info == selected.binding)
        found->second = selected.index;
    }
  }
  const auto exactStructuredIndex = [&](const HirValue &value) {
    const auto *ordinal =
        value.literal ? std::get_if<std::uint64_t>(&*value.literal) : nullptr;
    const auto expected = structuredIndices.find(value.id);
    return exactSyntheticCommon(value) && value.kind == HirValueKind::Literal &&
           ordinal != nullptr && value.symbol == 0 && !value.place &&
           value.fullExpression == 0 && !value.regionBody &&
           value.defaultArguments.empty() &&
           value.info ==
               ExpressionInfo{.type = SemanticType::UInt64,
                              .category = ValueCategory::Value,
                              .access = AccessMode::ReadOnly,
                              .traits = semanticTraits(SemanticType::UInt64)} &&
           expected != structuredIndices.end() && expected->second &&
           *expected->second == *ordinal;
  };
  const auto exactSyntheticValue = [&](const HirValue &value) {
    if (exactDefaultArrayElement(value) || exactDefaultArrayLevel(value) ||
        listedContextualConstruction(value) || exactStructuredIndex(value)) {
      return true;
    }
    const auto parents = valueParents.find(value.id);
    if (!exactSyntheticCommon(value) || parents == valueParents.end() ||
        parents->second.size() != 1) {
      return false;
    }
    const HirValue &parent = *parents->second.front();
    if (value.kind == HirValueKind::Literal) {
      const auto *literal = exprAs<LiteralExpr>(parent.source);
      return literal != nullptr && parent.constructorTarget &&
             std::holds_alternative<std::nullptr_t>(literal->value()) &&
             parent.parameterTypes ==
                 std::vector<SemanticType>{SemanticType::NullPtr} &&
             parent.operands == std::vector<HirValueId>{value.id} &&
             value.info == ExpressionInfo{.type = SemanticType::NullPtr,
                                          .category = ValueCategory::Value,
                                          .access = AccessMode::ReadOnly,
                                          .traits = semanticTraits(
                                              SemanticType::NullPtr)} &&
             value.symbol == 0 && value.literal &&
             std::holds_alternative<std::nullptr_t>(*value.literal) &&
             !value.place && value.fullExpression == parent.fullExpression;
    }
    const auto *assignment = exprAs<Assign>(parent.source);
    const SymbolId symbol =
        assignment == nullptr ? 0 : semantics.findResolvedSymbol(*assignment);
    const SymbolRecord *record = semantics.database().findSymbol(symbol);
    const ResolvedOperatorInfo *operation =
        assignment == nullptr ? nullptr : semantics.findOperator(*assignment);
    const PlaceKey *semanticPlace =
        assignment == nullptr ? nullptr : semantics.findPlace(*assignment);
    if (assignment == nullptr || record == nullptr || operation == nullptr ||
        semanticPlace == nullptr || !parent.functionTarget ||
        parent.receiver != value.id || parent.operands.empty() ||
        parent.operands.front() != value.id) {
      return false;
    }
    PlaceKey exactPlace = *semanticPlace;
    exactPlace.domain = program.module().placeDomain;
    return value.kind == (assignment->path().segments.size() == 1
                              ? HirValueKind::Variable
                              : HirValueKind::QualifiedName) &&
           value.info == operation->receiverInfo && value.symbol == symbol &&
           !value.literal && value.place == exactPlace &&
           value.fullExpression == parent.fullExpression;
  };
  for (const HirValue &value : program.module().values) {
    if (!reachableValues.contains(value.id)) {
      continue;
    }
    if (value.source == nullptr) {
      if (!exactSyntheticValue(value)) {
        result.errors.emplace_back(
            "source-less module initializer value is not an exact admitted "
            "lowerer-owned shape");
      }
      continue;
    }
    // A contextual conversion of a source's value is a further call that
    // shares the source; its result is the conversion's, not the source's.
    if (contextualConversionCall(program.module(), value, semantics)) {
      const ResolvedOperatorInfo &conversion =
          *semantics.findContextualConversion(*value.source);
      if (value.info.type != conversion.returnType ||
          value.info.category != ValueCategory::Value || value.constant ||
          value.programConstantSubstitution) {
        result.errors.emplace_back(
            "module contextual conversion value differs from its semantic "
            "conversion");
      }
      continue;
    }
    const ExpressionInfo *semanticInfo =
        semantics.findExpression(*value.source);
    const std::optional<ConstantValue> semanticConstant =
        semantics.findConstant(*value.source);
    const bool semanticSubstitution =
        semantics.isProgramConstantSubstitution(*value.source);
    if (semanticInfo == nullptr || value.info != *semanticInfo ||
        value.constant != semanticConstant ||
        value.programConstantSubstitution != semanticSubstitution ||
        (value.programConstantSubstitution &&
         !materializableProgramConstant(value.constant))) {
      result.errors.emplace_back(
          "module initializer value differs from exact semantic "
          "constant/expression provenance");
    }
  }

  const std::optional<HostedProgramEntryPlan> &semanticHosted =
      semantics.hostedProgramEntryPlan();
  const std::optional<HirHostedProgramEntryPlan> &hosted =
      program.hostedProgramEntryPlan();
  if (semanticHosted.has_value() != hosted.has_value()) {
    result.errors.emplace_back(
        "hosted program-entry plan presence differs from semantics");
    return result;
  }
  if (!semanticHosted) {
    return result;
  }
  if (hosted->semanticEntry != semanticHosted->entry ||
      hosted->semanticAppendFunction != semanticHosted->appendFunction ||
      hosted->semanticVectorConstructor != semanticHosted->vectorConstructor ||
      hosted->semanticStringConstructor != semanticHosted->stringConstructor ||
      hosted->kind != semanticHosted->kind ||
      hosted->sourceUnit != semanticHosted->sourceUnit ||
      !sameSpan(hosted->mainAnchor, semanticHosted->mainAnchor) ||
      hosted->validateCount != semanticHosted->validateCount ||
      hosted->convertCount != semanticHosted->convertCount) {
    result.errors.emplace_back(
        "hosted HIR provenance differs from the semantic entry plan");
  }
  const HirFunctionInstance *entry =
      program.findFunctionInstance(hosted->entry);
  const FunctionInfo *semanticEntry =
      semantics.findFunction(semanticHosted->entry);
  if (entry == nullptr || entry->declaration != semanticHosted->entry ||
      semanticEntry == nullptr || entry->source != semanticEntry->declaration ||
      entry->entryKind != semanticHosted->kind || entry->owner ||
      !entry->typeArguments.empty() || !entry->valueArguments.empty() ||
      entry->returnType != SemanticType::Int32 ||
      entry->parameterTypes != semanticEntry->parameterTypes ||
      entry->source == nullptr ||
      !sameSpan(hosted->mainAnchor, tokenSpan(entry->source->name()))) {
    result.errors.emplace_back(
        "hosted entry plan references the wrong HIR function instance");
  }
  if (semanticHosted->kind == ProgramEntryKind::NoArguments) {
    if (hosted->appendFunction != 0 || hosted->vectorConstructor != 0 ||
        hosted->stringConstructor != 0 ||
        (entry != nullptr && entry->entryArgumentAppendTarget) ||
        semanticEntry == nullptr || !semanticEntry->parameterTypes.empty() ||
        !hosted->validateCount.empty() || !hosted->convertCount.empty()) {
      result.errors.emplace_back(
          "no-argument hosted entry contains owned-argument operations");
    }
    return result;
  }
  const HirFunctionInstance *append =
      program.findFunctionInstance(hosted->appendFunction);
  const HirConstructorInstance *vectorConstructor =
      program.findConstructorInstance(hosted->vectorConstructor);
  const HirConstructorInstance *stringConstructor =
      program.findConstructorInstance(hosted->stringConstructor);
  const SemanticType vectorType =
      semanticEntry != nullptr && semanticEntry->parameterTypes.size() == 2
          ? semanticEntry->parameterTypes[1]
          : SemanticType::Unknown;
  const SemanticType stringType =
      vectorType.kind == SemanticType::Class && vectorType.arguments.size() == 1
          ? vectorType.arguments.front()
          : SemanticType::Unknown;
  const auto exactOwner = [&](HirClassInstanceId owner,
                              const SemanticType &type) {
    return owner != 0 && owner <= program.classInstances().size() &&
           program.classInstances()[owner - 1].type == type &&
           program.classInstances()[owner - 1].typeArguments ==
               type.arguments &&
           program.classInstances()[owner - 1].valueArguments ==
               type.valueArguments;
  };
  const bool exactFailures =
      exactHostedFailure(hosted->validateCount,
                         DefinedFailureCode::HostedRuntimeContractFailure,
                         DefinedFailureDetail::NegativeArgumentCount,
                         hosted->sourceUnit, hosted->mainAnchor) &&
      exactHostedFailure(hosted->convertCount,
                         DefinedFailureCode::NumericConversionOutOfRange,
                         DefinedFailureDetail::HostedArgumentCount,
                         hosted->sourceUnit, hosted->mainAnchor);
  const bool exactAppend =
      entry != nullptr && entry->entryArgumentAppendTarget &&
      *entry->entryArgumentAppendTarget == hosted->appendFunction &&
      append != nullptr &&
      append->declaration == semanticHosted->appendFunction && append->owner &&
      exactOwner(*append->owner, vectorType) && append->typeArguments.empty() &&
      append->valueArguments.empty() &&
      append->returnType == SemanticType::Void &&
      append->parameterTypes == std::vector<SemanticType>{stringType} &&
      !append->staticMember;
  const bool exactVectorConstructor =
      vectorConstructor != nullptr &&
      vectorConstructor->declaration == semanticHosted->vectorConstructor &&
      exactOwner(vectorConstructor->owner, vectorType) &&
      vectorConstructor->typeArguments.empty() &&
      vectorConstructor->valueArguments.empty() &&
      vectorConstructor->parameterTypes.empty();
  const bool exactStringConstructor =
      stringConstructor != nullptr &&
      stringConstructor->declaration == semanticHosted->stringConstructor &&
      exactOwner(stringConstructor->owner, stringType) &&
      stringConstructor->typeArguments.empty() &&
      stringConstructor->valueArguments.empty() &&
      stringConstructor->parameterTypes ==
          std::vector<SemanticType>{SemanticType::StringView};
  if (append == nullptr || vectorConstructor == nullptr ||
      stringConstructor == nullptr || vectorType.kind != SemanticType::Class ||
      stringType.kind != SemanticType::Class || !exactAppend ||
      !exactVectorConstructor || !exactStringConstructor || !exactFailures) {
    result.errors.emplace_back(
        "owned-argument hosted entry lacks exact HIR startup targets");
  }
  return result;
}

} // namespace lang
