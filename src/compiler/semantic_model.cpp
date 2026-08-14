#include "gti/semantic_analyzer.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <utility>

namespace lang {

[[nodiscard]] const std::vector<SymbolRecord> &
SemanticDatabase::symbols() const {
  return symbolRecords;
}

[[nodiscard]] const SymbolRecord *
SemanticDatabase::findSymbol(SymbolId id) const {
  if (id == 0) {
    return nullptr;
  }
  if (base != nullptr && id <= baseSymbolCount) {
    return base->findSymbol(id);
  }
  const SymbolId local = id - baseSymbolCount;
  return local > symbolRecords.size() ? nullptr : &symbolRecords[local - 1];
}

[[nodiscard]] std::vector<const SemanticOccurrence *>
SemanticDatabase::occurrencesForSymbol(SymbolId id) const {
  std::vector<const SemanticOccurrence *> result;
  if (id == 0) {
    return result;
  }
  for (const auto &[_, unitOccurrences] : occurrencesByUnit) {
    for (const SemanticOccurrence &occurrence : unitOccurrences) {
      if (occurrence.symbol == id) {
        result.push_back(&occurrence);
      }
    }
  }
  return result;
}

[[nodiscard]] const std::vector<SemanticOccurrence> &
SemanticDatabase::occurrences(SourceUnitId sourceUnit) const {
  static const std::vector<SemanticOccurrence> empty;
  const auto found = occurrencesByUnit.find(sourceUnit);
  return found == occurrencesByUnit.end() ? empty : found->second;
}

[[nodiscard]] const SemanticOccurrence *
SemanticDatabase::findOccurrence(SourceUnitId sourceUnit,
                                 std::size_t byteOffset) const {
  const std::vector<SemanticOccurrence> &unitOccurrences =
      occurrences(sourceUnit);
  const auto after = std::upper_bound(
      unitOccurrences.begin(), unitOccurrences.end(), byteOffset,
      [](std::size_t offset, const SemanticOccurrence &occurrence) {
        return offset < occurrence.span.start;
      });
  if (after == unitOccurrences.begin()) {
    return nullptr;
  }

  auto current = after;
  --current;
  const std::size_t candidateStart = current->span.start;
  const SemanticOccurrence *best = nullptr;
  while (true) {
    if (current->span.start != candidateStart) {
      break;
    }
    if (current->span.start <= byteOffset && byteOffset < current->span.end &&
        (best == nullptr || priority(current->kind) > priority(best->kind))) {
      best = &*current;
    }
    if (current == unitOccurrences.begin()) {
      break;
    }
    --current;
  }
  return best;
}

[[nodiscard]] const SymbolRecord *
SemanticDatabase::findSymbolAt(SourceUnitId sourceUnit,
                               std::size_t byteOffset) const {
  const SemanticOccurrence *occurrence = findOccurrence(sourceUnit, byteOffset);
  return occurrence == nullptr ? nullptr : findSymbol(occurrence->symbol);
}

int SemanticDatabase::priority(SemanticOccurrenceKind kind) {
  switch (kind) {
  case SemanticOccurrenceKind::SelectedCall:
  case SemanticOccurrenceKind::SelectedConstruction:
    return 4;
  case SemanticOccurrenceKind::Function:
  case SemanticOccurrenceKind::Symbol:
  case SemanticOccurrenceKind::ClassType:
  case SemanticOccurrenceKind::EnumType:
  case SemanticOccurrenceKind::TypeAlias:
  case SemanticOccurrenceKind::Constructor:
  case SemanticOccurrenceKind::Destructor:
    return 3;
  case SemanticOccurrenceKind::Binding:
  case SemanticOccurrenceKind::InferredType:
    return 2;
  case SemanticOccurrenceKind::Expression:
    return 1;
  }
  return 0;
}

void SemanticDatabase::clear() {
  symbolRecords.clear();
  symbolsByDeclaration.clear();
  occurrencesByUnit.clear();
  base = nullptr;
  baseSymbolCount = 0;
}

// Turns this database into an instance-analysis delta over baseDatabase:
// lookups fall back to the base, and new symbol identities continue after
// the base's so instance records never collide with base SymbolIds.
void SemanticDatabase::beginInstanceDelta(
    const SemanticDatabase &baseDatabase) {
  clear();
  base = &baseDatabase;
  baseSymbolCount = baseDatabase.symbolRecords.size();
}

void SemanticDatabase::rebase(const SemanticDatabase *baseDatabase) {
  base = baseDatabase;
}

SymbolId SemanticDatabase::recordSymbol(SymbolRecord symbol) {
  if (symbol.sourceUnit == 0 || symbol.nameSpan.end <= symbol.nameSpan.start) {
    return 0;
  }
  const DeclarationKey key{symbol.sourceUnit, symbol.nameSpan.start,
                           symbol.nameSpan.end,
                           symbol.generated ? symbol.name : std::string{}};
  if (const auto found = symbolsByDeclaration.find(key);
      found != symbolsByDeclaration.end()) {
    SymbolRecord &existing = symbolRecords[found->second - 1];
    if (existing.qualifiedName.empty()) {
      existing.qualifiedName = std::move(symbol.qualifiedName);
    }
    if (existing.type == SemanticType::Unknown &&
        symbol.type != SemanticType::Unknown) {
      existing.type = std::move(symbol.type);
      existing.traits = symbol.traits;
    }
    if (symbol.definitionSpan) {
      existing.definitionSpan = std::move(symbol.definitionSpan);
    }
    existing.access = symbol.access;
    existing.mutableBinding = symbol.mutableBinding;
    existing.defaultLibrary = existing.defaultLibrary || symbol.defaultLibrary;
    existing.staticMember = existing.staticMember || symbol.staticMember;
    existing.internalLinkage =
        existing.internalLinkage || symbol.internalLinkage;
    existing.compilerPrivate =
        existing.compilerPrivate || symbol.compilerPrivate;
    return found->second;
  }

  if (base != nullptr) {
    if (const auto inherited = base->symbolsByDeclaration.find(key);
        inherited != base->symbolsByDeclaration.end()) {
      // The declaration already has a base identity; instance analysis
      // reuses it and skips base-record enrichment (the delta is
      // discarded after lowering, so enrichment would be invisible).
      return inherited->second;
    }
  }

  symbol.id = baseSymbolCount + symbolRecords.size() + 1;
  const SymbolId id = symbol.id;
  symbolRecords.emplace_back(std::move(symbol));
  symbolsByDeclaration.emplace(key, id);
  return id;
}

[[nodiscard]] SymbolId
SemanticDatabase::symbolForDeclaration(SourceUnitId sourceUnit,
                                       const SourceSpan &span,
                                       std::string_view generatedName) const {
  const auto found = symbolsByDeclaration.find(DeclarationKey{
      sourceUnit, span.start, span.end, std::string(generatedName)});
  if (found != symbolsByDeclaration.end()) {
    return found->second;
  }
  return base == nullptr
             ? 0
             : base->symbolForDeclaration(sourceUnit, span, generatedName);
}

void SemanticDatabase::record(SemanticOccurrence occurrence) {
  if (!toolingOccurrences || occurrence.sourceUnit == 0 ||
      occurrence.span.end <= occurrence.span.start) {
    return;
  }
  occurrencesByUnit[occurrence.sourceUnit].push_back(std::move(occurrence));
}

void SemanticDatabase::setToolingOccurrencesEnabled(bool enabled) {
  toolingOccurrences = enabled;
}

void SemanticDatabase::finalize() {
  for (auto &[_, unitOccurrences] : occurrencesByUnit) {
    std::stable_sort(
        unitOccurrences.begin(), unitOccurrences.end(),
        [](const SemanticOccurrence &left, const SemanticOccurrence &right) {
          if (left.span.start != right.span.start) {
            return left.span.start < right.span.start;
          }
          if (left.span.end != right.span.end) {
            return left.span.end < right.span.end;
          }
          return priority(left.kind) < priority(right.kind);
        });
    std::vector<SemanticOccurrence> compacted;
    compacted.reserve(unitOccurrences.size());
    for (SemanticOccurrence &occurrence : unitOccurrences) {
      if (!compacted.empty() && occurrence.symbol != 0 &&
          compacted.back().span.start == occurrence.span.start &&
          compacted.back().span.end == occurrence.span.end &&
          compacted.back().kind == occurrence.kind &&
          compacted.back().symbol == occurrence.symbol) {
        compacted.back().roles |= occurrence.roles;
        continue;
      }
      compacted.emplace_back(std::move(occurrence));
    }
    unitOccurrences = std::move(compacted);
  }
}

[[nodiscard]] ExecutionProfile SemanticModel::executionProfile() const {
  return executionProfile_;
}

[[nodiscard]] std::size_t SemanticModel::placeSnapshot() const {
  return placeSnapshot_;
}

// AST identities remain valid while the analyzed Program is alive.
[[nodiscard]] const ExpressionInfo *
SemanticModel::findExpression(const Expr &expression) const {
  const auto found = expressions.find(&expression);
  if (found != expressions.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findExpression(expression);
}

[[nodiscard]] UnsafeOperationKind
SemanticModel::unsafeOperation(const Expr &expression) const {
  const auto found = unsafeOperations.find(&expression);
  if (found != unsafeOperations.end()) {
    return found->second;
  }
  return base == nullptr ? UnsafeOperationKind::None
                         : base->unsafeOperation(expression);
}

[[nodiscard]] const DefinedFailureOperation *
SemanticModel::findDefinedFailure(const Expr &expression) const {
  const auto found = definedFailures.find(&expression);
  if (found != definedFailures.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findDefinedFailure(expression);
}

[[nodiscard]] const PlaceKey *
SemanticModel::findPlace(const Expr &expression) const {
  const auto found = places.find(&expression);
  if (found != places.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findPlace(expression);
}

[[nodiscard]] const OwnershipEvent *
SemanticModel::findOwnershipEvent(const Expr &expression) const {
  const auto found = ownershipEvents.find(&expression);
  if (found != ownershipEvents.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findOwnershipEvent(expression);
}

[[nodiscard]] std::optional<LambdaCaptureMode>
SemanticModel::lambdaCaptureMode(SymbolId symbol) const {
  const auto found = lambdaCaptureModes.find(symbol);
  if (found != lambdaCaptureModes.end()) {
    return found->second;
  }
  return base == nullptr ? std::nullopt : base->lambdaCaptureMode(symbol);
}

[[nodiscard]] std::size_t
SemanticModel::placeSelection(const Expr &expression) const {
  const auto found = placeSelections.find(&expression);
  if (found != placeSelections.end()) {
    return found->second;
  }
  return base == nullptr ? 0 : base->placeSelection(expression);
}

[[nodiscard]] std::size_t SemanticModel::ownershipEventCount() const {
  return ownershipEventOrder.size();
}

[[nodiscard]] CompilerCapabilityTypeKind
SemanticModel::compilerCapabilityType(const TypeRef &type) const {
  const auto found = compilerCapabilityTypes.find(&type);
  if (found != compilerCapabilityTypes.end()) {
    return found->second;
  }
  return base == nullptr ? CompilerCapabilityTypeKind::None
                         : base->compilerCapabilityType(type);
}

[[nodiscard]] bool
SemanticModel::isCompilerPrivateType(const SemanticType &type) const {
  switch (type.kind) {
  case SemanticType::UniqueOwner:
  case SemanticType::Storage:
    return true;
  case SemanticType::Class: {
    const ClassTypeInfo *info = findClassType(type.classId);
    if (info != nullptr && info->compilerPrivate) {
      return true;
    }
    break;
  }
  case SemanticType::Enum: {
    const EnumTypeInfo *info = findEnumType(type.enumId);
    if (info != nullptr && info->compilerPrivate) {
      return true;
    }
    break;
  }
  default:
    break;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     [this](const SemanticType &argument) {
                       return isCompilerPrivateType(argument);
                     });
}

[[nodiscard]] bool
SemanticModel::canPresent(SourceUnitId requester, const SymbolRecord &symbol,
                          const SourceGraph &sourceGraph) const {
  return sourceGraph.isCompilerTrusted(requester) ||
         (!symbol.compilerPrivate && !isCompilerPrivateType(symbol.type));
}

[[nodiscard]] bool
SemanticModel::canPresent(SourceUnitId requester,
                          const SemanticOccurrence &occurrence,
                          const SourceGraph &sourceGraph) const {
  if (sourceGraph.isCompilerTrusted(requester)) {
    return true;
  }
  if (isCompilerPrivateType(occurrence.type)) {
    return false;
  }
  const SymbolRecord *symbol = semanticDatabase.findSymbol(occurrence.symbol);
  return symbol == nullptr || canPresent(requester, *symbol, sourceGraph);
}

[[nodiscard]] ExpressionInfo
SemanticModel::expressionInfo(const Expr &expression) const {
  const ExpressionInfo *info = findExpression(expression);
  return info == nullptr ? makeExpressionInfo(SemanticType::Unknown) : *info;
}

[[nodiscard]] const SemanticType *
SemanticModel::findType(const Expr &expression) const {
  const ExpressionInfo *info = findExpression(expression);
  return info == nullptr ? nullptr : &info->type;
}

[[nodiscard]] std::optional<ConstantValue>
SemanticModel::findConstant(const Expr &expression) const {
  const auto found = constants.find(&expression);
  if (found != constants.end()) {
    return found->second;
  }
  return base == nullptr ? std::nullopt : base->findConstant(expression);
}

[[nodiscard]] const CompileTimeValue *
SemanticModel::findArrayExtent(const ArrayExtentExpr &extent) const {
  const auto found = arrayExtents.find(&extent);
  if (found != arrayExtents.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findArrayExtent(extent);
}

[[nodiscard]] SemanticType SemanticModel::typeOf(const Expr &expression) const {
  const SemanticType *type = findType(expression);
  return type == nullptr ? SemanticType::Unknown : *type;
}

[[nodiscard]] bool SemanticModel::hasType(const Expr &expression) const {
  return expressions.contains(&expression) ||
         (base != nullptr && base->hasType(expression));
}

[[nodiscard]] std::size_t SemanticModel::expressionCount() const {
  return expressions.size() + (base == nullptr ? 0 : base->expressionCount());
}

[[nodiscard]] const std::vector<SemanticFullExpression> &
SemanticModel::fullExpressionsFor(const Stmt &statement) const {
  const auto found = statementFullExpressions.find(&statement);
  if (found != statementFullExpressions.end()) {
    return found->second;
  }
  if (base != nullptr) {
    return base->fullExpressionsFor(statement);
  }
  static const std::vector<SemanticFullExpression> empty;
  return empty;
}

[[nodiscard]] const std::vector<SemanticFullExpression> &
SemanticModel::fullExpressionsFor(
    const ConstructorInitializer &initializer) const {
  const auto found = constructorFullExpressions.find(&initializer);
  if (found != constructorFullExpressions.end()) {
    return found->second;
  }
  if (base != nullptr) {
    return base->fullExpressionsFor(initializer);
  }
  static const std::vector<SemanticFullExpression> empty;
  return empty;
}

[[nodiscard]] const std::vector<SemanticFullExpression> &
SemanticModel::fullExpressions() const {
  return fullExpressionOrder;
}

[[nodiscard]] const BindingInfo *
SemanticModel::findBinding(const VariableDecl &declaration) const {
  const auto found = variableBindings.find(&declaration);
  if (found != variableBindings.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findBinding(declaration);
}

[[nodiscard]] const BindingInfo *
SemanticModel::findBinding(const Parameter &parameter) const {
  const auto found = parameterBindings.find(&parameter);
  if (found != parameterBindings.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findBinding(parameter);
}

[[nodiscard]] std::size_t SemanticModel::bindingCount() const {
  return variableBindings.size() + parameterBindings.size() +
         payloadBindings.size() + (base == nullptr ? 0 : base->bindingCount());
}

[[nodiscard]] const BindingInfo *
SemanticModel::findPayloadBinding(const Token &name) const {
  const auto found = payloadBindings.find(&name);
  if (found != payloadBindings.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findPayloadBinding(name);
}

[[nodiscard]] const SemanticLoanInfo *
SemanticModel::findLoan(SemanticLoanId id) const {
  if (id == 0 || id > retainedLoans.size()) {
    return nullptr;
  }
  const SemanticLoanInfo &loan = retainedLoans[id - 1];
  return loan.id == id ? &loan : nullptr;
}

[[nodiscard]] const std::vector<SemanticLoanInfo> &
SemanticModel::loans() const {
  return retainedLoans;
}

[[nodiscard]] std::vector<SemanticLoanId>
SemanticModel::loansEndingAfter(const Stmt &statement) const {
  const auto found = loanEnds.find(&statement);
  return found == loanEnds.end() ? std::vector<SemanticLoanId>{}
                                 : found->second;
}

[[nodiscard]] std::vector<SemanticLoanId>
SemanticModel::loansEndingAtConditionalEntry(const IfStmt &statement,
                                             bool thenBranch) const {
  const auto found = conditionalLoanEnds.find(&statement);
  if (found == conditionalLoanEnds.end()) {
    return {};
  }
  return thenBranch ? found->second.thenEntry : found->second.elseEntry;
}

[[nodiscard]] std::optional<bool>
SemanticModel::findConstexprBranch(const IfStmt &statement) const {
  const auto found = constexprBranches.find(&statement);
  if (found != constexprBranches.end()) {
    return found->second;
  }
  return base == nullptr ? std::nullopt : base->findConstexprBranch(statement);
}

[[nodiscard]] std::vector<SemanticLoanId>
SemanticModel::loansEndingAtSwitchArmEntry(const SwitchStmt &statement,
                                           std::size_t armIndex) const {
  const auto found = switchArmLoanEnds.find(&statement);
  if (found == switchArmLoanEnds.end() || armIndex >= found->second.size()) {
    return {};
  }
  return found->second[armIndex];
}

[[nodiscard]] const StructuredBindingInfo *SemanticModel::findStructuredBinding(
    const StructuredBindingDecl &declaration) const {
  const auto found = structuredBindings.find(&declaration);
  if (found != structuredBindings.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findStructuredBinding(declaration);
}

[[nodiscard]] const FunctionInfo *
SemanticModel::findFunction(const FunctionDecl &declaration) const {
  const auto found = functions.find(&declaration);
  if (found != functions.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findFunction(declaration);
}

[[nodiscard]] const FunctionInfo *
SemanticModel::findFunction(FunctionId id) const {
  const auto found = functionsById.find(id);
  if (found != functionsById.end() && found->second != nullptr) {
    return findFunction(*found->second);
  }
  return base == nullptr ? nullptr : base->findFunction(id);
}

[[nodiscard]] const LambdaInfo *
SemanticModel::findLambda(const Lambda &declaration) const {
  const auto found = lambdas.find(&declaration);
  if (found != lambdas.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findLambda(declaration);
}

[[nodiscard]] const LambdaInfo *SemanticModel::findLambda(LambdaId id) const {
  const auto found = lambdasById.find(id);
  if (found != lambdasById.end() && found->second != nullptr) {
    return findLambda(*found->second);
  }
  return base == nullptr ? nullptr : base->findLambda(id);
}

[[nodiscard]] const ClassTypeInfo *
SemanticModel::findClassType(const ClassDecl &declaration) const {
  const auto found = classTypes.find(&declaration);
  if (found != classTypes.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findClassType(declaration);
}

[[nodiscard]] const ClassTypeInfo *
SemanticModel::findClassType(ClassId id) const {
  const auto found = classTypesById.find(id);
  if (found != classTypesById.end() && found->second != nullptr) {
    return findClassType(*found->second);
  }
  return base == nullptr ? nullptr : base->findClassType(id);
}

[[nodiscard]] const TypeAliasInfo *
SemanticModel::findTypeAlias(const TypeAliasDecl &declaration) const {
  const auto found = typeAliases.find(&declaration);
  if (found != typeAliases.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findTypeAlias(declaration);
}

[[nodiscard]] const EnumTypeInfo *
SemanticModel::findEnumType(const EnumDecl &declaration) const {
  const auto found = enumTypes.find(&declaration);
  if (found != enumTypes.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findEnumType(declaration);
}

[[nodiscard]] const EnumTypeInfo *SemanticModel::findEnumType(EnumId id) const {
  const auto found = enumTypesById.find(id);
  if (found != enumTypesById.end() && found->second != nullptr) {
    return findEnumType(*found->second);
  }
  return base == nullptr ? nullptr : base->findEnumType(id);
}

[[nodiscard]] const ResolvedEnumeratorInfo *
SemanticModel::findEnumerator(const QualifiedName &expression) const {
  const auto found = enumerators.find(&expression);
  if (found != enumerators.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findEnumerator(expression);
}

[[nodiscard]] const SwitchCaseValue *
SemanticModel::findSwitchCase(const Expr &expression) const {
  const auto found = switchCases.find(&expression);
  if (found != switchCases.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findSwitchCase(expression);
}

[[nodiscard]] bool
SemanticModel::isExhaustiveSwitch(const SwitchStmt &statement) const {
  return exhaustiveSwitches.contains(&statement) ||
         (base != nullptr && base->isExhaustiveSwitch(statement));
}

[[nodiscard]] const ResolvedPayloadConstructionInfo *
SemanticModel::findPayloadConstruction(const Call &call) const {
  const auto found = payloadConstructions.find(&call);
  if (found != payloadConstructions.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findPayloadConstruction(call);
}

[[nodiscard]] const ResolvedPayloadPatternInfo *
SemanticModel::findPayloadPattern(const Expr &expression) const {
  const auto found = payloadPatterns.find(&expression);
  if (found != payloadPatterns.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findPayloadPattern(expression);
}

[[nodiscard]] const ResolvedCallInfo *
SemanticModel::findCall(const Call &call) const {
  const auto found = calls.find(&call);
  if (found != calls.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findCall(call);
}

[[nodiscard]] const ResolvedPackFoldInfo *
SemanticModel::findPackFold(const PackFold &fold) const {
  const auto found = packFolds.find(&fold);
  if (found != packFolds.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findPackFold(fold);
}

[[nodiscard]] const ResolvedLambdaCallInfo *
SemanticModel::findLambdaCall(const Call &call) const {
  const auto found = lambdaCalls.find(&call);
  if (found != lambdaCalls.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findLambdaCall(call);
}

[[nodiscard]] const DeferredCallableCallInfo *
SemanticModel::findDeferredCallableCall(const Call &call) const {
  const auto found = deferredCallableCalls.find(&call);
  if (found != deferredCallableCalls.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findDeferredCallableCall(call);
}

[[nodiscard]] const ResolvedOperatorInfo *
SemanticModel::findOperator(const Expr &expression) const {
  const auto found = operators.find(&expression);
  if (found != operators.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findOperator(expression);
}

[[nodiscard]] const ResolvedOperatorInfo *
SemanticModel::findContextualConversion(const Expr &expression) const {
  const auto found = contextualConversions.find(&expression);
  if (found != contextualConversions.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findContextualConversion(expression);
}

[[nodiscard]] bool
SemanticModel::isContextualIntegerOperand(const Expr &expression) const {
  if (contextualIntegerOperands.contains(&expression)) {
    return true;
  }
  return base != nullptr && base->isContextualIntegerOperand(expression);
}

[[nodiscard]] const ClassLifecycleInfo *
SemanticModel::findClassLifecycle(const ClassDecl &declaration) const {
  const auto found = classLifecycles.find(&declaration);
  if (found != classLifecycles.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findClassLifecycle(declaration);
}

[[nodiscard]] const ResolvedConstructionInfo *
SemanticModel::findConstruction(const Expr &expression) const {
  const auto found = constructions.find(&expression);
  if (found != constructions.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findConstruction(expression);
}

[[nodiscard]] const ResolvedConstructorInitializerInfo *
SemanticModel::findConstructorInitializer(
    const ConstructorInitializer &initializer) const {
  const auto found = constructorInitializers.find(&initializer);
  if (found != constructorInitializers.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr
                         : base->findConstructorInitializer(initializer);
}

[[nodiscard]] SymbolId
SemanticModel::findResolvedSymbol(const Expr &expression) const {
  const auto found = resolvedSymbols.find(&expression);
  if (found != resolvedSymbols.end()) {
    return found->second;
  }
  return base == nullptr ? 0 : base->findResolvedSymbol(expression);
}

[[nodiscard]] std::size_t SemanticModel::functionCount() const {
  return functions.size() + (base == nullptr ? 0 : base->functionCount());
}

[[nodiscard]] std::size_t SemanticModel::lambdaCount() const {
  return lambdas.size() + (base == nullptr ? 0 : base->lambdaCount());
}

[[nodiscard]] std::size_t SemanticModel::resolvedCallCount() const {
  return calls.size() + (base == nullptr ? 0 : base->resolvedCallCount());
}

[[nodiscard]] std::size_t SemanticModel::classLifecycleCount() const {
  return classLifecycles.size() +
         (base == nullptr ? 0 : base->classLifecycleCount());
}

[[nodiscard]] std::size_t SemanticModel::resolvedConstructionCount() const {
  return constructions.size() +
         (base == nullptr ? 0 : base->resolvedConstructionCount());
}

[[nodiscard]] const SemanticDatabase &SemanticModel::database() const {
  return semanticDatabase;
}

[[nodiscard]] const std::optional<SemanticCompletionContext> &
SemanticModel::completionContext() const {
  return completion;
}

[[nodiscard]] const GenericParameterInfo *
SemanticModel::findGenericParameter(GenericParameterId id) const {
  const auto find = [id](const auto &records) -> const GenericParameterInfo * {
    for (const auto &[_, record] : records) {
      const auto parameter = std::find_if(
          record.genericParameters.begin(), record.genericParameters.end(),
          [id](const GenericParameterInfo &candidate) {
            return candidate.id == id;
          });
      if (parameter != record.genericParameters.end()) {
        return &*parameter;
      }
    }
    return nullptr;
  };
  if (const GenericParameterInfo *parameter = find(functions)) {
    return parameter;
  }
  if (const GenericParameterInfo *parameter = find(classTypes)) {
    return parameter;
  }
  for (const auto &[_, lifecycle] : classLifecycles) {
    for (const ConstructorInfo &constructor : lifecycle.constructors) {
      const auto parameter =
          std::find_if(constructor.genericParameters.begin(),
                       constructor.genericParameters.end(),
                       [id](const GenericParameterInfo &candidate) {
                         return candidate.id == id;
                       });
      if (parameter != constructor.genericParameters.end()) {
        return &*parameter;
      }
    }
  }
  return base == nullptr ? nullptr : base->findGenericParameter(id);
}

[[nodiscard]] const ConstructorInfo *
SemanticModel::findConstructor(const ConstructorDecl &declaration) const {
  for (const auto &[_, lifecycle] : classLifecycles) {
    const auto found = std::find_if(
        lifecycle.constructors.begin(), lifecycle.constructors.end(),
        [&declaration](const ConstructorInfo &candidate) {
          return candidate.declaration == &declaration;
        });
    if (found != lifecycle.constructors.end()) {
      return &*found;
    }
    if (lifecycle.declaredCopyConstructor &&
        lifecycle.declaredCopyConstructor->declaration == &declaration) {
      return &*lifecycle.declaredCopyConstructor;
    }
    if (lifecycle.declaredMoveConstructor &&
        lifecycle.declaredMoveConstructor->declaration == &declaration) {
      return &*lifecycle.declaredMoveConstructor;
    }
  }
  return base == nullptr ? nullptr : base->findConstructor(declaration);
}

[[nodiscard]] const DestructorInfo *
SemanticModel::findDestructor(const DestructorDecl &declaration) const {
  for (const auto &[_, lifecycle] : classLifecycles) {
    if (lifecycle.declaredDestructor &&
        lifecycle.declaredDestructor->declaration == &declaration) {
      return &*lifecycle.declaredDestructor;
    }
  }
  return base == nullptr ? nullptr : base->findDestructor(declaration);
}

void SemanticModel::clear() {
  expressions.clear();
  statementFullExpressions.clear();
  constructorFullExpressions.clear();
  fullExpressionOrder.clear();
  constants.clear();
  unsafeOperations.clear();
  definedFailures.clear();
  places.clear();
  ownershipEvents.clear();
  ownershipEventOrder.clear();
  placeSelections.clear();
  compilerCapabilityTypes.clear();
  arrayExtents.clear();
  variableBindings.clear();
  parameterBindings.clear();
  payloadBindings.clear();
  retainedLoans.clear();
  loanEnds.clear();
  conditionalLoanEnds.clear();
  constexprBranches.clear();
  switchArmLoanEnds.clear();
  structuredBindings.clear();
  functions.clear();
  functionsById.clear();
  lambdas.clear();
  lambdasById.clear();
  classTypes.clear();
  classTypesById.clear();
  typeAliases.clear();
  enumTypes.clear();
  enumTypesById.clear();
  enumerators.clear();
  switchCases.clear();
  exhaustiveSwitches.clear();
  payloadConstructions.clear();
  payloadPatterns.clear();
  calls.clear();
  packFolds.clear();
  lambdaCalls.clear();
  deferredCallableCalls.clear();
  pendingCallableForwardings.clear();
  operators.clear();
  contextualConversions.clear();
  contextualIntegerOperands.clear();
  classLifecycles.clear();
  constructions.clear();
  constructorInitializers.clear();
  resolvedSymbols.clear();
  semanticDatabase.clear();
  completion.reset();
  executionProfile_ = ExecutionProfile::SingleThreaded;
  placeSnapshot_ = 0;
  base = nullptr;
}

void SemanticModel::setExecutionProfile(ExecutionProfile profile) {
  executionProfile_ = profile;
}

void SemanticModel::setPlaceSnapshot(std::size_t snapshot) {
  placeSnapshot_ = snapshot;
}

// Turns this model into an instance-analysis delta over baseModel: reads
// fall back to the base while writes stay local, so concrete instance
// reanalysis records only what it produces instead of copying the whole
// program's model. Loan tables deliberately do not fall back - instance
// analysis restarts loan identities, mirroring the clearLoans() semantics
// the previous whole-model copy relied on.
void SemanticModel::beginInstanceDelta(const SemanticModel &baseModel) {
  clear();
  executionProfile_ = baseModel.executionProfile();
  placeSnapshot_ = baseModel.placeSnapshot();
  base = &baseModel;
  semanticDatabase.beginInstanceDelta(baseModel.semanticDatabase);
}

// Re-points an instance delta at a relocated base (the analyzer restores
// its model after each instance analysis; the delta must follow it).
void SemanticModel::rebase(const SemanticModel *baseModel) {
  base = baseModel;
  semanticDatabase.rebase(baseModel == nullptr ? nullptr
                                               : &baseModel->semanticDatabase);
}

// Copies a base function record into the delta so record mutators can
// update it locally. Returns the local entry, or functions.end() when the
// declaration is unknown to both the delta and the base.
[[nodiscard]] std::unordered_map<const FunctionDecl *, FunctionInfo>::iterator
SemanticModel::materializeFunction(const FunctionDecl &declaration) {
  auto local = functions.find(&declaration);
  if (local != functions.end() || base == nullptr) {
    return local;
  }
  const FunctionInfo *inherited = base->findFunction(declaration);
  if (inherited == nullptr) {
    return functions.end();
  }
  local = functions.insert_or_assign(&declaration, *inherited).first;
  functionsById.insert_or_assign(local->second.id, &declaration);
  return local;
}

[[nodiscard]] bool SemanticModel::validLoan(SemanticLoanId id) const {
  return id != 0 && id <= retainedLoans.size() &&
         retainedLoans[id - 1].id == id;
}

void SemanticModel::appendUniqueLoan(std::vector<SemanticLoanId> &loans,
                                     SemanticLoanId loan) {
  if (std::find(loans.begin(), loans.end(), loan) == loans.end()) {
    loans.push_back(loan);
  }
}

void SemanticModel::recordLoanEndpoint(SemanticLoanId id,
                                       SemanticLoanEndKind kind,
                                       const Stmt &statement,
                                       std::size_t switchArm) {
  std::vector<SemanticLoanEndpoint> &endpoints =
      retainedLoans[id - 1].endpoints;
  const auto duplicate =
      std::find_if(endpoints.begin(), endpoints.end(),
                   [&](const SemanticLoanEndpoint &endpoint) {
                     return endpoint.kind == kind &&
                            endpoint.statement == &statement &&
                            (kind != SemanticLoanEndKind::SwitchArmEntry ||
                             endpoint.switchArm == switchArm);
                   });
  if (duplicate == endpoints.end()) {
    endpoints.push_back(
        {.kind = kind, .statement = &statement, .switchArm = switchArm});
  }
}

void SemanticModel::record(const Expr &expression, ExpressionInfo info) {
  expressions.insert_or_assign(&expression, std::move(info));
}

void SemanticModel::recordDefinedFailure(const Expr &expression,
                                         DefinedFailureOperation operation) {
  if (operation.empty()) {
    definedFailures.erase(&expression);
    return;
  }
  definedFailures.insert_or_assign(&expression, std::move(operation));
}

bool SemanticModel::appendFullExpression(
    std::vector<SemanticFullExpression> &expressions,
    const SemanticFullExpression &expression) {
  if (expression.roots.empty()) {
    return false;
  }
  const auto duplicate =
      std::find_if(expressions.begin(), expressions.end(),
                   [&](const SemanticFullExpression &candidate) {
                     return candidate.roots == expression.roots;
                   });
  if (duplicate == expressions.end()) {
    expressions.push_back(expression);
    return true;
  }
  return false;
}

void SemanticModel::recordFullExpression(const Stmt &statement,
                                         const ExprPtr &root) {
  if (root == nullptr) {
    return;
  }
  SemanticFullExpression expression{.order = fullExpressionOrder.size() + 1,
                                    .statement = &statement,
                                    .roots = {root.get()}};
  if (appendFullExpression(statementFullExpressions[&statement], expression)) {
    fullExpressionOrder.push_back(std::move(expression));
  }
}

void SemanticModel::recordFullExpression(
    const ConstructorInitializer &initializer) {
  SemanticFullExpression expression{.order = fullExpressionOrder.size() + 1,
                                    .constructorInitializer = &initializer};
  expression.roots.reserve(initializer.arguments.size());
  for (const ExprPtr &argument : initializer.arguments) {
    if (argument != nullptr) {
      expression.roots.push_back(argument.get());
    }
  }
  if (appendFullExpression(constructorFullExpressions[&initializer],
                           expression)) {
    fullExpressionOrder.push_back(std::move(expression));
  }
}

void SemanticModel::recordConstant(const Expr &expression,
                                   ConstantValue value) {
  constants.insert_or_assign(&expression, std::move(value));
}

void SemanticModel::recordUnsafeOperation(const Expr &expression,
                                          UnsafeOperationKind operation) {
  unsafeOperations.insert_or_assign(&expression, operation);
}

void SemanticModel::recordPlace(const Expr &expression, PlaceKey place) {
  places.insert_or_assign(&expression, std::move(place));
}

void SemanticModel::recordOwnershipEvent(const Expr &expression,
                                         OwnershipEvent event) {
  if (!ownershipEvents.contains(&expression)) {
    ownershipEventOrder.push_back(&expression);
  }
  ownershipEvents.insert_or_assign(&expression, std::move(event));
}

void SemanticModel::recordLambdaCaptureMode(SymbolId symbol,
                                            LambdaCaptureMode mode) {
  if (symbol != 0) {
    lambdaCaptureModes.insert_or_assign(symbol, mode);
  }
}

void SemanticModel::markOwnershipEventsUnreachableFrom(std::size_t first) {
  for (std::size_t index = first; index < ownershipEventOrder.size(); ++index) {
    ownershipEvents[ownershipEventOrder[index]].reachable = false;
  }
}

void SemanticModel::recordPlaceSelection(const Expr &expression,
                                         std::size_t selection) {
  placeSelections.insert_or_assign(&expression, selection);
}

void SemanticModel::record(const ArrayExtentExpr &extent,
                           CompileTimeValue value) {
  arrayExtents.insert_or_assign(&extent, value);
}

void SemanticModel::record(const VariableDecl &declaration, BindingInfo info) {
  variableBindings.insert_or_assign(&declaration, std::move(info));
}

void SemanticModel::record(const Parameter &parameter, BindingInfo info) {
  parameterBindings.insert_or_assign(&parameter, std::move(info));
}

void SemanticModel::recordPayloadBinding(const Token &name, BindingInfo info) {
  payloadBindings.insert_or_assign(&name, std::move(info));
}

void SemanticModel::recordLoan(SemanticLoanInfo info) {
  if (info.id == 0) {
    return;
  }
  if (retainedLoans.size() < info.id) {
    retainedLoans.resize(info.id);
  }
  retainedLoans[info.id - 1] = std::move(info);
}

void SemanticModel::recordBindingLoan(const VariableDecl &declaration,
                                      SemanticLoanId loan) {
  auto binding = variableBindings.find(&declaration);
  if (binding == variableBindings.end() && base != nullptr) {
    if (const BindingInfo *inherited = base->findBinding(declaration)) {
      binding =
          variableBindings.insert_or_assign(&declaration, *inherited).first;
    }
  }
  if (binding != variableBindings.end()) {
    binding->second.retainedLoan = loan;
  }
}

void SemanticModel::recordBindingLoan(const Parameter &parameter,
                                      SemanticLoanId loan) {
  if (auto binding = parameterBindings.find(&parameter);
      binding != parameterBindings.end()) {
    binding->second.retainedLoan = loan;
  }
}

void SemanticModel::recordLoanEndAfter(SemanticLoanId id,
                                       const Stmt &statement) {
  if (!validLoan(id)) {
    return;
  }
  recordLoanEndpoint(id, SemanticLoanEndKind::AfterStatement, statement);
  appendUniqueLoan(loanEnds[&statement], id);
}

void SemanticModel::recordLoanEndAtConditionalEntry(SemanticLoanId id,
                                                    const IfStmt &statement,
                                                    bool thenBranch) {
  if (!validLoan(id)) {
    return;
  }
  const SemanticLoanEndKind kind = thenBranch
                                       ? SemanticLoanEndKind::ThenBranchEntry
                                       : SemanticLoanEndKind::ElseBranchEntry;
  recordLoanEndpoint(id, kind, statement);
  SemanticConditionalLoanEnds &ends = conditionalLoanEnds[&statement];
  appendUniqueLoan(thenBranch ? ends.thenEntry : ends.elseEntry, id);
}

void SemanticModel::recordConstexprBranch(const IfStmt &statement,
                                          bool thenBranch) {
  constexprBranches.insert_or_assign(&statement, thenBranch);
}

void SemanticModel::recordLoanEndAtSwitchArmEntry(SemanticLoanId id,
                                                  const SwitchStmt &statement,
                                                  std::size_t armIndex) {
  if (!validLoan(id)) {
    return;
  }
  recordLoanEndpoint(id, SemanticLoanEndKind::SwitchArmEntry, statement,
                     armIndex);
  std::vector<std::vector<SemanticLoanId>> &ends =
      switchArmLoanEnds[&statement];
  if (ends.size() <= armIndex) {
    ends.resize(armIndex + 1);
  }
  appendUniqueLoan(ends[armIndex], id);
}

void SemanticModel::recordLoanCarrier(SemanticLoanId id, SymbolId carrier) {
  if (id == 0 || carrier == 0 || id > retainedLoans.size() ||
      retainedLoans[id - 1].id != id) {
    return;
  }
  std::vector<SymbolId> &carriers = retainedLoans[id - 1].carriers;
  if (std::find(carriers.begin(), carriers.end(), carrier) == carriers.end()) {
    carriers.push_back(carrier);
  }
}

void SemanticModel::clearLoans() {
  retainedLoans.clear();
  loanEnds.clear();
  conditionalLoanEnds.clear();
  switchArmLoanEnds.clear();
  for (auto &[_, binding] : variableBindings) {
    binding.retainedLoan = 0;
  }
  for (auto &[_, binding] : parameterBindings) {
    binding.retainedLoan = 0;
  }
}

void SemanticModel::record(const StructuredBindingDecl &declaration,
                           StructuredBindingInfo info) {
  structuredBindings.insert_or_assign(&declaration, std::move(info));
}

void SemanticModel::recordExplicitMove(const VariableDecl &declaration) {
  auto binding = variableBindings.find(&declaration);
  if (binding == variableBindings.end() && base != nullptr) {
    if (const BindingInfo *inherited = base->findBinding(declaration)) {
      binding =
          variableBindings.insert_or_assign(&declaration, *inherited).first;
    }
  }
  if (binding != variableBindings.end()) {
    binding->second.explicitlyMoved = true;
  }
}

void SemanticModel::recordExplicitMove(const Parameter &parameter) {
  auto binding = parameterBindings.find(&parameter);
  if (binding == parameterBindings.end() && base != nullptr) {
    if (const BindingInfo *inherited = base->findBinding(parameter)) {
      binding =
          parameterBindings.insert_or_assign(&parameter, *inherited).first;
    }
  }
  if (binding != parameterBindings.end()) {
    binding->second.explicitlyMoved = true;
  }
}

void SemanticModel::record(const FunctionDecl &declaration, FunctionInfo info) {
  const auto [found, _] =
      functions.insert_or_assign(&declaration, std::move(info));
  functionsById.insert_or_assign(found->second.id, &declaration);
}

void SemanticModel::record(const Lambda &declaration, LambdaInfo info) {
  const auto [found, _] =
      lambdas.insert_or_assign(&declaration, std::move(info));
  lambdasById.insert_or_assign(found->second.id, &declaration);
}

void SemanticModel::recordClassType(const ClassDecl &declaration,
                                    ClassTypeInfo info) {
  const auto [found, _] =
      classTypes.insert_or_assign(&declaration, std::move(info));
  classTypesById.insert_or_assign(found->second.id, &declaration);
}

void SemanticModel::record(const TypeAliasDecl &declaration,
                           TypeAliasInfo info) {
  typeAliases.insert_or_assign(&declaration, std::move(info));
}

void SemanticModel::recordEnumType(const EnumDecl &declaration,
                                   EnumTypeInfo info) {
  const auto [found, _] =
      enumTypes.insert_or_assign(&declaration, std::move(info));
  enumTypesById.insert_or_assign(found->second.id, &declaration);
}

void SemanticModel::record(const QualifiedName &expression,
                           ResolvedEnumeratorInfo info) {
  enumerators.insert_or_assign(&expression, std::move(info));
}

void SemanticModel::recordSwitchCase(const Expr &expression,
                                     SwitchCaseValue value) {
  switchCases.insert_or_assign(&expression, std::move(value));
}

void SemanticModel::recordExhaustiveSwitch(const SwitchStmt &statement) {
  exhaustiveSwitches.insert(&statement);
}

void SemanticModel::recordPayloadConstruction(
    const Call &call, ResolvedPayloadConstructionInfo info) {
  payloadConstructions.insert_or_assign(&call, std::move(info));
}

void SemanticModel::recordPayloadPattern(const Expr &expression,
                                         ResolvedPayloadPatternInfo info) {
  payloadPatterns.insert_or_assign(&expression, std::move(info));
}

void SemanticModel::record(const Call &call, ResolvedCallInfo info) {
  calls.insert_or_assign(&call, std::move(info));
}

void SemanticModel::record(const PackFold &fold, ResolvedPackFoldInfo info) {
  packFolds.insert_or_assign(&fold, std::move(info));
}

void SemanticModel::recordLambdaCall(const Call &call,
                                     ResolvedLambdaCallInfo info) {
  lambdaCalls.insert_or_assign(&call, std::move(info));
}

void SemanticModel::recordDeferredCallableCall(const Call &call,
                                               DeferredCallableCallInfo info) {
  deferredCallableCalls.insert_or_assign(&call, std::move(info));
}

void SemanticModel::recordCallableRequirement(
    const FunctionDecl &declaration, CallableParameterContract requirement) {
  const auto function = materializeFunction(declaration);
  if (function == functions.end()) {
    return;
  }
  auto existing = std::find_if(function->second.callableParameters.begin(),
                               function->second.callableParameters.end(),
                               [&](const CallableParameterContract &candidate) {
                                 return candidate.parameterIndex ==
                                        requirement.parameterIndex;
                               });
  if (existing == function->second.callableParameters.end()) {
    function->second.callableParameters.emplace_back(std::move(requirement));
    return;
  }
  if (requirement.boundary == CallableBoundary::Owned &&
      existing->signatures.empty() && existing->forwardings.empty()) {
    existing->boundary = CallableBoundary::Owned;
    existing->ownedTransport = std::move(requirement.ownedTransport);
  }
  for (CallableSignatureRequirement &signature : requirement.signatures) {
    const auto concrete =
        std::find_if(existing->signatures.begin(), existing->signatures.end(),
                     [&](const CallableSignatureRequirement &candidate) {
                       return candidate.source == signature.source;
                     });
    if (concrete == existing->signatures.end()) {
      existing->signatures.emplace_back(std::move(signature));
    } else {
      *concrete = std::move(signature);
    }
  }
  for (CallableForwardingRequirement &forwarding : requirement.forwardings) {
    if (std::none_of(existing->forwardings.begin(), existing->forwardings.end(),
                     [&](const CallableForwardingRequirement &candidate) {
                       return candidate.source == forwarding.source &&
                              candidate.parameterIndex ==
                                  forwarding.parameterIndex;
                     })) {
      existing->forwardings.emplace_back(std::move(forwarding));
    }
  }
}

void SemanticModel::recordCallableForwarding(
    const FunctionDecl &source, std::size_t sourceParameterIndex,
    GenericParameterId sourceGenericParameter, SemanticType sourceType,
    AccessMode sourceAccess, const Call &call, FunctionId target,
    std::size_t targetParameterIndex) {
  const auto duplicate = std::find_if(
      pendingCallableForwardings.begin(), pendingCallableForwardings.end(),
      [&](const PendingCallableForwarding &candidate) {
        return candidate.source == &source && candidate.call == &call &&
               candidate.sourceParameterIndex == sourceParameterIndex &&
               candidate.targetParameterIndex == targetParameterIndex;
      });
  if (duplicate != pendingCallableForwardings.end()) {
    return;
  }
  pendingCallableForwardings.push_back(
      {.source = &source,
       .sourceParameterIndex = sourceParameterIndex,
       .sourceGenericParameter = sourceGenericParameter,
       .sourceType = std::move(sourceType),
       .sourceAccess = sourceAccess,
       .call = &call,
       .target = target,
       .targetParameterIndex = targetParameterIndex});
}

void SemanticModel::recordOperator(const Expr &expression,
                                   ResolvedOperatorInfo info) {
  operators.insert_or_assign(&expression, std::move(info));
}

void SemanticModel::recordContextualConversion(const Expr &expression,
                                               ResolvedOperatorInfo info) {
  contextualConversions.insert_or_assign(&expression, std::move(info));
}

void SemanticModel::recordContextualIntegerOperand(const Expr &expression) {
  contextualIntegerOperands.insert(&expression);
}

void SemanticModel::record(const ClassDecl &declaration,
                           ClassLifecycleInfo info) {
  classLifecycles.insert_or_assign(&declaration, std::move(info));
}

void SemanticModel::record(const Expr &expression,
                           ResolvedConstructionInfo info) {
  constructions.insert_or_assign(&expression, std::move(info));
}

void SemanticModel::record(const ConstructorInitializer &initializer,
                           ResolvedConstructorInitializerInfo info) {
  constructorInitializers.insert_or_assign(&initializer, std::move(info));
}

void SemanticModel::recordResolvedSymbol(const Expr &expression,
                                         SymbolId symbol) {
  if (symbol != 0) {
    resolvedSymbols.insert_or_assign(&expression, symbol);
  }
}

void SemanticModel::recordCompilerCapabilityType(
    const TypeRef &type, CompilerCapabilityTypeKind kind) {
  if (kind != CompilerCapabilityTypeKind::None) {
    compilerCapabilityTypes.insert_or_assign(&type, kind);
  }
}

SymbolId SemanticModel::recordSymbol(SymbolRecord symbol) {
  return semanticDatabase.recordSymbol(std::move(symbol));
}

[[nodiscard]] SymbolId
SemanticModel::symbolForDeclaration(SourceUnitId sourceUnit,
                                    const SourceSpan &span,
                                    std::string_view generatedName) const {
  return semanticDatabase.symbolForDeclaration(sourceUnit, span, generatedName);
}

void SemanticModel::recordOccurrence(SemanticOccurrence occurrence) {
  semanticDatabase.record(std::move(occurrence));
}

// Occurrences answer editor position queries (hover, definition, semantic
// tokens) and are consumed only by language queries and the LSP. Symbols
// stay recorded either way because HIR and the emitter resolve member
// identity through them. A compile-only consumer disables occurrences so
// analysis does not build, sort, and retain a table nothing will read.
void SemanticModel::setToolingOccurrencesEnabled(bool enabled) {
  semanticDatabase.setToolingOccurrencesEnabled(enabled);
}

void SemanticModel::recordCompletion(SemanticCompletionContext context) {
  completion = std::move(context);
}

void SemanticModel::finalizeOccurrences() { semanticDatabase.finalize(); }

void SemanticModel::finalizeCallableArguments(ResolvedCallInfo &call) const {
  const FunctionInfo *function = findFunction(call.function);
  if (function == nullptr) {
    return;
  }
  for (const CallableParameterContract &parameter :
       function->callableParameters) {
    if (parameter.parameterIndex < call.parameterTypes.size()) {
      const auto containsLambda = [&](const auto &self,
                                      const SemanticType &type) -> bool {
        return type.kind == SemanticType::Lambda ||
               std::any_of(type.arguments.begin(), type.arguments.end(),
                           [&](const SemanticType &argument) {
                             return self(self, argument);
                           });
      };
      if (parameter.boundary == CallableBoundary::Owned &&
          !containsLambda(containsLambda,
                          call.parameterTypes[parameter.parameterIndex])) {
        continue;
      }
      const auto existing = std::find_if(
          call.callableArguments.begin(), call.callableArguments.end(),
          [&](const CallableArgumentBoundary &candidate) {
            return candidate.parameterIndex == parameter.parameterIndex;
          });
      if (existing == call.callableArguments.end()) {
        call.callableArguments.push_back(
            {.parameterIndex = parameter.parameterIndex,
             .boundary = parameter.boundary});
      } else {
        // The function contract is authoritative. The provisional lambda
        // marker recorded at a call site must not mask a later, more
        // precise boundary when owned callable transport is implemented.
        existing->boundary = parameter.boundary;
      }
    }
  }
  std::sort(call.callableArguments.begin(), call.callableArguments.end(),
            [](const CallableArgumentBoundary &left,
               const CallableArgumentBoundary &right) {
              return left.parameterIndex < right.parameterIndex;
            });
  call.callableArguments.erase(
      std::unique(call.callableArguments.begin(), call.callableArguments.end(),
                  [](const CallableArgumentBoundary &left,
                     const CallableArgumentBoundary &right) {
                    return left.parameterIndex == right.parameterIndex;
                  }),
      call.callableArguments.end());
}

void SemanticModel::finalizeCallableArguments() {
  for (auto &[_, call] : calls) {
    finalizeCallableArguments(call);
  }
  for (auto &[_, occurrences] : semanticDatabase.occurrencesByUnit) {
    for (SemanticOccurrence &occurrence : occurrences) {
      if (occurrence.selectedCall) {
        finalizeCallableArguments(*occurrence.selectedCall);
      }
    }
  }
}

void SemanticModel::finalizeCallableForwardings() {
  bool changed = true;
  while (changed) {
    changed = false;
    for (const PendingCallableForwarding &forwarding :
         pendingCallableForwardings) {
      const FunctionInfo *target = findFunction(forwarding.target);
      if (target == nullptr) {
        continue;
      }
      const auto targetContract = std::find_if(
          target->callableParameters.begin(), target->callableParameters.end(),
          [&](const CallableParameterContract &candidate) {
            return candidate.parameterIndex ==
                       forwarding.targetParameterIndex &&
                   candidate.boundary == CallableBoundary::Confined;
          });
      if (targetContract == target->callableParameters.end()) {
        continue;
      }

      const auto source = forwarding.source == nullptr
                              ? functions.end()
                              : materializeFunction(*forwarding.source);
      if (source == functions.end()) {
        continue;
      }
      auto contract = std::find_if(
          source->second.callableParameters.begin(),
          source->second.callableParameters.end(),
          [&](const CallableParameterContract &candidate) {
            return candidate.parameterIndex == forwarding.sourceParameterIndex;
          });
      if (contract == source->second.callableParameters.end()) {
        source->second.callableParameters.push_back(
            {.parameterIndex = forwarding.sourceParameterIndex,
             .genericParameter = forwarding.sourceGenericParameter,
             .callableType = forwarding.sourceType,
             .access = forwarding.sourceAccess,
             .forwardings = {
                 {.source = forwarding.call,
                  .function = forwarding.target,
                  .parameterIndex = forwarding.targetParameterIndex}}});
        changed = true;
        continue;
      }

      const bool exists = std::any_of(
          contract->forwardings.begin(), contract->forwardings.end(),
          [&](const CallableForwardingRequirement &candidate) {
            return candidate.source == forwarding.call &&
                   candidate.parameterIndex == forwarding.targetParameterIndex;
          });
      if (!exists) {
        contract->forwardings.push_back(
            {.source = forwarding.call,
             .function = forwarding.target,
             .parameterIndex = forwarding.targetParameterIndex});
        changed = true;
      }
    }
  }
}

[[nodiscard]] std::size_t acquirePlaceSnapshotIdentity() {
  static std::atomic_size_t next{1};
  std::size_t result = next.fetch_add(1, std::memory_order_relaxed);
  if (result == 0) {
    result = next.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

[[nodiscard]] PlaceRelationResult placeRelation(const PlaceKey &left,
                                                const PlaceKey &right) {
  if (left.domain != right.domain) {
    return {.compatibleDomain = false};
  }
  if (!left.valid() || !right.valid()) {
    return {.relation = PlaceRelation::MayAlias};
  }
  if (left.receiver != right.receiver || left.root != right.root) {
    return {.relation = PlaceRelation::Disjoint};
  }

  const std::size_t common =
      std::min(left.projections.size(), right.projections.size());
  for (std::size_t projectionIndex = 0; projectionIndex < common;
       ++projectionIndex) {
    const PlaceProjection &lhs = left.projections[projectionIndex];
    const PlaceProjection &rhs = right.projections[projectionIndex];
    if (lhs == rhs) {
      continue;
    }
    if (lhs.kind == PlaceProjectionKind::Field &&
        rhs.kind == PlaceProjectionKind::Field && lhs.field != 0 &&
        rhs.field != 0 && lhs.field != rhs.field) {
      return {.relation = PlaceRelation::Disjoint};
    }
    if (lhs.kind == PlaceProjectionKind::ConstantIndex &&
        rhs.kind == PlaceProjectionKind::ConstantIndex &&
        lhs.index != rhs.index) {
      return {.relation = PlaceRelation::Disjoint};
    }
    if (lhs.kind == PlaceProjectionKind::DynamicIndex &&
        rhs.kind == PlaceProjectionKind::DynamicIndex && lhs.selection != 0 &&
        lhs.selection == rhs.selection) {
      continue;
    }
    return {.relation = PlaceRelation::MayAlias};
  }
  if (left.projections.size() == right.projections.size()) {
    return {.relation = PlaceRelation::Equal};
  }
  return {.relation = left.projections.size() < right.projections.size()
                          ? PlaceRelation::LeftStrictPrefix
                          : PlaceRelation::RightStrictPrefix};
}

[[nodiscard]] bool placesMayOverlap(const PlaceKey &left,
                                    const PlaceKey &right) {
  const PlaceRelationResult relation = placeRelation(left, right);
  return !relation.compatibleDomain ||
         relation.relation != PlaceRelation::Disjoint;
}

[[nodiscard]] ArrayExtentEvaluation
evaluateArrayExtent(const ArrayExtentExpr &expression) {
  if (expression.isAtom()) {
    if (const auto *value =
            std::get_if<std::uint64_t>(&expression.token.literal)) {
      return {.value = *value};
    }
    return {.error = ArrayExtentEvaluationError::NonLiteral,
            .token = &expression.token};
  }
  if (!expression.left || !expression.right) {
    return {.error = ArrayExtentEvaluationError::NonLiteral,
            .token = &expression.token};
  }

  const ArrayExtentEvaluation left = evaluateArrayExtent(*expression.left);
  if (!left.value) {
    return left;
  }
  const ArrayExtentEvaluation right = evaluateArrayExtent(*expression.right);
  if (!right.value) {
    return right;
  }

  std::optional<CheckedIntegerOperation> operation;
  switch (expression.token.kind) {
  case TokenKind::PLUS:
    operation = CheckedIntegerOperation::Add;
    break;
  case TokenKind::MINUS:
    operation = CheckedIntegerOperation::Subtract;
    break;
  case TokenKind::STAR:
    operation = CheckedIntegerOperation::Multiply;
    break;
  case TokenKind::SLASH:
    operation = CheckedIntegerOperation::Divide;
    break;
  case TokenKind::PERCENT:
    operation = CheckedIntegerOperation::Remainder;
    break;
  default:
    return {.error = ArrayExtentEvaluationError::NonLiteral,
            .token = &expression.token};
  }

  const std::optional<CheckedIntegerOutcome> evaluated =
      evaluateCheckedIntegerBinary(*operation, {.magnitude = *left.value},
                                   {.magnitude = *right.value},
                                   CheckedIntegerDomain{.width = 64});
  if (!evaluated) {
    return {.error = ArrayExtentEvaluationError::NonLiteral,
            .token = &expression.token};
  }
  if (const auto *value = std::get_if<CheckedIntegerValue>(&*evaluated)) {
    return {.value = value->magnitude};
  }

  const CheckedIntegerFailure failure =
      std::get<CheckedIntegerFailure>(*evaluated);
  if (failure == CheckedIntegerFailure::Overflow) {
    return {.error = expression.token.kind == TokenKind::MINUS
                         ? ArrayExtentEvaluationError::Underflow
                         : ArrayExtentEvaluationError::Overflow,
            .token = &expression.token};
  }
  if (failure == CheckedIntegerFailure::DivisionByZero ||
      failure == CheckedIntegerFailure::ModuloByZero) {
    return {.error = ArrayExtentEvaluationError::ZeroDivisor,
            .token = &expression.token};
  }
  return {.error = ArrayExtentEvaluationError::NonLiteral,
          .token = &expression.token};
}

[[nodiscard]] std::optional<CheckedIntegerDomain>
constantIntegerDomain(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Int8:
    return CheckedIntegerDomain{.width = 8, .signedValue = true};
  case SemanticType::Int16:
    return CheckedIntegerDomain{.width = 16, .signedValue = true};
  case SemanticType::Int32:
    return CheckedIntegerDomain{.width = 32, .signedValue = true};
  case SemanticType::Int64:
    return CheckedIntegerDomain{.width = 64, .signedValue = true};
  case SemanticType::UInt8:
    return CheckedIntegerDomain{.width = 8};
  case SemanticType::UInt16:
    return CheckedIntegerDomain{.width = 16};
  case SemanticType::UInt32:
    return CheckedIntegerDomain{.width = 32};
  case SemanticType::UInt64:
    return CheckedIntegerDomain{.width = 64};
  default:
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<BinaryFloatFormat>
semanticFloatFormat(const SemanticType &type) {
  if (type == SemanticType::Float) {
    return BinaryFloatFormat::Binary32;
  }
  if (type == SemanticType::Double) {
    return BinaryFloatFormat::Binary64;
  }
  return std::nullopt;
}

[[nodiscard]] SemanticType semanticIntegerType(CheckedIntegerDomain domain) {
  if (domain.signedValue) {
    switch (domain.width) {
    case 8:
      return SemanticType::Int8;
    case 16:
      return SemanticType::Int16;
    case 32:
      return SemanticType::Int32;
    case 64:
      return SemanticType::Int64;
    default:
      return SemanticType::Unknown;
    }
  }
  switch (domain.width) {
  case 8:
    return SemanticType::UInt8;
  case 16:
    return SemanticType::UInt16;
  case 32:
    return SemanticType::UInt32;
  case 64:
    return SemanticType::UInt64;
  default:
    return SemanticType::Unknown;
  }
}

[[nodiscard]] SemanticTypeTraits semanticTraits(const SemanticType &type) {
  SemanticTypeTraits traits;
  switch (type.kind) {
  case SemanticType::Unknown:
    traits.drop = DropKind::Lexical;
    traits.copyable = false;
    traits.movable = false;
    traits.copyAssignable = false;
    traits.moveAssignable = false;
    traits.transferCapable = false;
    traits.shareCapable = false;
    return traits;
  case SemanticType::Void:
  case SemanticType::TypePack:
  case SemanticType::TypeName:
  case SemanticType::Function:
    traits.copyable = false;
    traits.movable = false;
    traits.copyAssignable = false;
    traits.moveAssignable = false;
    traits.transferCapable = false;
    traits.shareCapable = false;
    return traits;
  case SemanticType::Lambda:
    traits.drop = DropKind::Lexical;
    return traits;
  case SemanticType::Reference:
    traits.ownership = OwnershipKind::Borrowed;
    traits.copyAssignable = false;
    traits.moveAssignable = false;
    traits.containsBorrowedState = true;
    traits.transferCapable = false;
    traits.shareCapable = false;
    return traits;
  case SemanticType::RawPointer:
  case SemanticType::StringView:
    traits.transferCapable = false;
    traits.shareCapable = false;
    return traits;
  case SemanticType::Array:
    if (type.arguments.size() == 1) {
      const SemanticTypeTraits element = semanticTraits(type.arguments[0]);
      traits.drop = element.drop;
      traits.copyable = element.copyable;
      traits.movable = element.movable;
      traits.copyAssignable = element.copyAssignable;
      traits.moveAssignable = element.moveAssignable;
      traits.containsBorrowedState = element.containsBorrowedState;
      traits.transferCapable = element.transferCapable;
      traits.shareCapable = element.shareCapable;
      return traits;
    }
    traits.drop = DropKind::Lexical;
    traits.copyable = false;
    traits.movable = false;
    traits.copyAssignable = false;
    traits.moveAssignable = false;
    traits.transferCapable = false;
    traits.shareCapable = false;
    return traits;
  case SemanticType::UniqueOwner:
    traits.ownership = OwnershipKind::Unique;
    traits.drop = DropKind::Lexical;
    traits.copyable = false;
    traits.copyAssignable = false;
    if (type.arguments.size() == 1) {
      const SemanticTypeTraits element = semanticTraits(type.arguments[0]);
      traits.transferCapable = element.transferCapable;
      traits.shareCapable = element.shareCapable;
    } else {
      traits.transferCapable = false;
      traits.shareCapable = false;
    }
    return traits;
  case SemanticType::Storage:
    traits.ownership = OwnershipKind::Unique;
    traits.drop = DropKind::Lexical;
    traits.copyable = false;
    traits.copyAssignable = false;
    if (type.arguments.size() == 1) {
      const SemanticTypeTraits element = semanticTraits(type.arguments[0]);
      traits.transferCapable = element.transferCapable;
      traits.shareCapable = element.shareCapable;
    } else {
      traits.transferCapable = false;
      traits.shareCapable = false;
    }
    return traits;
  case SemanticType::SharedPointer:
    traits.ownership = OwnershipKind::Shared;
    traits.drop = DropKind::Lexical;
    traits.transferCapable = false;
    traits.shareCapable = false;
    return traits;
  case SemanticType::Class:
  case SemanticType::TypeParameter:
    traits.drop = DropKind::Lexical;
    traits.transferCapable = false;
    traits.shareCapable = false;
    return traits;
  case SemanticType::Expected:
  case SemanticType::Unexpected:
    traits.drop = DropKind::Lexical;
    if (type.arguments.empty()) {
      traits.transferCapable = false;
      traits.shareCapable = false;
      return traits;
    }
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      if (type.kind == SemanticType::Expected && index == 0 &&
          type.arguments[index] == SemanticType::Void) {
        continue;
      }
      const SemanticTypeTraits component =
          semanticTraits(type.arguments[index]);
      traits.copyable = traits.copyable && component.copyable;
      traits.movable = traits.movable && component.movable;
      traits.copyAssignable = traits.copyAssignable && component.copyAssignable;
      traits.moveAssignable = traits.moveAssignable && component.moveAssignable;
      traits.containsBorrowedState =
          traits.containsBorrowedState || component.containsBorrowedState;
      traits.transferCapable =
          traits.transferCapable && component.transferCapable;
      traits.shareCapable = traits.shareCapable && component.shareCapable;
    }
    return traits;
  default:
    return traits;
  }
}

[[nodiscard]] std::optional<IntegerArithmeticIntrinsic>
integerArithmeticIntrinsic(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::IntegerWrappingAdd:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Add,
                                      IntegerArithmeticMode::Wrapping};
  case IntrinsicKind::IntegerWrappingSubtract:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Subtract,
                                      IntegerArithmeticMode::Wrapping};
  case IntrinsicKind::IntegerWrappingMultiply:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Multiply,
                                      IntegerArithmeticMode::Wrapping};
  case IntrinsicKind::IntegerSaturatingAdd:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Add,
                                      IntegerArithmeticMode::Saturating};
  case IntrinsicKind::IntegerSaturatingSubtract:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Subtract,
                                      IntegerArithmeticMode::Saturating};
  case IntrinsicKind::IntegerSaturatingMultiply:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Multiply,
                                      IntegerArithmeticMode::Saturating};
  case IntrinsicKind::IntegerCheckedAdd:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Add,
                                      IntegerArithmeticMode::CheckedResult};
  case IntrinsicKind::IntegerCheckedSubtract:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Subtract,
                                      IntegerArithmeticMode::CheckedResult};
  case IntrinsicKind::IntegerCheckedMultiply:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Multiply,
                                      IntegerArithmeticMode::CheckedResult};
  default:
    return std::nullopt;
  }
}

[[nodiscard]] ExpressionInfo makeExpressionInfo(SemanticType type,
                                                ValueCategory category,
                                                AccessMode access) {
  const SemanticTypeTraits traits = semanticTraits(type);
  return ExpressionInfo{.type = std::move(type),
                        .category = category,
                        .access = access,
                        .traits = traits};
}

[[nodiscard]] BindingInfo makeBindingInfo(SemanticType type,
                                          AccessMode access) {
  const SemanticTypeTraits traits = semanticTraits(type);
  return BindingInfo{
      .type = std::move(type), .access = access, .traits = traits};
}

} // namespace lang
