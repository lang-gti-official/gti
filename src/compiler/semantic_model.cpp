#include "gti/semantic_model.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <iterator>
#include <unordered_set>
#include <utility>

namespace lang {
RestrictedBorrowResultDomain restrictCompleteBorrowResultDomain(
    const FunctionBorrowDependencySummary &summary,
    std::span<const BorrowSupplierAvailability> suppliers) {
  if (suppliers.size() != summary.dependencies.size())
    return {};
  const auto contains = [](const auto &all, const auto &required) {
    return std::all_of(required.begin(), required.end(), [&](const auto &item) {
      return std::find(all.begin(), all.end(), item) != all.end();
    });
  };
  RestrictedBorrowResultDomain result{.status = BorrowResultDomainStatus::Known,
                                      .possibleAlternatives =
                                          summary.possibleAlternatives};
  const auto possible = [&](const auto &guard) {
    return guard.empty() ||
           std::any_of(result.possibleAlternatives.begin(),
                       result.possibleAlternatives.end(),
                       [&](const auto &path) {
                         return contains(path.conditions, guard);
                       });
  };
  const auto prune = [&](const auto &guard) {
    if (guard.empty()) {
      result.status = BorrowResultDomainStatus::NoNormalResult;
      result.possibleAlternatives.clear();
      return;
    }
    std::erase_if(result.possibleAlternatives, [&](const auto &path) {
      return contains(path.conditions, guard);
    });
  };
  const auto sameResult = [](const BorrowedFieldOrigin &left,
                             const BorrowedFieldOrigin &right) {
    return left.resultFields == right.resultFields &&
           left.resultConditions == right.resultConditions &&
           left.access == right.access;
  };
  for (std::size_t index = 0; index < summary.dependencies.size(); ++index) {
    const auto &dependency = summary.dependencies[index];
    if (!possible(dependency.resultConditions))
      continue;
    bool allImpossible = true;
    for (std::size_t other = 0; other < summary.dependencies.size(); ++other)
      if (sameResult(dependency, summary.dependencies[other]) &&
          suppliers[other] != BorrowSupplierAvailability::Impossible)
        allImpossible = false;
    if (allImpossible)
      prune(dependency.resultConditions);
  }
  // Removing the final alternative of a required nested enum makes its
  // parent case impossible as well. Domain identity is the exact typed
  // subobject; siblings of another enum type or field cannot satisfy it.
  // Repeat because pruning a child may empty another ancestor's required
  // domain.
  bool changed = true;
  while (result.status == BorrowResultDomainStatus::Known && changed) {
    changed = false;
    for (const auto &dependency : summary.dependencies)
      for (std::size_t depth = 0; depth < dependency.resultConditions.size();
           ++depth) {
        const auto ancestors =
            std::span(dependency.resultConditions).first(depth);
        if (!possible(ancestors))
          continue;
        const auto &domain = dependency.resultConditions[depth];
        const bool remains = std::any_of(
            result.possibleAlternatives.begin(),
            result.possibleAlternatives.end(), [&](const auto &path) {
              return contains(path.conditions, ancestors) &&
                     std::any_of(
                         path.conditions.begin(), path.conditions.end(),
                         [&](const auto &condition) {
                           return condition.subobject == domain.subobject &&
                                  condition.enumeration == domain.enumeration;
                         });
            });
        if (!remains) {
          const auto count = result.possibleAlternatives.size();
          prune(ancestors);
          changed |= result.possibleAlternatives.size() != count;
        }
      }
  }
  if (result.status == BorrowResultDomainStatus::NoNormalResult ||
      (!summary.possibleAlternatives.empty() &&
       result.possibleAlternatives.empty()))
    return {.status = BorrowResultDomainStatus::NoNormalResult};
  for (std::size_t index = 0; index < summary.dependencies.size(); ++index) {
    if (!possible(summary.dependencies[index].resultConditions))
      continue;
    if (suppliers[index] == BorrowSupplierAvailability::Unknown)
      return {};
    if (suppliers[index] == BorrowSupplierAvailability::Available)
      result.dependencies.push_back(index);
  }
  return result;
}

SemanticType substituteSemanticType(const SemanticType &type,
                                    const GenericSubstitution &substitution,
                                    const SemanticModel *model) {
  if (type.kind == SemanticType::TypeParameter ||
      (type.kind == SemanticType::TypePack && !type.concretePack)) {
    const auto found = substitution.types.find(type.genericParameterId);
    if (found == substitution.types.end())
      return type;
    return model ? model->resolveExactClassSpecialization(found->second)
                 : found->second;
  }
  SemanticType result = type;
  const auto substituteTypes = [&](auto &types) {
    for (auto &argument : types)
      argument = substituteSemanticType(argument, substitution, model);
  };
  substituteTypes(result.arguments);
  if (result.kind == SemanticType::Class || result.kind == SemanticType::Enum ||
      result.kind == SemanticType::FunctionSignature) {
    std::vector<SemanticType> expanded;
    for (SemanticType &argument : result.arguments)
      SemanticType::appendExpandedArgument(expanded, std::move(argument));
    result.arguments = std::move(expanded);
  }
  substituteTypes(result.lambdaEnclosingClassTypes);
  substituteTypes(result.lambdaEnclosingFunctionTypes);
  const auto substituteValues = [&](auto &values) {
    for (auto &argument : values)
      argument = substituteCompileTimeValue(argument, substitution.values);
  };
  substituteValues(result.valueArguments);
  substituteValues(result.lambdaEnclosingClassValues);
  substituteValues(result.lambdaEnclosingFunctionValues);
  if (result.arrayLengthParameterId != 0) {
    const auto found = substitution.values.find(result.arrayLengthParameterId);
    if (found != substitution.values.end()) {
      if (found->second.kind == CompileTimeValue::UInt64) {
        result.arrayLength = found->second.value;
        result.arrayLengthParameterId = 0;
      } else if (found->second.kind == CompileTimeValue::Parameter) {
        result.arrayLengthParameterId = found->second.parameterId;
      }
    }
  }
  return model ? model->resolveExactClassSpecialization(std::move(result))
               : result;
}

CompileTimeValue substituteCompileTimeValue(
    const CompileTimeValue &value,
    const std::unordered_map<GenericParameterId, CompileTimeValue>
        &substitution) {
  if (value.kind == CompileTimeValue::Parameter) {
    const auto found = substitution.find(value.parameterId);
    return found == substitution.end() ? value : found->second;
  }
  if (!value.expression)
    return value;
  const auto evaluate =
      [&](const auto &self,
          const GenericValueExpression &expression) -> CompileTimeValue {
    if (expression.kind == GenericValueExpression::UInt64)
      return CompileTimeValue::uint64(expression.value);
    if (expression.kind == GenericValueExpression::Parameter)
      return substituteCompileTimeValue(
          CompileTimeValue::parameter(expression.parameterId), substitution);
    if (!expression.left || !expression.right)
      return {};
    auto left = self(self, *expression.left);
    auto right = self(self, *expression.right);
    if (left.kind == CompileTimeValue::UInt64 &&
        right.kind == CompileTimeValue::UInt64) {
      const auto evaluated = evaluateConstantBinary(
          expression.token.kind,
          ConstantValue{
              makeConstantInteger({.magnitude = left.value}, {.width = 64})},
          ConstantValue{
              makeConstantInteger({.magnitude = right.value}, {.width = 64})},
          CheckedIntegerDomain{.width = 64});
      if (evaluated)
        if (const auto result = constantUnsignedMagnitude(*evaluated.value))
          return CompileTimeValue::uint64(*result);
      return {};
    }
    const auto retain = [](const CompileTimeValue &operand) {
      if (operand.expression)
        return operand.expression;
      return std::shared_ptr<const GenericValueExpression>(
          std::make_shared<GenericValueExpression>(GenericValueExpression{
              .kind = operand.kind == CompileTimeValue::Parameter
                          ? GenericValueExpression::Parameter
                          : GenericValueExpression::UInt64,
              .value = operand.value,
              .parameterId = operand.parameterId}));
    };
    if ((left.kind == CompileTimeValue::Unknown && !left.expression) ||
        (right.kind == CompileTimeValue::Unknown && !right.expression))
      return {};
    return {.expression = std::make_shared<GenericValueExpression>(
                GenericValueExpression{.kind = GenericValueExpression::Binary,
                                       .token = expression.token,
                                       .left = retain(left),
                                       .right = retain(right)})};
  };
  return evaluate(evaluate, *value.expression);
}

namespace {

void collectActiveProgramStatement(const Stmt *statement,
                                   const TargetInfo &target,
                                   std::vector<const Stmt *> &result);

void collectActiveProgramStatements(const StmtList &statements,
                                    const TargetInfo &target,
                                    std::vector<const Stmt *> &result) {
  for (const StmtPtr &statement : statements) {
    collectActiveProgramStatement(statement.get(), target, result);
  }
}

void collectActiveProgramStatement(const Stmt *statement,
                                   const TargetInfo &target,
                                   std::vector<const Stmt *> &result) {
  if (statement == nullptr) {
    return;
  }
  result.push_back(statement);
  if (const auto *block = stmtAs<BlockStmt>(statement)) {
    collectActiveProgramStatements(block->statements(), target, result);
  } else if (const auto *owner = stmtAs<ClassDecl>(statement)) {
    collectActiveProgramStatements(owner->members(), target, result);
  } else if (const auto *group = stmtAs<MutableFieldGroupDecl>(statement)) {
    collectActiveProgramStatements(group->members(), target, result);
  } else if (const auto *conditional = stmtAs<ConditionalStmt>(statement)) {
    if (const StmtList *branch = conditional->activeBranch(target)) {
      collectActiveProgramStatements(*branch, target, result);
    }
  } else if (const auto *constructor = stmtAs<ConstructorDecl>(statement)) {
    collectActiveProgramStatement(constructor->body().get(), target, result);
  } else if (const auto *destructor = stmtAs<DestructorDecl>(statement)) {
    collectActiveProgramStatement(destructor->body().get(), target, result);
  } else if (const auto *loop = stmtAs<DoWhileStmt>(statement)) {
    collectActiveProgramStatement(loop->body().get(), target, result);
  } else if (const auto *external = stmtAs<ExternCDecl>(statement)) {
    collectActiveProgramStatements(external->declarations(), target, result);
  } else if (const auto *loop = stmtAs<ForStmt>(statement)) {
    collectActiveProgramStatement(loop->initializer().get(), target, result);
    collectActiveProgramStatement(loop->body().get(), target, result);
  } else if (const auto *function = stmtAs<FunctionDecl>(statement)) {
    collectActiveProgramStatement(function->body().get(), target, result);
  } else if (const auto *branch = stmtAs<IfStmt>(statement)) {
    collectActiveProgramStatement(branch->thenBranch().get(), target, result);
    collectActiveProgramStatement(branch->elseBranch().get(), target, result);
  } else if (const auto *space = stmtAs<NamespaceDecl>(statement)) {
    collectActiveProgramStatements(space->declarations(), target, result);
  } else if (const auto *loop = stmtAs<RangeForStmt>(statement)) {
    collectActiveProgramStatement(loop->lowered().get(), target, result);
  } else if (const auto *loop = stmtAs<ConstexprForStmt>(statement)) {
    collectActiveProgramStatement(loop->elementBinding().get(), target, result);
    collectActiveProgramStatement(loop->body().get(), target, result);
  } else if (const auto *selection = stmtAs<SwitchStmt>(statement)) {
    for (const SwitchArm &arm : selection->arms()) {
      collectActiveProgramStatements(arm.statements, target, result);
    }
  } else if (const auto *binding = stmtAs<StructuredBindingDecl>(statement)) {
    for (const VariableDecl &element : binding->bindings()) {
      collectActiveProgramStatement(&element, target, result);
    }
  } else if (const auto *loop = stmtAs<WhileStmt>(statement)) {
    collectActiveProgramStatement(loop->body().get(), target, result);
  }
}

} // namespace

SemanticAnalysisSeal makeSemanticAnalysisSeal(const Program &program,
                                              const TargetInfo &target,
                                              const SourceGraph *sourceGraph) {
  SemanticAnalysisSeal result{.target = target};
  result.programSnapshot = program.snapshotId();
  collectActiveProgramStatements(program.declarations(), target,
                                 result.activeStatements);
  if (sourceGraph == nullptr) {
    return result;
  }
  result.sourceGraph.entry = sourceGraph->entryUnit();
  result.sourceGraph.preludeRoots = sourceGraph->preludeRoots();
  result.sourceGraph.units.reserve(sourceGraph->sourceUnits().size());
  for (const SourceUnit &unit : sourceGraph->sourceUnits()) {
    result.sourceGraph.units.push_back(
        {.id = unit.id,
         .path = unit.path.generic_string(),
         .declarationStart = unit.declarationStart,
         .declarationCount = unit.declarationCount,
         .standardLibraryName = unit.standardLibraryName,
         .packageIdentity = unit.packageIdentity,
         .packageRelativePath = unit.packageRelativePath,
         .role = unit.role,
         .entry = unit.entry,
         .prelude = unit.prelude});
  }
  result.sourceGraph.dependencies.reserve(
      sourceGraph->dependencyEdges().size());
  for (const SourceDependency &dependency : sourceGraph->dependencyEdges()) {
    result.sourceGraph.dependencies.push_back(
        {.source = dependency.source,
         .target = dependency.target,
         .kind = dependency.kind,
         .directiveSource = dependency.directive ? dependency.directive->source
                                                 : std::string{},
         .directiveStart =
             dependency.directive ? dependency.directive->start : 0,
         .directiveEnd = dependency.directive ? dependency.directive->end : 0,
         .directiveLine = dependency.directive ? dependency.directive->line : 0,
         .includeSpelling = dependency.includeSpelling,
         .includeOccurrence = dependency.includeOccurrence});
  }
  return result;
}

bool SemanticAnalysisSeal::matchesProgram(const Program &program,
                                          const TargetInfo &candidate) const {
  if (!matchesTarget(candidate)) {
    return false;
  }
  if (programSnapshot == 0 || programSnapshot != program.snapshotId()) {
    return false;
  }
  const SemanticAnalysisSeal candidateSeal =
      makeSemanticAnalysisSeal(program, candidate);
  return activeStatements == candidateSeal.activeStatements;
}

const ProgramInitializationStep *
ProgramInitializationPlan::findStepForSymbol(SymbolId symbol) const {
  const auto found =
      std::find_if(steps.begin(), steps.end(), [symbol](const auto &step) {
        return step.symbol == symbol;
      });
  return found == steps.end() ? nullptr : &*found;
}

[[nodiscard]] const std::vector<SymbolRecord> &
SemanticDatabase::symbols() const {
  return symbolRecords;
}

SymbolId SemanticDatabase::symbolIdentityCeiling() const {
  return baseSymbolCount + symbolRecords.size();
}

[[nodiscard]] const SymbolRecord *
SemanticDatabase::findSymbol(SymbolId id) const {
  if (id == 0) {
    return nullptr;
  }
  if (base != nullptr && id <= baseSymbolCount) {
    return base->findSymbol(noteBaseSymbol(id));
  }
  const SymbolId local = id - baseSymbolCount;
  if (local > symbolRecords.size())
    return nullptr;
  // An identity reserved by reserveIdentities belongs to an instance delta.
  const SymbolRecord &record = symbolRecords[local - 1];
  return record.sourceUnit == 0 ? nullptr : &record;
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
  baseSymbolReads.reset();
  frozenBaseSymbols.clear();
}

SymbolId SemanticDatabase::noteBaseSymbol(SymbolId id) const {
  if (baseSymbolReads && id != 0 && id <= baseSymbolCount)
    baseSymbolReads->insert(id);
  return id;
}

bool SemanticDatabase::freezeBaseSymbolReads() {
  if (!baseSymbolReads || base == nullptr)
    return false;
  std::vector<SymbolId> ids(baseSymbolReads->begin(), baseSymbolReads->end());
  baseSymbolReads.reset();
  std::sort(ids.begin(), ids.end());
  frozenBaseSymbols.clear();
  frozenBaseSymbols.reserve(ids.size());
  for (const SymbolId id : ids) {
    const SymbolRecord *record = base->findSymbol(id);
    if (record == nullptr)
      return false;
    frozenBaseSymbols.push_back(
        {id, DeclarationKey{record->sourceUnit, record->nameSpan.start,
                            record->nameSpan.end,
                            record->generated ? record->name : std::string{},
                            record->expansion}});
  }
  return true;
}

bool SemanticDatabase::baseSymbolReadsMatch(
    const SemanticDatabase &candidate) const {
  for (const auto &[id, key] : frozenBaseSymbols) {
    const SymbolRecord *record = candidate.findSymbol(id);
    if (record == nullptr || record->sourceUnit != key.sourceUnit ||
        record->nameSpan.start != key.start ||
        record->nameSpan.end != key.end || record->expansion != key.expansion ||
        (record->generated ? record->name : std::string{}) != key.generatedName)
      return false;
  }
  return true;
}

// Turns this database into an instance-analysis delta over baseDatabase:
// lookups fall back to the base, and new symbol identities continue after
// the base's so instance records never collide with base SymbolIds.
void SemanticDatabase::beginInstanceDelta(const SemanticDatabase &baseDatabase,
                                          SymbolId reservedIdentityCeiling) {
  clear();
  base = &baseDatabase;
  baseSymbolCount =
      std::max(baseDatabase.symbolIdentityCeiling(), reservedIdentityCeiling);
}

void SemanticDatabase::rebase(const SemanticDatabase *baseDatabase) {
  base = baseDatabase;
}

void SemanticDatabase::reserveIdentities(SymbolId ceiling) {
  while (symbolIdentityCeiling() < ceiling) {
    SymbolRecord placeholder;
    placeholder.id = symbolIdentityCeiling() + 1;
    placeholder.generated = true;
    placeholder.compilerPrivate = true;
    symbolRecords.push_back(std::move(placeholder));
  }
}

SymbolId SemanticDatabase::recordSymbol(SymbolRecord symbol) {
  if (symbol.sourceUnit == 0 || symbol.nameSpan.end <= symbol.nameSpan.start) {
    return 0;
  }
  const DeclarationKey key{
      symbol.sourceUnit, symbol.nameSpan.start, symbol.nameSpan.end,
      symbol.generated ? symbol.name : std::string{}, symbol.expansion};
  if (const auto found = symbolsByDeclaration.find(key);
      found != symbolsByDeclaration.end()) {
    SymbolRecord &existing = symbolRecords[found->second - baseSymbolCount - 1];
    if (existing.qualifiedName.empty()) {
      existing.qualifiedName = std::move(symbol.qualifiedName);
    }
    if (existing.type == SemanticType::Unknown &&
        symbol.type != SemanticType::Unknown) {
      existing.type = std::move(symbol.type);
      existing.traits = symbol.traits;
      existing.instanceDependentType = false;
    } else if (existing.type == SemanticType::Unknown) {
      existing.instanceDependentType =
          existing.instanceDependentType || symbol.instanceDependentType;
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
    if (!existing.documentationMarkdown && symbol.documentationMarkdown) {
      existing.documentationMarkdown = std::move(symbol.documentationMarkdown);
    }
    return found->second;
  }

  if (base != nullptr) {
    if (const auto inherited =
            base->symbolForDeclaration(symbol.sourceUnit, symbol.nameSpan,
                                       key.generatedName, symbol.expansion)) {
      // The declaration already has a base identity; instance analysis
      // reuses it and skips base-record enrichment (the delta is
      // discarded after lowering, so enrichment would be invisible).
      return noteBaseSymbol(inherited);
    }
  }

  symbol.id = baseSymbolCount + symbolRecords.size() + 1;
  const SymbolId id = symbol.id;
  symbolRecords.emplace_back(std::move(symbol));
  symbolsByDeclaration.emplace(key, id);
  return id;
}

void SemanticDatabase::setSymbolDocumentation(
    SymbolId symbol, std::string documentationMarkdown) {
  if (symbol == 0 || symbol <= baseSymbolCount) {
    return;
  }
  const SymbolId local = symbol - baseSymbolCount;
  if (local > symbolRecords.size()) {
    return;
  }
  symbolRecords[local - 1].documentationMarkdown =
      std::move(documentationMarkdown);
}

[[nodiscard]] SymbolId SemanticDatabase::symbolForDeclaration(
    SourceUnitId sourceUnit, const SourceSpan &span,
    std::string_view generatedName, std::uint32_t expansion) const {
  const auto found = symbolsByDeclaration.find(DeclarationKey{
      sourceUnit, span.start, span.end, std::string(generatedName), expansion});
  if (found != symbolsByDeclaration.end()) {
    return found->second;
  }
  return base == nullptr ? 0
                         : noteBaseSymbol(base->symbolForDeclaration(
                               sourceUnit, span, generatedName, expansion));
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
    finalizeUnit(unitOccurrences);
  }
}

void SemanticDatabase::finalize(const std::unordered_set<SourceUnitId> &units) {
  for (const SourceUnitId unit : units) {
    if (const auto found = occurrencesByUnit.find(unit);
        found != occurrencesByUnit.end())
      finalizeUnit(found->second);
  }
}

void SemanticDatabase::finalizeUnit(
    std::vector<SemanticOccurrence> &unitOccurrences) {
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
  std::size_t write = 0;
  for (std::size_t read = 0; read < unitOccurrences.size(); ++read) {
    SemanticOccurrence &occurrence = unitOccurrences[read];
    if (write != 0 && occurrence.symbol != 0 &&
        unitOccurrences[write - 1].span.start == occurrence.span.start &&
        unitOccurrences[write - 1].span.end == occurrence.span.end &&
        unitOccurrences[write - 1].kind == occurrence.kind &&
        unitOccurrences[write - 1].symbol == occurrence.symbol) {
      unitOccurrences[write - 1].roles |= occurrence.roles;
      continue;
    }
    if (write != read) {
      unitOccurrences[write] = std::move(occurrence);
    }
    ++write;
  }
  unitOccurrences.erase(unitOccurrences.begin() +
                            static_cast<std::ptrdiff_t>(write),
                        unitOccurrences.end());
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

const CallerLocationOrigin *
SemanticModel::findCallerLocation(const Expr &expression) const {
  const auto found = callerLocations.find(&expression);
  if (found != callerLocations.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findCallerLocation(expression);
}

const SemanticAnalysisSeal &SemanticModel::analysisSeal() const {
  return base == nullptr ? semanticAnalysisSeal : base->analysisSeal();
}

std::optional<SemanticCallerLocationProof>
SemanticModel::callerLocationProof(const Expr &expression) const {
  const auto *origin = findCallerLocation(expression);
  if (origin == nullptr)
    return std::nullopt;
  return SemanticCallerLocationProof(expression, *origin,
                                     analysisSeal().programSnapshot);
}

std::optional<SemanticOwnerlessTextContractProof>
SemanticModel::ownerlessTextContractProof(
    const FunctionDecl &declaration) const {
  const auto *function = findFunction(declaration);
  if (!function || !function->bodyDeclaration())
    return std::nullopt;
  return SemanticOwnerlessTextContractProof(*function,
                                            analysisSeal().programSnapshot);
}

std::optional<SemanticNoNormalBorrowResultProof>
SemanticModel::noNormalBorrowResultProof(const Call &call) const {
  const auto *selected = findCall(call);
  if (!selected || !selected->noNormalBorrowResult ||
      selected->intrinsic != IntrinsicKind::None || !selected->function ||
      selected->dispatch != CallDispatch::Static || !selected->neverReturns)
    return std::nullopt;
  return SemanticNoNormalBorrowResultProof(call, *selected,
                                           analysisSeal().programSnapshot);
}

const RawReferenceAssertionInfo *
SemanticModel::findRawReferenceAssertion(const Expr &expression) const {
  const auto found = rawReferenceAssertions.find(&expression);
  if (found != rawReferenceAssertions.end())
    return &found->second;
  return base == nullptr ? nullptr
                         : base->findRawReferenceAssertion(expression);
}

void SemanticModel::recordRawReferenceAssertion(
    const Expr &expression, RawReferenceAssertionInfo info) {
  rawReferenceAssertions.insert_or_assign(&expression, std::move(info));
}

const ProgramInitializationPlan &
SemanticModel::programInitializationPlan() const {
  return base != nullptr && programInitialization.steps.empty() &&
                 programInitialization.unitOrder.empty()
             ? base->programInitializationPlan()
             : programInitialization;
}

const std::optional<HostedProgramEntryPlan> &
SemanticModel::hostedProgramEntryPlan() const {
  return base != nullptr && !hostedProgramEntry ? base->hostedProgramEntryPlan()
                                                : hostedProgramEntry;
}

ProgramKind SemanticModel::programKind() const { return programKind_; }

const std::optional<SemanticEmbeddingPlan> &
SemanticModel::embeddingPlan() const {
  return embeddingPlan_;
}

bool SemanticModel::hasNativeFunctionConversions() const {
  return !nativeFunctionConversions.empty() ||
         (base != nullptr && base->hasNativeFunctionConversions());
}

bool SemanticModel::isProgramConstantSubstitution(
    const Expr &expression) const {
  return programConstantSubstitutions.contains(&expression) ||
         (base != nullptr && base->isProgramConstantSubstitution(expression));
}

[[nodiscard]] const PlaceKey *
SemanticModel::findPlace(const Expr &expression) const {
  const auto found = places.find(&expression);
  if (found != places.end()) {
    return &found->second;
  }
  const PlaceKey *inherited =
      base == nullptr ? nullptr : base->findPlace(expression);
  if (inherited != nullptr)
    (void)semanticDatabase.noteBaseSymbol(inherited->root);
  return inherited;
}

[[nodiscard]] const OwnershipEvent *
SemanticModel::findOwnershipEvent(const Expr &expression) const {
  const auto found = ownershipEvents.find(&expression);
  if (found != ownershipEvents.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findOwnershipEvent(expression);
}

[[nodiscard]] const ImplicitReturnMoveInfo *
SemanticModel::findImplicitReturnMove(const Expr &expression) const {
  const auto found = implicitReturnMoves.find(&expression);
  // This decision depends on concrete copy/move traits. An instance delta's
  // absence must therefore shadow, rather than inherit, a symbolic base fact.
  return found == implicitReturnMoves.end() ? nullptr : &found->second;
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
    return true;
  case SemanticType::Class: {
    const ClassTypeInfo *info = findClassType(type.classId);
    // The opaque referent identity may cross a public checked-reference API.
    // Its private declaration spelling and erase/restore operations remain
    // trusted; unlike a storage implementation type, the referent itself is
    // the intended public capability and has no constructible object value.
    if (info != nullptr &&
        info->compilerCapability == CompilerCapabilityTypeKind::ErasedObject)
      return false;
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
  if (occurrence.selectedCall && occurrence.selectedCall->argumentReceiver) {
    // A normalized builtin member is public syntax over a private source
    // implementation. Only its exact trusted declaration is presentable via
    // this occurrence; ordinary private names remain hidden.
    const auto *function = findFunction(occurrence.selectedCall->function);
    const auto *unit =
        function ? sourceGraph.findUnit(function->sourceUnit) : nullptr;
    if (unit != nullptr && unit->role == SourceUnitRole::StandardLibrary &&
        unit->standardLibraryName == "std/string_view")
      return true;
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
  const BindingInfo *inherited =
      base == nullptr ? nullptr : base->findBinding(declaration);
  if (inherited != nullptr)
    (void)semanticDatabase.noteBaseSymbol(inherited->symbol);
  return inherited;
}

const ExprPtr &
SemanticModel::implicitFieldInitializer(const VariableDecl &field) const {
  const auto found = implicitFieldInitializers.find(&field);
  if (found != implicitFieldInitializers.end())
    return *found->second;
  if (base != nullptr) {
    bool shared = false;
    if (const auto *inherited =
            base->findImplicitFieldInitializer(field, &shared)) {
      // A snapshot-owned anchor is recreated at another address by the next
      // snapshot; a shared one is validated when a delta is reused.
      if (reuseJournal && shared)
        reuseJournal->sharedAnchors.insert(&field);
      else
        noteBaseSyntheticSyntax(true);
      return **inherited;
    }
  }
  static const ExprPtr absent;
  return absent;
}

const std::shared_ptr<const ExprPtr> *
SemanticModel::findImplicitFieldInitializer(const VariableDecl &field,
                                            bool *shared) const {
  for (const SemanticModel *model = this; model != nullptr;
       model = model->base) {
    const auto found = model->implicitFieldInitializers.find(&field);
    if (found != model->implicitFieldInitializers.end()) {
      if (shared != nullptr)
        *shared = model->sharedImplicitFieldInitializers.contains(&field);
      return &found->second;
    }
  }
  return nullptr;
}

void SemanticModel::retainImplicitFieldInitializer(
    const VariableDecl &field, std::shared_ptr<const ExprPtr> anchor) {
  if (implicitFieldInitializer(field) || anchor == nullptr || !*anchor)
    return;
  implicitFieldInitializers.emplace(&field, std::move(anchor));
  sharedImplicitFieldInitializers.insert(&field);
}

void SemanticModel::retainImplicitFieldInitializer(const VariableDecl &field) {
  if (implicitFieldInitializer(field))
    return;
  implicitFieldInitializers.emplace(
      &field,
      std::make_shared<const ExprPtr>(std::make_unique<DirectInitializer>(
          field.name(), ExprList{}, field.name())));
}

const ExprPtr &
SemanticModel::resolvedFieldInitializer(const VariableDecl &field) const {
  if (field.initializer())
    return field.initializer();
  const ExprPtr &implicit = implicitFieldInitializer(field);
  if (implicit && findConstruction(*implicit) != nullptr)
    return implicit;
  static const ExprPtr absent;
  return absent;
}

[[nodiscard]] const BindingInfo *
SemanticModel::findBinding(const Parameter &parameter) const {
  const auto found = parameterBindings.find(&parameter);
  if (found != parameterBindings.end()) {
    return &found->second;
  }
  const BindingInfo *inherited =
      base == nullptr ? nullptr : base->findBinding(parameter);
  if (inherited != nullptr)
    (void)semanticDatabase.noteBaseSymbol(inherited->symbol);
  return inherited;
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

[[nodiscard]] const ConstexprForExpansion *
SemanticModel::findConstexprForExpansion(
    const ConstexprForStmt &statement) const {
  const auto found = constexprForExpansions.find(&statement);
  if (found != constexprForExpansions.end()) {
    return found->second ? &*found->second : nullptr;
  }
  const ConstexprForExpansion *inherited =
      base == nullptr ? nullptr : base->findConstexprForExpansion(statement);
  noteBaseSyntheticSyntax(inherited != nullptr &&
                          !baseStableSyntheticSyntax(&statement));
  return inherited;
}

const Stmt *
SemanticModel::findMacroExpansion(const ExpressionStmt &statement) const {
  if (const auto found = macroExpansions.find(&statement);
      found != macroExpansions.end())
    return found->second.get();
  const Stmt *inherited =
      base == nullptr ? nullptr : base->findMacroExpansion(statement);
  noteBaseSyntheticSyntax(inherited != nullptr &&
                          !baseStableSyntheticSyntax(&statement));
  return inherited;
}

std::shared_ptr<const Stmt>
SemanticModel::shareMacroExpansion(const ExpressionStmt &statement) const {
  if (const auto found = macroExpansions.find(&statement);
      found != macroExpansions.end())
    return found->second;
  auto inherited =
      base == nullptr ? nullptr : base->shareMacroExpansion(statement);
  noteBaseSyntheticSyntax(inherited != nullptr &&
                          !baseStableSyntheticSyntax(&statement));
  return inherited;
}

const Token *SemanticModel::findMacroDefault(const Expr &expression) const {
  if (const auto found = macroDefaults.find(&expression);
      found != macroDefaults.end())
    return &found->second;
  return base == nullptr ? nullptr : base->findMacroDefault(expression);
}

std::shared_ptr<const Stmt>
SemanticModel::shareMacroExpansion(const Call &call) const {
  if (const auto found = macroValueExpansions.find(&call);
      found != macroValueExpansions.end())
    return found->second;
  auto inherited = base == nullptr ? nullptr : base->shareMacroExpansion(call);
  noteBaseSyntheticSyntax(inherited != nullptr &&
                          !baseStableSyntheticSyntax(&call));
  return inherited;
}

std::vector<MacroExpansionSite> SemanticModel::macroExpansionSites() const {
  std::vector<MacroExpansionSite> sites;
  std::unordered_set<const ExpressionStmt *> seen;
  for (const SemanticModel *model = this; model != nullptr; model = model->base)
    for (const auto &[invocation, body] : model->macroExpansions) {
      if (!seen.insert(invocation).second || !body)
        continue;
      const auto declaration = model->macroDeclarations.find(invocation);
      sites.push_back({invocation, body.get(),
                       declaration == model->macroDeclarations.end()
                           ? nullptr
                           : declaration->second});
    }
  std::unordered_set<const Call *> calls;
  for (const SemanticModel *model = this; model != nullptr; model = model->base)
    for (const auto &[call, body] : model->macroValueExpansions) {
      if (!calls.insert(call).second || !body)
        continue;
      const auto declaration = model->macroValueDeclarations.find(call);
      sites.push_back({nullptr, body.get(),
                       declaration == model->macroValueDeclarations.end()
                           ? nullptr
                           : declaration->second,
                       call});
    }
  return sites;
}

[[nodiscard]] const std::vector<ResolvedDeclarationAttribute> *
SemanticModel::findDeclarationAttributes(const Stmt &declaration) const {
  const auto found = declarationAttributes.find(&declaration);
  if (found != declarationAttributes.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr
                         : base->findDeclarationAttributes(declaration);
}

[[nodiscard]] const Expr *
SemanticModel::findExpressionAlias(const Expr &expression) const {
  const auto found = expressionAliases.find(&expression);
  if (found != expressionAliases.end()) {
    return found->second.get();
  }
  const Expr *inherited =
      base == nullptr ? nullptr : base->findExpressionAlias(expression);
  noteBaseSyntheticSyntax(inherited != nullptr &&
                          !baseStableSyntheticSyntax(&expression));
  return inherited;
}

[[nodiscard]] std::optional<StaticAssertResult>
SemanticModel::findStaticAssertResult(
    const StaticAssertDecl &declaration) const {
  const auto found = staticAssertResults.find(&declaration);
  if (found != staticAssertResults.end()) {
    return found->second;
  }
  return base == nullptr ? std::nullopt
                         : base->findStaticAssertResult(declaration);
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

[[nodiscard]] const ResolvedRangeIterationInfo *
SemanticModel::findRangeIteration(const RangeForStmt &statement) const {
  const auto found = rangeIterations.find(&statement);
  if (found != rangeIterations.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findRangeIteration(statement);
}

const FunctionDecl &SemanticModel::canonicalFunctionDeclaration(
    const FunctionDecl &declaration) const {
  if (const auto alias = functionDeclarations.find(&declaration);
      alias != functionDeclarations.end()) {
    return *alias->second;
  }
  return base == nullptr ? declaration
                         : base->canonicalFunctionDeclaration(declaration);
}

[[nodiscard]] const FunctionInfo *
SemanticModel::findFunction(const FunctionDecl &declaration) const {
  const auto &canonical = canonicalFunctionDeclaration(declaration);
  const auto found = functions.find(&canonical);
  if (found != functions.end()) {
    if (functionDemandSink != nullptr) {
      functionDemandSink->insert(found->second.id);
    }
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findFunction(canonical);
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
  const LambdaInfo *inherited =
      base == nullptr ? nullptr : base->findLambda(declaration);
  noteBaseLambda(inherited);
  return inherited;
}

[[nodiscard]] const LambdaInfo *SemanticModel::findLambda(LambdaId id) const {
  const auto found = lambdasById.find(id);
  if (found != lambdasById.end() && found->second != nullptr) {
    return findLambda(*found->second);
  }
  if (const auto discovery = lambdaTypeDiscoveries.find(id);
      discovery != lambdaTypeDiscoveries.end())
    return &discovery->second;
  const LambdaInfo *inherited =
      base == nullptr ? nullptr : base->findLambda(id);
  noteBaseLambda(inherited);
  return inherited;
}

[[nodiscard]] std::vector<LambdaCaptureLink>
SemanticModel::lambdaCaptureLinks() const {
  std::vector<LambdaCaptureLink> links = base == nullptr
                                             ? std::vector<LambdaCaptureLink>{}
                                             : base->lambdaCaptureLinks();
  for (const auto &[_, lambda] : lambdas) {
    for (const LambdaCaptureInfo &capture : lambda.captures) {
      if (capture.sourceSymbol != 0 && capture.bindingSymbol != 0) {
        links.push_back({.source = capture.sourceSymbol,
                         .binding = capture.bindingSymbol,
                         .mode = capture.mode});
      }
    }
  }
  return links;
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

[[nodiscard]] SemanticType
SemanticModel::resolveExactClassSpecialization(SemanticType type) const {
  for (SemanticType &argument : type.arguments) {
    argument = resolveExactClassSpecialization(std::move(argument));
  }
  for (SemanticType &argument : type.lambdaEnclosingClassTypes) {
    argument = resolveExactClassSpecialization(std::move(argument));
  }
  for (SemanticType &argument : type.lambdaEnclosingFunctionTypes) {
    argument = resolveExactClassSpecialization(std::move(argument));
  }
  if (type.kind != SemanticType::Class || type.classId == 0) {
    return type;
  }
  // Only a recorded specialization of the root's canonical primary can
  // match. Canonicalizing the complete key for every nested class made
  // resolving a deeply nested generic quadratic in its depth.
  ClassId primary = type.classId;
  for (const ClassTypeInfo *info = findClassType(primary);
       info != nullptr && info->exactSpecializationPrimary != 0;
       info = findClassType(primary)) {
    primary = info->exactSpecializationPrimary;
  }
  bool specialized = false;
  for (const SemanticModel *model = this; model != nullptr && !specialized;
       model = model->base) {
    specialized =
        std::any_of(model->exactClassSpecializations.begin(),
                    model->exactClassSpecializations.end(),
                    [&](const ExactClassSpecializationRecord &candidate) {
                      return candidate.primary == primary;
                    });
  }
  if (!specialized) {
    return type;
  }

  const auto canonicalize = [&](const auto &self,
                                SemanticType value) -> SemanticType {
    for (SemanticType &argument : value.arguments) {
      argument = self(self, std::move(argument));
    }
    for (SemanticType &argument : value.lambdaEnclosingClassTypes) {
      argument = self(self, std::move(argument));
    }
    for (SemanticType &argument : value.lambdaEnclosingFunctionTypes) {
      argument = self(self, std::move(argument));
    }
    if (value.kind != SemanticType::Class || value.classId == 0) {
      return value;
    }
    const ClassTypeInfo *info = findClassType(value.classId);
    if (info == nullptr || info->exactSpecializationPrimary == 0) {
      return value;
    }
    SemanticType primary = SemanticType::classType(
        info->exactSpecializationPrimary, info->exactTypeArguments,
        info->exactValueArguments);
    return self(self, std::move(primary));
  };
  const SemanticType key = canonicalize(canonicalize, type);

  const auto find = [&](const auto &self,
                        const SemanticModel *model) -> ClassId {
    if (model == nullptr) {
      return 0;
    }
    const auto found =
        std::find_if(model->exactClassSpecializations.begin(),
                     model->exactClassSpecializations.end(),
                     [&](const ExactClassSpecializationRecord &candidate) {
                       return candidate.primary == key.classId &&
                              candidate.typeArguments == key.arguments &&
                              candidate.valueArguments == key.valueArguments;
                     });
    return found == model->exactClassSpecializations.end()
               ? self(self, model->base)
               : found->specialization;
  };
  const ClassId specialization = find(find, this);
  return specialization == 0 ? type : SemanticType::classType(specialization);
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

std::optional<EnumTypeInfo>
SemanticModel::enumTypeFor(const SemanticType &type) const {
  if (type.kind != SemanticType::Enum)
    return std::nullopt;
  const auto found =
      std::find_if(enumInstanceTypes.begin(), enumInstanceTypes.end(),
                   [&](const auto &row) { return row.first == type; });
  if (found != enumInstanceTypes.end())
    return found->second;
  const auto *primary = findEnumType(type.enumId);
  if (primary != nullptr && primary->genericParameters.empty() &&
      type == SemanticType::enumType(primary->id))
    return *primary;
  return base == nullptr ? std::nullopt : base->enumTypeFor(type);
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

[[nodiscard]] const BindingInfo *
SemanticModel::findConsumedSwitchSubject(const SwitchStmt &statement) const {
  const auto found = consumedSwitchSubjects.find(&statement);
  if (found != consumedSwitchSubjects.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findConsumedSwitchSubject(statement);
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
  if (suppressedInheritedCalls.contains(&call)) {
    return nullptr;
  }
  return base == nullptr ? nullptr : base->findCall(call);
}

[[nodiscard]] const NativeFunctionConversionInfo *
SemanticModel::findNativeFunctionConversion(const Expr &expression) const {
  const auto found = nativeFunctionConversions.find(&expression);
  if (found != nativeFunctionConversions.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr
                         : base->findNativeFunctionConversion(expression);
}

[[nodiscard]] const FunctionItemSelectionInfo *
SemanticModel::findFunctionItemSelection(const Expr &expression) const {
  const auto found = functionItemSelections.find(&expression);
  if (found != functionItemSelections.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr
                         : base->findFunctionItemSelection(expression);
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
  if (suppressedInheritedOperators.contains(&expression)) {
    return nullptr;
  }
  return base == nullptr ? nullptr : base->findOperator(expression);
}

[[nodiscard]] const ResolvedOperatorInfo *
SemanticModel::findProjectionOperator(const Expr &expression) const {
  const auto found = projectionOperators.find(&expression);
  if (found != projectionOperators.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findProjectionOperator(expression);
}

[[nodiscard]] const ResolvedOperatorInfo *
SemanticModel::findContextualConversion(const Expr &expression) const {
  const auto found = contextualConversions.find(&expression);
  if (found != contextualConversions.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findContextualConversion(expression);
}

bool SemanticModel::recordsContextualConversion(const Expr &expression) const {
  return contextualConversions.contains(&expression);
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

const Binary *SemanticModel::builtinComma(const Expr &expression) const {
  const auto *binary = exprAs<Binary>(&expression);
  return binary != nullptr && binary->oper().kind == TokenKind::COMMA &&
                 binary->right() != nullptr &&
                 findOperator(expression) == nullptr
             ? binary
             : nullptr;
}

bool SemanticModel::isFreshClassResult(const Expr &expression) const {
  const auto *info = findExpression(expression);
  if (info == nullptr || info->type.kind != SemanticType::Class ||
      info->category != ValueCategory::Value)
    return false;
  if (const auto *group = exprAs<Grouping>(&expression))
    return isFreshClassResult(*group->expression());
  if (const auto *comma = builtinComma(expression))
    return isFreshClassResult(*comma->right());
  if (const auto *construction = findConstruction(expression))
    return construction->constructedType == info->type;
  if (const auto *operation = findOperator(expression))
    return operation->returnType == info->type;
  if (const auto *call = exprAs<Call>(&expression)) {
    if (const auto *lambda = findLambdaCall(*call))
      return lambda->returnType == info->type;
    if (const auto *deferred = findDeferredCallableCall(*call))
      return deferred->returnType == info->type;
    const auto *resolved = findCall(*call);
    return resolved != nullptr && resolved->returnType == info->type &&
           resolved->intrinsic != IntrinsicKind::Move;
  }
  return false;
}

const Expr *
SemanticModel::directInitializationSource(const Expr &expression) const {
  // A grouping, or a comma after evaluating its left operand, denotes its
  // operand's object value itself: no second object, copy or obligation.
  const auto *info = findExpression(expression);
  const bool objectValue = info != nullptr &&
                           info->category == ValueCategory::Value &&
                           info->type.kind == SemanticType::Class;
  if (const auto *group = exprAs<Grouping>(&expression)) {
    if (objectValue)
      return group->expression().get();
  }
  if (const auto *comma = builtinComma(expression)) {
    if (objectValue)
      return comma->right().get();
  }
  const auto *construction = findConstruction(expression);
  if (construction == nullptr || !construction->directInitialization)
    return nullptr;
  if (const auto *call = exprAs<Call>(&expression);
      call != nullptr && call->arguments().size() == 1)
    return call->arguments().front().get();
  if (const auto *direct = exprAs<DirectInitializer>(&expression);
      direct != nullptr && direct->arguments().size() == 1)
    return direct->arguments().front().get();
  return nullptr;
}

std::optional<SemanticDirectInitializationProof>
SemanticModel::directInitializationProof(const Expr &expression) const {
  const auto *selected = findConstruction(expression);
  const auto *info = findExpression(expression);
  const auto *producer = directInitializationSource(expression);
  if (selected == nullptr || !selected->directInitialization ||
      selected->kind != ConstructorKind::Ordinary ||
      selected->constructor != 0 || selected->declaration != nullptr ||
      selected->generatedDefault || !selected->defaultArguments.empty() ||
      !selected->designatedArguments.empty() || info == nullptr ||
      info->type.kind != SemanticType::Class ||
      info->category != ValueCategory::Value ||
      selected->constructedType != info->type ||
      selected->parameterTypes.size() != 1 ||
      selected->parameterTypes.front() != info->type || producer == nullptr ||
      !isFreshClassResult(*producer))
    return std::nullopt;
  const auto *input = findExpression(*producer);
  if (input == nullptr || input->type != info->type)
    return std::nullopt;
  return SemanticDirectInitializationProof(expression, *producer, info->type,
                                           analysisSeal().programSnapshot);
}

[[nodiscard]] const ResolvedConstructionInfo *
SemanticModel::findArrayElementConstruction(
    const ArrayInitializer &initializer) const {
  const auto found = arrayElementConstructions.find(&initializer);
  if (found != arrayElementConstructions.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr
                         : base->findArrayElementConstruction(initializer);
}

[[nodiscard]] const ResolvedConstructionInfo *
SemanticModel::findContextualConstruction(const Expr &initializer) const {
  const auto found = contextualConstructions.find(&initializer);
  if (found != contextualConstructions.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr
                         : base->findContextualConstruction(initializer);
}

std::optional<SemanticContextualConstructionProof>
SemanticModel::contextualConstructionProof(const Expr &initializer) const {
  const auto *construction = findContextualConstruction(initializer);
  if (construction == nullptr)
    return std::nullopt;
  return SemanticContextualConstructionProof(initializer, *construction,
                                             analysisSeal().programSnapshot);
}

[[nodiscard]] bool
SemanticModel::recordsContextualConstruction(const Expr &initializer) const {
  return contextualConstructions.contains(&initializer);
}

[[nodiscard]] std::optional<Token>
SemanticModel::contextualConstructionSite(const Expr &initializer) const {
  const auto found = contextualConstructionSites.find(&initializer);
  if (found != contextualConstructionSites.end()) {
    return found->second;
  }
  return base == nullptr ? std::nullopt
                         : base->contextualConstructionSite(initializer);
}

const ResolvedCollectionInfo *
SemanticModel::findCollection(const ArrayInitializer &initializer) const {
  const auto found = collections.find(&initializer);
  if (found != collections.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findCollection(initializer);
}

void SemanticModel::recordCollection(const ArrayInitializer &initializer,
                                     ResolvedCollectionInfo info) {
  collections.insert_or_assign(&initializer, std::move(info));
}

const ProtocolConceptSpelling *
SemanticModel::findProtocolConcept(ConceptId id) const {
  const auto found = protocolConcepts.find(id);
  if (found != protocolConcepts.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findProtocolConcept(id);
}

void SemanticModel::recordProtocolConcept(ConceptId id,
                                          ProtocolConceptSpelling spelling) {
  protocolConcepts.insert_or_assign(id, std::move(spelling));
}

const ResolvedTryablePropagationInfo *
SemanticModel::findTryablePropagation(const Unary &propagation) const {
  const auto found = tryablePropagations.find(&propagation);
  if (found != tryablePropagations.end()) {
    return &found->second;
  }
  return base == nullptr ? nullptr : base->findTryablePropagation(propagation);
}

void SemanticModel::recordTryablePropagation(
    const Unary &propagation, ResolvedTryablePropagationInfo info) {
  tryablePropagations.insert_or_assign(&propagation, std::move(info));
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
  return base == nullptr ? 0
                         : semanticDatabase.noteBaseSymbol(
                               base->findResolvedSymbol(expression));
}

[[nodiscard]] std::size_t SemanticModel::functionCount() const {
  return functions.size() + (base == nullptr ? 0 : base->functionCount());
}

std::optional<Diagnostic>
SemanticModel::executableEntryDiagnostic(SourceUnitId entryUnit,
                                         const std::string &entryPath) const {
  std::vector<const FunctionInfo *> candidates;
  for (const auto &[declaration, function] : functions) {
    if (function.entryPoint && function.sourceUnit == entryUnit &&
        declaration->body()) {
      return std::nullopt;
    }
    if (function.sourceUnit == entryUnit && function.ownerClass == 0 &&
        declaration->name().lexeme == "main" && declaration->body()) {
      candidates.push_back(&function);
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const auto *a, const auto *b) {
              return a->declaration->name().position <
                     b->declaration->name().position;
            });
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2094", DiagnosticPhase::Semantics,
      candidates.empty() ? SourceSpan{entryPath, 0, 0, 1}
                         : tokenSpan(candidates.front()->declaration->name()),
      "Executable requires a global 'main' definition in its entry source "
      "unit.");
  for (const FunctionInfo *candidate : candidates) {
    diagnostic.related.push_back(
        {tokenSpan(candidate->declaration->name()),
         "'" + candidate->qualifiedName +
             "' is a namespaced function, not the program entry point."});
  }
  diagnostic.hints.emplace_back(
      "Define 'int main()' or 'int main(int, std::vector<std::string>)' "
      "outside all namespaces in the executable's entry file; it may forward "
      "to a namespaced function.");
  return diagnostic;
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
  return constructions.size() + arrayElementConstructions.size() +
         contextualConstructions.size() +
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
  if (const GenericParameterInfo *parameter = find(enumTypes)) {
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
  foreignStorage_.clear();
  analyzedFunctionBodies.clear();
  expressions.clear();
  statementFullExpressions.clear();
  constructorFullExpressions.clear();
  fullExpressionOrder.clear();
  semanticAnalysisSeal = {};
  constants.clear();
  unsafeOperations.clear();
  rawReferenceAssertions.clear();
  definedFailures.clear();
  callerLocations.clear();
  programInitialization = {};
  hostedProgramEntry.reset();
  programKind_ = ProgramKind::Executable;
  embeddingPlan_.reset();
  programConstantSubstitutions.clear();
  places.clear();
  ownershipEvents.clear();
  implicitReturnMoves.clear();
  ownershipEventOrder.clear();
  placeSelections.clear();
  compilerCapabilityTypes.clear();
  arrayExtents.clear();
  variableBindings.clear();
  parameterBindings.clear();
  payloadBindings.clear();
  retainedLoans.clear();
  expressionBorrowDependencies.clear();
  captureBorrowDependencies.clear();
  loanEnds.clear();
  conditionalLoanEnds.clear();
  constexprBranches.clear();
  constexprForExpansions.clear();
  macroExpansions.clear();
  macroValueExpansions.clear();
  macroValueDeclarations.clear();
  macroDeclarations.clear();
  macroDefaults.clear();
  declarationAttributes.clear();
  expressionAliases.clear();
  staticAssertResults.clear();
  switchArmLoanEnds.clear();
  structuredBindings.clear();
  rangeIterations.clear();
  functions.clear();
  functionsById.clear();
  functionDeclarations.clear();
  lambdas.clear();
  lambdasById.clear();
  lambdaTypeDiscoveries.clear();
  classTypes.clear();
  classTypesById.clear();
  exactClassSpecializations.clear();
  typeAliases.clear();
  enumTypes.clear();
  enumInstanceTypes.clear();
  enumTypesById.clear();
  enumerators.clear();
  switchCases.clear();
  exhaustiveSwitches.clear();
  consumedSwitchSubjects.clear();
  payloadConstructions.clear();
  payloadPatterns.clear();
  calls.clear();
  suppressedInheritedCalls.clear();
  packFolds.clear();
  lambdaCalls.clear();
  deferredCallableCalls.clear();
  pendingCallableForwardings.clear();
  operators.clear();
  suppressedInheritedOperators.clear();
  projectionOperators.clear();
  contextualConversions.clear();
  contextualIntegerOperands.clear();
  classLifecycles.clear();
  implicitFieldInitializers.clear();
  sharedImplicitFieldInitializers.clear();
  designatedConstructors.clear();
  constructions.clear();
  contextualConstructions.clear();
  contextualConstructionSites.clear();
  emptyReturnConstructions.clear();
  collections.clear();
  protocolConcepts.clear();
  tryablePropagations.clear();
  constructorInitializers.clear();
  resolvedSymbols.clear();
  semanticDatabase.clear();
  completion.reset();
  executionProfile_ = ExecutionProfile::SingleThreaded;
  placeSnapshot_ = 0;
  base = nullptr;
  reuseJournal.reset();
  frozenBaseLambdas.clear();
  frozenBaseAnchors.clear();
}

void SemanticModel::setExecutionProfile(ExecutionProfile profile) {
  executionProfile_ = profile;
}

void SemanticModel::setPlaceSnapshot(std::size_t snapshot) {
  placeSnapshot_ = snapshot;
}

void SemanticModel::setAnalysisSeal(SemanticAnalysisSeal seal) {
  semanticAnalysisSeal = std::move(seal);
}

void SemanticModel::setProgramInitializationPlan(
    ProgramInitializationPlan plan) {
  programInitialization = std::move(plan);
}

void SemanticModel::setHostedProgramEntryPlan(
    std::optional<HostedProgramEntryPlan> plan) {
  hostedProgramEntry = std::move(plan);
}

void SemanticModel::setProgramKind(ProgramKind kind) { programKind_ = kind; }

void SemanticModel::setEmbeddingPlan(
    std::optional<SemanticEmbeddingPlan> plan) {
  embeddingPlan_ = std::move(plan);
}

void SemanticModel::recordProgramConstantSubstitution(const Expr &expression) {
  programConstantSubstitutions.insert(&expression);
}

void SemanticModel::clearProgramConstantSubstitution(const Expr &expression) {
  programConstantSubstitutions.erase(&expression);
}

// Turns this model into an instance-analysis delta over baseModel: reads
// fall back to the base while writes stay local, so concrete instance
// reanalysis records only what it produces instead of copying the whole
// program's model. Loan tables deliberately do not fall back - instance
// analysis restarts loan identities, mirroring the clearLoans() semantics
// the previous whole-model copy relied on.
void SemanticModel::beginInstanceDelta(const SemanticModel &baseModel,
                                       SymbolId reservedIdentityCeiling) {
  clear();
  callJournal = nullptr;
  executionProfile_ = baseModel.executionProfile();
  placeSnapshot_ = baseModel.placeSnapshot();
  programKind_ = baseModel.programKind();
  embeddingPlan_ = baseModel.embeddingPlan();
  base = &baseModel;
  semanticDatabase.beginInstanceDelta(baseModel.semanticDatabase,
                                      reservedIdentityCeiling);
}

void SemanticModel::beginSpeculativeDelta(const SemanticModel &baseModel) {
  beginInstanceDelta(baseModel);
  semanticAnalysisSeal = baseModel.analysisSeal();
  retainedLoans = baseModel.retainedLoans;
  setToolingOccurrencesEnabled(baseModel.semanticDatabase.toolingOccurrences);
}

// Re-points an instance delta at a relocated base (the analyzer restores
// its model after each instance analysis; the delta must follow it).
void SemanticModel::rebase(const SemanticModel *baseModel) {
  base = baseModel;
  semanticDatabase.rebase(baseModel == nullptr ? nullptr
                                               : &baseModel->semanticDatabase);
}

void SemanticModel::beginInstanceReuseJournal() {
  reuseJournal.emplace();
  frozenBaseLambdas.clear();
  semanticDatabase.baseSymbolReads.emplace();
  semanticDatabase.frozenBaseSymbols.clear();
}

void SemanticModel::endInstanceReuseJournal() {
  reuseJournal.reset();
  semanticDatabase.baseSymbolReads.reset();
}

void SemanticModel::noteBaseLambda(const LambdaInfo *lambda) const {
  if (reuseJournal && lambda != nullptr)
    reuseJournal->lambdas.insert(lambda->id);
}

void SemanticModel::noteBaseSyntheticSyntax(bool used) const {
  if (reuseJournal && used)
    reuseJournal->baseSyntheticSyntax = true;
}

bool SemanticModel::baseOwnsGeneratedConstructor(
    const ConstructorDecl *declaration) const {
  for (const SemanticModel *model = base; model != nullptr;
       model = model->base) {
    if (model->stableSyntheticSyntax.contains(declaration))
      return false;
    for (const auto &generated : model->designatedConstructors)
      if (generated.get() == declaration)
        return true;
  }
  return false;
}

void SemanticModel::markSyntheticSyntaxStable() {
  for (const auto &generated : designatedConstructors)
    stableSyntheticSyntax.insert(generated.get());
  for (const auto &[statement, expansion] : constexprForExpansions)
    if (expansion)
      stableSyntheticSyntax.insert(statement);
  for (const auto &[statement, expansion] : macroExpansions)
    if (expansion)
      stableSyntheticSyntax.insert(statement);
  for (const auto &[call, expansion] : macroValueExpansions)
    if (expansion)
      stableSyntheticSyntax.insert(call);
  for (const auto &[expression, alias] : expressionAliases)
    stableSyntheticSyntax.insert(expression);
  for (const auto &[field, anchor] : implicitFieldInitializers)
    sharedImplicitFieldInitializers.insert(field);
}

bool SemanticModel::baseStableSyntheticSyntax(const void *object) const {
  for (const SemanticModel *model = base; model != nullptr; model = model->base)
    if (model->stableSyntheticSyntax.contains(object))
      return true;
  return false;
}

void SemanticModel::noteConstruction(const ConstructorDecl *declaration) {
  if (reuseJournal && declaration != nullptr &&
      baseOwnsGeneratedConstructor(declaration))
    reuseJournal->baseSyntheticSyntax = true;
}

bool SemanticModel::freezeInstanceReuseJournal() {
  if (!reuseJournal || base == nullptr)
    return false;
  const bool synthetic = reuseJournal->baseSyntheticSyntax;
  std::vector<LambdaId> lambdaIds(reuseJournal->lambdas.begin(),
                                  reuseJournal->lambdas.end());
  const std::vector<const VariableDecl *> anchors(
      reuseJournal->sharedAnchors.begin(), reuseJournal->sharedAnchors.end());
  reuseJournal.reset();
  if (synthetic)
    return false;
  frozenBaseAnchors.clear();
  for (const VariableDecl *field : anchors) {
    const auto *anchor = base->findImplicitFieldInitializer(*field);
    if (anchor == nullptr)
      return false;
    // Holding the anchor keeps its address from denoting anything else.
    frozenBaseAnchors.emplace_back(field, *anchor);
  }
  std::sort(lambdaIds.begin(), lambdaIds.end());
  frozenBaseLambdas.clear();
  for (const LambdaId id : lambdaIds) {
    const LambdaInfo *lambda = base->findLambda(id);
    if (lambda == nullptr || lambda->declaration == nullptr)
      return false;
    frozenBaseLambdas.emplace_back(id, lambda->declaration);
    for (const LambdaCaptureInfo &capture : lambda->captures) {
      (void)semanticDatabase.noteBaseSymbol(capture.sourceSymbol);
      (void)semanticDatabase.noteBaseSymbol(capture.bindingSymbol);
    }
  }
  return semanticDatabase.freezeBaseSymbolReads();
}

bool SemanticModel::instanceReuseJournalMatches(
    const SemanticModel &candidate) const {
  for (const auto &[id, declaration] : frozenBaseLambdas) {
    const LambdaInfo *lambda = candidate.findLambda(id);
    if (lambda == nullptr || lambda->declaration != declaration)
      return false;
  }
  for (const auto &[field, anchor] : frozenBaseAnchors) {
    const auto *current = candidate.findImplicitFieldInitializer(*field);
    if (current == nullptr || current->get() != anchor.get())
      return false;
  }
  return semanticDatabase.baseSymbolReadsMatch(candidate.semanticDatabase);
}

void SemanticModel::rebaseReusedInstance(const SemanticModel &baseModel) {
  rebase(&baseModel);
  reuseJournal.reset();
  semanticDatabase.baseSymbolReads.reset();
  callJournal = nullptr;
  functionDemandSink = nullptr;
  executionProfile_ = baseModel.executionProfile();
  placeSnapshot_ = baseModel.placeSnapshot();
  programKind_ = baseModel.programKind();
  embeddingPlan_ = baseModel.embeddingPlan();
}

std::size_t SemanticModel::estimatedInstanceBytes() const {
  std::size_t occurrences = 0;
  for (const auto &[_, unit] : semanticDatabase.occurrencesByUnit)
    occurrences += unit.size();
  std::size_t borrowedDependencies = 0;
  for (const auto &loan : retainedLoans) {
    borrowedDependencies += loan.conditions.size();
    if (loan.referenceSelection)
      borrowedDependencies += loan.referenceSelection->fields.size() +
                              loan.referenceSelection->conditions.size();
    for (const auto &dependency : loan.additionalDependencies)
      borrowedDependencies += dependency.conditions.size();
  }
  for (const auto &[_, complete] : expressionBorrowDependencies) {
    if (!complete)
      continue;
    borrowedDependencies += complete->dependencies.size();
    for (const auto &dependency : complete->dependencies)
      borrowedDependencies += dependency.conditions.size();
    for (const auto &alternative : complete->possibleAlternatives)
      borrowedDependencies += alternative.conditions.size();
  }
  for (const auto &[_, captures] : captureBorrowDependencies) {
    borrowedDependencies += captures.size();
    for (const auto &dependency : captures)
      borrowedDependencies += dependency.conditions.size();
  }
  // Records hold nested types and vectors whose allocator footprint is not
  // exposed; charge a deliberately generous per-record estimate so the LRU
  // bound is conservative rather than byte-exact.
  return sizeof(SemanticModel) + expressions.size() * 256 +
         (calls.size() + constructions.size() + contextualConstructions.size() +
          operators.size() + projectionOperators.size() +
          contextualConversions.size()) *
             768 +
         (variableBindings.size() + parameterBindings.size()) * 256 +
         (places.size() + ownershipEvents.size() + constants.size()) * 160 +
         (functions.size() + lambdas.size() + classLifecycles.size()) * 1024 +
         (statementFullExpressions.size() + constructorFullExpressions.size() +
          fullExpressionOrder.size()) *
             96 +
         (retainedLoans.size() + borrowedDependencies) * 256 +
         (expressionBorrowDependencies.size() +
          captureBorrowDependencies.size()) *
             96 +
         semanticDatabase.symbolRecords.size() * 384 + occurrences * 256 +
         (resolvedSymbols.size() + definedFailures.size() +
          placeSelections.size() + implicitReturnMoves.size()) *
             96 +
         (frozenBaseLambdas.size() +
          semanticDatabase.frozenBaseSymbols.size()) *
             64;
}

// Copies a base function record into the delta so record mutators can
// update it locally. Returns the local entry, or functions.end() when the
// declaration is unknown to both the delta and the base.
[[nodiscard]] std::unordered_map<const FunctionDecl *, FunctionInfo>::iterator
SemanticModel::materializeFunction(const FunctionDecl &declaration) {
  const auto &canonical = canonicalFunctionDeclaration(declaration);
  auto local = functions.find(&canonical);
  if (local != functions.end() || base == nullptr) {
    return local;
  }
  const FunctionInfo *inherited = base->findFunction(canonical);
  if (inherited == nullptr) {
    return functions.end();
  }
  local = functions.insert_or_assign(inherited->declaration, *inherited).first;
  functionsById.insert_or_assign(local->second.id, inherited->declaration);
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
  // A concrete reanalysis can prove that a deferred generic operation has no
  // GTI failure channel. Preserve that explicit empty fact in the overlay;
  // erasing it would expose the base model's deferred propagation again.
  if (operation.empty() &&
      (base == nullptr || base->findDefinedFailure(expression) == nullptr)) {
    definedFailures.erase(&expression);
    return;
  }
  definedFailures.insert_or_assign(&expression, std::move(operation));
}

void SemanticModel::recordCallerLocation(const Expr &expression,
                                         CallerLocationOrigin origin) {
  callerLocations.insert_or_assign(&expression, origin);
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

void SemanticModel::recordImplicitReturnMove(const Expr &expression,
                                             ImplicitReturnMoveInfo info) {
  implicitReturnMoves.insert_or_assign(&expression, std::move(info));
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

const SemanticBorrowDependencySet *
SemanticModel::findBorrowDependencySet(const Expr &expression) const {
  if (const auto found = expressionBorrowDependencies.find(&expression);
      found != expressionBorrowDependencies.end())
    return found->second ? &*found->second : nullptr;
  return base == nullptr ? nullptr : base->findBorrowDependencySet(expression);
}

std::span<const SemanticLoanDependency>
SemanticModel::findBorrowDependencies(const Expr &expression) const {
  const auto *complete = findBorrowDependencySet(expression);
  return complete == nullptr
             ? findCaptureBorrowDependencies(expression)
             : std::span<const SemanticLoanDependency>{complete->dependencies};
}

std::span<const SemanticLoanDependency>
SemanticModel::findCaptureBorrowDependencies(const Expr &expression) const {
  if (const auto found = captureBorrowDependencies.find(&expression);
      found != captureBorrowDependencies.end())
    return found->second;
  return base == nullptr ? std::span<const SemanticLoanDependency>{}
                         : base->findCaptureBorrowDependencies(expression);
}

void SemanticModel::recordCaptureBorrowDependencies(
    const Expr &expression, std::vector<SemanticLoanDependency> dependencies) {
  captureBorrowDependencies.insert_or_assign(&expression,
                                             std::move(dependencies));
}

void SemanticModel::recordBorrowDependencies(
    const Expr &expression, std::vector<SemanticLoanDependency> dependencies) {
  recordBorrowDependencySet(expression,
                            {.dependencies = std::move(dependencies)});
}

void SemanticModel::recordBorrowDependencySet(
    const Expr &expression, SemanticBorrowDependencySet dependencies) {
  expressionBorrowDependencies.insert_or_assign(&expression,
                                                std::move(dependencies));
}

void SemanticModel::clearBorrowDependencySet(const Expr &expression) {
  expressionBorrowDependencies.insert_or_assign(&expression, std::nullopt);
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
                                          std::optional<bool> thenBranch) {
  constexprBranches.insert_or_assign(&statement, thenBranch);
}

void SemanticModel::recordConstexprForExpansion(
    const ConstexprForStmt &statement,
    std::optional<ConstexprForExpansion> expansion) {
  constexprForExpansions.insert_or_assign(&statement, std::move(expansion));
}

void SemanticModel::recordMacroExpansion(const ExpressionStmt &statement,
                                         StmtPtr body,
                                         const FunctionDecl &declaration) {
  macroExpansions.insert_or_assign(&statement, std::move(body));
  macroDeclarations.insert_or_assign(&statement, &declaration);
}

void SemanticModel::recordMacroDefault(const Expr &expression, Token caller) {
  macroDefaults.insert_or_assign(&expression, std::move(caller));
}

void SemanticModel::recordMacroExpansion(const Call &call, StmtPtr body,
                                         const FunctionDecl &declaration) {
  macroValueExpansions.insert_or_assign(&call, std::move(body));
  macroValueDeclarations.insert_or_assign(&call, &declaration);
}

void SemanticModel::recordDeclarationAttributes(
    const Stmt &declaration,
    std::vector<ResolvedDeclarationAttribute> attributes) {
  declarationAttributes.insert_or_assign(&declaration, std::move(attributes));
}

const Expr &
SemanticModel::recordExpressionAlias(const Expr &expression,
                                     std::shared_ptr<const Expr> alias) {
  const Expr &result = *alias;
  expressionAliases.insert_or_assign(&expression, std::move(alias));
  return result;
}

void SemanticModel::recordStaticAssertResult(
    const StaticAssertDecl &declaration, StaticAssertResult result) {
  staticAssertResults.insert_or_assign(&declaration, result);
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

void SemanticModel::removeLoanCarrier(SemanticLoanId id, SymbolId carrier) {
  if (id == 0 || carrier == 0 || id > retainedLoans.size() ||
      retainedLoans[id - 1].id != id) {
    return;
  }
  std::erase(retainedLoans[id - 1].carriers, carrier);
}

void SemanticModel::clearLoans() {
  retainedLoans.clear();
  expressionBorrowDependencies.clear();
  captureBorrowDependencies.clear();
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

void SemanticModel::record(const RangeForStmt &statement,
                           ResolvedRangeIterationInfo info) {
  rangeIterations.insert_or_assign(&statement, std::move(info));
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
  // Definition syntax is an alias, never a second function identity. This
  // also covers instance overlays whose alias map lives in the base model.
  const FunctionDecl *canonical =
      info.declaration != nullptr ? info.declaration : &declaration;
  const auto [found, _] =
      functions.insert_or_assign(canonical, std::move(info));
  functionsById.insert_or_assign(found->second.id, canonical);
}

void SemanticModel::bindFunctionDefinition(const FunctionDecl &declaration,
                                           const FunctionDecl &definition) {
  auto found = functions.find(&declaration);
  assert(found != functions.end());
  found->second.definition = &definition;
  functionDeclarations.emplace(&definition, &declaration);
}

void SemanticModel::record(const Lambda &declaration, LambdaInfo info) {
  const auto [found, _] =
      lambdas.insert_or_assign(&declaration, std::move(info));
  lambdasById.insert_or_assign(found->second.id, &declaration);
}

void SemanticModel::recordLambdaTypeDiscovery(LambdaInfo info) {
  for (auto &capture : info.captures) {
    capture.initializer = nullptr;
    capture.sourceSymbol = 0;
    capture.bindingSymbol = 0;
  }
  lambdaTypeDiscoveries.insert_or_assign(info.id, std::move(info));
}

void SemanticModel::recordClassType(const ClassDecl &declaration,
                                    ClassTypeInfo info) {
  const auto [found, _] =
      classTypes.insert_or_assign(&declaration, std::move(info));
  classTypesById.insert_or_assign(found->second.id, &declaration);
}

void SemanticModel::recordExactClassSpecialization(
    ClassId primary, ClassId specialization,
    std::vector<SemanticType> typeArguments,
    std::vector<CompileTimeValue> valueArguments) {
  exactClassSpecializations.push_back(
      {.primary = primary,
       .specialization = specialization,
       .typeArguments = std::move(typeArguments),
       .valueArguments = std::move(valueArguments)});
}

void SemanticModel::record(const TypeAliasDecl &declaration,
                           TypeAliasInfo info) {
  typeAliases.insert_or_assign(&declaration, std::move(info));
}

void SemanticModel::recordEnumType(const EnumDecl &declaration,
                                   EnumTypeInfo info) {
  std::erase_if(enumInstanceTypes,
                [&](const auto &row) { return row.first.enumId == info.id; });
  const auto [found, _] =
      enumTypes.insert_or_assign(&declaration, std::move(info));
  enumTypesById.insert_or_assign(found->second.id, &declaration);
}

void SemanticModel::recordEnumInstanceType(SemanticType type,
                                           EnumTypeInfo info) {
  const auto found =
      std::find_if(enumInstanceTypes.begin(), enumInstanceTypes.end(),
                   [&](const auto &row) { return row.first == type; });
  if (found == enumInstanceTypes.end())
    enumInstanceTypes.emplace_back(std::move(type), std::move(info));
  else
    found->second = std::move(info);
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

void SemanticModel::recordConsumedSwitchSubject(const SwitchStmt &statement,
                                                BindingInfo subject) {
  consumedSwitchSubjects.insert_or_assign(&statement, std::move(subject));
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
  if (const FunctionInfo *target = findFunction(info.function)) {
    info.neverReturns = target->neverReturns || info.noNormalBorrowResult;
    info.tracksCaller = target->tracksCaller;
  } else if (intrinsicTerminalFailureOutcome(info.intrinsic)) {
    info.neverReturns = true;
    info.tracksCaller = intrinsicTracksCaller(info.intrinsic);
  }
  suppressedInheritedCalls.erase(&call);
  calls.insert_or_assign(&call, std::move(info));
  if (callJournal != nullptr)
    callJournal->insert(&call);
}

void SemanticModel::reserveSymbolIdentities(SymbolId ceiling) {
  semanticDatabase.reserveIdentities(ceiling);
}

void SemanticModel::recordNativeFunctionConversion(
    const Expr &expression, NativeFunctionConversionInfo info) {
  nativeFunctionConversions.insert_or_assign(&expression, std::move(info));
}

void SemanticModel::recordFunctionItemSelection(
    const Expr &expression, FunctionItemSelectionInfo info) {
  functionItemSelections.insert_or_assign(&expression, std::move(info));
}

void SemanticModel::suppressInheritedCall(const Call &call) {
  calls.erase(&call);
  suppressedInheritedCalls.insert(&call);
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
      requirement.ownedTransport) {
    const bool transportOnly =
        existing->signatures.empty() && existing->forwardings.empty();
    const bool repeatableExactReturn =
        requirement.ownedTransport->kind ==
            CallableOwnedTransportKind::ExactReturn &&
        existing->forwardings.empty() &&
        existing->access == requirement.access &&
        std::none_of(existing->signatures.begin(), existing->signatures.end(),
                     [](const CallableSignatureRequirement &signature) {
                       return signature.capability ==
                              CallableInvocationCapability::Once;
                     });
    if (transportOnly) {
      existing->boundary = CallableBoundary::Owned;
      existing->ownedTransport = std::move(requirement.ownedTransport);
    } else if (repeatableExactReturn &&
               existing->boundary == CallableBoundary::Confined) {
      existing->ownedTransport = std::move(requirement.ownedTransport);
    }
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
  suppressedInheritedOperators.erase(&expression);
  operators.insert_or_assign(&expression, std::move(info));
}

void SemanticModel::suppressInheritedOperator(const Expr &expression) {
  operators.erase(&expression);
  suppressedInheritedOperators.insert(&expression);
}

void SemanticModel::recordProjectionOperator(const Expr &expression,
                                             ResolvedOperatorInfo info) {
  projectionOperators.insert_or_assign(&expression, std::move(info));
}

void SemanticModel::recordContextualConversion(const Expr &expression,
                                               ResolvedOperatorInfo info) {
  contextualConversions.insert_or_assign(&expression, std::move(info));
}

void SemanticModel::forgetContextualConversion(const Expr &expression) {
  contextualConversions.erase(&expression);
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
  noteConstruction(info.declaration);
  constructions.insert_or_assign(&expression, std::move(info));
}

void SemanticModel::recordArrayElementConstruction(
    const ArrayInitializer &initializer, ResolvedConstructionInfo info) {
  noteConstruction(info.declaration);
  arrayElementConstructions.insert_or_assign(&initializer, std::move(info));
}

void SemanticModel::recordContextualConstruction(const Expr &initializer,
                                                 ResolvedConstructionInfo info,
                                                 Token site) {
  noteConstruction(info.declaration);
  contextualConstructions.insert_or_assign(&initializer, std::move(info));
  contextualConstructionSites.insert_or_assign(&initializer, std::move(site));
}

void SemanticModel::forgetContextualConstruction(const Expr &initializer) {
  contextualConstructions.erase(&initializer);
  contextualConstructionSites.erase(&initializer);
}

void SemanticModel::recordEmptyReturnConstruction(const ReturnStmt &statement,
                                                  ResolvedConstructionInfo info,
                                                  bool unsafeConstruction) {
  noteConstruction(info.declaration);
  emptyReturnConstructions.insert_or_assign(
      &statement, EmptyReturnConstruction{std::move(info), unsafeConstruction});
  SemanticFullExpression expression{.order = fullExpressionOrder.size() + 1,
                                    .statement = &statement};
  auto &expressions = statementFullExpressions[&statement];
  if (expressions.empty()) {
    expressions.push_back(expression);
    fullExpressionOrder.push_back(std::move(expression));
  }
}

std::optional<SemanticEmptyReturnConstructionProof>
SemanticModel::emptyReturnConstructionProof(const ReturnStmt &statement) const {
  const auto found = emptyReturnConstructions.find(&statement);
  if (found == emptyReturnConstructions.end())
    return base == nullptr ? std::nullopt
                           : base->emptyReturnConstructionProof(statement);
  return SemanticEmptyReturnConstructionProof(
      statement, found->second.construction, analysisSeal().programSnapshot,
      found->second.unsafeConstruction);
}

void SemanticModel::record(const ConstructorInitializer &initializer,
                           ResolvedConstructorInitializerInfo info) {
  noteConstruction(info.declaration);
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

void SemanticModel::setSymbolDocumentation(SymbolId symbol,
                                           std::string documentationMarkdown) {
  semanticDatabase.setSymbolDocumentation(symbol,
                                          std::move(documentationMarkdown));
}

[[nodiscard]] SymbolId SemanticModel::symbolForDeclaration(
    SourceUnitId sourceUnit, const SourceSpan &span,
    std::string_view generatedName, std::uint32_t expansion) const {
  return semanticDatabase.symbolForDeclaration(sourceUnit, span, generatedName,
                                               expansion);
}

void SemanticModel::adoptExpansionArena(
    std::vector<StmtPtr> statements,
    std::vector<std::shared_ptr<const Expr>> expressions) {
  expansionArena.insert(expansionArena.end(),
                        std::make_move_iterator(statements.begin()),
                        std::make_move_iterator(statements.end()));
  expressionArena.insert(expressionArena.end(),
                         std::make_move_iterator(expressions.begin()),
                         std::make_move_iterator(expressions.end()));
}

void SemanticModel::recordOccurrence(SemanticOccurrence occurrence) {
  noteConstruction(occurrence.constructor);
  if (occurrence.selectedConstruction)
    noteConstruction(occurrence.selectedConstruction->declaration);
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

void SemanticModel::finalizeOccurrences(
    const std::unordered_set<SourceUnitId> &units) {
  semanticDatabase.finalize(units);
}

std::unordered_map<SourceUnitId, std::size_t>
SemanticModel::occurrenceCounts() const {
  std::unordered_map<SourceUnitId, std::size_t> counts;
  for (const auto &[unit, occurrences] : semanticDatabase.occurrencesByUnit)
    counts.emplace(unit, occurrences.size());
  return counts;
}

std::vector<CallableParameterContract>
FunctionInfo::instantiatedCallableParameters(
    const std::vector<SemanticType> &concreteParameters) const {
  std::vector<SemanticType> flattened;
  for (const SemanticType &type : concreteParameters)
    SemanticType::appendExpandedArgument(flattened, type);
  std::vector<CallableParameterContract> result;
  for (const CallableParameterContract &contract : callableParameters) {
    const bool finalPack = parameterPack && !parameterTypes.empty() &&
                           contract.parameterIndex + 1 == parameterTypes.size();
    const std::size_t end =
        finalPack ? flattened.size() : contract.parameterIndex + 1;
    for (std::size_t index = contract.parameterIndex;
         index < end && index < flattened.size(); ++index) {
      CallableParameterContract concrete = contract;
      concrete.parameterIndex = index;
      concrete.callableType = flattened[index];
      if (concrete.ownedTransport &&
          concrete.ownedTransport->kind == CallableOwnedTransportKind::Value)
        concrete.ownedTransport->destinationType = flattened[index];
      result.push_back(std::move(concrete));
    }
  }
  return result;
}

void SemanticModel::finalizeCallableArguments(ResolvedCallInfo &call) const {
  const FunctionInfo *function = findFunction(call.function);
  if (function == nullptr) {
    return;
  }
  for (const CallableParameterContract &parameter :
       function->instantiatedCallableParameters(call.parameterTypes)) {
    const auto containsLambda = [&](const auto &self,
                                    const SemanticType &type) -> bool {
      return type.kind == SemanticType::Lambda ||
             std::any_of(type.arguments.begin(), type.arguments.end(),
                         [&](const SemanticType &argument) {
                           return self(self, argument);
                         });
    };
    const bool threadTask =
        parameter.ownedTransport && parameter.ownedTransport->kind ==
                                        CallableOwnedTransportKind::ThreadTask;
    if (parameter.boundary == CallableBoundary::Owned && !threadTask &&
        !containsLambda(containsLambda, parameter.callableType)) {
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

void SemanticModel::finalizeCallableArguments(
    const std::unordered_set<const Call *> &batchCalls,
    const std::unordered_set<SourceUnitId> &units) {
  for (const Call *call : batchCalls) {
    if (const auto found = calls.find(call); found != calls.end())
      finalizeCallableArguments(found->second);
  }
  for (const SourceUnitId unit : units) {
    const auto found = semanticDatabase.occurrencesByUnit.find(unit);
    if (found == semanticDatabase.occurrencesByUnit.end())
      continue;
    for (SemanticOccurrence &occurrence : found->second) {
      if (occurrence.selectedCall) {
        finalizeCallableArguments(*occurrence.selectedCall);
      }
    }
  }
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
  case SemanticType::FunctionSignature:
    traits.copyable = false;
    traits.movable = false;
    traits.copyAssignable = false;
    traits.moveAssignable = false;
    traits.transferCapable = false;
    traits.shareCapable = false;
    return traits;
  case SemanticType::FunctionItem:
    // A function item is a zero-state, process-lifetime callable identity.
    // Its backend representation is private; no raw address or borrowed
    // state is exposed through the GTI type.
    traits.relocation = RelocationKind::Bitwise;
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
  case SemanticType::NativeFunction:
  case SemanticType::StringView:
  case SemanticType::CString:
    traits.transferCapable = false;
    traits.shareCapable = false;
    traits.relocation = RelocationKind::Bitwise;
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
      traits.relocation = element.relocation;
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
    traits.relocation = RelocationKind::Structural;
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
    traits.relocation = RelocationKind::Structural;
    return traits;
  case SemanticType::Class:
  case SemanticType::TypeParameter:
    traits.drop = DropKind::Lexical;
    traits.transferCapable = false;
    traits.shareCapable = false;
    return traits;
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
  case SemanticType::NullPtr:
  case SemanticType::Enum:
    traits.relocation = RelocationKind::Bitwise;
    return traits;
  }
  // New or unrecognized kinds have no relocation authority until their
  // ownership and lifetime behavior is explicitly classified above.
  return traits;
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
  case IntrinsicKind::IntegerAddOverflows:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Add,
                                      IntegerArithmeticMode::OverflowPredicate};
  case IntrinsicKind::IntegerSubtractOverflows:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Subtract,
                                      IntegerArithmeticMode::OverflowPredicate};
  case IntrinsicKind::IntegerMultiplyOverflows:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Multiply,
                                      IntegerArithmeticMode::OverflowPredicate};
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
