#pragma once

#include "gti/semantic_analyzer.h"

#include "diagnostic_registry.h"
#include "gti/ast_clone.h"
#include "gti/frontend_work.h"
#include "gti/standard_library.h"
#include "semantic_diagnostics.h"
#include "semantic_spelling.h"

#include <cassert>
#include <chrono>

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

  explicit Impl(TargetInfo target, const SourceGraph *sourceGraph,
                bool toolingOccurrences, FrontendWorkMetrics *workMetrics,
                const SourceManager *sources,
                RawPointerAuditMode rawPointerAudit = RawPointerAuditMode::Off,
                bool requireFunctionDefinitions = false)
      : target(std::move(target)), sourceGraph(sourceGraph), sources(sources),
        toolingOccurrences(toolingOccurrences), workMetrics(workMetrics),
        rawPointerAudit(rawPointerAudit),
        requireFunctionDefinitions(requireFunctionDefinitions) {
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
  struct InstanceAnalysisScope {
    explicit InstanceAnalysisScope(Impl &visitor)
        : visitor(visitor), baseModel(std::move(visitor.semanticModel)),
          baseDiagnostics(std::move(visitor.diagnostics)),
          savedGenericParameterId(visitor.nextGenericParameterId),
          savedConstructorId(visitor.nextConstructorId),
          savedFunctionId(visitor.nextFunctionId),
          savedLambdaId(visitor.nextLambdaId),
          savedSemanticLoanId(visitor.nextSemanticLoanId),
          savedPlaceSelectionId(visitor.nextPlaceSelectionId) {
      visitor.catalog.assertUnchangedSinceSeal();
      visitor.semanticModel.beginInstanceDelta(baseModel,
                                               visitor.instanceSymbolCeiling);
      visitor.diagnostics.clear();
    }

    ~InstanceAnalysisScope() { visitor.catalog.assertUnchangedSinceSeal(); }

    // Takes the delta and diagnostics, restores the visitor, and re-points
    // the delta at the restored base model. Call exactly once.
    [[nodiscard]] SemanticInstanceAnalysis finish() {
      visitor.instanceSymbolCeiling =
          visitor.semanticModel.database().symbolIdentityCeiling();
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
      visitor.instanceAnalysisActive = false;
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
  };

  // New concrete-only declarations must not reuse another instance's IDs.
  // Primary declaration IDs remain inherited; the base snapshot stays
  // immutable.
  SymbolId instanceSymbolCeiling = 0;

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

  void visitUnexpectedExpr(const Unexpected &expr) override;

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
    std::vector<FunctionId> virtualRoots;
    SemanticType dispatchOwner = SemanticType::Unknown;
    bool compilerPrivate = false;

    [[nodiscard]] const CallableDeclaration &record() const {
      assert(declarationRecord != nullptr);
      return *declarationRecord;
    }
  };

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

  struct LoanFlowConditionalBoundary {
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
    bool deferredConstexprSelection = false;
    std::size_t rawConditionalExpressionDepth = 0;
    std::unordered_map<SemanticLoanId, LoanFlowRecord> records;
    std::unordered_map<SymbolId, SemanticLoanId> carrierLoans;
    std::vector<SemanticLoanId> receiverStorageLoans;
    std::vector<PendingBorrowConflict> conflicts;
    // Audit uses may inspect checked liveness, but cannot request an endpoint
    // or otherwise change the mandatory loan schedule.
    std::vector<PendingBorrowConflict> rawAuditConflicts;
    std::vector<LoanFlowUse> uses;
    std::vector<LoanFlowConditionalBoundary> conditionalBoundaries;
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
    SemanticLoanId retainedLoan = 0;
    std::vector<SemanticLoanId> storageLoans;
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
      Visible
    } space = Namespace::Global;
    SourceUnitId unit = 0;
    std::string name;
    std::vector<RawPointerOrigin> origins;
    std::vector<ProjectedRawPointerOrigins> fields;
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

  using Scope = std::unordered_map<std::string, Symbol>;
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
  };

  struct ClassInfo {
    enum class DefaultsState { Unresolved, Resolving, Resolved, Invalid };
    ClassId id = 0;
    SourceUnitId sourceUnit = 0;
    const ClassDecl *declaration = nullptr;
    Token name;
    ClassKind kind = ClassKind::Class;
    std::vector<std::string> namespaceScope;
    ClassId exactSpecializationPrimary = 0;
    // Disjoint signature receiver definitions share a lookup name, not a
    // concrete class identity. The first registered definition owns the list.
    ClassId signatureFamilyPrimary = 0;
    std::vector<ClassId> signatureFamilyMembers;
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
                           std::vector<SemanticType> *visiting = nullptr) const;

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

  [[nodiscard]] static std::optional<bool>
  constantBoolean(const ExprPtr &expression);

  [[nodiscard]] bool expressionNeverReturns(const Expr *expression) const;

  [[nodiscard]] FlowSummary summarizeFlow(const StmtList &statements) const;

  [[nodiscard]] FlowSummary summarizeFlow(const Stmt *statement) const;

  // MIR currently materializes both runtime `if` successors even when the
  // condition is a literal. Loan endpoint selection must therefore use the
  // same structural reachability instead of the constant-aware return-flow
  // summary used for source diagnostics.
  [[nodiscard]] FlowSummary summarizeLoanFlow(const StmtList &statements) const;

  [[nodiscard]] FlowSummary summarizeLoanFlow(const Stmt *statement) const;

  void analyzeInferredVariable(const VariableDecl &declaration);

  bool reportMissingLambdaCapture(const Token &use);

  [[nodiscard]] SemanticTypeTraits typeTraits(const SemanticType &type) const;

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

  [[nodiscard]] ConcurrencyCapabilities
  typeCapabilities(const SemanticType &type,
                   std::vector<SemanticType> &visiting) const;

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

  void reportOwnerDependentTextStorage(const ExprPtr &expression,
                                       std::string_view destination);

  [[nodiscard]] BindingInfo
  bindingInfo(SemanticType type,
              AccessMode access = AccessMode::ReadOnly) const;

  [[nodiscard]] bool isMoveOnlyOwnerType(const SemanticType &type) const;

  struct StoredReferencePath {
    std::vector<const VariableDecl *> fields;
    SemanticType reference = SemanticType::Unknown;
  };

  struct StoredReferenceBinding {
    StoredReferencePath destination;
    std::size_t parameter = 0;
    // A nominal parameter contributes the same path below the initialized
    // field. A direct reference parameter contributes its referent itself.
    std::vector<const VariableDecl *> sourceFields;
  };

  [[nodiscard]] std::vector<StoredReferenceBinding>
  storedCarrierConstructorBindings(
      const SemanticType &type, const ConstructorDecl *constructor,
      std::span<const SemanticType> parameterTypes) const;

  [[nodiscard]] std::vector<SymbolId>
  storedFieldSymbols(SemanticType type,
                     std::span<const VariableDecl *const> fields);

  // Enumerate every independently checked dependency in declaration order.
  // An empty result means either no reference or an unsupported carrier shape.
  [[nodiscard]] std::vector<StoredReferencePath>
  storedReferencePaths(const SemanticType &type) const;

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

  [[nodiscard]] bool isFallibleBorrowType(const SemanticType &type) const;

  [[nodiscard]] std::optional<SemanticType>
  fallibleBorrowReferent(const SemanticType &type) const;

  [[nodiscard]] std::optional<SemanticType>
  storedReferenceReferent(const SemanticType &type) const;

  [[nodiscard]] bool isBorrowReturningType(const SemanticType &type) const;

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

  void reportMissingFunctionReturnBorrowSummary(const FunctionDecl &function,
                                                const FunctionInfo &info);

  [[nodiscard]] AccessMode borrowAccess(const SemanticType &type) const;

  [[nodiscard]] bool nestsBorrowedState(const SemanticType &type) const;

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

  [[nodiscard]] std::vector<SemanticLoanDependency>
  storedBorrowDependencies(const ExprPtr &expression);

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

  void validateCallPlaceExclusivityImpl(
      std::span<const SemanticType> parameterTypes,
      std::span<const ExprPtr *> arguments, const Token &location,
      std::optional<SemanticLoanPlace> receiver = std::nullopt,
      AccessMode receiverAccess = AccessMode::ReadOnly,
      bool transientReceiver = false);

  void validateCallPlaceExclusivity(
      std::span<const SemanticType> parameterTypes,
      std::span<const ExprPtr> arguments, const Token &location,
      std::optional<SemanticLoanPlace> receiver = std::nullopt,
      AccessMode receiverAccess = AccessMode::ReadOnly,
      bool transientReceiver = false);

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
      std::vector<SymbolId> designatedFields = {});

  void recordParameterEntryLoan(const Parameter &parameter);
  SemanticLoanId recordStoredEntryDependency(Symbol &carrier,
                                             SymbolId carrierSymbol,
                                             const SemanticType &type,
                                             bool receiver,
                                             const Stmt *declaration = nullptr);

  void recordRetainedBorrow(const VariableDecl &binding,
                            const ExprPtr &expression);

  void recordSemanticLoanUseForLoan(SemanticLoanId loan);

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
                             std::vector<AnalyzedCallArgument> &result) const;

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

  [[nodiscard]] static bool tryInferTypeArguments(
      const SemanticType &pattern, const SemanticType &argument,
      const std::vector<GenericParameterInfo> &parameters,
      GenericSubstitution &substitution, const ExprPtr *expression = nullptr,
      GenericParameterId *conflictingParameter = nullptr,
      bool *conflictingValueParameter = nullptr);

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

  [[nodiscard]] static bool containsExactType(const SemanticType &type,
                                              const SemanticType &candidate);

  [[nodiscard]] bool
  containsDisallowedCallableCaptureState(const SemanticType &type,
                                         std::unordered_set<ClassId> &visiting,
                                         bool reviewedRawState = false) const;

  [[nodiscard]] bool
  containsDisallowedCallableCaptureState(const SemanticType &type) const;

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
      ContextualCallArguments *contextualArguments = nullptr);

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
                     CallReceiverMode receiverMode);

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

  [[nodiscard]] ContextualCallArguments
  analyzeContextualCallArguments(const ExprList &arguments);

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

  void analyzeConstructorCallArguments(
      const Expr &construction, ClassId classId,
      const std::vector<SemanticType> &typeArguments,
      const std::vector<CompileTimeValue> &valueArguments,
      std::vector<AnalyzedCallArgument> arguments, const Token &paren,
      bool requireExactNullParameter = false);

  void analyzeExpectedMemberCall(const Call &call, const Get &member,
                                 const SemanticType &expected,
                                 const std::vector<SemanticType> &argumentTypes,
                                 const Token &paren);

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
                                  bool genericFunctionParameter = false);

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
                       std::vector<std::string> scope);
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

  [[nodiscard]] ClassId selectSignatureClassDefinition(
      ClassId id, const std::vector<SemanticType> &arguments) const;

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
                                std::vector<std::string> scope);

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

  struct PredeclaredTextBorrowSource {
    bool known = false;
    bool ownerDependent = false;
    BorrowOriginKind origin = BorrowOriginKind::None;
    std::size_t parameter = 0;
  };

  [[nodiscard]] PredeclaredTextBorrowSource
  predeclaredTextBorrowSource(const ExprPtr &expression,
                              const FunctionInfo &function);

  std::unordered_set<const FunctionDecl *> activeTextBorrowSummaries;

  [[nodiscard]] std::optional<FunctionReturnBorrowSummary>
  dynamicTextReturnBorrowSummary(const FunctionInfo &function);

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
      std::span<const ExprPtr> arguments);

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

  void recordProgramCallEffects(const Expr &expression);

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

  SemanticType analyze(const ExprPtr &expr);

  SemanticType analyze(const ExprPtr &expr, OccurrenceRole roles);

  SemanticType
  analyzeExpectedCallableResult(const ExprPtr &expr,
                                const SemanticType &expectedType,
                                bool inferFromInstantiation = false);

  SemanticType analyzeInitializer(const ExprPtr &expr,
                                  const SemanticType &expectedType,
                                  bool allowExactNullConstruction = false);

  [[nodiscard]] bool isDefaultLibraryUnit(SourceUnitId sourceUnit) const;

  [[nodiscard]] bool isPreludeUnit(SourceUnitId sourceUnit) const;

  [[nodiscard]] bool isThreadLibraryUnit(SourceUnitId sourceUnit) const;

  [[nodiscard]] bool isAtomicLibraryUnit(SourceUnitId sourceUnit) const;

  [[nodiscard]] bool isMutexLibraryUnit(SourceUnitId sourceUnit) const;

  [[nodiscard]] bool isMemoryLibraryUnit(SourceUnitId sourceUnit) const;
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
  void analyzeMetaFieldAccess(const Call &expr, const Get &member,
                              const SemanticType &fieldObject,
                              bool mutableAccess);
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
      bool substitutedCallable = false, bool mutableReceiver = true) const;

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

  [[nodiscard]] std::optional<ResolvedPayloadPatternInfo>
  analyzePayloadSwitchPattern(const ExprPtr &expression,
                              const SemanticType &subjectType);

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

  [[nodiscard]] FreshTemporaryValueAssessment
  freshTemporaryValue(const ExprPtr &expression) const;

  // Receiver-only extension. Temporary reference arguments retain the stricter
  // self-contained-value rule in freshTemporaryValue.
  [[nodiscard]] FreshTemporaryValueAssessment
  freshTemporaryReceiverValue(const ExprPtr &expression);

  [[nodiscard]] bool
  freshTemporaryMethodSupported(const CallableCandidate &function) const;

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
                    const RawInvocation *invocation = nullptr);
  [[nodiscard]] std::shared_ptr<RawResultQuery>
  rawResultQuery(const Call &call, const ResolvedCallInfo &resolved);
  [[nodiscard]] std::shared_ptr<RawResultQuery>
  rawResultQuery(const ResolvedCallInfo &resolved, SemanticType owner);
  [[nodiscard]] std::shared_ptr<RawResultQuery>
  rawConstructorResultQuery(const ResolvedConstructionInfo &resolved);
  void runRawResultQuery(const std::shared_ptr<RawResultQuery> &query);
  void recordRawResult(const ExprPtr &expression);
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

  [[nodiscard]] static bool isExpectedVoid(const SemanticType &type);

  [[nodiscard]] SemanticType numericResult(SemanticType left,
                                           SemanticType right,
                                           const Expr *leftExpression,
                                           const Expr *rightExpression) const;

  [[nodiscard]] bool isOwnershipAssignable(const SemanticType &target,
                                           const SemanticType &value,
                                           const ExprPtr &expression) const;

  [[nodiscard]] bool isOwnershipAssignment(const SemanticType &target,
                                           const SemanticType &value,
                                           const ExprPtr &expression) const;

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
              callableDeclarationsBySyntax.size(),
              callableDeclarations.size(),
              constructorGenericParameters.size(),
              rootNativeStorageSymbols.size(),
              globalBorrowStorage.size(),
              genericConstraints.size(),
              genericConstraintConcepts.size(),
              classes.size(),
              exactClassSpecializations.size(),
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

    void clear() {
      scopes.clear();
      rawOperatorResults.clear();
      controlFlow.clear();
      rawPointerLoops.clear();
      nextRawPointerLoop = 1;
      namespaceSymbols.clear();
      internalNamespaceSymbols.clear();
      visibleNamespaceSymbols.clear();
    }

    void beginBody(const SemanticCatalog &catalog) {
      namespaceSymbols = catalog.namespaceSymbols;
      internalNamespaceSymbols = catalog.internalNamespaceSymbols;
      visibleNamespaceSymbols = catalog.visibleNamespaceSymbols;
    }
  };

  // One public check() owns one complete request session. The references below
  // are a migration seam that keeps the analyzer algorithms readable while
  // R-STRUCT-02 moves catalog and flow algorithms behind narrower services.
  // They do not introduce another owner: every binding refers into this one
  // object, and resetRequestState() is its only public-entry reset operation.
  struct RequestSessionState {
    struct MutableLocalCandidate {
      SymbolId symbol = 0;
      Token keyword;
      Token name;
      ProgramEffectOwner owner;
    };
    struct UnsafeBlockCandidate {
      const BlockStmt *block = nullptr;
      Token keyword;
      ProgramEffectOwner owner;
    };

    std::vector<SemanticDiagnostic> diagnostics;
    SemanticModel semanticModel;
    // Diagnostic construction lives in its own translation unit
    // (semantic_diagnostics.cpp); the reporter binds this session's sink and
    // model, and analysis facts arrive as call parameters.
    SemanticDiagnosticReporter reporter{semanticModel, diagnostics};
    SemanticCatalog catalog;
    ValueFlowState valueFlow;
    std::unordered_set<std::string> reportedCompilerPrivateAccess;
    std::unordered_map<const Expr *, const ParameterDefault *>
        defaultArgumentSyntax;
    std::unordered_map<const Expr *, std::vector<const Expr *>>
        defaultArgumentDependencies;
    std::unordered_set<const Expr *> reportedDefaultArgumentCycles;
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
    std::vector<const BlockStmt *> activeUnsafeBlocks;
    std::unordered_set<const BlockStmt *> usedUnsafeBlocks;
    TypeSubstitution instanceTypeSubstitution;
    ValueSubstitution instanceValueSubstitution;
    std::vector<std::string> currentNamespace;
    SourceUnitId currentSourceUnit = 0;
    std::unordered_set<const VariableDecl *> structuredBindingElements;
    // Recovery-only flow facts: never published as valid exhaustive matches.
    std::unordered_set<const SwitchStmt *> recoveredExhaustiveSwitches;
    std::vector<std::unordered_map<std::string, Token>> lambdaUncapturedLocals;
    bool measureTopLevelStatements = false;
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
    // Non-null only while analysing the compiler-generated read-only range
    // owner initializer. An exact expected.value() at this identity may
    // publish the bounded receiver-rooted payload borrow.
    const Expr *expectedPayloadRangeCandidate = nullptr;
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
    const FunctionDecl *currentFunctionDeclaration = nullptr;
    std::optional<SemanticType> contextualInitializerType;
    const Expr *contextualInitializerExpression = nullptr;
    std::optional<SemanticType> contextualOperatorIntegerType;
    std::optional<ContextualCallableResult> contextualCallableResult;
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
    std::shared_ptr<RawResultQuery> collectingRawResult;
    std::size_t rawAccessObservation = 0;
    bool hasCThreadRequirements = false;
    std::unordered_map<ProgramEffectOwner, ProgramEffectSummary,
                       ProgramEffectOwnerHash>
        programEffects;
    std::shared_ptr<ProgramConcreteEffectState> programConcreteEffects;
    std::size_t programStoragePlaceUseDepth = 0;
    LambdaId nextLambdaId = 1;
    SemanticLoanId nextSemanticLoanId = 1;
    std::size_t nextPlaceSelectionId = 1;
    std::vector<SemanticType> invalidOrdinaryLayouts;
  } requestSession;

  std::vector<SemanticDiagnostic> &diagnostics = requestSession.diagnostics;
  SemanticModel &semanticModel = requestSession.semanticModel;
  SemanticDiagnosticReporter &reporter = requestSession.reporter;
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
  const Expr *&expectedPayloadRangeCandidate =
      requestSession.expectedPayloadRangeCandidate;
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
  const FunctionDecl *&currentFunctionDeclaration =
      requestSession.currentFunctionDeclaration;
  std::optional<SemanticType> &contextualInitializerType =
      requestSession.contextualInitializerType;
  const Expr *&contextualInitializerExpression =
      requestSession.contextualInitializerExpression;
  std::optional<SemanticType> &contextualOperatorIntegerType =
      requestSession.contextualOperatorIntegerType;
  std::optional<ContextualCallableResult> &contextualCallableResult =
      requestSession.contextualCallableResult;
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
};

} // namespace lang
