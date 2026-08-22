#include "cpp_mir_representation_snapshot.h"

#include "cpp_representation.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace lang {

class CppMirRepresentationSnapshotBuilderAccess {
public:
  static void seal(CppMirRepresentationSnapshot &snapshot) {
    snapshot.inventorySeal_ = CppMirRepresentationSnapshot::InventorySeal{
        .mir = snapshot.mir,
        .bodies = snapshot.bodies,
        .data = snapshot.data,
        .declarationRoots = snapshot.declarationRoots,
        .thunks = snapshot.thunks};
  }
};

namespace {

struct ConstexprDeclaration {
  const VariableDecl *declaration = nullptr;
  const ClassDecl *owner = nullptr;
};

struct SourceDeclaration {
  CppMirDataKind kind = CppMirDataKind::OtherDeclaration;
  const Stmt *declaration = nullptr;
  const ClassDecl *owner = nullptr;
  std::size_t traversalOrdinal = 0;
};

struct ProgramDeclarationInventory {
  std::unordered_set<const ClassDecl *> classes;
  std::unordered_set<const EnumDecl *> enums;
  std::unordered_set<const FunctionDecl *> functions;
  std::unordered_set<const ConstructorDecl *> constructors;
  std::unordered_set<const DestructorDecl *> destructors;
  std::unordered_set<const VariableDecl *> variables;
  std::unordered_set<const Lambda *> lambdas;
  std::vector<const EnumDecl *> orderedEnums;
  std::vector<const ClassDecl *> orderedClasses;
  std::vector<ConstexprDeclaration> constexprBindings;
  std::vector<SourceDeclaration> sourceDeclarations;
  std::size_t nextSourceDeclarationOrdinal = 1;

  std::size_t addSourceDeclaration(CppMirDataKind kind, const Stmt *declaration,
                                   const ClassDecl *owner = nullptr) {
    const std::size_t ordinal = nextSourceDeclarationOrdinal++;
    sourceDeclarations.push_back({.kind = kind,
                                  .declaration = declaration,
                                  .owner = owner,
                                  .traversalOrdinal = ordinal});
    return ordinal;
  }

  void addSourceDeclaration(CppMirDataKind kind, const Stmt *declaration,
                            const ClassDecl *owner,
                            std::size_t traversalOrdinal) {
    sourceDeclarations.push_back({.kind = kind,
                                  .declaration = declaration,
                                  .owner = owner,
                                  .traversalOrdinal = traversalOrdinal});
  }
};

template <typename Symbol>
[[nodiscard]] std::string cppProgramStorageSpelling(const Symbol &record,
                                                    SymbolId symbol) {
  if (record.kind != SymbolKind::GlobalVariable || record.name.empty()) {
    return {};
  }

  std::string_view qualified = record.qualifiedName;
  if (qualified.empty()) {
    qualified = record.name;
  }
  if (qualified.size() < record.name.size() ||
      qualified.substr(qualified.size() - record.name.size()) != record.name) {
    return {};
  }

  std::string spelling = "::__gti_program";
  if (qualified.size() != record.name.size()) {
    const std::size_t nameOffset = qualified.size() - record.name.size();
    if (nameOffset < 2 || qualified.substr(nameOffset - 2, 2) != "::") {
      return {};
    }
    const std::string_view scopes = qualified.substr(0, nameOffset - 2);
    std::size_t begin = 0;
    std::size_t index = 0;
    while (begin < scopes.size()) {
      const std::size_t end = scopes.find("::", begin);
      const std::string_view scope = scopes.substr(
          begin,
          end == std::string_view::npos ? std::string_view::npos : end - begin);
      if (scope.empty()) {
        return {};
      }
      spelling += "::";
      spelling += index == 0 && scope == "std"
                      ? std::string(cppEmittedStandardNamespace)
                      : std::string(scope);
      ++index;
      if (end == std::string_view::npos) {
        break;
      }
      begin = end + 2;
    }
  }

  spelling += "::";
  spelling += record.internalLinkage
                  ? cppStaticStorageValueSpelling(symbol, record.name)
                  : record.name;
  return spelling;
}

[[nodiscard]] bool containsDependentType(const SemanticType &type) {
  if (type.kind == SemanticType::TypeParameter ||
      type.kind == SemanticType::TypePack || type.arrayLengthParameterId != 0 ||
      std::any_of(type.valueArguments.begin(), type.valueArguments.end(),
                  [](const CompileTimeValue &value) {
                    return value.kind == CompileTimeValue::Parameter;
                  })) {
    return true;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     containsDependentType);
}

class ProgramLambdaCollector final : public ExprVisitor, public StmtVisitor {
public:
  ProgramLambdaCollector(const TargetInfo &target,
                         std::unordered_set<const Lambda *> &lambdas)
      : target(target), lambdas(lambdas) {}

  void collect(const StmtList &statements) {
    for (const StmtPtr &statement : statements) {
      collect(statement);
    }
  }

  void visitAssignExpr(const Assign &expression) override {
    collect(expression.value());
  }
  void visitArrayInitializerExpr(const ArrayInitializer &expression) override {
    collect(expression.elements());
  }
  void visitBinaryExpr(const Binary &expression) override {
    collect(expression.left());
    collect(expression.right());
  }
  void visitCallExpr(const Call &expression) override {
    collect(expression.callee());
    collect(expression.arguments());
  }
  void visitConditionalExpr(const ConditionalExpr &expression) override {
    collect(expression.condition());
    collect(expression.thenExpression());
    collect(expression.elseExpression());
  }
  void visitConversionExpr(const Conversion &expression) override {
    collect(expression.value());
  }
  void
  visitDirectInitializerExpr(const DirectInitializer &expression) override {
    collect(expression.arguments());
  }
  void visitDereferenceSetExpr(const DereferenceSet &expression) override {
    collect(expression.object());
    collect(expression.value());
  }
  void visitGetExpr(const Get &expression) override {
    collect(expression.object());
  }
  void visitGroupingExpr(const Grouping &expression) override {
    collect(expression.expression());
  }
  void visitIndexExpr(const Index &expression) override {
    collect(expression.object());
    collect(expression.index());
  }
  void visitIndexSetExpr(const IndexSet &expression) override {
    collect(expression.object());
    collect(expression.index());
    collect(expression.value());
  }
  void visitLambdaExpr(const Lambda &expression) override {
    lambdas.insert(&expression);
    for (const LambdaCapture &capture : expression.captures()) {
      collect(capture.initializer);
    }
    collect(expression.body());
  }
  void visitLayoutQueryExpr(const LayoutQuery &) override {}
  void visitLiteralExpr(const LiteralExpr &) override {}
  void visitLogicalExpr(const Logical &expression) override {
    collect(expression.left());
    collect(expression.right());
  }
  void visitPackFoldExpr(const PackFold &expression) override {
    collect(expression.pattern());
  }
  void visitPackExpansionExpr(const PackExpansion &) override {}
  void visitPostfixExpr(const Postfix &expression) override {
    collect(expression.expression());
  }
  void visitQualifiedNameExpr(const QualifiedName &) override {}
  void visitThisExpr(const This &) override {}
  void visitSetExpr(const Set &expression) override {
    collect(expression.object());
    collect(expression.value());
  }
  void visitUnaryExpr(const Unary &expression) override {
    collect(expression.right());
  }
  void visitUnexpectedExpr(const Unexpected &expression) override {
    collect(expression.error());
  }
  void visitVariableExpr(const Variable &) override {}

  void visitAccessSpecifierDecl(const AccessSpecifierDecl &) override {}
  void visitBlockStmt(const BlockStmt &statement) override {
    collect(statement.statements());
  }
  void visitClassDecl(const ClassDecl &statement) override {
    collect(statement.members());
  }
  void visitCompileErrorDirective(const CompileErrorDirective &) override {}
  void visitConceptDecl(const ConceptDecl &) override {}
  void visitConditionalStmt(const ConditionalStmt &statement) override {
    if (const StmtList *active = statement.activeBranch(target)) {
      collect(*active);
    }
  }
  void visitConstructorDecl(const ConstructorDecl &statement) override {
    for (const ConstructorInitializer &initializer : statement.initializers()) {
      collect(initializer.arguments);
    }
    if (statement.body()) {
      statement.body()->accept(*this);
    }
  }
  void visitDestructorDecl(const DestructorDecl &statement) override {
    if (statement.body()) {
      statement.body()->accept(*this);
    }
  }
  void visitDoWhileStmt(const DoWhileStmt &statement) override {
    collect(statement.body());
    collect(statement.condition());
  }
  void visitEmptyStmt(const EmptyStmt &) override {}
  void visitEnumDecl(const EnumDecl &statement) override {
    for (const EnumeratorDecl &enumerator : statement.enumerators()) {
      collect(enumerator.initializer);
    }
  }
  void visitExternCDecl(const ExternCDecl &statement) override {
    collect(statement.declarations());
  }
  void visitExpressionStmt(const ExpressionStmt &statement) override {
    collect(statement.expression());
  }
  void visitForStmt(const ForStmt &statement) override {
    collect(statement.initializer());
    collect(statement.condition());
    collect(statement.increment());
    collect(statement.body());
  }
  void visitFunctionDecl(const FunctionDecl &statement) override {
    if (statement.body()) {
      statement.body()->accept(*this);
    }
  }
  void visitIfStmt(const IfStmt &statement) override {
    collect(statement.condition());
    collect(statement.thenBranch());
    collect(statement.elseBranch());
  }
  void visitLoopControlStmt(const LoopControlStmt &) override {}
  void visitNamespaceAliasDecl(const NamespaceAliasDecl &) override {}
  void visitNamespaceDecl(const NamespaceDecl &statement) override {
    collect(statement.declarations());
  }
  void visitRangeForStmt(const RangeForStmt &statement) override {
    collect(statement.lowered());
  }
  void visitReturnStmt(const ReturnStmt &statement) override {
    collect(statement.value());
  }
  void visitSwitchStmt(const SwitchStmt &statement) override {
    collect(statement.expression());
    for (const SwitchArm &arm : statement.arms()) {
      for (const SwitchLabel &label : arm.labels) {
        collect(label.value);
      }
      collect(arm.statements);
    }
  }
  void
  visitStructuredBindingDecl(const StructuredBindingDecl &statement) override {
    collect(statement.initializer());
  }
  void visitTypeAliasDecl(const TypeAliasDecl &) override {}
  void visitVariableDecl(const VariableDecl &statement) override {
    collect(statement.initializer());
  }
  void visitWhileStmt(const WhileStmt &statement) override {
    collect(statement.condition());
    collect(statement.body());
  }

private:
  void collect(const ExprPtr &expression) {
    if (expression) {
      expression->accept(*this);
    }
  }
  void collect(const ExprList &expressions) {
    for (const ExprPtr &expression : expressions) {
      collect(expression);
    }
  }
  void collect(const StmtPtr &statement) {
    if (statement) {
      statement->accept(*this);
    }
  }

  const TargetInfo &target;
  std::unordered_set<const Lambda *> &lambdas;
};

void collectProgramDeclarations(const StmtList &declarations,
                                const TargetInfo &target,
                                ProgramDeclarationInventory &inventory,
                                const ClassDecl *owner = nullptr) {
  for (const StmtPtr &statement : declarations) {
    if (const auto *conditional =
            dynamic_cast<const ConditionalStmt *>(statement.get())) {
      if (const StmtList *active = conditional->activeBranch(target)) {
        collectProgramDeclarations(*active, target, inventory, owner);
      }
      continue;
    }
    if (const auto *nameSpace =
            dynamic_cast<const NamespaceDecl *>(statement.get())) {
      inventory.addSourceDeclaration(CppMirDataKind::NamespaceDeclaration,
                                     nameSpace, owner);
      collectProgramDeclarations(nameSpace->declarations(), target, inventory,
                                 owner);
      continue;
    }
    if (const auto *external =
            dynamic_cast<const ExternCDecl *>(statement.get())) {
      inventory.addSourceDeclaration(CppMirDataKind::LanguageLinkageDeclaration,
                                     external, owner);
      collectProgramDeclarations(external->declarations(), target, inventory,
                                 owner);
      continue;
    }
    if (const auto *classDeclaration =
            dynamic_cast<const ClassDecl *>(statement.get())) {
      inventory.addSourceDeclaration(CppMirDataKind::ClassDeclaration,
                                     classDeclaration, owner);
      inventory.classes.insert(classDeclaration);
      inventory.orderedClasses.push_back(classDeclaration);
      collectProgramDeclarations(classDeclaration->members(), target, inventory,
                                 classDeclaration);
      continue;
    }
    if (const auto *enumeration =
            dynamic_cast<const EnumDecl *>(statement.get())) {
      inventory.enums.insert(enumeration);
      inventory.orderedEnums.push_back(enumeration);
      continue;
    }
    if (const auto *function =
            dynamic_cast<const FunctionDecl *>(statement.get())) {
      const std::size_t ordinal = inventory.addSourceDeclaration(
          CppMirDataKind::CallableDeclaration, function, owner);
      if (!function->genericParameters().empty()) {
        inventory.addSourceDeclaration(
            CppMirDataKind::CallableTemplateDeclaration, function, owner,
            ordinal);
      }
      inventory.functions.insert(function);
      continue;
    }
    if (const auto *constructor =
            dynamic_cast<const ConstructorDecl *>(statement.get())) {
      const std::size_t ordinal = inventory.addSourceDeclaration(
          CppMirDataKind::CallableDeclaration, constructor, owner);
      if (!constructor->genericParameters().empty()) {
        inventory.addSourceDeclaration(
            CppMirDataKind::CallableTemplateDeclaration, constructor, owner,
            ordinal);
      }
      inventory.constructors.insert(constructor);
      continue;
    }
    if (const auto *destructor =
            dynamic_cast<const DestructorDecl *>(statement.get())) {
      inventory.addSourceDeclaration(CppMirDataKind::CallableDeclaration,
                                     destructor, owner);
      inventory.destructors.insert(destructor);
      continue;
    }
    if (const auto *variable =
            dynamic_cast<const VariableDecl *>(statement.get())) {
      inventory.addSourceDeclaration(CppMirDataKind::StorageDeclaration,
                                     variable, owner);
      inventory.variables.insert(variable);
      if (variable->isConstexpr()) {
        inventory.constexprBindings.push_back(
            {.declaration = variable, .owner = owner});
      }
      continue;
    }
    if (const auto *alias =
            dynamic_cast<const NamespaceAliasDecl *>(statement.get())) {
      inventory.addSourceDeclaration(CppMirDataKind::NamespaceAliasDeclaration,
                                     alias, owner);
      continue;
    }
    if (const auto *alias =
            dynamic_cast<const TypeAliasDecl *>(statement.get())) {
      inventory.addSourceDeclaration(CppMirDataKind::TypeAliasDeclaration,
                                     alias, owner);
      continue;
    }
    if (const auto *access =
            dynamic_cast<const AccessSpecifierDecl *>(statement.get())) {
      inventory.addSourceDeclaration(CppMirDataKind::AccessDeclaration, access,
                                     owner);
      continue;
    }
    if (const auto *empty = dynamic_cast<const EmptyStmt *>(statement.get())) {
      inventory.addSourceDeclaration(CppMirDataKind::EmptyDeclaration, empty,
                                     owner);
      continue;
    }
    // Any newly admitted scope-level statement remains a conservative source
    // declaration row until this exhaustive classifier gives it a narrower
    // representation kind. New AST surface therefore cannot make an otherwise
    // empty program look Complete merely by being absent from this inventory.
    inventory.addSourceDeclaration(CppMirDataKind::OtherDeclaration,
                                   statement.get(), owner);
  }
}

void addIssue(CppMirRepresentationSnapshotBuild &build,
              CppMirRepresentationSnapshotIssueKind kind, std::string detail) {
  build.issues.push_back({.kind = kind, .detail = std::move(detail)});
}

[[nodiscard]] bool isInitializerBody(MirBodyKind kind) {
  return kind == MirBodyKind::Module ||
         kind == MirBodyKind::FieldInitializers ||
         kind == MirBodyKind::StaticFieldInitializers;
}

[[nodiscard]] bool isCanonicalNoExecutionInitializer(const MirBody &body) {
  if (!isInitializerBody(body.kind) || body.returnType != SemanticType::Void ||
      body.entry != 1 || body.blocks.size() != 1 || !body.places.empty() ||
      !body.loans.empty() || !body.fullExpressions.empty() ||
      !body.cleanupBoundaries.empty() || !body.dropObligations.empty() ||
      !body.failureRecords.empty() || !body.values.empty() ||
      !body.valueUses.empty()) {
    return false;
  }
  MirBlock expected;
  expected.id = 1;
  expected.terminator.kind = MirTerminatorKind::Exit;
  expected.reachable = true;
  return body.blocks.front() == expected;
}

[[nodiscard]] bool
hasExecutableProgramInitialization(const HirProgram &program) {
  return std::any_of(
      program.programInitializationPlan().steps.begin(),
      program.programInitializationPlan().steps.end(), [](const auto &step) {
        return step.role == ProgramInitializationStepRole::Initializer;
      });
}

[[nodiscard]] bool
hasExecutableProgramInitialization(const MirProgram &program) {
  return std::any_of(
      program.programInitializationPlan().steps.begin(),
      program.programInitializationPlan().steps.end(), [](const auto &step) {
        return step.role == ProgramInitializationStepRole::Initializer;
      });
}

[[nodiscard]] const HirBinding *findBinding(const HirBody &body,
                                            HirBindingId id) {
  const auto found = std::find_if(
      body.bindings.begin(), body.bindings.end(),
      [id](const HirBinding &binding) { return binding.id == id; });
  return found == body.bindings.end() ? nullptr : &*found;
}

[[nodiscard]] const HirFullExpression *
findFullExpression(const HirBody &body, HirFullExpressionId id) {
  const auto found =
      std::find_if(body.fullExpressions.begin(), body.fullExpressions.end(),
                   [id](const HirFullExpression &expression) {
                     return expression.id == id;
                   });
  return found == body.fullExpressions.end() ? nullptr : &*found;
}

[[nodiscard]] bool exactProgramInitializationSnapshotsMatch(
    const HirProgram &hir, const MirProgram &mir, std::string &mismatch) {
  const HirProgramInitializationPlan &source = hir.programInitializationPlan();
  const MirProgramInitializationPlan &lowered = mir.programInitializationPlan();
  const HirBody &sourceModule = hir.module();
  const MirBody &loweredModule = mir.module();
  const auto reject = [&](std::string detail) {
    mismatch = std::move(detail);
    return false;
  };

  if (source.unitOrder.size() != lowered.units.size()) {
    return reject("HIR and MIR program-initialization unit counts differ");
  }
  for (std::size_t index = 0; index < source.unitOrder.size(); ++index) {
    std::vector<ProgramInitializationStepId> expectedSteps;
    for (const HirProgramInitializationStep &step : source.steps) {
      if (step.sourceUnit == source.unitOrder[index]) {
        expectedSteps.push_back(step.id);
      }
    }
    if (lowered.units[index].sourceUnit != source.unitOrder[index] ||
        lowered.units[index].steps != expectedSteps) {
      return reject("HIR and MIR program-initialization unit order differs");
    }
  }
  if (source.steps.size() != lowered.steps.size()) {
    return reject("HIR and MIR program-initialization step counts differ");
  }

  std::vector<MirProgramConstantSubstitution> expectedSubstitutions;
  for (const HirValue &value : sourceModule.values) {
    if (!value.programConstantSubstitution) {
      continue;
    }
    if (!value.constant) {
      return reject("HIR program-constant substitution has no exact value");
    }
    expectedSubstitutions.push_back(
        {.hirValue = value.id, .constant = *value.constant});
  }
  if (loweredModule.programConstantSubstitutions != expectedSubstitutions) {
    return reject("HIR and MIR program-constant substitutions differ");
  }

  for (std::size_t index = 0; index < source.steps.size(); ++index) {
    const HirProgramInitializationStep &sourceStep = source.steps[index];
    const MirProgramInitializationStep &loweredStep = lowered.steps[index];
    if (loweredStep.id != sourceStep.id ||
        loweredStep.sourceUnit != sourceStep.sourceUnit ||
        loweredStep.storageKind != sourceStep.kind ||
        loweredStep.role != sourceStep.role ||
        loweredStep.symbol != sourceStep.symbol ||
        loweredStep.ownerClass != sourceStep.ownerClass ||
        loweredStep.requiresActiveCleanup != sourceStep.requiresActiveCleanup ||
        loweredStep.binding != sourceStep.binding) {
      return reject("HIR and MIR program-initialization step identity differs");
    }

    const HirBinding *binding = findBinding(sourceModule, sourceStep.binding);
    const MirPlace *storage = loweredModule.findPlace(loweredStep.storagePlace);
    if (binding == nullptr || storage == nullptr) {
      return reject("program-initialization step lacks exact storage");
    }
    MirPlace expectedStorage;
    expectedStorage.id = loweredStep.storagePlace;
    expectedStorage.root = MirPlaceRootKind::Binding;
    expectedStorage.binding = binding->id;
    expectedStorage.symbol = binding->info.symbol;
    expectedStorage.type = binding->info.type;
    expectedStorage.access = binding->info.access;
    expectedStorage.traits = binding->info.traits;
    expectedStorage.key = PlaceKey{.domain = loweredModule.placeDomain,
                                   .root = binding->info.symbol};
    if (*storage != expectedStorage) {
      return reject("HIR binding and MIR program storage place differ");
    }

    if (sourceStep.role == ProgramInitializationStepRole::DataOnly) {
      const MirProgramDataInitializationKind expectedKind =
          binding->info.constant
              ? MirProgramDataInitializationKind::Constant
              : MirProgramDataInitializationKind::ImplicitZero;
      if (loweredStep.dataInitialization != expectedKind ||
          loweredStep.dataConstant != binding->info.constant ||
          loweredStep.statement != 0 || loweredStep.initializer != 0 ||
          loweredStep.fullExpression != 0) {
        return reject("HIR and MIR data-only initialization facts differ");
      }
      continue;
    }

    const HirValue *initializer =
        sourceStep.initializer ? sourceModule.findValue(*sourceStep.initializer)
                               : nullptr;
    const HirFullExpression *sourceFull =
        initializer == nullptr
            ? nullptr
            : findFullExpression(sourceModule, initializer->fullExpression);
    const MirFullExpression *loweredFull =
        loweredStep.fullExpression == 0 ||
                loweredStep.fullExpression >
                    loweredModule.fullExpressions.size()
            ? nullptr
            : &loweredModule.fullExpressions[loweredStep.fullExpression - 1];
    if (!sourceStep.initializer || initializer == nullptr ||
        sourceFull == nullptr || loweredFull == nullptr ||
        loweredStep.dataInitialization !=
            MirProgramDataInitializationKind::None ||
        loweredStep.dataConstant ||
        loweredStep.statement != sourceStep.statement ||
        loweredStep.initializer != *sourceStep.initializer ||
        *loweredFull !=
            MirFullExpression{.id = loweredStep.fullExpression,
                              .hirExpression = sourceFull->id,
                              .statement = sourceFull->statement,
                              .constructorInitializer =
                                  sourceFull->constructorInitializer,
                              .roots = sourceFull->roots}) {
      return reject("HIR and MIR executable initialization facts differ");
    }
  }

  return true;
}

[[nodiscard]] const MirInstruction *findInstruction(const MirBody &body,
                                                    MirInstructionId id) {
  for (const MirBlock &block : body.blocks) {
    const auto found =
        std::find_if(block.instructions.begin(), block.instructions.end(),
                     [id](const MirInstruction &instruction) {
                       return instruction.id == id;
                     });
    if (found != block.instructions.end()) {
      return &*found;
    }
  }
  return nullptr;
}

[[nodiscard]] std::size_t
programInitializationBodyCallCount(const MirProgram &program) {
  const MirBody *body = program.hostedStartup();
  if (body == nullptr) {
    return 0;
  }
  std::size_t result = 0;
  for (const MirBlock &block : body->blocks) {
    result += static_cast<std::size_t>(std::count_if(
        block.instructions.begin(), block.instructions.end(),
        [](const MirInstruction &instruction) {
          return instruction.kind == MirInstructionKind::CallBody &&
                 instruction.bodyTarget ==
                     MirBodyAddress{.kind = MirBodyKind::Module, .owner = 0};
        }));
  }
  return result;
}

[[nodiscard]] bool exactHostedStartupSnapshotsMatch(const HirProgram &hir,
                                                    const MirProgram &mir,
                                                    std::string &mismatch) {
  const std::optional<HirHostedProgramEntryPlan> &source =
      hir.hostedProgramEntryPlan();
  const std::optional<MirHostedStartupPlan> &lowered = mir.hostedStartupPlan();
  const MirBody *body = mir.hostedStartup();
  const auto reject = [&](std::string detail) {
    mismatch = std::move(detail);
    return false;
  };
  if (source.has_value() != lowered.has_value() ||
      lowered.has_value() != (body != nullptr)) {
    return reject("HIR and MIR hosted-startup presence differs");
  }
  if (!source) {
    return true;
  }
  if (source->kind != lowered->kind || source->entry != lowered->entry ||
      source->appendFunction != lowered->appendFunction ||
      source->vectorConstructor != lowered->vectorConstructor ||
      source->stringConstructor != lowered->stringConstructor ||
      source->sourceUnit != lowered->sourceAnchor.sourceUnit ||
      source->mainAnchor.start != lowered->sourceAnchor.start ||
      source->mainAnchor.end != lowered->sourceAnchor.end ||
      source->mainAnchor.line != lowered->sourceAnchor.line ||
      lowered->exitPolicy != MirHostedStartupExitPolicy::ImmediateExit70 ||
      body->kind != MirBodyKind::HostedStartup || lowered->entry == 0 ||
      findMirBody(mir, {.kind = MirBodyKind::HostedStartup,
                        .owner = lowered->entry}) != body) {
    return reject("HIR and MIR hosted-startup identity differs");
  }

  for (const MirHostedStartupOperation &operation : lowered->operations) {
    const MirInstruction *instruction =
        operation.instruction == 0
            ? nullptr
            : findInstruction(*body, operation.instruction);
    if (operation.kind ==
            MirHostedStartupOperationKind::ValidateArgumentCount &&
        (instruction == nullptr ||
         instruction->definedFailure != source->validateCount)) {
      return reject("hosted count-validation failure provenance differs");
    }
    if (operation.kind == MirHostedStartupOperationKind::ConvertArgumentCount &&
        (instruction == nullptr ||
         instruction->definedFailure != source->convertCount)) {
      return reject("hosted count-conversion failure provenance differs");
    }
  }

  const bool initializationExpected = hasExecutableProgramInitialization(mir);
  const std::size_t planCalls = static_cast<std::size_t>(std::count_if(
      lowered->operations.begin(), lowered->operations.end(),
      [](const MirHostedStartupOperation &operation) {
        return operation.kind ==
               MirHostedStartupOperationKind::CallProgramInitialization;
      }));
  const std::size_t bodyCalls = programInitializationBodyCallCount(mir);
  if (planCalls != (initializationExpected ? 1U : 0U) ||
      bodyCalls != (initializationExpected ? 1U : 0U)) {
    return reject("hosted startup and merged program initialization differ");
  }
  return true;
}

[[nodiscard]] CppMirBodyRepresentation *
findBody(CppMirRepresentationSnapshot &snapshot, MirBodyAddress address) {
  const auto found =
      std::find_if(snapshot.bodies.begin(), snapshot.bodies.end(),
                   [&](const CppMirBodyRepresentation &body) {
                     return body.identity.address == address;
                   });
  return found == snapshot.bodies.end() ? nullptr : &*found;
}

[[nodiscard]] const HirBody *findHirBody(const HirProgram &program,
                                         MirBodyAddress address) {
  switch (address.kind) {
  case MirBodyKind::Module:
    return address.owner == 0 ? &program.module() : nullptr;
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
    if (address.owner == 0 || address.owner > program.classInstances().size()) {
      return nullptr;
    }
    return address.kind == MirBodyKind::FieldInitializers
               ? &program.classInstances()[address.owner - 1].fieldInitializers
               : &program.classInstances()[address.owner - 1]
                      .staticFieldInitializers;
  case MirBodyKind::Function:
    if (const HirFunctionInstance *instance =
            program.findFunctionInstance(address.owner)) {
      return &instance->body;
    }
    return nullptr;
  case MirBodyKind::Constructor:
    if (const HirConstructorInstance *instance =
            program.findConstructorInstance(address.owner)) {
      return &instance->body;
    }
    return nullptr;
  case MirBodyKind::Destructor:
    if (const HirDestructorInstance *instance =
            program.findDestructorInstance(address.owner)) {
      return &instance->body;
    }
    return nullptr;
  case MirBodyKind::Lambda:
    if (const HirLambda *instance = program.findLambda(address.owner)) {
      return &instance->body;
    }
    return nullptr;
  case MirBodyKind::HostedStartup:
    return nullptr;
  }
  return nullptr;
}

[[nodiscard]] bool
containsClassSource(const ProgramDeclarationInventory &inventory,
                    const ClassDecl *declaration) {
  return declaration != nullptr && inventory.classes.contains(declaration);
}

[[nodiscard]] bool
bodySourceBelongsToProgram(MirBodyAddress address,
                           const ProgramDeclarationInventory &inventory,
                           const HirProgram &hir) {
  switch (address.kind) {
  case MirBodyKind::Module:
    return address.owner == 0;
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
    return address.owner != 0 && address.owner <= hir.classInstances().size() &&
           containsClassSource(inventory,
                               hir.classInstances()[address.owner - 1].source);
  case MirBodyKind::Function:
    return address.owner != 0 &&
           address.owner <= hir.functionInstances().size() &&
           hir.functionInstances()[address.owner - 1].source != nullptr &&
           inventory.functions.contains(
               hir.functionInstances()[address.owner - 1].source);
  case MirBodyKind::Constructor: {
    if (address.owner == 0 ||
        address.owner > hir.constructorInstances().size()) {
      return false;
    }
    const HirConstructorInstance &instance =
        hir.constructorInstances()[address.owner - 1];
    return instance.source == nullptr ||
           inventory.constructors.contains(instance.source);
  }
  case MirBodyKind::Destructor: {
    if (address.owner == 0 ||
        address.owner > hir.destructorInstances().size()) {
      return false;
    }
    const HirDestructorInstance &instance =
        hir.destructorInstances()[address.owner - 1];
    return instance.source == nullptr ||
           inventory.destructors.contains(instance.source);
  }
  case MirBodyKind::Lambda:
    return address.owner != 0 &&
           address.owner <= hir.lambdaInstances().size() &&
           hir.lambdaInstances()[address.owner - 1].source != nullptr &&
           inventory.lambdas.contains(
               hir.lambdaInstances()[address.owner - 1].source);
  case MirBodyKind::HostedStartup: {
    const std::optional<HirHostedProgramEntryPlan> &startup =
        hir.hostedProgramEntryPlan();
    return startup && startup->entry == address.owner && address.owner != 0 &&
           address.owner <= hir.functionInstances().size() &&
           hir.functionInstances()[address.owner - 1].source != nullptr &&
           inventory.functions.contains(
               hir.functionInstances()[address.owner - 1].source);
  }
  }
  return false;
}

[[nodiscard]] std::size_t classOwnerId(const SemanticModel &semantics,
                                       const ClassDecl *owner) {
  if (owner == nullptr) {
    return 0;
  }
  const ClassTypeInfo *info = semantics.findClassType(*owner);
  return info == nullptr ? 0 : info->id;
}

[[nodiscard]] bool isAbiRepresentation(const ClassTypeInfo &info) {
  return info.cAbiRecord || info.cOpaqueHandle ||
         info.kind == ClassKind::Union || info.unionLayout.has_value();
}

void appendSourceDeclarationRows(const ProgramDeclarationInventory &inventory,
                                 const SemanticModel &semantics,
                                 CppMirRepresentationSnapshot &snapshot,
                                 CppMirRepresentationSnapshotBuild &build) {
  for (const SourceDeclaration &source : inventory.sourceDeclarations) {
    std::size_t declarationIdentity = source.traversalOrdinal;
    const std::size_t expectedOwner = classOwnerId(semantics, source.owner);
    if (source.owner != nullptr && expectedOwner == 0) {
      addIssue(
          build,
          CppMirRepresentationSnapshotIssueKind::MissingSemanticDeclaration,
          "source declaration owner has no semantic class identity");
    }

    if (const auto *declaration =
            dynamic_cast<const ClassDecl *>(source.declaration)) {
      const ClassTypeInfo *info = semantics.findClassType(*declaration);
      if (info == nullptr || info->id == 0 ||
          info->declaration != declaration) {
        addIssue(
            build,
            CppMirRepresentationSnapshotIssueKind::MissingSemanticDeclaration,
            "source class declaration has no exact semantic identity");
      } else {
        declarationIdentity = info->id;
      }
    } else if (const auto *declaration =
                   dynamic_cast<const FunctionDecl *>(source.declaration)) {
      const FunctionInfo *info = semantics.findFunction(*declaration);
      if (info == nullptr || info->id == 0 ||
          info->declaration != declaration ||
          info->ownerClass != expectedOwner) {
        addIssue(
            build,
            CppMirRepresentationSnapshotIssueKind::MissingSemanticDeclaration,
            "source function declaration has no exact semantic "
            "identity/owner");
      } else {
        declarationIdentity = info->id;
      }
    } else if (const auto *declaration =
                   dynamic_cast<const ConstructorDecl *>(source.declaration)) {
      const ConstructorInfo *info = semantics.findConstructor(*declaration);
      if (info == nullptr || info->id == 0 ||
          info->declaration != declaration || info->owner != expectedOwner) {
        addIssue(
            build,
            CppMirRepresentationSnapshotIssueKind::MissingSemanticDeclaration,
            "source constructor declaration has no exact semantic "
            "identity/owner");
      } else {
        declarationIdentity = info->id;
      }
    } else if (const auto *declaration =
                   dynamic_cast<const DestructorDecl *>(source.declaration)) {
      const DestructorInfo *info = semantics.findDestructor(*declaration);
      if (info == nullptr || info->declaration != declaration ||
          info->owner != expectedOwner) {
        addIssue(
            build,
            CppMirRepresentationSnapshotIssueKind::MissingSemanticDeclaration,
            "source destructor declaration has no exact semantic "
            "owner");
      } else {
        declarationIdentity = info->owner;
      }
    } else if (const auto *declaration =
                   dynamic_cast<const VariableDecl *>(source.declaration)) {
      const BindingInfo *info = semantics.findBinding(*declaration);
      const ClassTypeInfo *ownerInfo =
          source.owner == nullptr ? nullptr
                                  : semantics.findClassType(*source.owner);
      const auto isOwnedField =
          [&](const std::vector<ClassFieldTypeInfo> &fields) {
            return std::any_of(fields.begin(), fields.end(),
                               [&](const ClassFieldTypeInfo &field) {
                                 return field.declaration == declaration;
                               });
          };
      const bool ownerMatches =
          source.owner == nullptr ||
          (ownerInfo != nullptr && (isOwnedField(ownerInfo->fields) ||
                                    isOwnedField(ownerInfo->staticFields)));
      if (info == nullptr || info->symbol == 0 || !ownerMatches) {
        addIssue(
            build,
            CppMirRepresentationSnapshotIssueKind::MissingSemanticDeclaration,
            "source storage declaration has no exact semantic binding/owner "
            "identity");
      } else {
        declarationIdentity = info->symbol;
      }
    } else if (const auto *declaration =
                   dynamic_cast<const TypeAliasDecl *>(source.declaration)) {
      const TypeAliasInfo *info = semantics.findTypeAlias(*declaration);
      if (info == nullptr || info->declaration != declaration) {
        addIssue(
            build,
            CppMirRepresentationSnapshotIssueKind::MissingSemanticDeclaration,
            "source type alias has no exact semantic identity");
      }
    }

    snapshot.data.push_back({.identity = {.kind = source.kind,
                                          .declaration = declarationIdentity,
                                          .owner = expectedOwner,
                                          .ordinal = source.traversalOrdinal},
                             .support = CppMirSurfaceSupport::Supported});
  }
}

[[nodiscard]] bool
exactCAbiLayoutSnapshotsMatch(const HirClassInstance &source,
                              const MirClassInstance &lowered) {
  if (source.cAbiLayout.has_value() != lowered.cAbiLayout.has_value()) {
    return false;
  }
  if (!source.cAbiLayout) {
    return true;
  }
  if (source.cAbiLayout->sizeBytes != lowered.cAbiLayout->sizeBytes ||
      source.cAbiLayout->abiAlignmentBytes !=
          lowered.cAbiLayout->abiAlignmentBytes ||
      source.cAbiLayout->fields.size() != lowered.cAbiLayout->fields.size()) {
    return false;
  }
  for (std::size_t index = 0; index < source.cAbiLayout->fields.size();
       ++index) {
    const CAbiRecordFieldLayout &sourceField = source.cAbiLayout->fields[index];
    const MirCAbiRecordFieldLayout &loweredField =
        lowered.cAbiLayout->fields[index];
    const auto declaration =
        std::find_if(source.fields.begin(), source.fields.end(),
                     [&](const HirClassField &field) {
                       return field.declaration == sourceField.declaration;
                     });
    if (declaration == source.fields.end() ||
        declaration->info.symbol != loweredField.field ||
        sourceField.type != loweredField.type ||
        sourceField.offsetBytes != loweredField.offsetBytes ||
        sourceField.sizeBytes != loweredField.sizeBytes ||
        sourceField.abiAlignmentBytes != loweredField.abiAlignmentBytes) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool
exactUnionLayoutSnapshotsMatch(const HirClassInstance &source,
                               const MirClassInstance &lowered) {
  if (source.unionLayout.has_value() != lowered.unionLayout.has_value()) {
    return false;
  }
  if (!source.unionLayout) {
    return true;
  }
  if (source.unionLayout->sizeBytes != lowered.unionLayout->sizeBytes ||
      source.unionLayout->abiAlignmentBytes !=
          lowered.unionLayout->abiAlignmentBytes ||
      source.unionLayout->fields.size() != lowered.unionLayout->fields.size()) {
    return false;
  }
  for (std::size_t index = 0; index < source.unionLayout->fields.size();
       ++index) {
    const UnionFieldLayout &sourceField = source.unionLayout->fields[index];
    const MirUnionFieldLayout &loweredField =
        lowered.unionLayout->fields[index];
    const auto declaration =
        std::find_if(source.fields.begin(), source.fields.end(),
                     [&](const HirClassField &field) {
                       return field.declaration == sourceField.declaration;
                     });
    if (declaration == source.fields.end() ||
        declaration->info.symbol != loweredField.field ||
        sourceField.type != loweredField.type ||
        sourceField.sizeBytes != loweredField.sizeBytes ||
        sourceField.abiAlignmentBytes != loweredField.abiAlignmentBytes) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool
exactClassInstanceSnapshotsMatch(const HirClassInstance &source,
                                 const MirClassInstance &lowered) {
  if (source.id != lowered.id || source.declaration != lowered.declaration ||
      source.type != lowered.type || source.traits != lowered.traits ||
      source.kind != lowered.kind || source.bases != lowered.bases ||
      source.bases != lowered.structuralBases ||
      source.abstract != lowered.abstract ||
      source.polymorphic != lowered.polymorphic ||
      source.cAbiRecord != lowered.cAbiRecord ||
      !exactCAbiLayoutSnapshotsMatch(source, lowered) ||
      !exactUnionLayoutSnapshotsMatch(source, lowered) ||
      source.defaultConstructor != lowered.defaultConstructor ||
      source.copyConstructor != lowered.copyConstructor ||
      source.moveConstructor != lowered.moveConstructor ||
      source.copyAssignment != lowered.copyAssignment ||
      source.moveAssignment != lowered.moveAssignment ||
      source.destructorStatus != lowered.destructorStatus ||
      source.destructor != lowered.destructor ||
      source.requiresActiveDropState != lowered.requiresActiveDropState ||
      source.requiresActiveCleanup != lowered.requiresActiveCleanup ||
      source.fields.size() != lowered.declaredFields.size()) {
    return false;
  }

  std::vector<MirClassFieldLifecycle> lifecycleFields;
  lifecycleFields.reserve(source.fields.size());
  for (std::size_t index = 0; index < source.fields.size(); ++index) {
    const HirClassField &field = source.fields[index];
    const MirClassFieldInfo &mirField = lowered.declaredFields[index];
    if (field.binding != mirField.field ||
        field.info.symbol != mirField.symbol ||
        field.info.type != mirField.type ||
        field.info.traits.drop != mirField.dropKind ||
        field.requiresActiveCleanup != mirField.requiresActiveCleanup) {
      return false;
    }
    if (field.info.traits.drop == DropKind::Lexical) {
      lifecycleFields.push_back(
          {.field = field.binding,
           .symbol = field.info.symbol,
           .type = field.info.type,
           .dropKind = field.info.traits.drop,
           .requiresActiveCleanup = field.requiresActiveCleanup});
    }
  }
  if (lifecycleFields != lowered.fields ||
      lifecycleFields.size() != lowered.fieldDropOrder.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lifecycleFields.size(); ++index) {
    const MirClassFieldLifecycle &field =
        lifecycleFields[lifecycleFields.size() - index - 1];
    const MirFieldDrop &drop = lowered.fieldDropOrder[index];
    if (field.field != drop.field || field.symbol != drop.symbol ||
        field.type != drop.type ||
        field.requiresActiveCleanup != drop.requiresActiveCleanup) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool containsTemplateTypeParameter(const SemanticType &type) {
  if (type.kind == SemanticType::TypeParameter ||
      type.kind == SemanticType::TypePack) {
    return true;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     containsTemplateTypeParameter);
}

[[nodiscard]] bool containsLambdaType(const SemanticType &type) {
  if (type.kind == SemanticType::Lambda) {
    return true;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     containsLambdaType);
}

[[nodiscard]] bool applyTemplateTypeOverlay(
    CppMirBodyEmissionMapRows &rows,
    std::vector<std::pair<SemanticType, std::string>> &overlays,
    const SemanticType &concrete, std::string_view parameterName) {
  if (parameterName.empty() || concrete.kind == SemanticType::TypeParameter ||
      concrete.kind == SemanticType::TypePack) {
    return false;
  }
  const std::optional<CppMirTypeRepresentationKind> concreteKind =
      cppMirExpectedTypeRepresentation(concrete);
  if (!concreteKind) {
    return false;
  }
  const auto prior =
      std::find_if(overlays.begin(), overlays.end(), [&](const auto &overlay) {
        return overlay.first == concrete;
      });
  if (prior != overlays.end()) {
    return prior->second == parameterName;
  }
  const auto existing = std::find_if(rows.types.begin(), rows.types.end(),
                                     [&](const CppMirTypeRepresentation &row) {
                                       return row.type == concrete;
                                     });
  if (existing == rows.types.end()) {
    rows.types.push_back({.type = concrete,
                          .kind = *concreteKind,
                          .spelling = std::string(parameterName)});
  } else {
    if (existing->kind != *concreteKind) {
      return false;
    }
    existing->spelling = parameterName;
  }
  overlays.emplace_back(concrete, parameterName);
  return true;
}

} // namespace

bool cppMirFrontendSnapshotsMatch(const SemanticModel &semantics,
                                  const HirProgram &hir, const MirProgram &mir,
                                  std::string *mismatch) {
  const auto reject = [&](std::string detail) {
    if (mismatch != nullptr) {
      *mismatch = std::move(detail);
    }
    return false;
  };
  if (semantics.analysisSeal() != hir.analysisSeal()) {
    return reject("semantic and HIR analyzed-Program/target seals differ");
  }
  const MirVerificationResult mirVerification = verifyMirProgram(mir);
  if (!mirVerification.valid()) {
    return reject("MIR program verification failed: " +
                  (mirVerification.errors.empty()
                       ? std::string{"unknown MIR verification failure"}
                       : mirVerification.errors.front().message));
  }
  const HirProgramPlanVerificationResult planVerification =
      verifyHirProgramPlans(semantics, hir);
  if (!planVerification.valid()) {
    return reject("semantic and HIR program plans differ: " +
                  (planVerification.errors.empty()
                       ? std::string{"unknown plan-verification failure"}
                       : planVerification.errors.front()));
  }
  if (semantics.executionProfile() != hir.executionProfile() ||
      hir.executionProfile() != mir.executionProfile() ||
      semantics.placeSnapshot() != hir.module().placeDomain.snapshot ||
      hir.module().placeDomain != mir.module().placeDomain ||
      hir.classInstances().size() != mir.classInstances().size() ||
      hir.functionInstances().size() != mir.functionInstances().size() ||
      hir.nativeCallbackAdapters().size() !=
          mir.nativeCallbackAdapters().size() ||
      hir.constructorInstances().size() != mir.constructorInstances().size() ||
      hir.destructorInstances().size() != mir.destructorInstances().size() ||
      hir.lambdaInstances().size() != mir.lambdaInstances().size()) {
    return reject("module profile, place domain, or instance counts differ");
  }

  std::string initializationMismatch;
  if (!exactProgramInitializationSnapshotsMatch(hir, mir,
                                                initializationMismatch)) {
    return reject(std::move(initializationMismatch));
  }
  std::string startupMismatch;
  if (!exactHostedStartupSnapshotsMatch(hir, mir, startupMismatch)) {
    return reject(std::move(startupMismatch));
  }

  for (std::size_t index = 0; index < hir.classInstances().size(); ++index) {
    const HirClassInstance &source = hir.classInstances()[index];
    const MirClassInstance &lowered = mir.classInstances()[index];
    const ClassTypeInfo *info = source.source == nullptr
                                    ? nullptr
                                    : semantics.findClassType(*source.source);
    if (!exactClassInstanceSnapshotsMatch(source, lowered) ||
        source.fieldInitializers.placeDomain !=
            lowered.fieldInitializers.placeDomain ||
        source.staticFieldInitializers.placeDomain !=
            lowered.staticFieldInitializers.placeDomain ||
        info == nullptr || info->id != source.declaration ||
        info->declaration != source.source ||
        info->sourceUnit != source.sourceUnit) {
      return reject("class instance " + std::to_string(index + 1) + " differs");
    }
  }
  for (std::size_t index = 0; index < hir.functionInstances().size(); ++index) {
    const HirFunctionInstance &source = hir.functionInstances()[index];
    const MirFunctionInstance &lowered = mir.functionInstances()[index];
    const MirDefinitionKind definitionKind =
        source.source == nullptr           ? MirDefinitionKind::Declaration
        : source.source->runtimeBinding()  ? MirDefinitionKind::RuntimeBinding
        : source.source->body() != nullptr ? MirDefinitionKind::Source
                                           : MirDefinitionKind::Declaration;
    const FunctionInfo *info = source.source == nullptr
                                   ? nullptr
                                   : semantics.findFunction(*source.source);
    const std::optional<OverloadedOperator> overloadedOperator =
        source.source != nullptr && source.source->operatorName()
            ? std::optional<OverloadedOperator>{source.source->operatorName()
                                                    ->kind}
            : std::nullopt;
    const HirFunctionInstance *appendTarget =
        source.entryArgumentAppendTarget
            ? hir.findFunctionInstance(*source.entryArgumentAppendTarget)
            : nullptr;
    const bool appendTargetMatches =
        info != nullptr &&
        ((info->entryArgumentAppendFunction == 0 &&
          !source.entryArgumentAppendTarget) ||
         (info->entryArgumentAppendFunction != 0 && appendTarget != nullptr &&
          appendTarget->declaration == info->entryArgumentAppendFunction));
    if (source.id != lowered.id || source.declaration != lowered.declaration ||
        source.owner != lowered.owner ||
        source.returnType != lowered.returnType ||
        source.parameterTypes != lowered.parameterTypes ||
        source.parameterBindings != lowered.parameterBindings ||
        source.entryKind != lowered.entryKind ||
        (info != nullptr && info->entryKind != source.entryKind) ||
        (info != nullptr &&
         info->entryPoint != (source.entryKind != ProgramEntryKind::None)) ||
        !appendTargetMatches ||
        source.entryArgumentAppendTarget != lowered.entryArgumentAppendTarget ||
        source.staticMember != lowered.staticMember ||
        (source.source != nullptr &&
         source.source->receiverMutability() != lowered.receiverMutability) ||
        overloadedOperator != lowered.overloadedOperator ||
        source.constexprFunction != lowered.constexprFunction ||
        source.returnBorrowOrigin != lowered.returnBorrowOrigin ||
        source.returnBorrowParameter != lowered.returnBorrowParameter ||
        source.returnBorrowAccess != lowered.returnBorrowAccess ||
        source.returnBorrowPlace != lowered.returnBorrowPlace ||
        source.linkage != lowered.linkage ||
        source.externalSymbol != lowered.externalSymbol ||
        source.cArrayCountParameter != lowered.cArrayCountParameter ||
        source.virtualMethod != lowered.virtualMethod ||
        source.pureVirtual != lowered.pureVirtual ||
        source.overrideMethod != lowered.overrideMethod ||
        source.virtualRoots != lowered.virtualRoots ||
        definitionKind != lowered.definitionKind ||
        source.body.placeDomain != lowered.body.placeDomain ||
        info == nullptr || info->id != source.declaration ||
        info->declaration != source.source ||
        info->sourceUnit != source.sourceUnit) {
      return reject("function instance " + std::to_string(index + 1) +
                    " differs");
    }
  }
  for (std::size_t index = 0; index < hir.nativeCallbackAdapters().size();
       ++index) {
    const HirNativeCallbackAdapter &source =
        hir.nativeCallbackAdapters()[index];
    const MirNativeCallbackAdapter &lowered =
        mir.nativeCallbackAdapters()[index];
    const MirFunctionInstance *target =
        mir.findFunctionInstance(lowered.target);
    if (source.id != lowered.id || source.target != lowered.target ||
        source.type != lowered.type || target == nullptr ||
        lowered.targetMayRaiseDefinedFailure !=
            target->mayRaiseDefinedFailure ||
        lowered.failurePolicy !=
            MirNativeCallbackFailurePolicy::TerminateInvocation ||
        !lowered.catchesNativeExceptions) {
      return reject("native callback adapter " + std::to_string(index + 1) +
                    " differs");
    }
  }
  for (std::size_t index = 0; index < hir.constructorInstances().size();
       ++index) {
    const HirConstructorInstance &source = hir.constructorInstances()[index];
    const MirConstructorInstance &lowered = mir.constructorInstances()[index];
    const ConstructorInfo *info =
        source.source == nullptr ? nullptr
                                 : semantics.findConstructor(*source.source);
    const MirDefinitionKind definitionKind =
        source.source != nullptr && source.source->body() != nullptr
            ? MirDefinitionKind::Source
            : MirDefinitionKind::Declaration;
    const HirClassInstance *owner =
        source.owner == 0 || source.owner > hir.classInstances().size()
            ? nullptr
            : &hir.classInstances()[source.owner - 1];
    if (source.id != lowered.id || source.owner != lowered.owner ||
        lowered.definitionKind != definitionKind ||
        source.body.placeDomain != lowered.body.placeDomain ||
        (source.source != nullptr &&
         (info == nullptr || info->id != source.declaration ||
          info->declaration != source.source || owner == nullptr ||
          info->owner != owner->declaration)) ||
        (source.source == nullptr && source.declaration != 0)) {
      return reject("constructor instance " + std::to_string(index + 1) +
                    " differs");
    }
  }
  for (std::size_t index = 0; index < hir.destructorInstances().size();
       ++index) {
    const HirDestructorInstance &source = hir.destructorInstances()[index];
    const MirDestructorInstance &lowered = mir.destructorInstances()[index];
    const DestructorInfo *info = source.source == nullptr
                                     ? nullptr
                                     : semantics.findDestructor(*source.source);
    const MirDefinitionKind definitionKind =
        source.source != nullptr && source.source->body() != nullptr
            ? MirDefinitionKind::Source
            : MirDefinitionKind::Declaration;
    const HirClassInstance *owner =
        source.owner == 0 || source.owner > hir.classInstances().size()
            ? nullptr
            : &hir.classInstances()[source.owner - 1];
    if (source.id != lowered.id || source.owner != lowered.owner ||
        lowered.definitionKind != definitionKind ||
        source.body.placeDomain != lowered.body.placeDomain ||
        (source.source != nullptr &&
         (info == nullptr || info->declaration != source.source ||
          owner == nullptr || info->owner != owner->declaration))) {
      return reject("destructor instance " + std::to_string(index + 1) +
                    " differs");
    }
  }
  for (std::size_t index = 0; index < hir.lambdaInstances().size(); ++index) {
    const HirLambda &source = hir.lambdaInstances()[index];
    const MirLambdaInstance &lowered = mir.lambdaInstances()[index];
    const LambdaInfo *info = source.source == nullptr
                                 ? nullptr
                                 : semantics.findLambda(*source.source);
    if (source.id != lowered.id || source.declaration != lowered.declaration ||
        source.body.placeDomain != lowered.body.placeDomain ||
        info == nullptr || info->id != source.declaration ||
        info->declaration != source.source) {
      return reject("lambda instance " + std::to_string(index + 1) +
                    " differs");
    }
  }
  return true;
}

std::optional<std::size_t> cppMirApplyCallableTemplateTypeOverlays(
    CppMirBodyEmissionMapRows &rows, const SemanticModel &semantics,
    CppStandard standard, const FunctionInfo &declaration,
    const MirFunctionInstance &instance) {
  if (declaration.id == 0 || instance.declaration != declaration.id ||
      instance.parameterTypes.size() != declaration.parameterTypes.size()) {
    return std::nullopt;
  }

  std::vector<std::pair<SemanticType, std::string>> overlays;
  for (std::size_t index = 0; index < declaration.parameterTypes.size();
       ++index) {
    const SemanticType &declared = declaration.parameterTypes[index];
    const SemanticType &concrete = instance.parameterTypes[index];
    if (declared.kind == SemanticType::TypeParameter) {
      const GenericParameterInfo *parameter =
          semantics.findGenericParameter(declared.genericParameterId);
      if (parameter == nullptr ||
          !applyTemplateTypeOverlay(rows, overlays, concrete,
                                    parameter->name.lexeme)) {
        return std::nullopt;
      }
      continue;
    }
    if (declared != concrete && containsTemplateTypeParameter(declared) &&
        (concrete.kind == SemanticType::Lambda ||
         concrete.kind == SemanticType::Class) &&
        !applyTemplateTypeOverlay(
            rows, overlays, concrete,
            cppSemanticTypeSpelling(semantics, standard, declared))) {
      return std::nullopt;
    }
  }
  if (declaration.returnType != instance.returnType &&
      containsTemplateTypeParameter(declaration.returnType) &&
      (instance.returnType.kind == SemanticType::Lambda ||
       instance.returnType.kind == SemanticType::Class) &&
      !applyTemplateTypeOverlay(
          rows, overlays, instance.returnType,
          cppSemanticTypeSpelling(semantics, standard,
                                  declaration.returnType))) {
    return std::nullopt;
  }
  return overlays.size();
}

std::optional<std::size_t> cppMirApplyCallableTemplateTypeOverlays(
    CppMirBodyEmissionMapRows &rows, const LoweredProgram &program,
    CppStandard standard, const LoweredFunctionDeclaration &declaration,
    const MirFunctionInstance &instance) {
  if (declaration.id == 0 || instance.declaration != declaration.id ||
      instance.parameterTypes.size() != declaration.parameters.size()) {
    return std::nullopt;
  }

  std::vector<std::pair<SemanticType, std::string>> overlays;
  for (std::size_t index = 0; index < declaration.parameters.size(); ++index) {
    const SemanticType &declared = declaration.parameters[index].type;
    const SemanticType &concrete = instance.parameterTypes[index];
    if (declared.kind == SemanticType::TypeParameter) {
      const LoweredGenericParameter *parameter =
          program.findGenericParameter(declared.genericParameterId);
      if (parameter == nullptr ||
          !applyTemplateTypeOverlay(rows, overlays, concrete,
                                    parameter->name)) {
        return std::nullopt;
      }
      continue;
    }
    if (declared != concrete && containsTemplateTypeParameter(declared) &&
        (concrete.kind == SemanticType::Lambda ||
         concrete.kind == SemanticType::Class) &&
        !applyTemplateTypeOverlay(
            rows, overlays, concrete,
            cppSemanticTypeSpelling(program, standard, declared))) {
      return std::nullopt;
    }
  }
  if (declaration.returnType != instance.returnType &&
      containsTemplateTypeParameter(declaration.returnType) &&
      (instance.returnType.kind == SemanticType::Lambda ||
       instance.returnType.kind == SemanticType::Class) &&
      !applyTemplateTypeOverlay(
          rows, overlays, instance.returnType,
          cppSemanticTypeSpelling(program, standard, declaration.returnType))) {
    return std::nullopt;
  }
  return overlays.size();
}

std::optional<std::size_t> cppMirApplyGenericOwnerConstructorTypeOverlays(
    CppMirBodyEmissionMapRows &rows, const SemanticModel &semantics,
    const HirProgram &hir, const MirProgram &mir,
    const ConstructorDecl &declaration,
    const MirConstructorInstance &instance) {
  const HirConstructorInstance *source =
      hir.findConstructorInstance(instance.id);
  const MirClassInstance *mirOwner = mir.findClassInstance(instance.owner);
  const HirClassInstance *owner =
      instance.owner == 0 || instance.owner > hir.classInstances().size()
          ? nullptr
          : &hir.classInstances()[instance.owner - 1];
  const ClassTypeInfo *ownerInfo =
      owner == nullptr ? nullptr : semantics.findClassType(owner->declaration);
  const ConstructorInfo *constructorInfo =
      semantics.findConstructor(declaration);
  if (source == nullptr || source->source != &declaration || owner == nullptr ||
      mirOwner == nullptr || ownerInfo == nullptr ||
      constructorInfo == nullptr || owner->source == nullptr ||
      owner->id != mirOwner->id || owner->type != mirOwner->type ||
      source->owner != owner->id ||
      source->declaration != constructorInfo->id ||
      constructorInfo->declaration != &declaration ||
      constructorInfo->owner != ownerInfo->id ||
      source->parameterTypes != instance.parameterTypes ||
      ownerInfo->declaration != owner->source ||
      ownerInfo->genericParameters.empty() || owner->typeArguments.empty() ||
      !owner->valueArguments.empty()) {
    return std::nullopt;
  }

  std::vector<std::pair<SemanticType, std::string>> overlays;
  std::size_t typeIndex = 0;
  bool lambdaArgument = false;
  for (const GenericParameterInfo &parameter : ownerInfo->genericParameters) {
    if (parameter.pack || parameter.value ||
        typeIndex >= owner->typeArguments.size()) {
      return std::nullopt;
    }
    const SemanticType &argument = owner->typeArguments[typeIndex++];
    lambdaArgument = lambdaArgument || containsLambdaType(argument);
    if (!applyTemplateTypeOverlay(rows, overlays, argument,
                                  parameter.name.lexeme)) {
      return std::nullopt;
    }
  }
  if (!lambdaArgument || typeIndex != owner->typeArguments.size() ||
      overlays.empty()) {
    return std::nullopt;
  }
  return overlays.size();
}

std::optional<std::size_t> cppMirApplyGenericOwnerConstructorTypeOverlays(
    CppMirBodyEmissionMapRows &rows, const LoweredProgram &program,
    const LoweredConstructorDeclaration &declaration,
    const MirConstructorInstance &instance) {
  const LoweredConstructorInstance *source =
      program.findConstructorInstance(instance.id);
  const LoweredClassInstance *owner = program.findClassInstance(instance.owner);
  const LoweredClassDeclaration *ownerInfo =
      owner == nullptr ? nullptr
                       : program.findClassDeclaration(owner->declaration);
  const MirClassInstance *mirOwner =
      program.mir().findClassInstance(instance.owner);
  if (source == nullptr || source->declaration != declaration.id ||
      source->owner != instance.owner || owner == nullptr ||
      ownerInfo == nullptr || mirOwner == nullptr ||
      owner->id != mirOwner->id ||
      owner->declaration != mirOwner->declaration ||
      declaration.owner != ownerInfo->id ||
      ownerInfo->genericParameters.empty() || owner->typeArguments.empty() ||
      !owner->valueArguments.empty()) {
    return std::nullopt;
  }

  std::vector<std::pair<SemanticType, std::string>> overlays;
  std::size_t typeIndex = 0;
  bool lambdaArgument = false;
  for (const LoweredGenericParameter &parameter :
       ownerInfo->genericParameters) {
    if (parameter.pack || parameter.value ||
        typeIndex >= owner->typeArguments.size()) {
      return std::nullopt;
    }
    const SemanticType &argument = owner->typeArguments[typeIndex++];
    lambdaArgument = lambdaArgument || containsLambdaType(argument);
    if (!applyTemplateTypeOverlay(rows, overlays, argument, parameter.name)) {
      return std::nullopt;
    }
  }
  if (!lambdaArgument || typeIndex != owner->typeArguments.size() ||
      overlays.empty()) {
    return std::nullopt;
  }
  return overlays.size();
}

namespace {

[[nodiscard]] bool
hasOnlyUnsupportedTextVocabulary(const CppMirBodyEmissionAnalysis &analysis) {
  return !analysis.issues.empty() &&
         std::all_of(
             analysis.issues.begin(), analysis.issues.end(),
             [](const CppMirBodyEmissionIssue &issue) {
               return issue.kind ==
                      CppMirBodyEmissionIssueKind::UnsupportedTextVocabulary;
             });
}

[[nodiscard]] bool
specializedBodyTextIsReady(const SemanticModel &semantics,
                           const HirProgram &hir, const MirProgram &mir,
                           CppStandard standard,
                           const CppMirBodyEmissionMapRows &baseRows,
                           const CppMirBodyEmissionAnalysis &analysis) {
  if (!hasOnlyUnsupportedTextVocabulary(analysis)) {
    return false;
  }

  CppMirBodyEmissionMapRows rows = baseRows;
  std::optional<std::size_t> overlayCount;
  switch (analysis.body.kind) {
  case MirBodyKind::Function: {
    const MirFunctionInstance *instance =
        mir.findFunctionInstance(analysis.body.owner);
    const FunctionInfo *declaration =
        instance == nullptr ? nullptr
                            : semantics.findFunction(instance->declaration);
    if (instance == nullptr || declaration == nullptr ||
        instance->definitionKind != MirDefinitionKind::Source) {
      return false;
    }
    overlayCount = cppMirApplyCallableTemplateTypeOverlays(
        rows, semantics, standard, *declaration, *instance);
    if (!overlayCount || *overlayCount == 0) {
      return false;
    }
    const CppMirBodyEmissionMap overlay(std::move(rows));
    const CppMirBodyEmitter emitter(mir, overlay);
    const CppMirBodyEmissionAnalysis overlayAnalysis =
        emitter.analyze(analysis.body);
    if (!overlayAnalysis.ready()) {
      return false;
    }
    const bool failure = emitter.supportsFailureBodyText(analysis.body);
    const bool plain = emitter.supportsBodyText(analysis.body);
    return instance->mayRaiseDefinedFailure ? failure || plain : plain;
  }
  case MirBodyKind::Constructor: {
    const MirConstructorInstance *instance =
        mir.findConstructorInstance(analysis.body.owner);
    const HirConstructorInstance *source =
        instance == nullptr ? nullptr
                            : hir.findConstructorInstance(instance->id);
    if (instance == nullptr || source == nullptr || source->source == nullptr ||
        instance->definitionKind != MirDefinitionKind::Source ||
        !instance->mayRaiseDefinedFailure) {
      return false;
    }
    overlayCount = cppMirApplyGenericOwnerConstructorTypeOverlays(
        rows, semantics, hir, mir, *source->source, *instance);
    if (!overlayCount || *overlayCount == 0) {
      return false;
    }
    const CppMirBodyEmissionMap overlay(std::move(rows));
    const CppMirBodyEmitter emitter(mir, overlay);
    return emitter.analyze(analysis.body).ready() &&
           emitter.supportsFailureBodyText(analysis.body);
  }
  case MirBodyKind::Module:
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
  case MirBodyKind::Destructor:
  case MirBodyKind::Lambda:
  case MirBodyKind::HostedStartup:
    return false;
  }
  return false;
}

[[nodiscard]] bool
specializedBodyTextIsReady(const LoweredProgram &program, CppStandard standard,
                           const CppMirBodyEmissionMapRows &baseRows,
                           const CppMirBodyEmissionAnalysis &analysis) {
  if (!hasOnlyUnsupportedTextVocabulary(analysis)) {
    return false;
  }

  CppMirBodyEmissionMapRows rows = baseRows;
  std::optional<std::size_t> overlayCount;
  switch (analysis.body.kind) {
  case MirBodyKind::Function: {
    const MirFunctionInstance *instance =
        program.mir().findFunctionInstance(analysis.body.owner);
    const LoweredFunctionDeclaration *declaration =
        instance == nullptr
            ? nullptr
            : program.findFunctionDeclaration(instance->declaration);
    if (instance == nullptr || declaration == nullptr ||
        instance->definitionKind != MirDefinitionKind::Source) {
      return false;
    }
    overlayCount = cppMirApplyCallableTemplateTypeOverlays(
        rows, program, standard, *declaration, *instance);
    if (!overlayCount || *overlayCount == 0) {
      return false;
    }
    const CppMirBodyEmissionMap overlay(std::move(rows));
    const CppMirBodyEmitter emitter(program.mir(), overlay);
    const CppMirBodyEmissionAnalysis overlayAnalysis =
        emitter.analyze(analysis.body);
    if (!overlayAnalysis.ready()) {
      return false;
    }
    const bool failure = emitter.supportsFailureBodyText(analysis.body);
    const bool plain = emitter.supportsBodyText(analysis.body);
    return instance->mayRaiseDefinedFailure ? failure || plain : plain;
  }
  case MirBodyKind::Constructor: {
    const MirConstructorInstance *instance =
        program.mir().findConstructorInstance(analysis.body.owner);
    const LoweredConstructorInstance *source =
        instance == nullptr ? nullptr
                            : program.findConstructorInstance(instance->id);
    const LoweredConstructorDeclaration *declaration =
        source == nullptr
            ? nullptr
            : program.findConstructorDeclaration(source->declaration);
    if (instance == nullptr || declaration == nullptr ||
        instance->definitionKind != MirDefinitionKind::Source ||
        !instance->mayRaiseDefinedFailure) {
      return false;
    }
    overlayCount = cppMirApplyGenericOwnerConstructorTypeOverlays(
        rows, program, *declaration, *instance);
    if (!overlayCount || *overlayCount == 0) {
      return false;
    }
    const CppMirBodyEmissionMap overlay(std::move(rows));
    const CppMirBodyEmitter emitter(program.mir(), overlay);
    return emitter.analyze(analysis.body).ready() &&
           emitter.supportsFailureBodyText(analysis.body);
  }
  case MirBodyKind::Module:
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
  case MirBodyKind::Destructor:
  case MirBodyKind::Lambda:
  case MirBodyKind::HostedStartup:
    return false;
  }
  return false;
}

} // namespace

CppMirRepresentationSnapshotBuild buildCppMirRepresentationSnapshot(
    const Program &program, const SemanticModel &semantics,
    const HirProgram &hir, const MirProgram &mir, const TargetInfo &target,
    CppStandard standard, const MirProgram *sourceMir) {
  CppMirRepresentationSnapshotBuild build;
  CppMirRepresentationSnapshot snapshot;
  snapshot.mir = mir;

  if (!semantics.analysisSeal().matchesTarget(target)) {
    addIssue(build, CppMirRepresentationSnapshotIssueKind::CrossPhaseMismatch,
             "backend target differs from the analyzed target seal");
  }
  if (!semantics.analysisSeal().matchesProgram(program, target)) {
    addIssue(build,
             CppMirRepresentationSnapshotIssueKind::MissingProgramDeclaration,
             "supplied Program/target traversal differs from the analyzed "
             "semantic seal");
  }

  std::string crossPhaseMismatch;
  if (!cppMirFrontendSnapshotsMatch(semantics, hir, mir, &crossPhaseMismatch)) {
    addIssue(build, CppMirRepresentationSnapshotIssueKind::CrossPhaseMismatch,
             std::move(crossPhaseMismatch));
  }

  ProgramDeclarationInventory declarations;
  collectProgramDeclarations(program.declarations(), target, declarations);
  ProgramLambdaCollector(target, declarations.lambdas)
      .collect(program.declarations());
  appendSourceDeclarationRows(declarations, semantics, snapshot, build);

  for (const MirBodyAddress address : enumerateMirBodyAddresses(mir)) {
    const std::optional<CppMirBodyIdentity> identity =
        captureCppMirBodyIdentity(mir, address);
    if (!identity) {
      addIssue(build,
               CppMirRepresentationSnapshotIssueKind::MissingMirBodyIdentity,
               "core MIR body does not retain an exact representation "
               "identity");
      continue;
    }
    if (!bodySourceBelongsToProgram(address, declarations, hir)) {
      addIssue(build,
               CppMirRepresentationSnapshotIssueKind::MissingProgramDeclaration,
               "core MIR body source is not owned by the supplied Program");
    }

    CppMirBodyRepresentation row{.identity = *identity};
    const MirBody *body = findMirBody(mir, address);
    const HirBody *hirBody = findHirBody(hir, address);
    if (isInitializerBody(address.kind) &&
        address.kind != MirBodyKind::Module && body != nullptr &&
        hirBody != nullptr &&
        (!hirBody->roots.empty()) !=
            (!isCanonicalNoExecutionInitializer(*body))) {
      addIssue(build, CppMirRepresentationSnapshotIssueKind::CrossPhaseMismatch,
               "initializer executable/data role differs between HIR and "
               "verified MIR");
    }
    switch (identity->definition) {
    case CppMirBodyDefinitionKind::ImplicitSource:
      if (address.kind == MirBodyKind::Module) {
        const bool executable = hasExecutableProgramInitialization(mir);
        if (executable != hasExecutableProgramInitialization(hir)) {
          addIssue(build,
                   CppMirRepresentationSnapshotIssueKind::CrossPhaseMismatch,
                   "HIR and MIR program-initialization execution roles "
                   "differ");
        }
        row.role = executable ? CppMirBodyRole::SourceExecutable
                              : CppMirBodyRole::DataOnly;
        row.family = executable ? CppMirExecutionFamily::GeneralV1
                                : CppMirExecutionFamily::None;
      } else if (body != nullptr && isCanonicalNoExecutionInitializer(*body)) {
        row.role = CppMirBodyRole::DataOnly;
        row.family = CppMirExecutionFamily::None;
      } else {
        row.role = CppMirBodyRole::SourceExecutable;
        row.family = CppMirExecutionFamily::GeneralV1;
      }
      break;
    case CppMirBodyDefinitionKind::Source:
      row.role = CppMirBodyRole::SourceExecutable;
      row.family = CppMirExecutionFamily::GeneralV1;
      break;
    case CppMirBodyDefinitionKind::CompilerGenerated:
      row.role = CppMirBodyRole::SourceExecutable;
      row.family = CppMirExecutionFamily::GeneralV1;
      break;
    case CppMirBodyDefinitionKind::RuntimeBinding:
    case CppMirBodyDefinitionKind::Declaration:
      row.role = CppMirBodyRole::AbiDeclaration;
      row.family = CppMirExecutionFamily::None;
      break;
    case CppMirBodyDefinitionKind::Count:
      addIssue(build,
               CppMirRepresentationSnapshotIssueKind::MissingMirBodyIdentity,
               "core MIR body has an invalid definition provenance");
      break;
    }
    snapshot.bodies.push_back(std::move(row));
  }

  std::unordered_set<const EnumDecl *> representedEnums;
  for (const HirEnum &enumeration : hir.enumDeclarations()) {
    const EnumTypeInfo *info =
        enumeration.source == nullptr
            ? nullptr
            : semantics.findEnumType(*enumeration.source);
    if (enumeration.source == nullptr ||
        !declarations.enums.contains(enumeration.source)) {
      addIssue(build,
               CppMirRepresentationSnapshotIssueKind::MissingProgramDeclaration,
               "HIR enum data is not owned by the supplied Program");
      continue;
    }
    if (info == nullptr || info->id != enumeration.declaration ||
        info->declaration != enumeration.source ||
        info->sourceUnit != enumeration.sourceUnit) {
      addIssue(
          build,
          CppMirRepresentationSnapshotIssueKind::MissingSemanticDeclaration,
          "HIR enum data does not match its semantic declaration");
      continue;
    }
    representedEnums.insert(enumeration.source);
    snapshot.data.push_back(
        {.identity = {.kind = CppMirDataKind::EnumDefinition,
                      .declaration = enumeration.declaration},
         .support = CppMirSurfaceSupport::Supported});
  }
  for (const EnumDecl *enumeration : declarations.orderedEnums) {
    const EnumTypeInfo *info = semantics.findEnumType(*enumeration);
    if (info == nullptr) {
      addIssue(
          build,
          CppMirRepresentationSnapshotIssueKind::MissingSemanticDeclaration,
          "Program enum has no semantic declaration");
    } else if (!representedEnums.contains(enumeration)) {
      addIssue(build,
               CppMirRepresentationSnapshotIssueKind::MissingHirDeclaration,
               "semantic enum is absent from the HIR representation inventory");
    }
  }

  for (const ClassDecl *declaration : declarations.orderedClasses) {
    const ClassTypeInfo *info = semantics.findClassType(*declaration);
    if (info == nullptr || info->id == 0 || info->declaration != declaration) {
      addIssue(
          build,
          CppMirRepresentationSnapshotIssueKind::MissingSemanticDeclaration,
          "Program class has no exact semantic declaration");
      continue;
    }
    if (isAbiRepresentation(*info)) {
      snapshot.data.push_back(
          {.identity = {.kind = CppMirDataKind::AbiTypeDeclaration,
                        .declaration = info->id},
           .support = CppMirSurfaceSupport::Supported});
    } else if (!info->genericParameters.empty()) {
      // Concrete HIR instances inventory instantiated bodies. This separate
      // declaration row keeps an unused source template visible to the future
      // representation emitter instead of silently treating it as absent.
      snapshot.data.push_back(
          {.identity = {.kind = CppMirDataKind::ClassTemplateDeclaration,
                        .declaration = info->id},
           .support = CppMirSurfaceSupport::Supported});
    }
  }

  std::unordered_set<const VariableDecl *> representedModuleConstexpr;
  for (const HirBinding &binding : hir.module().bindings) {
    if (binding.variable == nullptr || !binding.variable->isConstexpr()) {
      continue;
    }
    if (!declarations.variables.contains(binding.variable) ||
        binding.info.symbol == 0) {
      addIssue(build,
               CppMirRepresentationSnapshotIssueKind::MissingProgramDeclaration,
               "module constexpr binding lacks an exact Program identity");
      continue;
    }
    const BindingInfo *info = semantics.findBinding(*binding.variable);
    if (info == nullptr || info->symbol != binding.info.symbol) {
      addIssue(
          build,
          CppMirRepresentationSnapshotIssueKind::MissingSemanticDeclaration,
          "module constexpr binding lacks exact semantic identity");
      continue;
    }
    representedModuleConstexpr.insert(binding.variable);
  }

  for (const HirClassInstance &instance : hir.classInstances()) {
    const ClassTypeInfo *classInfo =
        instance.source == nullptr ? nullptr
                                   : semantics.findClassType(*instance.source);
    if (classInfo == nullptr || classInfo->id != instance.declaration ||
        !containsClassSource(declarations, instance.source)) {
      addIssue(
          build,
          CppMirRepresentationSnapshotIssueKind::MissingSemanticDeclaration,
          "concrete HIR class lacks exact Program/semantic identity");
      continue;
    }

    const auto verifyClassConstexpr = [&](const std::vector<HirClassField>
                                              &fields) {
      for (std::size_t index = 0; index < fields.size(); ++index) {
        const HirClassField &field = fields[index];
        if (field.declaration == nullptr || !field.declaration->isConstexpr()) {
          continue;
        }
        if (!declarations.variables.contains(field.declaration) ||
            field.info.symbol == 0) {
          addIssue(
              build,
              CppMirRepresentationSnapshotIssueKind::MissingProgramDeclaration,
              "class constexpr binding lacks exact Program identity");
          continue;
        }
        const BindingInfo *info = semantics.findBinding(*field.declaration);
        if (info == nullptr || info->symbol != field.info.symbol) {
          addIssue(
              build,
              CppMirRepresentationSnapshotIssueKind::MissingSemanticDeclaration,
              "class constexpr binding lacks exact semantic identity");
          continue;
        }
      }
    };
    verifyClassConstexpr(instance.fields);
    verifyClassConstexpr(instance.staticFields);
  }

  for (std::size_t index = 0; index < declarations.constexprBindings.size();
       ++index) {
    const ConstexprDeclaration &declaration =
        declarations.constexprBindings[index];
    const BindingInfo *binding =
        semantics.findBinding(*declaration.declaration);
    if (binding == nullptr || binding->symbol == 0) {
      addIssue(
          build,
          CppMirRepresentationSnapshotIssueKind::MissingSemanticDeclaration,
          "Program constexpr binding has no semantic identity");
      continue;
    }
    if (declaration.owner == nullptr &&
        !representedModuleConstexpr.contains(declaration.declaration)) {
      addIssue(build,
               CppMirRepresentationSnapshotIssueKind::MissingHirDeclaration,
               "namespace constexpr binding is absent from the HIR module");
    }
    if (declaration.owner != nullptr &&
        classOwnerId(semantics, declaration.owner) == 0) {
      addIssue(
          build,
          CppMirRepresentationSnapshotIssueKind::MissingSemanticDeclaration,
          "class constexpr owner has no semantic identity");
      continue;
    }
    snapshot.data.push_back(
        {.identity = {.kind = CppMirDataKind::ConstexprBinding,
                      .declaration = binding->symbol,
                      .owner = classOwnerId(semantics, declaration.owner),
                      .ordinal = index + 1},
         .support = CppMirSurfaceSupport::Supported});
  }

  CppMirBodyRepresentation *module =
      findBody(snapshot, {.kind = MirBodyKind::Module, .owner = 0});
  std::optional<CppMirThunkIdentity> initialization;
  if (hasExecutableProgramInitialization(mir)) {
    initialization =
        CppMirThunkIdentity{.kind = CppMirThunkKind::ProgramInitialization,
                            .owner = 0,
                            .ordinal = 0};
    if (module == nullptr || module->role != CppMirBodyRole::SourceExecutable) {
      addIssue(
          build,
          CppMirRepresentationSnapshotIssueKind::InvalidProgramInitialization,
          "executable program-initialization plan lacks exact "
          "Module/0 body authority");
    } else {
      module->requiredThunks.push_back(*initialization);
    }
    snapshot.thunks.push_back(
        {.identity = *initialization,
         .sourceBody = {.kind = MirBodyKind::Module, .owner = 0},
         .support = CppMirSurfaceSupport::Supported});
  }

  std::size_t hostedEntries = 0;
  for (const MirFunctionInstance &function : mir.functionInstances()) {
    if (function.entryKind == ProgramEntryKind::None) {
      continue;
    }
    ++hostedEntries;
    const HirFunctionInstance *source = hir.findFunctionInstance(function.id);
    const std::optional<MirHostedStartupPlan> &startup =
        mir.hostedStartupPlan();
    CppMirBodyRepresentation *body = findBody(
        snapshot, {.kind = MirBodyKind::HostedStartup, .owner = function.id});
    if (source == nullptr || source->entryKind != function.entryKind ||
        function.definitionKind != MirDefinitionKind::Source || !startup ||
        startup->entry != function.id || mir.hostedStartup() == nullptr ||
        body == nullptr || body->role != CppMirBodyRole::SourceExecutable) {
      addIssue(build, CppMirRepresentationSnapshotIssueKind::InvalidHostedEntry,
               "hosted entry lacks its exact compiler-generated startup "
               "body");
      continue;
    }
    const CppMirThunkIdentity hosted{.kind = CppMirThunkKind::HostedEntry,
                                     .owner = function.id,
                                     .ordinal = 0};
    body->requiredThunks.push_back(hosted);
    CppMirGeneratedThunk thunk{
        .identity = hosted,
        .sourceBody = {.kind = MirBodyKind::HostedStartup,
                       .owner = function.id},
        .support = CppMirSurfaceSupport::Supported};
    if (programInitializationBodyCallCount(mir) != 0 && initialization) {
      thunk.dependencies.push_back(*initialization);
    }
    snapshot.thunks.push_back(std::move(thunk));
  }
  if (hostedEntries > 1) {
    addIssue(build, CppMirRepresentationSnapshotIssueKind::InvalidHostedEntry,
             "representation snapshot contains more than one hosted entry");
  }

  std::vector<std::vector<MirBodyAddress>> nativeCallbackRoots(
      mir.nativeCallbackAdapters().size() + 1);
  for (const MirBodyAddress address : enumerateMirBodyAddresses(mir)) {
    const MirBody *body = findMirBody(mir, address);
    if (body == nullptr) {
      continue;
    }
    std::vector<MirNativeCallbackAdapterId> adapters;
    for (const MirBlock &block : body->blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.operation == MirOperation::NativeCallback &&
            instruction.nativeCallbackAdapter) {
          adapters.push_back(*instruction.nativeCallbackAdapter);
        }
      }
    }
    std::sort(adapters.begin(), adapters.end());
    adapters.erase(std::unique(adapters.begin(), adapters.end()),
                   adapters.end());
    for (const MirNativeCallbackAdapterId adapter : adapters) {
      if (adapter < nativeCallbackRoots.size()) {
        nativeCallbackRoots[adapter].push_back(address);
      }
    }
  }

  for (const MirNativeCallbackAdapter &adapter : mir.nativeCallbackAdapters()) {
    const HirNativeCallbackAdapter *source =
        hir.findNativeCallbackAdapter(adapter.id);
    const HirFunctionInstance *hirTarget =
        hir.findFunctionInstance(adapter.target);
    const MirFunctionInstance *target =
        mir.findFunctionInstance(adapter.target);
    const FunctionInfo *declaration =
        target == nullptr ? nullptr
                          : semantics.findFunction(target->declaration);
    CppMirBodyRepresentation *sourceBody = findBody(
        snapshot, {.kind = MirBodyKind::Function, .owner = adapter.target});
    if (source == nullptr || source->id != adapter.id ||
        source->target != adapter.target || source->type != adapter.type ||
        hirTarget == nullptr || target == nullptr || declaration == nullptr ||
        hirTarget->source == nullptr ||
        declaration->declaration != hirTarget->source || target->owner ||
        target->linkage != LanguageLinkage::Gti ||
        target->definitionKind != MirDefinitionKind::Source ||
        adapter.targetMayRaiseDefinedFailure !=
            target->mayRaiseDefinedFailure ||
        sourceBody == nullptr ||
        sourceBody->role != CppMirBodyRole::SourceExecutable) {
      addIssue(
          build,
          CppMirRepresentationSnapshotIssueKind::InvalidNativeCallbackAdapter,
          "native callback adapter lacks its exact semantic, HIR, and MIR "
          "source function");
      continue;
    }

    const CppMirThunkIdentity identity{
        .kind = CppMirThunkKind::NativeInteropAdapter,
        .owner = adapter.id,
        .ordinal = 0};
    CppMirGeneratedThunk thunk{
        .identity = identity,
        .sourceBody = {.kind = MirBodyKind::Function, .owner = adapter.target},
        .support = CppMirSurfaceSupport::Supported};
    thunk.payload = CppMirNativeCallbackThunk{.adapter = adapter};
    snapshot.thunks.push_back(std::move(thunk));

    bool rooted = false;
    for (const MirBodyAddress address : nativeCallbackRoots[adapter.id]) {
      CppMirBodyRepresentation *consumer = findBody(snapshot, address);
      if (consumer == nullptr ||
          consumer->role != CppMirBodyRole::SourceExecutable) {
        addIssue(
            build,
            CppMirRepresentationSnapshotIssueKind::InvalidNativeCallbackAdapter,
            "native callback use lacks its exact executable MIR body root");
        continue;
      }
      consumer->requiredThunks.push_back(identity);
      rooted = true;
    }
    if (!rooted) {
      addIssue(
          build,
          CppMirRepresentationSnapshotIssueKind::InvalidNativeCallbackAdapter,
          "native callback adapter is not rooted by a MIR callback operation");
    }
  }

  if (module == nullptr) {
    addIssue(
        build,
        CppMirRepresentationSnapshotIssueKind::InvalidProgramInitialization,
        "representation snapshot omitted exact Module/0");
  }

  // Legacy direct-backend tests still enter through the frontend tuple while
  // BackendInput is being narrowed. Derive declaration-generated adapters
  // through the same sealed LoweredProgram builder as production so this
  // temporary oracle cannot retain a second eligibility implementation.
  if (build.issues.empty()) {
    LoweredProgramBuild lowered = LoweredProgramBuilder().build(
        program, semantics, hir, sourceMir == nullptr ? mir : *sourceMir, mir,
        target);
    if (!lowered.valid()) {
      const std::string detail =
          lowered.issues.empty() ? "unknown LoweredProgram construction error"
                                 : lowered.issues.front().detail;
      addIssue(build, CppMirRepresentationSnapshotIssueKind::CrossPhaseMismatch,
               "frontend compatibility snapshot could not construct its "
               "LoweredProgram adapter inventory: " +
                   detail);
    } else {
      for (const LoweredDeclaration &declaration :
           lowered.program->declarations()) {
        CppMirDeclarationThunkRoots roots{.declaration = declaration.id};
        for (const LoweredGeneratedItemIdentity &required :
             declaration.requiredGeneratedItems) {
          if (required.kind ==
                  LoweredGeneratedItemKind::StructuralOperatorAdapter ||
              required.kind == LoweredGeneratedItemKind::CallableAdapter) {
            roots.requiredThunks.push_back(
                {.kind =
                     required.kind ==
                             LoweredGeneratedItemKind::StructuralOperatorAdapter
                         ? CppMirThunkKind::StructuralOperatorAdapter
                         : CppMirThunkKind::CallableAdapter,
                 .owner = required.owner,
                 .ordinal = required.ordinal});
          }
        }
        if (!roots.requiredThunks.empty()) {
          snapshot.declarationRoots.push_back(std::move(roots));
        }
      }
      for (const LoweredGeneratedItem &item :
           lowered.program->generatedItems()) {
        if (item.identity.kind !=
                LoweredGeneratedItemKind::StructuralOperatorAdapter &&
            item.identity.kind != LoweredGeneratedItemKind::CallableAdapter) {
          continue;
        }
        CppMirGeneratedThunk thunk{
            .identity =
                {.kind =
                     item.identity.kind ==
                             LoweredGeneratedItemKind::StructuralOperatorAdapter
                         ? CppMirThunkKind::StructuralOperatorAdapter
                         : CppMirThunkKind::CallableAdapter,
                 .owner = item.identity.owner,
                 .ordinal = item.identity.ordinal},
            .sourceKind = CppMirGeneratedThunkSourceKind::Declaration,
            .sourceDeclaration = item.sourceDeclaration,
            .support = CppMirSurfaceSupport::Supported};
        if (const auto *adapter =
                std::get_if<LoweredStructuralOperatorAdapterItem>(
                    &item.payload)) {
          thunk.payload = CppMirStructuralOperatorThunk{
              .function = adapter->function, .operation = adapter->operation};
        } else if (const auto *adapter =
                       std::get_if<LoweredCallableAdapterItem>(&item.payload)) {
          thunk.payload = CppMirCallableThunk{
              .function = adapter->function, .capability = adapter->capability};
        }
        snapshot.thunks.push_back(std::move(thunk));
      }
    }
  }

  if (build.issues.empty()) {
    const CppMirBodyEmissionMapRows emissionRows =
        buildCppMirBodyEmissionMapRows(semantics, mir, standard);
    const CppMirBodyEmissionMap emissionMap(emissionRows);
    const CppMirProgramEmissionAnalysis emission =
        CppMirBodyEmitter(mir, emissionMap).analyzeProgram();
    bool emissionReady = emission.ready();
    const CppMirBodyEmissionIssue *unresolvedIssue = nullptr;
    if (!emissionReady) {
      emissionReady = emission.issues.empty();
      for (const CppMirBodyEmissionAnalysis &body : emission.bodies) {
        if (body.ready()) {
          continue;
        }
        if (!specializedBodyTextIsReady(semantics, hir, mir, standard,
                                        emissionRows, body)) {
          emissionReady = false;
          if (!body.issues.empty()) {
            unresolvedIssue = &body.issues.front();
          }
          break;
        }
      }
    }
    if (!emissionReady) {
      std::string detail =
          "verified MIR program is outside the complete C++ text vocabulary";
      if (unresolvedIssue == nullptr && !emission.issues.empty()) {
        unresolvedIssue = &emission.issues.front();
      }
      if (unresolvedIssue != nullptr) {
        const CppMirBodyEmissionIssue &issue = *unresolvedIssue;
        detail += ": " + issue.detail + " (body-kind " +
                  std::to_string(static_cast<std::size_t>(issue.body.kind)) +
                  " owner " + std::to_string(issue.body.owner) + ')';
      }
      addIssue(build,
               CppMirRepresentationSnapshotIssueKind::UnsupportedMirEmission,
               std::move(detail));
    }
  }

  if (build.issues.empty()) {
    CppMirRepresentationSnapshotBuilderAccess::seal(snapshot);
    build.snapshot = std::move(snapshot);
  }
  return build;
}

namespace {

[[nodiscard]] CppMirBodyDefinitionKind
cppBodyDefinition(LoweredBodyDefinitionKind definition) {
  switch (definition) {
  case LoweredBodyDefinitionKind::ImplicitSource:
    return CppMirBodyDefinitionKind::ImplicitSource;
  case LoweredBodyDefinitionKind::Source:
    return CppMirBodyDefinitionKind::Source;
  case LoweredBodyDefinitionKind::CompilerGenerated:
    return CppMirBodyDefinitionKind::CompilerGenerated;
  case LoweredBodyDefinitionKind::RuntimeBinding:
    return CppMirBodyDefinitionKind::RuntimeBinding;
  case LoweredBodyDefinitionKind::Declaration:
    return CppMirBodyDefinitionKind::Declaration;
  case LoweredBodyDefinitionKind::Count:
    return CppMirBodyDefinitionKind::Count;
  }
  return CppMirBodyDefinitionKind::Count;
}

[[nodiscard]] CppMirBodyRole cppBodyRole(LoweredBodyRole role) {
  switch (role) {
  case LoweredBodyRole::SourceExecutable:
    return CppMirBodyRole::SourceExecutable;
  case LoweredBodyRole::AbiDeclaration:
    return CppMirBodyRole::AbiDeclaration;
  case LoweredBodyRole::DataOnly:
    return CppMirBodyRole::DataOnly;
  case LoweredBodyRole::Count:
    return CppMirBodyRole::Count;
  }
  return CppMirBodyRole::Count;
}

[[nodiscard]] CppMirThunkKind cppThunkKind(LoweredGeneratedItemKind kind) {
  switch (kind) {
  case LoweredGeneratedItemKind::ProgramInitialization:
    return CppMirThunkKind::ProgramInitialization;
  case LoweredGeneratedItemKind::HostedEntry:
    return CppMirThunkKind::HostedEntry;
  case LoweredGeneratedItemKind::StructuralOperatorAdapter:
    return CppMirThunkKind::StructuralOperatorAdapter;
  case LoweredGeneratedItemKind::CallableAdapter:
    return CppMirThunkKind::CallableAdapter;
  case LoweredGeneratedItemKind::LifecycleCleanup:
    return CppMirThunkKind::LifecycleCleanup;
  case LoweredGeneratedItemKind::NativeInteropAdapter:
    return CppMirThunkKind::NativeInteropAdapter;
  case LoweredGeneratedItemKind::ConcreteInstanceAdapter:
    return CppMirThunkKind::ConcreteInstanceAdapter;
  case LoweredGeneratedItemKind::Count:
    return CppMirThunkKind::Count;
  }
  return CppMirThunkKind::Count;
}

[[nodiscard]] CppMirThunkIdentity
cppThunkIdentity(const LoweredGeneratedItemIdentity &identity) {
  return {.kind = cppThunkKind(identity.kind),
          .owner = identity.owner,
          .ordinal = identity.ordinal};
}

[[nodiscard]] CppMirGeneratedThunkSourceKind
cppThunkSourceKind(LoweredGeneratedItemSourceKind kind) {
  switch (kind) {
  case LoweredGeneratedItemSourceKind::Body:
    return CppMirGeneratedThunkSourceKind::Body;
  case LoweredGeneratedItemSourceKind::Declaration:
    return CppMirGeneratedThunkSourceKind::Declaration;
  case LoweredGeneratedItemSourceKind::Count:
    return CppMirGeneratedThunkSourceKind::Count;
  }
  return CppMirGeneratedThunkSourceKind::Count;
}

[[nodiscard]] std::optional<CppMirDataKind>
cppSourceDataKind(LoweredDeclarationKind kind) {
  switch (kind) {
  case LoweredDeclarationKind::Namespace:
    return CppMirDataKind::NamespaceDeclaration;
  case LoweredDeclarationKind::NamespaceAlias:
    return CppMirDataKind::NamespaceAliasDeclaration;
  case LoweredDeclarationKind::TypeAlias:
    return CppMirDataKind::TypeAliasDeclaration;
  case LoweredDeclarationKind::Class:
    return CppMirDataKind::ClassDeclaration;
  case LoweredDeclarationKind::Function:
  case LoweredDeclarationKind::Constructor:
  case LoweredDeclarationKind::Destructor:
    return CppMirDataKind::CallableDeclaration;
  case LoweredDeclarationKind::Storage:
    return CppMirDataKind::StorageDeclaration;
  case LoweredDeclarationKind::Access:
    return CppMirDataKind::AccessDeclaration;
  case LoweredDeclarationKind::LanguageLinkage:
    return CppMirDataKind::LanguageLinkageDeclaration;
  case LoweredDeclarationKind::Empty:
    return CppMirDataKind::EmptyDeclaration;
  case LoweredDeclarationKind::Concept:
  case LoweredDeclarationKind::Other:
    return CppMirDataKind::OtherDeclaration;
  case LoweredDeclarationKind::Enum:
  case LoweredDeclarationKind::Count:
    return std::nullopt;
  }
  return std::nullopt;
}

void appendLoweredDeclarationRows(const LoweredProgram &program,
                                  CppMirRepresentationSnapshot &snapshot,
                                  CppMirRepresentationSnapshotBuild &build) {
  std::size_t sourceOrdinal = 0;
  std::vector<const LoweredEnumDeclaration *> enums;
  std::vector<const LoweredClassDeclaration *> classes;
  std::vector<const LoweredStorageDeclaration *> constexprBindings;

  for (const LoweredDeclaration &declaration : program.declarations()) {
    if (const auto *enumeration =
            std::get_if<LoweredEnumDeclaration>(&declaration.payload)) {
      enums.push_back(enumeration);
    }
    if (const auto *classDeclaration =
            std::get_if<LoweredClassDeclaration>(&declaration.payload)) {
      classes.push_back(classDeclaration);
    }
    if (const auto *storage =
            std::get_if<LoweredStorageDeclaration>(&declaration.payload);
        storage != nullptr && storage->constexprStorage) {
      constexprBindings.push_back(storage);
    }

    const std::optional<CppMirDataKind> kind =
        cppSourceDataKind(declaration.kind);
    if (!kind) {
      if (declaration.kind == LoweredDeclarationKind::Count) {
        addIssue(build,
                 CppMirRepresentationSnapshotIssueKind::InvalidLoweredProgram,
                 "lowered declaration inventory contains a sentinel kind");
      }
      continue;
    }
    ++sourceOrdinal;
    const std::size_t identity = declaration.semanticIdentity == 0
                                     ? sourceOrdinal
                                     : declaration.semanticIdentity;
    snapshot.data.push_back({.identity = {.kind = *kind,
                                          .declaration = identity,
                                          .owner = declaration.ownerClass,
                                          .ordinal = sourceOrdinal},
                             .support = CppMirSurfaceSupport::Supported});

    bool callableTemplate = false;
    if (const auto *function =
            std::get_if<LoweredFunctionDeclaration>(&declaration.payload)) {
      callableTemplate = !function->genericParameters.empty();
    } else if (const auto *constructor =
                   std::get_if<LoweredConstructorDeclaration>(
                       &declaration.payload)) {
      callableTemplate = !constructor->genericParameters.empty();
    }
    if (callableTemplate) {
      snapshot.data.push_back(
          {.identity = {.kind = CppMirDataKind::CallableTemplateDeclaration,
                        .declaration = identity,
                        .owner = declaration.ownerClass,
                        .ordinal = sourceOrdinal},
           .support = CppMirSurfaceSupport::Supported});
    }
  }

  for (const LoweredEnumDeclaration *enumeration : enums) {
    if (enumeration == nullptr || enumeration->id == 0) {
      addIssue(build,
               CppMirRepresentationSnapshotIssueKind::InvalidLoweredProgram,
               "lowered enum declaration lacks an exact identity");
      continue;
    }
    snapshot.data.push_back(
        {.identity = {.kind = CppMirDataKind::EnumDefinition,
                      .declaration = enumeration->id},
         .support = CppMirSurfaceSupport::Supported});
  }

  for (const LoweredClassDeclaration *classDeclaration : classes) {
    if (classDeclaration == nullptr || classDeclaration->id == 0) {
      addIssue(build,
               CppMirRepresentationSnapshotIssueKind::InvalidLoweredProgram,
               "lowered class declaration lacks an exact identity");
      continue;
    }
    const bool abi = classDeclaration->cAbiRecord ||
                     classDeclaration->cOpaqueHandle ||
                     classDeclaration->kind == ClassKind::Union ||
                     classDeclaration->unionLayout.has_value();
    if (abi) {
      snapshot.data.push_back(
          {.identity = {.kind = CppMirDataKind::AbiTypeDeclaration,
                        .declaration = classDeclaration->id},
           .support = CppMirSurfaceSupport::Supported});
    } else if (!classDeclaration->genericParameters.empty()) {
      snapshot.data.push_back(
          {.identity = {.kind = CppMirDataKind::ClassTemplateDeclaration,
                        .declaration = classDeclaration->id},
           .support = CppMirSurfaceSupport::Supported});
    }
  }

  for (std::size_t index = 0; index < constexprBindings.size(); ++index) {
    const LoweredStorageDeclaration &storage = *constexprBindings[index];
    snapshot.data.push_back(
        {.identity = {.kind = CppMirDataKind::ConstexprBinding,
                      .declaration = storage.symbol,
                      .owner = storage.ownerClass,
                      .ordinal = index + 1},
         .support = CppMirSurfaceSupport::Supported});
  }
}

} // namespace

CppMirRepresentationSnapshotBuild
buildCppMirRepresentationSnapshot(const LoweredProgram &program,
                                  CppStandard standard) {
  CppMirRepresentationSnapshotBuild build;
  const std::vector<LoweredProgramIssue> loweredIssues =
      verifyLoweredProgram(program);
  if (!loweredIssues.empty()) {
    addIssue(build,
             CppMirRepresentationSnapshotIssueKind::InvalidLoweredProgram,
             "invalid LoweredProgram: " + loweredIssues.front().detail);
    return build;
  }

  CppMirRepresentationSnapshot snapshot;
  snapshot.mir = program.mir();
  for (const LoweredBody &body : program.bodies()) {
    // The C++-private plan predates constructor declaration identities in the
    // shared lowered boundary. Preserve its established identity contract
    // until the private plan itself is retired.
    const std::size_t declaration =
        body.identity.address.kind == MirBodyKind::Constructor
            ? 0
            : body.identity.declaration;
    CppMirBodyRepresentation row{
        .identity = {.address = body.identity.address,
                     .placeDomain = body.identity.placeDomain,
                     .definition = cppBodyDefinition(body.identity.definition),
                     .declaration = declaration,
                     .concreteOwner = body.identity.concreteOwner},
        .role = cppBodyRole(body.role),
        .family = body.role == LoweredBodyRole::SourceExecutable
                      ? CppMirExecutionFamily::GeneralV1
                      : CppMirExecutionFamily::None};
    row.requiredThunks.reserve(body.requiredGeneratedItems.size());
    for (const LoweredGeneratedItemIdentity &required :
         body.requiredGeneratedItems) {
      row.requiredThunks.push_back(cppThunkIdentity(required));
    }
    snapshot.bodies.push_back(std::move(row));
  }

  appendLoweredDeclarationRows(program, snapshot, build);
  for (const LoweredDeclaration &declaration : program.declarations()) {
    if (declaration.requiredGeneratedItems.empty()) {
      continue;
    }
    CppMirDeclarationThunkRoots roots{.declaration = declaration.id};
    roots.requiredThunks.reserve(declaration.requiredGeneratedItems.size());
    for (const LoweredGeneratedItemIdentity &required :
         declaration.requiredGeneratedItems) {
      roots.requiredThunks.push_back(cppThunkIdentity(required));
    }
    snapshot.declarationRoots.push_back(std::move(roots));
  }
  for (const LoweredGeneratedItem &item : program.generatedItems()) {
    CppMirGeneratedThunk thunk{.identity = cppThunkIdentity(item.identity),
                               .sourceKind =
                                   cppThunkSourceKind(item.sourceKind),
                               .sourceBody = item.sourceBody,
                               .sourceDeclaration = item.sourceDeclaration,
                               .support = CppMirSurfaceSupport::Supported};
    thunk.dependencies.reserve(item.dependencies.size());
    for (const LoweredGeneratedItemIdentity &dependency : item.dependencies) {
      thunk.dependencies.push_back(cppThunkIdentity(dependency));
    }
    if (const auto *adapter =
            std::get_if<LoweredStructuralOperatorAdapterItem>(&item.payload)) {
      thunk.payload = CppMirStructuralOperatorThunk{
          .function = adapter->function, .operation = adapter->operation};
    } else if (const auto *adapter =
                   std::get_if<LoweredCallableAdapterItem>(&item.payload)) {
      thunk.payload = CppMirCallableThunk{.function = adapter->function,
                                          .capability = adapter->capability};
    } else if (const auto *callback =
                   std::get_if<LoweredNativeCallbackItem>(&item.payload)) {
      thunk.payload = CppMirNativeCallbackThunk{.adapter = callback->adapter};
    }
    snapshot.thunks.push_back(std::move(thunk));
  }

  if (build.issues.empty()) {
    const CppMirBodyEmissionMapRows emissionRows =
        buildCppMirBodyEmissionMapRows(program, standard);
    const CppMirBodyEmissionMap emissionMap(emissionRows);
    const CppMirProgramEmissionAnalysis emission =
        CppMirBodyEmitter(program.mir(), emissionMap).analyzeProgram();
    bool emissionReady = emission.ready();
    const CppMirBodyEmissionIssue *unresolvedIssue = nullptr;
    if (!emissionReady) {
      emissionReady = emission.issues.empty();
      for (const CppMirBodyEmissionAnalysis &body : emission.bodies) {
        if (body.ready()) {
          continue;
        }
        if (!specializedBodyTextIsReady(program, standard, emissionRows,
                                        body)) {
          emissionReady = false;
          if (!body.issues.empty()) {
            unresolvedIssue = &body.issues.front();
          }
          break;
        }
      }
    }
    if (!emissionReady) {
      std::string detail =
          "verified MIR program is outside the complete C++ text vocabulary";
      if (unresolvedIssue == nullptr && !emission.issues.empty()) {
        unresolvedIssue = &emission.issues.front();
      }
      if (unresolvedIssue != nullptr) {
        detail += ": " + unresolvedIssue->detail + " (body-kind " +
                  std::to_string(
                      static_cast<std::size_t>(unresolvedIssue->body.kind)) +
                  " owner " + std::to_string(unresolvedIssue->body.owner) + ')';
      }
      addIssue(build,
               CppMirRepresentationSnapshotIssueKind::UnsupportedMirEmission,
               std::move(detail));
    }
  }

  if (build.issues.empty()) {
    CppMirRepresentationSnapshotBuilderAccess::seal(snapshot);
    build.snapshot = std::move(snapshot);
  }
  return build;
}

CppMirBackendProgramRoute
selectCppMirBackendProgramRoute(const CppMirProgramPlan &plan) {
  if (!plan.complete()) {
    std::string detail;
    if (!plan.issues.empty()) {
      detail = plan.issues.front().detail;
    } else if (!plan.unsupported.empty()) {
      detail = "sealed representation plan contains an unsupported surface";
    } else {
      detail = "unknown representation-plan failure";
    }
    throw std::logic_error(
        "C++ backend requires a complete verified-MIR representation plan: " +
        detail);
  }
  return CppMirBackendProgramRoute::VerifiedMir;
}

namespace {

struct RowClassFacts {
  std::string name;
  bool passiveCAbiRecord = false;
  SpecialMemberStatus defaultConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus copyConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus moveAssignment = SpecialMemberStatus::Deleted;
  SpecialMemberStatus copyAssignment = SpecialMemberStatus::Deleted;
};

struct RowEnumVariantFacts {
  std::size_t index = 0;
  std::vector<SemanticType> fieldTypes;
};

struct RowEnumFacts {
  SemanticType underlyingType = SemanticType::Unknown;
  bool payload = false;
  std::vector<RowEnumVariantFacts> variants;
};

struct RowSymbolFacts {
  SymbolKind kind = SymbolKind::LocalVariable;
  std::string name;
  std::string qualifiedName;
  SemanticType type = SemanticType::Unknown;
  bool internalLinkage = false;
};

struct RowFunctionFacts {
  std::string name;
  std::vector<std::string> namespaceScope;
  std::vector<LoweredGenericParameter> genericParameters;
  std::vector<SemanticType> parameterTypes;
  std::string spelling;
  bool cLinkage = false;
  bool runtimeBinding = false;
};

class RowFactProvider final {
public:
  explicit RowFactProvider(const SemanticModel &semantics)
      : semantics(&semantics) {}
  explicit RowFactProvider(const LoweredProgram &lowered) : lowered(&lowered) {}

  [[nodiscard]] std::string typeSpelling(CppStandard standard,
                                         const SemanticType &type) const {
    return semantics != nullptr
               ? cppSemanticTypeSpelling(*semantics, standard, type)
               : cppSemanticTypeSpelling(*lowered, standard, type);
  }

  [[nodiscard]] std::optional<RowClassFacts> classFacts(ClassId id) const {
    if (semantics != nullptr) {
      const ClassTypeInfo *info = semantics->findClassType(id);
      const ClassLifecycleInfo *lifecycle =
          info == nullptr || info->declaration == nullptr
              ? nullptr
              : semantics->findClassLifecycle(*info->declaration);
      if (info == nullptr || info->declaration == nullptr) {
        return std::nullopt;
      }
      return RowClassFacts{
          .name = info->declaration->name().lexeme,
          .passiveCAbiRecord = info->cAbiRecord && info->cAbiLayout &&
                               !info->cOpaqueHandle && !info->unionLayout,
          .defaultConstructor = lifecycle == nullptr
                                    ? SpecialMemberStatus::Deleted
                                    : lifecycle->defaultConstructor,
          .copyConstructor = lifecycle == nullptr ? SpecialMemberStatus::Deleted
                                                  : lifecycle->copyConstructor,
          .moveAssignment = lifecycle == nullptr ? SpecialMemberStatus::Deleted
                                                 : lifecycle->moveAssignment,
          .copyAssignment = lifecycle == nullptr ? SpecialMemberStatus::Deleted
                                                 : lifecycle->copyAssignment};
    }
    const LoweredClassDeclaration *info = lowered->findClassDeclaration(id);
    if (info == nullptr) {
      return std::nullopt;
    }
    const std::size_t separator = info->qualifiedName.rfind("::");
    return RowClassFacts{
        .name = separator == std::string::npos
                    ? info->qualifiedName
                    : info->qualifiedName.substr(separator + 2),
        .passiveCAbiRecord = info->cAbiRecord && info->cAbiLayout &&
                             !info->cOpaqueHandle && !info->unionLayout,
        .defaultConstructor = info->defaultConstructor,
        .copyConstructor = info->copyConstructor,
        .moveAssignment = info->moveAssignment,
        .copyAssignment = info->copyAssignment};
  }

  [[nodiscard]] std::optional<RowEnumFacts> enumFacts(EnumId id) const {
    RowEnumFacts result;
    if (semantics != nullptr) {
      const EnumTypeInfo *info = semantics->findEnumType(id);
      if (info == nullptr) {
        return std::nullopt;
      }
      result.underlyingType = info->underlyingType;
      result.payload = info->payload;
      result.variants.reserve(info->enumerators.size());
      for (const EnumeratorInfo &enumerator : info->enumerators) {
        result.variants.push_back({.index = enumerator.variantIndex,
                                   .fieldTypes = enumerator.payloadTypes});
      }
      return result;
    }
    const LoweredEnumDeclaration *info = lowered->findEnumDeclaration(id);
    if (info == nullptr) {
      return std::nullopt;
    }
    result.underlyingType = info->underlyingType;
    result.payload = info->payload;
    result.variants.reserve(info->enumerators.size());
    for (const LoweredEnumerator &enumerator : info->enumerators) {
      result.variants.push_back({.index = enumerator.variantIndex,
                                 .fieldTypes = enumerator.payloadTypes});
    }
    return result;
  }

  [[nodiscard]] std::optional<RowSymbolFacts> symbolFacts(SymbolId id) const {
    if (semantics != nullptr) {
      const SymbolRecord *record = semantics->database().findSymbol(id);
      if (record == nullptr) {
        return std::nullopt;
      }
      return RowSymbolFacts{.kind = record->kind,
                            .name = record->name,
                            .qualifiedName = record->qualifiedName,
                            .type = record->type,
                            .internalLinkage = record->internalLinkage};
    }
    const LoweredSymbol *record = lowered->findSymbol(id);
    if (record == nullptr) {
      return std::nullopt;
    }
    return RowSymbolFacts{.kind = record->kind,
                          .name = record->name,
                          .qualifiedName = record->qualifiedName,
                          .type = record->type,
                          .internalLinkage = record->internalLinkage};
  }

  [[nodiscard]] std::optional<RowFunctionFacts>
  functionFacts(FunctionId id) const {
    if (semantics != nullptr) {
      const FunctionInfo *info = semantics->findFunction(id);
      if (info == nullptr || info->declaration == nullptr) {
        return std::nullopt;
      }
      RowFunctionFacts result{
          .name = info->declaration->name().lexeme,
          .namespaceScope = info->namespaceScope,
          .parameterTypes = info->parameterTypes,
          .spelling = cppFunctionSpelling(*semantics, *info->declaration),
          .cLinkage = info->declaration->hasCLinkage(),
          .runtimeBinding = info->declaration->runtimeBinding().has_value()};
      result.genericParameters.reserve(info->genericParameters.size());
      for (const GenericParameterInfo &parameter : info->genericParameters) {
        result.genericParameters.push_back(
            {.id = parameter.id,
             .name = parameter.name.lexeme,
             .pack = parameter.pack,
             .value = parameter.value,
             .constraints = parameter.constraints});
      }
      return result;
    }
    for (const LoweredDeclaration &declaration : lowered->declarations()) {
      const auto *info =
          std::get_if<LoweredFunctionDeclaration>(&declaration.payload);
      if (info == nullptr || info->id != id) {
        continue;
      }
      RowFunctionFacts result{
          .name = declaration.name,
          .namespaceScope = declaration.namespaceScope,
          .genericParameters = info->genericParameters,
          .spelling = cppFunctionSpelling(*info, declaration.name),
          .cLinkage = info->linkage == LanguageLinkage::C,
          .runtimeBinding =
              info->definitionKind == MirDefinitionKind::RuntimeBinding};
      result.parameterTypes.reserve(info->parameters.size());
      for (const LoweredParameter &parameter : info->parameters) {
        result.parameterTypes.push_back(parameter.type);
      }
      return result;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::string>
  lambdaCaptureName(LambdaId declaration, HirLambdaId instanceId,
                    SymbolId symbol, std::size_t index) const {
    if (semantics != nullptr) {
      const LambdaInfo *info = semantics->findLambda(declaration);
      if (info == nullptr) {
        return std::nullopt;
      }
      const auto found =
          std::find_if(info->captures.begin(), info->captures.end(),
                       [symbol](const LambdaCaptureInfo &capture) {
                         return capture.bindingSymbol == symbol;
                       });
      return found == info->captures.end() || found->capture.lexeme.empty()
                 ? std::nullopt
                 : std::optional<std::string>{found->capture.lexeme};
    }
    if (instanceId == 0 || instanceId > lowered->lambdaInstances().size()) {
      return std::nullopt;
    }
    const LoweredLambdaInstance &instance =
        lowered->lambdaInstances()[instanceId - 1];
    if (index >= instance.captures.size() ||
        instance.captures[index].symbol != symbol ||
        instance.captures[index].name.empty()) {
      return std::nullopt;
    }
    return instance.captures[index].name;
  }

private:
  const SemanticModel *semantics = nullptr;
  const LoweredProgram *lowered = nullptr;
};

// Deterministic first-seen type-row collection with the exact argument
// closure the emission analysis recurses through.
struct RowsBuilder {
  const RowFactProvider &facts;
  const MirProgram &mir;
  CppStandard standard;
  CppMirBodyEmissionMapRows rows;

  void addType(const SemanticType &type) {
    if (type == SemanticType::Unknown) {
      return;
    }
    // A C++ closure type is unnameable: any row for a Lambda-kind type
    // would carry a forged spelling. The closure-chain and template
    // vocabularies own these types row-free (a template emission injects
    // its own overlay row spelling the template parameter name).
    if (type.kind == SemanticType::Lambda) {
      return;
    }
    const std::optional<CppMirTypeRepresentationKind> kind =
        cppMirExpectedTypeRepresentation(type);
    if (kind && std::none_of(rows.types.begin(), rows.types.end(),
                             [&](const CppMirTypeRepresentation &row) {
                               return row.type == type;
                             })) {
      // A Class row carries the 0.215 boundary proof: a usable default
      // constructor and move assignment let a value of the type declare
      // in the prelude and receive its construction by assignment.
      bool boundaryConstructible = false;
      bool copyable = false;
      std::string templateNameSpelling;
      if (type.kind == SemanticType::Class) {
        const std::optional<RowClassFacts> classInfo =
            facts.classFacts(type.classId);
        boundaryConstructible =
            classInfo &&
            (classInfo->passiveCAbiRecord ||
             (classInfo->defaultConstructor != SpecialMemberStatus::Deleted &&
              classInfo->moveAssignment != SpecialMemberStatus::Deleted));
        copyable =
            classInfo &&
            (classInfo->passiveCAbiRecord ||
             (classInfo->copyConstructor != SpecialMemberStatus::Deleted &&
              classInfo->copyAssignment != SpecialMemberStatus::Deleted));
        if (!type.arguments.empty() || !type.valueArguments.empty()) {
          SemanticType primary = type;
          primary.arguments.clear();
          primary.valueArguments.clear();
          templateNameSpelling = facts.typeSpelling(standard, primary);
        }
      }
      rows.types.push_back(
          {.type = type,
           .kind = *kind,
           .spelling = facts.typeSpelling(standard, type),
           .templateNameSpelling = std::move(templateNameSpelling),
           .boundaryConstructible = boundaryConstructible,
           .copyable = copyable});
      // An enum type's declaration authority is its enum row; copy it
      // alongside the type row so a boundary that carries the enum can
      // prove its variant inventory.
      if (type.kind == SemanticType::Enum) {
        addEnum(type.enumId);
      }
    }
    for (const SemanticType &argument : type.arguments) {
      addType(argument);
    }
    for (const SemanticType &argument : type.lambdaEnclosingClassTypes) {
      addType(argument);
    }
    for (const SemanticType &argument : type.lambdaEnclosingFunctionTypes) {
      addType(argument);
    }
  }

  void addEnum(EnumId owner) {
    if (owner == 0 || std::any_of(rows.enums.begin(), rows.enums.end(),
                                  [owner](const CppMirEnumRepresentation &row) {
                                    return row.owner == owner;
                                  })) {
      return;
    }
    const std::optional<RowEnumFacts> info = facts.enumFacts(owner);
    if (!info) {
      return;
    }
    SemanticType type;
    type.kind = SemanticType::Enum;
    type.enumId = owner;
    CppMirEnumRepresentation row{.owner = owner,
                                 .spelling = facts.typeSpelling(standard, type),
                                 .underlyingType = info->underlyingType};
    // A payload enum's row copies the compatibility record layout: each
    // enumerator is a __gti_variant_<index> alternative of the __gti_value
    // variant, holding its payload fields in declaration order.
    if (info->payload) {
      for (const RowEnumVariantFacts &enumerator : info->variants) {
        row.payloadVariants.push_back(
            {.index = enumerator.index,
             .spelling = "__gti_variant_" + std::to_string(enumerator.index),
             .fieldTypes = enumerator.fieldTypes});
        for (const SemanticType &field : enumerator.fieldTypes) {
          addType(field);
        }
      }
    }
    rows.enums.push_back(std::move(row));
    addType(type);
    addType(info->underlyingType);
  }

  void addOperand(const MirOperand &operand) { addType(operand.type); }

  void addBody(const MirBody &body) {
    for (const MirPlace &place : body.places) {
      addType(place.type);
    }
    for (const MirValue &value : body.values) {
      addType(value.info.type);
    }
    addType(body.returnType);
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        addType(instruction.info.type);
        for (const SemanticType &type : instruction.parameterTypes) {
          addType(type);
        }
        for (const SemanticType &type : instruction.closureCaptureTypes) {
          addType(type);
        }
        if (instruction.receiver) {
          addOperand(*instruction.receiver);
        }
        for (const MirOperand &operand : instruction.operands) {
          addOperand(operand);
        }
        if (instruction.enumOwner) {
          addEnum(*instruction.enumOwner);
        }
      }
      if (block.terminator.value) {
        addOperand(*block.terminator.value);
      }
      for (const MirSwitchTarget &target : block.terminator.switchTargets) {
        if (!target.value) {
          continue;
        }
        addType(target.value->type);
        if (target.value->enumOwner != 0) {
          addEnum(target.value->enumOwner);
        }
      }
    }
  }
};

} // namespace

static CppMirBodyEmissionMapRows buildCppMirBodyEmissionMapRowsFromFacts(
    const RowFactProvider &facts, const MirProgram &mir, CppStandard standard) {
  RowsBuilder builder{.facts = facts, .mir = mir, .standard = standard};

  for (const MirBodyAddress address : enumerateMirBodyAddresses(mir)) {
    if (const MirBody *body = findMirBody(mir, address)) {
      builder.addBody(*body);
    }
  }

  // The emission analysis also requires instance-level owner metadata types
  // that need not appear inside any body structure: signatures, owner class
  // types, constructor initializer targets, and lambda captures.
  for (const MirClassInstance &instance : mir.classInstances()) {
    builder.addType(instance.type);
  }
  for (const MirFunctionInstance &function : mir.functionInstances()) {
    builder.addType(function.returnType);
    for (const SemanticType &type : function.parameterTypes) {
      builder.addType(type);
    }
    for (const MirCallableParameter &parameter : function.callableParameters) {
      builder.addType(parameter.callableType);
      for (const MirCallableSignature &signature : parameter.signatures) {
        builder.addType(signature.returnType);
        for (const SemanticType &type : signature.parameterTypes) {
          builder.addType(type);
        }
      }
    }
  }
  for (const MirConstructorInstance &constructor : mir.constructorInstances()) {
    for (const SemanticType &type : constructor.parameterTypes) {
      builder.addType(type);
    }
    for (const MirConstructorInitializer &initializer :
         constructor.initializers) {
      builder.addType(initializer.targetType);
    }
    // A constructor's emitted name is its owner type's spelling; a
    // destructor's appends the source class name. Declarations stay rowless.
    if (constructor.definitionKind == MirDefinitionKind::Source) {
      if (const MirClassInstance *owner =
              mir.findClassInstance(constructor.owner)) {
        builder.rows.bodies.push_back(
            {.address = {.kind = MirBodyKind::Constructor,
                         .owner = constructor.id},
             .spelling = facts.typeSpelling(standard, owner->type)});
      }
    }
  }
  for (const MirDestructorInstance &destructor : mir.destructorInstances()) {
    if (destructor.definitionKind != MirDefinitionKind::Source) {
      continue;
    }
    const MirClassInstance *owner = mir.findClassInstance(destructor.owner);
    const std::optional<RowClassFacts> info =
        owner == nullptr ? std::nullopt : facts.classFacts(owner->declaration);
    if (owner == nullptr || !info) {
      continue;
    }
    builder.rows.bodies.push_back(
        {.address = {.kind = MirBodyKind::Destructor, .owner = destructor.id},
         .spelling =
             facts.typeSpelling(standard, owner->type) + "::~" + info->name});
  }

  // Capture name rows: each lambda instance's captures spell exactly the
  // source capture names the compatibility literal prints, matched from
  // the semantic capture list by binding symbol.
  for (const MirLambdaInstance &lambda : mir.lambdaInstances()) {
    for (std::size_t index = 0; index < lambda.captureSymbols.size(); ++index) {
      const SymbolId symbol = lambda.captureSymbols[index];
      if (symbol == 0 || index >= lambda.captureTypes.size()) {
        continue;
      }
      const std::optional<std::string> name =
          facts.lambdaCaptureName(lambda.declaration, lambda.id, symbol, index);
      if (name) {
        builder.rows.symbols.push_back(
            {.kind = CppMirSymbolRepresentationKind::Capture,
             .owner = lambda.id,
             .symbol = symbol,
             .ordinal = index + 1,
             .type = lambda.captureTypes[index],
             .spelling = *name});
      }
    }
  }

  // A verified no-argument hosted-startup body's emitted name is the
  // program entry adapter itself.
  if ((cppMirHostedStartupNoArgumentsSchedule(mir) ||
       cppMirHostedStartupFailureFreeSchedule(mir)) &&
      mir.hostedStartupPlan()) {
    builder.rows.bodies.push_back(
        {.address = {.kind = MirBodyKind::HostedStartup,
                     .owner = mir.hostedStartupPlan()->entry},
         .spelling = "::main"});
  }

  // The executable module body carries its own name row like every other
  // executable body; it is never a call target.
  if (!mir.programInitializationPlan().steps.empty()) {
    builder.rows.bodies.push_back(
        {.address = {.kind = MirBodyKind::Module, .owner = 0},
         .spelling = "::__gti_program::__gti_module_initialization"});
  }

  // The sealed helpers the emitted artifact actually ships today. Naming
  // them here is what lets bodies that need them become analysis-Ready; the
  // text step's vocabulary still decides what actually emits.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::LifetimeStorage,
       .spelling = "::gti_internal::backend::mir_lifetime_slot"});
  // Program initialization names the shipped representation of the
  // merged module schedule: every staged step's definition carries its
  // initializer in the emitted declaration surface, so C++ static
  // initialization in plan order performs the module body's work and
  // the hosted CallProgramInitialization operation stages nothing.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::ProgramInitialization,
       .spelling = "cpp_static_initialization_v1"});
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::DefinedFailure,
       .spelling = "mir_failure_status_v1"});
  // Native interop names the artifact's shipped C-symbol surface: every
  // `gti_rt_*` call a MIR body can emit targets a prototype the artifact
  // declares (runtime/include/gti/runtime.h) and links from gti_runtime,
  // spelled identically to compatibility emission through its body-name
  // row. Intrinsic names the shipped `::gti_internal::backend` helper
  // family the compatibility path lowers intrinsics through; the text
  // vocabulary still declines any intrinsic call it cannot spell, so the
  // row moves analysis honesty, not emission.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::NativeInterop,
       .spelling = "gti_rt_c_symbols_v1"});
  // Bounds names the shipped checked fixed-array access family: the
  // mir_checked_array_read/write_v1 helpers carry the bounds check, the
  // status protocol, and the exact INDEX_OUT_OF_BOUNDS record data.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::Bounds,
       .spelling = "mir_checked_array_access_v1"});
  // Borrow names the loan erasure contract (ADR 018): loans lower to
  // typed pointers with deref-at-use, constness follows access mode,
  // endpoints are compile-proven with no runtime action, and references
  // survive only at ABI boundaries.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::Borrow,
       .spelling = "mir_loan_pointer_v1"});
  // RawMemory names the direct C++ raw-pointer vocabulary. Unsafe permission,
  // pointee access, and operation types are already fixed by semantics and
  // verified MIR; the backend spells only one-level address formation,
  // pointer arithmetic/difference, and raw index/dereference lvalue views.
  // There is no owning runtime primitive or inferred safety check here.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::RawMemory,
       .spelling = "cpp_raw_pointer_operations_v1"});
  // Aggregate names the value-initialization representation the emitted
  // artifact uses for empty aggregates: the row type's braced value
  // initialization (std::array<T, N>{}).
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::Aggregate,
       .spelling = "cpp_value_initialization_v1"});
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::Intrinsic,
       .spelling = "gti_internal_backend_helpers_v1"});
  // HostedEntry names the shipped program-entry adapter: every hosted
  // artifact carries the generated main() that marshals arguments,
  // forwards the entry call, and routes defined failure through the
  // terminal containment contract. The entry function's own body is
  // ordinary; this row is what its analysis demand resolves against.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::HostedEntry,
       .spelling = "gti_hosted_entry_adapter_v1"});
  // Expected names the artifact's shipped expected representation. The
  // row's spelling is the exact error-construction call the compatibility
  // path emits for the selected standard — std::unexpected under C++23,
  // the vendored expected-lite constructor under C++20 — so Unexpected
  // text is copied from this row and can never drift by standard.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::Expected,
       .spelling = standard == CppStandard::Cpp23
                       ? "std::unexpected"
                       : "::nonstd::make_unexpected"});
  // Closure names the inline C++ lambda literal representation: a Closure
  // compute spells `[name = <place>, ...](<params>) -> <ret> { <verified
  // body> }` with capture names copied from the lambda's Capture rows,
  // exactly like the compatibility path's inline lambda emission. C++
  // closure types are unnameable, so the chain fuses into its consuming
  // invocations and nothing here is ever a spelled type or call target.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::Closure,
       .spelling = "cpp_inline_lambda_v1"});
  // CallableDispatch names the deduction-based callable representation:
  // a callable value invokes as `<literal>(<args>)` and a callable-typed
  // argument passes by template-argument deduction, never through a
  // spelled closure type name. The text vocabulary still declines every
  // callable shape it cannot fuse, so the row moves analysis honesty,
  // not emission.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::CallableDispatch,
       .spelling = "cpp_deduced_callable_v1"});
  // Payload names the variant record representation the artifact ships
  // for payload enums: a struct wrapping std::variant of per-enumerator
  // __gti_variant_<index> records, constructed by wrapping the variant
  // record and read by std::get over the proven alternative. The enum
  // row's copied variant inventory carries the exact spellings.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::Payload,
       .spelling = "cpp_variant_record_v1"});
  // VirtualDispatch names the C++ virtual member representation: a
  // virtual or override method's emitted in-class definition carries the
  // language's own dispatch, and a virtual call spells the ordinary
  // member call through the base-typed receiver. The text vocabulary
  // still declines any dispatch shape it cannot spell.
  builder.rows.capabilities.push_back(
      {.kind = CppMirEmissionCapabilityKind::VirtualDispatch,
       .spelling = "cpp_virtual_member_v1"});

  // Executable per-instance field-initializer bodies carry their own name
  // row like every other executable body; the spelling is the owner scope
  // of the definitions the transitional emitter writes for them. They are
  // never call targets.
  for (const MirClassInstance &instance : mir.classInstances()) {
    const MirBody &fieldBody = instance.fieldInitializers;
    const bool executable =
        !fieldBody.blocks.empty() &&
        std::any_of(
            fieldBody.blocks.begin(), fieldBody.blocks.end(),
            [](const MirBlock &block) { return !block.instructions.empty(); });
    if (!executable) {
      continue;
    }
    builder.rows.bodies.push_back(
        {.address = {.kind = MirBodyKind::FieldInitializers,
                     .owner = instance.id},
         .spelling = facts.typeSpelling(standard, instance.type) +
                     "::__gti_field_initializers"});
  }
  for (const MirLambdaInstance &lambda : mir.lambdaInstances()) {
    // A lambda body is never a call target: it spells only nested inside
    // its closure literal. The row exists so a Closure site can prove the
    // exact body target is representable before fusing it.
    builder.rows.bodies.push_back(
        {.address = {.kind = MirBodyKind::Lambda, .owner = lambda.id},
         .spelling = "__gti_inline_lambda_" + std::to_string(lambda.id)});
    builder.addType(lambda.type);
    builder.addType(lambda.returnType);
    for (const SemanticType &type : lambda.parameterTypes) {
      builder.addType(type);
    }
    for (const SemanticType &type : lambda.captureTypes) {
      builder.addType(type);
    }
  }

  // Namespace-global storage rows for every Symbol-rooted place MIR reads.
  // The spelling is copied from semantic symbol identity, including nested
  // source namespaces and internal-linkage holder storage.
  for (const MirBodyAddress address : enumerateMirBodyAddresses(mir)) {
    const MirBody *body = findMirBody(mir, address);
    if (body == nullptr) {
      continue;
    }
    for (const MirPlace &place : body->places) {
      if (place.root != MirPlaceRootKind::Symbol || place.capture != 0 ||
          place.symbol == 0 || !place.projections.empty()) {
        continue;
      }
      const MirProgramInitializationStep *step =
          mir.programInitializationPlan().findStepForSymbol(place.symbol);
      if (step != nullptr && step->ownerClass != 0) {
        continue;
      }
      if (std::any_of(builder.rows.symbols.begin(), builder.rows.symbols.end(),
                      [&](const CppMirSymbolRepresentation &row) {
                        return row.kind ==
                                   CppMirSymbolRepresentationKind::Storage &&
                               row.owner == 0 && row.symbol == place.symbol;
                      })) {
        continue;
      }
      const std::optional<RowSymbolFacts> record =
          facts.symbolFacts(place.symbol);
      if (!record || record->name.empty()) {
        continue;
      }
      // An enumerator read spells the enum type's qualified constant; the
      // enum spelling is the same authority the enum rows carry. Payload
      // enums stay rowless — their variants are not plain constants.
      if (record->kind == SymbolKind::Enumerator &&
          place.type.kind == SemanticType::Enum) {
        const std::optional<RowEnumFacts> enumInfo =
            facts.enumFacts(place.type.enumId);
        const std::string enumSpelling =
            facts.typeSpelling(standard, place.type);
        if (!enumInfo || enumInfo->payload || enumSpelling.empty()) {
          continue;
        }
        builder.rows.symbols.push_back(
            {.kind = CppMirSymbolRepresentationKind::Storage,
             .owner = 0,
             .symbol = place.symbol,
             .ordinal = 0,
             .type = place.type,
             .spelling = enumSpelling + "::" + record->name});
        builder.addType(place.type);
        continue;
      }
      if (record->kind != SymbolKind::GlobalVariable) {
        continue;
      }
      std::string spelling = cppProgramStorageSpelling(*record, place.symbol);
      if (spelling.empty()) {
        continue;
      }
      builder.rows.symbols.push_back(
          {.kind = CppMirSymbolRepresentationKind::Storage,
           .owner = 0,
           .symbol = place.symbol,
           .ordinal = 0,
           .type = place.type,
           .spelling = std::move(spelling)});
      builder.addType(place.type);
    }
  }

  // Program-initialization steps for plain namespace globals also carry
  // storage rows: a constexpr global's reads are frontend-substituted, so
  // no Symbol-rooted place ever demands the row above, yet the module
  // body's own Binding places still resolve their storage through it.
  for (const MirProgramInitializationStep &step :
       mir.programInitializationPlan().steps) {
    if (step.symbol == 0) {
      continue;
    }
    // A class-owned static's storage row spells the class-qualified
    // member, exactly the compatibility definition's access path.
    if (step.ownerClass != 0) {
      const MirClassInstance *ownerInstance =
          mir.findClassInstance(step.ownerClass);
      const std::optional<RowSymbolFacts> record =
          facts.symbolFacts(step.symbol);
      const MirPlace *storage = mir.module().findPlace(step.storagePlace);
      if (ownerInstance == nullptr || !record || storage == nullptr ||
          record->name.empty() ||
          std::any_of(builder.rows.symbols.begin(), builder.rows.symbols.end(),
                      [&](const CppMirSymbolRepresentation &row) {
                        return row.kind ==
                                   CppMirSymbolRepresentationKind::Storage &&
                               row.owner == step.ownerClass &&
                               row.symbol == step.symbol;
                      })) {
        continue;
      }
      builder.rows.symbols.push_back(
          {.kind = CppMirSymbolRepresentationKind::Storage,
           .owner = step.ownerClass,
           .symbol = step.symbol,
           .ordinal = 0,
           .type = storage->type,
           .spelling = facts.typeSpelling(standard, ownerInstance->type) +
                       "::" + record->name});
      builder.addType(storage->type);
      continue;
    }
    if (std::any_of(builder.rows.symbols.begin(), builder.rows.symbols.end(),
                    [&](const CppMirSymbolRepresentation &row) {
                      return row.kind ==
                                 CppMirSymbolRepresentationKind::Storage &&
                             row.owner == 0 && row.symbol == step.symbol;
                    })) {
      continue;
    }
    const std::optional<RowSymbolFacts> record = facts.symbolFacts(step.symbol);
    const MirPlace *storage = mir.module().findPlace(step.storagePlace);
    if (!record || storage == nullptr) {
      continue;
    }
    std::string spelling = cppProgramStorageSpelling(*record, step.symbol);
    if (spelling.empty()) {
      continue;
    }
    builder.rows.symbols.push_back(
        {.kind = CppMirSymbolRepresentationKind::Storage,
         .owner = 0,
         .symbol = step.symbol,
         .ordinal = 0,
         .type = storage->type,
         .spelling = std::move(spelling)});
    builder.addType(storage->type);
  }

  for (const MirClassInstance &instance : mir.classInstances()) {
    for (const MirClassFieldInfo &field : instance.declaredFields) {
      const std::optional<RowSymbolFacts> record =
          facts.symbolFacts(field.symbol);
      if (!record || record->name.empty()) {
        continue;
      }
      std::string declarationTypeSpelling;
      if (record->type != field.type && containsDependentType(record->type)) {
        declarationTypeSpelling = facts.typeSpelling(standard, record->type);
        if (declarationTypeSpelling == "void") {
          declarationTypeSpelling.clear();
        }
      }
      builder.rows.symbols.push_back(
          {.kind = CppMirSymbolRepresentationKind::Field,
           .owner = instance.id,
           .symbol = field.symbol,
           .ordinal = 0,
           .type = field.type,
           .spelling = record->name,
           .declarationTypeSpelling = std::move(declarationTypeSpelling)});
      builder.addType(field.type);
    }
  }

  for (const MirFunctionInstance &function : mir.functionInstances()) {
    // Every function instance gets its emitted name row: source definitions
    // carry their definition spelling, runtime bindings and C-linkage
    // declarations their exact external names. Only the namespace-scope
    // source-defined GTI form is also a valid call-target spelling; the
    // scalar call family's gates admit exactly that form.
    const std::optional<RowFunctionFacts> info =
        facts.functionFacts(function.declaration);
    if (!info) {
      continue;
    }
    std::string spelling;
    if (function.owner) {
      const MirClassInstance *ownerInstance =
          mir.findClassInstance(*function.owner);
      if (ownerInstance == nullptr) {
        continue;
      }
      spelling = facts.typeSpelling(standard, ownerInstance->type);
      spelling += "::";
    } else if (!info->cLinkage && !info->runtimeBinding) {
      spelling = "::__gti_program";
      for (std::size_t index = 0; index < info->namespaceScope.size();
           ++index) {
        const std::string &scope = info->namespaceScope[index];
        spelling += "::";
        spelling += index == 0 && scope == "std"
                        ? std::string(cppEmittedStandardNamespace)
                        : scope;
      }
      spelling += "::";
    } else if (!info->namespaceScope.empty()) {
      // A C-linkage or runtime-binding declaration keeps its exact
      // external symbol, but extern "C" affects linkage, not C++ name
      // lookup: when the emitted declaration lives inside a namespace,
      // the call-target spelling must name that namespace.
      spelling = "::__gti_program";
      for (std::size_t index = 0; index < info->namespaceScope.size();
           ++index) {
        const std::string &scope = info->namespaceScope[index];
        spelling += "::";
        spelling += index == 0 && scope == "std"
                        ? std::string(cppEmittedStandardNamespace)
                        : scope;
      }
      spelling += "::";
    }
    spelling += info->spelling;
    // A generic parameter that appears in no parameter type cannot be
    // deduced at a call site, so the instance's call spelling carries its
    // substituted arguments explicitly — exactly like the compatibility
    // call site. Deducible instances keep the bare name so shipped
    // spellings stay byte-stable.
    if (!function.typeArguments.empty() && !info->genericParameters.empty()) {
      const auto mentionsParameter = [&](const GenericParameterId parameter) {
        const auto mentions = [&](const auto &self,
                                  const SemanticType &type) -> bool {
          if (type.kind == SemanticType::TypeParameter &&
              type.genericParameterId == parameter) {
            return true;
          }
          for (const SemanticType &argument : type.arguments) {
            if (self(self, argument)) {
              return true;
            }
          }
          return false;
        };
        for (const SemanticType &parameterType : info->parameterTypes) {
          if (mentions(mentions, parameterType)) {
            return true;
          }
        }
        return false;
      };
      bool needsExplicit = false;
      for (const LoweredGenericParameter &parameter : info->genericParameters) {
        if (!parameter.pack && !mentionsParameter(parameter.id)) {
          needsExplicit = true;
          break;
        }
      }
      if (needsExplicit) {
        spelling += '<';
        for (std::size_t index = 0; index < function.typeArguments.size();
             ++index) {
          if (index != 0) {
            spelling += ", ";
          }
          spelling +=
              facts.typeSpelling(standard, function.typeArguments[index]);
        }
        spelling += '>';
      }
    }
    builder.rows.bodies.push_back(
        {.address = {.kind = MirBodyKind::Function, .owner = function.id},
         .spelling = std::move(spelling)});
  }

  return builder.rows;
}

CppMirBodyEmissionMapRows
buildCppMirBodyEmissionMapRows(const SemanticModel &semantics,
                               const MirProgram &mir, CppStandard standard) {
  const RowFactProvider facts(semantics);
  return buildCppMirBodyEmissionMapRowsFromFacts(facts, mir, standard);
}

CppMirBodyEmissionMapRows
buildCppMirBodyEmissionMapRows(const LoweredProgram &program,
                               CppStandard standard) {
  const RowFactProvider facts(program);
  return buildCppMirBodyEmissionMapRowsFromFacts(facts, program.mir(),
                                                 standard);
}

} // namespace lang
