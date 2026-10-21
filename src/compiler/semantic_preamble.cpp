// Trusted prefix preamble: analysing a program's leading compiler-trusted
// units once and resuming later analyses from a copy of that state. See
// "Trusted Prefix Preamble" in docs/architecture/semantic-analysis.md.
#include "gti/semantic_preamble.h"

#include "semantic_analyzer_impl.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

namespace lang {

// ---- SemanticVisitor entry points -----------------------------------------

std::shared_ptr<const SemanticPreamble>
SemanticVisitor::buildPreamble(Program prefix) {
  impl->buildingPreamble = true;
  (void)impl->check(prefix);
  impl->buildingPreamble = false;
  impl->freezePreamble();
  // The caller's graph, sources and metrics die with this analysis; a
  // resumed analysis supplies its own.
  impl->detachFromCaller();
  std::unique_ptr<Impl> state = std::move(impl);
  impl = state->blankLikeThis();
  return std::make_shared<const SemanticPreamble>(std::move(prefix),
                                                  std::move(state));
}

void SemanticVisitor::adoptPreamble(
    std::shared_ptr<const SemanticPreamble> preamble,
    std::size_t prefixDeclarationCount) {
  impl->adoptedPreamble = std::move(preamble);
  impl->adoptedPrefixDeclarations = prefixDeclarationCount;
}

std::uint64_t semanticPreambleIdentity(const SemanticPreamble &preamble) {
  return preamble.identity();
}

std::uint64_t SemanticPreamble::nextIdentity() {
  static std::atomic<std::uint64_t> counter{0};
  return ++counter;
}

// ---- Impl -------------------------------------------------------------------

auto SemanticVisitor::Impl::freezePreamble() -> void {
  requestSession.preambleForwardings =
      semanticModel.pendingCallableForwardings.size();
  requestSession.preambleCThreadCallUses =
      requestSession.cThreadCallUses.size();
  requestSession.preambleUnsafeBlockCandidates =
      requestSession.unsafeBlockCandidates.size();
  requestSession.preambleMutableLocalCandidates =
      requestSession.mutableLocalCandidates.size();
  requestSession.preambleGenericDefaultConstantUses =
      genericDefaultConstantUses.size();
  requestSession.preambleOccurrenceCounts = semanticModel.occurrenceCounts();
  requestSession.preambleFirstLambda = nextLambdaId;
}

auto SemanticVisitor::Impl::detachFromCaller() -> void {
  sourceGraph = nullptr;
  sources = nullptr;
  workMetrics = nullptr;
}

auto SemanticVisitor::Impl::blankLikeThis() const -> std::unique_ptr<Impl> {
  return std::make_unique<Impl>(target, nullptr, toolingOccurrences, nullptr,
                                nullptr, RawPointerAuditMode::Off,
                                requireFunctionDefinitions, trustedBodies);
}

// Copies the preamble's session state into this analyzer. Everything the
// prefix analysis accumulated is value state in RequestSessionState and the
// members below; the compiler-generated copy carries the former in full, so a
// member added later is copied unless it is deliberately excluded here.
auto SemanticVisitor::Impl::adoptPreambleState() -> void {
  const Impl &source = *adoptedPreamble->state;
  requestSession = source.requestSession;
  // Request-local budgets and query states are never shared with the
  // preamble or with another resumed analysis.
  requestSession.constexprWork.reset();
  requestSession.programConcreteEffects =
      std::make_shared<ProgramConcreteEffectState>(
          *source.requestSession.programConcreteEffects);
  requestSession.rawResultQueries.reset();
  requestSession.openReanalyses.reset();
  requestSession.classAssertionOutcomes.clear();
  requestSession.trackAssertionDiagnostics = false;
  requestSession.trackedAssertionFailed = false;
  requestSession.assertionDiagnostics.clear();
  requestSession.collectingRawResult.reset();
  compilerTypeClasses = source.compilerTypeClasses;
  designatedMoves = source.designatedMoves;
  analyzedDeclarationAttributeOwners =
      source.analyzedDeclarationAttributeOwners;
  initializerEffectOwners = source.initializerEffectOwners;
  nextExpansion = source.nextExpansion;
  expansionArena = source.expansionArena;
  synthesizedExpressions = source.synthesizedExpressions;
  registrationFloor = {.classes = classes.size(),
                       .enums = enums.size(),
                       .typeAliases = typeAliases.size(),
                       .concepts = concepts.size(),
                       .functions = catalog.nextFunctionId};
  // The prefix sealed its catalog; the rest of the program registers into it.
  catalog.sealed = false;
  catalog.sealedShape.clear();
  // HIR asks for a primary body it does not find analysed; a held-back body
  // is analysed again only in the sense that reaching it reports it.
  for (const FunctionDecl *declaration : requestSession.preambleBodyOrder)
    semanticModel.forgetAnalyzedFunctionBody(*declaration);
  // The preamble's synthesized syntax is the same object in every analysis
  // resumed from it, so instance analyses that read it stay reusable while
  // the instance cache generation names this preamble.
  semanticModel.markSyntheticSyntaxStable();
}

auto SemanticVisitor::Impl::checkResumed(const Program &program) -> bool {
  const std::shared_ptr<const SemanticPreamble> preamble = adoptedPreamble;
  const std::size_t prefixCount = adoptedPrefixDeclarations;
  resetRequestState(&program);
  adoptedPreamble = preamble;
  adoptedPrefixDeclarations = prefixCount;
  const auto adoptStart = std::chrono::steady_clock::now();
  adoptPreambleState();
  analyzedProgram = &program;
  semanticModel.setAnalysisSeal(
      makeSemanticAnalysisSeal(program, target, sourceGraph));
  semanticModel.setToolingOccurrencesEnabled(toolingOccurrences);
  const StmtList &declarations = program.declarations();
  resumeDeclarations.assign(declarations.begin() +
                                static_cast<std::ptrdiff_t>(
                                    std::min(prefixCount, declarations.size())),
                            declarations.end());
  if (workMetrics != nullptr) {
    workMetrics->semanticPreambleAdoptMicroseconds += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - adoptStart)
            .count());
    workMetrics->semanticPreambleBodies += requestSession.preambleBodies.size();
  }
  validateModuleExports(resumeDeclarations);

  const auto registrationStart = std::chrono::steady_clock::now();
  registerDeclarations(resumeDeclarations);
  catalog.seal();
  registrationSymbolCeiling = semanticModel.database().symbolIdentityCeiling();
  // The prefix's body phase declared its globals; only add what the rest of
  // the program registered.
  valueFlow.extendBody(catalog);
  if (workMetrics != nullptr) {
    workMetrics->semanticRegistrationMicroseconds += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - registrationStart)
            .count());
  }

  // The body phase adds only this program's own bodies (and the trusted
  // bodies after the prefix); the finalization scope confines every
  // whole-program pass to what it added, exactly as late demand does.
  const auto bodyStart = std::chrono::steady_clock::now();
  std::unordered_set<const Call *> calls;
  std::vector<const FunctionDecl *> analyzed;
  LateFinalizationScope scope{
      .calls = &calls,
      .forwardings = requestSession.preambleForwardings,
      .cThreadCallUses = requestSession.preambleCThreadCallUses,
      .unsafeBlockCandidates = requestSession.preambleUnsafeBlockCandidates,
      .mutableLocalCandidates = requestSession.preambleMutableLocalCandidates,
      .genericDefaultConstantUses =
          requestSession.preambleGenericDefaultConstantUses,
      .occurrenceCounts = requestSession.preambleOccurrenceCounts,
      .firstLambda = requestSession.preambleFirstLambda};
  auto *const enclosingJournal = semanticModel.setCallJournal(&calls);
  lateAnalyzedBodies = &analyzed;
  analyzeBodies(resumeDeclarations, &scope);
  lateAnalyzedBodies = nullptr;
  semanticModel.setCallJournal(enclosingJournal);
  // Required bodies are reached outright; complete analysis reaches all.
  std::vector<const FunctionDecl *> required(requiredFunctionBodies.begin(),
                                             requiredFunctionBodies.end());
  (void)reachPreambleBodies(required,
                            trustedBodies == TrustedBodyAnalysis::Complete);
  if (workMetrics != nullptr) {
    workMetrics->semanticBodyMicroseconds += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - bodyStart)
            .count());
  }

  const auto finalizationStart = std::chrono::steady_clock::now();
  const std::unordered_set<const FunctionDecl *> functions(analyzed.begin(),
                                                           analyzed.end());
  scope.functions = &functions;
  for (const auto &[unit, count] : semanticModel.occurrenceCounts()) {
    const auto before = scope.occurrenceCounts.find(unit);
    if (before == scope.occurrenceCounts.end() || before->second != count)
      scope.units.insert(unit);
  }
  for (const FunctionDecl *declaration : analyzed)
    if (const FunctionInfo *function = semanticModel.findFunction(*declaration))
      scope.functionIds.insert(function->id);
  lateFinalization = &scope;
  discoverOwnedCallableContracts();
  semanticModel.finalizeCallableForwardings();
  finalizeOwnerlessTextContracts();
  discoverOwnedCallableValueParameters();
  validateOnceCallableForwardings();
  validateOwnedCallableArguments();
  validateProvisionalCallableArguments();
  validateConfinedCallableContracts();
  validateConfinedCallableUses();
  semanticModel.finalizeCallableArguments(calls, scope.units);
  finalizeCThreadRequirements();
  finalizeRawBorrowContracts();
  lateFinalization = nullptr;
  // The prefix planned no initialization: the whole program is planned here,
  // substituting program constants in every body.
  finalizeProgramInitialization(program);
  semanticModel.finalizeOccurrences(scope.units);
  lateFinalization = &scope;
  finalizeAdvisoryDiagnostics();
  lateFinalization = nullptr;
  catalog.assertUnchangedSinceSeal();
  if (workMetrics != nullptr) {
    workMetrics->semanticFinalizationMicroseconds += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - finalizationStart)
            .count());
  }
  // As in check(): no worker copied mid-analysis serves concrete requests.
  stagedWorker = {};
  lateDemandReady = trustedBodies == TrustedBodyAnalysis::Reachable;
  return !hadError();
}

auto SemanticVisitor::Impl::reachPreambleBodies(
    std::span<const FunctionDecl *const> roots, bool all) -> bool {
  const auto &bodies = requestSession.preambleBodies;
  if (bodies.empty())
    return false;
  std::vector<const FunctionDecl *> reached;
  const auto consider = [&](const FunctionDecl *declaration) {
    if (declaration != nullptr && bodies.contains(declaration) &&
        reachedPreambleBodies.insert(declaration).second)
      reached.push_back(declaration);
  };
  const auto considerFunction = [&](FunctionId id) {
    if (const FunctionInfo *function = semanticModel.findFunction(id))
      consider(function->bodyDeclaration());
  };
  if (all)
    for (const FunctionDecl *declaration : requestSession.preambleBodyOrder)
      consider(declaration);
  for (const FunctionDecl *root : roots)
    consider(root);
  // Snapshot: a function lookup with a demand sink active would grow the set.
  const std::vector<FunctionId> demanded(demandedFunctions.begin(),
                                         demandedFunctions.end());
  for (const FunctionId id : demanded)
    considerFunction(id);
  for (std::size_t index = 0; index < reached.size(); ++index)
    for (const FunctionId id : bodies.at(reached[index]).demands)
      considerFunction(id);
  if (reached.empty())
    return false;
  const std::unordered_set<const FunctionDecl *> newly(reached.begin(),
                                                       reached.end());
  bool errors = false;
  for (const FunctionDecl *declaration : requestSession.preambleBodyOrder) {
    if (!newly.contains(declaration))
      continue;
    semanticModel.recordAnalyzedFunctionBody(*declaration);
    for (const Diagnostic &diagnostic : bodies.at(declaration).diagnostics) {
      errors = errors || diagnostic.severity == DiagnosticSeverity::Error;
      diagnostics.push_back(diagnostic);
    }
  }
  if (workMetrics != nullptr)
    workMetrics->semanticPreambleReachedBodies += reached.size();
  return errors;
}

auto SemanticVisitor::Impl::reachConsultedPreambleBodies(
    const std::unordered_set<FunctionId> &consulted) -> bool {
  std::vector<const FunctionDecl *> roots;
  for (const FunctionId id : consulted) {
    const FunctionInfo *function = semanticModel.findFunction(id);
    const FunctionDecl *declaration =
        function == nullptr ? nullptr : function->bodyDeclaration();
    if (declaration != nullptr &&
        requestSession.preambleBodies.contains(declaration) &&
        !reachedPreambleBodies.contains(declaration))
      roots.push_back(declaration);
  }
  if (roots.empty() || !reachPreambleBodies(roots, false))
    return false;
  lateDemandFailed = true;
  lateDemandedBodies.insert(lateDemandedBodies.end(), roots.begin(),
                            roots.end());
  return true;
}

// ---- SemanticPreambleCache ------------------------------------------------

class SemanticPreambleCache::Impl {
public:
  explicit Impl(SemanticPreambleCacheLimits limits) : limits(limits) {}

  struct Entry {
    SemanticPreambleKey key;
    std::shared_ptr<const SemanticPreamble> preamble;
  };

  SemanticPreambleCacheLimits limits;
  mutable std::mutex mutex;
  // Least recently used first.
  std::vector<Entry> entries;
  std::size_t hits = 0;
  std::size_t misses = 0;
  std::size_t stores = 0;
};

SemanticPreambleCache::SemanticPreambleCache(SemanticPreambleCacheLimits limits)
    : impl(std::make_unique<Impl>(limits)) {}

SemanticPreambleCache::~SemanticPreambleCache() = default;

std::shared_ptr<const SemanticPreamble>
SemanticPreambleCache::lookup(const SemanticPreambleKey &key) {
  const std::lock_guard<std::mutex> lock(impl->mutex);
  for (std::size_t index = 0; index < impl->entries.size(); ++index) {
    if (impl->entries[index].key != key)
      continue;
    Impl::Entry entry = std::move(impl->entries[index]);
    impl->entries.erase(impl->entries.begin() +
                        static_cast<std::ptrdiff_t>(index));
    impl->entries.push_back(std::move(entry));
    ++impl->hits;
    return impl->entries.back().preamble;
  }
  ++impl->misses;
  return nullptr;
}

void SemanticPreambleCache::store(
    SemanticPreambleKey key, std::shared_ptr<const SemanticPreamble> preamble) {
  if (preamble == nullptr)
    return;
  const std::lock_guard<std::mutex> lock(impl->mutex);
  // A newer preamble for the same units (other syntax) replaces the older.
  std::erase_if(impl->entries, [&](const Impl::Entry &entry) {
    if (entry.key == key)
      return true;
    if (entry.key.context != key.context ||
        entry.key.units.size() != key.units.size() ||
        entry.key.graphUnits != key.graphUnits)
      return false;
    for (std::size_t index = 0; index < key.units.size(); ++index)
      if (entry.key.units[index].path != key.units[index].path)
        return false;
    return true;
  });
  impl->entries.push_back({std::move(key), std::move(preamble)});
  ++impl->stores;
  while (impl->entries.size() >
         std::max<std::size_t>(1, impl->limits.maximumEntries))
    impl->entries.erase(impl->entries.begin());
}

SemanticPreambleCacheStats SemanticPreambleCache::stats() const {
  const std::lock_guard<std::mutex> lock(impl->mutex);
  return {.entries = impl->entries.size(),
          .hits = impl->hits,
          .misses = impl->misses,
          .stores = impl->stores,
          .limits = impl->limits};
}

void SemanticPreambleCache::clear() {
  const std::lock_guard<std::mutex> lock(impl->mutex);
  impl->entries.clear();
}

} // namespace lang
