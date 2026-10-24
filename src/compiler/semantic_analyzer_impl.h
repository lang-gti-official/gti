#pragma once

#include "gti/semantic_analyzer.h"

#include "diagnostic_registry.h"
#include "gti/ast_clone.h"
#include "gti/frontend_work.h"
#include "gti/generic_instantiation_limits.h"
#include "gti/instance_analysis_cache.h"
#include "gti/standard_library.h"
#include "gti/support.h"
#include "semantic_diagnostics.h"
#include "semantic_reflection_traits.h"
#include "semantic_spelling.h"

#include <cassert>
#include <chrono>
#include <deque>
#include <limits>
#include <set>
#include <span>
#include <tuple>

namespace lang {

// Diagnostic text spelling lives in its own translation unit
// (semantic_spelling.cpp); the analyzer consumes it unqualified.
using semantic_spelling::callableSpelling;
using semantic_spelling::classKindSpelling;
using semantic_spelling::pathSpelling;
using semantic_spelling::qualifiedName;
using semantic_spelling::typeRefSpelling;

class SemanticVisitor::Impl final : public ExprVisitor,
                                    public StmtVisitor,
                                    public SemanticInstanceProvider {
public:
  enum class ThreadTransferBoundary { Owned, Scoped };

  [[nodiscard]] std::optional<ScopedThreadTaskShape>
  scopedThreadTaskShapeFor(const SemanticType &type);

  enum class ProgramEffectOwnerKind {
    None,
    Initializer,
    Function,
    Constructor,
    FunctionDefaultArgument,
    ConstructorDefaultArgument,
    Destructor,
    FieldInitializers,
    Lambda,
    ConcreteInstance,
  };

  struct ProgramEffectOwner {
    ProgramEffectOwnerKind kind = ProgramEffectOwnerKind::None;
    std::size_t id = 0;

    [[nodiscard]] bool valid() const {
      return kind != ProgramEffectOwnerKind::None && id != 0;
    }

    friend bool operator==(const ProgramEffectOwner &,
                           const ProgramEffectOwner &) = default;
  };

  struct ProgramEffectOwnerHash {
    [[nodiscard]] std::size_t
    operator()(const ProgramEffectOwner &owner) const {
      return (owner.id << 3U) ^ static_cast<std::size_t>(owner.kind);
    }
  };

  struct ProgramStorageEffectUse {
    SymbolId symbol = 0;
    SourceSpan span;
    const Expr *expression = nullptr;
    bool deferredConstantCandidate = false;
  };

  struct ProgramEffectEdge {
    ProgramEffectOwner target;
    SourceSpan span;
  };

  struct ProgramUnknownEffect {
    SourceSpan span;
    std::string reason;
  };

  struct ProgramEffectSummary {
    std::vector<ProgramStorageEffectUse> uses;
    std::vector<ProgramEffectEdge> edges;
    std::vector<ProgramUnknownEffect> unknown;
  };

  struct ProgramConcreteEffectQuery {
    ProgramEffectOwner target;
    SemanticType owner = SemanticType::Unknown;
    std::vector<SemanticType> types;
    std::vector<CompileTimeValue> values;
    SourceSpan site;
    bool complete = false;
  };
  struct ProgramConcreteEffectState {
    std::vector<ProgramConcreteEffectQuery> queries;
    // Hash buckets accelerate lookup; the ordered vector alone assigns IDs.
    std::unordered_map<std::size_t, std::vector<std::size_t>> buckets;
    std::size_t analyzed = 0;
  };

  void recordConcreteProgramEffect(ProgramEffectOwner target,
                                   const SemanticType &owner,
                                   const std::vector<SemanticType> &types,
                                   const std::vector<CompileTimeValue> &values,
                                   const SourceSpan &site);
  void closeConcreteProgramEffects(const ProgramInitializationPlan &plan);

  explicit Impl(
      TargetInfo target, const SourceGraph *sourceGraph,
      bool toolingOccurrences, FrontendWorkMetrics *workMetrics,
      const SourceManager *sources,
      RawPointerAuditMode rawPointerAudit = RawPointerAuditMode::Off,
      bool requireFunctionDefinitions = false,
      TrustedBodyAnalysis trustedBodies = TrustedBodyAnalysis::Complete)
      : target(std::move(target)), sourceGraph(sourceGraph), sources(sources),
        toolingOccurrences(toolingOccurrences), workMetrics(workMetrics),
        rawPointerAudit(rawPointerAudit),
        requireFunctionDefinitions(requireFunctionDefinitions),
        trustedBodies(trustedBodies) {
    semanticModel.setToolingOccurrencesEnabled(toolingOccurrences);
  }

  // Every public analysis entry point establishes one complete request state.
  // Constructor configuration remains on Impl; no result, AST pointer, flow
  // fact, diagnostic, or identity counter survives this operation.
  void resetRequestState(const Program *program);

  [[nodiscard]] bool rawAuditEnabled() const {
    return rawPointerAudit != RawPointerAuditMode::Off;
  }

  bool check(const Program &program);

  std::unordered_set<const FunctionDecl *> requiredFunctionBodies;

  bool check(const Expr &expr);

  bool applyProgramRequest(const Program &program,
                           const ProgramRequest &request);

  [[nodiscard]] bool hadError() const;

  [[nodiscard]] const std::vector<SemanticDiagnostic> &errors() const;

  [[nodiscard]] SemanticType expressionType() const;

  [[nodiscard]] const SemanticModel &model() const;

  // Moves the accumulated model out of the visitor. The visitor must not run
  // further analysis (including concrete instance reanalysis) afterwards.
  [[nodiscard]] SemanticModel takeModel();

  [[nodiscard]] SemanticTypeTraits
  traitsFor(const SemanticType &type) const override;

  [[nodiscard]] bool
  requiresActiveCleanupFor(const SemanticType &type) const override;
  [[nodiscard]] bool
  lifecycleBoundBorrowCarrierFor(const SemanticType &type) const override;
  [[nodiscard]] std::optional<EnumTypeInfo>
  enumTypeFor(const SemanticType &type) const override;

  void attachDeclarationDocumentation(SymbolId symbol, const Stmt &declaration);

  void attachTokenDocumentation(SymbolId symbol, const Token &declaration);

  // Concrete instance reanalysis runs on this visitor inside a detach/
  // restore bracket instead of copying the whole visitor per instance. The
  // bracket detaches the accumulated model and diagnostics, analysis writes
  // into an empty delta that reads through to the detached base, and the
  // bracket then restores the base and identity counters. Analyses are
  // strictly sequential: a bracket must close before the next one opens.
  // While it is open, reanalysisBase names the detached base and the
  // instance is on the staging chain's list of open reanalyses.
  struct InstanceAnalysisScope {
    InstanceAnalysisScope(Impl &visitor, InstanceAnalysisKey instance)
        : visitor(visitor), baseModel(std::move(visitor.semanticModel)),
          baseDiagnostics(std::move(visitor.diagnostics)),
          savedGenericParameterId(visitor.nextGenericParameterId),
          savedConstructorId(visitor.nextConstructorId),
          savedFunctionId(visitor.nextFunctionId),
          savedLambdaId(visitor.nextLambdaId),
          savedSemanticLoanId(visitor.nextSemanticLoanId),
          savedPlaceSelectionId(visitor.nextPlaceSelectionId),
          openReanalyses(visitor.openReanalyses()) {
      visitor.catalog.assertUnchangedSinceSeal();
      visitor.semanticModel.beginInstanceDelta(baseModel,
                                               visitor.instanceSymbolCeiling);
      if (visitor.journalNextInstance)
        visitor.semanticModel.beginInstanceReuseJournal();
      visitor.diagnostics.clear();
      visitor.reanalysisBase = &baseModel;
      // A constant that one of the instance's static assertions requires can
      // need the instance itself (reflecting on its own fields, constructing
      // it), so staging may reanalyse it in a nested worker. The enclosing
      // reanalysis owns those assertions and an assertion yields no value,
      // so the nested one supplies the instance's facts without them.
      nested = std::find(openReanalyses->begin(), openReanalyses->end(),
                         instance) != openReanalyses->end();
      if (!nested)
        openReanalyses->push_back(std::move(instance));
      visitor.requestSession.assertionsOwnedUpChain = nested;
    }

    ~InstanceAnalysisScope() { visitor.catalog.assertUnchangedSinceSeal(); }

    // Takes the delta and diagnostics, restores the visitor, and re-points
    // the delta at the restored base model. Call exactly once.
    [[nodiscard]] SemanticInstanceAnalysis finish() {
      visitor.instanceSymbolCeiling =
          visitor.semanticModel.database().symbolIdentityCeiling();
      visitor.instanceLambdaCeiling =
          std::max(visitor.instanceLambdaCeiling, visitor.nextLambdaId);
      // A reusable delta must not carry an identity another snapshot could
      // assign to something else; these counters restart for every instance.
      visitor.lastInstanceAllocatedIdentities =
          visitor.nextGenericParameterId != savedGenericParameterId ||
          visitor.nextConstructorId != savedConstructorId ||
          visitor.nextFunctionId != savedFunctionId ||
          visitor.nextLambdaId != savedLambdaId;
      SemanticInstanceAnalysis result{.model = std::move(visitor.semanticModel),
                                      .diagnostics =
                                          std::move(visitor.diagnostics)};
      visitor.semanticModel = std::move(baseModel);
      visitor.diagnostics = std::move(baseDiagnostics);
      visitor.nextGenericParameterId = savedGenericParameterId;
      visitor.nextConstructorId = savedConstructorId;
      visitor.nextFunctionId = savedFunctionId;
      visitor.nextLambdaId = savedLambdaId;
      visitor.nextSemanticLoanId = savedSemanticLoanId;
      visitor.nextPlaceSelectionId = savedPlaceSelectionId;
      visitor.instanceBaseModel = nullptr;
      visitor.reanalysisBase = nullptr;
      visitor.instanceAnalysisActive = false;
      visitor.requestSession.assertionsOwnedUpChain = false;
      if (!nested)
        openReanalyses->pop_back();
      result.model.rebase(&visitor.semanticModel);
      return result;
    }

    Impl &visitor;
    SemanticModel baseModel;
    std::vector<SemanticDiagnostic> baseDiagnostics;
    GenericParameterId savedGenericParameterId;
    ConstructorId savedConstructorId;
    FunctionId savedFunctionId;
    LambdaId savedLambdaId;
    SemanticLoanId savedSemanticLoanId;
    std::size_t savedPlaceSelectionId;
    std::shared_ptr<std::vector<InstanceAnalysisKey>> openReanalyses;
    bool nested = false;
  };

  // Concrete instances whose reanalysis is open along the current staging
  // chain: this analyzer's own and those of the analyzers whose constants
  // staged into it. Staged workers share the list.
  [[nodiscard]] std::shared_ptr<std::vector<InstanceAnalysisKey>>
  openReanalyses() {
    if (!requestSession.openReanalyses)
      requestSession.openReanalyses =
          std::make_shared<std::vector<InstanceAnalysisKey>>();
    return requestSession.openReanalyses;
  }

  // New concrete-only declarations must not reuse another instance's IDs.
  // Primary declaration IDs remain inherited; the base snapshot stays
  // immutable.
  SymbolId instanceSymbolCeiling = 0;
  // Lambda identities issued by any instance delta so far. A late demand
  // continues the base above it, so a base lambda never shares an identity
  // with a concrete-only lambda of an existing delta.
  LambdaId instanceLambdaCeiling = 0;

  // ---- Late demand ------------------------------------------------------
  // Reachable analysis finishes with compiler-trusted bodies that nothing it
  // analysed demanded. Concrete discovery (HIR, through the public
  // SemanticVisitor provider) can still select one. Rather than discarding
  // the analysis and repeating it with that body required, analyzeLateDemand
  // completes the body's primary analysis in the base model, exactly as the
  // deferred-body drain would have, and finalizes precisely the bodies it
  // analysed. Conditions under which that would not equal analysing the body
  // in the pass make it refuse or fail, and the caller falls back to the
  // bounded repeated analysis.
  struct LateFinalizationScope {
    const std::unordered_set<const FunctionDecl *> *functions = nullptr;
    const std::unordered_set<const Call *> *calls = nullptr;
    std::size_t forwardings = 0;
    std::size_t cThreadCallUses = 0;
    std::size_t unsafeBlockCandidates = 0;
    std::size_t mutableLocalCandidates = 0;
    std::size_t genericDefaultConstantUses = 0;
    // Occurrence counts per source unit before the batch, and the units the
    // batch extended.
    std::unordered_map<SourceUnitId, std::size_t> occurrenceCounts;
    std::unordered_set<SourceUnitId> units;
    // Program-effect owners of the batch: its functions and the lambdas it
    // created (identities from firstLambda on).
    std::unordered_set<FunctionId> functionIds;
    LambdaId firstLambda = 0;
  };
  // Set once the complete request (body phase and finalization) is done.
  bool lateDemandReady = false;
  // A late batch broke an equivalence condition after analysing; the
  // analysis must be repeated with lateDemandedBodies required.
  bool lateDemandFailed = false;
  std::vector<const FunctionDecl *> lateDemandedBodies;
  // Deferred declarations by position in deferredBodies.
  std::unordered_map<const FunctionDecl *, std::size_t> deferredBodyIndex;
  // Program-effect owners the initializer-effect validation visited.
  std::unordered_set<ProgramEffectOwner, ProgramEffectOwnerHash>
      initializerEffectOwners;
  // Bodies analysed by the current late batch, in analysis order.
  std::vector<const FunctionDecl *> *lateAnalyzedBodies = nullptr;
  const LateFinalizationScope *lateFinalization = nullptr;

  // ---- Trusted prefix preamble (semantic_preamble.cpp) -------------------
  // Set while buildPreamble analyses the prefix: bodies Reachable analysis
  // would defer are bucketed instead of reported.
  bool buildingPreamble = false;
  // Set by adoptPreamble: the next check() resumes from this preamble's
  // state and analyses only the declarations after the first
  // adoptedPrefixDeclarations.
  std::shared_ptr<const SemanticPreamble> adoptedPreamble;
  std::size_t adoptedPrefixDeclarations = 0;
  // The declarations a resumed analysis adds; lexical frames point into it,
  // so it lives as long as the analyzer.
  StmtList resumeDeclarations;
  // Preamble bodies whose diagnostics this analysis has reported.
  std::unordered_set<const FunctionDecl *> reachedPreambleBodies;
  // Registration below these positions/identities was adopted from the
  // preamble; whole-catalog registration passes start here.
  struct RegistrationFloor {
    std::size_t classes = 0;
    std::size_t enums = 0;
    std::size_t typeAliases = 0;
    std::size_t concepts = 0;
    FunctionId functions = 0;
  } registrationFloor;
  template <class T>
  [[nodiscard]] static std::span<T> registering(std::vector<T> &items,
                                                std::size_t floor) {
    return std::span<T>(items).subspan(std::min(floor, items.size()));
  }
  template <class T>
  [[nodiscard]] static std::span<const T>
  registering(const std::vector<T> &items, std::size_t floor) {
    return std::span<const T>(items).subspan(std::min(floor, items.size()));
  }
  // Whether bounded analysis would defer this body (its kind and unit), as
  // seen at its declaration; deferTrustedBody adds the mode and context.
  [[nodiscard]] bool reachableDeferrable(const FunctionDecl &stmt) const;
  // check() for an adopted preamble.
  [[nodiscard]] bool checkResumed(const Program &program);
  // The registration passes over `declarations`, honouring registrationFloor
  // for the whole-catalog ones; shared by check() and checkResumed().
  void registerDeclarations(const StmtList &declarations);
  // The body phase over `declarations` (between beginScope and endScope).
  void analyzeBodies(const StmtList &declarations,
                     const LateFinalizationScope *scope);
  void freezePreamble();
  // Drops the caller-owned graph, sources and metrics a frozen preamble
  // must not keep, and makes an unused analyzer of the same configuration.
  void detachFromCaller();
  [[nodiscard]] std::unique_ptr<Impl> blankLikeThis() const;
  void adoptPreambleState();
  // Reports the preamble bodies reached from `roots`, the demanded
  // functions and (with `all`) every preamble body, with the bodies they in
  // turn consulted. Returns whether a reported diagnostic is an error.
  bool reachPreambleBodies(std::span<const FunctionDecl *const> roots,
                           bool all);
  // Reaches the preamble bodies among `consulted` that are not reported
  // yet. When one of them reports an error, marks late demand failed with
  // those bodies required, exactly as a failed late analysis, and returns
  // true.
  bool
  reachConsultedPreambleBodies(const std::unordered_set<FunctionId> &consulted);

  bool analyzeLateDemand(std::span<const FunctionDecl *const> roots);
  void finalizeLateDemand(const LateFinalizationScope &scope);
  void drainDemandedDeferredBodies();
  // A deferred trusted body that no analysis has demanded yet.
  [[nodiscard]] bool pendingDeferredBody(const FunctionDecl *declaration) const;
  // Finalization covers every analysed body, or one late batch.
  [[nodiscard]] bool finalizesFunction(const FunctionInfo &function) const;
  [[nodiscard]] bool finalizesCall(const Call *call) const;

  // Runs one concrete instance analysis for an external (HIR) consumer. An
  // instance that consulted a pending deferred function is analysed again
  // after that function's late demand, so it never keeps a fact that a body
  // analysed in the pass would have changed.
  // `finalConsulted`, when given, receives the functions the returned
  // analysis consulted.
  template <typename Analyze>
  [[nodiscard]] SemanticInstanceAnalysis
  withLateDemand(Analyze analyze,
                 std::unordered_set<FunctionId> *finalConsulted = nullptr) {
    if (!lateDemandReady || bodyPhaseActive || instanceAnalysisActive)
      return analyze();
    for (;;) {
      std::unordered_set<FunctionId> consulted;
      SemanticInstanceAnalysis result;
      {
        const FunctionDemandScope probe(semanticModel, &consulted);
        result = analyze();
      }
      result.model.setFunctionDemandSink(nullptr);
      // A preamble body it consulted is complete already; reaching it reports
      // what the body held back, and an error there fails the request as a
      // fresh late analysis would have.
      if (adoptedPreamble != nullptr &&
          reachConsultedPreambleBodies(consulted)) {
        if (finalConsulted != nullptr)
          *finalConsulted = std::move(consulted);
        return result;
      }
      std::vector<const FunctionDecl *> demanded;
      for (const FunctionId id : consulted) {
        const FunctionInfo *function = semanticModel.findFunction(id);
        if (function != nullptr && pendingDeferredBody(function->declaration))
          demanded.push_back(function->declaration);
      }
      if (demanded.empty() || !analyzeLateDemand(demanded)) {
        if (finalConsulted != nullptr)
          *finalConsulted = std::move(consulted);
        return result;
      }
    }
  }

  // ---- Instance reuse ---------------------------------------------------
  // Bounded (Reachable) analysis may reuse a concrete instance analysis of an
  // earlier snapshot through an InstanceAnalysisCache owned by the frontend
  // caller. Only instances of compiler-trusted declarations whose arguments
  // name only compiler-trusted types qualify, and only within a generation
  // whose trusted units and registration identities are identical. A reused
  // delta must also pass its own journal: every base identity it read still
  // denotes the same declaration, its own identity range is free, and every
  // function it consulted is analysed (pending ones are late-demanded first,
  // exactly as a fresh analysis would). Reuse never replaces analysis with a
  // second path: the delta is this analyzer's own earlier output.
  std::shared_ptr<InstanceAnalysisCache> instanceCache;
  std::string instanceCacheContext;
  enum class InstanceReuseState { Unprepared, Active, Unavailable };
  InstanceReuseState instanceReuseState = InstanceReuseState::Unprepared;
  InstanceAnalysisCache::Generation instanceGeneration;
  const Program *analyzedProgram = nullptr;
  // Symbol identities registration assigned (stage 1-7); body analysis
  // continues above it.
  SymbolId registrationSymbolCeiling = 0;
  bool journalNextInstance = false;
  bool lastInstanceAllocatedIdentities = false;
  // Instance-delta identity ranges (first, last) live in this request.
  std::vector<std::pair<SymbolId, SymbolId>> instanceSymbolRanges;
  std::vector<std::pair<InstanceAnalysisKey, InstanceAnalysisCacheEntry>>
      pendingInstanceEntries;

  void resetInstanceReuse();
  [[nodiscard]] bool prepareInstanceReuse();
  [[nodiscard]] std::uint64_t trustedRegistrationFingerprint() const;
  [[nodiscard]] bool trustedUnit(SourceUnitId unit) const;
  [[nodiscard]] bool reusableInstanceType(const SemanticType &type) const;
  [[nodiscard]] bool reusableInstanceKey(const InstanceAnalysisKey &key);
  [[nodiscard]] const Stmt *
  instanceDeclarationSyntax(const InstanceAnalysisKey &key) const;
  [[nodiscard]] const ConstructorInfo *
  constructorInfoById(ConstructorId id, bool *generated = nullptr) const;
  [[nodiscard]] bool instanceRangeFree(const SemanticModel &delta) const;
  void reserveInstanceRange(const SemanticModel &delta);
  [[nodiscard]] std::optional<SemanticInstanceAnalysis>
  reuseInstance(const InstanceAnalysisKey &key);
  void retainInstance(InstanceAnalysisKey key,
                      SemanticInstanceAnalysis &analysis,
                      const std::unordered_set<FunctionId> &consulted);
  void publishInstanceAnalyses();

  template <typename Analyze>
  [[nodiscard]] SemanticInstanceAnalysis
  analyzeReusableInstance(InstanceAnalysisKey key, Analyze analyze) {
    if (!lateDemandReady || bodyPhaseActive || instanceAnalysisActive ||
        !prepareInstanceReuse())
      return withLateDemand(analyze);
    if (!reusableInstanceKey(key)) {
      if (workMetrics != nullptr)
        ++workMetrics->semanticInstanceReuseIneligible;
      SemanticInstanceAnalysis result = withLateDemand(analyze);
      reserveInstanceRange(result.model);
      return result;
    }
    if (auto reused = reuseInstance(key)) {
      if (workMetrics != nullptr)
        ++workMetrics->semanticInstanceReuseHits;
      return std::move(*reused);
    }
    if (workMetrics != nullptr)
      ++workMetrics->semanticInstanceReuseMisses;
    std::unordered_set<FunctionId> consulted;
    journalNextInstance = true;
    SemanticInstanceAnalysis result = withLateDemand(analyze, &consulted);
    journalNextInstance = false;
    reserveInstanceRange(result.model);
    retainInstance(std::move(key), result, consulted);
    return result;
  }

  [[nodiscard]] SemanticInstanceAnalysis analyzeFunctionInstance(
      FunctionId functionId,
      const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments,
      const std::vector<SemanticType> &functionTypeArguments,
      const std::vector<CompileTimeValue> &functionValueArguments) override;

  [[nodiscard]] SemanticInstanceAnalysis analyzeConstructorInstance(
      ConstructorId constructorId,
      const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments,
      const std::vector<SemanticType> &constructorTypeArguments,
      const std::vector<CompileTimeValue> &constructorValueArguments) override;

  [[nodiscard]] SemanticInstanceAnalysis analyzeDestructorInstance(
      ClassId classId, const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments) override;

  [[nodiscard]] SemanticInstanceAnalysis analyzeClassFieldInitializers(
      ClassId classId, const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments) override;

  void visitAccessSpecifierDecl(const AccessSpecifierDecl &) override;

  void visitEnumDecl(const EnumDecl &stmt) override;

  void visitBlockStmt(const BlockStmt &stmt) override;

  void visitClassDecl(const ClassDecl &stmt) override;

  void visitConditionalStmt(const ConditionalStmt &stmt) override;
  void visitConstexprForStmt(const ConstexprForStmt &stmt) override;

  void visitCompileErrorDirective(const CompileErrorDirective &stmt) override;

  void visitStaticAssertDecl(const StaticAssertDecl &stmt) override;

  void visitConstructorDecl(const ConstructorDecl &stmt) override;

  void visitDestructorDecl(const DestructorDecl &stmt) override;

  void visitDoWhileStmt(const DoWhileStmt &stmt) override;

  void visitEmptyStmt(const EmptyStmt &) override;

  void visitExternCDecl(const ExternCDecl &stmt) override;

  void visitExpressionStmt(const ExpressionStmt &stmt) override;

  void visitForStmt(const ForStmt &stmt) override;

  void visitRangeForStmt(const RangeForStmt &stmt) override;

  void visitFunctionDecl(const FunctionDecl &stmt) override;
  // Trusted-body deferral; see TrustedBodyAnalysis and analyzeDeferredBodies.
  [[nodiscard]] bool deferTrustedBody(const FunctionDecl &stmt) const;
  void analyzeFunctionBody(const FunctionDecl &stmt,
                           const FunctionInfo *functionInfo,
                           bool declarationErrors,
                           bool constexprDeclarationValid);
  void analyzeDeferredBodies();

  void visitIfStmt(const IfStmt &stmt) override;

  void visitLoopControlStmt(const LoopControlStmt &stmt) override;

  void visitMutableFieldGroupDecl(const MutableFieldGroupDecl &stmt) override;

  void visitNamespaceAliasDecl(const NamespaceAliasDecl &stmt) override;

  void visitNamespaceDecl(const NamespaceDecl &stmt) override;

  void visitReturnStmt(const ReturnStmt &stmt) override;

  void visitSwitchStmt(const SwitchStmt &stmt) override;

  void visitStructuredBindingDecl(const StructuredBindingDecl &stmt) override;

  void visitTypeAliasDecl(const TypeAliasDecl &stmt) override;

  void visitConceptDecl(const ConceptDecl &stmt) override;
  void visitConformanceDecl(const ConformanceDecl &stmt) override;

  void visitVariableDecl(const VariableDecl &stmt) override;

  void visitWhileStmt(const WhileStmt &stmt) override;

  void visitAssignExpr(const Assign &expr) override;

  void visitArrayInitializerExpr(const ArrayInitializer &expr) override;
  void analyzeCollectionInitializer(const ArrayInitializer &expr,
                                    const SemanticType &target);

  void visitBinaryExpr(const Binary &expr) override;

  void visitCallExpr(const Call &expr) override;

  [[nodiscard]] static bool
  isCharacterCodeConversion(const SemanticType &source,
                            const SemanticType &target);

  void analyzeTypeParameterCall(const Call &expr,
                                const SemanticType &targetType);

  void analyzeAliasConversion(const Call &expr, const SemanticType &targetType,
                              const std::string &aliasName);

  void visitConditionalExpr(const ConditionalExpr &expr) override;

  void visitConversionExpr(const Conversion &expr) override;

  void visitDirectInitializerExpr(const DirectInitializer &expr) override;

  void visitDereferenceSetExpr(const DereferenceSet &expr) override;
  void visitPlaceSetExpr(const PlaceSet &expr) override;

  void visitGetExpr(const Get &expr) override;

  void visitGroupingExpr(const Grouping &expr) override;

  void visitIndexExpr(const Index &expr) override;

  void visitIndexSetExpr(const IndexSet &expr) override;

  void visitLambdaExpr(const Lambda &expr) override;

  enum class LayoutEvaluationError {
    None,
    UnsupportedType,
    SymbolicType,
    SymbolicExtent,
    ZeroExtent,
    SizeOverflow,
  };

  struct LayoutEvaluation {
    std::uint64_t sizeBytes = 0;
    std::uint32_t abiAlignmentBytes = 0;
    LayoutEvaluationError error = LayoutEvaluationError::None;

    [[nodiscard]] explicit operator bool() const {
      return error == LayoutEvaluationError::None;
    }
  };

  [[nodiscard]] static std::optional<TargetScalarKind>
  layoutScalarKind(const SemanticType &type);

  [[nodiscard]] LayoutEvaluation evaluateLayout(const SemanticType &type) const;

  [[nodiscard]] static const Token &
  lastArrayExtentToken(const ArrayExtentExpr &extent);

  [[nodiscard]] static SourceSpan typeNameSpan(const TypeRef &type);

  [[nodiscard]] const Token *
  directLayoutExtentErrorToken(const TypeRef &type,
                               LayoutEvaluationError error) const;

  [[nodiscard]] SourceSpan
  layoutDiagnosticSpan(const TypeRef &type, LayoutEvaluationError error) const;

  void visitLayoutQueryExpr(const LayoutQuery &expr) override;
  void visitMemberRequirementExpr(const MemberRequirement &expr) override;

  void visitLiteralExpr(const LiteralExpr &expr) override;

  void visitLogicalExpr(const Logical &expr) override;

  void visitPackFoldExpr(const PackFold &expr) override;

  void visitPackExpansionExpr(const PackExpansion &expr) override;

  void visitPostfixExpr(const Postfix &expr) override;

  void visitQualifiedNameExpr(const QualifiedName &expr) override;

  void visitThisExpr(const This &expr) override;

  void visitSetExpr(const Set &expr) override;

  void visitUnaryExpr(const Unary &expr) override;

  void visitVariableExpr(const Variable &expr) override;

private:
  using ValueState = OwnershipStateSet;

  [[nodiscard]] static std::optional<OverloadedOperator>
  binaryOperator(TokenKind kind);

  [[nodiscard]] static std::optional<OverloadedOperator>
  compoundOperator(TokenKind kind);

  [[nodiscard]] static std::optional<OverloadedOperator>
  unaryValueOperator(TokenKind kind);

  [[nodiscard]] static bool isCompoundOperator(OverloadedOperator kind);

  [[nodiscard]] static bool
  isArithmeticOrBitwiseValueOperator(OverloadedOperator kind);

  struct ProjectedValueState {
    std::vector<PlaceProjection> projections;
    ValueState state = ValueState::Available;
    // Diagnostic provenance only; never participates in ownership decisions.
    std::vector<SourceSpan> moveOrigins;
  };

  // The catalog owns exactly one declaration record for each source callable.
  // Registration completes these records before the catalog is sealed; body
  // analysis publishes and queries them but never mutates them.
  struct CallableDeclaration {
    FunctionId id = 0;
    SourceUnitId sourceUnit = 0;
    const FunctionDecl *syntax = nullptr;
    const FunctionDecl *definition = nullptr;
    std::string qualifiedName;
    std::vector<std::string> namespaceScope;
    SemanticType returnType = SemanticType::Unknown;
    std::vector<SemanticType> parameterTypes;
    std::size_t requiredParameterCount = 0;
    std::vector<std::size_t> defaultArgumentEffectOwners;
    std::vector<GenericParameterInfo> genericParameters;
    std::vector<AppliedConceptRequirement> requirements;
    bool parameterPack = false;
    ClassId ownerClass = 0;
    bool entryPoint = false;
    ProgramEntryKind entryKind = ProgramEntryKind::None;
    FunctionId entryArgumentAppendFunction = 0;
    bool staticMember = false;
    bool internalLinkage = false;
    bool constexprFunction = false;
    bool requiresUnsafe = false;
    bool neverReturns = false;
    bool tracksCaller = false;
    bool discardableResult = false;
    bool macroExpansion = false;
    bool threadConfinedCallback = false;
    bool testFunction = false;
    CThreadRuntimeOperation cThreadOperation = CThreadRuntimeOperation::None;
    ProcessMemoryRuntimeOperation processMemoryOperation =
        ProcessMemoryRuntimeOperation::None;
    LanguageLinkage linkage = LanguageLinkage::Gti;
    std::string externalSymbol;
    std::optional<std::size_t> cArrayCountParameter;
    bool virtualMethod = false;
    bool pureVirtual = false;
    bool overrideMethod = false;
    IntrinsicKind intrinsic = IntrinsicKind::None;
    BorrowOriginKind returnBorrowOrigin = BorrowOriginKind::None;
    std::size_t returnBorrowParameter = 0;
    AccessMode returnBorrowAccess = AccessMode::ReadOnly;
    std::vector<std::size_t> returnBorrowParameters;
    std::vector<BorrowedFieldOrigin> returnBorrowFields;
    std::optional<BorrowOriginPlace> returnBorrowPlace;
    std::optional<OwnerlessTextField> returnedTextReference;
    std::vector<FunctionId> virtualRoots;
    bool compilerPrivate = false;
  };

  // Resolution may copy this bounded view to substitute types and adjust the
  // receiver/access/dispatch context. Declaration identity and source policy
  // stay in the catalog-owned record referenced by `declarationRecord`.
  struct CallableCandidate {
    const CallableDeclaration *declarationRecord = nullptr;
    FunctionId id = 0;
    const FunctionDecl *declaration = nullptr;
    SemanticType returnType = SemanticType::Unknown;
    std::vector<SemanticType> parameterTypes;
    std::size_t requiredParameterCount = 0;
    std::vector<AppliedConceptRequirement> requirements;
    bool parameterPack = false;
    ClassId ownerClass = 0;
    ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly;
    AccessModifier access = AccessModifier::Public;
    bool staticMember = false;
    bool virtualMethod = false;
    bool pureVirtual = false;
    bool overrideMethod = false;
    BorrowOriginKind returnBorrowOrigin = BorrowOriginKind::None;
    std::size_t returnBorrowParameter = 0;
    AccessMode returnBorrowAccess = AccessMode::ReadOnly;
    std::vector<std::size_t> returnBorrowParameters;
    std::vector<BorrowedFieldOrigin> returnBorrowFields;
    std::optional<BorrowOriginPlace> returnBorrowPlace;
    std::optional<OwnerlessTextField> returnedTextReference;
    std::vector<FunctionId> virtualRoots;
    SemanticType dispatchOwner = SemanticType::Unknown;
    bool compilerPrivate = false;
    // Only provisional open-body candidates lack a declaration record.
    bool requiresUnsafeFact = false;

    [[nodiscard]] const CallableDeclaration &record() const {
      assert(declarationRecord != nullptr);
      return *declarationRecord;
    }
    [[nodiscard]] const FunctionDecl *bodyDeclaration() const {
      return declarationRecord != nullptr &&
                     declarationRecord->definition != nullptr
                 ? declarationRecord->definition
                 : declaration;
    }
  };

  void bindOutOfClassDefinitions(const StmtList &statements,
                                 std::vector<std::string> scope);

  [[nodiscard]] static std::size_t
  requiredParameterCount(std::span<const Parameter> parameters);

  [[nodiscard]] static std::vector<const Expr *>
  omittedDefaultArguments(std::span<const Parameter> parameters,
                          std::size_t suppliedArguments);

  [[nodiscard]] std::vector<std::size_t>
  allocateDefaultArgumentEffectOwners(std::span<const Parameter> parameters);

  enum class DefaultArgumentOwnerKind {
    Function,
    Constructor,
  };

  void recordDefaultArgumentDependencies(std::span<const Expr *const> defaults);

  void validateDefaultArgumentCycles();

  void analyzeParameterDefaults(std::span<const Parameter> parameters,
                                std::span<const SemanticType> parameterTypes,
                                std::span<const std::size_t> effectOwners,
                                DefaultArgumentOwnerKind ownerKind,
                                bool defaultsPermitted,
                                std::string_view unsupportedContext,
                                bool hasParameterPack,
                                bool validateDeclarationRules);

  struct FunctionReturnBorrowSummary {
    BorrowOriginKind origin = BorrowOriginKind::None;
    std::size_t parameter = 0;
    AccessMode access = AccessMode::ReadOnly;
    std::vector<std::size_t> parameters;
    std::vector<BorrowedFieldOrigin> fields;
    std::optional<BorrowOriginPlace> place;
    std::size_t eligibleParameters = 0;
    bool hasMutableCandidate = false;
  };

  [[nodiscard]] std::optional<OwnedPointeeProjection>
  ownedPointeeReturnProjection(const FunctionInfo &function);

  struct AnalyzedCallArgument {
    SemanticType type = SemanticType::Unknown;
    const ExprPtr *expression = nullptr;
    bool forwardedPackElement = false;
    bool deferred = false;
    // The exact element identity of a flattened concrete pack argument.
    // A type alone cannot recover origins when repeated element types occur.
    std::vector<std::size_t> packPath;
  };

  struct ContextualCallArguments {
    std::vector<SemanticType> types;
    std::vector<bool> deferred;

    [[nodiscard]] bool hasDeferred() const {
      return std::any_of(deferred.begin(), deferred.end(),
                         [](bool value) { return value; });
    }
  };

  using ConstraintFailure = SemanticConstraintFailure;

  struct ExpandedStructuralRequirement {
    StructuralConstraintKind kind = StructuralConstraintKind::None;
    std::vector<SemanticType> arguments;
    const ConceptApplication *application = nullptr;
    std::string memberName;
    bool mutableReceiver = false;
    bool requiresUnsafe = false;
  };

  struct ContextualCallableResult {
    const Call *call = nullptr;
    SemanticType type = SemanticType::Unknown;
    bool inferResultFromInstantiation = false;
  };

  // How one expression's result is used, for choosing between otherwise exact
  // read-only and mutable-receiver overloads of an accessor. Steps are the
  // projections applied to the result, innermost first; `top` is the access
  // the outermost use needs. Unknown keeps the mutable-receiver preference.
  enum class ResultUseAccess { Unknown, ReadOnly, Mutable };
  struct ResultUseStep {
    enum class Kind { Field, Member, Operator, Bind };
    Kind kind = Kind::Field;
    std::string name;
    OverloadedOperator operation = OverloadedOperator::Subscript;
    // Bind: the declared types the value may initialize, such as each
    // candidate's parameter at the argument's position.
    std::vector<SemanticType> targets;
  };
  struct ResultUse {
    const Expr *expression = nullptr;
    std::vector<ResultUseStep> steps;
    ResultUseAccess top = ResultUseAccess::Unknown;
  };

  struct LoanFlowRecord {
    SemanticLoanId id = 0;
    std::size_t region = 0;
    std::size_t lastUseOrder = 0;
    const Stmt *lastUse = nullptr;
    std::unordered_set<SymbolId> carriers;
    bool confinedToRegion = true;
    bool hasAliases = false;
    bool lifecycleBound = false;
    bool active = true;
  };

  struct RetainedLoanSource {
    SemanticLoanId loan = 0;
    const Variable *carrier = nullptr;
    bool transfer = false;
    std::optional<SemanticLoanDependency> dependency;
  };

  enum class StoredCarrierReturnProvenance {
    None,
    CarrierValue,
    CarrierShellProjection,
    ExternalReferent,
    ExternalReferentProjection,
  };

  struct ActiveLoanFlowConditionalArm {
    std::size_t conditional = 0;
    bool thenBranch = false;

    friend bool operator==(const ActiveLoanFlowConditionalArm &,
                           const ActiveLoanFlowConditionalArm &) = default;
  };

  struct ActiveLoanFlowSwitchArm {
    std::size_t switchIndex = 0;
    std::size_t armIndex = 0;

    friend bool operator==(const ActiveLoanFlowSwitchArm &,
                           const ActiveLoanFlowSwitchArm &) = default;
  };

  struct PendingBorrowConflict {
    std::vector<SemanticLoanId> loans;
    std::size_t region = 0;
    std::size_t order = 0;
    std::size_t sequence = 0;
    const Stmt *statement = nullptr;
    std::vector<ActiveLoanFlowConditionalArm> path;
    std::vector<ActiveLoanFlowSwitchArm> switchPath;
    std::vector<std::size_t> loops;
    Diagnostic diagnostic;
  };

  struct LoanFlowUse {
    SemanticLoanId loan = 0;
    std::size_t sequence = 0;
    std::size_t region = 0;
    std::size_t order = 0;
    const Stmt *statement = nullptr;
    std::vector<ActiveLoanFlowConditionalArm> path;
    std::vector<ActiveLoanFlowSwitchArm> switchPath;
  };

  struct LoanFlowStatementBoundary {
    std::size_t region = 0;
    std::size_t order = 0;
    const Stmt *statement = nullptr;
  };

  struct LoanFlowSwitchBoundary {
    std::size_t region = 0;
    std::size_t order = 0;
    const SwitchStmt *statement = nullptr;
  };

  struct LoanFlowLoop {
    std::size_t region = 0;
    std::size_t order = 0;
    const Stmt *statement = nullptr;
    // IDs below this boundary existed before loop entry and may be projected
    // to its unified exit. Newer loans belong to initializer/body cleanup.
    SemanticLoanId firstNestedLoan = 0;
    // Writable bindings whose complete borrow dependency set this loop's entry
    // dropped. Diagnostic provenance only; never an ownership fact.
    std::vector<SymbolId> droppedBorrowSets;
  };

  struct LoanFlowConditionalArm {
    std::size_t useBegin = 0;
    std::size_t useEnd = 0;
    std::size_t conflictBegin = 0;
    std::size_t conflictEnd = 0;
    std::vector<std::size_t> nestedConditionals;
    bool containsUnsupportedControlFlow = false;
  };

  struct LoanFlowConditional {
    const IfStmt *statement = nullptr;
    std::size_t useBegin = 0;
    std::size_t useEnd = 0;
    std::size_t conflictBegin = 0;
    std::size_t conflictEnd = 0;
    std::size_t endSequence = 0;
    std::vector<ActiveLoanFlowConditionalArm> parentPath;
    std::vector<ActiveLoanFlowSwitchArm> parentSwitchPath;
    LoanFlowConditionalArm thenArm;
    LoanFlowConditionalArm elseArm;
  };

  struct LoanFlowSwitchArm {
    bool containsUnsupportedControlFlow = false;
  };

  struct LoanFlowSwitch {
    const SwitchStmt *statement = nullptr;
    std::size_t endSequence = 0;
    std::vector<ActiveLoanFlowConditionalArm> parentPath;
    std::vector<ActiveLoanFlowSwitchArm> parentSwitchPath;
    std::vector<LoanFlowSwitchArm> arms;
    bool hasDefault = false;
  };

  struct LoanFlowBreak {
    const LoopControlStmt *statement = nullptr;
    const Stmt *target = nullptr;
    std::size_t region = 0;
    std::size_t order = 0;
    std::size_t sequence = 0;
    std::vector<ActiveLoanFlowConditionalArm> path;
    std::vector<ActiveLoanFlowSwitchArm> switchPath;
  };

  struct LoanFlowArmEndpoint {
    bool atEntry = true;
    bool proofOnly = false;
    const Stmt *after = nullptr;
    std::size_t sequence = 0;
    const IfStmt *entryConditional = nullptr;
    bool thenBranch = false;
    const SwitchStmt *entrySwitch = nullptr;
    std::size_t switchArm = 0;
    std::vector<ActiveLoanFlowConditionalArm> path;
    std::vector<ActiveLoanFlowSwitchArm> switchPath;
  };

  struct LoanFlowConditionalEndPlan {
    std::size_t conditional = 0;
    const IfStmt *statement = nullptr;
    std::vector<LoanFlowArmEndpoint> endpoints;
  };

  struct LoanFlowSwitchEndPlan {
    std::size_t switchIndex = 0;
    const SwitchStmt *statement = nullptr;
    std::vector<LoanFlowArmEndpoint> endpoints;
  };

  struct LoanFlowBreakEndPlan {
    std::size_t conflict = 0;
    LoanFlowArmEndpoint endpoint;
  };

  struct LoanFlowState {
    // A symbolic constexpr branch has unknown control/ownership effects.
    // Name/type checking continues, but this body's flow is not a proof until
    // concrete instance analysis selects every branch and reruns the checks.
    bool deferredGenericFlow = false;
    // A synthetic early return needs its own complete result evidence;
    // explicit ReturnStmt facts alone cannot describe the callable's result.
    bool unknownImplicitResultReturn = false;
    // The complete result sets of synthetic early returns that have that
    // evidence, in addition to the body's explicit ReturnStmt facts.
    std::vector<SemanticBorrowDependencySet> implicitResultReturns;
    std::size_t rawConditionalExpressionDepth = 0;
    std::unordered_map<SemanticLoanId, LoanFlowRecord> records;
    std::unordered_map<SymbolId, SemanticLoanId> carrierLoans;
    std::vector<SemanticLoanId> receiverStorageLoans;
    std::vector<PendingBorrowConflict> conflicts;
    // Audit uses may inspect checked liveness, but cannot request an endpoint
    // or otherwise change the mandatory loan schedule.
    std::vector<PendingBorrowConflict> rawAuditConflicts;
    std::vector<LoanFlowUse> uses;
    std::vector<LoanFlowStatementBoundary> statementBoundaries;
    std::vector<LoanFlowSwitchBoundary> switchBoundaries;
    std::vector<LoanFlowLoop> loops;
    std::vector<std::size_t> activeLoops;
    std::vector<LoanFlowConditional> conditionals;
    std::vector<ActiveLoanFlowConditionalArm> activeConditionalArms;
    std::vector<LoanFlowSwitch> switches;
    std::vector<ActiveLoanFlowSwitchArm> activeSwitchArms;
    std::vector<LoanFlowBreak> breaks;
    std::size_t nextRegion = 1;
    std::size_t nextSequence = 1;
    std::size_t currentRegion = 0;
    std::size_t currentOrder = 0;
    const Stmt *currentStatement = nullptr;
  };

  // Address provenance is not a loan: copying it neither retains the source
  // nor prevents movement. It only preserves facts needed at an unsafe use.
  struct RawPointerOrigin {
    PlaceKey place;
    SourceSpan addressSite;
    SemanticLoanId throughLoan = 0;
    bool invalidated = false;
    // Symbolic input pointee, not the address of the parameter's stack slot.
    bool parameterPointee = false;
    // Ephemeral loop transfer variable; place names a pointer, not a pointee.
    std::size_t loopInput = 0;
    bool expired = false;
    // An unresolved source result is not a foreign address. It may be copied
    // inertly, but memory access requires a completed provenance query.
    std::size_t sourceResultQuery = 0;
    std::vector<PlaceKey> lifetimeInvalidations;
    // A type-only origin has no place and grants no lifetime/access proof.
    SemanticType pointeeType = SemanticType::Unknown;
    // A may-origin survives a choice or conservative source summary; even a
    // singleton then does not prove that this storage is selected at the use.
    bool possible = false;
    friend bool operator==(const RawPointerOrigin &,
                           const RawPointerOrigin &) = default;
  };

  struct ProjectedRawPointerOrigins {
    std::vector<PlaceProjection> projections;
    std::vector<RawPointerOrigin> origins;
    friend bool operator==(const ProjectedRawPointerOrigins &,
                           const ProjectedRawPointerOrigins &) = default;
  };

  struct RawResultOrigin {
    RawPointerOrigin origin;
    std::optional<std::size_t> parameter;
    bool receiver = false;
    bool pointerInput = false;
    bool globalInput = false;
  };

  struct RawResultField {
    std::vector<PlaceProjection> projections;
    std::vector<RawResultOrigin> origins;
  };

  struct RawResultWrite {
    RawResultOrigin destination;
    std::vector<RawResultField> fields;
    bool strong = false;
  };

  struct RawResultAccess {
    std::vector<RawResultField> fields;
    AccessMode access = AccessMode::ReadOnly;
    bool checksLoans = true;
    SourceSpan site;
  };

  struct RawResultQuery {
    FunctionId function = 0;
    ConstructorId constructor = 0;
    ClassId defaultOwner = 0;
    std::vector<SemanticType> ownerTypes;
    std::vector<CompileTimeValue> ownerValues;
    std::vector<SemanticType> types;
    std::vector<CompileTimeValue> values;
    std::vector<RawResultField> fields;
    std::vector<RawResultWrite> writes;
    std::vector<RawResultAccess> accesses;
    bool linearWrites = false;
    bool writesComplete = true;
    bool accessesComplete = true;
    std::size_t identity = 0;
    // Only independently budgeted top-level outcomes may be reused by another
    // demand. Nested results depend on their enclosing family's remaining work.
    bool topLevel = false;
    bool complete = false;
    bool valid = false;
    std::optional<Diagnostic> failure;
  };

  struct SemanticCatalog;

  struct RawResultQueryState {
    // Interned diagnostic records only. Keeping evidence is independent of
    // admitting a complete root result to the bounded reuse cache.
    std::vector<std::shared_ptr<RawResultQuery>> entries;
    std::vector<std::shared_ptr<RawResultQuery>> cachedRoots;
    std::vector<std::shared_ptr<RawResultQuery>> familyEntries;
    std::size_t nextIdentity = 1;
    std::size_t depth = 0;
    std::optional<bool> hasRawGlobals;
    // Sealed catalog snapshots released by completed query workers. The
    // request catalog is sealed and asserted unchanged across every instance
    // analysis, so each snapshot has the same content; a later worker takes a
    // released one instead of re-registering every callable declaration.
    std::vector<SemanticCatalog> idleCatalogs;
  };

  struct CompleteReturnQuery {
    InstanceAnalysisKey key;
    std::optional<FunctionBorrowDependencySummary> result;
    bool complete = false;
  };
  struct CompleteReturnQueryState {
    std::unordered_map<InstanceAnalysisKey,
                       std::shared_ptr<CompleteReturnQuery>,
                       InstanceAnalysisKeyHash>
        entries;
    std::unordered_set<InstanceAnalysisKey, InstanceAnalysisKeyHash> discovered;
    std::size_t depth = 0;
    // Retention is independent of the compilation resource policy: completed
    // entries can be discarded and recomputed without changing acceptance.
    static constexpr std::size_t maximumRetainedResults = 64;
    std::vector<InstanceAnalysisKey> completed;
    std::vector<SemanticCatalog> idleCatalogs;
  };

  struct Symbol {
    SemanticType type = SemanticType::Unknown;
    std::optional<ConstantValue> constant;
    SourceUnitId sourceUnit = 0;
    bool assignable = false;
    ValueState valueState = ValueState::Available;
    // Diagnostic provenance only; never participates in ownership decisions.
    std::vector<SourceSpan> moveOrigins;
    std::vector<RawPointerOrigin> rawPointerOrigins;
    std::vector<ProjectedRawPointerOrigins> rawPointerFields;
    std::vector<CallableCandidate> overloads;
    ClassId ownerClass = 0;
    Token declaration;
    const VariableDecl *variableDeclaration = nullptr;
    const Parameter *parameterDeclaration = nullptr;
    // Lexical order of a namespace global's declaration statement; 0 for
    // every other symbol. See symbolVisibleNow.
    std::size_t declarationOrder = 0;
    SemanticLoanId retainedLoan = 0;
    std::vector<SemanticLoanId> storageLoans;
    // Conditional dependency facts follow value flow. Existing valueState and
    // loan flow remain the authority for availability and liveness.
    std::optional<SemanticBorrowDependencySet> completeBorrowSet;
    bool lambdaCapture = false;
    SemanticBindingKind bindingKind = SemanticBindingKind::None;
    std::string qualifiedName;
    bool staticMember = false;
    bool internalLinkage = false;
    SymbolId toolingSymbol = 0;
    AccessModifier access = AccessModifier::Public;
    std::vector<ProjectedValueState> projectedValueStates;
    bool compilerPrivate = false;
  };

  struct SemanticPlace {
    Symbol *root = nullptr;
    SymbolId rootIdentity = 0;
    std::vector<PlaceProjection> projections;
    bool receiver = false;
  };

  struct RawProgramStorageState {
    Symbol *symbol = nullptr;
    enum class Namespace {
      Global,
      Internal,
      Visible,
      StaticField
    } space = Namespace::Global;
    SourceUnitId unit = 0;
    std::string name;
    std::vector<RawPointerOrigin> origins;
    std::vector<ProjectedRawPointerOrigins> fields;
    const VariableDecl *staticField = nullptr;
  };

  struct OperatorReceiverState {
    SemanticPlace place;
    bool mutableReceiver = false;
    std::optional<ExpressionInfo> info;
  };

  // One resolved source invocation, independent of its surface syntax. Operator
  // resolution supplies this while synthetic receiver places are still alive.
  struct RawInvocation {
    ResolvedCallInfo resolved;
    std::span<const ExprPtr> arguments;
    const Expr *receiver = nullptr;
    std::optional<SemanticPlace> receiverPlace;
    SemanticType receiverType = SemanticType::Unknown;
    Token site;
  };

  struct ResolvedFieldUse {
    const Token *use = nullptr;
    const Symbol *symbol = nullptr;
  };

  struct ScopeIdentity {};
  struct Scope : std::unordered_map<std::string, Symbol> {
    // Only isolated query frames need an identity. Ordinary lexical frames
    // remain allocation-free; snapshots retain the same lexical identity.
    std::shared_ptr<const ScopeIdentity> identity;
  };
  using ScopeStack = std::vector<Scope>;

  enum class ValueControlFlowKind {
    Loop,
    Switch,
  };

  struct ValueControlFlowContext {
    ValueControlFlowKind kind = ValueControlFlowKind::Loop;
    const Stmt *statement = nullptr;
    std::vector<ScopeStack> breakStates;
    std::vector<ScopeStack> continueStates;
  };

  struct RawPointerLoopUse {
    std::vector<RawPointerOrigin> origins;
    std::vector<Diagnostic> diagnostics;
    std::size_t pendingLoops = 0;
  };

  struct RawPointerLoop {
    struct Write {
      RawPointerOrigin destination;
      std::vector<PlaceProjection> projections;
      std::vector<ProjectedRawPointerOrigins> fields;
    };
    struct Access {
      std::vector<RawPointerOrigin> origins;
      Token site;
      std::shared_ptr<RawPointerLoopUse> deferred;
      // A type-only use is inert: check contradictions, not pointee lifetime.
      SemanticType expectedPointee = SemanticType::Unknown;
      std::optional<AccessMode> memoryAccess;
      std::shared_ptr<LoanFlowState> loanContext;
    };
    std::size_t identity = 0;
    std::vector<Access> accesses;
    std::vector<Write> writes;
  };

  struct MemberInfo {
    Symbol symbol;
    AccessModifier access = AccessModifier::Private;
  };

  struct MemberTypeAliasInfo {
    TypeAliasId id = 0;
    const TypeAliasDecl *declaration = nullptr;
    SemanticType type = SemanticType::Unknown;
    std::string qualifiedName;
    ClassId ownerClass = 0;
    AccessModifier access = AccessModifier::Private;
    bool compilerPrivate = false;
  };

  struct FieldInfo {
    const VariableDecl *declaration = nullptr;
  };

  // `@compiler_type` bindings of classes in the trusted std/meta unit.
  // MetaList, MetaField and MetaType are ordinary classes the compiler must
  // be able to name; MetaFieldList and MetaFieldType are computed types whose
  // concrete instances canonicalization rewrites.
  enum class CompilerTypeKind {
    None,
    MetaList,
    MetaField,
    MetaType,
    MetaFieldList,
    MetaFieldType,
    MetaFieldAttributes,
    MetaTypeAttributes,
    MetaEnumerator,
    MetaPayloadField,
    MetaEnumeratorList,
    MetaPayloadList,
    MetaPayloadType,
    MetaBaseList,
    MetaArgumentList,
    MetaMethod,
    MetaParameter,
    MetaConstructor,
    MetaConstructorParameter,
    MetaMethodList,
    MetaOperatorList,
    MetaParameterList,
    MetaParameterType,
    MetaReturnType,
    MetaCallableReturnType,
    MetaConstructorList,
    MetaConstructorParameterList,
    MetaConstructorParameterType,
    // `list<First, Rest...>` -> First / `list<Rest...>`.
    MetaListFirst,
    MetaListRest,
  };

  struct ClassInfo {
    enum class DefaultsState { Unresolved, Resolving, Resolved, Invalid };
    ClassId id = 0;
    SourceUnitId sourceUnit = 0;
    const ClassDecl *declaration = nullptr;
    Token name;
    ClassKind kind = ClassKind::Class;
    std::vector<std::string> namespaceScope;
    ClassId enclosingClass = 0;
    AccessModifier enclosingAccess = AccessModifier::Public;
    std::unordered_map<std::string, ClassId> nestedTypes;
    ClassId exactSpecializationPrimary = 0;
    // Disjoint source definitions share a lookup name, not a
    // concrete class identity. The first registered definition owns the list.
    ClassId definitionFamilyPrimary = 0;
    std::vector<ClassId> definitionFamilyMembers;
    std::vector<SemanticType> exactTypeArguments;
    std::vector<CompileTimeValue> exactValueArguments;
    std::vector<GenericParameterInfo> genericParameters;
    DefaultsState defaultsState = DefaultsState::Unresolved;
    std::vector<SemanticType> genericArgumentPatterns;
    bool argumentPatternsResolving = false;
    bool argumentPatternsResolved = false;
    std::unordered_map<std::string, MemberInfo> members;
    std::unordered_map<std::string, MemberTypeAliasInfo> typeAliases;
    std::vector<FieldInfo> fields;
    std::vector<FieldInfo> staticFields;
    std::vector<ConstructorInfo> constructors;
    std::optional<ConstructorInfo> designatedConstructor;
    // The same generated constructor with its parameters in reverse field
    // order; std::meta::construct selects it because a GTI call can only
    // extend a parameter pack at its front.
    std::optional<ConstructorInfo> reversedDesignatedConstructor;
    std::optional<ConstructorInfo> copyConstructor;
    std::optional<ConstructorInfo> moveConstructor;
    std::optional<DestructorInfo> destructor;
    std::optional<StoredReferenceInfo> storedReference;
    std::vector<ClassBaseTypeInfo> bases;
    bool abstract = false;
    bool polymorphic = false;
    bool cAbiRecord = false;
    std::optional<Token> cAbiAttribute;
    bool cOpaqueHandle = false;
    std::optional<Token> cOpaqueAttribute;
    // `[[attribute]]`: a struct whose constant instances annotate
    // declarations; `[[reflect_private]]`: reflected access reaches private
    // fields from anywhere.
    bool attributeType = false;
    std::optional<Token> attributeAttribute;
    bool reflectPrivate = false;
    std::optional<Token> reflectPrivateAttribute;
    std::optional<CAbiRecordLayout> cAbiLayout;
    std::optional<UnionLayout> unionLayout;
    ConcurrencyCapabilityPolicy transferPolicy =
        ConcurrencyCapabilityPolicy::Structural;
    ConcurrencyCapabilityPolicy sharePolicy =
        ConcurrencyCapabilityPolicy::Structural;
    std::optional<Token> transferPolicySource;
    std::optional<Token> sharePolicySource;
    std::optional<SemanticType> transferCondition;
    std::optional<SemanticType> shareCondition;
    bool compilerPrivate = false;
    CompilerCapabilityTypeKind compilerCapability =
        CompilerCapabilityTypeKind::None;
    NativeCScalarKind nativeCScalar = NativeCScalarKind::None;
    CompilerTypeKind compilerType = CompilerTypeKind::None;
  };

  struct ExactClassSpecialization {
    ClassId primary = 0;
    ClassId specialization = 0;
    std::vector<SemanticType> typeArguments;
    std::vector<CompileTimeValue> valueArguments;
  };

  struct CapabilityPolicyRegistration {
    ConcurrencyCapabilityPolicy transfer =
        ConcurrencyCapabilityPolicy::Structural;
    ConcurrencyCapabilityPolicy share = ConcurrencyCapabilityPolicy::Structural;
    std::optional<Token> transferSource;
    std::optional<Token> shareSource;
    std::optional<SemanticType> transferCondition;
    std::optional<SemanticType> shareCondition;
    bool cAbiRecord = false;
    std::optional<Token> cAbiSource;
    bool cOpaqueHandle = false;
    std::optional<Token> cOpaqueSource;
    bool attributeType = false;
    std::optional<Token> attributeSource;
    bool reflectPrivate = false;
    std::optional<Token> reflectPrivateSource;
  };

  struct ShareCapabilityDenial {
    const Token *source = nullptr;
    std::string message;
  };

  struct TransferCapabilityDenial {
    const Token *source = nullptr;
    std::string message;
  };

  struct EnumeratorRecord {
    const EnumeratorDecl *declaration = nullptr;
    EnumConstant value;
    std::size_t variantIndex = 0;
    std::vector<SemanticType> payloadTypes;
    std::vector<SymbolId> payloadSymbols;
    Symbol symbol;
  };

  struct EnumInfo {
    enum class DefaultsState { Unresolved, Resolving, Resolved, Invalid };
    EnumId id = 0;
    SourceUnitId sourceUnit = 0;
    const EnumDecl *declaration = nullptr;
    Token name;
    std::vector<std::string> namespaceScope;
    std::vector<GenericParameterInfo> genericParameters;
    DefaultsState defaultsState = DefaultsState::Unresolved;
    SemanticType underlyingType = SemanticType::Int32;
    std::unordered_map<std::string, EnumeratorRecord> enumerators;
    bool payload = false;
    bool compilerPrivate = false;
  };

  struct NamespaceAliasInfo {
    std::string target;
    SourceUnitId sourceUnit = 0;
    bool compilerPrivate = false;
  };

  struct ResolvedNamespaceSegment {
    std::string declaration;
    std::string target;
  };

  enum class TypeAliasResolution {
    Unresolved,
    Resolving,
    Resolved,
    Invalid,
  };

  struct RegisteredTypeAlias {
    SourceUnitId sourceUnit = 0;
    const TypeAliasDecl *declaration = nullptr;
    const ClassDecl *ownerDeclaration = nullptr;
    std::string qualifiedName;
    std::vector<std::string> namespaceScope;
    std::vector<std::string> lookupScope;
    ClassId ownerClass = 0;
    AccessModifier access = AccessModifier::Public;
    SemanticType type = SemanticType::Unknown;
    TypeAliasResolution resolution = TypeAliasResolution::Unresolved;
    bool compilerPrivate = false;
  };

  struct ResolvedTypeAlias {
    TypeAliasId id = 0;
    const TypeAliasDecl *declaration = nullptr;
    SemanticType type = SemanticType::Unknown;
    std::string qualifiedName;
    ClassId ownerClass = 0;
    AccessModifier access = AccessModifier::Public;
    bool compilerPrivate = false;
    bool specializedOwner = false;
    SemanticType ownerType = SemanticType::Unknown;
    std::vector<ResolvedTypeAlias> qualifiers;
  };

  enum class ConceptResolution {
    Unresolved,
    Resolving,
    Resolved,
    Invalid,
  };

  struct RegisteredConcept {
    ConceptId id = 0;
    SourceUnitId sourceUnit = 0;
    const ConceptDecl *declaration = nullptr;
    std::string qualifiedName;
    std::vector<std::string> namespaceScope;
    GenericConstraintSet constraints = 0;
    std::vector<GenericConstraintSet> parameterConstraints;
    struct StructuralPattern {
      struct Argument {
        std::optional<std::size_t> parameter;
        SemanticType concrete = SemanticType::Unknown;

        friend bool operator==(const Argument &, const Argument &) = default;
      };
      StructuralConstraintKind kind = StructuralConstraintKind::None;
      std::vector<Argument> arguments;
      std::string memberName;
      bool mutableReceiver = false;
      bool requiresUnsafe = false;

      friend bool operator==(const StructuralPattern &,
                             const StructuralPattern &) = default;
    };
    struct RequirementPattern {
      ConceptId conceptId = 0;
      const ConceptApplication *syntax = nullptr;
      std::vector<StructuralPattern::Argument> arguments;
      std::size_t expressionApplication =
          ConstraintExpressionNode::invalidIndex;
    };
    std::vector<StructuralPattern> structuralConstraints;
    std::vector<RequirementPattern> definitionRequirements;
    const ConstraintExpression *definitionExpression = nullptr;
    bool hasStructuralRequirements = false;
    ConceptResolution resolution = ConceptResolution::Unresolved;
    SymbolId symbol = 0;
    std::vector<SymbolId> parameterSymbols;
    std::vector<GenericParameterId> parameterIds;
    bool compilerPrivate = false;
    // A concept with a body is satisfied only through conformance blocks.
    // Operation types are in the concept parameters' space, where an
    // associated name types as a projection of those parameters.
    struct ProtocolOperation {
      std::string name;
      const FunctionDecl *declaration = nullptr;
      SemanticType returnType = SemanticType::Unknown;
      std::vector<SemanticType> parameterTypes;
      // A declaration-relative result origin. The contract is inert for
      // concrete owned results and binds checked borrowed results.
      std::optional<std::size_t> returnBorrowParameter;
    };
    bool protocol = false;
    std::vector<std::string> associatedTypes;
    std::vector<ProtocolOperation> operations;

    // `[[unsatisfied("...")]]`: what satisfies the concept, in its author's
    // words.
    [[nodiscard]] std::optional<std::string> explanation() const {
      if (declaration == nullptr || !declaration->explanation())
        return std::nullopt;
      return std::string(declaration->explanation()->text());
    }
  };

  // One `for <Parameters> concept Concept<Patterns...>` block. Its patterns
  // and associated types are in the block's parameter space; selecting the
  // block for concrete arguments binds those parameters.
  struct RegisteredConformance {
    ConceptId conceptId = 0;
    SourceUnitId sourceUnit = 0;
    const ConformanceDecl *declaration = nullptr;
    // The enclosing namespace, and the generated namespace holding the
    // block's functions.
    std::vector<std::string> namespaceScope;
    std::vector<std::string> functionScope;
    std::vector<GenericParameterInfo> genericParameters;
    // One pattern per concept parameter: a closed concrete type, or one class
    // or enum template applied to block parameters (an open pattern).
    std::vector<SemanticType> patterns;
    std::vector<SemanticType> associatedTypes;
    std::vector<Symbol> operations;
    bool operationsBound = false;
  };

  // A block's written patterns and `using` types, with the scope they are
  // written in, kept for validateConformanceTypes whether or not the block
  // registered.
  struct ConformanceTypeCheck {
    SourceUnitId sourceUnit = 0;
    std::vector<std::string> namespaceScope;
    std::vector<GenericParameterInfo> genericParameters;
    std::vector<const TypeRef *> types;
  };

  struct ConformanceMatch {
    std::size_t conformance = 0;
    std::vector<SemanticType> binding;
  };

  struct ProtocolOperationTarget {
    Symbol symbol;
    std::vector<SemanticType> typeArguments;
  };

  struct FlowSummary {
    bool canFallThrough = true;
    bool breaksEnclosingControl = false;
    bool continuesEnclosingLoop = false;
    const ReturnStmt *normalReturn = nullptr;
  };

  struct ConstexprEvaluation {
    std::optional<ConstantValue> value;
    ConstantEvaluationFailure failure = ConstantEvaluationFailure::None;
    Token location;
    std::string detail;
    // A failure inside a compiler-trusted library body is anchored at the
    // user's evaluation site; this keeps the library location for a note.
    std::optional<Token> libraryLocation;

    [[nodiscard]] explicit operator bool() const { return value.has_value(); }
  };

  using ConstexprLocalValue = std::optional<ConstantValue>;

  struct ConstexprWorkBudget {
    std::size_t steps = 0;
    std::size_t stagingDepth = 0;
  };

  struct ConstexprInterpreter {
    std::shared_ptr<ConstexprWorkBudget> work;
    std::size_t callDepth = 0;
    std::vector<std::unordered_map<SymbolId, ConstexprLocalValue>> scopes;
    std::vector<GenericSubstitution> substitutions;
    std::vector<ConstantValue> receivers;
  };

  enum class ConstexprControlFlow {
    Next,
    Return,
    Break,
    Continue,
    Failure,
  };

  struct ConstexprStatementEvaluation {
    ConstexprControlFlow control = ConstexprControlFlow::Next;
    std::optional<ConstantValue> value;
    ConstexprEvaluation failure;
  };

  static constexpr std::size_t constexprExpressionStepLimit = 4096;
  static constexpr std::size_t constexprCallDepthLimit = 64;

  [[nodiscard]] std::optional<ConstexprEvaluation> executeStagedConstexpr(
      const std::variant<const FunctionDecl *, ResolvedConstructionInfo>
          &target,
      const std::vector<ConstantValue> &arguments,
      const GenericSubstitution &substitution,
      const std::optional<ConstantValue> &receiver, const Token &callSite,
      ConstexprInterpreter &context);

  [[nodiscard]] ConstexprInterpreter makeConstexprInterpreter() const;

  [[nodiscard]] SemanticType
  constexprType(SemanticType type, const ConstexprInterpreter &context) const;

  [[nodiscard]] CompileTimeValue
  constexprValue(CompileTimeValue value,
                 const ConstexprInterpreter &context) const;

  [[nodiscard]] bool
  isSupportedConstexprType(const SemanticType &type,
                           bool allowDependent = false,
                           std::vector<SemanticType> *visiting = nullptr,
                           bool transient = false) const;

  bool validateConstexprFunction(const FunctionDecl &function,
                                 bool methodDeclaration,
                                 bool intrinsicDeclaration,
                                 const SemanticType &returnType,
                                 bool entryPoint);

  bool validateConstexprConstructor(const ConstructorDecl &constructor,
                                    const ConstructorInfo *info,
                                    const SemanticType &ownerType);

  [[nodiscard]] ConstexprEvaluation
  constexprSuccess(const Expr &expression, ConstantValue value,
                   ConstexprInterpreter &context);

  [[nodiscard]] static ConstexprEvaluation
  constexprFailure(const Token &location, ConstantEvaluationFailure failure,
                   std::string detail = {});

  [[nodiscard]] static ConstexprStatementEvaluation
  constexprStatementFailure(ConstexprEvaluation failure);

  [[nodiscard]] static const ConstexprLocalValue *
  findConstexprLocal(const ConstexprInterpreter &context, SymbolId symbol);

  [[nodiscard]] static ConstexprLocalValue *
  findMutableConstexprLocal(ConstexprInterpreter &context, SymbolId symbol);

  [[nodiscard]] static std::optional<TokenKind>
  constexprCompoundOperator(TokenKind operation);

  [[nodiscard]] ConstexprEvaluation
  evaluateConstexprMutation(const Expr &source, const ExprPtr &target,
                            const Token &operation, bool postfix,
                            ConstexprInterpreter &context);

  [[nodiscard]] ConstantEvaluation
  defaultConstexprValue(const SemanticType &type,
                        ConstexprInterpreter &context) const;

  void seedConstexprValueParameters(
      const std::vector<GenericParameterInfo> &parameters,
      const GenericSubstitution &substitution,
      std::unordered_map<SymbolId, ConstexprLocalValue> &scope);

  [[nodiscard]] ConstexprEvaluation evaluateConstexprConstruction(
      const Expr &source, const ResolvedConstructionInfo &resolved,
      const ExprList &explicitArguments, const Token &callSite,
      ConstexprInterpreter &context);

  [[nodiscard]] ConstexprEvaluation
  evaluateConstexprExpression(const ExprPtr &expression,
                              ConstexprInterpreter &context);

  [[nodiscard]] ConstexprEvaluation
  evaluateConstexprExpression(const Expr &source,
                              ConstexprInterpreter &context);

  [[nodiscard]] ConstexprEvaluation
  evaluateConstexprCall(const Expr &source, const ResolvedCallInfo &resolved,
                        const Expr *receiver,
                        const std::vector<const Expr *> &explicitArguments,
                        const Token &callSite, ConstexprInterpreter &context);

  [[nodiscard]] ConstexprStatementEvaluation
  executeConstexprStatements(const StmtList &statements,
                             ConstexprInterpreter &context);

  [[nodiscard]] ConstexprStatementEvaluation
  executeConstexprStatement(const Stmt *statement,
                            ConstexprInterpreter &context);

  [[nodiscard]] ConstexprEvaluation executeConstexprFunction(
      const FunctionDecl &function, const SemanticType &returnType,
      const std::vector<ConstantValue> &arguments,
      const GenericSubstitution &substitution,
      const std::optional<ConstantValue> &receiver, const Token &callSite,
      ConstexprInterpreter &context);

  [[nodiscard]] static SourceSpan
  staticAssertConditionSpan(const StaticAssertDecl &declaration);

  void appendStaticAssertSubstitutions(Diagnostic &diagnostic) const;

  void reportConstexprFailure(const VariableDecl &declaration,
                              const SemanticType &type,
                              const ConstexprEvaluation &evaluation);

  void reportConstexprConditionFailure(const IfStmt &statement,
                                       const ConstexprEvaluation &evaluation);

  [[nodiscard]] std::optional<ConstantValue>
  constexprBindingValue(const VariableDecl &declaration,
                        const SemanticType &type,
                        bool initializerSemanticallyValid);

  [[nodiscard]] std::optional<bool>
  constantBoolean(const ExprPtr &expression) const;

  [[nodiscard]] bool expressionNeverReturns(const Expr *expression) const;

  [[nodiscard]] FlowSummary
  summarizeFlow(const StmtList &statements,
                std::vector<const ReturnStmt *> *normalReturns = nullptr,
                bool structural = false) const;

  [[nodiscard]] FlowSummary
  summarizeFlow(const Stmt *statement,
                std::vector<const ReturnStmt *> *normalReturns = nullptr,
                bool structural = false) const;

  void recordCompleteFunctionReturnBorrow(const FunctionInfo &function);
  [[nodiscard]] bool conditionalReferenceReturn(
      const SemanticType &result, std::span<const SemanticType> parameters,
      const SemanticType &receiver = SemanticType::Unknown) const;
  // Normalize one already checked return expression before end-of-body
  // summary publication. Failure is unknown/invalid, never an empty proof.
  // The caller still enforces ordinary child-loan escape and availability.
  [[nodiscard]] std::optional<FunctionBorrowDependencySummary>
  normalizedCompleteReturnBorrow(const FunctionInfo &function,
                                 const SemanticBorrowDependencySet &complete);
  [[nodiscard]] std::optional<FunctionBorrowDependencySummary>
  normalizedCompleteBorrow(const FunctionInfo &function,
                           std::span<const Parameter> parameters,
                           ReceiverMutability receiverMutability,
                           const SemanticBorrowDependencySet &complete);
  void recordCompleteConstructorBorrow(const ConstructorDecl &constructor);
  std::optional<FunctionBorrowDependencySummary>
  completeConstructorBorrow(const ResolvedConstructionInfo &construction,
                            const SourceSpan &site);
  std::optional<FunctionBorrowDependencySummary> completeBorrowQuery(
      const InstanceAnalysisKey &key, const SemanticType &result,
      std::span<const SemanticType> parameters, const Token &name,
      const SourceSpan &site, const ConstructorDecl *constructor = nullptr);
  std::optional<FunctionBorrowDependencySummary> completeFunctionReturnBorrow(
      const CallableCandidate &function,
      const std::vector<SemanticType> &typeArguments,
      const std::vector<CompileTimeValue> &valueArguments,
      const SourceSpan &site);
  [[nodiscard]] std::optional<SemanticBorrowDependencySet>
  instantiateCompleteCallBorrow(const Call &call,
                                const ResolvedCallInfo &resolved,
                                const FunctionBorrowDependencySummary &summary,
                                bool *noNormalResult = nullptr);
  [[nodiscard]] std::optional<SemanticBorrowDependencySet>
  instantiateCompleteBorrowArguments(
      const ResolvedCallInfo &resolved,
      const FunctionBorrowDependencySummary &summary,
      std::span<const ExprPtr *const> arguments,
      const ExprPtr *receiver = nullptr, bool *noNormalResult = nullptr);

  // MIR currently materializes both runtime `if` successors even when the
  // condition is a literal. Loan endpoint selection must therefore use the
  // same structural reachability instead of the constant-aware return-flow
  // summary used for source diagnostics.
  [[nodiscard]] FlowSummary summarizeLoanFlow(const StmtList &statements) const;

  [[nodiscard]] FlowSummary summarizeLoanFlow(const Stmt *statement) const;

  void analyzeInferredVariable(const VariableDecl &declaration);

  bool reportMissingLambdaCapture(const Token &use);

  [[nodiscard]] SemanticTypeTraits typeTraits(const SemanticType &type) const;
  // A readonly iterator can yield an independent ordinary value instead of
  // lending an element. Shared by operator and structural range contracts.
  [[nodiscard]] bool readableIteratorValue(const SemanticType &type) const;

  struct ActiveCleanupCause {
    const Token *source = nullptr;
    std::string message;
  };

  enum class ActiveCleanupState : std::uint8_t {
    InProgress,
    Absent,
    Present,
  };

  struct ActiveCleanupEntry {
    SemanticType type = SemanticType::Unknown;
    ActiveCleanupState state = ActiveCleanupState::InProgress;
  };

  struct ActiveCleanupQuery {
    std::vector<ActiveCleanupEntry> entries;
    std::vector<SemanticType> activeTypes;
  };

  [[nodiscard]] static std::size_t
  semanticTypeComplexity(const SemanticType &type);

  [[nodiscard]] bool
  nonDecreasingNominalRecurrence(const SemanticType &type,
                                 std::span<const SemanticType> active) const;

  [[nodiscard]] SemanticTypeTraits
  lambdaTraitsForCaptures(std::span<const SemanticType> captureTypes) const;

  [[nodiscard]] bool requiresActiveCleanup(const SemanticType &type,
                                           ActiveCleanupQuery &query) const;

  [[nodiscard]] std::optional<ActiveCleanupCause>
  activeCleanupCause(const SemanticType &type) const;

  [[nodiscard]] ConcurrencyCapabilities typeCapabilities(
      const SemanticType &type, std::vector<SemanticType> &visiting,
      ThreadTransferBoundary boundary = ThreadTransferBoundary::Owned) const;

  [[nodiscard]] SemanticTypeTraits
  typeTraits(const SemanticType &type,
             std::vector<SemanticType> &visiting) const;

  [[nodiscard]] std::optional<ShareCapabilityDenial>
  shareCapabilityDenial(const SemanticType &type) const;

  [[nodiscard]] std::optional<TransferCapabilityDenial>
  transferCapabilityDenial(const SemanticType &type) const;

  bool reportOwnershipErasureCapabilityAmplification(
      const Token &site, std::string_view ownership,
      const SemanticType &sourcePointee, const SemanticType &targetPointee,
      const SemanticType &sourceOwner, const SemanticType &targetOwner);

  bool reportConcurrentGlobalPolicyFailure(const VariableDecl &declaration,
                                           const SemanticType &type,
                                           bool globalStorage);

  [[nodiscard]] ExpressionInfo
  expressionInfo(SemanticType type,
                 ValueCategory category = ValueCategory::Value,
                 AccessMode access = AccessMode::ReadOnly) const;

  [[nodiscard]] ExpressionInfo
  ownerDependentTextInfo(SemanticType type,
                         ValueCategory category = ValueCategory::Value,
                         AccessMode access = AccessMode::ReadOnly) const;

  [[nodiscard]] bool
  isOwnerDependentTextExpression(const ExprPtr &expression) const;

  // A value of a class or payload enum whose type has no borrowed state but
  // whose direct text field holds owner-dependent text (L-OWN-02). The
  // dependency belongs to the value: its traits are the type's, made borrowed
  // and non-assignable, as ownerDependentTextInfo does for a dynamic view.
  [[nodiscard]] ExpressionInfo
  textDependentValueInfo(SemanticType type,
                         ValueCategory category = ValueCategory::Value,
                         AccessMode access = AccessMode::ReadOnly) const;

  // Whether `info` describes a value whose class type has no borrowed state
  // but whose direct text field holds dynamic text.
  [[nodiscard]] bool isTextDependentValue(const ExpressionInfo &info) const;
  // Whether `expression` is a contextual construction of `type` that
  // publishes its argument's owner: a dependent value of that type, although
  // the expression's own facts describe the argument.
  [[nodiscard]] bool contextualTextValue(const Expr &expression,
                                         const SemanticType &type) const;
  // The field paths that hold dynamic text in a dependent value that
  // `expression` names: a carrier binding, a publishing construction, a
  // grouping or move of one, or a field of one that the paths go through.
  [[nodiscard]] std::vector<std::vector<SymbolId>>
  textDependentPaths(const Expr &expression) const;
  // Of those, the direct text fields. A payload reached through a field is
  // observed only by a switch on that field.
  [[nodiscard]] std::vector<SymbolId>
  textDependentFields(const Expr &expression) const;
  void noteTextDependentValue(const Expr &expression);
  void consumeTextDependentValue(const ExprPtr &expression);
  // The owner dependency of a switch subject when it designates exactly this
  // binding's text payload (L-OWN-02), through the subject's `prefix` of
  // fields of the dependency's carrier.
  [[nodiscard]] const SemanticLoanInfo *
  dependentTextPayloadLoan(SemanticLoanId subject,
                           const ResolvedPayloadPatternInfo &pattern,
                           const PayloadBindingInfo &payload,
                           std::span<const SymbolId> prefix = {}) const;
  void finalizeTextDependentValues();

  // Whether a construction of `type` may publish dynamic text into a direct
  // text field: a class without borrowed state, bases or a destructor, so no
  // code outside the creating body reads the field as ownerless text.
  [[nodiscard]] bool textPublicationOwner(const SemanticType &type) const;
  // A publishable class with a text path under it: a text field, a text
  // payload of a payload-enum field, or such a class in a field or payload.
  // A dependent value of it nests its paths (L-OWN-02 phase 3).
  [[nodiscard]] bool nestedTextClass(const SemanticType &type,
                                     std::size_t depth = 0) const;
  // Whether a payload or field of `type` holds text: it is text, or a
  // nested text class.
  [[nodiscard]] bool holdsText(const SemanticType &type) const;
  // Notes `expression` when it is a contract formal's reference relay to a
  // nested text class, or a field of one, of type `type`.
  void noteContractRelayUse(const Expr &expression, const SemanticType &type);
  // The contract prefix that `object`, a noted relay use, reads under.
  [[nodiscard]] std::optional<OwnerlessTextField>
  contractRelayPrefix(const Expr *object) const;
  // The field contract naming a dependent path of `type`: its text field,
  // or its payload-enum field's payload, text or a nested text class with
  // the rest of the path under it. A path through a class field has none.
  [[nodiscard]] std::optional<OwnerlessTextField>
  contractPrefix(const SemanticType &type, std::span<const SymbolId> path,
                 OwnerlessTextField formal) const;

  // L-OWN-02 phase 1b: field-level ownerless text contracts. A function
  // states which text fields of its formals and receiver it lets outlive the
  // call (OwnerlessTextField); a dependent value may be relayed into any
  // other field.
  //
  // The function being analyzed, when it can carry such a contract: a
  // non-generic, non-virtual GTI function or method body, outside lambdas,
  // constructors and destructors.
  [[nodiscard]] const FunctionInfo *textFieldContractScope() const;
  // In an instance analysis of a generic class's method, the concrete
  // function: its MIR derives requirements from the instance's field types,
  // which the open analysis cannot see.
  [[nodiscard]] const FunctionInfo *instanceFieldContractScope() const;
  // Whether a function can carry a field-level contract, by the same rule.
  // It reads declaration facts only, because looking the function up during
  // analysis would demand its body.
  [[nodiscard]] bool textFieldContractFunction(const FunctionDecl *declaration,
                                               const FunctionDecl *body,
                                               bool dynamicDispatch,
                                               ClassId ownerClass) const;
  // Whether formal `index` is read-only: an immutable by-value parameter or
  // a read-only reference. Only such a formal takes a relayed value.
  [[nodiscard]] bool
  readOnlyTextFieldFormal(const FunctionDecl &declaration,
                          std::span<const SemanticType> types,
                          std::size_t index) const;
  // The formal or receiver that `object` names inside a contract scope,
  // when its class can hold a dependent text value.
  [[nodiscard]] std::optional<OwnerlessTextField>
  textFieldContractFormal(const Expr &object) const;
  [[nodiscard]] std::optional<OwnerlessTextField>
  textFieldContractFormal(const Expr &object,
                          const FunctionInfo &function) const;
  // The formal or receiver text field that `expression` reads directly.
  [[nodiscard]] std::optional<OwnerlessTextField>
  ownerlessTextForwardingField(const ExprPtr &expression) const;
  // The receiver text field that an implicit member read names.
  [[nodiscard]] std::optional<OwnerlessTextField>
  implicitReceiverTextField(const Variable &variable) const;
  // A text call result that carries the dependency its argument or receiver
  // holds in one direct text field: the callee returns that field.
  struct FieldSourcedTextResult {
    // The argument or receiver; null for an implicit receiver call.
    const ExprPtr *origin = nullptr;
    bool receiver = false;
    SymbolId field = 0;
    // The field's text payload, for a payload-enum field.
    SymbolId payload = 0;
  };
  // The paths of a reference relay's dependent origin under the relayed
  // field or payload, without that prefix: what the returned reference to a
  // nested text class designates.
  [[nodiscard]] std::optional<std::vector<std::vector<SymbolId>>>
  nestedTextRelay(const FieldSourcedTextResult &relay) const;
  // The field paths, relative to a reference-returning callee's borrowed
  // receiver or argument, that its returns designate: each return is a field
  // path of that origin, or a receiver accessor's result. Nullopt when a
  // return designates anything else.
  [[nodiscard]] std::optional<std::vector<std::vector<SymbolId>>>
  referenceReturnPrefixes(const FunctionInfo &callee, BorrowOriginKind origin,
                          std::size_t parameter, std::size_t depth = 0) const;
  // A call returning a reference into its receiver or argument when that
  // origin holds dynamic text from a local owner (L-OWN-02): the reference is
  // the origin's dependent field at the designated prefixes.
  struct DependentReferenceResult {
    const ExprPtr *origin = nullptr;
    // Nullopt when the callee's returns do not name field paths of the origin.
    std::optional<std::vector<std::vector<SymbolId>>> prefixes;
  };
  [[nodiscard]] std::optional<DependentReferenceResult>
  dependentReferenceResult(const Call &call,
                           const ResolvedCallInfo &resolved) const;
  // The origin's dependent paths under the result's prefixes, without them.
  [[nodiscard]] std::vector<std::vector<SymbolId>>
  dependentReferencePaths(const DependentReferenceResult &result) const;
  [[nodiscard]] std::optional<FieldSourcedTextResult>
  fieldSourcedTextResult(const Call &call,
                         const ResolvedCallInfo &resolved) const;
  // A call whose callee returns a reference to one direct text field of its
  // receiver: reading the result reads that field of the receiver.
  [[nodiscard]] std::optional<FieldSourcedTextResult>
  textReferenceRelay(const Call &call, const ResolvedCallInfo &resolved) const;
  // The contract formal or receiver of the current scope that such a
  // result's origin names, with its field: the result reads that field.
  [[nodiscard]] std::optional<OwnerlessTextField>
  fieldSourcedTextFormal(const FieldSourcedTextResult &result) const;
  // The direct text fields of a class, as tooling symbols; `conditional`
  // adds open type-parameter fields, as directTextField does.
  [[nodiscard]] std::vector<SymbolId>
  directTextFieldSymbols(const SemanticType &type,
                         bool conditional = false) const;
  void noteTextFieldFormalUse(const Expr &expression,
                              OwnerlessTextField formal);
  // A returned reference that a call borrowed from a contract formal or the
  // receiver lets that whole formal escape: a whole use (L-OWN-02).
  // The contract formal (or read-only receiver) whose storage a call's
  // reference result borrows.
  [[nodiscard]] std::optional<OwnerlessTextField>
  callBorrowedTextFormal(const Call &call,
                         const ResolvedCallInfo &resolved) const;
  void noteReturnedFormalReference(const Expr &returned);
  // A read, in a generic class method, of a type-parameter field of the
  // read-only receiver or of a contract formal: the field may hold a relayed
  // owner's text in a text instance.
  [[nodiscard]] std::optional<OwnerlessTextField>
  conditionalTextField(const Expr &expression) const;
  // The returns of `function` that each yield the same type-parameter field
  // of the receiver, which the method relays; empty when it relays none.
  [[nodiscard]] const std::unordered_set<const Expr *> &
  conditionalRelayReturns(const FunctionInfo &function);
  // Records the requirement a conditional field read implies (L-OWN-02).
  void noteConditionalTextFieldRead(const Expr &expression);
  void consumeTextFieldFormalUse(const ExprPtr &expression);
  void recordTextFieldCall(const Call &call, const CallableCandidate &target);
  // The relays and forwards of a call's explicit receiver (or an implicit
  // receiver call) and arguments.
  void recordTextFieldUses(const ExprPtr *receiver, bool implicitReceiver,
                           std::span<const ExprPtr> arguments,
                           const CallableCandidate &target);
  bool requireOwnerlessTextField(const FunctionDecl *function,
                                 OwnerlessTextField field);
  void finalizeTextFieldContracts();
  // Whether a construction through `constructor` (null for a designated
  // list) keeps a published text field from every other reader. Its body
  // must be empty, and no other initializer or field default may read the
  // object under construction; such code would read the field as ownerless
  // text.
  [[nodiscard]] bool
  constructionKeepsPublishedText(const SemanticType &type,
                                 const ConstructorDecl *constructor) const;
  // Whether `expression` may read the object under construction: `this`, or
  // a name that is not one of `formals` and names a non-static member.
  // Expression forms this does not inspect count as reads.
  [[nodiscard]] bool
  readsConstructedObject(const Expr &expression, const ClassInfo &owner,
                         const std::vector<Parameter> *formals) const;

  // Whether `field` names a direct text field of the class `type`, or with
  // `conditional` an open type-parameter field an instance may make text.
  [[nodiscard]] bool directTextField(const SemanticType &type, SymbolId field,
                                     bool conditional = false) const;
  // Whether `payload` names a text payload of the concrete payload enum
  // `type`.
  [[nodiscard]] bool directTextPayload(const SemanticType &type,
                                       SymbolId payload) const;
  // A result that returns one text field of the receiver (L-OWN-02): the
  // text that field holds, never storage of the receiver itself, so even a
  // consuming method may return it.
  [[nodiscard]] static bool
  receiverTextFieldResult(const SemanticType &result, BorrowOriginKind origin,
                          const std::vector<BorrowedFieldOrigin> &fields) {
    return (result.kind == SemanticType::StringView ||
            result.kind == SemanticType::CString) &&
           origin == BorrowOriginKind::Receiver && fields.size() == 1 &&
           fields.front().sourceFields.size() == 1 &&
           fields.front().resultFields.empty();
  }
  // A receiver whose text fields carry field contracts (L-OWN-02): a
  // read-only one, or a consuming one, which owns the caller's value and can
  // only write ownerless text into a text field.
  [[nodiscard]] static constexpr bool
  contractReceiver(ReceiverMutability mutability) {
    return mutability == ReceiverMutability::ReadOnly ||
           mutability == ReceiverMutability::Consuming;
  }
  // Whether `function`'s formal `index` has a type parameter, or a reference
  // to one, as its declared type.
  [[nodiscard]] static bool typeParameterFormal(const FunctionInfo &function,
                                                std::size_t index);
  // Whether `entry`'s field, or its payload through that field, is text in
  // the class `type`. Its formal is not consulted.
  [[nodiscard]] bool contractTextPath(const SemanticType &type,
                                      const OwnerlessTextField &entry) const;
  // The text payloads of the class `type`'s payload-enum fields, as
  // requirement entries for `formal`.
  [[nodiscard]] std::vector<OwnerlessTextField>
  textPayloadFields(const SemanticType &type, OwnerlessTextField formal) const;
  // In a field-contract scope, the requirement that a switch binding a text
  // payload of a contract formal's or the read-only receiver's payload-enum
  // field `subject` places on that payload; a type-parameter payload of a
  // generic class binds only its text instances.
  [[nodiscard]] std::optional<OwnerlessTextField>
  contractPayloadBinding(const ExprPtr &subject,
                         const ResolvedPayloadPatternInfo &pattern,
                         const PayloadBindingInfo &payload) const;
  // In the field-contract scope `scope`, the contract formal's or receiver's
  // fields that `expression` names, with each field's type there: one for
  // `formal.field`, each field an accessor call's returns designate.
  [[nodiscard]] std::vector<std::pair<OwnerlessTextField, SemanticType>>
  contractEnumFields(const Expr *expression, const FunctionInfo &scope) const;
  // The one field `expression` names.
  [[nodiscard]] std::optional<std::pair<OwnerlessTextField, SemanticType>>
  contractEnumField(const Expr *expression, const FunctionInfo &scope) const;
  // A contract formal's or the receiver's payload-enum field read as a value,
  // rather than observed in place as a switch subject, may let any of its
  // text payloads outlive the call: each is required ownerless (L-OWN-02).
  void notePayloadFieldRead(const Expr &expression);

  // The sole named owner of a dynamic text argument, designating `field`.
  // Empty when the argument is ownerless or its owner is not a named place.
  [[nodiscard]] std::vector<SemanticLoanDependency>
  textFieldOwnerDependency(const ExprPtr &argument, SymbolId field);
  // Re-roots an argument's sole owner dependency at a published text path.
  void publishTextDependency(SemanticLoanDependency &dependency,
                             std::vector<SymbolId> path) const;

  // The owner dependencies that `argument` contributes when a constructor
  // stores it directly in the text field `initializer` names, or in that
  // field's text `payload`, re-rooted at that path. Empty when the argument
  // is ownerless or has no nameable owner.
  [[nodiscard]] std::vector<SemanticLoanDependency>
  textFieldInitializerDependencies(const SemanticType &owner,
                                   const ConstructorInitializer &initializer,
                                   const ExprPtr &argument,
                                   SymbolId payload = 0);

  void reportOwnerDependentTextStorage(const ExprPtr &expression,
                                       std::string_view destination);

  [[nodiscard]] std::optional<std::size_t>
  ownerlessTextForwardingParameter(const ExprPtr &expression) const;
  [[nodiscard]] bool requireOwnerlessTextParameter(const ExprPtr &expression);
  // Whether `result`, this function's returned value of type `type`, relays
  // the formal `argument` names at its text field `field` (L-OWN-02),
  // recording it in publishedTextFields instead of an ownerless requirement.
  // `constructor` is the selected constructor of a returned construction.
  [[nodiscard]] bool relayReturnedTextFormal(const Expr &result,
                                             const SemanticType &type,
                                             const ConstructorDecl *constructor,
                                             const ExprPtr &argument,
                                             SymbolId field);
  // A callee that returns an argument in a text field of its fresh result
  // makes `call` publish that argument's owner there, as a constructor does.
  void publishTextCallResult(const Call &call, ResolvedCallInfo &resolved,
                             const FunctionInfo &callee);
  // A returned dependent value whose sole owner is the referent of a
  // read-only reference formal makes the function relay that referent's
  // text at the value's path (FunctionInfo::dependentTextReturn).
  void noteDependentTextReturn(const ExprPtr &value);
  // A call of such a function: its result views the borrowed argument's
  // text at that path.
  void publishTextReturnResult(const Call &call, ResolvedCallInfo &resolved,
                               const FunctionInfo &callee);
  void recordOwnerlessTextCall(const Call &call,
                               const CallableCandidate &target);
  // An overloaded operator's receiver and operands relay or forward like a
  // call's (L-OWN-02).
  void recordTextFieldOperator(const ExprPtr &receiver,
                               std::span<const ExprPtr> arguments,
                               const CallableCandidate &target);
  // The receiver field or payload that a dereference operator's returned
  // reference designates, as a call's reference relay does.
  [[nodiscard]] std::optional<FieldSourcedTextResult>
  operatorTextReferenceRelay(const Unary &unary) const;
  void finalizeOwnerlessTextContracts();

  // A by-value text formal that a constructor initializer stores in its
  // field, directly or as one text payload of the field's payload-enum
  // construction.
  struct ConstructorTextInput {
    std::size_t parameter = 0;
    const ConstructorInitializer *initializer = nullptr;
    // The payload the formal fills, or 0 when the field itself is text.
    SymbolId payload = 0;
    // The payload construction's argument naming the formal.
    const Expr *payloadArgument = nullptr;
    // For a class formal, the text field of it that the initializer stores,
    // read directly or through an accessor relaying it, or a field of a
    // publishable class type holding text paths.
    SymbolId sourceField = 0;
    // A class formal the initializer stores whole, in a field or payload of
    // its own type: every text path of the argument nests under it.
    bool wholeValue = false;
  };
  [[nodiscard]] std::vector<ConstructorTextInput>
  ownerlessTextConstructorInputs(
      const SemanticType &type, const ConstructorDecl *constructor,
      std::span<const SemanticType> parameterTypes) const;
  // At a construction, a dependent `argument` designating the class formal
  // `input`'s source field publishes its owner at the field path that the
  // constructor stores it in; any other argument keeps the existing rules.
  void relayConstructorTextField(const SemanticType &constructedType,
                                 const ConstructorDecl &constructor,
                                 const ConstructorTextInput &input,
                                 const ExprPtr &argument,
                                 std::vector<BorrowedFieldOrigin> &textOrigins,
                                 bool contextual);

  [[nodiscard]] BindingInfo
  bindingInfo(SemanticType type,
              AccessMode access = AccessMode::ReadOnly) const;

  [[nodiscard]] bool isMoveOnlyOwnerType(const SemanticType &type) const;

  // Declaration identity of one environment/record projection. Captures and
  // ordinary fields participate in the same dependency paths; neither needs
  // a separate lifetime checker or a fabricated source field declaration.
  struct StoredField {
    StoredField(const VariableDecl *field) : declaration(field) {}
    StoredField(const LambdaCapture *field) : capture(field) {}
    StoredField(const Parameter *field) : payload(field) {}

    [[nodiscard]] const Token &name() const {
      return declaration != nullptr ? declaration->name()
             : capture != nullptr   ? capture->name
                                    : payload->name;
    }
    bool operator==(const StoredField &) const = default;

    const VariableDecl *declaration = nullptr;
    const LambdaCapture *capture = nullptr;
    // The parameter declaration belongs to one exact enum alternative. Its
    // stable symbol is also the alternative-qualified dependency identity;
    // it is not an ordinary class member or an unconditional field.
    const Parameter *payload = nullptr;
  };

  struct StoredReferenceCondition {
    // Path from the carrier to the enum subobject whose alternative guards
    // this dependency. Nested alternatives form a conjunction, not a flat tag.
    std::vector<StoredField> subobject;
    SemanticType enumeration = SemanticType::Unknown;
    std::size_t variant = 0;
  };

  struct StoredReferencePath {
    std::vector<StoredField> fields;
    SemanticType reference = SemanticType::Unknown;
    std::vector<StoredReferenceCondition> conditions;
  };

  struct StoredReferenceBinding {
    StoredReferencePath destination;
    std::size_t parameter = 0;
    // A nominal parameter contributes the same path below the initialized
    // field. A direct reference parameter contributes its referent itself.
    std::vector<StoredField> sourceFields;
  };

  [[nodiscard]] std::vector<StoredReferenceBinding>
  storedCarrierConstructorBindings(
      const SemanticType &type, const ConstructorDecl *constructor,
      std::span<const SemanticType> parameterTypes) const;

  [[nodiscard]] std::vector<SymbolId>
  storedFieldSymbols(SemanticType type, std::span<const StoredField> fields);

  [[nodiscard]] std::optional<std::vector<BorrowDependencyCondition>>
  storedConditionSymbols(const SemanticType &type,
                         std::span<const StoredReferenceCondition> conditions);

  // A present inventory enumerates every independently checked dependency in
  // declaration order. Present-empty is dependency-free; absent is unsupported
  // or unresolved and must never be used to establish empty-set evidence.
  [[nodiscard]] std::optional<std::vector<StoredReferencePath>>
  storedReferenceInventory(const SemanticType &type) const;

  [[nodiscard]] std::optional<std::vector<BorrowAlternativePath>>
  storedBorrowAlternativeInventory(const SemanticType &type);

  enum class StoredReferenceQuery { SymbolicPaths, CompleteInventory };
  [[nodiscard]] std::optional<std::vector<StoredReferencePath>>
  collectStoredReferencePaths(const SemanticType &type,
                              StoredReferenceQuery query) const;

  // Compatibility query for paths that require a nonempty inventory. New
  // complete-set producers must use storedReferenceInventory instead.
  [[nodiscard]] std::vector<StoredReferencePath>
  storedReferencePaths(const SemanticType &type) const;

  [[nodiscard]] std::optional<SemanticBorrowDependencySet>
  completeBorrowDependencySet(const ExprPtr &expression) const;

  [[nodiscard]] std::optional<SemanticBorrowDependencySet>
  payloadBorrowDependencySet(const Call &call,
                             const EnumeratorRecord &enumerator);
  [[nodiscard]] std::optional<SemanticBorrowDependencySet>
  selectedPayloadReferenceDependencies(
      const ResolvedPayloadPatternInfo &pattern,
      const PayloadBindingInfo &binding,
      const SemanticBorrowDependencySet &source);
  [[nodiscard]] std::optional<SemanticBorrowDependencySet>
  joinedBorrowDependencySets(
      const std::optional<SemanticBorrowDependencySet> &left,
      const std::optional<SemanticBorrowDependencySet> &right) const;
  void invalidateBorrowDependencySet(const SemanticPlace &place);

  // A concrete carrier may wrap its one direct reference in ordinary nominal
  // fields. The complete declaration path is the dependency identity; an
  // aggregate with two paths is not a one-owner carrier.
  [[nodiscard]] std::optional<StoredReferencePath>
  storedReferencePath(const SemanticType &type) const;

  [[nodiscard]] std::optional<std::size_t> storedCarrierConstructorParameter(
      const SemanticType &type, const ConstructorInfo &constructor,
      std::span<const SemanticType> parameterTypes) const;

  [[nodiscard]] std::vector<std::size_t> storedCarrierConstructorParameters(
      const SemanticType &type, const ConstructorInfo &constructor,
      std::span<const SemanticType> parameterTypes) const;

  [[nodiscard]] bool
  isDirectStoredReferenceType(const SemanticType &type) const;

  [[nodiscard]] std::optional<SemanticType>
  storedReferenceReferent(const SemanticType &type) const;

  [[nodiscard]] bool isBorrowReturningType(const SemanticType &type) const;
  [[nodiscard]] bool isConditionalBorrowResult(const SemanticType &type) const;

  [[nodiscard]] FunctionReturnBorrowSummary functionReturnBorrowSummary(
      const SemanticType &returnType,
      const std::vector<SemanticType> &parameterTypes, ClassId ownerClass,
      bool staticMember, IntrinsicKind intrinsic,
      std::optional<std::size_t> exactReturnParameter = std::nullopt) const;

  void applyFunctionReturnBorrowSummary(CallableCandidate &function) const;

  [[nodiscard]] std::optional<FunctionReturnBorrowSummary>
  storedFieldReturnBorrowSummary(
      const FunctionInfo &function,
      SemanticType ownerType = SemanticType::Unknown);

  // An explicit borrowed_from contract on a generic-result declaration binds
  // the instances whose substituted result is a checked reference: the named
  // parameter, or the one eligible element of the named final pack, owns it.
  void resolveGenericReturnBorrowContract(const FunctionInfo &declaration,
                                          FunctionInfo &instance);
  struct GenericReturnBorrowContract {
    BorrowOriginKind origin = BorrowOriginKind::None;
    std::size_t parameter = 0;
    AccessMode access = AccessMode::ReadOnly;
    bool ambiguous = false;
    bool missing = false;
  };
  // The contract an explicit borrowed_from selects for one substitution of a
  // generic-result declaration: None when the substituted result is not a
  // checked reference or conditional reference-bearing result, otherwise the
  // named parameter or the one eligible element of the named final pack, by
  // expanded parameter position.
  [[nodiscard]] GenericReturnBorrowContract
  genericReturnBorrowContract(const FunctionDecl &syntax,
                              const SemanticType &returnType,
                              std::span<const SemanticType> parameterTypes,
                              ClassId ownerClass, bool staticMember) const;
  void reportMissingLambdaReturnBorrowSummary(
      const Lambda &lambda, const FunctionReturnBorrowSummary &summary);
  // Whether the selected callable of this call (a lambda value or function
  // item) publishes a result-origin contract its reference result relays.
  [[nodiscard]] bool callableResultOriginContract(const Call &call) const;
  void reportMissingFunctionReturnBorrowSummary(const FunctionDecl &function,
                                                const FunctionInfo &info);

  [[nodiscard]] AccessMode borrowAccess(const SemanticType &type) const;

  [[nodiscard]] bool nestsBorrowedState(const SemanticType &type) const;

  [[nodiscard]] bool
  composedCarrierPaths(std::span<const StoredReferencePath> paths) const;
  [[nodiscard]] bool isBorrowedCarrierReference(const SemanticType &type) const;

  [[nodiscard]] static bool isDirectOwnerType(const SemanticType &type);

  [[nodiscard]] bool runtimeObjectLayoutType(const SemanticType &type) const;

  [[nodiscard]] IntrinsicKind
  trustedIntrinsicKind(SourceUnitId sourceUnit,
                       const std::vector<std::string> &scope,
                       std::string_view name) const;

  [[nodiscard]] SynchronizationOperation
  trustedSynchronizationOperation(SourceUnitId sourceUnit,
                                  const std::vector<std::string> &scope,
                                  std::string_view name) const;

  [[nodiscard]] std::optional<CallableCandidate>
  intrinsicCandidate(const ExprPtr &callee) const;

  [[nodiscard]] IntrinsicKind resolvedIntrinsicKind(const Call &call) const;

  [[nodiscard]] static const Variable *movedVariable(const ExprPtr &expression);

  // GTI-S2003 for a return whose value would be copied although its type is
  // move-only: a named local or parameter is told to move explicitly.
  void reportMoveOnlyReturnCopy(const ExprPtr &value,
                                const SemanticType &valueType);

  enum class ImplicitReturnMoveStatus {
    NotEligible,
    Moved,
    Invalid,
  };

  [[nodiscard]] ImplicitReturnMoveStatus
  analyzeImplicitReturnMove(const ExprPtr &expression,
                            const SemanticType &returnType,
                            const SemanticType &valueType);

  [[nodiscard]] const ExprPtr *
  operatorBorrowSource(const ExprPtr &expression,
                       const ResolvedOperatorInfo &resolved) const;

  [[nodiscard]] const ExprPtr *
  storedBorrowSource(const ExprPtr &expression) const;

  [[nodiscard]] const Symbol *
  resolvedBorrowVariable(const Variable &variable) const;

  [[nodiscard]] std::vector<SemanticLoanDependency>
  storedBorrowDependencies(const ExprPtr &expression);
  [[nodiscard]] std::vector<SemanticLoanDependency>
  storedBorrowDependencies(const AnalyzedCallArgument &argument);

  [[nodiscard]] const Variable *
  borrowedOwnerVariable(const ExprPtr &expression) const;

  [[nodiscard]] const Variable *
  borrowedOwnerVariable(const ExprPtr &expression,
                        std::unordered_set<const Expr *> &visiting) const;

  [[nodiscard]] const Variable *
  directStorageVariable(const ExprPtr &expression) const;

  [[nodiscard]] bool hasRetainedBorrow(const Variable &variable) const;

  [[nodiscard]] std::optional<RetainedLoanSource>
  retainedLoanSource(const ExprPtr &expression) const;

  [[nodiscard]] std::optional<SemanticLoanDependency>
  projectedStoredDependency(const SemanticLoanInfo &loan,
                            std::span<const PlaceProjection> projections) const;

  static void appendUniqueLoan(std::vector<SemanticLoanId> &loans,
                               SemanticLoanId loan);

  [[nodiscard]] SemanticLoanPlace loanPlace(SemanticPlace place);

  [[nodiscard]] SemanticLoanPlace
  loanPlace(const BorrowOriginPlace &place) const;

  [[nodiscard]] SemanticLoanPlace loanPlace(const ExprPtr &expression);

  // A selected stored reference has one runtime address but may retain
  // several possible external owners after a control-flow join. Snapshot
  // those origins at access evaluation; never choose its first dependency.
  [[nodiscard]] std::optional<std::vector<SemanticLoanDependency>>
  selectedReferenceDependencies(const SemanticPlace &place) const;
  // Prove an external referent through exact checked entry paths and parent
  // edges; a non-storage-protecting dependency is not itself lifetime proof.
  [[nodiscard]] bool
  hasCheckedExternalBorrowOrigin(const SemanticLoanDependency &dependency);

  [[nodiscard]] std::vector<SemanticLoanPlace>
  loanPlaces(const SemanticPlace &place);

  [[nodiscard]] std::vector<SemanticLoanPlace>
  loanPlaces(const ExprPtr &expression, bool transient = false);

  [[nodiscard]] static bool loanPlacesOverlap(const SemanticLoanPlace &left,
                                              const SemanticLoanPlace &right);

  [[nodiscard]] bool loanIsAncestor(SemanticLoanId ancestor,
                                    SemanticLoanId descendant) const;

  [[nodiscard]] SemanticLoanId accessLoan(const SemanticPlace &place) const;

  [[nodiscard]] SemanticLoanId accessLoan(const ExprPtr &expression) const;

  [[nodiscard]] std::vector<SemanticLoanId>
  conflictingLoans(const SemanticLoanPlace &place, AccessMode requested,
                   SemanticLoanId throughLoan = 0) const;

  void reportLoanPlaceConflict(const Token &token,
                               const SemanticLoanPlace &stable,
                               AccessMode requested, std::string message,
                               SemanticLoanId throughLoan = 0);

  void reportLoanPlaceConflicts(const Token &token,
                                std::span<const SemanticLoanPlace> places,
                                AccessMode requested, std::string message,
                                SemanticLoanId throughLoan = 0);

  void reportLoanPlaceConflict(const Token &token, const SemanticPlace &place,
                               AccessMode requested, std::string message,
                               SemanticLoanId throughLoan = 0);

  void reportRawLoanConflict(const Token &token, const SemanticLoanPlace &place,
                             AccessMode access, SemanticLoanId throughLoan);

  void validateLoanPlaceAccess(const Expr &expression, OccurrenceRole roles);

  void
  validateMutableCallArgumentLoans(std::span<const SemanticType> parameterTypes,
                                   std::span<const ExprPtr> arguments,
                                   const Token &location);

  struct TransientBorrowProduction {
    SemanticLoanPlace place;
    AccessMode access = AccessMode::ReadOnly;
    bool globalStorage = false;
    bool retainsValueCarrier = false;
  };

  [[nodiscard]] std::optional<TransientBorrowProduction>
  transientBorrowProduction(const ExprPtr &expression);

  [[nodiscard]] SemanticLoanPlace transientLoanPlace(const ExprPtr &expression);

  [[nodiscard]] SemanticLoanPlace retainedLoanPlace(const ExprPtr &expression);
  [[nodiscard]] bool receiverStorageBorrow(const ExprPtr &expression) const;

  static const ExprPtr *operatorReceiverExpression(const ExprPtr &expression);

  static void appendUniquePlace(std::vector<SemanticLoanPlace> &places,
                                SemanticLoanPlace place);

  void appendLoanPlaces(std::vector<SemanticLoanPlace> &places,
                        const SemanticPlace &place);

  void appendLoanPlaces(std::vector<SemanticLoanPlace> &places,
                        const ExprPtr &expression, bool transient = false);

  void appendTransientMutationPlaces(const ExprPtr &expression,
                                     std::vector<SemanticLoanPlace> &places);

  void appendTransientReadPlaces(
      const ExprPtr &expression, std::vector<SemanticLoanPlace> &places,
      std::vector<TransientBorrowProduction> &productions);

  [[nodiscard]] bool reportCommaTransientConflict(const ExprPtr &expression,
                                                  const Token &location);

  void validateSequencedTransientLoans(const ExprPtr &left,
                                       const ExprPtr &right,
                                       const Token &location);

  [[nodiscard]] bool validateForeignStorageBorrow(const ExprPtr &expression);
  // A checked reference to a raw pointer borrows a GTI pointer slot. An
  // element of a fixed pointer array or a field of a native record or union
  // holds a native pointer object the reference cannot represent.
  [[nodiscard]] bool
  validateNativePointerSlotBorrow(const ExprPtr &expression,
                                  const SemanticType &reference);

  void validateCallPlaceExclusivityImpl(
      std::span<const SemanticType> parameterTypes,
      std::span<const ExprPtr *> arguments, const Token &location,
      std::optional<SemanticLoanPlace> receiver = std::nullopt,
      AccessMode receiverAccess = AccessMode::ReadOnly,
      bool transientReceiver = false,
      std::span<const SemanticLoanPlace> receiverOrigins = {});

  void validateCallPlaceExclusivity(
      std::span<const SemanticType> parameterTypes,
      std::span<const ExprPtr> arguments, const Token &location,
      std::optional<SemanticLoanPlace> receiver = std::nullopt,
      AccessMode receiverAccess = AccessMode::ReadOnly,
      bool transientReceiver = false,
      std::span<const SemanticLoanPlace> receiverOrigins = {});

  void noteUnsupportedNestedLoanControlFlow();

  void recordLoanFlowBreak(const LoopControlStmt &statement);

  void beginLoanFlowLoop(const Stmt &statement);

  void endLoanFlowLoop();

  void attachLoanCarrier(SemanticLoanId loan, Symbol &carrier,
                         SymbolId carrierSymbol, bool transfer);

  void registerStorageLoan(SymbolId owner, bool receiver, SemanticLoanId loan);

  SemanticLoanId createRetainedLoan(
      Symbol &carrier, SymbolId carrierSymbol, SemanticLoanKind kind,
      const Expr *origin, SemanticLoanPlace place, SymbolId owner,
      bool receiverOrigin, AccessMode access, bool protectsStorage,
      SemanticLoanId parent = 0, bool entry = false,
      std::vector<SymbolId> designatedFields = {},
      std::vector<BorrowDependencyCondition> conditions = {});

  void recordParameterEntryLoan(const Parameter &parameter);
  SemanticLoanId recordStoredEntryDependency(Symbol &carrier,
                                             SymbolId carrierSymbol,
                                             const SemanticType &type,
                                             bool receiver,
                                             const Stmt *declaration = nullptr);

  void recordRetainedBorrow(const VariableDecl &binding,
                            const ExprPtr &expression);

  void recordSemanticLoanUseForLoan(SemanticLoanId loan);

  // A use of `loan` and of every authority it depends on: parents, stored
  // dependencies and additional-dependency parents.
  void recordSemanticLoanAuthorityUse(SemanticLoanId loan);

  void recordSemanticLoanUse(SymbolId carrier);

  [[nodiscard]] bool supportsProvenLoanEnd(const LoanFlowRecord &loan) const;

  [[nodiscard]] bool canEndLoanEarly(const LoanFlowRecord &loan) const;

  [[nodiscard]] static bool
  supportsConditionalArmLoanEnd(const Stmt *statement);

  [[nodiscard]] static bool
  pathStartsWith(const std::vector<ActiveLoanFlowConditionalArm> &path,
                 const std::vector<ActiveLoanFlowConditionalArm> &prefix);

  [[nodiscard]] static bool
  pathStartsWith(const std::vector<ActiveLoanFlowSwitchArm> &path,
                 const std::vector<ActiveLoanFlowSwitchArm> &prefix);

  [[nodiscard]] static bool
  sameStructuredPath(const std::vector<ActiveLoanFlowConditionalArm> &left,
                     const std::vector<ActiveLoanFlowSwitchArm> &leftSwitch,
                     const std::vector<ActiveLoanFlowConditionalArm> &right,
                     const std::vector<ActiveLoanFlowSwitchArm> &rightSwitch);

  [[nodiscard]] static bool structuredPathsMayOverlap(
      const std::vector<ActiveLoanFlowConditionalArm> &left,
      const std::vector<ActiveLoanFlowSwitchArm> &leftSwitch,
      const std::vector<ActiveLoanFlowConditionalArm> &right,
      const std::vector<ActiveLoanFlowSwitchArm> &rightSwitch);

  [[nodiscard]] bool pathContainsUnsupportedControlFlow(
      const std::vector<ActiveLoanFlowConditionalArm> &path,
      const std::vector<ActiveLoanFlowSwitchArm> &switchPath) const;

  [[nodiscard]] std::optional<LoanFlowBreakEndPlan>
  pathLocalBreakEndPlan(SemanticLoanId id, const LoanFlowRecord &loan,
                        std::size_t conflictIndex) const;

  [[nodiscard]] bool conditionalHasConflict(std::size_t conditionalIndex,
                                            SemanticLoanId loan) const;

  [[nodiscard]] bool armHasConflict(const LoanFlowConditionalArm &arm,
                                    SemanticLoanId loan) const;

  // When a later use keeps the fallthrough path live, an earlier nested
  // conditional may still end the loan on a sibling path that terminates.
  // Collect only those terminating-path endpoints; reachable fallthrough paths
  // deliberately retain the loan for their continuation.
  [[nodiscard]] std::optional<std::vector<LoanFlowArmEndpoint>>
  provenTerminatingPathEndpoints(std::size_t conditionalIndex, bool thenBranch,
                                 SemanticLoanId loan) const;

  // An arm endpoint must run on every fallthrough path that reaches the
  // enclosing join. A nested if can therefore either end the loan at its own
  // merge, or recursively end it on both of its arms when a conflict occurs
  // before that merge. Loops and switch edges remain conservative.
  [[nodiscard]] std::optional<std::vector<LoanFlowArmEndpoint>>
  provenConditionalArmEndpoints(std::size_t conditionalIndex, bool thenBranch,
                                SemanticLoanId loan) const;

  [[nodiscard]] static bool
  endpointPrecedesConflict(const LoanFlowArmEndpoint &endpoint,
                           const PendingBorrowConflict &conflict);

  [[nodiscard]] bool
  planHasResolvedConflict(const LoanFlowConditionalEndPlan &plan,
                          SemanticLoanId loan) const;

  [[nodiscard]] std::optional<LoanFlowConditionalEndPlan>
  conditionalEndPlan(SemanticLoanId id, const LoanFlowRecord &loan) const;

  [[nodiscard]] std::optional<LoanFlowSwitchEndPlan>
  switchEndPlan(SemanticLoanId id, const LoanFlowRecord &loan) const;

  [[nodiscard]] bool
  conflictFollowsConditionalEndpoint(const LoanFlowConditionalEndPlan &plan,
                                     std::size_t conflictIndex,
                                     SemanticLoanId loan) const;

  [[nodiscard]] bool
  conflictIsInConditionalArm(const LoanFlowConditionalEndPlan &plan,
                             std::size_t conflictIndex) const;

  void reportBorrowConflict(const Token &token,
                            const std::vector<SemanticLoanId> &loans,
                            std::string message, bool rawAudit = false);

  void recordProvenLoanEndpoint(SemanticLoanId id,
                                const LoanFlowArmEndpoint &endpoint);

  void finalizeLoanFlow();

  void removeLoanCarrier(Symbol &carrier);

  [[nodiscard]] bool isThreadHandleType(const SemanticType &type) const;

  [[nodiscard]] std::optional<SemanticType>
  threadHandleResultType(const SemanticType &type) const;

  [[nodiscard]] bool isTransferableThreadResult(const SemanticType &type) const;

  [[nodiscard]] std::optional<ResolvedThreadTaskEntry>
  exactThreadTaskEntry(const SemanticType &type,
                       const SemanticType &resultType);

  [[nodiscard]] bool isExactThreadTaskType(const SemanticType &type,
                                           const SemanticType &resultType);

  void analyzeThreadIntrinsicCall(const Call &expr,
                                  const CallableCandidate &declaration);

  void analyzeThreadScopeCall(const Call &expr);

  void analyzeAtomicIntrinsicCall(const Call &expr,
                                  const CallableCandidate &declaration);

  void analyzeMutexIntrinsicCall(const Call &expr,
                                 const CallableCandidate &declaration);

  void analyzeSourceLocationIntrinsicCall(const Call &expr,
                                          const CallableCandidate &declaration);

  void analyzeIntrinsicCall(const Call &expr,
                            const CallableCandidate &declaration);
  void analyzeMoveCall(const Call &expr,
                       const CallableCandidate *declaration = nullptr,
                       bool immediateReceiver = false);
  std::unordered_set<const Call *> designatedMoves;

  void
  analyzeIntegerArithmeticIntrinsicCall(const Call &expr,
                                        const CallableCandidate &declaration);

  void analyzeUniqueOwnerIntrinsicCall(const Call &expr,
                                       IntrinsicKind intrinsic);
  void analyzeRestoringIntrinsicCall(const Call &expr, IntrinsicKind intrinsic);
  void analyzeSharedOwnerIntrinsicCall(const Call &expr,
                                       const CallableCandidate &declaration);

  void analyzePolymorphicStorageIntrinsicCall(const Call &expr,
                                              IntrinsicKind intrinsic);
  void analyzeReferenceErasureCall(const Call &expr, IntrinsicKind intrinsic);
  void analyzeFunctionPointerCall(const Call &expr, IntrinsicKind intrinsic);
  [[nodiscard]] bool
  inlinePolymorphicPayload(const SemanticType &model,
                           const SemanticType &interface) const;
  void analyzeOptionalStorageIntrinsicCall(const Call &expr,
                                           IntrinsicKind intrinsic);

  // The identity-bound public logical-size check (P-STORAGE-01): two
  // uint64_t scalars — the index and the logical size — with a void
  // result; the only effect is the defined index_out_of_bounds failure.
  void analyzeStorageBoundsCheckCall(const Call &expr);

  [[nodiscard]] static bool isStorageElementType(const SemanticType &type);
  [[nodiscard]] bool isAllocatedScalarType(const SemanticType &type) const;

  void requireMutableStorage(const ExprPtr &argument,
                             std::string_view operation);

  void validateStorageValueConstruction(
      const Call &expr, const SemanticType &elementType,
      const std::vector<AnalyzedCallArgument> &arguments);

  [[nodiscard]] static std::string
  uniqueOwnerIntrinsicName(IntrinsicKind intrinsic);

  [[nodiscard]] std::optional<ResolvedTypeAlias>
  typeAliasForCallee(const ExprPtr &callee) const;

  [[nodiscard]] static const Call *directCall(const ExprPtr &expression);

  [[nodiscard]] const ExprPtr *
  explicitMoveSource(const ExprPtr &expression) const;

  [[nodiscard]] bool isExplicitMoveReceiver(const ExprPtr &expression) const;

  [[nodiscard]] const Variable *
  directCallableParameterVariable(const ExprPtr &expression) const;

  [[nodiscard]] const FunctionInfo *currentFunctionInfo() const;

  // The owner contract a `return` in the current body must satisfy: a
  // function's declared or inferred summary, or the innermost lambda's
  // inferred one. A lambda body never consults its enclosing function.
  struct ReturnBorrowContract {
    BorrowOriginKind origin = BorrowOriginKind::None;
    std::size_t parameter = 0;
    AccessMode access = AccessMode::ReadOnly;
    std::optional<BorrowOriginPlace> place;
    const std::vector<Parameter> *parameters = nullptr;
    const std::vector<SemanticType> *parameterTypes = nullptr;
    bool lambda = false;
    // A lambda over open parameter types decides its origin once concrete.
    bool deferred = false;
    // The origin is an expanded parameter index; a final pack covers every
    // index past the parameters written before it.
    [[nodiscard]] const Parameter *originParameter() const {
      if (parameters == nullptr || origin != BorrowOriginKind::Argument)
        return nullptr;
      std::size_t expanded = 0;
      for (const Parameter &candidate : *parameters) {
        if (candidate.pack)
          return &candidate;
        if (expanded == parameter)
          return &candidate;
        ++expanded;
      }
      return nullptr;
    }
    [[nodiscard]] const SemanticType *originParameterType() const {
      return parameterTypes != nullptr &&
                     origin == BorrowOriginKind::Argument &&
                     parameter < parameterTypes->size()
                 ? &(*parameterTypes)[parameter]
                 : nullptr;
    }
  };
  [[nodiscard]] std::optional<ReturnBorrowContract>
  currentReturnBorrowContract() const;
  struct LambdaReturnContext {
    const Lambda *declaration = nullptr;
    BorrowOriginKind origin = BorrowOriginKind::None;
    std::size_t parameter = 0;
    AccessMode access = AccessMode::ReadOnly;
    std::vector<SemanticType> parameterTypes;
    bool deferred = false;
  };
  // Set while a lambda body is analyzed, even when the lambda returns an
  // owned value, so returns inside it never read the enclosing function.
  std::optional<LambdaReturnContext> currentLambdaReturn;

  [[nodiscard]] const CallableParameterContract *
  callableParameterContract(const ExprPtr &callee) const;

  [[nodiscard]] const CallableParameterContract *
  callableParameterContract(FunctionId functionId,
                            std::size_t parameterIndex) const;

  [[nodiscard]] std::optional<CallableSignatureRequirement>
  callableSignatureRequirement(const Call &call) const;

  void
  analyzeDeferredCallableCall(const Call &call, const SemanticType &calleeType,
                              const std::vector<SemanticType> &argumentTypes);

  [[nodiscard]] bool validateCallableReturn(const Call &call,
                                            const SemanticType &returnType);

  [[nodiscard]] bool
  validateCallableCapability(const Call &call,
                             CallableInvocationCapability provided);

  void analyzeLambdaCall(const Call &call, const SemanticType &calleeType,
                         const std::vector<SemanticType> &argumentTypes);

  void
  analyzeNativeIndirectCall(const Call &call, const SemanticType &calleeType,
                            const std::vector<SemanticType> &argumentTypes);
  void analyzeFunctionItemCall(const Call &call, const SemanticType &calleeType,
                               const std::vector<SemanticType> &argumentTypes);

  [[nodiscard]] static const GenericParameterInfo *
  findGenericParameter(const std::vector<GenericParameterInfo> &parameters,
                       GenericParameterId id);

  [[nodiscard]] static const GenericParameterInfo *
  packGenericParameter(const CallableCandidate &function);

  [[nodiscard]] static std::size_t
  fixedParameterCount(const CallableCandidate &function);

  [[nodiscard]] static bool
  hasPackExpansion(const std::vector<SemanticType> &arguments);

  [[nodiscard]] bool
  appendConcreteCallArgument(const SemanticType &type,
                             const ExprPtr &expression,
                             std::vector<AnalyzedCallArgument> &result,
                             std::vector<std::size_t> packPath = {}) const;

  [[nodiscard]] std::optional<std::vector<AnalyzedCallArgument>>
  concreteCallArguments(const std::vector<SemanticType> &types,
                        const ExprList &expressions) const;

  [[nodiscard]] static bool
  acceptsArgumentShape(const CallableCandidate &function,
                       const std::vector<SemanticType> &arguments);

  [[nodiscard]] bool
  hasExactComparisonOperator(const SemanticType &type,
                             OverloadedOperator operation) const;

  [[nodiscard]] bool
  isEqualityComparableConstraintType(const SemanticType &type) const;

  [[nodiscard]] bool
  isRelationallyOrderedConstraintType(const SemanticType &type) const;

  [[nodiscard]] bool
  satisfiesAtomicConstraint(const SemanticType &argument,
                            GenericConstraintKind constraint) const;

  [[nodiscard]] std::optional<GenericConstraintKind>
  firstUnsatisfiedConstraint(const SemanticType &argument,
                             GenericConstraintSet constraints) const;

  [[nodiscard]] bool
  satisfiesConstraint(const SemanticType &argument,
                      GenericConstraintSet constraints) const;

  [[nodiscard]] std::optional<ConstraintFailure>
  firstConstraintFailure(const std::vector<GenericParameterInfo> &parameters,
                         const std::vector<SemanticType> &arguments) const;

  [[nodiscard]] std::vector<ExpandedStructuralRequirement>
  expandStructuralRequirements(
      std::span<const AppliedConceptRequirement> requirements) const;

  [[nodiscard]] static std::vector<StructuralConstraintKind>
  guaranteedStructuralKinds(StructuralConstraintKind kind);
  [[nodiscard]] bool readMemberRequirementSatisfied(
      const SemanticType &receiver, std::string_view member,
      const SemanticType &result,
      SemanticMemberRequirementFailure *failure = nullptr,
      std::span<const SemanticType> parameters = {},
      bool mutableReceiver = false, bool requiresUnsafe = false,
      bool trustedMemberAccess = false) const;
  [[nodiscard]] std::optional<ExpandedStructuralRequirement>
  expressionRequirementFact(const MemberRequirement &query,
                            std::span<const SemanticType> arguments) const;
  [[nodiscard]] bool expressionRequirementSatisfied(
      const MemberRequirement &query, std::span<const SemanticType> arguments,
      std::string *reason = nullptr,
      const std::vector<std::string> *lookupScope = nullptr) const;
  static void
  collectRequirementTypeArguments(const Expr *expression,
                                  std::vector<const TypeRef *> &types);
  void prepareRequirementLookup(const std::vector<std::string> *lookupScope,
                                SourceUnitId unit);
  [[nodiscard]] std::unique_ptr<Impl> unevaluatedExpressionWorker() const;
  [[nodiscard]] bool constraintPredicateSatisfied(
      const ConceptApplication &application,
      const std::vector<SemanticType> &arguments, std::string *reason,
      const std::vector<std::string> *lookupScope = nullptr) const;
  [[nodiscard]] std::optional<PredicateCapability> predicateCapabilityFor(
      const ConceptApplication &application,
      const std::vector<SemanticType> &arguments,
      const std::vector<std::string> *lookupScope = nullptr) const;
  mutable std::size_t expressionRequirementDepth = 0;
  void recordMemberRequirementReceiver(const MemberRequirement &query,
                                       const SemanticType &type);
  struct ActiveMemberRequirement {
    SemanticType receiver;
    std::string member;
    SemanticType result;
    std::vector<SemanticType> parameters;
    bool mutableReceiver = false;
    bool requiresUnsafe = false;
  };
  mutable std::vector<ActiveMemberRequirement> activeMemberRequirements;
  [[nodiscard]] std::optional<SemanticType>
  symbolicReadMemberReturnType(const SemanticType &receiver,
                               std::string_view member) const;
  [[nodiscard]] TypeSubstitution
  conceptSubstitution(const RegisteredConcept &conceptInfo,
                      std::span<const SemanticType> arguments) const;

  [[nodiscard]] std::vector<ExpandedStructuralRequirement>
  activeStructuralRequirements() const;

  [[nodiscard]] GenericConstraintSet
  activeAtomicConstraints(const SemanticType &type) const;

  [[nodiscard]] bool
  structuralRequirementAvailable(StructuralConstraintKind kind,
                                 std::span<const SemanticType> arguments) const;

  [[nodiscard]] std::optional<ConstraintFailure> firstRequirementFailure(
      std::span<const AppliedConceptRequirement> requirements) const;

  [[nodiscard]] bool activeRequirementsImply(
      std::span<const AppliedConceptRequirement> requirements,
      const ConstraintExpression &expression, std::size_t root) const;

  [[nodiscard]] std::optional<ConstraintFailure> firstConceptApplicationFailure(
      const AppliedConceptRequirement &requirement) const;

  [[nodiscard]] static bool
  hasConstraintFailure(const ConstraintFailure &failure);

  void reportConstraintFailure(const Token &site,
                               const ConstraintFailure &failure);

  void
  attachConstraintCallContext(ConstraintFailure &failure,
                              const CallableCandidate &candidate,
                              const std::vector<SemanticType> &argumentTypes,
                              const ExprList &expressions) const;

  bool applyVariadicFunctionTypeArguments(
      CallableCandidate &function,
      const std::vector<SemanticType> &explicitTypeArguments,
      const std::vector<CompileTimeValue> &explicitValueArguments,
      const std::vector<SemanticType> &argumentTypes, const ExprList &arguments,
      const Token &paren);

  bool inferTypeArguments(const SemanticType &pattern,
                          const SemanticType &argument,
                          const std::vector<GenericParameterInfo> &parameters,
                          GenericSubstitution &substitution,
                          const Token &argumentToken,
                          const ExprPtr *expression = nullptr);

  bool applyFunctionTypeArguments(
      CallableCandidate &function,
      const std::vector<SemanticType> &explicitTypeArguments,
      const std::vector<CompileTimeValue> &explicitValueArguments,
      const std::vector<SemanticType> &argumentTypes, const ExprList &arguments,
      const Token &paren);

  [[nodiscard]] bool tryInferTypeArguments(
      const SemanticType &pattern, const SemanticType &argument,
      const std::vector<GenericParameterInfo> &parameters,
      GenericSubstitution &substitution, const ExprPtr *expression = nullptr,
      GenericParameterId *conflictingParameter = nullptr,
      bool *conflictingValueParameter = nullptr) const;

  [[nodiscard]] bool tryInstantiateFunction(
      const CallableCandidate &candidate,
      const std::vector<SemanticType> &explicitTypeArguments,
      const std::vector<CompileTimeValue> &explicitValueArguments,
      const std::vector<SemanticType> &argumentTypes,
      const ExprList &expressions, CallableCandidate &resolved,
      std::vector<SemanticType> &resolvedTypeArguments,
      std::vector<CompileTimeValue> &resolvedValueArguments,
      ConstraintFailure *constraintFailure = nullptr);

  [[nodiscard]] static bool containsLambdaType(const SemanticType &type);

  [[nodiscard]] static bool containsClosureObjectType(const SemanticType &type);

  [[nodiscard]] bool containsExactType(const SemanticType &type,
                                       const SemanticType &candidate) const;

  // Whether `std::meta::callable_return_type` cannot reflect `subject`: a
  // concrete type with no callable signature, such as a class whose
  // operator() it does not select. Its written use reports GTI-S2023.
  [[nodiscard]] static bool
  callableReturnSubjectUnsupported(const SemanticType &subject);

  // Whether `type` contains a callable-result projection over such a subject.
  [[nodiscard]] bool
  containsUnsupportedCallableReturn(const SemanticType &type) const;

  // Whether a candidate's signature contains such a projection.
  [[nodiscard]] bool
  signatureReflectsUnsupportedCallable(const CallableCandidate &function) const;

  // Reports GTI-S2023 at `site` when instantiating `declared` as
  // `instantiated` made its signature reflect such a callable's result, so
  // the call names no result type. A written projection is reported where
  // it is written instead.
  [[nodiscard]] bool
  reportUnsupportedCallableReturn(const CallableCandidate &declared,
                                  const CallableCandidate &instantiated,
                                  const Token &site);

  [[nodiscard]] bool containsDisallowedCallableCaptureState(
      const SemanticType &type, std::unordered_set<ClassId> &visiting,
      bool reviewedRawState = false, bool checkedBorrows = false) const;

  [[nodiscard]] bool
  containsDisallowedCallableCaptureState(const SemanticType &type,
                                         bool checkedBorrows = false) const;

  [[nodiscard]] std::optional<std::size_t>
  movedFunctionParameter(const ExprPtr &expression,
                         const FunctionInfo &function) const;

  [[nodiscard]] std::optional<std::size_t>
  exactMovedReturnParameter(const FunctionInfo &function) const;

  [[nodiscard]] static const ExprList *
  constructionArguments(const ExprPtr &expression);

  [[nodiscard]] std::optional<CallableOwnedTransport>
  exactOwnedFieldTransport(const FunctionInfo &function,
                           std::size_t parameterIndex,
                           const ExprPtr &expression) const;

  void discoverOwnedCallableContracts();
  void discoverOwnedCallableValueParameters();

  [[nodiscard]] bool
  ownedTransportMatchesConcrete(const FunctionInfo &function,
                                const CallableParameterContract &contract,
                                const ResolvedCallInfo &call);

  void validateOwnedCallableArguments();

  void validateProvisionalCallableArguments();

  void validateConfinedCallableContracts();

  [[nodiscard]] bool callableContractRequiresOnce(
      FunctionId function, std::size_t parameterIndex,
      std::vector<std::pair<FunctionId, std::size_t>> &visiting) const;

  [[nodiscard]] bool
  callableContractRequiresOnce(FunctionId function,
                               std::size_t parameterIndex) const;

  void validateOnceCallableForwardings();

  void validateConfinedCallableUses();

  bool validateConfinedCallableArguments(
      const CallableCandidate &candidate, const CallableCandidate &,
      const std::vector<SemanticType> &argumentTypes,
      const ExprList &arguments);

  [[nodiscard]] static std::vector<CallableArgumentBoundary>
  lambdaArgumentBoundaries(const std::vector<SemanticType> &argumentTypes);

  [[nodiscard]] static std::string
  functionSignatureSpelling(const CallableCandidate &function);

  [[nodiscard]] bool
  typeRequiresThreads(const SemanticType &type,
                      std::unordered_set<ClassId> &visiting) const;

  [[nodiscard]] bool typeRequiresThreads(const SemanticType &type) const;

  [[nodiscard]] bool
  validateConcurrentSurfaceAvailability(const Token &location,
                                        std::string_view surface);

  void finalizeCThreadRequirements();

  void recordRawBorrowCall(const Call &call, const ResolvedCallInfo &resolved);
  void finalizeRawBorrowContracts();
  bool analyzeRawReferenceReturn(const ExprPtr &expression);

  struct RawBorrowCallArgument {
    PlaceKey place;
    std::vector<SemanticLoanDependency> dependencies;
    std::optional<std::size_t> ownerParameter;
    std::vector<RawPointerOrigin> origins;
    std::vector<std::size_t> pointerParameters;
    bool pointerReceiver = false;
    std::vector<Diagnostic> lifetimeDiagnostics;
    std::shared_ptr<RawPointerLoopUse> loopUse;
  };

  struct RawBorrowCallUse {
    const FunctionDecl *caller = nullptr;
    const FunctionDecl *callee = nullptr;
    IntrinsicKind intrinsic = IntrinsicKind::None;
    Token location;
    std::vector<RawBorrowCallArgument> arguments;
    std::optional<RawBorrowCallArgument> receiver;
  };

  RawBorrowCallArgument rawBorrowArgument(const ExprPtr &argument,
                                          const Token &location);
  void completeRawBorrowArgument(RawBorrowCallArgument &snapshot,
                                 const Token &location);

  void recordMutableBindingUse(SymbolId symbol);

  void recordMutableBindingUse(const ExprPtr &expression);

  void finalizeAdvisoryDiagnostics();

  struct CThreadCallUse {
    const FunctionDecl *caller = nullptr;
    const FunctionDecl *callee = nullptr;
    Token location;
    bool trusted = false;
  };

  [[nodiscard]] static std::string
  constructorSignatureSpelling(const ConstructorInfo &constructor);

  [[nodiscard]] bool
  validateSelectedFunction(const CallableCandidate &function,
                           const ExprPtr &callee, const Token &paren,
                           std::span<const ExprPtr> arguments,
                           CallReceiverMode receiverMode);

  // Whether a consuming method's (instantiated) result is a reference or
  // carries tracked borrowed state, which its consumed receiver cannot keep
  // alive. GTI-S2022 names that result type.
  [[nodiscard]] bool
  consumingResultCarriesBorrow(const CallableCandidate &function) const;
  void reportConsumingBorrowedResult(const CallableCandidate &function,
                                     const Token &paren);

  [[nodiscard]] std::vector<CallableCandidate>
  structuralOperatorCandidates(const SemanticType &receiver,
                               OverloadedOperator kind) const;

  [[nodiscard]] std::optional<CallableCandidate> selectStructuralOperator(
      const SemanticType &receiver, OverloadedOperator kind,
      std::span<const SemanticType> parameterTypes, bool mutableReceiver) const;

  [[nodiscard]] bool
  structuralConstraintSatisfied(StructuralConstraintKind kind,
                                std::span<const SemanticType> arguments) const;

  // A constrained generic range is analysed before its concrete begin/end
  // carrier types exist. Generated range-for endpoints receive private
  // symbolic iterator/sentinel types; concrete instance reanalysis replaces
  // them with the selected source declarations before HIR lowering.
  [[nodiscard]] std::optional<SemanticType>
  symbolicRangeMemberReturnType(const SemanticType &rangeType,
                                std::string_view member) const;

  [[nodiscard]] std::vector<CallableCandidate> symbolicOperatorRequirements(
      OverloadedOperator kind, const SemanticType &receiverType,
      std::span<const SemanticType> argumentTypes) const;

  [[nodiscard]] bool hasSymbolicOperatorRequirement(
      OverloadedOperator kind, const SemanticType &receiverType,
      std::span<const SemanticType> argumentTypes = {}) const;

  [[nodiscard]] std::optional<CallableCandidate> resolveSymbolicOperator(
      const Expr &site, OverloadedOperator kind, const ExprPtr &receiver,
      const SemanticType &receiverType, const Token &token,
      std::span<const SemanticType> argumentTypes,
      std::span<const ExprPtr> arguments, bool contextual,
      std::optional<CallableBoundary> boundary,
      const std::optional<OperatorReceiverState> &receiverState,
      bool projection);

  [[nodiscard]] bool
  hasExactOperatorCandidate(OverloadedOperator kind,
                            const SemanticType &receiverType,
                            const SemanticType &argumentType) const;

  [[nodiscard]] bool
  exactOperatorArgumentMatches(const SemanticType &parameterType,
                               const SemanticType &argumentType,
                               const ExprPtr &argument) const;

  [[nodiscard]] std::optional<CallableCandidate> resolveOperator(
      const Expr &site, OverloadedOperator kind, const ExprPtr &receiver,
      const SemanticType &receiverType, const Token &token,
      std::span<const SemanticType> argumentTypes = {},
      std::span<const ExprPtr> arguments = {}, bool contextual = false,
      std::optional<CallableBoundary> boundary = std::nullopt,
      std::optional<OperatorReceiverState> receiverState = std::nullopt,
      bool projection = false, bool preferReadOnlyReceiver = false,
      ContextualCallArguments *contextualArguments = nullptr,
      std::optional<SemanticType> conversionTarget = std::nullopt,
      bool implicitOnly = false);

  // The operator a conversion to `target` names: operator bool for bool and
  // the general conversion operator for every other target.
  [[nodiscard]] static OverloadedOperator
  conversionOperatorKind(const SemanticType &target) {
    return target == SemanticType::Bool ? OverloadedOperator::ContextualBool
                                        : OverloadedOperator::Conversion;
  }

  // A reference, a text view, or a value carrying a borrow: converting to it
  // needs stored-borrow provenance a conversion site does not yet record.
  [[nodiscard]] bool conversionTargetCarriesBorrow(const SemanticType &target) {
    return target.kind == SemanticType::Reference ||
           target.kind == SemanticType::StringView ||
           target.kind == SemanticType::CString ||
           typeTraits(target).containsBorrowedState;
  }

  // The conversion operators of the class `source` to exactly `target`.
  [[nodiscard]] std::vector<CallableCandidate>
  conversionOperatorCandidates(const SemanticType &source,
                               const SemanticType &target, bool implicitOnly);

  // The constructors of the class `target` that take `argument` as their one
  // explicit argument by type; null stands for a union's first member.
  [[nodiscard]] std::vector<const ConstructorInfo *>
  conversionConstructorCandidates(const Expr &site, const ExprPtr &argument,
                                  const SemanticType &argumentType,
                                  const SemanticType &target,
                                  const Token &token, bool implicitOnly);

  void reportMissingConversionOperator(const Token &token,
                                       const SemanticType &source,
                                       const SemanticType &target);

  void reportAmbiguousConversion(
      const Token &token, const SemanticType &source,
      const SemanticType &target, std::span<const CallableCandidate> operators,
      std::span<const ConstructorInfo *const> constructors, bool declaration);

  // Selects and records `source`'s conversion operator to `target`: on `site`
  // for an explicit conversion, and as `source`'s contextual conversion for a
  // declaration, which considers only implicit operators.
  std::optional<CallableCandidate> selectConversionOperator(
      const Expr &site, const ExprPtr &source, const SemanticType &sourceType,
      const SemanticType &target, const Token &token, bool declaration);

  // `Target(source)` or `int(source)` when `source`'s class declares a
  // conversion operator to the target. Returns false, having done nothing,
  // when it declares none.
  bool analyzeExplicitConversion(const Expr &site, const ExprPtr &source,
                                 const SemanticType &sourceType,
                                 const SemanticType &target,
                                 const Token &token);

  SemanticType arrowTargetType(const Expr &site, const ExprPtr &receiver,
                               const SemanticType &receiverType,
                               const Token &token, bool projection = false);

  void recordCallableForwardings(const Call &call,
                                 const CallableCandidate &target);

  // Editor signature help reads the parser-recorded '(' ',' ')' offsets from
  // the occurrence instead of re-parsing call punctuation. Synthesized calls
  // have no source geometry.
  [[nodiscard]] static std::optional<SemanticCallGeometry>
  callArgumentGeometry(const Call &call);

  void recordSelectedCallOccurrence(const Call &call,
                                    ResolvedCallInfo resolved);

  void recordSelectedConstructionOccurrence(const Expr &construction);

  void bindIntrinsicCallDeclaration(const Call &call,
                                    const CallableCandidate &function);

  void recordDefaultArgumentEffects(const Call &call,
                                    const CallableCandidate &function);

  [[nodiscard]] bool
  containsCAbiRawPointer(const SemanticType &type,
                         std::unordered_set<ClassId> &visiting) const;

  [[nodiscard]] bool containsCAbiRawPointer(const SemanticType &type) const;

  void
  recordResolvedCall(const Call &call, const CallableCandidate &function,
                     std::vector<SemanticType> typeArguments,
                     std::vector<CompileTimeValue> valueArguments,
                     std::vector<std::size_t> ephemeralBorrowArguments,
                     std::vector<CallableArgumentBoundary> callableArguments,
                     CallReceiverMode receiverMode,
                     const ExprPtr *argumentReceiver = nullptr);

  void requireFunctionDefinition(const CallableCandidate &function,
                                 const Token &location);

  [[nodiscard]] static SemanticType
  callExpressionType(const SemanticType &returnType);

  void reportOverloadResolutionFailure(
      const Call &call, const Symbol &overloadSet,
      const std::vector<SemanticType> &argumentTypes,
      const std::vector<const CallableCandidate *> &exactMatches,
      std::span<const ConstraintFailure> constraintFailures = {});

  [[nodiscard]] static int callConversionRank(const SemanticType &parameter,
                                              const SemanticType &argument);

  [[nodiscard]] static bool
  callConversionIsBetter(std::span<const SemanticType> leftParameters,
                         std::span<const SemanticType> rightParameters,
                         std::span<const SemanticType> arguments);

  template <typename Candidate, typename ParameterSelector>
  static void
  retainBestCallConversionMatches(std::vector<Candidate> &candidates,
                                  std::span<const SemanticType> arguments,
                                  ParameterSelector parameters) {
    if (candidates.size() < 2) {
      return;
    }
    std::vector<bool> dominated(candidates.size(), false);
    for (std::size_t candidate = 0; candidate < candidates.size();
         ++candidate) {
      for (std::size_t other = 0; other < candidates.size(); ++other) {
        if (candidate != other &&
            callConversionIsBetter(parameters(candidates[other]),
                                   parameters(candidates[candidate]),
                                   arguments)) {
          dominated[candidate] = true;
          break;
        }
      }
    }
    std::vector<Candidate> retained;
    retained.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      if (!dominated[index]) {
        retained.emplace_back(std::move(candidates[index]));
      }
    }
    candidates = std::move(retained);
  }

  [[nodiscard]] bool isContextualArrayArgument(const ExprPtr &expression) const;

  [[nodiscard]] static bool
  nativeFunctionCandidateIsEligible(const CallableCandidate &candidate);

  [[nodiscard]] static bool
  functionItemCandidateIsEligible(const CallableCandidate &candidate);

  [[nodiscard]] bool
  nativeFunctionCandidateMatches(const CallableCandidate &candidate,
                                 const SemanticType &expectedType) const;

  [[nodiscard]] std::vector<const CallableCandidate *>
  matchingNativeFunctionCandidates(const Symbol &symbol,
                                   const SemanticType &expectedType) const;

  [[nodiscard]] SemanticType
  analyzeNativeFunctionItem(const Expr &expression, const Symbol &symbol,
                            const SemanticType &expectedType,
                            const Token &location);

  [[nodiscard]] SemanticType
  analyzeFunctionItem(const Expr &expression, const Symbol &symbol,
                      const SemanticType &expectedType, const Token &location);

  [[nodiscard]] bool contextualizeFunctionItemArguments(
      const CallableCandidate &target,
      const std::vector<SemanticType> &explicitTypeArguments,
      const std::vector<CompileTimeValue> &explicitValueArguments,
      const ExprList &arguments, ContextualCallArguments &analysis,
      bool diagnose);

  [[nodiscard]] bool
  isContextualNativeFunctionArgument(const ExprPtr &expression) const;

  [[nodiscard]] bool
  isContextualCallableResultArgument(const ExprPtr &expression) const;

  // `uses` holds the optional Bind step of each argument's result use.
  [[nodiscard]] ContextualCallArguments analyzeContextualCallArguments(
      const ExprList &arguments,
      std::span<const std::optional<ResultUseStep>> uses = {});

  [[nodiscard]] bool contextualCallArgumentPotentiallyMatches(
      const SemanticType &parameter, const SemanticType &argument,
      const ExprPtr &expression, bool deferred) const;

  bool finishContextualCallArguments(
      const std::vector<SemanticType> &parameterTypes,
      const ExprList &arguments, ContextualCallArguments &analysis,
      const Token &location, std::string_view callable);

  [[nodiscard]] bool
  callArgumentRejectedByOwnership(const SemanticType &parameter,
                                  const SemanticType &argument,
                                  const ExprPtr &expression) const;

  [[nodiscard]] bool
  callArgumentMatches(const SemanticType &parameter,
                      const SemanticType &argument, const ExprPtr &expression,
                      bool allowValueAssignment = false) const;

  enum class CallArgumentMatchKind {
    Mismatch,
    Exact,
    EphemeralReadBorrow,
  };

  [[nodiscard]] bool
  ephemeralBorrowResultSupported(const CallableCandidate &function) const;

  [[nodiscard]] bool
  ephemeralBorrowTargetSupported(const CallableCandidate &function) const;

  [[nodiscard]] bool ephemeralTemporaryArgumentMatches(
      const CallableCandidate &function, const SemanticType &parameter,
      const SemanticType &argument, const ExprPtr &expression) const;

  [[nodiscard]] CallArgumentMatchKind
  functionCallArgumentMatch(const CallableCandidate &function,
                            std::size_t index, const SemanticType &argument,
                            const ExprPtr &expression, bool deferred) const;

  void reportCallArgumentMismatch(std::size_t index,
                                  const SemanticType &parameter,
                                  const SemanticType &argument,
                                  const ExprPtr &expression,
                                  std::string_view callable,
                                  const CallableCandidate *function = nullptr);

  ResolvedClassArguments
  resolveClassArguments(ClassId classId, const std::vector<TypeRef> &arguments,
                        const Token &site,
                        std::string diagnosticCode = "GTI-S2026");
  ResolvedClassArguments
  resolveNominalArguments(const std::vector<GenericParameterInfo> &parameters,
                          const Token &name,
                          const std::vector<TypeRef> &arguments,
                          const Token &site, std::string diagnosticCode);

  void
  analyzeConstructorCall(const Expr &construction, ClassId classId,
                         const std::vector<SemanticType> &typeArguments,
                         const std::vector<CompileTimeValue> &valueArguments,
                         const std::vector<SemanticType> &argumentTypes,
                         const ExprList &arguments, const Token &paren);

  [[nodiscard]] bool
  forwardedPackArgumentMatches(const SemanticType &parameter,
                               const SemanticType &argument) const;

  // A contextual construction consumes `construction` as its sole argument;
  // the selection is recorded beside the expression's own records.
  void analyzeConstructorCallArguments(
      const Expr &construction, ClassId classId,
      const std::vector<SemanticType> &typeArguments,
      const std::vector<CompileTimeValue> &valueArguments,
      std::vector<AnalyzedCallArgument> arguments, const Token &paren,
      bool requireExactNullParameter = false,
      bool contextualConstruction = false,
      std::vector<const ConstructorInfo *> *candidateProbe = nullptr);

  void selectConstructorCallArguments(
      const Expr *construction, const ReturnStmt *emptyReturn, ClassId classId,
      const std::vector<SemanticType> &typeArguments,
      const std::vector<CompileTimeValue> &valueArguments,
      std::vector<AnalyzedCallArgument> arguments, const Token &paren,
      bool requireExactNullParameter, bool contextualConstruction,
      std::vector<const ConstructorInfo *> *candidateProbe);

  void analyzeArrayMemberCall(const Call &call, const Get &member,
                              const std::vector<SemanticType> &argumentTypes,
                              const Token &paren);

  void
  analyzeStringViewMemberCall(const Call &call, const Get &member,
                              const std::vector<SemanticType> &argumentTypes,
                              const Token &paren);

  SemanticType analyzeStringViewIndexAfterOperands(
      const ExprPtr &object, const ExprPtr &index,
      const SemanticType &indexType, const Token &);

  SemanticType analyzeArrayIndexAfterOperands(const SemanticType &objectType,
                                              const ExprPtr &index,
                                              const SemanticType &indexType,
                                              const Token &bracket);

  [[nodiscard]] bool isDefaultInitializable(const SemanticType &type,
                                            bool requirePublic = false) const;

  // The exact concrete class element at the bottom of a fixed-array chain. A
  // plain class type is its own leaf. A chain broken by a length parameter, a
  // zero extent, or a non-class leaf yields Unknown.
  [[nodiscard]] static SemanticType
  fixedArrayClassLeafType(const SemanticType &type);

  [[nodiscard]] std::optional<ResolvedConstructionInfo>
  resolveDefaultConstruction(const SemanticType &type,
                             bool baseSubobject = false) const;

  // A declared default constructor's requirements for one concrete
  // application of its class.
  [[nodiscard]] std::vector<AppliedConceptRequirement>
  defaultConstructorRequirements(const ConstructorInfo &constructor,
                                 const GenericSubstitution &substitution) const;

  // Construction never selects a declared default constructor whose
  // requirements fail for an application's arguments, so that application
  // has no default constructor.
  [[nodiscard]] bool
  declaredDefaultConstructorApplies(const SemanticType &type) const;

  [[nodiscard]] static std::size_t genericTypeParameterCount(
      const std::vector<GenericParameterInfo> &parameters);

  [[nodiscard]] static std::size_t genericValueParameterCount(
      const std::vector<GenericParameterInfo> &parameters);

  [[nodiscard]] const ConstantValue *constantFor(const Symbol &symbol) const;

  [[nodiscard]] std::optional<CompileTimeValue>
  genericValueArgument(const TypeRef &argument) const;

  [[nodiscard]] bool
  reportGenericValueArgumentFailure(const TypeRef &argument,
                                    std::string_view context);

  [[nodiscard]] ArrayExtentEvaluation
  evaluateResolvedArrayExtent(const ArrayExtentExpr &expression) const;

  [[nodiscard]] std::optional<CompileTimeValue>
  resolvedArrayExtent(const ArrayExtentExprPtr &extent) const;

  void validateArrayExtent(const ArrayExtentExprPtr &extent);

  void validateType(const TypeRef &type, bool allowNativeFunctionType = false);

  [[nodiscard]] CompilerCapabilityTypeKind
  compilerCapabilityType(const TypeRef &type,
                         const std::vector<std::string> &fromScope) const;

  [[nodiscard]] CompilerCapabilityTypeKind
  compilerCapabilityType(const TypeRef &type) const;

  [[nodiscard]] bool isGtiInternalUniqueOwner(const TypeRef &type) const;

  [[nodiscard]] bool isGtiInternalTextView(const TypeRef &type) const;

  [[nodiscard]] bool containsReference(const TypeRef &type) const;

  [[nodiscard]] static bool containsRawPointer(const TypeRef &type);

  [[nodiscard]] bool isActiveTypePack(const TypeRef &type) const;
  [[nodiscard]] std::optional<GenericParameterId>
  activeTypePackIdentity(const TypeRef &type) const;

  void validateReferencePlacement(const TypeRef &type, bool allowTopLevel,
                                  std::string_view context,
                                  bool allowRvalueReference = false,
                                  bool typeMetadata = false,
                                  bool genericFunctionParameter = false,
                                  bool contractedReferenceResult = false);

  void validateReferenceReferent(const SemanticType &type, const Token &site);

  [[nodiscard]] bool containsOwningPayloadEnum(const SemanticType &type) const;

  void validateOwningPayloadEnumByValueBoundary(const SemanticType &type,
                                                const Token &location,
                                                std::string_view boundary,
                                                bool directTransport = false);

  [[nodiscard]] bool
  isPublicBaseConversion(const SemanticType &derived, const SemanticType &base,
                         std::unordered_set<ClassId> &visited) const;

  [[nodiscard]] bool isPublicBaseConversion(const SemanticType &derived,
                                            const SemanticType &base) const;

  void validateReferenceBinding(const SemanticType &reference,
                                const SemanticType &initializerType,
                                const ExprPtr &initializer);

  void validateStoredBorrowInitialization(const SemanticType &type,
                                          const ExprPtr &initializer);

  bool rejectEscapingLocalChildLoan(const ExprPtr &value,
                                    std::string_view destination);

  void validateStoredBorrowReturn(const SemanticType &returnType,
                                  const SemanticType &valueType,
                                  const ExprPtr &value);

  void validateOwnerDependentTextReturn(const SemanticType &valueType,
                                        const ExprPtr &value);

  void validateReferenceReturn(const SemanticType &reference,
                               const SemanticType &valueType,
                               const ExprPtr &value);

  void validateCurrentReturnBorrowSource(const ExprPtr &value,
                                         std::string_view description,
                                         std::string code);

  [[nodiscard]] StoredCarrierReturnProvenance storedCarrierReturnProvenance(
      const ExprPtr &expression, const Parameter &parameter,
      std::unordered_set<const Expr *> &visiting) const;

  void validateStoredCarrierReturnSource(const FunctionInfo &function,
                                         const Parameter &source,
                                         const ExprPtr &value,
                                         std::string_view description);

  [[nodiscard]] bool isParameterDerivedBorrow(const ExprPtr &expression,
                                              const Parameter &parameter) const;

  [[nodiscard]] bool
  isParameterDerivedBorrow(const ExprPtr &expression,
                           const Parameter &parameter,
                           std::unordered_set<const Expr *> &visiting) const;

  [[nodiscard]] bool isReceiverDerivedBorrow(const ExprPtr &expression) const;

  [[nodiscard]] bool
  isReceiverDerivedBorrow(const ExprPtr &expression,
                          std::unordered_set<const Expr *> &visiting) const;

  [[nodiscard]] bool hasStableBorrowStorage(const ExprPtr &expression) const;

  [[nodiscard]] bool
  hasStableBorrowStorage(const ExprPtr &expression,
                         std::unordered_set<const Expr *> &visiting) const;

  std::vector<GenericParameterInfo> makeGenericParameters(
      const std::vector<GenericParameter> &parameters,
      const Token &declarationName, const std::vector<std::string> &fromScope,
      bool allowStructural = false, bool resolveDefaults = true);
  void resolveGenericParameterDefaults(
      const std::vector<GenericParameter> &parameters,
      std::vector<GenericParameterInfo> &result,
      const std::vector<std::string> &fromScope);

  [[nodiscard]] std::shared_ptr<const GenericValueExpression>
  normalizeGenericValueDefault(const ArrayExtentExpr &expression,
                               const std::vector<std::string> &fromScope);

  struct GenericDefaultConstant {
    std::uint64_t value = 0;
    SourceUnitId sourceUnit = 0;
    bool internalLinkage = false;
    std::string qualifiedName;
    Token declaration;
  };

  struct GenericDefaultConstantUse {
    Token token;
    GenericDefaultConstant constant;
  };

  [[nodiscard]] std::optional<GenericDefaultConstant>
  genericDefaultConstant(const Token &name,
                         const std::vector<std::string> &fromScope) const;

  [[nodiscard]] std::optional<std::uint64_t>
  evaluateGenericDefaultConstantInitializer(
      const Expr &expression, const std::vector<std::string> &fromScope) const;

  void recordGenericDefaultConstant(const VariableDecl &declaration,
                                    const std::vector<std::string> &fromScope);

  void recordGenericDefaultConstantUses();

  struct GenericValueEvaluation {
    std::optional<CompileTimeValue> value;
    ConstantEvaluationFailure failure = ConstantEvaluationFailure::None;
    const Token *token = nullptr;
  };

  [[nodiscard]] GenericValueEvaluation
  evaluateGenericValueExpression(const GenericValueExpression &expression,
                                 const GenericSubstitution &substitution) const;

  [[nodiscard]] bool
  applyGenericDefaults(const std::vector<GenericParameterInfo> &parameters,
                       GenericSubstitution &substitution, const Token &site,
                       bool diagnoseFailure);

  [[nodiscard]] std::optional<ConstraintFailure>
  firstNestedTypeConstraintFailure(const SemanticType &type) const;

  [[nodiscard]] const std::vector<GenericParameterInfo> &
  genericParametersFor(const FunctionDecl &function) const;

  void
  beginTypeParameterScope(const std::vector<GenericParameterInfo> &parameters);

  void endTypeParameterScope();

  bool beginRequirementScope(const FunctionInfo *function);

  bool beginRequirementScope(const ConstructorInfo *constructor);

  void endRequirementScope();

  [[nodiscard]] std::optional<SemanticType>
  resolveTypeParameter(const NamePath &name) const;

  [[nodiscard]] std::optional<CompileTimeValue>
  resolveValueParameter(const Token &name) const;

  [[nodiscard]] SemanticType
  substituteType(const SemanticType &type,
                 const GenericSubstitution &substitution) const;

  [[nodiscard]] SemanticType
  substituteType(const SemanticType &type,
                 const TypeSubstitution &substitution) const;

  [[nodiscard]] GenericSubstitution
  classSubstitution(const SemanticType &objectType) const;
  [[nodiscard]] GenericSubstitution
  enumSubstitution(const SemanticType &type) const;
  [[nodiscard]] std::optional<ResolvedClassArguments>
  nominalArguments(const std::vector<GenericParameterInfo> &parameters,
                   const std::vector<TypeRef> &arguments,
                   const std::vector<std::string> &fromScope) const;

  [[nodiscard]] SemanticType openClassType(ClassId id) const;

  [[nodiscard]] static bool containsOpenTypeParameter(const SemanticType &type);

  [[nodiscard]] static bool
  containsGenericParameter(const SemanticType &type,
                           GenericParameterId parameter, bool value);

  std::vector<AppliedConceptRequirement>
  resolveRequirements(const RequiresClause *clause,
                      const std::vector<std::string> &scope);

  void analyzeConcreteClassStaticAssertions(const StmtList &members);

  [[nodiscard]] bool
  declaresConcreteClassStaticAssertions(const StmtList &members) const;

  // Whether the concrete class instance whose context is prepared fails one
  // of its own static assertions. They state what every analysis of the
  // instance assumes: its field analysis reports the failure, and analyses
  // of its methods, constructors and destructor report only their own
  // static assertions. Evaluated at most once per instance and request
  // outside that field analysis.
  [[nodiscard]] bool
  concreteClassAssertionsFail(ClassId classId,
                              const std::vector<SemanticType> &typeArguments,
                              const std::vector<CompileTimeValue> &values);

  void recordConcreteClassAssertionOutcome(
      ClassId classId, const std::vector<SemanticType> &typeArguments,
      const std::vector<CompileTimeValue> &values, bool failed);

  // Whether an earlier analysis in this request learned that the instance's
  // assertions fail (true) or hold (false).
  [[nodiscard]] std::optional<bool> knownConcreteClassAssertionOutcome(
      ClassId classId, const std::vector<SemanticType> &typeArguments,
      const std::vector<CompileTimeValue> &values) const;

  // Brackets a concrete instance analysis. A failed static assertion states
  // why the instance is invalid: when one of its own fails, or its owner
  // class instance's does (`ownerRejected`), the analysis reports only its
  // static assertions, not the consequences of the broken precondition.
  void beginAssertionTracking();
  void endAssertionTracking(bool ownerRejected);

  void prepareInstanceAnalysis();

  void seedExternalLambdaTypes(const std::vector<SemanticType> &types,
                               const SemanticModel &sourceModel);

  bool prepareClassInstanceContext(
      ClassId classId, const std::vector<SemanticType> &typeArguments,
      const std::vector<CompileTimeValue> &valueArguments);

  bool prepareInstanceContext(
      const FunctionInfo &function,
      const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments,
      const std::vector<SemanticType> &functionTypeArguments,
      const std::vector<CompileTimeValue> &functionValueArguments);

  void finishClassInstanceContext();

  void finishInstanceContext(const FunctionInfo &function);

  [[nodiscard]] Symbol substituteSymbol(const Symbol &symbol,
                                        const SemanticType &objectType) const;

  Symbol functionSymbol(const FunctionDecl &function,
                        const std::vector<std::string> &scope);

  void validateFunctionPacks(
      const FunctionDecl &function,
      const std::vector<GenericParameterInfo> &genericParameters);

  void validateParameterPacks(
      std::span<const Parameter> parameters,
      const std::vector<GenericParameterInfo> &genericParameters,
      const TypeRef *returnType = nullptr);

  void validateOperatorDeclaration(const FunctionDecl &function,
                                   bool methodDeclaration);

  void validateRuntimeBinding(const FunctionDecl &function);

  [[nodiscard]] static bool isCAbiScalar(const SemanticType &type);

  [[nodiscard]] bool isCAbiRecordType(const SemanticType &type) const;

  [[nodiscard]] bool isCOpaqueHandleType(const SemanticType &type) const;

  [[nodiscard]] bool isCAbiRawPointer(const SemanticType &type) const;

  [[nodiscard]] bool
  nativeFunctionBoundaryTypeAllowed(const SemanticType &type,
                                    bool returnPosition) const;

  [[nodiscard]] bool isCAbiNativeFunction(const SemanticType &type) const;

  void validateNativeCArrayAttribute(const FunctionDecl &function,
                                     const SemanticType &returnType);

  void validateNativeLinkNameAttribute(const FunctionDecl &function);

  void validateExternCFunction(const FunctionDecl &function,
                               bool methodDeclaration,
                               const SemanticType &returnType);

  void collectRootNativeStorageSymbols(const StmtList &statements);
  void validateExternCVariable(const VariableDecl &variable,
                               const SemanticType &type, SymbolId symbol);

  struct GlobalBorrowStorageInfo {
    const VariableDecl *declaration = nullptr;
    SourceUnitId sourceUnit = 0;
    SemanticType type = SemanticType::Unknown;
    SymbolId symbol = 0;
    bool internalLinkage = false;
  };

  void registerGlobalBorrowStorage(const StmtList &statements,
                                   std::vector<std::string> scope);

  [[nodiscard]] const GlobalBorrowStorageInfo *
  findGlobalBorrowStorage(std::string_view qualified) const;

  [[nodiscard]] const GlobalBorrowStorageInfo *
  resolveGlobalBorrowStorage(const ExprPtr &expression) const;

  [[nodiscard]] std::optional<BorrowOriginPlace>
  predeclaredGlobalBorrowPlace(const ExprPtr &expression) const;

  [[nodiscard]] SourceUnitId sourceUnitFor(const Token &token) const;

  [[nodiscard]] bool declarationVisible(SourceUnitId declaration,
                                        const Token &name) const;
  void validateModuleExports(const StmtList &statements,
                             bool inherited = false);

  [[nodiscard]] bool isCompilerTrustedUnit(SourceUnitId sourceUnit) const;

  [[nodiscard]] bool isCompilerTrustedRequester(const Token &use) const;

  [[nodiscard]] static bool
  isCompilerPrivateQualifiedName(std::string_view name);

  [[nodiscard]] static bool
  isCompilerPrivateScope(const std::vector<std::string> &scope);

  [[nodiscard]] bool
  compilerPrivateDeclaration(SourceUnitId sourceUnit,
                             const std::vector<std::string> &scope) const;

  [[nodiscard]] bool compilerPrivatePathDenied(const NamePath &path) const;

  bool reportCompilerPrivatePath(const NamePath &path);

  void reportCompilerPrivateAccess(const Token &location, std::string name);

  void reportCompilerPrivateDeclaration(const Token &name);

  template <typename Callback>
  void forEachSourceConsumer(SourceUnitId declaration, bool compilerPrivate,
                             std::size_t nameOffset, Callback callback) {
    if (sourceGraph == nullptr || declaration == 0) {
      return;
    }
    if (compilerPrivate && !isCompilerTrustedUnit(declaration)) {
      return;
    }
    for (const SourceUnit &unit : sourceGraph->sourceUnits()) {
      if (sourceGraph->isDeclarationVisible(unit.id, declaration, nameOffset) &&
          (!compilerPrivate || isCompilerTrustedUnit(unit.id))) {
        callback(unit.id);
      }
    }
  }

  void publishNamespace(const std::string &name, SourceUnitId declaration,
                        const Token &token);

  void publishNamespaceAlias(const std::string &name,
                             const NamespaceAliasInfo &alias,
                             const Token &token);

  void publishTypeAlias(const std::string &name, TypeAliasId id,
                        SourceUnitId declaration, bool compilerPrivate = false);

  void publishConcept(const std::string &name, ConceptId id,
                      SourceUnitId declaration, const Token &token);

  void publishClass(const std::string &name, ClassId id,
                    SourceUnitId declaration, const Token &token);

  void publishEnum(const std::string &name, EnumId id, SourceUnitId declaration,
                   const Token &token);

  [[nodiscard]] std::unordered_map<std::string, Symbol> &
  activeNamespaceSymbols();

  [[nodiscard]] const std::unordered_map<std::string, Symbol> &
  activeNamespaceSymbols() const;

  [[nodiscard]] auto &activeInternalNamespaceSymbols() {
    return catalog.sealed ? valueFlow.internalNamespaceSymbols
                          : catalog.internalNamespaceSymbols;
  }

  [[nodiscard]] auto &activeVisibleNamespaceSymbols() {
    return catalog.sealed ? valueFlow.visibleNamespaceSymbols
                          : catalog.visibleNamespaceSymbols;
  }

  [[nodiscard]] const auto &activeVisibleNamespaceSymbols() const {
    return catalog.sealed ? valueFlow.visibleNamespaceSymbols
                          : catalog.visibleNamespaceSymbols;
  }

  void publishNamespaceSymbol(const std::string &name, const Symbol &symbol);

  [[nodiscard]] bool namespaceIsVisible(const std::string &name) const;

  [[nodiscard]] const NamespaceAliasInfo *
  findNamespaceAlias(const std::string &name) const;

  [[nodiscard]] const std::unordered_map<std::string, Symbol> &
  currentNamespaceSymbols() const;

  [[nodiscard]] std::unordered_map<std::string, Symbol> *
  currentNamespaceSymbolsMutable();

  [[nodiscard]] const std::unordered_map<std::string, ClassId> &
  currentClassIds() const;

  [[nodiscard]] const std::unordered_map<std::string, EnumId> &
  currentEnumIds() const;

  [[nodiscard]] const std::unordered_map<std::string, TypeAliasId> &
  currentTypeAliasIds() const;

  [[nodiscard]] const std::unordered_map<std::string, ConceptId> &
  currentConceptIds() const;

  void registerNamespaces(const StmtList &statements,
                          std::vector<std::string> scope);

  void registerNamespaceAliases(const StmtList &statements,
                                std::vector<std::string> scope);

  void registerTypeAliases(const StmtList &statements,
                           std::vector<std::string> scope);

  void registerClassTypeAliases(const StmtList &members,
                                const std::vector<std::string> &scope,
                                const ClassDecl &owner, AccessModifier &access);

  void registerTypeAlias(const TypeAliasDecl &alias,
                         const std::vector<std::string> &lookupScope,
                         const std::vector<std::string> &namespaceScope,
                         const ClassDecl *owner, AccessModifier access);

  void resolveTypeAliases();
  void validateTypeAliasConstraints();

  void registerConcepts(const StmtList &statements,
                        std::vector<std::string> scope);

  void resolveConcepts();

  struct ConstraintPredicateDeclaration {
    const FunctionDecl *syntax = nullptr;
    SourceUnitId sourceUnit = 0;
    std::vector<std::string> namespaceScope;
  };

  // This early declaration index supplies only proof discovery before the
  // ordinary callable catalog exists. It never selects executable calls.
  void
  registerConstraintPredicateDeclarations(const StmtList &statements,
                                          std::vector<std::string> scope = {});
  void recordConstraintPredicateUses(
      const std::vector<ConceptApplication> &requirements);
  [[nodiscard]] std::optional<PredicateCapability>
  constraintPredicateCapability(const Call &call,
                                const std::vector<SemanticType> &typeArguments);

  [[nodiscard]] GenericConstraintSet resolveConcept(ConceptId id,
                                                    const Token *use);

  void resolveTypeAliasDependencies(const TypeRef &type,
                                    const std::vector<std::string> &scope);

  [[nodiscard]] SemanticType resolveTypeAlias(TypeAliasId id, const Token *use);

  [[nodiscard]] static SemanticType
  enumUnderlyingType(const std::optional<TypeRef> &type);

  [[nodiscard]] static bool validEnumUnderlyingSyntax(const TypeRef &type);

  [[nodiscard]] static std::optional<EnumConstant>
  nextEnumConstant(EnumConstant value);

  void registerEnums(const StmtList &statements,
                     std::vector<std::string> scope);

  [[nodiscard]] bool payloadTypeAllowed(const SemanticType &type,
                                        EnumId owner) const;

  void resolveEnumPayloads();
  void registerEnumGenericParameters();
  void resolveEnumGenericDefaults(EnumId id);
  void validateConcreteEnumPayloads(const SemanticType &type,
                                    const Token &site);

  [[nodiscard]] CapabilityPolicyRegistration registerClassAttributes(
      const ClassDecl &declaration,
      const std::vector<GenericParameterInfo> &parameters = {});

  void registerClasses(const StmtList &statements,
                       std::vector<std::string> scope,
                       ClassId enclosingClass = 0,
                       AccessModifier *enclosingAccess = nullptr);
  [[nodiscard]] bool isWithinClass(ClassId owner) const;
  // Visibility only: callers still enforce receiver mutability, loans and
  // lifetime. An Unknown receiver denotes a static member, type, or base
  // initializer.
  [[nodiscard]] bool
  canAccessMember(AccessModifier access, ClassId owner,
                  const SemanticType &receiver = SemanticType::Unknown,
                  const SemanticType &lookupType = SemanticType::Unknown) const;
  [[nodiscard]] SemanticType memberDeclaringType(const SemanticType &type,
                                                 ClassId owner) const;
  [[nodiscard]] bool validateNestedClassAccess(ClassId id, const Token &use);
  [[nodiscard]] bool validateClassPathAliases(const NamePath &path,
                                              const Token &use);
  void resolveClassGenericDefaults(ClassId id);

  [[nodiscard]] std::vector<GenericParameterInfo>
  makeClassPatternParameters(const ClassDecl &declaration,
                             const std::vector<std::string> &scope);
  void resolveClassArgumentPatterns(ClassId id);
  [[nodiscard]] bool
  classArgumentPatternsMatch(const ClassInfo &owner,
                             const std::vector<SemanticType> &arguments) const;

  [[nodiscard]] SemanticType
  canonicalExactSpecializationType(SemanticType type) const;

  [[nodiscard]] SemanticType
  selectExactClassSpecialization(SemanticType type) const;

  [[nodiscard]] ClassId
  selectClassDefinition(ClassId id,
                        const std::vector<SemanticType> &arguments) const;

  [[nodiscard]] static bool
  isConcreteExactSpecializationType(const SemanticType &type);

  [[nodiscard]] bool samePackageOwnershipDomain(SourceUnitId left,
                                                SourceUnitId right) const;

  [[nodiscard]] bool
  ownsNominalSpecializationArgument(SourceUnitId sourceUnit,
                                    const SemanticType &argument) const;

  void reportExactSpecializationError(const ClassDecl &declaration,
                                      std::string message,
                                      const ClassInfo *primary = nullptr);

  // Protocol concepts and conformance blocks.
  void resolveProtocolConcept(RegisteredConcept &conceptInfo, bool &valid);
  void registerConformances(const StmtList &statements,
                            std::vector<std::string> scope);
  void bindConformanceOperations();
  void validateConformanceTypes();
  [[nodiscard]] std::optional<ConformanceMatch>
  matchConformance(ConceptId id,
                   const std::vector<SemanticType> &arguments) const;
  [[nodiscard]] std::optional<std::vector<SemanticType>>
  classTemplateArguments(const SemanticType &type, ClassId owner) const;
  [[nodiscard]] static bool openConformancePattern(const SemanticType &pattern);
  [[nodiscard]] static bool
  builtinConformanceConstructor(const SemanticType &type);
  [[nodiscard]] static bool
  sameConformanceConstructor(const SemanticType &left,
                             const SemanticType &right);
  [[nodiscard]] std::optional<std::vector<SemanticType>>
  patternInstanceArguments(const SemanticType &pattern,
                           const SemanticType &type) const;
  [[nodiscard]] bool
  conformancePatternsOverlap(const RegisteredConformance &existing,
                             const std::vector<SemanticType> &patterns) const;
  [[nodiscard]] std::optional<ConceptId>
  protocolConceptOwner(const TypeRef &owner,
                       const std::vector<std::string> &fromScope) const;
  [[nodiscard]] SemanticType
  associatedTypeProjection(ConceptId id, std::uint32_t member,
                           std::vector<SemanticType> arguments) const;
  [[nodiscard]] SemanticType
  reduceAssociatedTypeProjection(SemanticType projection) const;
  [[nodiscard]] SemanticType
  protocolOperationType(ConceptId id, const SemanticType &type,
                        const std::vector<SemanticType> &arguments) const;
  [[nodiscard]] const RegisteredConformance *
  enclosingConformance(const std::vector<std::string> &fromScope) const;
  [[nodiscard]] SemanticType
  blockScopeType(const RegisteredConformance &conformance,
                 const SemanticType &type) const;
  [[nodiscard]] std::optional<SemanticType>
  blockAssociatedType(std::string_view name,
                      const std::vector<std::string> &fromScope) const;
  [[nodiscard]] std::optional<SemanticType>
  ownBlockProjection(ConceptId id, std::uint32_t member,
                     const std::vector<SemanticType> &arguments,
                     const std::vector<std::string> &fromScope) const;
  [[nodiscard]] std::optional<ConstraintFailure> protocolConformanceFailure(
      const AppliedConceptRequirement &requirement) const;
  [[nodiscard]] std::optional<std::size_t>
  protocolBorrowContractFailure(const RegisteredConformance &conformance,
                                const std::vector<SemanticType> &binding) const;
  void validateAssociatedTypePath(const TypeRef &type, ConceptId id);
  [[nodiscard]] std::optional<ProtocolOperationTarget>
  protocolOperationTarget(const QualifiedName &callee) const;
  [[nodiscard]] bool analyzeProtocolOperationCall(const Call &expr,
                                                  const QualifiedName &callee,
                                                  ConceptId id);
  // Prefix `?` through the prelude's `std::tryable` protocol concept.
  [[nodiscard]] std::optional<ConceptId> tryableConcept() const;
  [[nodiscard]] std::optional<ResolvedCallInfo>
  protocolOperationCall(const RegisteredConformance &conformance,
                        const std::vector<SemanticType> &binding,
                        std::size_t operation) const;
  [[nodiscard]] bool
  selectResidualReturn(const Unary &expr, const SemanticType &returnType,
                       const SemanticType &residualType,
                       std::optional<ResolvedConstructionInfo> &construction);
  [[nodiscard]] SemanticType
  analyzeTryablePropagation(const Unary &expr, const SemanticType &operandType);
  void reportConformanceError(const Token &site, std::string message,
                              const Token *related = nullptr,
                              std::string relatedMessage = {});
  void registerClassSpecializations(const StmtList &statements,
                                    std::vector<std::string> scope);

  void resolveClassInheritance();

  void detectInheritanceCycle(ClassId id, std::vector<std::uint8_t> &state);

  void collectUniqueAncestors(const ClassInfo &current, ClassId root,
                              std::unordered_set<ClassId> &ancestors);

  void registerFunctionGenericParameters(const StmtList &statements,
                                         std::vector<std::string> scope,
                                         bool classMember);

  [[nodiscard]] static std::optional<std::size_t>
  nativeCArrayCountParameter(const FunctionDecl &function);

  void registerNamespaceSymbols(const StmtList &statements,
                                std::vector<std::string> scope,
                                bool typesOnly = false);

  void collectClassMembers(const StmtList &statements,
                           std::vector<std::string> scope);

  [[nodiscard]] bool
  byValueTypeReachesClass(const SemanticType &type, const SemanticType &target,
                          std::vector<SemanticType> &visiting,
                          std::vector<SemanticType> &visitingEnums,
                          bool &exhausted) const;

  void validateOrdinaryClassLayouts();
  void validateOrdinaryClassLayout(const SemanticType &type);

  void refreshFunctionPolymorphism(const CallableCandidate &candidate);

  static void refreshOverloadPrivacy(Symbol &symbol);

  void mergeInheritedMember(ClassInfo &owner, const std::string &name,
                            MemberInfo incoming);

  void mergeDeclaredMember(ClassInfo &owner, const std::string &name,
                           MemberInfo local);

  void buildInheritedMembers(ClassId id, std::vector<std::uint8_t> &state);

  void resolveInheritedMembers();

  [[nodiscard]] static const ClassBaseTypeInfo *
  concreteBase(const ClassInfo &owner);

  [[nodiscard]] static const ConstructorInfo *
  defaultConstructor(const ClassInfo &owner);

  [[nodiscard]] static std::size_t
  defaultConstructorCandidateCount(const ClassInfo &owner);

  [[nodiscard]] static bool
  requestsStructuralDefaultConstruction(const ConstructorInfo &constructor);

  [[nodiscard]] static bool hasSourceDestructorBody(const ClassInfo &owner);

  [[nodiscard]] bool
  classCanUseStructuralDefaultConstruction(const ClassInfo &owner) const;

  [[nodiscard]] bool classCanUseStructuralDefaultConstruction(
      const SemanticType &type, std::vector<SemanticType> &visiting) const;
  [[nodiscard]] bool
  classCanUseStructuralDefaultConstruction(const SemanticType &type) const;
  [[nodiscard]] bool
  classCanGenerateDefaultConstructor(const SemanticType &type) const;

  [[nodiscard]] bool
  classCanGenerateDefaultConstructor(const ClassInfo &owner) const;

  [[nodiscard]] bool
  baseHasAccessibleDefaultConstructor(const ClassBaseTypeInfo &base,
                                      ClassId derived) const;

  void
  analyzeBaseConstructorInitializer(const ConstructorInitializer &initializer,
                                    const SemanticType &baseType);

  void reportCAbiRecord(const Token &location, std::string message);

  [[nodiscard]] static bool isC17Keyword(std::string_view spelling);

  [[nodiscard]] static bool
  isNativeBridgeSupportTypeName(std::string_view spelling);

  [[nodiscard]] static bool
  isNativeBridgeObjectMacroName(std::string_view spelling);

  [[nodiscard]] static bool
  isNativeBridgeFunctionMacroName(std::string_view spelling);

  [[nodiscard]] static std::optional<std::string>
  reservedNativeIdentifierConflict(std::string_view spelling,
                                   bool occupiesCFileScope);

  [[nodiscard]] static std::optional<std::string> nativeCIdentifierConflict(
      std::string_view spelling, bool exposedWithExactCSpelling,
      bool occupiesCOrdinaryNamespace, bool followedByLeftParen);

  [[nodiscard]] static bool
  isPortableNativeCIdentifier(std::string_view spelling);

  bool validateNativeCIdentifierSpelling(std::string_view spelling,
                                         const Token &location,
                                         std::string_view diagnosticCode,
                                         std::string_view entity,
                                         bool exposedWithExactCSpelling,
                                         bool occupiesCOrdinaryNamespace,
                                         bool followedByLeftParen = false);

  bool validateNativeCIdentifier(const Token &identifier,
                                 std::string_view diagnosticCode,
                                 std::string_view entity,
                                 bool exposedWithExactCSpelling,
                                 bool occupiesCOrdinaryNamespace,
                                 bool followedByLeftParen = false);

  bool validateNativeFacingNamespaces(const StmtList &declarations,
                                      std::size_t depth = 0);

  void reportCOpaqueHandle(const Token &location, std::string message);

  bool rejectCOpaquePointeeOperation(const SemanticType &pointer,
                                     const Token &location,
                                     std::string_view operation);

  void validateCOpaqueHandles();

  void validateCAbiAccessSpecifiers(const StmtList &members,
                                    const ClassInfo &owner, bool &valid);

  [[nodiscard]] bool cAbiPointerPointeeAllowed(const SemanticType &type) const;

  [[nodiscard]] bool cAbiFieldTypeAllowed(const SemanticType &type) const;

  [[nodiscard]] bool validateCAbiRecordShape(ClassInfo &owner);

  [[nodiscard]] static bool alignCAbiOffset(std::uint64_t value,
                                            std::uint32_t alignment,
                                            std::uint64_t &result);

  [[nodiscard]] std::optional<CAbiRecordLayout>
  computeCAbiRecordLayout(ClassId id, std::vector<std::uint8_t> &state,
                          const std::vector<bool> &shapeValid);

  void reportUnion(const Token &location, std::string message);

  [[nodiscard]] bool unionFieldTypeAllowed(const SemanticType &type) const;

  [[nodiscard]] bool validateUnionShape(ClassInfo &owner);

  [[nodiscard]] std::optional<UnionLayout>
  computeUnionLayout(ClassId id, std::vector<std::uint8_t> &state,
                     const std::vector<bool> &shapeValid);

  void validateUnions();

  void validateNativeRecords();

  void validateStoredReferenceContracts();

  using BorrowSummaryCandidateIndex =
      std::unordered_map<FunctionId, std::vector<CallableCandidate *>>;

  void
  indexFunctionCandidateBorrowSummaries(Symbol &symbol,
                                        BorrowSummaryCandidateIndex &index);

  [[nodiscard]] BorrowSummaryCandidateIndex
  functionCandidateBorrowSummaryIndex();

  void publishFunctionBorrowSummary(
      const FunctionInfo &function,
      const BorrowSummaryCandidateIndex &candidateIndex);

  void
  collectBorrowReturnExpressions(const Stmt *statement,
                                 std::vector<const ExprPtr *> &returns) const;

  [[nodiscard]] std::optional<std::vector<RestoredBorrowField>>
  restoredReferenceFields(const SemanticType &referent);

  void recordRestoredReferenceResult(const FunctionInfo &function);

  struct PredeclaredTextBorrowSource {
    bool known = false;
    bool ownerDependent = false;
    BorrowOriginKind origin = BorrowOriginKind::None;
    std::size_t parameter = 0;
    // A direct text field of the origin (L-OWN-02): the result carries the
    // dependency the caller's value holds in that field, not the origin's.
    SymbolId field = 0;
  };

  [[nodiscard]] PredeclaredTextBorrowSource predeclaredTextBorrowSource(
      const ExprPtr &expression, const FunctionInfo &function,
      const SemanticType &ownerType = SemanticType::Unknown);

  // The direct text field of a read-only formal or the receiver that
  // `expression` reads, when `function` can carry field-level text contracts.
  // With `field`, `expression` names the formal or receiver and the field is
  // given: a helper returns that field of it.
  // A generic class method's receiver is `ownerType` when that names an
  // instance of its class, and otherwise the open class, whose type-parameter
  // fields count with `conditional`.
  [[nodiscard]] std::optional<PredeclaredTextBorrowSource>
  predeclaredTextFieldSource(
      const ExprPtr &expression, const FunctionInfo &function,
      SymbolId field = 0, const SemanticType &ownerType = SemanticType::Unknown,
      bool conditional = false);

  [[nodiscard]] bool storedTextViewRead(const ExprPtr &expression,
                                        const FunctionInfo &function,
                                        SemanticType ownerType);

  // What a function's returns read, accumulated over the passes of a
  // recursive summary: whether some return's provenance is unknown, the
  // direct origins of its dependent returns, and the contract-formal text
  // fields they read (L-OWN-02). The facts only grow.
  struct TextRelaySources {
    bool unknown = false;
    std::set<std::pair<BorrowOriginKind, std::size_t>> direct;
    std::set<std::tuple<BorrowOriginKind, std::size_t, SymbolId>> fields;

    void add(const PredeclaredTextBorrowSource &source);
    // A summary no further return can change: unknown or two origins.
    [[nodiscard]] bool settled() const { return unknown || direct.size() > 1; }
    friend bool operator==(const TextRelaySources &,
                           const TextRelaySources &) = default;
  };
  [[nodiscard]] static std::optional<FunctionReturnBorrowSummary>
  textRelaySummary(const TextRelaySources &sources);

  // The text summaries of the outermost dynamicTextReturnBorrowSummary query,
  // per declaration. A summary whose evaluation read one lower on the stack
  // is provisional until the evaluation that read nothing lower completes.
  struct TextBorrowSummaryEntry {
    const FunctionDecl *declaration = nullptr;
    TextRelaySources sources;
    // The 1-based position on the evaluation stack while its returns are
    // being evaluated, and zero otherwise.
    std::size_t depth = 0;
    // The lowest stack position a provisional summary depends on.
    std::size_t lowestRead = 0;
    bool provisional = false;
    bool complete = false;
  };
  std::deque<TextBorrowSummaryEntry> textBorrowSummaries;
  // Provisional summaries, in the order they were computed.
  std::vector<std::size_t> provisionalTextBorrowSummaries;
  std::size_t textBorrowSummaryDepth = 0;
  // Reference relay summaries being composed, outermost first.
  std::vector<const FunctionDecl *> textReferenceSummaryStack;
  // The contextual construction whose own argument storedBorrowDependencies
  // is reading, rather than its converted value.
  const Expr *contextualTextArgument = nullptr;
  // The switch subject being analyzed, without groupings: a payload-enum
  // field read there is observed in place (notePayloadFieldRead).
  const Expr *observedSwitchSubject = nullptr;
  // A reference local bound to a dependent place: its uses are that place's,
  // so a switch may observe it but it cannot carry the text out (L-OWN-02).
  std::unordered_map<const VariableDecl *, const Expr *>
      dependentReferenceAliases;
  // The lowest stack position the current evaluation has read.
  std::size_t textBorrowReadDepth = std::numeric_limits<std::size_t>::max();
  // Whether a provisional summary grew in the current evaluation.
  bool provisionalTextBorrowGrew = false;

  // Whether `function`'s summary is still being computed: a call to it is
  // recursive and reads its current approximation.
  [[nodiscard]] bool textBorrowSummaryInProgress(const FunctionInfo &function);

  [[nodiscard]] std::optional<FunctionReturnBorrowSummary>
  dynamicTextReturnBorrowSummary(
      const FunctionInfo &function,
      const SemanticType &ownerType = SemanticType::Unknown);

  // The receiver's direct text field, or text payload of a payload-enum
  // field, that every returned reference to text designates, in a
  // field-contract method.
  [[nodiscard]] std::optional<OwnerlessTextField> textReferenceReturnSummary(
      const FunctionInfo &function,
      const SemanticType &ownerType = SemanticType::Unknown);
  // The receiver payload that `arm` of `selection` relays by reference: the
  // whole arm returns a read-only reference binding of one text payload of
  // the receiver field `this.field` that the switch observes.
  // The relay of the receiver accessor that `returned` calls on `this`: a
  // zero-argument call selecting one read-only method, whose relay a
  // method returning that call composes.
  [[nodiscard]] std::optional<OwnerlessTextField>
  composedTextReference(const ExprPtr &returned, const FunctionInfo &function,
                        const SemanticType &ownerType);
  [[nodiscard]] std::optional<OwnerlessTextField>
  payloadReferenceRelayArm(const SwitchStmt &selection, const SwitchArm &arm,
                           const FunctionInfo &function,
                           const SemanticType &ownerType,
                           bool conditional) const;
  // The one read-only, argument-free, non-virtual method `name` of `owner`:
  // the overload a call `this.name()` on a read-only receiver selects.
  [[nodiscard]] const FunctionInfo *
  readOnlyReceiverAccessor(ClassId owner, const std::string &name) const;
  // The receiver fields that `accessor` returns references to, sorted: every
  // return is `this.field`, or a call `this.other()` of another such
  // accessor, or, in a body that declares nothing, the implicit `field` or
  // `other()`.
  [[nodiscard]] std::optional<std::vector<std::string>>
  accessorReceiverFields(const FunctionInfo &accessor,
                         std::vector<const FunctionInfo *> &visiting) const;
  // The receiver field `accessor` returns a reference to when every return
  // designates that one field.
  [[nodiscard]] std::optional<std::string>
  accessorReceiverField(const FunctionInfo &accessor,
                        std::vector<const FunctionInfo *> &visiting) const;
  // The receiver fields that `subject`, a call `this.accessor()` in a method
  // of `owner`, designates; also an implicit `accessor()` where
  // `implicitMembers` says no local can shadow the name.
  [[nodiscard]] std::optional<std::vector<std::string>>
  receiverAccessorFields(const Expr &subject, ClassId owner,
                         std::vector<const FunctionInfo *> &visiting,
                         bool implicitMembers = false) const;
  // The one receiver field that `subject` designates.
  [[nodiscard]] std::optional<std::string>
  receiverAccessorField(const Expr &subject, ClassId owner,
                        std::vector<const FunctionInfo *> &visiting,
                        bool implicitMembers = false) const;

  [[nodiscard]] std::optional<BorrowOriginPlace>
  globalReturnBorrowPlace(const FunctionInfo &function);

  void resolveFunctionBorrowSummaries();

  void recordClassLifecycles();
  void registerDesignatedConstructors();
  void analyzeDesignatedInitializer(const DirectInitializer &expr);

  void collectMembers(const StmtList &members, ClassInfo &owner,
                      AccessModifier &access,
                      const Token *mutableGroupOrigin = nullptr);

  [[nodiscard]] static FunctionInfo
  projectCallableDeclaration(const CallableDeclaration &declaration);

  void publishCallableDeclaration(
      const FunctionDecl &function, const CallableCandidate &candidate,
      const std::vector<std::string> &namespaceScope, ClassId ownerClass);

  [[nodiscard]] const ClassInfo *
  capabilityRequirement(const ClassInfo &owner, bool transfer,
                        std::unordered_set<ClassId> &visiting) const;

  void validateInterfaceCapabilities();

  void recordClassTypeIdentities();

  void recordClassFieldSymbols();
  void recordClassTypes();

  [[nodiscard]] static bool
  canMaterializeProgramConstant(const ConstantValue &value);

  [[nodiscard]] static bool
  canDataInitializeProgramConstant(const ConstantValue &value);

  void recordProgramStorageEffect(const Expr &expression,
                                  SemanticBindingKind bindingKind,
                                  SymbolId symbol, OccurrenceRole roles);

  void addProgramEffectEdge(ProgramEffectOwner target, const Expr &expression);

  void clearProgramConstantSubstitutionsInBoundary(const Expr &expression,
                                                   SymbolId symbol);

  void forceProgramStorageBoundaryAccess(const ExprPtr &argument);

  void recordProgramArgumentBoundaryEffects(
      std::span<const SemanticType> parameterTypes,
      ResolvedCallArguments arguments);

  void addProgramEffectEdge(ProgramEffectOwner target, SourceSpan span);

  void addUnknownProgramEffect(const Expr &expression, std::string reason);

  void addUnknownProgramEffect(SourceSpan span, std::string reason);

  [[nodiscard]] const ConstructorInfo *
  programConstructor(ConstructorId id) const;

  void recordProgramConstructorDefaultArgumentEffects(
      const ConstructorInfo *constructor, std::size_t suppliedArguments,
      const SourceSpan &span);

  void recordProgramDefaultConstructionEffects(
      const SemanticType &type, const SourceSpan &span,
      std::unordered_set<ClassId> &visiting);

  void recordProgramDefaultConstructionEffects(const SemanticType &type,
                                               const SourceSpan &span);

  void recordProgramSelectedConstructorEffect(
      const ConstructorInfo *constructor, const SemanticType &constructedType,
      const SourceSpan &span, const std::vector<SemanticType> &types = {},
      const std::vector<CompileTimeValue> &values = {});

  [[nodiscard]] bool hasOpenProgramCleanupShape(const SemanticType &type) const;

  void recordProgramCleanupEffects(const SemanticType &type,
                                   const SourceSpan &span,
                                   std::vector<SemanticType> &visiting,
                                   bool includeDirectDestructor = true);

  void recordProgramCleanupEffects(const SemanticType &type,
                                   const SourceSpan &span,
                                   bool includeDirectDestructor = true);

  // The effect edge of a selected overloaded operator's target function.
  void
  recordProgramOperatorTargetEffects(const Expr &expression,
                                     const ResolvedOperatorInfo &operation);

  void recordProgramCallEffects(const Expr &expression);

  // Whether the construction's closed effects were open and recording stopped.
  [[nodiscard]] bool
  recordProgramConstructionEffects(const Expr &site,
                                   const ResolvedConstructionInfo &construction,
                                   std::size_t suppliedArguments);
  [[nodiscard]] bool
  recordProgramConstructionEffects(const SourceSpan &site,
                                   const ResolvedConstructionInfo &construction,
                                   std::size_t suppliedArguments);

  [[nodiscard]] std::vector<SourceUnitId>
  programInitializationUnitOrder() const;

  void collectProgramInitializationStatement(
      const Stmt &statement, SourceUnitId sourceUnit,
      std::optional<ClassId> enclosingClass,
      std::vector<ProgramInitializationStep> &steps) const;

  void
  collectProgramEntryStatement(const Stmt &statement,
                               const FunctionDecl *&entryDeclaration) const;

  [[nodiscard]] ConstructorId
  findProgramEntryConstructor(const SemanticType &type,
                              std::span<const SemanticType> parameters) const;

  [[nodiscard]] static DefinedFailureOperation
  hostedFailure(const SourceSpan &anchor, DefinedFailureCode code,
                DefinedFailureDetail detail);

  void finalizeHostedProgramEntry(const Program &program);

  void reportProgramInitializationAccess(
      const ProgramInitializationStep &initializer, const SourceSpan &primary,
      const ProgramStorageEffectUse *use, const ProgramUnknownEffect *unknown);

  void
  validateProgramInitializationEffects(const ProgramInitializationPlan &plan);

  void finalizeDeferredProgramConstantSubstitutions(
      const ProgramInitializationPlan &plan);

  void finalizeProgramInitialization(const Program &program);

  [[nodiscard]] SourceUnitId statementSourceUnit(const Stmt &statement) const;

  void analyze(const StmtList &statements);

  // Constant loop edges are unreachable for move-state purposes, but the
  // current MIR keeps their structural blocks. Preserve semantic loan uses so
  // HIR/MIR borrow endpoints remain aligned while discarding move states and
  // break/continue exits from the skipped edge.
  void analyzeMoveUnreachable(const StmtPtr &statement);

  SemanticType analyzeMoveUnreachable(const ExprPtr &expression);

  SemanticType analyzeExpectedCallableResultMoveUnreachable(
      const ExprPtr &expression, const SemanticType &expectedType);

  void analyzeConditionalArm(const StmtPtr &statement);

  // A direct loop body is still a distinct dynamic region. Giving unbraced
  // bodies the same region boundary as blocks prevents a loan created before
  // the loop from being mistaken for an iteration-local conditional loan.
  void analyzeLoopBody(const StmtPtr &statement);

  void analyze(const StmtPtr &stmt);

  [[nodiscard]] ExpressionInfo classifyExpression(const Expr &expr,
                                                  SemanticType type) const;

  [[nodiscard]] static FailurePropagationKind
  failurePropagation(CallDispatch dispatch);

  // The logical-size check is identity-bound: the outcome detail names the
  // enclosing trusted default-library container, so a vector or string
  // index failure reports "in vector"/"in string". Any other enclosing
  // context keeps the private-storage detail.
  [[nodiscard]] DefinedFailureDetail storageBoundsCheckDetail() const;

  static void addFailureOutcome(DefinedFailureOperation &operation,
                                DefinedFailureCode code,
                                DefinedFailureDetail detail);

  [[nodiscard]] static std::optional<DefinedFailureDetail>
  arithmeticFailureDetail(TokenKind operation);

  void addIntegerOperationFailures(
      DefinedFailureOperation &operation, TokenKind token,
      const SemanticType &resultType,
      const SemanticType &countType = SemanticType::Unknown) const;

  [[nodiscard]] static bool
  numericConversionMayFail(const SemanticType &source,
                           const SemanticType &target);

  [[nodiscard]] SemanticType
  compoundIntermediateType(TokenKind operation, const SemanticType &target,
                           const SemanticType &right,
                           const Expr *leftExpression,
                           const Expr *rightExpression) const;

  void addCompoundConversionFailure(DefinedFailureOperation &operation,
                                    TokenKind token, const SemanticType &target,
                                    const SemanticType &right,
                                    const Expr *leftExpression,
                                    const Expr *rightExpression) const;

  [[nodiscard]] DefinedFailureDetail uniqueOwnerAccessDetail() const;

  [[nodiscard]] DefinedFailureOperation
  classifyDefinedFailure(const Expr &expr, const ExpressionInfo &info) const;

  SemanticType analyze(const Expr &expr);

  SemanticType analyze(const Expr &expr, OccurrenceRole requestedRoles);

  void discardResult(const ExprPtr &expr);
  // Reports a used result of a built-in assignment to a move-only type.
  void checkAssignmentResultUse(const Expr &expr, const SemanticType &result);

  SemanticType analyze(const ExprPtr &expr);

  SemanticType analyze(const ExprPtr &expr, OccurrenceRole roles);

  SemanticType
  analyzeExpectedCallableResult(const ExprPtr &expr,
                                const SemanticType &expectedType,
                                bool inferFromInstantiation = false);

  SemanticType analyzeInitializer(const ExprPtr &expr,
                                  const SemanticType &expectedType,
                                  bool allowExactNullConstruction = false,
                                  bool allowDeclarationConstruction = false);

  [[nodiscard]] bool
  declarationConstructionApplies(const ExprPtr &expr,
                                 const SemanticType &declaredType,
                                 const SemanticType &initializerType);

  SemanticType
  analyzeDeclarationConstruction(const ExprPtr &expr,
                                 const SemanticType &declaredType,
                                 const SemanticType &initializerType);

  // Whether a declaration's initializer of another type could convert to the
  // declared type through an implicit conversion operator of its class.
  [[nodiscard]] bool
  declarationConversionApplies(const ExprPtr &expr,
                               const SemanticType &declaredType,
                               const SemanticType &initializerType);

  SemanticType analyzeDeclarationConversion(const ExprPtr &expr,
                                            const SemanticType &declaredType,
                                            const SemanticType &initializerType,
                                            bool constructionApplies);

  // A declaration initializer whose value an implicit constructor or implicit
  // conversion operator converts to the declared type.
  [[nodiscard]] bool
  recordsDeclarationConversion(const Expr &initializer) const {
    return semanticModel.recordsContextualConstruction(initializer) ||
           semanticModel.recordsContextualConversion(initializer);
  }

  [[nodiscard]] std::vector<ProjectedRawPointerOrigins>
  contextualConstructionRawFields(const Expr &initializer,
                                  const SemanticType &type);

  // The hint for a declaration no implicit conversion accepts, with the
  // explicit spelling that selects any constructor or conversion operator.
  [[nodiscard]] static std::string
  implicitConstructionHint(const std::string &explicitSpelling) {
    return "Only a constructor or conversion operator declared 'implicit' "
           "converts a declaration's initializer; write '" +
           explicitSpelling + "' to convert explicitly.";
  }

  [[nodiscard]] bool isDefaultLibraryUnit(SourceUnitId sourceUnit) const;

  [[nodiscard]] bool isPreludeUnit(SourceUnitId sourceUnit) const;

  [[nodiscard]] bool isThreadLibraryUnit(SourceUnitId sourceUnit) const;

  [[nodiscard]] bool isAtomicLibraryUnit(SourceUnitId sourceUnit) const;

  [[nodiscard]] bool isMutexLibraryUnit(SourceUnitId sourceUnit) const;

  [[nodiscard]] bool isMemoryLibraryUnit(SourceUnitId sourceUnit) const;
  [[nodiscard]] std::optional<CallableCandidate>
  textViewMemberFactory(std::string_view member) const;
  [[nodiscard]] bool isOptionalLibraryUnit(SourceUnitId sourceUnit) const;
  [[nodiscard]] bool isPolymorphicLibraryUnit(SourceUnitId sourceUnit) const;

  [[nodiscard]] bool isSourceLocationLibraryUnit(SourceUnitId sourceUnit) const;
  [[nodiscard]] bool isMetaLibraryUnit(SourceUnitId sourceUnit) const;

  // ---- Compile-time reflection (std/meta) ----
  // Classes bound by `@compiler_type`, by binding kind.
  std::unordered_map<CompilerTypeKind, ClassId> compilerTypeClasses;
  [[nodiscard]] static std::optional<CompilerTypeKind>
  compilerTypeKind(std::string_view binding);
  [[nodiscard]] ClassId compilerTypeClass(CompilerTypeKind kind) const;
  // Rewrites a concrete instance of a computed compiler type to the type it
  // denotes; every other type is returned unchanged.
  [[nodiscard]] SemanticType rewriteCompilerType(SemanticType type) const;
  struct MetaFieldFacts {
    std::string name;
    bool isPublic = false;
    SemanticType type = SemanticType::Unknown;
    const VariableDecl *declaration = nullptr;
  };
  [[nodiscard]] std::uint64_t metaFieldCount(const SemanticType &type) const;
  [[nodiscard]] std::uint64_t metaTypeKind(const SemanticType &type) const;
  [[nodiscard]] std::optional<MetaFieldFacts>
  metaFieldFacts(const SemanticType &owner, std::uint64_t index) const;
  // The element types of a `constexpr for` sequence value, or nullopt when
  // the type is not a class whose generic arguments are one type pack.
  [[nodiscard]] std::optional<std::vector<SemanticType>>
  compileTimeSequenceElements(const SemanticType &type) const;
  // Whether `T()` is a constant the bounded evaluator can produce, which
  // decides whether a `constexpr for` element binding is constexpr.
  [[nodiscard]] bool
  isConstexprDefaultConstructible(const SemanticType &type) const;
  void analyzeMetaIntrinsicCall(const Call &expr,
                                const CallableCandidate &declaration);
  [[nodiscard]] std::optional<ConstantValue>
  metaIntrinsicConstant(const ResolvedCallInfo &call,
                        const ConstexprInterpreter &context) const;
  [[nodiscard]] bool
  metaQueryAcceptsOpenSubject(IntrinsicKind intrinsic,
                              const SemanticType &subject) const;
  [[nodiscard]] std::optional<ConstantValue>
  metaQueryConstant(IntrinsicKind intrinsic, const SemanticType &subject,
                    const std::optional<SemanticType> &attribute,
                    std::optional<std::uint64_t> index,
                    std::optional<std::uint64_t> second) const;
  // Why a concrete reflection query has no constant: {code, message}.
  [[nodiscard]] std::pair<std::string, std::string>
  metaQueryFailure(IntrinsicKind intrinsic, const SemanticType &subject,
                   std::optional<std::uint64_t> index,
                   std::optional<std::uint64_t> second) const;
  struct MetaEnumeratorFacts {
    std::string name;
    ConstantValue value;
    std::vector<SemanticType> payloadTypes;
    std::vector<std::string> payloadNames;
    bool payloadEnum = false;
  };
  // Enumerator `index`, in declaration order, of enum `type` (generic
  // instances substituted), or nullopt when `type` is not an enum or the
  // index is out of range.
  [[nodiscard]] std::optional<MetaEnumeratorFacts>
  metaEnumeratorFacts(const SemanticType &type, std::uint64_t index) const;
  [[nodiscard]] std::uint64_t
  metaEnumeratorCount(const SemanticType &type) const;
  // The expanded type arguments of a class or enum instance.
  [[nodiscard]] std::vector<SemanticType>
  metaTypeArguments(const SemanticType &type) const;
  struct MetaMethodFacts {
    std::string name;
    const FunctionDecl *declaration = nullptr;
    bool staticMember = false;
    bool operatorMethod = false;
    bool constexprFunction = false;
    bool virtualMethod = false;
    bool mutableReceiver = false;
    bool publicAccess = false;
    SemanticType returnType = SemanticType::Unknown;
    std::vector<SemanticType> parameterTypes;
    std::vector<std::string> parameterNames;
  };
  // Method `index` of class `type` in declaration order (operators and
  // static methods included; constructors and destructors excluded), with
  // the instance's substitution applied; nullopt when out of range.
  [[nodiscard]] std::optional<MetaMethodFacts>
  metaMethodFacts(const SemanticType &type, std::uint64_t index) const;
  [[nodiscard]] std::vector<const FunctionDecl *>
  metaMethodDeclarations(const SemanticType &type) const;
  struct MetaConstructorFacts {
    const ConstructorDecl *declaration = nullptr;
    std::vector<SemanticType> parameterTypes;
    std::vector<std::string> parameterNames;
  };
  [[nodiscard]] std::optional<MetaConstructorFacts>
  metaConstructorFacts(const SemanticType &type, std::uint64_t index) const;
  // Source line of a reflected declaration: kind 0 type, 1 field,
  // 2 enumerator, 3 method, 4 constructor; nullopt when unavailable.
  [[nodiscard]] std::optional<std::uint64_t>
  metaDeclarationLine(const SemanticType &type, std::uint64_t kind,
                      std::uint64_t index) const;
  [[nodiscard]] std::string metaDeclarationFile(const SemanticType &type) const;
  // std::meta::construct: the call constructs `T` through its designated
  // constructor from the call's arguments.
  void analyzeMetaConstructCall(const Call &expr);
  // Set while analyzeMetaConstructCall resolves the construction: the
  // designated constructor is the only candidate.
  bool selectingDesignatedConstructor = false;
  // Whether the receiver of a reflected access can be dropped without
  // changing observable behaviour (no call that is not a metaobject default
  // construction).
  [[nodiscard]] bool isEffectFreeMetaReceiver(const Expr &expression) const;
  // Whether `type` is `owner` or publicly derives from it.
  [[nodiscard]] bool isSameOrDerivedClass(const SemanticType &type,
                                          const SemanticType &owner) const;
  void analyzeMetaFieldAccess(const Call &expr, const Get &member,
                              const SemanticType &fieldObject,
                              bool mutableAccess);
  // Set while the synthesized projection of a reflected field access is
  // analysed: a `[[reflect_private]]` owner then admits its private fields.
  bool analyzingReflectedFieldAccess = false;
  // ---- Declared attributes ----
  // Evaluates every `[[ns::name(args)]]` application once per declaration at
  // the start of the body phase, before any body can query it.
  void analyzeDeclarationAttributes();
  void analyzeDeclarationAttributeList(
      const std::vector<DeclarationAttribute> &attributes, const Stmt &owner);
  std::unordered_set<const Stmt *> analyzedDeclarationAttributeOwners;
  // The attributes of class `subject`, or of its instance field `index`;
  // nullptr when `subject` is not a class or the index is out of range, and a
  // pointer to an empty list when the declaration has none.
  [[nodiscard]] const std::vector<ResolvedDeclarationAttribute> *
  declarationAttributesOf(const SemanticType &subject,
                          std::optional<std::uint64_t> index) const;
  const std::vector<ResolvedDeclarationAttribute> noDeclarationAttributes;
  void analyzeMetaFieldDefault(const Call &expr, const Get &member,
                               const SemanticType &fieldObject);
  void analyzeMetaAttributeAccess(const Call &expr, const Get &member,
                                  const SemanticType &metaObject,
                                  bool typeSubject);
  // `attribute<A>().field`: aliases the read to a literal of the field's
  // constant so run-time code carries the constant, not the attribute object.
  [[nodiscard]] bool foldAttributeMemberRead(const Get &expr,
                                             const SemanticType &objectType);
  // A source expression denoting `value` with exact type `type`, or nullptr
  // when the constant has no literal spelling (aggregates).
  [[nodiscard]] ExprPtr constantLiteralExpression(const ConstantValue &value,
                                                  const SemanticType &type,
                                                  const Token &anchor) const;
  // The keyword token naming a builtin scalar type (`uint32_t`, `bool`...),
  // generated at `anchor`; nullopt for every other type.
  [[nodiscard]] static std::optional<Token>
  builtinTypeToken(const SemanticType &type, const Token &anchor);
  // std::meta::reflectable: a struct or class whose instance fields are all
  // public, or one declared [[reflect_private]].
  [[nodiscard]] bool isReflectableType(const SemanticType &type) const;
  // The element binding of the `constexpr for` copy being analysed and the
  // element it is initialized with; visitCallExpr binds the generated
  // initializer to a default construction of that element.
  struct ConstexprForFrame {
    const Call *initializer = nullptr;
    SemanticType element = SemanticType::Unknown;
  };
  std::vector<ConstexprForFrame> constexprForFrames;
  // Expansion ids stamped on the tokens of each expanded copy, and the
  // copies themselves until the model adopts them.
  std::uint32_t nextExpansion = 0;
  std::vector<StmtPtr> expansionArena;
  std::vector<std::shared_ptr<const Expr>> synthesizedExpressions;
  // Deferred statement helpers borrow no closure environment. Each expansion
  // retains definition-site lookup for its body and caller-site lookup for
  // substituted arguments. Flow/loans remain those of the calling body.
  struct MacroLookupContext {
    struct ScopeOverlay {
      std::size_t index = 0;
      std::shared_ptr<const ScopeIdentity> identity;
    };
    std::vector<std::string> namespaceScope;
    SourceUnitId sourceUnit = 0;
    std::optional<ClassId> ownerClass;
    bool staticMember = false;
    bool definition = false;
    // A definition may see only lexical frames created within its expansion.
    // Captured argument syntax is capped at the invocation's lexical depth so
    // later macro-local binders cannot capture same-spelled caller names.
    std::size_t localScopeBegin = 0;
    std::optional<std::size_t> localScopeEnd;
    std::size_t lexicalLambdaDepth = 0;
    // A query can add synthetic locals without exposing intervening macro
    // frames. Identity, not a reused numeric index, owns that permission.
    std::vector<ScopeOverlay> queryScopes;
    std::size_t declarationOrder = 0;
    std::optional<CallerLocationOrigin> caller;
    std::optional<Token> invocation;
    std::unordered_map<std::string, SemanticType> types;
    std::unordered_map<std::string, CompileTimeValue> values;
  };
  std::unordered_map<std::uint32_t, MacroLookupContext> macroLookup;
  // A speculative worker is confined to the lifetime of its caller. Read
  // earlier expansion domains through that caller instead of repeatedly
  // copying the growing domain map; newly expanded syntax stays local.
  const Impl *macroLookupBase = nullptr;
  struct MacroLookupScope {
    Impl &owner;
    std::vector<std::string> namespaceScope;
    SourceUnitId sourceUnit;
    std::optional<ClassId> ownerClass;
    bool staticMember;
    std::size_t declarationOrder;
    MacroLookupScope(Impl &owner, const MacroLookupContext &context);
    ~MacroLookupScope();
  };
  bool macroProbe = false;
  const Call *macroProbeCall = nullptr;
  const Call *macroStatementCall = nullptr;
  std::unordered_set<const Expr *> macroProbeArguments;
  std::size_t macroExpansionDepth = 0;
  std::size_t macroExpandedBodies = 0;
  std::vector<std::pair<const FunctionDecl *, std::size_t>> macroActiveBodies;
  [[nodiscard]] const MacroLookupContext *
  macroContext(const Token &token) const;
  struct MacroLocalScopeRange {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::span<const MacroLookupContext::ScopeOverlay> overlays;

    [[nodiscard]] bool contains(std::size_t index, const Scope &scope) const {
      return (index >= begin && index < end) ||
             std::any_of(overlays.begin(), overlays.end(),
                         [&](const auto &entry) {
                           return entry.index == index && entry.identity &&
                                  entry.identity == scope.identity;
                         });
    }
  };
  [[nodiscard]] MacroLocalScopeRange
  macroLocalScopeRange(const Token &token) const;
  [[nodiscard]] bool analyzeMacroStatement(const ExpressionStmt &statement);
  [[nodiscard]] bool
  analyzeMacroSelection(const Call &call, const CallableCandidate &selected,
                        const std::vector<SemanticType> &typeArguments,
                        const std::vector<CompileTimeValue> &valueArguments);
  [[nodiscard]] bool validMacroBody(const Stmt &statement,
                                    std::size_t ownLoopDepth = 0) const;
  static constexpr std::size_t kConstexprForElementLimit = 1024;
  static constexpr std::size_t kConstexprForBodyLimit = 4096;
  // Bodies expanded by `constexpr for` inside the declaration being analysed.
  std::size_t constexprForExpandedBodies = 0;

  [[nodiscard]] bool isNumericLibraryUnit(SourceUnitId sourceUnit) const;
  [[nodiscard]] bool isSimdLibraryUnit(SourceUnitId sourceUnit) const;

  [[nodiscard]] bool isIteratorLibraryUnit(SourceUnitId sourceUnit) const;

  [[nodiscard]] bool isFunctionalLibraryUnit(SourceUnitId sourceUnit) const;

  [[nodiscard]] bool
  isDefaultLibraryStorageBorrowCarrier(const ClassInfo &owner) const;

  [[nodiscard]] bool
  isDefaultLibraryMutexGuardCarrier(const ClassInfo &owner) const;

  [[nodiscard]] bool isDefaultLibraryStorageBorrowParameter(
      const ClassInfo &owner, const SemanticType &parameterType) const;

  [[nodiscard]] AccessModifier memberAccess(ClassId ownerId,
                                            const Token &declaration) const;

  [[nodiscard]] AccessModifier memberAccess(const Token &declaration) const;

  [[nodiscard]] AccessModifier
  functionAccess(const FunctionInfo *function) const;

  [[nodiscard]] static AccessModifier
  constructorAccess(const ClassInfo &owner, const ConstructorDecl *declaration);

  SymbolId recordToolingSymbol(
      const Token &name, SymbolKind kind, std::string qualified,
      const SemanticType &type, bool mutableBinding = false,
      bool definition = true, AccessModifier access = AccessModifier::Public,
      bool staticMember = false, bool internalLinkage = false,
      bool compilerPrivate = false, bool instanceDependentType = false);

  [[nodiscard]] SymbolId symbolForDeclaration(const Token &name) const;

  [[nodiscard]] static SymbolKind
  bindingSymbolKind(SemanticBindingKind bindingKind);

  SymbolId recordBindingSymbol(
      const Token &name, const SemanticType &type, bool mutableBinding,
      SemanticBindingKind bindingKind, ClassId ownerClass = 0,
      std::string_view resolvedQualifiedName = {}, bool staticMember = false,
      bool internalLinkage = false, bool instanceDependentType = false);

  SymbolId recordFunctionSymbol(const FunctionDecl &declaration);

  SymbolId toolingSymbolFor(const Symbol &symbol);

  [[nodiscard]] const GenericParameterInfo *
  genericParameterInfo(GenericParameterId id) const;

  void recordGenericParameterUse(const Token &use,
                                 const CompileTimeValue &value,
                                 OccurrenceRole role);

  void recordArrayExtentUses(const ArrayExtentExpr &extent);

  void recordTypeUse(const TypeRef &type);

  void recordQualifiedPathUses(const NamePath &path,
                               bool finalSegmentIsNamespace = false);

  SymbolId recordBindingOccurrence(const Token &name, const SemanticType &type,
                                   bool mutableBinding,
                                   SemanticBindingKind bindingKind,
                                   bool staticMember = false,
                                   bool internalLinkage = false,
                                   bool instanceDependentType = false);

  SymbolId recordParameterBinding(const Parameter &parameter,
                                  const SemanticType &type);

  [[nodiscard]] static std::optional<std::size_t>
  genericParameterIndex(std::span<const GenericParameterInfo> parameters,
                        GenericParameterId id);

  [[nodiscard]] static bool
  sameSignatureValue(const CompileTimeValue &left,
                     std::span<const GenericParameterInfo> leftParameters,
                     const CompileTimeValue &right,
                     std::span<const GenericParameterInfo> rightParameters);

  [[nodiscard]] static bool
  sameSignatureType(const SemanticType &left,
                    std::span<const GenericParameterInfo> leftParameters,
                    const SemanticType &right,
                    std::span<const GenericParameterInfo> rightParameters);

  [[nodiscard]] static bool
  sameFunctionSignature(const CallableCandidate &left,
                        const CallableCandidate &right);

  [[nodiscard]] static bool
  isConversionOperator(const CallableCandidate &candidate) {
    return candidate.declaration != nullptr &&
           candidate.declaration->operatorName() &&
           candidate.declaration->operatorName()->kind ==
               OverloadedOperator::Conversion;
  }

  [[nodiscard]] static bool
  sameConstructorSignature(const ConstructorInfo &left,
                           const ConstructorInfo &right);

  bool appendFunctionOverload(Symbol &existing, Symbol incoming,
                              const Token &name);

  bool declare(const Token &name, SemanticType type, bool assignable,
               SemanticBindingKind bindingKind,
               const VariableDecl *variableDeclaration = nullptr,
               const Parameter *parameterDeclaration = nullptr,
               std::optional<ConstantValue> constant = std::nullopt);

  bool declare(const Token &name, Symbol symbol);

  bool
  declareNamespaceSymbol(const std::vector<std::string> &scope,
                         const Token &name, SemanticType type, bool assignable,
                         bool internalLinkage = false,
                         SymbolId toolingSymbol = 0,
                         const VariableDecl *variableDeclaration = nullptr,
                         std::optional<ConstantValue> constant = std::nullopt);

  bool declareNamespaceSymbol(const std::vector<std::string> &scope,
                              const Token &name, Symbol symbol);

  [[nodiscard]] static bool directNamespaceChild(std::string_view qualified,
                                                 std::string_view parent,
                                                 std::string &name);

  [[nodiscard]] SemanticCompletionCandidateKind
  completionKind(const Symbol &symbol) const;

  void appendSymbolCompletions(
      std::vector<SemanticCompletionCandidateRecord> &result,
      const std::string &name, const std::string &qualifiedName,
      const Symbol &symbol, std::size_t scopeDistance,
      bool substitutedCallable = false, bool mutableReceiver = true,
      const SemanticType &receiverType = SemanticType::Unknown) const;

  void appendNamespaceChildren(
      std::vector<SemanticCompletionCandidateRecord> &result,
      const std::string &parent, std::size_t scopeDistance,
      std::unordered_set<std::string> *seen = nullptr) const;

  void captureUnqualifiedCompletion(const Token &completion);

  void captureQualifiedCompletion(const NamePath &path,
                                  const TypeRef *ownerType = nullptr,
                                  bool typeOnly = false);

  void captureMemberCompletion(const Token &completion,
                               const SemanticType &objectType,
                               bool mutableReceiver);

  [[nodiscard]] const Symbol *resolve(const Token &name) const;
  [[nodiscard]] const Symbol *
  resolveGeneratedTestReference(const NamePath &path) const;

  [[nodiscard]] Symbol *resolveMutable(const Token &name);

  [[nodiscard]] Symbol *resolvePlaceSymbol(const Token &name);

  [[nodiscard]] std::vector<ResolvedNamespaceSegment>
  resolveNamespaceSegments(const NamePath &path,
                           const std::vector<std::string> &fromScope) const;

  [[nodiscard]] std::optional<std::string>
  resolveNamespacePath(const NamePath &path,
                       const std::vector<std::string> &fromScope) const;

  [[nodiscard]] std::optional<std::string>
  resolveNamespacePath(const NamePath &path) const;

  [[nodiscard]] const Symbol *resolveQualified(const NamePath &path) const;

  [[nodiscard]] Symbol *resolveQualifiedPlaceSymbol(const NamePath &path);

  // The flow-state carrier of a class static field once body analysis has
  // sealed the catalog; any other symbol is its own carrier.
  [[nodiscard]] Symbol *staticFieldFlowSymbol(Symbol &member);

  [[nodiscard]] std::optional<std::string> resolveInitialNamespaceGlobally(
      const Token &name, const std::vector<std::string> &fromScope) const;

  [[nodiscard]] std::optional<std::string>
  resolveNamespacePathGlobally(const NamePath &path,
                               const std::vector<std::string> &fromScope) const;

  [[nodiscard]] const Symbol *resolveGlobally(const Token &name) const;

  [[nodiscard]] const Symbol *
  resolveQualifiedGlobally(const NamePath &path) const;

  [[nodiscard]] SourceUnitId declarationSourceUnit(const Symbol &symbol) const;

  bool reportInvisibleDeclaration(const Token &use, std::string name,
                                  const Token &declaration,
                                  SourceUnitId declarationUnit);

  bool reportInvisibleSymbol(const Token &use, std::string name,
                             const Symbol *symbol);

  [[nodiscard]] std::optional<ResolvedTypeAlias>
  resolveTypeAliasPath(const NamePath &path,
                       const std::vector<std::string> &fromScope) const;

  [[nodiscard]] std::optional<ResolvedTypeAlias>
  resolveMemberTypeAlias(ClassId owner, const std::string &name) const;

  [[nodiscard]] std::optional<ResolvedTypeAlias>
  resolveTypeAliasPath(const TypeRef &type,
                       const std::vector<std::string> &fromScope) const;

  [[nodiscard]] std::optional<TypeAliasId>
  resolveTypeAliasPathGlobally(const NamePath &path,
                               const std::vector<std::string> &fromScope) const;

  [[nodiscard]] bool validateTypeAliasAccess(const ResolvedTypeAlias &alias,
                                             const Token &use);

  [[nodiscard]] std::optional<ConceptId>
  resolveConceptPath(const NamePath &path,
                     const std::vector<std::string> &fromScope) const;

  [[nodiscard]] std::optional<ConceptId>
  resolveConceptPathGlobally(const NamePath &path,
                             const std::vector<std::string> &fromScope) const;

  [[nodiscard]] std::optional<ClassId>
  resolveClassPath(const NamePath &path,
                   const std::vector<std::string> &fromScope) const;

  [[nodiscard]] std::optional<ClassId>
  resolveClassPathGlobally(const NamePath &path,
                           const std::vector<std::string> &fromScope) const;

  [[nodiscard]] std::optional<EnumId>
  resolveEnumPath(const NamePath &path,
                  const std::vector<std::string> &fromScope) const;

  [[nodiscard]] std::optional<EnumId>
  resolveEnumPathGlobally(const NamePath &path,
                          const std::vector<std::string> &fromScope) const;

  [[nodiscard]] const EnumeratorRecord *
  resolveEnumerator(const NamePath &path, bool global = false) const;
  [[nodiscard]] std::optional<EnumeratorRecord>
  resolveEnumerator(const QualifiedName &expression) const;

  [[nodiscard]] std::optional<EnumeratorRecord>
  payloadEnumeratorForCallee(const ExprPtr &callee) const;

  void analyzePayloadConstruction(const Call &call,
                                  const EnumeratorRecord &enumerator);

  // `consumingSubject` is true when the switch consumes its subject (a
  // prvalue or an explicit std::move); by-value bindings then move their
  // fields out instead of copying them.
  [[nodiscard]] std::optional<ResolvedPayloadPatternInfo>
  analyzePayloadSwitchPattern(const ExprPtr &expression,
                              const SemanticType &subjectType,
                              bool consumingSubject);

  [[nodiscard]] std::optional<Symbol>
  resolveExpressionSymbol(const ExprPtr &expression) const;

  [[nodiscard]] bool isMutableTarget(const ExprPtr &expression) const;

  [[nodiscard]] bool isMutableObject(const ExprPtr &expression) const;

  [[nodiscard]] static bool isThisObject(const ExprPtr &expression);

  [[nodiscard]] bool receiverRestrictionActive() const;

  [[nodiscard]] std::optional<ResolvedFieldUse>
  receiverRestrictedField(const Token &use, const Symbol &symbol) const;

  [[nodiscard]] std::optional<ResolvedFieldUse>
  receiverRestrictedField(const ExprPtr &expression) const;

  [[nodiscard]] std::optional<ResolvedFieldUse>
  receiverRestrictedMemberRoot(const ExprPtr &object, const Token &memberUse,
                               const Symbol &member) const;

  void appendReceiverRestrictionContext(Diagnostic &diagnostic,
                                        const ResolvedFieldUse &root) const;

  bool reportReceiverRestrictedMutation(const Token &writtenField,
                                        const ResolvedFieldUse &root);

  [[nodiscard]] static const Token *
  receiverRestrictedWrittenField(const ExprPtr &expression);

  bool reportReceiverRestrictedMutation(const ExprPtr &targetExpression);

  bool reportReceiverRestrictedReference(const ExprPtr &initializer);

  [[nodiscard]] SemanticType memberAccessObjectType(const Get &member) const;

  [[nodiscard]] SemanticType memberAccessObjectType(const Set &member) const;

  [[nodiscard]] bool memberReceiverIsMutable(const Get &member) const;

  enum class FreshTemporaryValueFailure {
    None,
    NotClassValue,
    ExplicitMove,
    NonConcreteRecord,
    BorrowedState,
    UnstableBorrowOwner,
    UnsupportedProducer,
  };

  struct FreshTemporaryValueAssessment {
    bool eligible = false;
    FreshTemporaryValueFailure failure =
        FreshTemporaryValueFailure::NotClassValue;
    SemanticType type = SemanticType::Unknown;
  };

  [[nodiscard]] const Expr *
  transparentTemporaryProducer(const ExprPtr &expression) const;

  // Whether `type` is an enum with payload alternatives: a self-contained
  // aggregate value like a class or struct, unlike a scalar-backed enum.
  [[nodiscard]] bool payloadEnumType(const SemanticType &type) const;

  // A fresh self-contained class, struct or payload-enum value that one call
  // may borrow transiently, as opposed to an existing place or a value that
  // carries borrowed state.
  [[nodiscard]] FreshTemporaryValueAssessment
  freshTemporaryValue(const ExprPtr &expression) const;

  // Receiver-only extension. Temporary reference arguments retain the stricter
  // self-contained-value rule in freshTemporaryValue.
  [[nodiscard]] FreshTemporaryValueAssessment
  freshTemporaryReceiverValue(const ExprPtr &expression);

  [[nodiscard]] bool
  freshTemporaryMethodSupported(const CallableCandidate &function) const;
  // A read-only method that returns one direct text field of its receiver
  // (L-OWN-02).
  [[nodiscard]] bool
  receiverTextFieldRelay(const CallableCandidate &function) const;

  [[nodiscard]] CallReceiverMode receiverModeForObject(
      const ExprPtr &receiver, const CallableCandidate &function,
      bool allowFreshTemporary, bool consumingReceiver = false);
  [[nodiscard]] bool consumingReceiverValue(const ExprPtr &receiver) const;

  [[nodiscard]] CallReceiverMode
  callReceiverMode(const ExprPtr &callee, const CallableCandidate &function);

  [[nodiscard]] static bool receiverModeAllowsMutation(CallReceiverMode mode);

  void reportMutableReceiverFailure(const ExprPtr &receiver,
                                    const Token &location,
                                    const CallableCandidate &function,
                                    std::string_view operation);

  [[nodiscard]] const ClassInfo *classInfo(const SemanticType &type) const;

  [[nodiscard]] bool isDefaultLibraryClass(const SemanticType &type,
                                           std::string_view expectedName) const;

  [[nodiscard]] std::optional<FunctionId>
  ownedProgramEntryAppendFunction(const FunctionDecl &declaration,
                                  const FunctionInfo &function) const;

  [[nodiscard]] ClassInfo &classInfo(ClassId id);

  [[nodiscard]] const ClassInfo &classInfo(ClassId id) const;

  // The contextual use of `expression`'s result, when its enclosing
  // expression or statement supplied one.
  [[nodiscard]] std::optional<ResultUse>
  resultUseFor(const Expr &expression) const;
  // The access a result of `type` needs under `use`: projections through
  // fields keep the outer use, a member or operator whose every overload has
  // one receiver capability decides it, and an accessor pair defers to its
  // own use. Unknown when a step cannot be resolved.
  [[nodiscard]] ResultUseAccess accessorResultUse(const ResultUse &use,
                                                  SemanticType type) const;
  // True when `operation` on `receiver` is an accessor pair whose result
  // `use` only reads.
  [[nodiscard]] bool
  readOnlyAccessorOperator(const std::optional<ResultUse> &use,
                           const SemanticType &receiver,
                           OverloadedOperator operation) const;
  // Whether `expression` can be an accessor's result or pass its own use on
  // to one. Only these forms read a contextual use.
  [[nodiscard]] static bool consultsResultUse(const Expr *expression);
  // Sets the use of one child expression for the scope's lifetime.
  class ResultUseScope {
  public:
    ResultUseScope(Impl &impl, const Expr *expression,
                   std::vector<ResultUseStep> steps, ResultUseAccess top)
        : impl_(impl), active_(consultsResultUse(expression)) {
      if (active_)
        set(expression, std::move(steps), top);
    }
    // A use passed through unchanged, as by a grouping or a conditional arm.
    ResultUseScope(Impl &impl, const Expr *expression,
                   const std::optional<ResultUse> &use)
        : impl_(impl), active_(use && consultsResultUse(expression)) {
      if (active_)
        set(expression, use->steps, use->top);
    }
    ~ResultUseScope() {
      if (active_)
        impl_.contextualResultUse = std::move(saved_);
    }
    ResultUseScope(const ResultUseScope &) = delete;
    ResultUseScope &operator=(const ResultUseScope &) = delete;

  private:
    void set(const Expr *expression, std::vector<ResultUseStep> steps,
             ResultUseAccess top) {
      saved_ = std::move(impl_.contextualResultUse);
      impl_.contextualResultUse = ResultUse{
          .expression = expression, .steps = std::move(steps), .top = top};
    }

    Impl &impl_;
    bool active_ = false;
    std::optional<ResultUse> saved_;
  };
  // A child's use after one more projection of the parent's result.
  [[nodiscard]] static std::vector<ResultUseStep>
  prependResultUseStep(ResultUseStep step,
                       const std::optional<ResultUse> &parent);
  [[nodiscard]] static ResultUseStep
  operatorResultUseStep(OverloadedOperator operation) {
    return {.kind = ResultUseStep::Kind::Operator, .operation = operation};
  }
  [[nodiscard]] static ResultUseStep
  bindResultUseStep(std::vector<SemanticType> targets) {
    return {.kind = ResultUseStep::Kind::Bind, .targets = std::move(targets)};
  }
  // The Bind step for each argument of `expr`, from the parameters at its
  // position in every candidate of the callee; none when they are not known.
  [[nodiscard]] std::vector<std::optional<ResultUseStep>>
  argumentResultUses(const Call &expr, const SemanticType &calleeType,
                     const std::optional<Symbol> &callee) const;

  [[nodiscard]] const MemberInfo *findMember(const SemanticType &objectType,
                                             const Token &name) const;

  [[nodiscard]] const MemberInfo *resolveMember(const SemanticType &objectType,
                                                const Token &name);

  [[nodiscard]] std::string typeSpelling(const SemanticType &type) const;

  [[nodiscard]] bool tracksValueState(const Symbol &symbol) const;

  static constexpr std::string_view receiverStateName = "__gti_receiver_state";

  void declareReceiverState();

  [[nodiscard]] Symbol *receiverStateSymbol();

  [[nodiscard]] SemanticPlace placeForSymbol(Symbol &symbol);

  [[nodiscard]] std::optional<PlaceProjection>
  fixedArrayIndexProjection(const SemanticType &objectType,
                            const ExprPtr &index, const Expr &selectionSource);

  [[nodiscard]] PlaceKey placeKey(const SemanticPlace &place) const;

  [[nodiscard]] SemanticPlace semanticPlace(const Expr *expression);

  [[nodiscard]] SemanticPlace semanticPlace(const ExprPtr &expression);

  [[nodiscard]] SemanticPlace semanticPlace(const Set &expression);

  [[nodiscard]] SemanticPlace semanticPlace(const IndexSet &expression);

  [[nodiscard]] PlaceKey
  projectedPlaceKey(const SemanticPlace &place,
                    std::span<const PlaceProjection> projections) const;

  [[nodiscard]] static ValueState
  projectedValueState(const Symbol &root,
                      std::span<const PlaceProjection> projections);

  [[nodiscard]] const ProjectedValueState *
  unavailableProjectedState(const SemanticPlace &place,
                            bool includeIndirectDescendants = false) const;

  [[nodiscard]] const ProjectedValueState *
  unavailableAncestorState(const SemanticPlace &place) const;

  [[nodiscard]] bool samePlace(const SemanticPlace &left,
                               const SemanticPlace &right) const;

  [[nodiscard]] SemanticPlace movedSourcePlace(const ExprPtr &expression);

  void setProjectedValueState(const SemanticPlace &place, ValueState state,
                              const Token &origin);
  static std::vector<SourceSpan>
  mergedMoveOrigins(const std::vector<SourceSpan> &left,
                    const std::vector<SourceSpan> &right);
  static void appendMoveContext(Diagnostic &diagnostic,
                                const std::vector<SourceSpan> &origins);
  void appendReadOnlyPlaceContext(Diagnostic &diagnostic,
                                  const ExprPtr &expression) const;

  void reinitializePlace(const SemanticPlace &place);

  void reportUnavailablePlace(const Token &use,
                              const ProjectedValueState &state,
                              const SemanticPlace *place = nullptr);

  void validateProjectedPlaceRead(const Expr &expression);

  SemanticType analyzeProjectionBase(const ExprPtr &expression);

  void requireInitializedReceiver(const Token &location);

  [[nodiscard]] bool packRequiresMove(const SemanticType &type) const;

  void reportUnavailableValue(const Token &use, const Symbol &symbol);

  [[nodiscard]] static ValueState mergedValueState(ValueState left,
                                                   ValueState right);

  [[nodiscard]] static std::vector<ProjectedValueState>
  mergedProjectedValueStates(const Symbol &left, const Symbol &right);

  void mergeValueStates(const ScopeStack &base, const ScopeStack &left,
                        const ScopeStack &right);

  [[nodiscard]] ScopeStack
  mergedValueStates(const ScopeStack &base,
                    const std::vector<ScopeStack> &states);

  void applyMoveStates(const ScopeStack &source);

  // Replaces the current scope state with `source` without reallocating the
  // Symbol of any binding present in both. An enclosing expression may hold a
  // SemanticPlace (whose root points into `scopes`) across the analysis of a
  // nested expression, so expression-level flow joins restore through this
  // rather than assigning or moving a whole ScopeStack into `scopes`.
  void restoreScopeState(const ScopeStack &source);

  void beginValueControlFlow(ValueControlFlowKind kind, const Stmt &statement);

  [[nodiscard]] ValueControlFlowContext endValueControlFlow();

  void recordValueControlFlowExit(bool isBreak);

  [[nodiscard]] static std::vector<ValueControlFlowContext>
  mergedValueControlFlow(const std::vector<ValueControlFlowContext> &base,
                         const std::vector<ValueControlFlowContext> &left,
                         const std::vector<ValueControlFlowContext> &right);

  void validateLoopBackedge(const ScopeStack &beforeLoop,
                            const ScopeStack &backedge, const Token &location);

  void beginScope();
  void endScope();

  void report(const Token &token, std::string message,
              std::string code = "GTI-S2000");

  void requireUnsafe(const Expr &expression, UnsafeOperationKind operation,
                     const Token &token, std::string_view description);
  void enterUnsafeRegion(const Token &keyword, bool expression);
  void leaveUnsafeRegion();

  [[nodiscard]] std::vector<RawPointerOrigin>
  rawPointerOrigins(const ExprPtr &expression);
  [[nodiscard]] std::vector<RawPointerOrigin>
  rawPointerOrigins(const Expr *expression);
  [[nodiscard]] std::vector<ProjectedRawPointerOrigins>
  rawPointerFields(const Expr *expression);
  [[nodiscard]] std::vector<ProjectedRawPointerOrigins>
  rawPointerFieldsAt(const PlaceKey &place);
  [[nodiscard]] std::vector<std::vector<PlaceProjection>>
  rawPointerFieldPaths(const SemanticType &type);
  [[nodiscard]] std::vector<SemanticType>
  rawPointerTypesAt(const SemanticType &type,
                    const std::vector<PlaceProjection> &projections);
  [[nodiscard]] std::vector<RawPointerOrigin>
  rawCallPointerOrigins(const Expr &expression, bool includeProduced = true);
  [[nodiscard]] std::vector<ProjectedRawPointerOrigins>
  rawProducedFields(const Expr &expression,
                    const std::vector<RawResultField> *fields = nullptr,
                    const RawInvocation *invocation = nullptr,
                    const ResolvedConstructionInfo *contextual = nullptr);
  [[nodiscard]] std::shared_ptr<RawResultQuery>
  rawResultQuery(const Call &call, const ResolvedCallInfo &resolved);
  [[nodiscard]] std::shared_ptr<RawResultQuery>
  rawResultQuery(const ResolvedCallInfo &resolved, SemanticType owner);
  [[nodiscard]] std::shared_ptr<RawResultQuery>
  rawConstructorResultQuery(const ResolvedConstructionInfo &resolved);
  void runRawResultQuery(const std::shared_ptr<RawResultQuery> &query);
  void recordRawResult(const ExprPtr &expression);
  void recordEmptyReturnRawResult(const ReturnStmt &statement);
  void recordRawConstructorResult(const ConstructorDecl &constructor);
  void
  collectRawResultFields(const std::vector<ProjectedRawPointerOrigins> &fields,
                         const std::vector<Parameter> &parameters,
                         const std::vector<SemanticType> &parameterTypes,
                         bool constructedReceiver,
                         std::vector<RawResultField> *output = nullptr);
  std::vector<RawProgramStorageState> saveRawProgramStorage();
  void restoreRawProgramStorage(std::vector<RawProgramStorageState> state);
  void recordRawWrite(const SemanticPlace &place,
                      const std::vector<ProjectedRawPointerOrigins> &fields,
                      bool strong);
  void applyRawCallWrites(const Expr &expression,
                          const RawInvocation *invocation = nullptr);
  void applyUnknownRawWrite(const SemanticPlace &place,
                            const SemanticType &type, const SourceSpan &span,
                            std::size_t query = 0);
  void applyRawCleanupWrites(
      const SemanticType &type, const SourceSpan &span,
      const std::vector<ProjectedRawPointerOrigins> &fields = {});
  void applyRawAliasWrites(std::vector<ProjectedRawPointerOrigins> fields,
                           const SemanticType &type, const SourceSpan &span);
  void
  applyRawPointerFields(const SemanticPlace &place,
                        const std::vector<ProjectedRawPointerOrigins> &fields,
                        bool strong);
  static bool rawWritesAreLinear(const StmtList &statements);
  [[nodiscard]] Symbol *rawPointerRoot(const PlaceKey &place);
  // Replaces program storage's raw facts with symbolic inputs, so a cached
  // source-result summary describes the storage rather than one caller's
  // current value.
  void seedRawStorageInput(Symbol &symbol);
  void preserveRawCallWrites(const Call &call);
  void invalidateRawCallOrigins(const Call *call,
                                const RawInvocation *invocation = nullptr);
  void
  applyRawPointerLifetimeInvalidations(RawPointerOrigin &origin,
                                       const std::vector<PlaceKey> &places);
  void assignRawPointerFields(const SemanticPlace &place, const Expr *value);
  std::vector<SemanticPlace> rawPointerStoragePlaces(const SemanticPlace &place,
                                                     bool &weak);
  void invalidateRawPointerOrigins(const SemanticPlace &place);
  static void
  mergeRawPointerFields(std::vector<ProjectedRawPointerOrigins> &target,
                        const std::vector<ProjectedRawPointerOrigins> &source);
  static void
  mergeRawPointerOrigins(std::vector<RawPointerOrigin> &target,
                         const std::vector<RawPointerOrigin> &source);
  void validateRawPointerAccess(const ExprPtr &pointer, const Token &site,
                                AccessMode access);
  void validateRawPointerOrigins(const std::vector<RawPointerOrigin> &origins,
                                 const Token &site, AccessMode access,
                                 bool checksLoans = true);
  void recordRawAccess(const std::vector<RawPointerOrigin> &origins,
                       const Token &site, AccessMode access, bool checksLoans);
  void validateRawCallAccesses(const Expr &expression,
                               const RawInvocation *invocation = nullptr);
  void applyRawOperatorInvocation(const Expr &expression,
                                  const RawInvocation &invocation,
                                  bool contextual, bool projection);
  [[nodiscard]] bool rawDispatchAccessFree(const ResolvedCallInfo &resolved,
                                           const SemanticPlace &receiver);
  void beginRawPointerLoop();
  void endRawPointerLoop(const ScopeStack &backedge, bool reachable);
  std::shared_ptr<RawPointerLoopUse>
  deferredRawPointerLoopUse(const ExprPtr &pointer, const Token &site);
  std::shared_ptr<RawPointerLoopUse>
  deferredRawPointerLoopUse(const std::vector<RawPointerOrigin> &origins,
                            const Token &site);
  [[nodiscard]] std::optional<Diagnostic>
  rawPointerLifetimeDiagnostic(const RawPointerOrigin &origin,
                               const Token &site);
  void publishRawLifetimeDiagnostic(const Diagnostic &diagnostic);

  [[nodiscard]] static bool containsRawPointer(const SemanticType &type);

  void requireBool(const ExprPtr &expression, SemanticType type,
                   const Token &token, std::string_view message);

  [[nodiscard]] static bool
  acceptsContextualIntegerOperand(TokenKind operation);

  [[nodiscard]] static const LiteralExpr *
  contextualIntegerLiteral(const Expr *expression, bool allowSign = true);

  [[nodiscard]] static bool isContextualIntegerLiteral(const Expr *expression);

  SemanticType analyzeContextualIntegerOperand(const ExprPtr &expression,
                                               SemanticType target);

  [[nodiscard]] std::pair<SemanticType, SemanticType>
  analyzeBinaryOperands(const Binary &expression);

  void requireNumeric(SemanticType left, SemanticType right,
                      const Token &token);

  void requireOrdered(SemanticType left, SemanticType right,
                      const Token &token);

  void requireInteger(SemanticType left, SemanticType right,
                      const Token &token);

  void validateIntegerDivisor(const Token &operation, const Expr *right);

  void validateCompoundAssignment(const Token &operation, SemanticType target,
                                  SemanticType value,
                                  const Expr *valueExpression);

  void validateShiftCount(SemanticType result, const Expr *right,
                          const Token &token);

  [[nodiscard]] static bool isInteger(SemanticType type);

  [[nodiscard]] static bool constraintImplies(GenericConstraintSet actual,
                                              GenericConstraintKind required);
  [[nodiscard]] static GenericConstraintSet
  impliedConstraints(GenericConstraintSet constraints);

  [[nodiscard]] GenericConstraintSet
  constraintOf(const SemanticType &type) const;

  [[nodiscard]] bool isNumeric(SemanticType type) const;

  [[nodiscard]] bool isOrdered(SemanticType type) const;

  [[nodiscard]] bool isIntegral(SemanticType type) const;

  [[nodiscard]] bool isSignedNumeric(SemanticType type) const;

  [[nodiscard]] static bool isSignedInteger(SemanticType type);

  [[nodiscard]] static bool isUnsignedInteger(SemanticType type);

  [[nodiscard]] static int integerRank(SemanticType type);

  struct IntegerConstant {
    bool negative = false;
    std::uint64_t magnitude = 0;
  };

  [[nodiscard]] static bool integerFits(SemanticType type,
                                        IntegerConstant value);

  [[nodiscard]] static std::optional<IntegerConstant>
  integerConstant(const Expr *expression);

  [[nodiscard]] std::optional<SwitchCaseValue>
  switchCaseConstant(const Expr *expression,
                     const SemanticType &subjectType) const;

  [[nodiscard]] static bool integerRangeFits(SemanticType target,
                                             SemanticType value);

  [[nodiscard]] static SemanticType promotedInteger(SemanticType type);

  [[nodiscard]] SemanticType widerInteger(SemanticType left,
                                          SemanticType right) const;

  [[nodiscard]] static bool canConvertToUnsigned(SemanticType originalType,
                                                 const Expr *expression,
                                                 SemanticType unsignedTarget);

  [[nodiscard]] static bool isContextuallyBool(const SemanticType &type);

  [[nodiscard]] SemanticType numericResult(SemanticType left,
                                           SemanticType right,
                                           const Expr *leftExpression,
                                           const Expr *rightExpression) const;

  [[nodiscard]] bool isOwnershipAssignable(const SemanticType &target,
                                           const SemanticType &value,
                                           const ExprPtr &expression) const;

  [[nodiscard]] bool isOwnershipAssignment(const SemanticType &target,
                                           const SemanticType &value,
                                           const ExprPtr &expression,
                                           const SemanticPlace &destination);

  [[nodiscard]] static bool isAssignable(SemanticType target,
                                         SemanticType value,
                                         const Expr *expression = nullptr);

  [[nodiscard]] bool isComparable(SemanticType left, SemanticType right,
                                  const Expr *leftExpression,
                                  const Expr *rightExpression) const;

  [[nodiscard]] SemanticType
  baseTypeOf(const TypeRef &type,
             const std::vector<std::string> &fromScope) const;

  [[nodiscard]] SemanticType
  typeOf(const TypeRef &type, const std::vector<std::string> &fromScope) const;

  [[nodiscard]] SemanticType typeOf(const TypeRef &type) const;

  [[nodiscard]] SemanticType
  typeOf(const TypeRef &type, Mutability mutability,
         const std::vector<std::string> &fromScope) const;

  [[nodiscard]] SemanticType typeOf(const TypeRef &type,
                                    Mutability mutability) const;

  [[nodiscard]] SemanticType
  typeOf(const Parameter &parameter,
         const std::vector<std::string> &fromScope) const;

  [[nodiscard]] SemanticType typeOf(const Parameter &parameter) const;

  [[nodiscard]] static SemanticType literalType(const Literal &literal);

  [[nodiscard]] static Token expressionToken(const Expr &expression);

  [[nodiscard]] static Token expressionToken(const ExprPtr &expr);

  [[nodiscard]] static Token callableToken(const ExprPtr &callee);

  // Registration owns declaration identities and lookup indexes. Body and
  // concrete-instance analysis share this one request-local catalog; they do
  // not rebuild declaration authority in a second table. The catalog is
  // sealed after the ordered registration prepasses complete.
  struct SemanticCatalog {
    std::unordered_set<std::string> namespaces;
    std::unordered_map<std::string, SymbolId> namespaceToolingSymbols;
    std::unordered_map<std::string, NamespaceAliasInfo> namespaceAliases;
    SymbolId compilerPrivateNamespaceSymbol = 0;
    std::unordered_map<std::string, TypeAliasId> typeAliasIds;
    std::vector<RegisteredTypeAlias> typeAliases;
    std::unordered_map<std::string, ConceptId> conceptIds;
    std::vector<RegisteredConcept> concepts;
    std::unordered_map<std::string, Symbol> namespaceSymbols;
    std::unordered_map<SourceUnitId, std::unordered_map<std::string, Symbol>>
        internalNamespaceSymbols;
    std::unordered_map<std::string, ClassId> classIds;
    std::unordered_map<std::string, EnumId> enumIds;
    std::unordered_map<SourceUnitId, std::unordered_set<std::string>>
        visibleNamespaces;
    std::unordered_map<SourceUnitId,
                       std::unordered_map<std::string, NamespaceAliasInfo>>
        visibleNamespaceAliases;
    std::unordered_map<SourceUnitId,
                       std::unordered_map<std::string, TypeAliasId>>
        visibleTypeAliasIds;
    std::unordered_map<SourceUnitId, std::unordered_map<std::string, ConceptId>>
        visibleConceptIds;
    std::unordered_map<SourceUnitId, std::unordered_map<std::string, Symbol>>
        visibleNamespaceSymbols;
    std::unordered_map<SourceUnitId, std::unordered_map<std::string, ClassId>>
        visibleClassIds;
    std::unordered_map<SourceUnitId, std::unordered_map<std::string, EnumId>>
        visibleEnumIds;
    std::unordered_map<const ClassDecl *, ClassId> classDeclIds;
    std::unordered_map<const ConceptDecl *, ConceptId> conceptDeclIds;
    std::unordered_map<const FunctionDecl *, std::vector<GenericParameterInfo>>
        functionGenericParameters;
    std::unordered_map<std::string, std::vector<ConstraintPredicateDeclaration>>
        constraintPredicateDeclarations;
    std::unordered_map<const FunctionDecl *, CallableDeclaration *>
        callableDeclarationsBySyntax;
    std::vector<std::unique_ptr<CallableDeclaration>> callableDeclarations;
    std::unordered_map<const ConstructorDecl *,
                       std::vector<GenericParameterInfo>>
        constructorGenericParameters;
    std::unordered_map<std::string, const VariableDecl *>
        rootNativeStorageSymbols;
    std::unordered_map<std::string, std::vector<GlobalBorrowStorageInfo>>
        globalBorrowStorage;
    std::unordered_map<GenericParameterId, GenericConstraintSet>
        genericConstraints;
    std::unordered_map<GenericParameterId, ConceptId> genericConstraintConcepts;
    std::vector<ClassInfo> classes;
    std::vector<ExactClassSpecialization> exactClassSpecializations;
    std::vector<RegisteredConformance> conformances;
    std::vector<EnumInfo> enums;
    std::unordered_set<const VariableDecl *> predeclaredVariables;
    std::unordered_map<const VariableDecl *, const Token *>
        mutableFieldGroupOrigins;
    std::unordered_set<const VariableDecl *> recursiveLayoutFields;
    GenericParameterId nextGenericParameterId = 1;
    ConstructorId nextConstructorId = 1;
    std::size_t nextDefaultArgumentEffectOwner = 1;
    FunctionId nextFunctionId = 1;
    bool sealed = false;
    std::vector<std::size_t> sealedShape;

    SemanticCatalog() = default;
    SemanticCatalog(SemanticCatalog &&) noexcept = default;
    SemanticCatalog &operator=(SemanticCatalog &&) noexcept = default;
    // A copy owns its own callable declarations and re-points
    // callableDeclarationsBySyntax at them; everything else is value state.
    SemanticCatalog(const SemanticCatalog &other);
    SemanticCatalog &operator=(const SemanticCatalog &other);

    void clear() { *this = SemanticCatalog{}; }
    [[nodiscard]] SemanticCatalog snapshotForInstanceAnalysis() const;
    void seal() {
      assert(!sealed);
      sealedShape = shape();
      sealed = true;
    }

    void assertUnchangedSinceSeal() const {
      assert(sealed);
      assert(shape() == sealedShape);
    }

    [[nodiscard]] CallableDeclaration &
    registerCallable(CallableDeclaration declaration) {
      assert(!sealed);
      auto owned =
          std::make_unique<CallableDeclaration>(std::move(declaration));
      CallableDeclaration &result = *owned;
      callableDeclarationsBySyntax.insert_or_assign(result.syntax, &result);
      callableDeclarations.emplace_back(std::move(owned));
      return result;
    }

    [[nodiscard]] CallableDeclaration *
    mutableCallable(const FunctionDecl &syntax) {
      assert(!sealed);
      const auto found = callableDeclarationsBySyntax.find(&syntax);
      return found == callableDeclarationsBySyntax.end() ? nullptr
                                                         : found->second;
    }

    [[nodiscard]] const CallableDeclaration *
    callable(const FunctionDecl &syntax) const {
      const auto found = callableDeclarationsBySyntax.find(&syntax);
      return found == callableDeclarationsBySyntax.end() ? nullptr
                                                         : found->second;
    }

  private:
    [[nodiscard]] std::vector<std::size_t> shape() const {
      return {namespaces.size(),
              namespaceToolingSymbols.size(),
              namespaceAliases.size(),
              compilerPrivateNamespaceSymbol,
              typeAliasIds.size(),
              typeAliases.size(),
              conceptIds.size(),
              concepts.size(),
              namespaceSymbols.size(),
              internalNamespaceSymbols.size(),
              classIds.size(),
              enumIds.size(),
              visibleNamespaces.size(),
              visibleNamespaceAliases.size(),
              visibleTypeAliasIds.size(),
              visibleConceptIds.size(),
              visibleNamespaceSymbols.size(),
              visibleClassIds.size(),
              visibleEnumIds.size(),
              classDeclIds.size(),
              conceptDeclIds.size(),
              functionGenericParameters.size(),
              constraintPredicateDeclarations.size(),
              callableDeclarationsBySyntax.size(),
              callableDeclarations.size(),
              constructorGenericParameters.size(),
              rootNativeStorageSymbols.size(),
              globalBorrowStorage.size(),
              genericConstraints.size(),
              genericConstraintConcepts.size(),
              classes.size(),
              exactClassSpecializations.size(),
              conformances.size(),
              enums.size(),
              predeclaredVariables.size(),
              mutableFieldGroupOrigins.size(),
              recursiveLayoutFields.size(),
              nextGenericParameterId,
              nextConstructorId,
              nextDefaultArgumentEffectOwner,
              nextFunctionId};
    }
  };

  // Initialization/move state lives in lexical scopes; loop/switch snapshots
  // and their joins are part of the same body-scoped value-flow owner.
  struct ValueFlowState {
    ScopeStack scopes;
    std::unordered_map<const Expr *, std::vector<ProjectedRawPointerOrigins>>
        rawOperatorResults;
    std::vector<ValueControlFlowContext> controlFlow;
    std::vector<RawPointerLoop> rawPointerLoops;
    std::size_t nextRawPointerLoop = 1;
    std::unordered_map<std::string, Symbol> namespaceSymbols;
    std::unordered_map<SourceUnitId, std::unordered_map<std::string, Symbol>>
        internalNamespaceSymbols;
    std::unordered_map<SourceUnitId, std::unordered_map<std::string, Symbol>>
        visibleNamespaceSymbols;
    // Class static fields are program storage like namespace globals, but
    // their declarations live in the sealed class catalog. Body analysis
    // keeps their flow facts here, keyed by declaration as their place
    // identity is, so writes never mutate the catalog and later reads,
    // summaries and call writes see them.
    std::unordered_map<const VariableDecl *, Symbol> staticFieldSymbols;

    void clear() {
      scopes.clear();
      rawOperatorResults.clear();
      controlFlow.clear();
      rawPointerLoops.clear();
      nextRawPointerLoop = 1;
      namespaceSymbols.clear();
      internalNamespaceSymbols.clear();
      visibleNamespaceSymbols.clear();
      staticFieldSymbols.clear();
    }

    void beginBody(const SemanticCatalog &catalog) {
      namespaceSymbols = catalog.namespaceSymbols;
      internalNamespaceSymbols = catalog.internalNamespaceSymbols;
      visibleNamespaceSymbols = catalog.visibleNamespaceSymbols;
      staticFieldSymbols.clear();
      extendBody(catalog);
    }

    // Adds the catalog's symbols a body phase does not know yet, keeping
    // the ones an earlier body phase declared or refined (globals from the
    // trusted prefix a resumed analysis adopted; see checkResumed).
    void extendBody(const SemanticCatalog &catalog) {
      for (const auto &[name, symbol] : catalog.namespaceSymbols)
        namespaceSymbols.try_emplace(name, symbol);
      for (const auto &[unit, symbols] : catalog.internalNamespaceSymbols)
        for (const auto &[name, symbol] : symbols)
          internalNamespaceSymbols[unit].try_emplace(name, symbol);
      for (const auto &[unit, symbols] : catalog.visibleNamespaceSymbols)
        for (const auto &[name, symbol] : symbols)
          visibleNamespaceSymbols[unit].try_emplace(name, symbol);
      for (const auto &owner : catalog.classes)
        for (const auto &[_, member] : owner.members)
          if (member.symbol.bindingKind == SemanticBindingKind::StaticField &&
              member.symbol.variableDeclaration != nullptr)
            staticFieldSymbols.try_emplace(member.symbol.variableDeclaration,
                                           member.symbol);
    }
  };

  // One public check() owns one complete request session. The references below
  // are a migration seam that keeps the analyzer algorithms readable while
  // R-STRUCT-02 moves catalog and flow algorithms behind narrower services.
  // They do not introduce another owner: every binding refers into this one
  // object, and resetRequestState() is its only public-entry reset operation.
  // Mutable request context can be forked independently of the persistent
  // semantic facts. Speculative expression checks use a model delta rather
  // than copying every earlier expression in the caller's program.
  struct RequestSessionContext {
    struct OwnerlessTextCallUse {
      const FunctionDecl *caller = nullptr;
      FunctionId callee = 0;
      std::size_t parameter = 0;
      SemanticType type = SemanticType::Unknown;
      std::optional<std::size_t> forwardedParameter;
      // The forwarded formal's type in the analysis that recorded this use.
      // A concrete instance evaluates only uses recorded against its own
      // signature, not those of the open body or a sibling instance.
      SemanticType callerFormal = SemanticType::Unknown;
      const ExprPtr *argument = nullptr;
      bool ownerless = false;
      bool diagnosed = false;
    };
    struct OwnerlessTextCallableUse {
      FunctionId target = 0;
      Token location;
      bool diagnosed = false;
    };
    // A value holding dynamic text (L-OWN-02). Phase 1 keeps it in the body
    // that creates it: a context that does so consumes it, and every other
    // use is reported once at finalization.
    struct TextDependentValueUse {
      Token location;
      bool consumed = false;
      bool diagnosed = false;
      // A relay waits for the request's own finalization to decide it.
      bool relayed = false;
    };
    // A use of a formal or the receiver of a publishable class inside a
    // contract scope. A field read or a forwarding call consumes it; any
    // other use lets every text field of the formal outlive the call.
    struct TextFieldFormalUse {
      const FunctionDecl *function = nullptr;
      OwnerlessTextField formal;
      bool consumed = false;
    };
    // A formal or receiver passed on as another function's class formal or
    // receiver: that callee's requirements on it become the caller's.
    struct TextFieldForwardUse {
      const FunctionDecl *caller = nullptr;
      OwnerlessTextField callerFormal;
      FunctionId callee = 0;
      OwnerlessTextField calleeFormal;
    };
    // A dependent value passed as a class formal or receiver, with the
    // fields that hold dynamic text, recorded while its scope is live.
    struct TextDependentRelay {
      const Expr *argument = nullptr;
      FunctionId callee = 0;
      OwnerlessTextField calleeFormal;
      std::vector<std::vector<SymbolId>> paths;
      bool decided = false;
    };
    struct MutableLocalCandidate {
      SymbolId symbol = 0;
      Token keyword;
      Token name;
      ProgramEffectOwner owner;
    };
    struct UnsafeBlockCandidate {
      const Token *keyword = nullptr;
      bool expression = false;
      ProgramEffectOwner owner;
    };

    std::vector<SemanticDiagnostic> diagnostics;
    SemanticCatalog catalog;
    std::vector<OwnerlessTextCallUse> ownerlessTextCallUses;
    std::vector<OwnerlessTextCallableUse> ownerlessTextCallableUses;
    std::vector<const Expr *> textDependentValueOrder;
    std::unordered_map<const Expr *, TextDependentValueUse> textDependentValues;
    // Payload arguments of the constructor initializer being analysed that
    // name a text input formal; each call publishes that input's owner.
    std::vector<const Expr *> constructorTextPayloadArguments;
    std::vector<const Expr *> textFieldFormalOrder;
    std::unordered_map<const Expr *, TextFieldFormalUse> textFieldFormalUses;
    std::vector<TextFieldForwardUse> textFieldForwardUses;
    std::vector<TextDependentRelay> textDependentRelays;
    // A contract formal's reference relay to a nested text class, or a field
    // chain under one, and the requirement that any use of it but a field
    // projection places on the formal (L-OWN-02 phase 3).
    struct ContractRelayUse {
      const FunctionDecl *function = nullptr;
      OwnerlessTextField prefix;
      bool projected = false;
    };
    std::vector<const Expr *> contractRelayOrder;
    std::unordered_map<const Expr *, ContractRelayUse> contractRelayUses;
    // Per generic class method: the returns that relay its receiver's
    // type-parameter field (conditionalRelayReturns).
    std::unordered_map<const FunctionDecl *, std::unordered_set<const Expr *>>
        conditionalRelayReturns;
    ValueFlowState valueFlow;
    std::unordered_set<std::string> reportedCompilerPrivateAccess;
    std::unordered_map<const Expr *, const ParameterDefault *>
        defaultArgumentSyntax;
    std::unordered_map<const Expr *, std::vector<const Expr *>>
        defaultArgumentDependencies;
    std::unordered_set<const Expr *> reportedDefaultArgumentCycles;
    // Expressions whose value is discarded: an expression statement, a
    // for-loop increment, a comma's left operand, and what a discarded
    // grouping, comma or conditional evaluates in its place.
    std::unordered_set<const Expr *> discardedResults;
    std::unordered_map<std::string, const FunctionDecl *> externCSymbols;
    std::unordered_set<const FunctionDecl *> constexprDefinitionsAnalyzed;
    std::unordered_set<const ConstructorDecl *> constexprConstructorsAnalyzed;
    // Only nested staging workers share the request's execution budget.
    std::shared_ptr<ConstexprWorkBudget> constexprWork;
    std::unordered_set<ProgramEffectOwner, ProgramEffectOwnerHash>
        deferredFlowOwners;
    std::unordered_map<std::string, std::vector<GenericDefaultConstant>>
        genericDefaultConstants;
    std::vector<GenericDefaultConstantUse> genericDefaultConstantUses;
    std::vector<std::unordered_map<std::string, SemanticType>>
        typeParameterScopes;
    std::vector<std::unordered_map<std::string, CompileTimeValue>>
        valueParameterScopes;
    std::vector<std::unordered_map<std::string, GenericParameterId>>
        typePackScopes;
    std::vector<std::vector<AppliedConceptRequirement>> requirementScopes;
    std::vector<MutableLocalCandidate> mutableLocalCandidates;
    std::unordered_set<SymbolId> mutableBindingUses;
    std::vector<UnsafeBlockCandidate> unsafeBlockCandidates;
    std::vector<const Token *> activeUnsafeBlocks;
    std::unordered_set<const Token *> usedUnsafeBlocks;
    TypeSubstitution instanceTypeSubstitution;
    ValueSubstitution instanceValueSubstitution;
    std::vector<std::string> currentNamespace;
    SourceUnitId currentSourceUnit = 0;
    std::unordered_set<const VariableDecl *> structuredBindingElements;
    // Recovery-only flow facts: never published as valid exhaustive matches.
    std::unordered_set<const SwitchStmt *> recoveredExhaustiveSwitches;
    std::vector<std::unordered_map<std::string, Token>> lambdaUncapturedLocals;
    bool measureTopLevelStatements = false;
    // Trusted bodies the lexical pass skipped; see TrustedBodyAnalysis.
    struct DeferredBody {
      const FunctionDecl *declaration = nullptr;
      FunctionId function = 0;
      std::vector<std::string> namespaceScope;
      SourceUnitId sourceUnit = 0;
      std::optional<ClassId> owner;
      bool declarationErrors = false;
      bool constexprDeclarationValid = false;
      std::size_t declarationOrder = 0;
    };
    std::vector<DeferredBody> deferredBodies;
    std::unordered_set<FunctionId> demandedFunctions;
    // Declarations visitFunctionDecl has processed, bodies analyzeFunctionBody
    // has run, and bodies currently being analysed; see ensureBodyAnalyzed.
    std::unordered_set<const FunctionDecl *> visitedFunctionDeclarations;
    std::unordered_set<const FunctionDecl *> analyzedBodies;
    std::unordered_set<const FunctionDecl *> bodiesInProgress;
    bool bodyPhaseActive = false;
    bool onDemandBodyAnalysis = false;
    // Lexical order of every declaration-level statement the body pass has
    // processed, assigned in source order. Namespace globals and constexpr
    // definitions are visible to a body only when their order is lower than
    // the body's own, which is what the lexical pass guaranteed implicitly
    // before the pass became resumable.
    std::unordered_map<const Stmt *, std::size_t> declarationOrders;
    std::size_t declarationOrderCounter = 0;
    std::size_t currentDeclarationOrder = 0;
    // The resumable top-level walk; see runLexicalPass.
    struct LexicalFrame {
      const StmtList *statements = nullptr;
      std::size_t index = 0;
      std::size_t region = 0;
      const NamespaceDecl *space = nullptr;
      const ClassDecl *owner = nullptr;
      ClassId ownerId = 0;
      SourceUnitId sourceUnit = 0;
      bool reachable = true;
      bool reportedUnreachable = false;
    };
    std::vector<LexicalFrame> lexicalFrames;
    std::size_t lexicalFrameVersion = 0;
    // Declaration-level loan flow between leaves of the walk; a paused
    // leaf's own loan flow lives in its BodyLocalState.
    LoanFlowState lexicalLoanFlow;
    // Diagnostics emitted while a demand advanced the walk past the leaf
    // being analysed, one entry per open leaf. They are appended after that
    // leaf's own diagnostics so the result reads in source order.
    std::vector<std::vector<Diagnostic>> heldDiagnostics;
    std::unordered_set<const FunctionDecl *> lexicallyReachableFunctions;
    SemanticType currentType = SemanticType::Unknown;
    SemanticType currentReturnType = SemanticType::Unknown;
    std::optional<ClassId> currentClass;
    bool analyzingFieldInitializer = false;
    bool analyzingConstructorInitializer = false;
    bool analyzingDefaultArgument = false;
    const Expr *currentDefaultArgumentRoot = nullptr;
    std::unordered_set<std::string> currentDefaultParameterNames;
    bool analyzingCallCallee = false;
    const Call *immediateCallReceiver = nullptr;
    bool analyzingPackFoldElement = false;
    bool analyzingDependentConstantCondition = false;
    bool constantConditionGenericDependency = false;
    bool allowPayloadEnumeratorReference = false;
    bool currentStaticMemberFunction = false;
    bool allowPackTypeReference = false;
    bool resolvingGenericTypeDefault = false;
    bool deferringTypeAliasConstraints = false;
    LoanFlowState loanFlow;
    bool instanceClassContextActive = false;
    bool instanceAnalysisActive = false;
    const SemanticModel *instanceBaseModel = nullptr;
    // The detached canonical model of the open InstanceAnalysisScope, for
    // every kind of concrete reanalysis. instanceBaseModel publishes the
    // same model to function and constructor bodies only.
    const SemanticModel *reanalysisBase = nullptr;
    const FunctionDecl *currentFunctionDeclaration = nullptr;
    std::optional<SemanticType> contextualInitializerType;
    const Expr *contextualInitializerExpression = nullptr;
    // The value of the return statement whose declared type it initializes.
    const Expr *contextualReturnValue = nullptr;
    std::optional<SemanticType> contextualOperatorIntegerType;
    std::optional<ContextualCallableResult> contextualCallableResult;
    std::optional<ResultUse> contextualResultUse;
    ReceiverMutability currentReceiverMutability = ReceiverMutability::ReadOnly;
    std::size_t constructorDepth = 0;
    std::size_t destructorDepth = 0;
    std::size_t functionDepth = 0;
    std::size_t loopDepth = 0;
    std::size_t switchDepth = 0;
    std::size_t lambdaDepth = 0;
    std::size_t unsafeDepth = 0;
    std::size_t suppressLoanPlaceAccessDepth = 0;
    std::size_t preferReadOnlySubscriptReceiverDepth = 0;
    std::vector<const Expr *> suppressedProjectedPlaceReads;
    ProgramEffectOwner currentProgramEffectOwner;
    std::vector<CThreadCallUse> cThreadCallUses;
    std::vector<RawBorrowCallUse> rawBorrowCallUses;
    std::shared_ptr<RawResultQueryState> rawResultQueries;
    // See openReanalyses() and InstanceAnalysisScope.
    std::shared_ptr<std::vector<InstanceAnalysisKey>> openReanalyses;
    // Set while this analyzer reanalyses an instance whose reanalysis is
    // already open further up the staging chain, which owns its assertions.
    bool assertionsOwnedUpChain = false;
    // Whether each concrete class instance with static assertions fails one,
    // keyed by its field-initializer reanalysis; see
    // concreteClassAssertionsFail().
    std::unordered_map<InstanceAnalysisKey, bool, InstanceAnalysisKeyHash>
        classAssertionOutcomes;
    // While a concrete instance is analysed, each static assertion's own
    // diagnostics are also copied here. If one fails, or the instance's class
    // assertion did, they are all the analysis reports; see
    // beginAssertionTracking().
    bool trackAssertionDiagnostics = false;
    bool trackedAssertionFailed = false;
    std::size_t assertionTrackingStart = 0;
    std::vector<SemanticDiagnostic> assertionDiagnostics;
    std::shared_ptr<CompleteReturnQueryState> completeReturnQueries;
    std::shared_ptr<RawResultQuery> collectingRawResult;
    std::size_t rawAccessObservation = 0;
    bool hasCThreadRequirements = false;
    std::unordered_map<ProgramEffectOwner, ProgramEffectSummary,
                       ProgramEffectOwnerHash>
        programEffects;
    std::shared_ptr<ProgramConcreteEffectState> programConcreteEffects;
    std::size_t programStoragePlaceUseDepth = 0;
    std::vector<const Expr *> foreignAddressPlaces;
    std::unordered_map<std::string, const VariableDecl *> externCVariables;
    LambdaId nextLambdaId = 1;
    SemanticLoanId nextSemanticLoanId = 1;
    std::size_t nextPlaceSelectionId = 1;
    std::vector<SemanticType> invalidOrdinaryLayouts;
    // ---- Trusted prefix preamble (see SemanticPreamble) -----------------
    // A trusted body that bounded (Reachable) analysis would have deferred,
    // analysed while a preamble was built: its own body diagnostics, held
    // back until something reaches it, and the function records it
    // consulted, which carry reachability on.
    struct PreambleBody {
      std::vector<Diagnostic> diagnostics;
      std::vector<FunctionId> demands;
    };
    std::unordered_map<const FunctionDecl *, PreambleBody> preambleBodies;
    std::vector<const FunctionDecl *> preambleBodyOrder;
    // Finalization boundaries recorded when the preamble was frozen, so an
    // analysis resumed from it finalizes only what it added.
    std::size_t preambleForwardings = 0;
    std::size_t preambleCThreadCallUses = 0;
    std::size_t preambleUnsafeBlockCandidates = 0;
    std::size_t preambleMutableLocalCandidates = 0;
    std::size_t preambleGenericDefaultConstantUses = 0;
    std::unordered_map<SourceUnitId, std::size_t> preambleOccurrenceCounts;
    LambdaId preambleFirstLambda = 0;
    // Native record and union shape/layout states, by class position. They
    // outlive registration so a resumed analysis extends them.
    std::vector<bool> cAbiRecordShapes;
    std::vector<std::uint8_t> cAbiRecordStates;
    std::vector<bool> unionShapes;
    std::vector<std::uint8_t> unionStates;
  };
  struct RequestSessionState : RequestSessionContext {
    SemanticModel semanticModel;
  } requestSession;

  std::vector<SemanticDiagnostic> &diagnostics = requestSession.diagnostics;
  SemanticModel &semanticModel = requestSession.semanticModel;
  // Diagnostic construction lives in its own translation unit
  // (semantic_diagnostics.cpp); the reporter binds this session's sink and
  // model, and analysis facts arrive as call parameters. It is bound per
  // analyzer, not per session state, so a session state copied from a
  // preamble reports into its own sink.
  SemanticDiagnosticReporter reporter{semanticModel, diagnostics};
  SemanticCatalog &catalog = requestSession.catalog;
  ValueFlowState &valueFlow = requestSession.valueFlow;
  ScopeStack &scopes = requestSession.valueFlow.scopes;
  std::unordered_set<std::string> &namespaces =
      requestSession.catalog.namespaces;
  std::unordered_map<std::string, SymbolId> &namespaceToolingSymbols =
      requestSession.catalog.namespaceToolingSymbols;
  std::unordered_map<std::string, NamespaceAliasInfo> &namespaceAliases =
      requestSession.catalog.namespaceAliases;
  std::unordered_set<std::string> &reportedCompilerPrivateAccess =
      requestSession.reportedCompilerPrivateAccess;
  SymbolId &compilerPrivateNamespaceSymbol =
      requestSession.catalog.compilerPrivateNamespaceSymbol;
  std::unordered_map<std::string, TypeAliasId> &typeAliasIds =
      requestSession.catalog.typeAliasIds;
  std::vector<RegisteredTypeAlias> &typeAliases =
      requestSession.catalog.typeAliases;
  std::unordered_map<std::string, ConceptId> &conceptIds =
      requestSession.catalog.conceptIds;
  std::vector<RegisteredConcept> &concepts = requestSession.catalog.concepts;
  std::unordered_map<std::string, ClassId> &classIds =
      requestSession.catalog.classIds;
  std::unordered_map<std::string, EnumId> &enumIds =
      requestSession.catalog.enumIds;
  std::unordered_map<SourceUnitId, std::unordered_set<std::string>>
      &visibleNamespaces = requestSession.catalog.visibleNamespaces;
  std::unordered_map<SourceUnitId,
                     std::unordered_map<std::string, NamespaceAliasInfo>>
      &visibleNamespaceAliases = requestSession.catalog.visibleNamespaceAliases;
  std::unordered_map<SourceUnitId, std::unordered_map<std::string, TypeAliasId>>
      &visibleTypeAliasIds = requestSession.catalog.visibleTypeAliasIds;
  std::unordered_map<SourceUnitId, std::unordered_map<std::string, ConceptId>>
      &visibleConceptIds = requestSession.catalog.visibleConceptIds;
  std::unordered_map<SourceUnitId, std::unordered_map<std::string, ClassId>>
      &visibleClassIds = requestSession.catalog.visibleClassIds;
  std::unordered_map<SourceUnitId, std::unordered_map<std::string, EnumId>>
      &visibleEnumIds = requestSession.catalog.visibleEnumIds;
  std::unordered_map<const ClassDecl *, ClassId> &classDeclIds =
      requestSession.catalog.classDeclIds;
  std::unordered_map<const ConceptDecl *, ConceptId> &conceptDeclIds =
      requestSession.catalog.conceptDeclIds;
  std::unordered_map<const FunctionDecl *, std::vector<GenericParameterInfo>>
      &functionGenericParameters =
          requestSession.catalog.functionGenericParameters;
  std::unordered_map<const ConstructorDecl *, std::vector<GenericParameterInfo>>
      &constructorGenericParameters =
          requestSession.catalog.constructorGenericParameters;
  std::unordered_set<const FunctionDecl *> &constexprDefinitionsAnalyzed =
      requestSession.constexprDefinitionsAnalyzed;
  std::unordered_set<const ConstructorDecl *> &constexprConstructorsAnalyzed =
      requestSession.constexprConstructorsAnalyzed;
  std::unordered_map<std::string, std::vector<GenericDefaultConstant>>
      &genericDefaultConstants = requestSession.genericDefaultConstants;
  std::vector<GenericDefaultConstantUse> &genericDefaultConstantUses =
      requestSession.genericDefaultConstantUses;
  std::unordered_map<const Expr *, const ParameterDefault *>
      &defaultArgumentSyntax = requestSession.defaultArgumentSyntax;
  std::unordered_map<const Expr *, std::vector<const Expr *>>
      &defaultArgumentDependencies = requestSession.defaultArgumentDependencies;
  std::unordered_set<const Expr *> &reportedDefaultArgumentCycles =
      requestSession.reportedDefaultArgumentCycles;
  std::unordered_set<const Expr *> &discardedResults =
      requestSession.discardedResults;
  std::unordered_map<std::string, const FunctionDecl *> &externCSymbols =
      requestSession.externCSymbols;
  std::unordered_map<std::string, const VariableDecl *>
      &rootNativeStorageSymbols =
          requestSession.catalog.rootNativeStorageSymbols;
  std::unordered_map<std::string, std::vector<GlobalBorrowStorageInfo>>
      &globalBorrowStorage = requestSession.catalog.globalBorrowStorage;
  std::unordered_map<GenericParameterId, GenericConstraintSet>
      &genericConstraints = requestSession.catalog.genericConstraints;
  std::unordered_map<GenericParameterId, ConceptId> &genericConstraintConcepts =
      requestSession.catalog.genericConstraintConcepts;
  std::vector<ClassInfo> &classes = requestSession.catalog.classes;
  std::vector<ExactClassSpecialization> &exactClassSpecializations =
      requestSession.catalog.exactClassSpecializations;
  std::vector<RegisteredConformance> &conformances =
      requestSession.catalog.conformances;
  // The parameters of the conformance block being registered or validated,
  // so diagnostics can spell them while the block is not in `conformances`.
  const std::vector<GenericParameterInfo> *registeringConformanceParameters =
      nullptr;
  // Filled by registerConformances, emptied by validateConformanceTypes in
  // the same registration.
  std::vector<ConformanceTypeCheck> conformanceTypeChecks;
  std::vector<EnumInfo> &enums = requestSession.catalog.enums;
  std::vector<std::unordered_map<std::string, SemanticType>>
      &typeParameterScopes = requestSession.typeParameterScopes;
  std::vector<std::unordered_map<std::string, CompileTimeValue>>
      &valueParameterScopes = requestSession.valueParameterScopes;
  std::vector<std::unordered_map<std::string, GenericParameterId>>
      &typePackScopes = requestSession.typePackScopes;
  std::vector<std::vector<AppliedConceptRequirement>> &requirementScopes =
      requestSession.requirementScopes;
  TypeSubstitution &instanceTypeSubstitution =
      requestSession.instanceTypeSubstitution;
  ValueSubstitution &instanceValueSubstitution =
      requestSession.instanceValueSubstitution;
  std::vector<std::string> &currentNamespace = requestSession.currentNamespace;
  SourceUnitId &currentSourceUnit = requestSession.currentSourceUnit;
  std::unordered_set<const VariableDecl *> &predeclaredVariables =
      requestSession.catalog.predeclaredVariables;
  std::unordered_map<const VariableDecl *, const Token *>
      &mutableFieldGroupOrigins =
          requestSession.catalog.mutableFieldGroupOrigins;
  std::unordered_set<const VariableDecl *> &recursiveLayoutFields =
      requestSession.catalog.recursiveLayoutFields;
  std::unordered_set<const VariableDecl *> &structuredBindingElements =
      requestSession.structuredBindingElements;
  std::vector<std::unordered_map<std::string, Token>> &lambdaUncapturedLocals =
      requestSession.lambdaUncapturedLocals;
  bool &measureTopLevelStatements = requestSession.measureTopLevelStatements;
  std::vector<RequestSessionState::DeferredBody> &deferredBodies =
      requestSession.deferredBodies;
  std::unordered_set<FunctionId> &demandedFunctions =
      requestSession.demandedFunctions;
  // Lookup is unchanged; a scoped sink records a conservative demand set.
  // Declaration bookkeeping must not itself demand the function's body.
  struct FunctionDemandScope {
    const SemanticModel &model;
    std::unordered_set<FunctionId> *previous;
    FunctionDemandScope(const SemanticModel &model,
                        std::unordered_set<FunctionId> *sink)
        : model(model), previous(model.setFunctionDemandSink(sink)) {}
    ~FunctionDemandScope() { model.setFunctionDemandSink(previous); }
  };
  std::unordered_set<const FunctionDecl *> &visitedFunctionDeclarations =
      requestSession.visitedFunctionDeclarations;
  std::unordered_set<const FunctionDecl *> &analyzedBodies =
      requestSession.analyzedBodies;
  std::unordered_set<const FunctionDecl *> &bodiesInProgress =
      requestSession.bodiesInProgress;
  bool &bodyPhaseActive = requestSession.bodyPhaseActive;
  bool &onDemandBodyAnalysis = requestSession.onDemandBodyAnalysis;
  std::unordered_map<const Stmt *, std::size_t> &declarationOrders =
      requestSession.declarationOrders;
  std::size_t &declarationOrderCounter = requestSession.declarationOrderCounter;
  std::size_t &currentDeclarationOrder = requestSession.currentDeclarationOrder;
  std::vector<RequestSessionState::LexicalFrame> &lexicalFrames =
      requestSession.lexicalFrames;
  std::size_t &lexicalFrameVersion = requestSession.lexicalFrameVersion;
  LoanFlowState &lexicalLoanFlow = requestSession.lexicalLoanFlow;
  std::vector<std::vector<Diagnostic>> &heldDiagnostics =
      requestSession.heldDiagnostics;
  std::unordered_set<const FunctionDecl *> &lexicallyReachableFunctions =
      requestSession.lexicallyReachableFunctions;
  // Analyses one deferred body in its own declaration context.
  void analyzeDeferredBody(RequestSessionState::DeferredBody body);

  // A concrete reanalysis of any instance kind (function, constructor,
  // destructor, or class field initializers and static assertions) runs over
  // the completed program, so declaration order gates nothing inside it.
  // Staged and query workers hold no declaration orders at all.
  [[nodiscard]] bool concreteReanalysis() const {
    return reanalysisBase != nullptr;
  }

  // Declaration-level lexical order. `beginDeclaration` gives a declaration
  // statement the next order and makes it current for the analysis of its
  // own initializer or body; the guard restores the enclosing order.
  struct DeclarationOrderScope {
    Impl &impl;
    std::size_t enclosing;
    DeclarationOrderScope(Impl &impl, const Stmt &stmt)
        : impl(impl), enclosing(impl.currentDeclarationOrder) {
      if (impl.concreteReanalysis())
        return;
      const auto found = impl.declarationOrders.find(&stmt);
      impl.currentDeclarationOrder =
          found != impl.declarationOrders.end()
              ? found->second
              : impl.declarationOrders
                    .emplace(&stmt, ++impl.declarationOrderCounter)
                    .first->second;
    }
    ~DeclarationOrderScope() { impl.currentDeclarationOrder = enclosing; }
  };
  [[nodiscard]] bool declaredBeforeCurrent(const Stmt *declaration) const {
    if (concreteReanalysis())
      return true;
    const auto found = declarationOrders.find(declaration);
    return found != declarationOrders.end() &&
           found->second < currentDeclarationOrder;
  }
  [[nodiscard]] bool symbolVisibleNow(const Symbol &symbol) const {
    return concreteReanalysis() || symbol.variableDeclaration == nullptr ||
           symbol.declarationOrder == 0 ||
           symbol.declarationOrder < currentDeclarationOrder;
  }
  [[nodiscard]] bool symbolVisibleAt(const Symbol &symbol,
                                     const Token &token) const {
    const auto *context = macroContext(token);
    return !context ? symbolVisibleNow(symbol)
                    : concreteReanalysis() ||
                          symbol.variableDeclaration == nullptr ||
                          symbol.declarationOrder == 0 ||
                          symbol.declarationOrder < context->declarationOrder;
  }

  // The resumable lexical pass over the program's declarations.
  void runLexicalPass(const StmtList &declarations);
  void advanceLexicalPass(const FunctionDecl *until);
  void rebuildLexicalEnvironment();
  void enterLexicalNamespace(const NamespaceDecl &space, SourceUnitId unit);
  bool enterLexicalClass(const ClassDecl &classDecl, SourceUnitId unit);
  void exitLexicalFrame();
  void processLexicalLeaf(const StmtPtr &statement, bool measure);
  void indexLexicallyReachableFunctions(const StmtList &declarations);
  [[nodiscard]] bool lexicalAdvanceAllowed() const {
    return (functionDepth > 0 || constructorDepth > 0 || destructorDepth > 0) &&
           !analyzingFieldInitializer && !analyzingDefaultArgument;
  }
  void beginClassBody(const ClassDecl &stmt, const ClassInfo &info,
                      const SemanticType &type);
  void endClassBody(const ClassInfo &info);

  // The body-local analysis state one function body owns. On-demand body
  // analysis moves it aside, analyses the callee from a clean state, and
  // restores it; the field list mirrors prepareInstanceAnalysis, which resets
  // the same state for a concrete instance. Program-wide accumulations
  // (model, catalog, diagnostics, identity counters, constexpr definitions)
  // are deliberately not part of it.
  struct BodyLocalState {
    ScopeStack scopes;
    decltype(RequestSessionState::typeParameterScopes) typeParameterScopes;
    decltype(RequestSessionState::valueParameterScopes) valueParameterScopes;
    decltype(RequestSessionState::typePackScopes) typePackScopes;
    decltype(RequestSessionState::requirementScopes) requirementScopes;
    decltype(RequestSessionState::currentNamespace) currentNamespace;
    SourceUnitId currentSourceUnit = 0;
    std::optional<ClassId> currentClass;
    const FunctionDecl *currentFunctionDeclaration = nullptr;
    bool analyzingFieldInitializer = false;
    bool analyzingConstructorInitializer = false;
    bool analyzingDefaultArgument = false;
    bool analyzingDependentConstantCondition = false;
    bool constantConditionGenericDependency = false;
    decltype(RequestSessionState::currentDefaultArgumentRoot)
        currentDefaultArgumentRoot{};
    decltype(RequestSessionState::currentDefaultParameterNames)
        currentDefaultParameterNames;
    bool analyzingCallCallee = false;
    bool currentStaticMemberFunction = false;
    bool allowPackTypeReference = false;
    LoanFlowState loanFlow;
    std::vector<ValueControlFlowContext> valueControlFlow;
    decltype(RequestSessionState::contextualInitializerType)
        contextualInitializerType;
    decltype(RequestSessionState::contextualInitializerExpression)
        contextualInitializerExpression{};
    decltype(RequestSessionState::contextualReturnValue)
        contextualReturnValue{};
    // A nested body analysis is not part of the enclosing instance's own
    // assertions.
    bool trackAssertionDiagnostics = false;
    decltype(RequestSessionState::contextualOperatorIntegerType)
        contextualOperatorIntegerType;
    decltype(RequestSessionState::contextualCallableResult)
        contextualCallableResult;
    decltype(RequestSessionState::contextualResultUse) contextualResultUse;
    ReceiverMutability currentReceiverMutability = ReceiverMutability::ReadOnly;
    decltype(RequestSessionState::suppressedProjectedPlaceReads)
        suppressedProjectedPlaceReads;
    std::size_t constructorDepth = 0;
    std::size_t destructorDepth = 0;
    std::size_t functionDepth = 0;
    std::size_t loopDepth = 0;
    std::size_t switchDepth = 0;
    std::size_t lambdaDepth = 0;
    decltype(RequestSessionState::lambdaUncapturedLocals)
        lambdaUncapturedLocals;
    decltype(RequestSessionState::structuredBindingElements)
        structuredBindingElements;
    SemanticType currentType = SemanticType::Unknown;
    SemanticType currentReturnType = SemanticType::Unknown;
    ProgramEffectOwner currentProgramEffectOwner;
    decltype(ValueFlowState::rawPointerLoops) rawPointerLoops;
    std::vector<RawProgramStorageState> rawProgramStorage;
    bool measureTopLevelStatements = false;
    bool onDemandBodyAnalysis = false;
    // `constexpr for` expansion state is per declaration: a demanded callee
    // must neither consume nor reset the caller's expansion budget.
    std::vector<ConstexprForFrame> constexprForFrames;
    std::size_t constexprForExpandedBodies = 0;
  };
  [[nodiscard]] BodyLocalState captureBodyLocalState();
  void restoreBodyLocalState(BodyLocalState state);
  // Analyses `function`'s body now if it has not been analysed yet, so a
  // body-derived fact a caller consumes never depends on source order.
  void ensureBodyAnalyzed(const FunctionInfo &function);
  void enterDeclarationContext(const std::vector<std::string> &namespaceScope,
                               SourceUnitId sourceUnit,
                               std::optional<ClassId> owner);
  void addEnclosingClassStaticMembers(ClassId owner);
  void exitDeclarationContext(std::optional<ClassId> owner);
  SemanticType &currentType = requestSession.currentType;
  SemanticType &currentReturnType = requestSession.currentReturnType;
  std::optional<ClassId> &currentClass = requestSession.currentClass;
  bool &analyzingFieldInitializer = requestSession.analyzingFieldInitializer;
  bool &analyzingConstructorInitializer =
      requestSession.analyzingConstructorInitializer;
  bool &analyzingDefaultArgument = requestSession.analyzingDefaultArgument;
  const Expr *&currentDefaultArgumentRoot =
      requestSession.currentDefaultArgumentRoot;
  std::unordered_set<std::string> &currentDefaultParameterNames =
      requestSession.currentDefaultParameterNames;
  bool &analyzingCallCallee = requestSession.analyzingCallCallee;
  const Call *&immediateCallReceiver = requestSession.immediateCallReceiver;
  bool &analyzingPackFoldElement = requestSession.analyzingPackFoldElement;
  bool &analyzingDependentConstantCondition =
      requestSession.analyzingDependentConstantCondition;
  bool &constantConditionGenericDependency =
      requestSession.constantConditionGenericDependency;
  bool &allowPayloadEnumeratorReference =
      requestSession.allowPayloadEnumeratorReference;
  bool &currentStaticMemberFunction =
      requestSession.currentStaticMemberFunction;
  bool &allowPackTypeReference = requestSession.allowPackTypeReference;
  bool &resolvingGenericTypeDefault =
      requestSession.resolvingGenericTypeDefault;
  bool &deferringTypeAliasConstraints =
      requestSession.deferringTypeAliasConstraints;
  LoanFlowState &loanFlow = requestSession.loanFlow;
  std::vector<ValueControlFlowContext> &valueControlFlow =
      requestSession.valueFlow.controlFlow;
  bool &instanceClassContextActive = requestSession.instanceClassContextActive;
  bool &instanceAnalysisActive = requestSession.instanceAnalysisActive;
  const SemanticModel *&instanceBaseModel = requestSession.instanceBaseModel;
  const SemanticModel *&reanalysisBase = requestSession.reanalysisBase;
  const FunctionDecl *&currentFunctionDeclaration =
      requestSession.currentFunctionDeclaration;
  std::optional<SemanticType> &contextualInitializerType =
      requestSession.contextualInitializerType;
  const Expr *&contextualInitializerExpression =
      requestSession.contextualInitializerExpression;
  const Expr *&contextualReturnValue = requestSession.contextualReturnValue;
  std::optional<SemanticType> &contextualOperatorIntegerType =
      requestSession.contextualOperatorIntegerType;
  std::optional<ContextualCallableResult> &contextualCallableResult =
      requestSession.contextualCallableResult;
  std::optional<ResultUse> &contextualResultUse =
      requestSession.contextualResultUse;
  ReceiverMutability &currentReceiverMutability =
      requestSession.currentReceiverMutability;
  std::size_t &constructorDepth = requestSession.constructorDepth;
  std::size_t &destructorDepth = requestSession.destructorDepth;
  std::size_t &functionDepth = requestSession.functionDepth;
  std::size_t &loopDepth = requestSession.loopDepth;
  std::size_t &switchDepth = requestSession.switchDepth;
  std::size_t &lambdaDepth = requestSession.lambdaDepth;
  std::size_t &unsafeDepth = requestSession.unsafeDepth;
  std::size_t &suppressLoanPlaceAccessDepth =
      requestSession.suppressLoanPlaceAccessDepth;
  std::size_t &preferReadOnlySubscriptReceiverDepth =
      requestSession.preferReadOnlySubscriptReceiverDepth;
  std::vector<const Expr *> &suppressedProjectedPlaceReads =
      requestSession.suppressedProjectedPlaceReads;
  ProgramEffectOwner &currentProgramEffectOwner =
      requestSession.currentProgramEffectOwner;
  std::unordered_map<ProgramEffectOwner, ProgramEffectSummary,
                     ProgramEffectOwnerHash> &programEffects =
      requestSession.programEffects;
  std::size_t &programStoragePlaceUseDepth =
      requestSession.programStoragePlaceUseDepth;
  GenericParameterId &nextGenericParameterId =
      requestSession.catalog.nextGenericParameterId;
  ConstructorId &nextConstructorId = requestSession.catalog.nextConstructorId;
  std::size_t &nextDefaultArgumentEffectOwner =
      requestSession.catalog.nextDefaultArgumentEffectOwner;
  FunctionId &nextFunctionId = requestSession.catalog.nextFunctionId;
  LambdaId &nextLambdaId = requestSession.nextLambdaId;
  SemanticLoanId &nextSemanticLoanId = requestSession.nextSemanticLoanId;
  std::size_t &nextPlaceSelectionId = requestSession.nextPlaceSelectionId;

  TargetInfo target;
  const SourceGraph *sourceGraph = nullptr;
  const SourceManager *sources = nullptr;
  // Mirrors FrontendOptions::toolingOccurrences; reapplied after the
  // model is cleared at the start of each check().
  bool toolingOccurrences = true;
  FrontendWorkMetrics *workMetrics = nullptr;
  RawPointerAuditMode rawPointerAudit = RawPointerAuditMode::Off;
  // Fragment analysis may resolve declarations without publishing executable
  // code. Closed executable/library requests need every actual GTI call target.
  bool requireFunctionDefinitions = false;
  TrustedBodyAnalysis trustedBodies = TrustedBodyAnalysis::Complete;

  // ---- Staged constant evaluation worker --------------------------------
  // The model the constants required here stage against: this analyzer's
  // canonical model, which an open concrete reanalysis has detached into its
  // scope. Never the reanalysis's partial delta, whose copy would keep
  // reading through to a scope that is gone once the reanalysis closes.
  [[nodiscard]] const SemanticModel &constexprStagingModel() const {
    return reanalysisBase != nullptr ? *reanalysisBase : semanticModel;
  }
  // The analyzer a staged constexpr evaluation lowers its constant instance
  // through. It copies the sealed catalog and the staging model, which is
  // most of an evaluation's cost, so it is kept and reused while nothing it
  // copied can have changed. The staging model is always the canonical model,
  // but that model moves between this analyzer and each open reanalysis
  // scope, so its address identifies nothing. The worker is keyed instead by
  // what the model can have gained: the current declaration order, whether a
  // concrete reanalysis (which sees every analysed definition) is open, and
  // the constexpr definitions and constructors available. Completing the
  // primary analysis drops the worker. A late-demand batch does not: it
  // completes only deferred trusted bodies, never constexpr ones, so nothing a
  // staged constant can execute changes. Per-evaluation context (the work
  // budget, raw result queries, the open reanalyses, the lambda identity
  // floor) is refreshed on every use.
  struct StagedWorker {
    std::unique_ptr<Impl> impl;
    bool reanalysis = false;
    std::size_t declarationOrder = 0;
    std::size_t definitions = 0;
    std::size_t constructors = 0;
  };
  StagedWorker stagedWorker;
  [[nodiscard]] Impl &
  stagedWorkerFor(const SemanticModel &stagingModel,
                  const std::shared_ptr<ConstexprWorkBudget> &work);
};

// The frozen analyzer of a trusted prefix (see semantic_preamble.h). It owns
// the prefix program, so the syntax its state refers to stays alive, and the
// spent analyzer whose session state resumed analyses copy. Immutable once
// built; analyzers on several threads may adopt it at once.
class SemanticPreamble {
public:
  SemanticPreamble(Program prefix, std::unique_ptr<SemanticVisitor::Impl> state)
      : prefix(std::move(prefix)), state(std::move(state)) {}
  ~SemanticPreamble() = default;

  SemanticPreamble(const SemanticPreamble &) = delete;
  SemanticPreamble &operator=(const SemanticPreamble &) = delete;

  [[nodiscard]] std::size_t declarationCount() const {
    return prefix.declarations().size();
  }
  // Distinct for every preamble ever built in this process, so reusable
  // records that refer to a preamble's synthesized syntax are never served
  // under another preamble of the same units.
  [[nodiscard]] std::uint64_t identity() const { return identity_; }

  Program prefix;
  std::unique_ptr<SemanticVisitor::Impl> state;

private:
  std::uint64_t identity_ = nextIdentity();
  static std::uint64_t nextIdentity();
};

} // namespace lang
