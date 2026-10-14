// Reuse of concrete instance analyses across editor snapshots. See
// SemanticVisitor::Impl::analyzeReusableInstance and "Concrete Generic
// Reanalysis" in docs/architecture/semantic-analysis.md.
#include "semantic_analyzer_impl.h"

#include <algorithm>

namespace lang {
namespace {

// Order-sensitive 64-bit mixing for the registration fingerprint.
struct Fingerprint {
  std::uint64_t value = 0xcbf29ce484222325ULL;

  void add(std::uint64_t item) {
    value ^= item + 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
    value *= 0x100000001b3ULL;
  }
  void add(const void *pointer) {
    add(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)));
  }
  void add(const std::string &text) { add(std::hash<std::string>{}(text)); }
  void add(const std::vector<GenericParameterInfo> &parameters) {
    add(parameters.size());
    for (const GenericParameterInfo &parameter : parameters)
      add(parameter.id);
  }
  void add(const std::vector<std::size_t> &values) {
    add(values.size());
    for (const std::size_t item : values)
      add(item);
  }
  void add(const SemanticType &type) {
    add(static_cast<std::uint64_t>(type.kind));
    add(type.classId);
    add(type.enumId);
    add(type.genericParameterId);
    add(type.functionId);
    add(type.lambdaId);
    add(type.arrayLength);
    add(type.arguments.size());
    for (const SemanticType &argument : type.arguments)
      add(argument);
    add(type.valueArguments.size());
    for (const CompileTimeValue &value : type.valueArguments) {
      add(static_cast<std::uint64_t>(value.kind));
      add(value.value);
      add(value.parameterId);
    }
  }
  void add(const ConstructorInfo &constructor) {
    add(constructor.id);
    add(constructor.declaration);
    add(constructor.genericParameters);
    add(constructor.defaultArgumentEffectOwners);
  }
};

} // namespace

void SemanticVisitor::Impl::resetInstanceReuse() {
  instanceReuseState = InstanceReuseState::Unprepared;
  instanceGeneration.reset();
  analyzedProgram = nullptr;
  registrationSymbolCeiling = 0;
  journalNextInstance = false;
  lastInstanceAllocatedIdentities = false;
  instanceSymbolRanges.clear();
  pendingInstanceEntries.clear();
}

bool SemanticVisitor::Impl::trustedUnit(SourceUnitId unit) const {
  return sourceGraph != nullptr && unit != 0 &&
         sourceGraph->isCompilerTrusted(unit);
}

// Registration assigns every declaration identity in program order, so a
// change in an earlier application unit can renumber trusted declarations
// even when their own text is unchanged. The fingerprint covers each trusted
// identity space an instance delta can reference (symbols, functions,
// classes and their constructors, enums, concepts, aliases, generic
// parameters and default-argument effect owners), plus every exact class
// specialization, wherever it is declared, because specialization selection
// is program-wide.
std::uint64_t SemanticVisitor::Impl::trustedRegistrationFingerprint() const {
  Fingerprint fingerprint;
  const SemanticDatabase &database = semanticModel.database();
  for (SymbolId id = 1; id <= registrationSymbolCeiling; ++id) {
    const SymbolRecord *symbol = database.findSymbol(id);
    if (symbol == nullptr || !trustedUnit(symbol->sourceUnit))
      continue;
    fingerprint.add(id);
    fingerprint.add(symbol->sourceUnit);
    fingerprint.add(symbol->nameSpan.start);
    fingerprint.add(symbol->nameSpan.end);
    fingerprint.add(symbol->expansion);
    fingerprint.add(static_cast<std::uint64_t>(symbol->kind));
    fingerprint.add(symbol->name);
  }
  fingerprint.add(0xca11ab1eULL);
  for (const auto &callable : catalog.callableDeclarations) {
    if (callable == nullptr || !trustedUnit(callable->sourceUnit))
      continue;
    fingerprint.add(callable->id);
    fingerprint.add(callable->syntax);
    fingerprint.add(callable->definition);
    fingerprint.add(callable->genericParameters);
    fingerprint.add(callable->defaultArgumentEffectOwners);
  }
  fingerprint.add(0xc1a55ULL);
  for (const ClassInfo &owner : catalog.classes) {
    if (!trustedUnit(owner.sourceUnit))
      continue;
    fingerprint.add(owner.id);
    fingerprint.add(owner.declaration);
    fingerprint.add(owner.exactSpecializationPrimary);
    fingerprint.add(owner.genericParameters);
    fingerprint.add(owner.constructors.size());
    for (const ConstructorInfo &constructor : owner.constructors)
      fingerprint.add(constructor);
    for (const auto *special :
         {&owner.designatedConstructor, &owner.reversedDesignatedConstructor,
          &owner.copyConstructor, &owner.moveConstructor}) {
      fingerprint.add(special->has_value() ? 1U : 0U);
      if (*special)
        fingerprint.add(**special);
    }
    fingerprint.add(owner.destructor ? owner.destructor->declaration : nullptr);
  }
  fingerprint.add(0xe0e0ULL);
  for (const EnumInfo &enumeration : catalog.enums) {
    if (!trustedUnit(enumeration.sourceUnit))
      continue;
    fingerprint.add(enumeration.id);
    fingerprint.add(enumeration.declaration);
    fingerprint.add(enumeration.genericParameters);
  }
  fingerprint.add(0xc0cebULL);
  for (const RegisteredConcept &registered : catalog.concepts) {
    if (!trustedUnit(registered.sourceUnit))
      continue;
    fingerprint.add(registered.id);
    fingerprint.add(registered.declaration);
  }
  fingerprint.add(0xa11a5ULL);
  for (std::size_t index = 0; index < catalog.typeAliases.size(); ++index) {
    const RegisteredTypeAlias &alias = catalog.typeAliases[index];
    if (!trustedUnit(alias.sourceUnit))
      continue;
    fingerprint.add(index);
    fingerprint.add(alias.declaration);
  }
  fingerprint.add(0x5bec1aULL);
  for (const ExactClassSpecialization &specialization :
       catalog.exactClassSpecializations) {
    fingerprint.add(specialization.primary);
    fingerprint.add(specialization.specialization);
    fingerprint.add(specialization.typeArguments.size());
    for (const SemanticType &argument : specialization.typeArguments)
      fingerprint.add(argument);
    fingerprint.add(specialization.valueArguments.size());
    for (const CompileTimeValue &value : specialization.valueArguments)
      fingerprint.add(value.value);
  }
  fingerprint.add(0xc0f0aULL);
  for (const RegisteredConformance &conformance : catalog.conformances) {
    fingerprint.add(conformance.conceptId);
    fingerprint.add(conformance.patternTemplate);
    fingerprint.add(conformance.pattern);
    fingerprint.add(conformance.associatedTypes.size());
    for (const SemanticType &type : conformance.associatedTypes)
      fingerprint.add(type);
  }
  return fingerprint.value;
}

bool SemanticVisitor::Impl::prepareInstanceReuse() {
  if (instanceReuseState != InstanceReuseState::Unprepared)
    return instanceReuseState == InstanceReuseState::Active;
  instanceReuseState = InstanceReuseState::Unavailable;
  if (instanceCache == nullptr || sourceGraph == nullptr ||
      analyzedProgram == nullptr ||
      trustedBodies != TrustedBodyAnalysis::Reachable || rawAuditEnabled())
    return false;
  InstanceAnalysisGenerationKey key;
  key.context = instanceCacheContext;
  key.context += requireFunctionDefinitions ? "\ndefinitions:required"
                                            : "\ndefinitions:optional";
  key.context += toolingOccurrences ? "\noccurrences:1" : "\noccurrences:0";
  std::vector<StmtPtr> syntax;
  const StmtList &declarations = analyzedProgram->declarations();
  for (const SourceUnit &unit : sourceGraph->sourceUnits()) {
    if (!sourceGraph->isCompilerTrusted(unit.id))
      continue;
    if (unit.declarationStart + unit.declarationCount > declarations.size())
      return false;
    InstanceAnalysisGenerationKey::TrustedUnit identity{
        .id = unit.id,
        .path = unit.path.string(),
        .sourceDigest = unit.sourceDigest,
        .syntaxDigest = unit.syntaxDigest,
        .declarations = {}};
    for (std::size_t index = 0; index < unit.declarationCount; ++index) {
      const StmtPtr &declaration = declarations[unit.declarationStart + index];
      identity.declarations.push_back(declaration.get());
      syntax.push_back(declaration);
    }
    key.units.push_back(std::move(identity));
  }
  key.registrationFingerprint = trustedRegistrationFingerprint();
  instanceGeneration =
      instanceCache->generation(std::move(key), std::move(syntax));
  if (instanceGeneration == nullptr)
    return false;
  instanceReuseState = InstanceReuseState::Active;
  return true;
}

bool SemanticVisitor::Impl::reusableInstanceType(
    const SemanticType &type) const {
  if (type.genericParameterId != 0 || type.functionId != 0 ||
      type.lambdaId != 0 || type.arrayLengthParameterId != 0)
    return false;
  switch (type.kind) {
  case SemanticType::Unknown:
  case SemanticType::TypeParameter:
  case SemanticType::TypeName:
  case SemanticType::FunctionItem:
  case SemanticType::Lambda:
    return false;
  case SemanticType::TypePack:
    if (!type.concretePack)
      return false;
    break;
  case SemanticType::Class:
    if (type.classId == 0 || type.classId > classes.size() ||
        !trustedUnit(classInfo(type.classId).sourceUnit))
      return false;
    break;
  case SemanticType::Enum:
    if (type.enumId == 0 || type.enumId > enums.size() ||
        !trustedUnit(enums[type.enumId - 1].sourceUnit))
      return false;
    break;
  default:
    break;
  }
  for (const CompileTimeValue &value : type.valueArguments)
    if (value.kind != CompileTimeValue::UInt64 || value.expression)
      return false;
  return std::all_of(type.arguments.begin(), type.arguments.end(),
                     [&](const SemanticType &argument) {
                       return reusableInstanceType(argument);
                     });
}

const ConstructorInfo *
SemanticVisitor::Impl::constructorInfoById(ConstructorId id,
                                           bool *generated) const {
  for (const ClassInfo &owner : classes) {
    for (const auto *special :
         {&owner.designatedConstructor, &owner.reversedDesignatedConstructor}) {
      if (*special && (*special)->id == id) {
        if (generated != nullptr)
          *generated = true;
        return &**special;
      }
    }
    for (const ConstructorInfo &constructor : owner.constructors) {
      if (constructor.id == id) {
        if (generated != nullptr)
          *generated = false;
        return &constructor;
      }
    }
  }
  return nullptr;
}

bool SemanticVisitor::Impl::reusableInstanceKey(
    const InstanceAnalysisKey &key) {
  const auto values = [](const std::vector<CompileTimeValue> &list) {
    return std::all_of(list.begin(), list.end(), [](const auto &value) {
      return value.kind == CompileTimeValue::UInt64 && !value.expression;
    });
  };
  if (!values(key.classValues) || !values(key.values))
    return false;
  for (const auto *types : {&key.classTypes, &key.types})
    for (const SemanticType &type : *types)
      if (!reusableInstanceType(type))
        return false;
  switch (key.kind) {
  case InstanceAnalysisKind::Function: {
    const FunctionInfo *function = semanticModel.findFunction(key.declaration);
    return function != nullptr && function->declaration != nullptr &&
           trustedUnit(function->sourceUnit);
  }
  case InstanceAnalysisKind::Constructor: {
    bool generated = false;
    const ConstructorInfo *constructor =
        constructorInfoById(key.declaration, &generated);
    // A designated constructor's syntax is generated per analysis, so no
    // other snapshot shares it.
    return constructor != nullptr && !generated &&
           constructor->declaration != nullptr && constructor->owner != 0 &&
           constructor->owner <= classes.size() &&
           trustedUnit(classInfo(constructor->owner).sourceUnit);
  }
  case InstanceAnalysisKind::Destructor:
  case InstanceAnalysisKind::ClassFieldInitializers:
    return key.declaration != 0 && key.declaration <= classes.size() &&
           trustedUnit(classInfo(key.declaration).sourceUnit);
  }
  return false;
}

// The syntax an instance key denotes in this snapshot. Equal registration
// fingerprints already imply it; reuse still checks it directly.
const Stmt *SemanticVisitor::Impl::instanceDeclarationSyntax(
    const InstanceAnalysisKey &key) const {
  switch (key.kind) {
  case InstanceAnalysisKind::Function: {
    const FunctionInfo *function = semanticModel.findFunction(key.declaration);
    return function == nullptr ? nullptr : function->declaration;
  }
  case InstanceAnalysisKind::Constructor: {
    const ConstructorInfo *constructor = constructorInfoById(key.declaration);
    return constructor == nullptr ? nullptr : constructor->declaration;
  }
  case InstanceAnalysisKind::Destructor:
  case InstanceAnalysisKind::ClassFieldInitializers:
    return key.declaration == 0 || key.declaration > classes.size()
               ? nullptr
               : classInfo(key.declaration).declaration;
  }
  return nullptr;
}

bool SemanticVisitor::Impl::instanceRangeFree(
    const SemanticModel &delta) const {
  const SymbolId floor = delta.database().instanceIdentityFloor();
  const SymbolId ceiling = delta.database().symbolIdentityCeiling();
  if (ceiling <= floor)
    return true;
  if (floor < semanticModel.database().symbolIdentityCeiling())
    return false;
  return std::none_of(instanceSymbolRanges.begin(), instanceSymbolRanges.end(),
                      [&](const auto &range) {
                        return floor + 1 <= range.second &&
                               range.first <= ceiling;
                      });
}

void SemanticVisitor::Impl::reserveInstanceRange(const SemanticModel &delta) {
  const SymbolId floor = delta.database().instanceIdentityFloor();
  const SymbolId ceiling = delta.database().symbolIdentityCeiling();
  if (ceiling <= floor)
    return;
  instanceSymbolRanges.emplace_back(floor + 1, ceiling);
  instanceSymbolCeiling = std::max(instanceSymbolCeiling, ceiling);
}

std::optional<SemanticInstanceAnalysis>
SemanticVisitor::Impl::reuseInstance(const InstanceAnalysisKey &key) {
  const auto start = std::chrono::steady_clock::now();
  const std::size_t lateDemandBefore =
      workMetrics == nullptr ? 0 : workMetrics->semanticLateDemandMicroseconds;
  // Time spent looking up, validating and copying entries; late demand is
  // semantic work reported on its own.
  struct Timer {
    FrontendWorkMetrics *metrics;
    std::chrono::steady_clock::time_point start;
    std::size_t lateDemandBefore;
    ~Timer() {
      if (metrics == nullptr)
        return;
      const auto elapsed = static_cast<std::size_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - start)
              .count());
      const std::size_t lateDemand =
          metrics->semanticLateDemandMicroseconds - lateDemandBefore;
      metrics->semanticInstanceReuseMicroseconds +=
          elapsed > lateDemand ? elapsed - lateDemand : 0;
    }
  } timer{workMetrics, start, lateDemandBefore};
  const std::shared_ptr<const InstanceAnalysisCacheEntry> entry =
      instanceCache->lookup(instanceGeneration, key);
  if (entry == nullptr)
    return std::nullopt;
  const auto reject = [&] {
    if (workMetrics != nullptr)
      ++workMetrics->semanticInstanceReuseRejected;
    return std::nullopt;
  };
  if (entry->declaration == nullptr ||
      entry->declaration != instanceDeclarationSyntax(key))
    return reject();
  std::vector<const FunctionDecl *> pending;
  for (const auto &[id, declaration] : entry->consulted) {
    const FunctionInfo *function = semanticModel.findFunction(id);
    if (function == nullptr || function->declaration != declaration)
      return reject();
    if (pendingDeferredBody(function->declaration))
      pending.push_back(function->declaration);
  }
  if (!entry->model.instanceReuseJournalMatches(semanticModel) ||
      !instanceRangeFree(entry->model))
    return reject();
  // The delta's own identities stay reserved even if the late demand below
  // fails, so no later identity can collide with them.
  reserveInstanceRange(entry->model);
  // A fresh analysis would late-demand the pending functions it consulted
  // and analyse again; the stored delta is that second analysis.
  if (!pending.empty() && !analyzeLateDemand(pending))
    return reject();
  // A reused delta consulted the same preamble bodies the fresh analysis did;
  // reaching them reports what they held back, exactly as then.
  if (adoptedPreamble != nullptr) {
    std::unordered_set<FunctionId> consulted;
    for (const auto &[id, declaration] : entry->consulted)
      consulted.insert(id);
    if (reachConsultedPreambleBodies(consulted))
      return reject();
  }
  SemanticInstanceAnalysis result{.model = entry->model, .diagnostics = {}};
  result.model.rebaseReusedInstance(semanticModel);
  return result;
}

void SemanticVisitor::Impl::retainInstance(
    InstanceAnalysisKey key, SemanticInstanceAnalysis &analysis,
    const std::unordered_set<FunctionId> &consulted) {
  // Diagnostics are reported against an instantiation site HIR supplies;
  // only diagnostic-free analyses are retained, so a reuse reports nothing
  // a fresh analysis would have reported.
  const bool reusable = analysis.diagnostics.empty() &&
                        !lastInstanceAllocatedIdentities && !lateDemandFailed;
  InstanceAnalysisCacheEntry entry;
  if (reusable) {
    for (const FunctionId id : consulted) {
      const FunctionInfo *function = semanticModel.findFunction(id);
      if (function == nullptr || function->declaration == nullptr ||
          !trustedUnit(function->sourceUnit) ||
          pendingDeferredBody(function->declaration)) {
        analysis.model.endInstanceReuseJournal();
        return;
      }
      entry.consulted.emplace_back(id, function->declaration);
    }
    std::sort(entry.consulted.begin(), entry.consulted.end());
    entry.declaration = instanceDeclarationSyntax(key);
    entry.model = analysis.model;
  }
  analysis.model.endInstanceReuseJournal();
  if (!reusable || entry.declaration == nullptr ||
      !entry.model.freezeInstanceReuseJournal())
    return;
  entry.estimatedBytes =
      entry.model.estimatedInstanceBytes() + entry.consulted.size() * 16;
  pendingInstanceEntries.emplace_back(std::move(key), std::move(entry));
}

void SemanticVisitor::Impl::publishInstanceAnalyses() {
  if (instanceReuseState == InstanceReuseState::Active && !lateDemandFailed) {
    for (auto &[key, entry] : pendingInstanceEntries) {
      instanceCache->store(instanceGeneration, std::move(key),
                           std::move(entry));
      if (workMetrics != nullptr)
        ++workMetrics->semanticInstanceReuseStores;
    }
  }
  pendingInstanceEntries.clear();
}

} // namespace lang
