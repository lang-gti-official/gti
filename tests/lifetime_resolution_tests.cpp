#include "../src/compiler/semantic_analyzer_impl.h"
#include "gti/frontend.h"
#include "gti/lifetime_types.h"
#include "gti/semantic_analyzer.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace lang {
struct SemanticModelTestAccess {
  static void aliasCheckedExpression(SemanticModel &model,
                                     const Expr &expression,
                                     const Expr &target) {
    model.recordExpressionAlias(
        expression, std::shared_ptr<const Expr>(&target, [](const Expr *) {}));
  }

  static bool contentsDeltaIsolation(const SemanticModel &base,
                                     const Expr &expression,
                                     SemanticLoanId opening) {
    if (!opening || base.findReferenceContentsLoan(expression) != opening ||
        !base.findLoan(opening))
      return false;
    SemanticModel instance;
    instance.beginInstanceDelta(base);
    instance.recordLoan({.id = opening, .kind = SemanticLoanKind::Reference});
    if (!instance.findLoan(opening) ||
        instance.findReferenceContentsLoan(expression))
      return false;
    instance.clearReferenceContentsLoan(expression);
    if (!instance.referenceContentsLoans.empty())
      return false;
    SemanticModel speculative;
    speculative.beginSpeculativeDelta(base);
    if (speculative.findReferenceContentsLoan(expression) != opening ||
        !speculative.findLoan(opening) ||
        speculative.findLoan(opening)->contentsOpening !=
            base.findLoan(opening)->contentsOpening)
      return false;
    const auto count = speculative.referenceContentsLoans.size();
    speculative.clearReferenceContentsLoan(expression);
    return count != 0 &&
           speculative.referenceContentsLoans.size() + 1 == count &&
           !speculative.findReferenceContentsLoan(expression) &&
           base.findReferenceContentsLoan(expression) == opening;
  }
};

// Exercise the real staging key and lookup without exposing cache internals as
// a product API or relying on a timing-sensitive number of source evaluations.
struct SemanticLifetimeTestAccess {
  static bool scopedProjectionCatalogCopies() {
    SemanticVisitor analyzer(TargetInfo::host());
    auto &catalog = analyzer.impl->catalog;
    TypeRef syntax(Token{}), unknown(Token{}), wrapped(Token{});
    const auto invocation = Lifetime::parameter(9001);
    auto result = SemanticType::referenceTo(SemanticType::Int32);
    result.referenceLifetime = invocation;
    auto subject = SemanticType::functionSignature(result, {result});
    subject.quantifiedLifetimes = {invocation.id};
    catalog.compilerTypeClasses.emplace(
        SemanticVisitor::Impl::CompilerTypeKind::MetaCallableReturnType, 71);
    analyzer.impl->recordCallableResultProjection(syntax, {71, subject});
    if (!catalog.callableResultProjectionRecipes.empty())
      return false; // An incidental type query is not a declaration recipe.
    analyzer.impl->registeringProjectionSyntax = &syntax;
    analyzer.impl->recordCallableResultProjection(syntax, {71, subject});
    analyzer.impl->registeringProjectionSyntax = &unknown;
    analyzer.impl->recordCallableResultProjection(unknown, {72, subject});
    wrapped.reference = Token{};
    analyzer.impl->registeringProjectionSyntax = &wrapped;
    analyzer.impl->recordCallableResultProjection(wrapped, {71, subject});
    if (catalog.callableResultProjectionRecipes.size() != 1 ||
        !catalog.callableResultProjectionRecipes.at(&syntax))
      return false;
    catalog.normalizedNominalTypes.emplace(&syntax, result);
    catalog.nominalLifetimeContractsReady = true;
    catalog.seal();
    auto copy = catalog;
    auto instance = catalog.snapshotForInstanceAnalysis();
    if (copy.callableResultProjectionRecipes !=
            catalog.callableResultProjectionRecipes ||
        instance.callableResultProjectionRecipes !=
            catalog.callableResultProjectionRecipes ||
        instance.normalizedNominalTypes != catalog.normalizedNominalTypes ||
        !instance.nominalLifetimeContractsReady ||
        instance.sealedShape != catalog.sealedShape)
      return false;
    analyzer.impl->registeringProjectionSyntax = &unknown;
    analyzer.impl->recordCallableResultProjection(unknown, {71, subject});
    if (catalog.callableResultProjectionRecipes.size() != 1)
      return false;
    copy.callableResultProjectionRecipes.at(&syntax).reset();
    instance.normalizedNominalTypes.at(&syntax) = SemanticType::Bool;
    if (!catalog.callableResultProjectionRecipes.at(&syntax) ||
        catalog.normalizedNominalTypes.at(&syntax) != result)
      return false;
    catalog.sealed = false;
    catalog.sealedShape.clear();
    analyzer.impl->registeringProjectionSyntax = &syntax;
    analyzer.impl->recordCallableResultProjection(syntax,
                                                  {71, SemanticType::Int32});
    analyzer.impl->recordCallableResultProjection(syntax, {71, subject});
    return !catalog.callableResultProjectionRecipes.at(&syntax);
  }

  static std::pair<bool, std::size_t>
  ownedTransportApplicationChecks(const FrontendResult &source) {
    SemanticVisitor analyzer(TargetInfo::host(), &source.sourceGraph, true,
                             nullptr, &source.sources);
    if (!analyzer.check(source.program))
      return {};
    const ResolvedCallInfo *selected = nullptr;
    for (const auto &declaration : source.program.declarations())
      if (const auto *main = stmtAs<FunctionDecl>(declaration.get());
          main && main->name().lexeme == "main" && main->body())
        for (const auto &statement : main->body()->statements())
          if (const auto *binding = stmtAs<VariableDecl>(statement.get());
              binding && binding->name().lexeme == "stored")
            if (const auto *call = exprAs<Call>(binding->initializer().get()))
              selected = analyzer.model().findCall(*call);
    const auto *target =
        selected ? analyzer.model().findFunction(selected->function) : nullptr;
    if (!target || !selected->lifetimeApplication.complete ||
        selected->lifetimeApplication.bindings.size() != 1)
      return {};
    const auto contracts =
        target->instantiatedCallableParameters(selected->parameterTypes);
    if (contracts.size() != 1 || !contracts.front().ownedTransport ||
        contracts.front().ownedTransport->kind !=
            CallableOwnedTransportKind::ExactField)
      return {};
    const auto exact = analyzer.impl->ownedTransportMatchesConcrete(
        *target, contracts.front(), *selected);
    const auto actual = selected->lifetimeApplication.bindings.front().second;
    std::size_t rejected = 0;
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
      auto call = *selected;
      auto contract = contracts.front();
      switch (mutation) {
      case 0:
        call.lifetimeApplication.bindings.clear();
        break;
      case 1:
        call.lifetimeApplication.bindings.front().second =
            Lifetime::staticLifetime();
        break;
      case 2:
        call.lifetimeApplication.bindings.front().second =
            Lifetime::inference(900001);
        break;
      case 3:
        call.lifetimeApplication.bindings.emplace_back(
            call.lifetimeApplication.bindings.front().first,
            Lifetime::staticLifetime());
        break;
      case 4:
        contract.ownedTransport->field = 0;
        break;
      case 5:
      case 6: {
        const std::vector<LifetimeBinding> replacement{
            {actual, Lifetime::staticLifetime()}};
        auto &type =
            mutation == 5 ? call.returnType : call.parameterTypes.front();
        const auto changed = substituteTypeLifetimes(type, replacement);
        if (!changed || *changed == type)
          return {};
        type = *changed;
        break;
      }
      case 7:
        contract.ownedTransport->field += 900001;
        break;
      }
      rejected += !analyzer.impl->ownedTransportMatchesConcrete(*target,
                                                                contract, call);
    }
    return {exact, rejected};
  }

  static std::pair<bool, std::size_t>
  closedDefaultContractChecks(const FrontendResult &source) {
    SemanticVisitor analyzer(TargetInfo::host(), &source.sourceGraph, true,
                             nullptr, &source.sources);
    if (!analyzer.check(source.program))
      return {};
    const ClassTypeInfo *record = nullptr;
    const TypeAliasInfo *selection = nullptr;
    for (const auto &declaration : source.program.declarations()) {
      if (const auto *syntax = stmtAs<ClassDecl>(declaration.get());
          syntax && syntax->name().lexeme == "Projected")
        record = analyzer.model().findClassType(*syntax);
      if (const auto *syntax = stmtAs<TypeAliasDecl>(declaration.get());
          syntax && syntax->name().lexeme == "Selection")
        selection = analyzer.model().findTypeAlias(*syntax);
    }
    if (!record || !selection)
      return {};
    auto &owner = analyzer.impl->classInfo(record->id);
    if (owner.constructors.size() != 1 ||
        owner.genericArgumentPatternLifetimes.size() != 1)
      return {};
    auto &constructor = owner.constructors.front();
    const auto original = constructor;
    const auto region =
        Lifetime::parameter(owner.genericArgumentPatternLifetimes.front().id);
    const InstanceAnalysisKey key{.kind = InstanceAnalysisKind::Constructor,
                                  .declaration = constructor.id,
                                  .classTypes = {selection->type}};
    const auto canonical = analyzer.canonicalizeExecutableInstance(key);
    const bool closed =
        canonical && canonical->lifetimeContract.valid() &&
        canonical->lifetimeApplication.bindings.empty() &&
        !canonical->callableScopes.empty() &&
        std::find(canonical->lifetimeContract.parameters.begin(),
                  canonical->lifetimeContract.parameters.end(),
                  region) == canonical->lifetimeContract.parameters.end() &&
        freeTypeLifetimes(canonical->ownerType).empty() &&
        !canonical->ownerType.arguments.front().quantifiedLifetimes.empty();
    std::size_t rejected = 0;
    const auto escaped = SemanticType::referenceTo(
        SemanticType::Int32, AccessMode::ReadOnly, region);
    for (unsigned mutation = 0; mutation < 11; ++mutation) {
      constructor = original;
      auto &contract = constructor.lifetimeContract;
      switch (mutation) {
      case 0:
        contract.bounds.push_back({region, Lifetime::staticLifetime()});
        break;
      case 1:
        contract.typeBounds.push_back({SemanticType::Int32, region});
        break;
      case 2:
        contract.typeBounds.push_back({escaped, Lifetime::staticLifetime()});
        break;
      case 3:
        contract.requirements = {
            .nodes = {{.kind = LifetimeRequirementNode::Kind::Region,
                       .bound = {region, Lifetime::staticLifetime()}}},
            .root = 0};
        break;
      case 4:
        contract.requirements = {
            .nodes = {{.kind = LifetimeRequirementNode::Kind::Type,
                       .bound = {{}, Lifetime::staticLifetime()},
                       .type = escaped}},
            .root = 0};
        break;
      case 5:
        constructor.requirements.push_back({.lifetimeArguments = {region}});
        break;
      case 6:
        constructor.requirements.push_back(
            {.lifetimeBound =
                 LifetimeBound{region, Lifetime::staticLifetime()}});
        break;
      case 7:
        constructor.requirements.push_back({.outlivesType = escaped});
        break;
      case 8:
        constructor.requirements.push_back({.arguments = {escaped}});
        break;
      case 9:
        contract.receiver = region;
        break;
      case 10:
        constructor.parameterTypes.push_back(escaped);
        break;
      }
      rejected += !analyzer.canonicalizeExecutableInstance(key).has_value();
    }
    constructor = original;
    return {closed, rejected};
  }

  static bool exactStaticReturnSupplier(const FunctionInfo &function) {
    return SemanticVisitor::Impl::hasExactStaticReturnSupplier(function);
  }

  struct ContentsAdapterChecks {
    std::size_t formed = 0;
    std::size_t normalized = 0;
    std::size_t addressPreserved = 0;
    std::size_t staleRejected = 0;
    std::size_t missingReadRejected = 0;
    std::size_t cleanupReadRejected = 0;
    std::size_t staleCallRejected = 0;
    std::size_t unsupportedRejected = 0;
    std::size_t deltaIsolated = 0;
    std::size_t callInvalidated = 0;
    std::size_t noncallPreserved = 0;
    std::size_t aliasPreserved = 0;
    std::size_t reevaluationReset = 0;
    bool entryReused = false;
  };

  static ContentsAdapterChecks
  referenceContentsAdapters(const FrontendResult &source) {
    ContentsAdapterChecks checks;
    SemanticVisitor analyzer(TargetInfo::host(), &source.sourceGraph, true,
                             nullptr, &source.sources);
    if (!analyzer.check(source.program))
      return checks;
    auto &impl = *analyzer.impl;
    using Impl = SemanticVisitor::Impl;
    impl.nextSemanticLoanId = analyzer.model().loans().size() + 1;
    std::vector<const Expr *> effects;
    const Expr *aliasEffect = nullptr;
    const auto resultExpression = [&](const auto &self,
                                      const Stmt *statement) -> const Expr * {
      if (const auto *returned = stmtAs<ReturnStmt>(statement))
        return returned->value().get();
      if (const auto *block = stmtAs<BlockStmt>(statement))
        for (const auto &child : block->statements())
          if (const auto *result = self(self, child.get()))
            return result;
      return nullptr;
    };
    for (const auto &declaration : source.program.declarations())
      if (const auto *function = stmtAs<FunctionDecl>(declaration.get());
          function && function->body() &&
          function->name().lexeme.starts_with("effect_")) {
        const auto *effect =
            resultExpression(resultExpression, function->body().get());
        if (function->name().lexeme == "effect_alias")
          aliasEffect = effect;
        else if (effect)
          effects.push_back(effect);
      }
    if (aliasEffect && !effects.empty())
      SemanticModelTestAccess::aliasCheckedExpression(
          impl.semanticModel, *aliasEffect, *effects.front());
    for (const auto &declaration : source.program.declarations()) {
      const auto *syntax = stmtAs<FunctionDecl>(declaration.get());
      if (!syntax || (syntax->name().lexeme != "direct" &&
                      syntax->name().lexeme != "forwarded" &&
                      syntax->name().lexeme != "callback" &&
                      syntax->name().lexeme != "exclusive" &&
                      syntax->name().lexeme != "conditional"))
        continue;
      const auto *function = analyzer.model().findFunction(*syntax);
      const auto *returned =
          syntax->body() && !syntax->body()->statements().empty()
              ? stmtAs<ReturnStmt>(syntax->body()->statements().front().get())
              : nullptr;
      if (!function || !returned)
        continue;
      const auto *field = exprAs<Get>(returned->value().get());
      const auto *call = exprAs<Call>(returned->value().get());
      const bool unsupported = syntax->name().lexeme == "exclusive" ||
                               syntax->name().lexeme == "conditional";
      const ExprPtr *input = unsupported ? &returned->value()
                             : field     ? &field->object()
                                         : nullptr;
      ResolvedCallInfo boundary;
      if (call) {
        if (const auto *resolved = analyzer.model().findCall(*call)) {
          boundary = *resolved;
          const auto *member = exprAs<Get>(call->callee().get());
          input = member ? &member->object() : nullptr;
        } else if (const auto *resolved =
                       analyzer.model().findOperator(*call)) {
          boundary = {.function = resolved->function,
                      .declaration = resolved->declaration,
                      .returnType = resolved->returnType,
                      .parameterTypes = resolved->parameterTypes,
                      .lifetimeApplication = resolved->lifetimeApplication,
                      .borrowFields = resolved->borrowFields,
                      .dispatch = resolved->dispatch,
                      .dispatchOwner = resolved->dispatchOwner,
                      .receiverMode = resolved->receiverMode};
          input = &call->callee();
        }
      }
      if (!input || !*input)
        continue;
      // Reestablish the checked body's lexical bindings and entry capabilities
      // for this explicit staged operation, without a production formation
      // hook.
      impl.loanFlow = {};
      impl.scopes.emplace_back();
      for (const auto &parameter : syntax->parameters()) {
        const auto *binding = analyzer.model().findBinding(parameter);
        if (!binding)
          continue;
        auto &symbol = impl.scopes.back()[parameter.name.lexeme];
        symbol.type = binding->type;
        symbol.parameterDeclaration = &parameter;
        symbol.toolingSymbol = binding->symbol;
        symbol.retainedLoan = binding->retainedLoan;
        const auto *physical = analyzer.model().findLoan(binding->retainedLoan);
        const auto *stored =
            physical ? analyzer.model().findLoan(physical->storedDependency)
                     : nullptr;
        if (stored) {
          SemanticBorrowDependencySet contents;
          const auto append = [&](SemanticLoanDependency dependency) {
            dependency.place.projections.insert(
                dependency.place.projections.end(),
                dependency.designatedFields.begin(),
                dependency.designatedFields.end());
            dependency.parent = stored->id;
            contents.dependencies.push_back(std::move(dependency));
          };
          append({.place = stored->place,
                  .designatedFields = stored->designatedFields,
                  .access = stored->access,
                  .protectsStorage = stored->protectsStorage,
                  .conditions = stored->conditions});
          for (const auto &dependency : stored->additionalDependencies)
            append(dependency);
          symbol.completeBorrowSet = std::move(contents);
        }
      }
      for (const auto &loan : analyzer.model().loans())
        impl.loanFlow.records.emplace(loan.id,
                                      Impl::LoanFlowRecord{.id = loan.id});
      if (unsupported) {
        const auto before = analyzer.model().loans().size();
        checks.unsupportedRejected +=
            !impl.recordReferenceContents(input->get()) &&
            analyzer.model().loans().size() == before &&
            !analyzer.model().findReferenceContentsLoan(**input);
        impl.scopes.pop_back();
        continue;
      }
      const auto address = impl.completeBorrowDependencySet(*input);
      const auto reference = impl.checkedReferenceExpressionType(input->get());
      if (!address || address->dependencies.empty() || !reference) {
        impl.scopes.pop_back();
        continue;
      }
      const auto read = address->dependencies.front().parent;
      if (read && impl.loanFlow.records.contains(read)) {
        impl.loanFlow.records.at(read).active = false;
        checks.missingReadRejected +=
            !impl.recordReferenceContents(input->get()) &&
            !analyzer.model().findReferenceContentsLoan(**input);
        impl.loanFlow.records.at(read).active = true;
        const auto original = *analyzer.model().findLoan(read);
        auto cleanup = original;
        cleanup.lifecycleBound = true;
        cleanup.entry = false;
        *const_cast<SemanticLoanInfo *>(analyzer.model().findLoan(read)) =
            std::move(cleanup);
        checks.cleanupReadRejected +=
            !impl.recordReferenceContents(input->get()) &&
            !analyzer.model().findReferenceContentsLoan(**input);
        *const_cast<SemanticLoanInfo *>(analyzer.model().findLoan(read)) =
            original;
      }
      std::optional<SemanticBorrowDependencySet> result;
      std::optional<FunctionBorrowDependencySummary> callSummary;
      if (field) {
        impl.recordMemberReferenceContents(*field);
        if (const auto *selected =
                analyzer.model().findBorrowDependencySet(*field))
          result = *selected;
      } else {
        const auto *target = analyzer.model().findFunction(boundary.function);
        if (target && target->completeReturnBorrow) {
          auto summary = *target->completeReturnBorrow;
          if (!boundary.borrowFields.empty())
            summary.dependencies = boundary.borrowFields;
          callSummary = summary;
          if (impl.recordReferenceContentsInputs(summary, {}, input))
            result = impl.instantiateCompleteBorrowArguments(
                boundary, summary,
                std::span<const Impl::AnalyzedCallArgument>{}, input);
        }
      }
      const auto id = analyzer.model().findReferenceContentsLoan(**input);
      const auto *opening = analyzer.model().findLoan(id);
      checks.deltaIsolated += SemanticModelTestAccess::contentsDeltaIsolation(
          analyzer.model(), **input, id);
      checks.formed +=
          result && result->dependencies.size() == 1 && opening &&
          opening->contentsOpening &&
          opening->contentsOpening->reference == input->get() &&
          opening->contentsOpening->referenceType == *reference &&
          !opening->parent && !opening->storedDependency &&
          result->dependencies.front().referenceContents &&
          result->dependencies.front().referenceContents->openingLoan == id &&
          result->dependencies.front().parent == id;
      checks.addressPreserved +=
          impl.completeBorrowDependencySet(*input) == address;
      if (result && result->dependencies.size() == 1) {
        const auto referenceResult =
            impl.checkedReferenceContentsSource(result->dependencies.front());
        auto publication = *function;
        if (referenceResult) {
          publication.returnType = *referenceResult;
          const auto normalized = impl.normalizedCompleteBorrow(
              publication, syntax->parameters(), ReceiverMutability::ReadOnly,
              *result);
          checks.normalized +=
              normalized && normalized->dependencies.size() == 1 &&
              normalized->dependencies.front().origin ==
                  BorrowOriginKind::Argument &&
              normalized->dependencies.front().parameter == 0 &&
              normalized->dependencies.front().sourceFields.size() == 2;
        }
      }
      const auto saved = impl.checkedReferenceContentsOpening(id);
      // The effect recorder must invalidate body-local contents even when an
      // isolated semantic query has no program-effect owner. The source above
      // has already checked each concrete call/operator/construction shape.
      const auto effectOwner =
          std::exchange(impl.currentProgramEffectOwner, {});
      for (const auto *effect : effects) {
        impl.loanFlow.invalidatedReferenceContents.erase(id);
        impl.recordProgramCallEffects(*effect);
        checks.callInvalidated +=
            effects.size() == 7 && saved &&
            impl.loanFlow.invalidatedReferenceContents.contains(id) &&
            impl.loanFlow.records.at(id).active &&
            !impl.referenceContentsDependencySet(input->get()) &&
            impl.checkedReferenceContentsOpening(id) == saved;
      }
      impl.loanFlow.invalidatedReferenceContents.erase(id);
      impl.recordProgramCallEffects(**input);
      checks.noncallPreserved +=
          saved && !impl.loanFlow.invalidatedReferenceContents.contains(id) &&
          impl.referenceContentsDependencySet(input->get()) == saved;
      if (aliasEffect) {
        impl.recordProgramCallEffects(*aliasEffect);
        checks.aliasPreserved +=
            saved && !impl.loanFlow.invalidatedReferenceContents.contains(id) &&
            impl.checkedReferenceContentsOpening(id) == saved;
      }
      impl.currentProgramEffectOwner = effectOwner;
      impl.invalidateReferenceContents();
      checks.staleRejected += saved &&
                              !impl.recordReferenceContents(input->get()) &&
                              impl.checkedReferenceContentsOpening(id) == saved;
      if (callSummary)
        checks.staleCallRejected += !impl.instantiateCompleteBorrowArguments(
            boundary, *callSummary,
            std::span<const Impl::AnalyzedCallArgument>{}, input);
      if (const auto *member = exprAs<Get>(input->get())) {
        const auto before = analyzer.model().loans().size();
        const auto entry = impl.recordReferenceContents(member->object().get());
        checks.entryReused |=
            entry && !entry->dependencies.empty() &&
            analyzer.model().loans().size() == before &&
            !analyzer.model().findReferenceContentsLoan(*member->object()) &&
            !entry->dependencies.front().referenceContents;
      }
      // Rechecking the same AST starts another evaluation. It clears the
      // lookup only; the previous copied snapshot still names its own loan.
      (void)impl.analyze(**input);
      const bool cleared = !analyzer.model().findReferenceContentsLoan(**input);
      const auto refreshed = impl.recordReferenceContents(input->get());
      const auto refreshedId =
          analyzer.model().findReferenceContentsLoan(**input);
      checks.reevaluationReset +=
          cleared && refreshed && refreshedId && refreshedId != id &&
          impl.loanFlow.records.at(id).active &&
          impl.checkedReferenceContentsOpening(id) == saved;
      impl.scopes.pop_back();
    }
    return checks;
  }

  struct ContentsChecks {
    bool selectors = false;
    bool normalized = false;
    bool immutableSnapshot = false;
    bool aliasing = false;
    bool malformed = false;
    bool callOwnerRejected = false;
  };

  static ContentsChecks referenceContentsChecks(const FrontendResult &source,
                                                std::size_t selectorDepth = 1) {
    ContentsChecks checks;
    SemanticVisitor analyzer(TargetInfo::host(), &source.sourceGraph, true,
                             nullptr, &source.sources);
    if (!analyzer.check(source.program))
      return checks;
    auto &impl = *analyzer.impl;
    const FunctionDecl *syntax = nullptr;
    const Get *indirect = nullptr;
    for (const auto &declaration : source.program.declarations())
      if (const auto *function = stmtAs<FunctionDecl>(declaration.get())) {
        if (function->name().lexeme == "read")
          syntax = function;
        if (function->name().lexeme == "indirect" && function->body() &&
            !function->body()->statements().empty())
          if (const auto *returned = stmtAs<ReturnStmt>(
                  function->body()->statements().front().get()))
            indirect = exprAs<Get>(returned->value().get());
      }
    const auto *function =
        syntax ? analyzer.model().findFunction(*syntax) : nullptr;
    const auto *returned =
        syntax && syntax->body() && !syntax->body()->statements().empty()
            ? stmtAs<ReturnStmt>(syntax->body()->statements().front().get())
            : nullptr;
    const auto *sum =
        returned ? exprAs<Binary>(returned->value().get()) : nullptr;
    const auto *left = sum ? exprAs<Get>(sum->left().get()) : nullptr;
    const auto *right = sum ? exprAs<Get>(sum->right().get()) : nullptr;
    if (!function || !left || !right)
      return checks;
    const auto first = impl.referenceContentsSelector(left->object().get());
    const auto second = impl.referenceContentsSelector(right->object().get());
    checks.selectors =
        first && second && left->object().get() != right->object().get() &&
        first->root == second->root && first->receiver == second->receiver &&
        first->fields == second->fields && first->type == second->type &&
        first->fields.size() == selectorDepth &&
        first->type.kind == SemanticType::Reference;
    checks.callOwnerRejected =
        indirect && !impl.referenceContentsSelector(indirect->object().get());
    if (!checks.selectors)
      return checks;
    const auto inventory =
        impl.referenceContentsInventory(first->type.arguments.front());
    const auto address = impl.completeBorrowDependencySet(left->object());
    if (!inventory || inventory->fields.size() != 1 || !address ||
        address->dependencies.empty())
      return checks;
    std::vector<SemanticLoanId> reads;
    for (const auto &dependency : address->dependencies)
      if (dependency.parent && dependency.designatedFields.empty() &&
          dependency.conditions.empty())
        if (std::find(reads.begin(), reads.end(), dependency.parent) ==
            reads.end())
          reads.push_back(dependency.parent);
    if (reads.empty())
      return checks;
    // Supply the staged producer shape to its semantic validators without
    // enabling production formation or bypassing the independent HIR guard.
    auto &loans =
        const_cast<std::vector<SemanticLoanInfo> &>(analyzer.model().loans());
    const auto makeOpening = [&](const Expr *expression) {
      const SemanticLoanId id = loans.size() + 1;
      const auto &field = inventory->fields.front();
      loans.push_back({.id = id,
                       .kind = SemanticLoanKind::StoredValue,
                       .origin = expression,
                       .referenceContents =
                           SemanticReferenceContentsSource{id, field.fields},
                       .access = AccessMode::ReadOnly,
                       .designatedFields = field.fields,
                       .conditions = field.conditions,
                       .contentsOpening = SemanticReferenceContentsOpening{
                           expression, first->type, reads}});
      impl.loanFlow.records.emplace(
          id, SemanticVisitor::Impl::LoanFlowRecord{.id = id});
      impl.loanFlow.referenceContentsOpenings.push_back(id);
      return id;
    };
    const auto id = makeOpening(left->object().get());
    const auto other = makeOpening(right->object().get());
    const auto &field = inventory->fields.front();
    SemanticLoanDependency dependency{
        .referenceContents = SemanticReferenceContentsSource{id, field.fields},
        .access = AccessMode::ReadOnly,
        .parent = id};
    auto publication = *function;
    publication.returnType = field.reference;
    const auto summary = impl.normalizedCompleteBorrow(
        publication, syntax->parameters(), ReceiverMutability::ReadOnly,
        SemanticBorrowDependencySet{.dependencies = {dependency}});
    BorrowPath expected = first->fields;
    expected.insert(expected.end(), field.fields.begin(), field.fields.end());
    checks.normalized =
        summary && summary->dependencies.size() == 1 &&
        summary->dependencies.front().origin == BorrowOriginKind::Argument &&
        summary->dependencies.front().parameter == 0 &&
        summary->dependencies.front().sourceFields == expected;
    const auto mutableConflicts = impl.conflictingLoans(
        {}, AccessMode::Mutable, id, dependency.referenceContents);
    const auto sharedConflicts = impl.conflictingLoans(
        {}, AccessMode::ReadOnly, id, dependency.referenceContents);
    checks.aliasing =
        std::find(mutableConflicts.begin(), mutableConflicts.end(), other) !=
            mutableConflicts.end() &&
        std::find(sharedConflicts.begin(), sharedConflicts.end(), other) ==
            sharedConflicts.end();
    impl.invalidateReferenceContents();
    checks.immutableSnapshot =
        impl.loanFlow.invalidatedReferenceContents.contains(id) &&
        impl.loanFlow.invalidatedReferenceContents.contains(other) &&
        impl.loanFlow.records.at(id).active &&
        impl.checkedReferenceContentsOpening(id).has_value();
    auto bad = dependency;
    bad.place.root = first->root;
    const bool mixed = !impl.referenceContentsSourceSelector(bad);
    bad = dependency;
    bad.referenceContents->fields.clear();
    const bool wrongField = !impl.referenceContentsSourceSelector(bad);
    bad = dependency;
    bad.access = AccessMode::Mutable;
    const bool exclusive = !impl.referenceContentsSourceSelector(bad);
    const auto original = loans[id - 1];
    loans[id - 1].contentsOpening->readLoans = {id};
    const bool forgedRead = !impl.checkedReferenceContentsOpening(id);
    loans[id - 1] = original;
    loans[id - 1].lifecycleBound = true;
    const bool cleanup = !impl.checkedReferenceContentsOpening(id);
    loans[id - 1] = original;
    checks.malformed =
        mixed && wrongField && exclusive && forgedRead && cleanup;
    return checks;
  }

  static std::optional<ReferenceSupplierQuery>
  referenceSuppliers(const FrontendResult &source, std::string_view name) {
    const auto &program = source.program;
    SemanticVisitor analyzer(TargetInfo::host(), &source.sourceGraph, true,
                             nullptr, &source.sources);
    if (!analyzer.check(program))
      return std::nullopt;
    for (const auto &declaration : program.declarations())
      if (const auto *syntax = stmtAs<ClassDecl>(declaration.get());
          syntax && syntax->name().lexeme == name)
        if (const auto *owner = analyzer.model().findClassType(*syntax))
          return analyzer.impl->referenceSupplierPaths(
              analyzer.impl->openClassType(owner->id));
    return std::nullopt;
  }

  struct BorrowedFromPackChecks {
    bool laterLane = false;
    bool ambiguous = false;
    bool empty = false;
    bool nested = false;
    bool atomicReference = false;
    bool malformed = false;
    bool fixedPrefix = false;
  };

  static BorrowedFromPackChecks
  borrowedFromPackWidthChecks(const FunctionDecl &packSyntax,
                              const FunctionDecl &fixedSyntax) {
    SemanticVisitor analyzer;
    auto &impl = *analyzer.impl;
    const auto reference = SemanticType::referenceTo(
        SemanticType::Int32, AccessMode::ReadOnly, Lifetime::staticLifetime());
    const auto selected = [](const auto &contract, std::size_t index) {
      return contract.origin == BorrowOriginKind::Argument &&
             contract.parameter == index && !contract.ambiguous &&
             !contract.missing;
    };
    const auto check = [&](std::vector<SemanticType> lanes,
                           const auto &predicate) {
      const std::vector<SemanticType> grouped{
          SemanticType::Int32, SemanticType::concreteTypePack(401, lanes)};
      std::vector<SemanticType> flat{SemanticType::Int32};
      for (const auto &lane : lanes)
        SemanticType::appendExpandedArgument(flat, lane);
      return predicate(impl.genericReturnBorrowContract(packSyntax, reference,
                                                        grouped, 0, false)) &&
             predicate(impl.genericReturnBorrowContract(packSyntax, reference,
                                                        flat, 0, false));
    };
    BorrowedFromPackChecks result;
    // The old applied-flat count inspected only lane 1, losing the sole
    // reference at lane 2; two reference lanes were incorrectly unambiguous.
    result.laterLane =
        check({SemanticType::Bool, reference},
              [&](const auto &contract) { return selected(contract, 2); });
    result.ambiguous = check({reference, reference}, [](const auto &contract) {
      return contract.origin == BorrowOriginKind::None && contract.ambiguous;
    });
    result.empty = check({}, [](const auto &contract) {
      return contract.origin == BorrowOriginKind::None && contract.missing &&
             !contract.ambiguous;
    });
    result.nested = check(
        {SemanticType::Bool,
         SemanticType::concreteTypePack(402, {SemanticType::UInt8, reference})},
        [&](const auto &contract) { return selected(contract, 3); });
    const auto atomic = SemanticType::referenceTo(
        SemanticType::concreteTypePack(403, {reference, reference}),
        AccessMode::ReadOnly, Lifetime::staticLifetime());
    result.atomicReference = check(
        {atomic}, [&](const auto &contract) { return selected(contract, 1); });
    result.malformed =
        check({SemanticType::Unknown, reference}, [](const auto &contract) {
          return contract.origin == BorrowOriginKind::None && contract.missing;
        });
    const std::vector<SemanticType> grouped{
        reference, SemanticType::concreteTypePack(404, {reference, reference})};
    const std::vector<SemanticType> flat{reference, reference, reference};
    result.fixedPrefix =
        selected(impl.genericReturnBorrowContract(fixedSyntax, reference,
                                                  grouped, 0, false),
                 0) &&
        selected(impl.genericReturnBorrowContract(fixedSyntax, reference, flat,
                                                  0, false),
                 0);
    return result;
  }

  static bool groupedReturnOrdinalChecks() {
    SemanticVisitor analyzer;
    auto &impl = *analyzer.impl;
    const auto firstRegion = Lifetime::parameter(201);
    const auto secondRegion = Lifetime::parameter(202);
    const auto first = SemanticType::referenceTo(
        SemanticType::Int32, AccessMode::ReadOnly, firstRegion);
    const auto second = SemanticType::referenceTo(
        SemanticType::Int32, AccessMode::ReadOnly, secondRegion);
    const std::vector<SemanticType> grouped{SemanticType::concreteTypePack(
        203, {first, SemanticType::concreteTypePack(
                         204, {second, SemanticType::Bool})})};
    const auto original = grouped;
    const LifetimeSignatureContract lifetime{
        .parameters = {firstRegion, secondRegion}, .complete = true};
    const auto summary = impl.functionReturnBorrowSummary(
        second, grouped, 0, false, IntrinsicKind::None, std::nullopt, false,
        &lifetime, ReceiverMutability::ReadOnly);
    SemanticVisitor::Impl::ReturnBorrowContract contract{
        .origin = BorrowOriginKind::Argument, .parameter = 1};
    contract.setParameterTypes(grouped);
    if (summary.origin != BorrowOriginKind::Argument ||
        summary.parameter != 1 || !contract.originParameterType() ||
        *contract.originParameterType() != second || grouped != original)
      return false;
    contract.parameter = 3;
    if (contract.originParameterType())
      return false;
    const std::vector<SemanticType> atomic{SemanticType::referenceTo(
        grouped.front(), AccessMode::ReadOnly, firstRegion)};
    contract.parameter = 0;
    contract.setParameterTypes(atomic);
    if (contract.expandedParameterTypes || !contract.originParameterType() ||
        *contract.originParameterType() != atomic.front())
      return false;
    contract.parameter = 1;
    return !contract.originParameterType();
  }

  static bool mutableReferenceReborrowChecks() {
    SemanticVisitor analyzer;
    auto &impl = *analyzer.impl;
    const auto outer = Lifetime::parameter(101);
    const auto input = Lifetime::parameter(102);
    const auto shorter = Lifetime::parameter(103);
    const auto call = Lifetime::inference(104);
    impl.loanFlow.lifetimeContract = {.parameters = {outer, input, shorter},
                                      .bounds = {{input, shorter}},
                                      .complete = true};
    SemanticVisitor::Impl::CallableCandidate candidate;
    candidate.lifetimeApplication.complete = true;
    candidate.lifetimeApplication.obligations = {{outer, call}};
    const auto mutableReference = [](SemanticType type, Lifetime region) {
      return SemanticType::referenceTo(std::move(type), AccessMode::Mutable,
                                       region);
    };
    const auto source = mutableReference(SemanticType::Int32, outer);
    const auto target = mutableReference(SemanticType::Int32, call);
    const auto adjusted =
        impl.callLifetimeArgumentType(candidate, source, target);
    if (!adjusted || *adjusted != target ||
        impl.callLifetimeArgumentType(
            candidate, source,
            mutableReference(SemanticType::Int32, Lifetime::staticLifetime())))
      return false;
    const auto nestedSource =
        mutableReference(SemanticType::referenceTo(SemanticType::Int32,
                                                   AccessMode::ReadOnly, input),
                         outer);
    const auto nestedTarget = mutableReference(
        SemanticType::referenceTo(SemanticType::Int32, AccessMode::ReadOnly,
                                  shorter),
        call);
    if (impl.callLifetimeArgumentType(candidate, nestedSource, nestedTarget))
      return false;
    impl.loanFlow.lifetimeContract.bounds.push_back({shorter, input});
    const auto equivalent =
        impl.callLifetimeArgumentType(candidate, nestedSource, nestedTarget);
    if (!equivalent || *equivalent != nestedTarget)
      return false;
    candidate.lifetimeApplication.complete = false;
    return !impl.callLifetimeArgumentType(candidate, source, target);
  }

  static bool packProjectionChecks() {
    SemanticVisitor analyzer;
    auto &impl = *analyzer.impl;
    const auto reference = SemanticType::referenceTo(
        SemanticType::Int32, AccessMode::ReadOnly, Lifetime::staticLifetime());
    const auto pack = SemanticType::concreteTypePack(1, {reference, reference});
    SemanticBorrowDependencySet source{
        .dependencies = {{.place = {.root = 101},
                          .designatedFields = {constantIndexProjection(0)},
                          .access = AccessMode::ReadOnly,
                          .protectsStorage = true,
                          .parent = 11},
                         {.place = {.root = 202},
                          .designatedFields = {constantIndexProjection(1)},
                          .access = AccessMode::ReadOnly,
                          .protectsStorage = true,
                          .parent = 22}}};
    const BorrowPath second{constantIndexProjection(1)};
    const auto projected = impl.projectBorrowDependencySet(source, pack, second,
                                                           SemanticType::Int32);
    if (!projected || projected->dependencies.size() != 1 ||
        projected->dependencies.front().place.root != 202 ||
        projected->dependencies.front().parent != 22 ||
        !projected->dependencies.front().designatedFields.empty())
      return false;
    if (impl.projectBorrowDependencySet(source, pack,
                                        BorrowPath{constantIndexProjection(2)},
                                        SemanticType::Int32) ||
        impl.projectBorrowDependencySet(source, pack, second,
                                        SemanticType::Bool))
      return false;
    const auto nested =
        SemanticType::concreteTypePack(2, {SemanticType::Int32, pack});
    auto nestedSource = source;
    for (auto &dependency : nestedSource.dependencies)
      dependency.designatedFields.insert(dependency.designatedFields.begin(),
                                         constantIndexProjection(1));
    const auto nestedProjection = impl.projectBorrowDependencySet(
        nestedSource, nested,
        BorrowPath{constantIndexProjection(1), constantIndexProjection(1)},
        SemanticType::Int32);
    if (!nestedProjection || *nestedProjection != *projected)
      return false;
    auto ancestor = source;
    ancestor.dependencies.front().designatedFields.clear();
    if (impl.projectBorrowDependencySet(ancestor, pack, second,
                                        SemanticType::Int32))
      return false;
    impl.scopes.emplace_back();
    auto &entry = impl.scopes.back()["values"];
    const auto invocation = [&] {
      SemanticVisitor::Impl::PackBorrowInvocationScope scope(impl);
      entry.entryBorrowInvocation = impl.loanFlow.activePackBorrowInvocation;
      impl.loanFlow.packBorrowInvocationsWithEntryUses.push_back(
          entry.entryBorrowInvocation);
      return entry.entryBorrowInvocation; // Early exit must retire its
                                          // transient permission.
    }();
    if (!invocation.sequence || entry.entryBorrowInvocation.sequence ||
        impl.loanFlow.activePackBorrowInvocation.sequence)
      return false;
    SemanticVisitor::Impl::PackBorrowInvocationScope replay(impl);
    return impl.loanFlow.activePackBorrowInvocation != invocation;
  }

  struct CacheChecks {
    bool repeatedHit = false;
    bool distinctLifetimeMiss = false;
    bool distinctCallableScopeMiss = false;
    bool invalidSubstitutionRejected = false;
    bool distinctGenerationMiss = false;
    bool distinctBoundaryMiss = false;
    bool workerRefreshed = false;
    bool boundaryWorkerRefreshed = false;
  };
  static CacheChecks stagedLifetimeKeys() {
    SemanticVisitor analyzer;
    auto &impl = *analyzer.impl;
    using Cache = SemanticVisitor::Impl::StagedConstantResults;
    auto cache = std::make_shared<Cache>();
    impl.requestSession.stagedConstants = cache;
    FunctionInfo function{.id = 1};
    GenericSubstitution substitution;
    substitution.lifetimes.emplace(1, Lifetime::staticLifetime());
    const auto key = [&] {
      return impl.stagedConstantKey(static_cast<const FunctionDecl *>(nullptr),
                                    &function, {}, substitution, std::nullopt,
                                    {}, {});
    };
    const auto original = key();
    if (!original)
      return {};
    cache->record({.key = *original, .value = ConstantValue{true}});
    CacheChecks checks;
    const auto repeated = key();
    checks.repeatedHit = repeated && cache->find(*repeated);
    substitution.lifetimes[1] = Lifetime::parameter(2);
    const auto different = key();
    checks.distinctLifetimeMiss = different && !cache->find(*different);
    substitution.lifetimes[1] = Lifetime::staticLifetime();
    substitution.callableScopes.push_back(
        {.argumentPath = {0},
         .pattern = SemanticType::functionSignature(
             SemanticType::typeParameter(11), {}),
         .signature =
             SemanticType::functionSignature(SemanticType::Int32, {})});
    const auto scoped = key();
    checks.distinctCallableScopeMiss = scoped && !cache->find(*scoped);
    substitution.valid = false;
    checks.invalidSubstitutionRejected = !key();
    substitution = original->substitution;
    ++impl.requestSession.lifetimeContractRevision;
    const auto revised = key();
    checks.distinctGenerationMiss = revised && !cache->find(*revised);
    EmptyStmt earlier(Token{}), later(Token{});
    impl.requestSession.privateConstexprPrerequisites =
        std::make_shared<SemanticVisitor::Impl::RequestSessionContext::
                             PrivateConstexprPrerequisites>();
    impl.requestSession.privateConstexprBoundary = &earlier;
    const auto earlierKey = key();
    if (earlierKey)
      cache->record({.key = *earlierKey, .value = ConstantValue{true}});
    impl.requestSession.privateConstexprBoundary = &later;
    const auto laterKey = key();
    checks.distinctBoundaryMiss =
        earlierKey && laterKey && !cache->find(*laterKey);
    impl.catalog.seal();
    auto work = std::make_shared<SemanticVisitor::Impl::ConstexprWorkBudget>();
    (void)impl.stagedWorkerFor(impl.semanticModel, work);
    impl.macroLookupBase = &impl;
    ++impl.requestSession.lifetimeContractRevision;
    auto &worker = impl.stagedWorkerFor(impl.semanticModel, work);
    checks.workerRefreshed = worker.macroLookupBase == &impl &&
                             worker.requestSession.lifetimeContractRevision ==
                                 impl.requestSession.lifetimeContractRevision;
    impl.requestSession.privateConstexprBoundary = &earlier;
    auto &earlierWorker = impl.stagedWorkerFor(impl.semanticModel, work);
    checks.boundaryWorkerRefreshed =
        earlierWorker.requestSession.privateConstexprBoundary == &earlier &&
        impl.stagedWorker.privateConstexprBoundary == &earlier;
    return checks;
  }
};
} // namespace lang

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult analyze(std::string_view source) {
  return lang::Frontend().analyze("lifetime-resolution.gti",
                                  std::string(source));
}

void expectValid(const lang::FrontendResult &result, std::string_view message) {
  if (!result.semanticValid)
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << " at " << diagnostic.primary.source << ':'
                << diagnostic.primary.line << ": " << diagnostic.message
                << '\n';
  expect(result.semanticValid, message);
}

const lang::FunctionInfo *function(const lang::FrontendResult &result,
                                   std::string_view name) {
  for (const auto &declaration : result.program.declarations())
    if (const auto *syntax =
            lang::stmtAs<lang::FunctionDecl>(declaration.get());
        syntax && syntax->name().lexeme == name)
      return result.semantics.findFunction(*syntax);
  return nullptr;
}

const lang::TypeAliasInfo *alias(const lang::FrontendResult &result,
                                 std::string_view name) {
  for (const auto &declaration : result.program.declarations())
    if (const auto *syntax =
            lang::stmtAs<lang::TypeAliasDecl>(declaration.get());
        syntax && syntax->name().lexeme == name)
      return result.semantics.findTypeAlias(*syntax);
  return nullptr;
}

const lang::ClassTypeInfo *record(const lang::FrontendResult &result,
                                  std::string_view name) {
  for (const auto &declaration : result.program.declarations())
    if (const auto *syntax = lang::stmtAs<lang::ClassDecl>(declaration.get());
        syntax && syntax->name().lexeme == name)
      return result.semantics.findClassType(*syntax);
  return nullptr;
}

void testAnonymousNominalRegions() {
  const auto result = analyze(R"(
struct PairView {
  int& first; int& second;
  PairView(int& left, int& right) : first(left), second(right) {}
};
using PairAlias = PairView;
struct Nested {
  PairAlias left; PairAlias right;
  Nested(PairAlias first, PairAlias second) : left(first), right(second) {}
};
struct Derived : public PairView {
  int& third;
  Derived(int& first, int& second, int& last) : PairView(first, second), third(last) {}
};
struct Packed<Ts...> { int& anchor; Packed(int& value) : anchor(value) {} };
enum Event { first(int& value), second(int& value) };
void inspect(PairView pair, Nested nested, Derived derived, Packed<int, bool> packed, Event event);
int main() { return 0; }
)");
  expectValid(result, "record, alias, inherited, packed, and enum regions "
                      "normalize before use");
  for (const auto &[name, count] : {std::pair{"PairView", 2u},
                                    {"Nested", 4u},
                                    {"Derived", 3u},
                                    {"Packed", 1u}}) {
    const auto *info = record(result, name);
    expect(info && std::size_t(std::count_if(info->genericParameters.begin(),
                                             info->genericParameters.end(),
                                             [](const auto &parameter) {
                                               return parameter.lifetime &&
                                                      parameter.implicit;
                                             })) == count,
           "each omitted stored occurrence has an independent anonymous "
           "declaration slot");
  }
  const auto *info = function(result, "inspect");
  expect(info && info->parameterTypes.size() == 5,
         "all normalized nominal signatures remain available");
  if (info && info->parameterTypes.size() == 5) {
    const std::vector<std::size_t> counts{2, 4, 3, 1, 2};
    for (std::size_t index = 0; index < counts.size(); ++index)
      expect(info->parameterTypes[index].lifetimeArguments.size() ==
                 counts[index],
             "normalized signatures carry every anonymous nominal region");
    expect(
        info->parameterTypes[3].arguments.size() == 2,
        "anonymous lifetime slots do not consume explicit type pack arguments");
  }
  const auto *pair = record(result, "PairView");
  expect(pair && pair->fields.size() == 2 &&
             pair->fields[0].type.referenceLifetime !=
                 pair->fields[1].type.referenceLifetime &&
             pair->fields[0].type.referenceLifetime.declarationRegion() &&
             pair->fields[1].type.referenceLifetime.declarationRegion(),
         "field metadata contains declaration binders rather than inference "
         "holes");
}

void testNominalVariance() {
  const auto result = analyze(R"(
struct Shared<T, lifetime a> {
  T&<a> value; Shared(T&<a> source) : value(source) {}
};
struct Exclusive<T, lifetime a> {
  mut T&<a> value; Exclusive(mut T&<a> source) : value(source) {}
};
struct Raw<T> { T* pointer = nullptr; };
struct Owned<T> { T value; Owned(T initial) : value(initial) {} void replace(T other) mut; };
struct Unused<T, lifetime a> {};
struct Node<lifetime a> {
  Node<a>* next = nullptr; int&<a> value;
  Node(int&<a> source) : value(source) {}
};
int main() { return 0; }
)");
  expectValid(result,
              "explicit recursive types and verified nominal variance resolve");
  const auto verify = [&](std::string_view name, lang::TypeVariance type,
                          std::optional<lang::TypeVariance> lifetime) {
    const auto *info = record(result, name);
    expect(
        info && info->variance.complete &&
            info->variance.types == std::vector{type} &&
            info->variance.lifetimes ==
                (lifetime ? std::vector{*lifetime}
                          : std::vector<lang::TypeVariance>{}),
        "variance follows verified owned/shared/mutable/raw storage positions");
  };
  verify("Shared", lang::TypeVariance::Covariant,
         lang::TypeVariance::Covariant);
  verify("Exclusive", lang::TypeVariance::Invariant,
         lang::TypeVariance::Covariant);
  verify("Raw", lang::TypeVariance::Invariant, std::nullopt);
  verify("Owned", lang::TypeVariance::Covariant, std::nullopt);
  verify("Unused", lang::TypeVariance::Bivariant,
         lang::TypeVariance::Bivariant);
  if (const auto *shared = record(result, "Shared");
      shared && shared->genericParameters.size() == 2) {
    const auto expected = lang::LifetimeTypeBound{
        lang::SemanticType::typeParameter(shared->genericParameters[0].id),
        lang::Lifetime::parameter(shared->genericParameters[1].id)};
    expect(shared->lifetimeContract.valid() &&
               std::find(shared->lifetimeContract.typeBounds.begin(),
                         shared->lifetimeContract.typeBounds.end(),
                         expected) != shared->lifetimeContract.typeBounds.end(),
           "nominal metadata retains the exact referent well-formedness "
           "obligation");
  }

  for (const std::string &source :
       {std::string("struct Recursive { Recursive* next = nullptr; int& value; "
                    "Recursive(int& source) : value(source) {} }; int main() { "
                    "return 0; }"),
        std::string("using Cycle = Cycle; int main() { return 0; }")}) {
    const auto invalid = analyze(source);
    expect(!invalid.semanticValid, "recursive anonymous region expansion and "
                                   "alias cycles are rejected finitely");
  }
}

void testSpecializationLifetimeShape() {
  const auto result = analyze(R"(
namespace family {
  struct View<T, lifetime a> { T&<a> value; View(T&<a> source) : value(source) {} };
}
struct family::View<int> { int&<a> value; View(int&<a> source) : value(source) {} };
void inspect<lifetime selected>(family::View<int, selected> value);
int main() { return 0; }
)");
  expectValid(
      result,
      "exact specialization retains the primary's universal region shape");
  const auto *inspect = function(result, "inspect");
  if (inspect && inspect->parameterTypes.size() == 1 &&
      inspect->genericParameters.size() == 1) {
    const auto &type = inspect->parameterTypes.front();
    const auto *selected = result.semantics.findClassType(type.classId);
    expect(selected && selected->exactSpecializationPrimary != 0 &&
               selected->lifetimeContract.valid() &&
               type.arguments ==
                   std::vector{lang::SemanticType(lang::SemanticType::Int32)} &&
               type.lifetimeArguments ==
                   std::vector{lang::Lifetime::parameter(
                       inspect->genericParameters.front().id)},
           "selected specialization metadata preserves the actual region "
           "argument");
  } else {
    expect(false,
           "specialized lifetime-bearing parameter metadata is available");
  }

  const auto nested = analyze(R"(
struct Token<lifetime storage> {};
namespace family {
  struct Holder<T, lifetime owner> { T&<owner> value; Holder(T&<owner> source) : value(source) {} };
}
struct family::Holder<Token<_>> {};
void inspectNested<lifetime nested, lifetime selected>(
    family::Holder<Token<nested>, selected> value) requires outlives<nested, selected>;
int main() { return 0; }
)");
  expectValid(nested, "exact specialization preserves lifetimes nested in "
                      "public type arguments");
  const auto *inspectNested = function(nested, "inspectNested");
  if (inspectNested && inspectNested->parameterTypes.size() == 1 &&
      inspectNested->genericParameters.size() == 2) {
    const auto &type = inspectNested->parameterTypes.front();
    const auto *selected = nested.semantics.findClassType(type.classId);
    const auto *primary = nested.semantics.findPublicClassType(type.classId);
    const auto expected =
        lang::Lifetime::parameter(inspectNested->genericParameters[0].id);
    expect(selected && primary && selected->id != primary->id &&
               type.arguments.size() == 1 &&
               type.arguments.front().lifetimeArguments ==
                   std::vector{expected} &&
               nested.semantics.resolveExactClassSpecialization(type) == type,
           "selected identity and primary argument projection retain the "
           "actual nested region");
    expect(primary && selected && !primary->genericParameters.empty() &&
               std::any_of(selected->lifetimeContract.typeBounds.begin(),
                           selected->lifetimeContract.typeBounds.end(),
                           [&](const auto &bound) {
                             return bound.type.kind ==
                                        lang::SemanticType::TypeParameter &&
                                    bound.type.genericParameterId ==
                                        primary->genericParameters.front().id;
                           }),
           "specialized formation retains the public type obligation until "
           "application");
  } else {
    expect(false, "nested specialization lifetime metadata is available");
  }
  for (const std::string &definition :
       {std::string("struct family::View<int> { int&<a> value; int& extra; "
                    "View(int&<a> first, int& second) : value(first), "
                    "extra(second) {} };"),
        std::string("struct family::View<int, static> { int&<static> value; "
                    "View(int&<static> source) : value(source) {} };")}) {
    const auto invalid =
        analyze("namespace family { struct View<T, lifetime a> { T&<a> value; "
                "View(T&<a> source) : value(source) {} }; } " +
                definition + " int main() { return 0; }");
    expect(!invalid.semanticValid &&
               std::any_of(invalid.diagnostics.begin(),
                           invalid.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.message.find(
                                        "cannot add anonymous lifetime") !=
                                        std::string::npos ||
                                    diagnostic.message.find(
                                        "cannot select a lifetime") !=
                                        std::string::npos;
                           }),
           "specializations cannot add hidden region arity or select on a "
           "lifetime");
  }
}

void testNominalArguments() {
  const auto result = analyze(R"(
struct Marker<T, lifetime a> { T value; };
enum Message<T, lifetime a> { item(T value) };
using Label<lifetime a> = Marker<int, a>;
void inspect<lifetime a>(Label<a> first, Message<int, a> second);
int main() { return 0; }
)");
  expectValid(result, "nominal lifetime arguments and generic aliases resolve");
  const auto *info = function(result, "inspect");
  expect(info && info->genericParameters.size() == 1 &&
             info->genericParameters.front().lifetime &&
             info->parameterTypes.size() == 2,
         "lifetime binders retain a dedicated semantic kind");
  if (!info || info->genericParameters.empty() ||
      info->parameterTypes.size() != 2)
    return;
  const auto region = lang::Lifetime::parameter(info->genericParameters[0].id);
  for (const auto &type : info->parameterTypes)
    expect(type.arguments.size() == 1 &&
               type.arguments[0] == lang::SemanticType::Int32 &&
               type.lifetimeArguments == std::vector{region},
           "regions do not occupy runtime type argument slots");
}

void testTransparentAliases() {
  const auto result = analyze(R"(
using Ref<lifetime a> = int&<a>;
using Elided = int&;
void inspect<lifetime a>(Ref<a> first, Elided second, Elided third);
int main() { return 0; }
)");
  expectValid(result, "explicit and elided reference aliases are transparent");
  const auto *elided = alias(result, "Elided");
  expect(elided && elided->genericParameters.size() == 1 &&
             elided->genericParameters[0].lifetime &&
             elided->type.referenceLifetime.kind ==
                 lang::Lifetime::Kind::Parameter,
         "an elided alias stores one finite anonymous declaration region");
  const auto *info = function(result, "inspect");
  if (!info || info->parameterTypes.size() != 3) {
    expect(false, "reference alias parameter metadata is available");
    return;
  }
  const auto &parameters = info->parameterTypes;
  expect(parameters[0].kind == lang::SemanticType::Reference &&
             parameters[1].kind == lang::SemanticType::Reference &&
             parameters[2].kind == lang::SemanticType::Reference &&
             parameters[1].referenceLifetime !=
                 parameters[2].referenceLifetime &&
             parameters[1].referenceLifetime.declarationRegion() &&
             parameters[2].referenceLifetime.declarationRegion(),
         "separate alias uses receive independent normalized lifetime slots");
}

void testHigherRankedAlias() {
  const auto result = analyze(R"(
using Callback = for<lifetime a> int&<a>(int&<a>);
int main() { return 0; }
)");
  expectValid(result, "higher-ranked aliases keep their local binder scope");
  const auto *info = alias(result, "Callback");
  expect(info && info->type.kind == lang::SemanticType::FunctionSignature &&
             info->type.quantifiedLifetimes.size() == 1 &&
             info->type.arguments.size() == 2,
         "higher-ranked callable metadata retains its quantified region");
  if (info && info->type.quantifiedLifetimes.size() == 1 &&
      info->type.arguments.size() == 2) {
    const auto region =
        lang::Lifetime::parameter(info->type.quantifiedLifetimes[0]);
    expect(info->type.arguments[0].referenceLifetime == region &&
               info->type.arguments[1].referenceLifetime == region &&
               lang::freeTypeLifetimes(info->type).empty(),
           "the result and input share a scoped region that does not escape");
  }
}

void testScopedNominalCallableFormation() {
  const std::string declarations = R"(
struct Holder<T, lifetime a> {
  T&<a> value;
  Holder(T&<a> initial) : value(initial) {}
};
struct Carrier<F> {};
using Scoped<T, lifetime outer> = for<lifetime inner> void(Holder<T, inner>, int&<outer>);
void preserve<T, lifetime outer>(Carrier<Scoped<T, outer>> callback) {}
)";
  const auto result = analyze(declarations + "int main() { return 0; }");
  expectValid(
      result,
      "higher-ranked nominal premises remain inside the callable's invocation");
  const auto *preserve = function(result, "preserve");
  expect(preserve && preserve->lifetimeContract.valid() &&
             preserve->lifetimeContract.parameters.size() == 1 &&
             preserve->lifetimeContract.bounds.empty() &&
             preserve->lifetimeContract.typeBounds.empty() &&
             preserve->lifetimeContract.requirements.nodes.empty(),
         "receiving a phantom callable carrier cannot assume its nested "
         "invocation premises");
  const auto escaping = analyze(declarations + R"(
int&<static> escape<lifetime outer>(Carrier<Scoped<int, outer>> callback, int&<outer> value) {
  return value;
}
int main() { return 0; }
)");
  expect(!escaping.semanticValid && std::any_of(escaping.diagnostics.begin(),
                                                escaping.diagnostics.end(),
                                                [](const auto &diagnostic) {
                                                  return diagnostic.code ==
                                                         "GTI-S2127";
                                                }),
         "a nested callable's lifetime premises cannot justify an outer "
         "reference escape");
  const auto exactSelector = analyze(R"(
namespace exact { struct Fixed<T> {}; }
struct exact::Fixed<int&<static>> {};
using Preserved = for<lifetime a> void(exact::Fixed<int&<a>>);
int main() { return 0; }
)");
  expectValid(
      exactSelector,
      "exact specialization selection preserves a callable's actual region");
  const auto *preserved = alias(exactSelector, "Preserved");
  expect(preserved && preserved->type.quantifiedLifetimes.size() == 1 &&
             preserved->type.arguments.size() == 2 &&
             preserved->type.arguments[1].arguments.size() == 1 &&
             preserved->type.arguments[1].arguments.front().referenceLifetime ==
                 lang::Lifetime::parameter(
                     preserved->type.quantifiedLifetimes.front()),
         "an exact selector's static spelling never replaces the actual "
         "quantified region");
  const auto fixedPattern = analyze(R"(
class Fixed<T&<static>> {};
using Invalid = for<lifetime a> void(Fixed<int&<a>>);
int main() { return 0; }
)");
  expect(!fixedPattern.semanticValid &&
             std::any_of(fixedPattern.diagnostics.begin(),
                         fixedPattern.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2127";
                         }),
         "an inner invocation must satisfy fixed nominal argument-pattern "
         "regions exactly");
}

void testAssociatedLifetimeFamily() {
  const auto result = analyze(R"(
concept BorrowType<T, lifetime owner> { typename Borrow<lifetime a>; };
for<lifetime owner> concept BorrowType<int, owner> {
  using Borrow<lifetime a> = int&<a>;
};
using Result<lifetime a> = BorrowType<int, a>::Borrow<a>;
int main() { return 0; }
)");
  expectValid(result,
              "associated families bind and substitute lifetime arguments");
  const auto *info = alias(result, "Result");
  expect(info && info->genericParameters.size() == 1 &&
             info->type.kind == lang::SemanticType::Reference &&
             info->type.referenceLifetime ==
                 lang::Lifetime::parameter(info->genericParameters[0].id),
         "associated type reduction preserves the selected family region");
}

void testBinderDiagnostics() {
  for (const std::string source :
       {"void invalid(int&<missing> value); int main() { return 0; }",
        "void invalid(int&<this> value); int main() { return 0; }",
        "void invalid<lifetime a, lifetime a>(); int main() { return 0; }",
        "struct Owner<T> { using a = int; }; struct Tag<lifetime a> {}; void "
        "invalid<lifetime a>(Tag<Owner<int>::a> value); int main() { return 0; "
        "}",
        "struct Pair<T, lifetime a> {}; using Bad = Pair<static, int>; int "
        "main() { return 0; }"}) {
    const auto result = analyze(source);
    expect(!result.semanticValid &&
               std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2128";
                           }),
           "invalid lifetime names and parameter kinds produce binder "
           "diagnostics");
    expect(function(result, "main") != nullptr,
           "binder diagnostics preserve following declaration analysis");
  }
}

void testProtocolOperationBinders() {
  const auto result = analyze(R"(
concept Shared<T> {
  T&<a> borrow<lifetime a>(T&<a> value);
  T&<a> relay<lifetime a>(T&<a> value);
};
for concept Shared<int> {
  int&<b> borrow<lifetime b>(int&<b> value) { return value; }
  int& relay(int& value) { return value; }
};
T&<a> forward<T, lifetime a>(T&<a> value) requires Shared<T> {
  return Shared<T>::relay<a>(value);
}
int main() {
  int value = 7;
  int& selected = Shared<int>::borrow<_>(value);
  int& relayed = Shared<int>::relay<_>(selected);
  return forward(relayed) - 7;
}
)");
  expectValid(result, "explicit protocol lifetimes select named and elided "
                      "implementation binders");
}

void testExplicitFunctionKinds() {
  const auto result = analyze(R"(
void mixed<T, lifetime a, uint64_t N, lifetime b>(T&<a> first, int&<b> second) {}
void collect<lifetime a, T, Args...>(T&<a> first, Args&<a>... rest) {}
void exercise<lifetime a, lifetime b>(int&<a> first, int&<b> second) {
  mixed<int, a, 3, b>(first, second);
  collect<a, int, int, int>(first, first, first);
}
int main() { int value = 7; exercise(value, value); return 0; }
)");
  expectValid(
      result,
      "explicit lifetime arguments retain mixed generic order and type packs");
  const auto *exercise = function(result, "exercise");
  if (!exercise || !exercise->declaration || !exercise->declaration->body()) {
    expect(false, "mixed-kind call metadata remains available");
    return;
  }
  std::size_t checked = 0;
  for (const auto &statement : exercise->declaration->body()->statements()) {
    const auto *expression =
        lang::stmtAs<lang::ExpressionStmt>(statement.get());
    const auto *call =
        expression ? lang::exprAs<lang::Call>(expression->expression().get())
                   : nullptr;
    const auto *selected = call ? result.semantics.findCall(*call) : nullptr;
    if (!selected)
      continue;
    expect(selected->lifetimeApplication.complete &&
               !selected->lifetimeApplication.bindings.empty(),
           "selected generic calls retain a checked lifetime application");
    if (checked == 0)
      expect(selected->typeArguments.size() == 1 &&
                 selected->valueArguments.size() == 1 &&
                 selected->lifetimeArguments.size() == 2,
             "mixed kinds use separate compact runtime and lifetime vectors");
    else
      expect(selected->typeArguments.size() == 3 &&
                 selected->valueArguments.empty() &&
                 selected->lifetimeArguments.size() == 1 &&
                 selected->parameterTypes.size() == 3,
             "a leading lifetime does not count toward fixed types or pack "
             "elements");
    ++checked;
  }
  expect(checked == 2, "both explicit generic calls publish semantic metadata");

  for (const std::string source :
       {"void use<T, lifetime a>(T&<a> value) {} int main() { int x = 0; "
        "use<static, int>(x); return 0; }",
        "void use<lifetime a>(int&<a> value) {} int main() { int x = 0; "
        "use<missing>(x); return 0; }"}) {
    const auto invalid = analyze(source);
    expect(!invalid.semanticValid && std::any_of(invalid.diagnostics.begin(),
                                                 invalid.diagnostics.end(),
                                                 [](const auto &diagnostic) {
                                                   return diagnostic.code ==
                                                          "GTI-S2128";
                                                 }),
           "explicit call arguments reject wrong kinds and unknown lifetime "
           "names");
  }
}
void testExecutableLifetimeIdentity() {
  const auto parsed = analyze(R"(
struct Phantom<lifetime left, lifetime right> {};
void keep<T>(T value) {}
void nested<T>(T value) { keep(value); }
int main() { return 0; }
)");
  expectValid(parsed, "executable lifetime identity fixture resolves");
  lang::SemanticVisitor visitor;
  expect(visitor.check(parsed.program),
         "canonical instance provider accepts source declarations");
  const auto *keepSyntax =
      lang::stmtAs<lang::FunctionDecl>(parsed.program.declarations()[1].get());
  const auto *nestedSyntax =
      lang::stmtAs<lang::FunctionDecl>(parsed.program.declarations()[2].get());
  const auto *phantomSyntax =
      lang::stmtAs<lang::ClassDecl>(parsed.program.declarations()[0].get());
  if (!keepSyntax || !nestedSyntax || !phantomSyntax)
    return;
  const auto *keep = visitor.model().findFunction(*keepSyntax);
  const auto *nested = visitor.model().findFunction(*nestedSyntax);
  const auto *phantom = visitor.model().findClassType(*phantomSyntax);
  if (!keep || !nested || !phantom)
    return;
  auto same = lang::SemanticType::classType(phantom->id);
  same.lifetimeArguments = {lang::Lifetime::local(41),
                            lang::Lifetime::local(41)};
  auto different = same;
  different.lifetimeArguments.back() = lang::Lifetime::local(42);
  const auto first = visitor.canonicalizeExecutableInstance(
      {.declaration = keep->id, .types = {same}});
  const auto second = visitor.canonicalizeExecutableInstance(
      {.declaration = keep->id, .types = {different}});
  expect(first && second && first->types == second->types &&
             first->lifetimeContract == second->lifetimeContract,
         "caller region identities and incidental equality share one universal "
         "instance");
  if (!first || !second)
    return;
  expect(first->types[0].lifetimeArguments.size() == 2 &&
             first->types[0].lifetimeArguments[0] !=
                 first->types[0].lifetimeArguments[1] &&
             first->lifetimeApplication.bindings.size() == 2 &&
             first->lifetimeApplication.bindings[0].second ==
                 first->lifetimeApplication.bindings[1].second &&
             second->lifetimeApplication.bindings[0].second !=
                 second->lifetimeApplication.bindings[1].second,
         "separate universal occurrences retain per-call equal or independent "
         "choices");
  const auto nestedAnalysis =
      visitor.analyzeFunctionInstance(nested->id, {}, {}, {same}, {});
  expect(nestedAnalysis.valid(),
         "nested concrete body analysis accepts universal lifted arguments");
  const auto replay = visitor.canonicalizeExecutableInstance(
      {.declaration = keep->id, .types = {same}});
  expect(replay && replay->types == first->types,
         "nested instance analysis never rewinds or reuses another lifted "
         "identity");
  auto callback = lang::SemanticType::functionSignature(
      lang::SemanticType::referenceTo(lang::SemanticType::Int32,
                                      lang::AccessMode::ReadOnly,
                                      lang::Lifetime::parameter(8001)),
      {lang::SemanticType::referenceTo(lang::SemanticType::Int32,
                                       lang::AccessMode::ReadOnly,
                                       lang::Lifetime::parameter(8001))});
  callback.quantifiedLifetimes = {8001};
  const auto higherRanked = visitor.canonicalizeExecutableInstance(
      {.declaration = keep->id, .types = {callback}});
  callback.quantifiedLifetimes = {9001};
  callback.arguments[0].referenceLifetime = lang::Lifetime::parameter(9001);
  callback.arguments[1].referenceLifetime = lang::Lifetime::parameter(9001);
  const auto alpha = visitor.canonicalizeExecutableInstance(
      {.declaration = keep->id, .types = {callback}});
  expect(higherRanked && alpha && higherRanked->types == alpha->types &&
             higherRanked->lifetimeApplication.bindings.empty() &&
             higherRanked->types[0].arguments[0].referenceLifetime ==
                 higherRanked->types[0].arguments[1].referenceLifetime,
         "higher-ranked binder scopes and repeated uses survive "
         "alpha-canonicalization");
}

void testCallablePatternScopes() {
  const auto parsed = analyze(R"(
class ScopeCarrier<R(Args...)> {
public:
  using Signature = R(Args...);
  ScopeCarrier() = default;
  R invoke(Args... args);
};
using Selection = for<lifetime a, lifetime b> int&<a>(int&<a>, int&<b>);
ScopeCarrier<Selection> selected();
int main() { return 0; }
)");
  expectValid(parsed, "callable pattern projections retain their invocation "
                      "scope until member instantiation");
  lang::SemanticVisitor visitor(lang::TargetInfo::host(), &parsed.sourceGraph,
                                true, nullptr, &parsed.sources);
  expect(visitor.check(parsed.program),
         "scoped callable pattern provider accepts the symbolic recipe");
  const auto *ownerSyntax =
      lang::stmtAs<lang::ClassDecl>(parsed.program.declarations()[0].get());
  const auto *selectionSyntax = alias(parsed, "Selection");
  const auto *selection =
      selectionSyntax && selectionSyntax->declaration
          ? visitor.model().findTypeAlias(*selectionSyntax->declaration)
          : nullptr;
  if (!ownerSyntax || !selection)
    return;
  const lang::FunctionInfo *method = nullptr;
  for (const auto &member : ownerSyntax->members())
    if (const auto *function = lang::stmtAs<lang::FunctionDecl>(member.get());
        function && function->name().lexeme == "invoke")
      method = visitor.model().findFunction(*function);
  if (!method) {
    expect(false, "scoped pattern method metadata exists");
    return;
  }
  const auto instance = visitor.canonicalizeExecutableInstance(
      {.declaration = method->id, .classTypes = {selection->type}});
  expect(instance && instance->lifetimeContract.valid() &&
             !instance->callableScopes.empty(),
         "a concrete member receives a complete fresh invocation contract");
  if (instance) {
    std::vector<lang::SemanticType> parameters;
    for (const auto &parameter : instance->parameterTypes)
      lang::SemanticType::appendExpandedArgument(parameters, parameter);
    const auto &stored = instance->ownerType.arguments.front();
    expect(parameters.size() == 2 &&
               instance->returnType.referenceLifetime ==
                   parameters[0].referenceLifetime &&
               parameters[0].referenceLifetime !=
                   parameters[1].referenceLifetime &&
               std::find(stored.quantifiedLifetimes.begin(),
                         stored.quantifiedLifetimes.end(),
                         parameters[0].referenceLifetime.id) ==
                   stored.quantifiedLifetimes.end(),
           "member invocations preserve supplier correlation using universals "
           "distinct from the stored signature");
  }
  const auto escaped = analyze(R"(
class Escaping<R(Args...)> { public: R leaked; Escaping(R source) : leaked(source) {} };
using Selection = for<lifetime a> int&<a>(int&<a>);
int main() { Escaping<Selection> item{1}; return 0; }
)");
  expect(
      !escaped.semanticValid,
      "invocation-bound projections cannot escape as an ordinary stored field");
  const auto invalidComponent = analyze(R"(
class Reversed<R(A)> { public: using Signature = A(R); Reversed() = default; };
using Source = void(int);
int main() { Reversed<Source> item{}; return 0; }
)");
  expect(!invalidComponent.semanticValid,
         "reconstructed callable aliases still reject a substituted void "
         "parameter");
  const auto qualified = analyze(R"(
class Mutable<R(Args...) mut> {
public:
  using Signature = R(Args...) mut;
  Mutable() = default;
  R invoke(Args... args) mut;
};
class Consuming<R&<result>(Args...) &&> {
public:
  using Signature = R&<result>(Args...) &&;
  Consuming() = default;
  R&<result> invoke(Args... args) &&;
};
using MutableSelection = for<lifetime a> int&<a>(int&<a>) mut;
using ConsumingSelection = for<lifetime a> int&<a>(int&<a>) &&;
Mutable<MutableSelection> mutableCallback();
Consuming<ConsumingSelection> consumingCallback();
int main() { return 0; }
)");
  expectValid(qualified, "mutable and consuming invocation recipes remain well "
                         "formed at declaration refresh");
}

void testStructuralDefaultCallableScopes() {
  const auto result = analyze(R"(
class Captured<R(A)> { public: Captured() = default; };
struct Storage<Signature> { Storage() = default; };
class Nested<R(Args...)> {
  Storage<R(Args...)> field;
public:
  Nested() = default;
};
class Opened<R(A)> { public: Opened(A input) {} };
class Projected<R&<result>(R&<result>)> { public: Projected() = default; };
class ProjectedStorage<R&<result>(R&<result>)> {
  Storage<R&<result>(R&<result>)> field;
public:
  ProjectedStorage() = default;
};
using Selection = for<lifetime a> int&<a>(int&<a>);
int main() {
  Captured<Selection> closed{};
  Nested<Selection> nested{};
  Projected<Selection> projected{};
  ProjectedStorage<Selection> projectedStorage{};
  int value = 7;
  Opened<Selection> opened{value};
  return 0;
}
)");
  expectValid(result, "structural defaults keep owner callable scopes closed, "
                      "including nested stored signatures");
  const auto *main = function(result, "main");
  std::size_t checked = 0;
  if (main && main->declaration && main->declaration->body())
    for (const auto &statement : main->declaration->body()->statements()) {
      const auto *binding = lang::stmtAs<lang::VariableDecl>(statement.get());
      if (!binding || !binding->initializer())
        continue;
      const auto *construction =
          result.semantics.findConstruction(*binding->initializer());
      if (!construction)
        continue;
      const bool closed = binding->name().lexeme != "opened";
      expect(construction->lifetimeApplication.complete &&
                 construction->lifetimeApplication.bindings.empty() == closed &&
                 construction->lifetimeApplication.variables.empty() == closed,
             "only an actual member invocation imports nested callable binders "
             "into its lifetime application");
      ++checked;
    }
  expect(checked == 5, "all closed structural defaults and the explicit "
                       "constructor retain exact construction metadata");
  const auto [closed, rejected] =
      lang::SemanticLifetimeTestAccess::closedDefaultContractChecks(result);
  expect(closed && rejected == 11,
         "only unused captured projected formals disappear; every free "
         "contract, type and requirement use remains checked");
  for (const std::string source :
       {"class Escape<R(A)> { R field; public: Escape() = default; }; using "
        "Selection = for<lifetime a> int&<a>(int&<a>); int main() { "
        "Escape<Selection> value{}; return 0; }",
        "class Fixed<R(A)> { public: Fixed() = default; int&<static> invoke(A "
        "value) { return value; } }; using Selection = for<lifetime a> "
        "int&<a>(int&<a>); int main() { Fixed<Selection> callback{}; int value "
        "= 7; return callback.invoke(value); }",
        "class Projected<R&<result>(R&<result>)> { int&<result> value; public: "
        "Projected() = default; }; using Selection = for<lifetime a> "
        "int&<a>(int&<a>); int main() { Projected<Selection> callback{}; "
        "return 0; }"}) {
    const auto invalid = analyze(source);
    expect(invalid.syntaxValid && !invalid.semanticValid,
           "closed default construction cannot expose an invocation binder in "
           "storage or promote it to static");
  }
}

void testCallablePatternLambdaScopes() {
  const auto parsed = analyze(R"(
class Local<R&<result>(R&<result>)> {
public:
  Local() = default;
  R&<result> invoke(R&<result> value) {
    auto relay = [](R&<result> input) -> R&<result> { return input; };
    return relay(value);
  }
};
using Selection = for<lifetime a> int&<a>(int&<a>);
int main() { Local<Selection> callback{}; int value = 4; return callback.invoke(value); }
)");
  expectValid(parsed, "a lambda projected from an invocation recipe does not "
                      "capture the enclosing invocation");
  lang::SemanticVisitor visitor(lang::TargetInfo::host(), &parsed.sourceGraph,
                                true, nullptr, &parsed.sources);
  expect(visitor.check(parsed.program),
         "projected lambda declaration resolves through the ordinary semantic "
         "provider");
  const auto *owner =
      lang::stmtAs<lang::ClassDecl>(parsed.program.declarations()[0].get());
  const auto *selectionSyntax = alias(parsed, "Selection");
  const auto *selection =
      selectionSyntax && selectionSyntax->declaration
          ? visitor.model().findTypeAlias(*selectionSyntax->declaration)
          : nullptr;
  if (!owner || !selection)
    return;
  const lang::FunctionDecl *method = nullptr;
  for (const auto &member : owner->members())
    if (const auto *candidate = lang::stmtAs<lang::FunctionDecl>(member.get());
        candidate && candidate->name().lexeme == "invoke")
      method = candidate;
  const auto *info = method ? visitor.model().findFunction(*method) : nullptr;
  if (!info || !method->body())
    return;
  const auto body =
      visitor.analyzeFunctionInstance(info->id, {selection->type}, {}, {}, {});
  expect(body.valid(), "the projected lambda body proves its own input/result "
                       "lifetime relation");
  if (!body.valid())
    for (const auto &diagnostic : body.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  const auto *local =
      lang::stmtAs<lang::VariableDecl>(method->body()->statements()[0].get());
  const auto *syntax =
      local ? lang::exprAs<lang::Lambda>(local->initializer().get()) : nullptr;
  const auto *lambda = syntax ? body.model.findLambda(*syntax) : nullptr;
  expect(lambda && lambda->type.quantifiedLifetimes.size() == 1 &&
             std::none_of(lambda->environmentLifetimes.begin(),
                          lambda->environmentLifetimes.end(),
                          [&](auto region) {
                            return region.id ==
                                   lambda->type.quantifiedLifetimes.front();
                          }),
         "the lambda invocation binder is independent of its fixed environment "
         "inventory");
  const auto escaped = analyze(R"(
class Local<R&<result>(R&<result>)> {
public:
  Local() = default;
  R&<result> invoke(R&<result> value) {
    auto relay = [](R&<result> input) -> R&<result> { int local = 1; return local; };
    return relay(value);
  }
};
using Selection = for<lifetime a> int&<a>(int&<a>);
int main() { Local<Selection> callback{}; int value = 4; return callback.invoke(value); }
)");
  expect(!escaped.semanticValid, "a projected lambda cannot satisfy its "
                                 "invocation result with local storage");
  const auto unrelated = analyze(R"(
class Unrelated<R&<result>(A)> {
public:
  R&<result> invoke(A value) {
    auto relay = [](A input) -> R&<result> { return input; };
    return relay(value);
  }
};
int main() { return 0; }
)");
  expect(!unrelated.semanticValid,
         "a projected lambda cannot infer a relation between unrelated result "
         "and parameter types");
  const auto completeType = analyze(R"(
class Captured<R(A)> {
public:
  Captured() = default;
  A invoke(A value) {
    auto relay = [](A input) -> A { return input; };
    return relay(value);
  }
};
using Selection = for<lifetime a> int&<a>(int&<a>);
int main() { Captured<Selection> callback{}; int value = 4; return callback.invoke(value); }
)");
  expectValid(completeType, "an exact invocation projection imports a complete "
                            "reference parameter and result type");
  std::size_t referenceBindings = 0;
  for (const auto &instance : completeType.hir.lambdaInstances())
    for (const auto &binding : instance.body.bindings)
      if (std::find(instance.parameterBindings.begin(),
                    instance.parameterBindings.end(),
                    binding.id) != instance.parameterBindings.end()) {
        expect(binding.info.type.kind == lang::SemanticType::Reference &&
                   binding.info.traits.ownership ==
                       lang::OwnershipKind::Borrowed &&
                   binding.info.traits.drop == lang::DropKind::Trivial,
               "a whole projected reference parameter has borrowed storage and "
               "no value destructor");
        ++referenceBindings;
      }
  expect(completeType.hirValid && referenceBindings == 1,
         "the complete projected parameter reaches one concrete HIR lambda "
         "binding");
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto owned =
      lang::Frontend().analyze("projected-owned-lambda.gti", R"gti(
import std.memory;
struct Ticket { Ticket() = default; ~Ticket() {} };
class Sink<R(A)> {
public:
  Sink() = default;
  int invoke(A value) {
    auto consume = [](A input) -> int { return 7; };
    return consume(std::move(value));
  }
};
int main() { Sink<int(Ticket)> sink{}; return sink.invoke(Ticket()) - 7; }
)gti",
                               {root / "prelude.gti"}, {}, {root});
  expectValid(
      owned,
      "a projected owning parameter retains its ordinary cleanup contract");
  std::size_t ownedBindings = 0;
  for (const auto &instance : owned.hir.lambdaInstances())
    for (const auto &binding : instance.body.bindings)
      if (std::find(instance.parameterBindings.begin(),
                    instance.parameterBindings.end(),
                    binding.id) != instance.parameterBindings.end()) {
        expect(binding.info.type.kind == lang::SemanticType::Class &&
                   binding.info.traits.ownership ==
                       lang::OwnershipKind::Value &&
                   binding.info.traits.drop == lang::DropKind::Lexical,
               "normalizing a projected parameter cannot erase an actual "
               "object's destructor");
        ++ownedBindings;
      }
  expect(owned.hirValid && ownedBindings == 1,
         "an owning projection keeps one concrete cleanup-bearing lambda "
         "parameter");
}

void testInferredCarrierRegions() {
  constexpr std::string_view declarations = R"(
struct View<lifetime owner> {
  int&<owner> value;
  View(int&<owner> source) : value(source) {}
};
void consume<T>(T value) {}
void combine<T>(T first, T second) {}
)";
  const auto result = lang::Frontend({.stopAfter = lang::FrontendPhase::Hir})
                          .analyze("lifetime-instance-reuse.gti",
                                   std::string(declarations) + R"(
void exercise<lifetime left, lifetime right>(View<left> first, View<right> second) {
  consume(first); consume(second); combine(first, second);
}
int main() {
  int leftValue = 3; int rightValue = 5;
  auto first = View(leftValue); auto second = View(rightValue);
  exercise(first, second);
  return 0;
}
)");
  expectValid(result, "same-shape borrowed carriers infer common call regions");
  expect(result.hir.valid(),
         "inferred carrier calls lower with universal instance signatures");
  if (const auto *target = function(result, "consume")) {
    std::size_t instances = 0;
    for (const auto &instance : result.hir.functionInstances())
      if (instance.declaration == target->id) {
        ++instances;
        expect(
            !instance.liftedLifetimes.empty() &&
                instance.lifetimeContract.valid(),
            "native body carries checked universal lifted binder descriptors");
      }
    if (instances != 1)
      std::cerr << "consume executable instances: " << instances << '\n';
    expect(
        instances == 1,
        "different source lifetimes do not multiply native function instances");
  }
  const auto fixed = analyze(std::string(declarations) + R"(
void rejected<lifetime left, lifetime right>(View<left> first, View<right> second) {
  combine<View<left>>(first, second);
}
int main() { return 0; }
)");
  expect(
      !fixed.semanticValid &&
          std::any_of(fixed.diagnostics.begin(), fixed.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code == "GTI-S2127";
                      }),
      "explicit generic type choices retain their fixed lifetime obligations");
}

void testInstantiatedReferenceIdentity() {
  const auto result = analyze(R"(
int&<region> identity<lifetime region>(int&<region> value) { return value; }
int main() { int value = 7; return identity(value) - 7; }
)");
  expectValid(result, "a generic lifetime instance rebuilds reference "
                      "dependencies in its own body identity domain");
}

void testPrivateLifetimeInference() {
  const auto result = analyze(R"(
static int& first(int& selected, int& ignored) { return selected; }
static int& relay(int& selected, int& ignored) { return first(selected, ignored); }
static int& recurse(bool done, int& selected, int& ignored) {
  if (done) { return selected; }
  return recurse(true, selected, ignored);
}

static int& both(bool chooseFirst, int& firstValue, int& secondValue) {
  if (chooseFirst) { return firstValue; }
  return secondValue;
}
static int& mutualFirst(bool done, int& selected, int& ignored) {
  if (done) { return selected; }
  return mutualSecond(true, selected, ignored);
}
static int& mutualSecond(bool done, int& selected, int& ignored) {
  return mutualFirst(done, selected, ignored);
}
int&<a> explicitRelay<lifetime a, lifetime b>(int&<a> selected, int&<b> ignored) {
  return first(selected, ignored);
}
int&<a> explicitMutualRelay<lifetime a, lifetime b>(int&<a> selected, int&<b> ignored) {
  return mutualSecond(true, selected, ignored);
}
int main() { int firstValue = 1; int secondValue = 2; return first(firstValue, secondValue) + recurse(false, firstValue, secondValue); }
)");
  expectValid(result, "private omitted lifetime contracts infer selected and "
                      "recursive suppliers");
  for (const auto name :
       {"first", "relay", "recurse", "mutualFirst", "mutualSecond"}) {
    const auto *info = function(result, name);
    if (!info) {
      expect(false, "private helper metadata is available");
      continue;
    }
    const auto selected =
        std::string_view(name) == "first" || std::string_view(name) == "relay"
            ? 0U
            : 1U;
    const auto ignored = selected + 1;
    lang::LifetimeBounds bounds;
    for (const auto edge : info->lifetimeContract.bounds)
      (void)bounds.assume(edge.longer, edge.shorter);
    const bool validSummary =
        info->lifetimeContract.origin ==
            lang::LifetimeContractOrigin::BodyInferred &&
        info->lifetimeContract.valid() &&
        info->parameterTypes.size() > ignored &&
        bounds.proves(info->parameterTypes[selected].referenceLifetime,
                      info->returnType.referenceLifetime) &&
        !bounds.proves(info->parameterTypes[ignored].referenceLifetime,
                       info->returnType.referenceLifetime) &&
        info->completeReturnBorrow &&
        info->completeReturnBorrow->dependencies.size() == 1 &&
        info->completeReturnBorrow->dependencies.front().origin ==
            lang::BorrowOriginKind::Argument &&
        info->completeReturnBorrow->dependencies.front().parameter == selected;
    expect(validSummary, std::string("private recursive fixed point preserves "
                                     "only productive return suppliers: ") +
                             name);
    if (!validSummary) {
      const auto region = [](lang::Lifetime value) {
        std::cerr << unsigned(value.kind) << ':' << value.id;
      };
      const auto edges = [&](std::span<const lang::LifetimeBound> values) {
        for (std::size_t index = 0;
             index < std::min<std::size_t>(values.size(), 16); ++index) {
          std::cerr << ' ';
          region(values[index].longer);
          std::cerr << '>';
          region(values[index].shorter);
        }
        std::cerr << " (" << values.size() << ")\n";
      };
      std::cerr << name << " result=";
      region(info->returnType.referenceLifetime);
      std::cerr << " contract:";
      edges(info->lifetimeContract.bounds);
      std::cerr << name << " binders:";
      for (const auto binder : info->lifetimeContract.parameters) {
        std::cerr << ' ';
        region(binder);
      }
      std::cerr << '\n';
      if (info->declaration && info->declaration->body())
        if (const auto *body =
                result.semantics.findLifetimeBody(*info->declaration->body())) {
          std::cerr << name << " body complete=" << body->complete
                    << " obligations:";
          edges(body->obligations);
          std::cerr << name << " type obligations:";
          for (std::size_t index = 0;
               index < std::min<std::size_t>(body->typeObligations.size(), 16);
               ++index) {
            const auto &bound = body->typeObligations[index];
            std::cerr << " kind=" << unsigned(bound.type.kind) << ':';
            region(bound.region);
            std::cerr << " free=";
            for (const auto free : lang::freeTypeLifetimes(bound.type)) {
              region(free);
              std::cerr << ',';
            }
          }
          std::cerr << '\n';
          for (const auto &statement :
               info->declaration->body()->statements()) {
            const auto *returned =
                lang::stmtAs<lang::ReturnStmt>(statement.get());
            const auto *call =
                returned ? lang::exprAs<lang::Call>(returned->value().get())
                         : nullptr;
            if (!call)
              continue;
            const auto *resolved = result.semantics.findCall(*call);
            std::cerr << name
                      << " return call=" << (resolved ? resolved->function : 0)
                      << " region=";
            if (resolved)
              region(resolved->returnType.referenceLifetime);
            if (const auto *complete =
                    result.semantics.findBorrowDependencySet(*call)) {
              std::cerr << " roots=";
              for (const auto &dependency : complete->dependencies)
                std::cerr << dependency.place.root << '/' << dependency.parent
                          << ',';
            }
            std::cerr << " parameter roots=";
            for (const auto &parameter : info->declaration->parameters())
              if (const auto *binding = result.semantics.findBinding(parameter))
                std::cerr << binding->symbol << ',';
            std::cerr << '\n';
          }
        }
    }
  }
  const auto *both = function(result, "both");
  lang::LifetimeBounds bothBounds;
  if (both)
    for (const auto edge : both->lifetimeContract.bounds)
      (void)bothBounds.assume(edge.longer, edge.shorter);
  expect(both && both->parameterTypes.size() == 3 &&
             bothBounds.proves(both->parameterTypes[1].referenceLifetime,
                               both->returnType.referenceLifetime) &&
             bothBounds.proves(both->parameterTypes[2].referenceLifetime,
                               both->returnType.referenceLifetime) &&
             both->completeReturnBorrow &&
             both->completeReturnBorrow->dependencies.size() == 2,
         "distinct return paths infer every permitted supplier");
  for (const std::string source :
       {"int& publicBoundary(int& first, int& second) { return first; } int "
        "main() { return 0; }",
        "static int&<a> explicitBoundary<lifetime a, lifetime b>(int&<a> "
        "first, int&<b> second) { return second; } int main() { return 0; }",
        "static int& dangling(int& first, int& second) { int local = 0; return "
        "local; } int main() { return 0; }",
        "static int& both(bool chooseFirst, int& first, int& second) { if "
        "(chooseFirst) { return first; } return second; } int&<a> "
        "escape<lifetime a, lifetime b>(int&<a> first, int&<b> second) { "
        "return both(false, first, second); } int main() { return 0; }",
        "static int& unproductive(int& first, int& second) { return "
        "unproductive(first, second); } int main() { return 0; }"}) {
    const auto invalid = analyze(source);
    expect(!invalid.semanticValid,
           "private inference cannot change a public/explicit contract or "
           "export a local region");
  }
}

void testPrivateLifetimeWorklist() {
  const auto cacheKeys = lang::SemanticLifetimeTestAccess::stagedLifetimeKeys();
  expect(cacheKeys.repeatedHit,
         "an unchanged staging request can reuse its result");
  expect(cacheKeys.distinctLifetimeMiss,
         "staged results distinguish explicit lifetime substitutions");
  expect(cacheKeys.distinctCallableScopeMiss &&
             cacheKeys.invalidSubstitutionRejected,
         "staged results preserve callable scopes and reject invalid "
         "selected-family evidence");
  expect(cacheKeys.distinctGenerationMiss,
         "an inferred contract revision invalidates the previous staging "
         "environment");
  expect(cacheKeys.distinctBoundaryMiss,
         "early staged results preserve the original required-constant source "
         "boundary");
  expect(cacheKeys.workerRefreshed, "a changed lifetime generation rebuilds "
                                    "the staged worker's semantic snapshot");
  expect(
      cacheKeys.boundaryWorkerRefreshed,
      "a staged query worker retains the exact source availability boundary");
  constexpr std::size_t chainLength = 16;
  constexpr std::size_t independentCount = 16;
  const auto checkOrder = [&](bool leafFirst) {
    std::string source;
    for (std::size_t order = 0; order < chainLength; ++order) {
      const auto index = leafFirst ? chainLength - order - 1 : order;
      source += "static int& chain" + std::to_string(index) +
                "(int& selected, int& ignored) { return ";
      source += index + 1 == chainLength ? "selected; }\n"
                                         : "chain" + std::to_string(index + 1) +
                                               "(selected, ignored); }\n";
    }
    for (std::size_t index = 0; index < independentCount; ++index)
      source += "static int& independent" + std::to_string(index) +
                "(int& selected, int& ignored) { return selected; }\n";
    source += R"(
int&<a> relay<lifetime a, lifetime b>(int&<a> selected, int&<b> ignored) {
  return chain0(selected, ignored);
}
int main() { int first = 3; int second = 5; return relay(first, second); }
)";
    lang::FrontendWorkMetrics metrics;
    const auto result =
        lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics,
                        .workMetrics = &metrics})
            .analyze("private-lifetime-worklist.gti", source);
    expectValid(
        result,
        "private contract propagation is independent of declaration order");
    for (std::size_t index = 0; index < chainLength; ++index) {
      const auto *info = function(result, "chain" + std::to_string(index));
      expect(info && info->completeReturnBorrow &&
                 info->completeReturnBorrow->dependencies.size() == 1 &&
                 info->completeReturnBorrow->dependencies.front().origin ==
                     lang::BorrowOriginKind::Argument &&
                 info->completeReturnBorrow->dependencies.front().parameter ==
                     0,
             "every relay retains the same exact productive supplier");
    }
    const auto maximumProbes = 4 * (chainLength + independentCount);
    if (metrics.semanticPrivateLifetimeProbes > maximumProbes)
      std::cerr << "Private lifetime work: candidates="
                << metrics.semanticPrivateLifetimeCandidates
                << " probes=" << metrics.semanticPrivateLifetimeProbes
                << " updates=" << metrics.semanticPrivateLifetimeUpdates
                << " edges=" << metrics.semanticPrivateLifetimeDependencyEdges
                << " opaque=" << metrics.semanticPrivateLifetimeOpaqueProbes
                << '\n';
    expect(metrics.semanticPrivateLifetimeCandidates ==
                   chainLength + independentCount &&
               metrics.semanticPrivateLifetimeProbes <= maximumProbes,
           "a relay update revisits its dependents without rechecking "
           "unrelated private bodies");
  };
  checkOrder(false);
  checkOrder(true);

  const std::string precondition = R"(
void requireStatic(int&<static> value);
static void relay(int& value) { leaf(value); }
static void leaf(int& value) { requireStatic(value); }
)";
  const auto accepted = analyze(precondition + R"(
void valid(int&<static> value) { relay(value); }
int main() { return 0; }
)");
  expectValid(
      accepted,
      "private inference also propagates a void helper's input preconditions");
  const auto *relay = function(accepted, "relay");
  expect(relay && !relay->parameterTypes.empty() &&
             std::find(relay->lifetimeContract.bounds.begin(),
                       relay->lifetimeContract.bounds.end(),
                       lang::LifetimeBound{
                           relay->parameterTypes.front().referenceLifetime,
                           lang::Lifetime::staticLifetime()}) !=
                 relay->lifetimeContract.bounds.end(),
         "a previously unconstrained caller is revisited when its callee "
         "requires static storage");
  const auto rejected = analyze(precondition + R"(
void invalid(int& value) { relay(value); }
int main() { return 0; }
)");
  expect(!rejected.semanticValid && std::any_of(rejected.diagnostics.begin(),
                                                rejected.diagnostics.end(),
                                                [](const auto &diagnostic) {
                                                  return diagnostic.code ==
                                                         "GTI-S2127";
                                                }),
         "a strengthened private precondition rejects a caller without the "
         "required relationship");

  const std::string cached = R"(
static constexpr bool leaf() { return true; }
static constexpr bool enabled() { return leaf(); }
static int& choose(int& selected, int& ignored) {
  if constexpr (enabled()) {
    if constexpr (enabled()) { return selected; }
    else { return ignored; }
  } else {
    return ignored;
  }
}
int&<a> invoke<lifetime a, lifetime b>(int&<a> selected, int&<b> ignored) {
  return choose(selected, ignored);
}
int main() { return 0; }
)";
  for (const bool reuse : {false, true}) {
    lang::FrontendWorkMetrics metrics;
    const auto result =
        lang::Frontend({.reuseConstantEvaluations = reuse,
                        .stopAfter = lang::FrontendPhase::Semantics,
                        .workMetrics = &metrics})
            .analyze("private-lifetime-cache.gti", cached);
    expectValid(result, "private lifetime inference agrees with fresh and "
                        "reused constant proofs");
    const auto *selected = function(result, "choose");
    expect(selected && selected->completeReturnBorrow &&
               selected->completeReturnBorrow->dependencies.size() == 1 &&
               selected->completeReturnBorrow->dependencies.front().parameter ==
                   0,
           "cached branch proofs preserve the exact inferred return supplier");
    if (reuse)
      expect(metrics.semanticPrivateLifetimeOpaqueProbes != 0,
             "an unjournaled cached branch proof retains conservative contract "
             "dependencies");
  }

  const auto construction =
      lang::Frontend().analyze("private-lifetime-constructor.gti", R"(
struct Choice {
  bool value;
  constexpr Choice(bool selected) : value(selected) {}
  constexpr bool enabled() { return this.value; }
};
static constexpr bool enabled() { Choice choice{true}; return choice.enabled(); }
static int& choose(int& selected, int& ignored) {
  if constexpr (enabled()) { return selected; }
  else { return ignored; }
}
int&<a> invoke<lifetime a, lifetime b>(int&<a> selected, int&<b> ignored) {
  return choose(selected, ignored);
}
int main() { return 0; }
)");
  expectValid(construction, "private inference can demand checked constexpr "
                            "constructor and method prerequisites");

  const auto dependent =
      lang::Frontend().analyze("private-lifetime-dependent.gti", R"(
static constexpr bool leaf() { return true; }
static constexpr bool enabled<uint64_t N>() {
  if constexpr (N == 0) { return leaf(); }
  else { return false; }
}
static int& choose(int& selected, int& ignored) {
  if constexpr (enabled<0>()) { return selected; }
  else { return ignored; }
}
int&<a> invoke<lifetime a, lifetime b>(int&<a> selected, int&<b> ignored) {
  return choose(selected, ignored);
}
int main() { return 0; }
)");
  expectValid(dependent, "a constexpr callee discovered by concrete "
                         "dependent-branch staging is checked in source order");
  const auto *dependentChoice = function(dependent, "choose");
  expect(dependentChoice && dependentChoice->completeReturnBorrow &&
             dependentChoice->completeReturnBorrow->dependencies.size() == 1 &&
             dependentChoice->completeReturnBorrow->dependencies.front()
                     .parameter == 0,
         "dependent staging preserves the inferred private supplier");

  const auto later = analyze(R"(
static int& choose(int& selected, int& ignored) {
  if constexpr (enabled()) { return selected; }
  else { return ignored; }
}
static constexpr bool enabled() { return true; }
int main() { return 0; }
)");
  expect(!later.semanticValid, "early private inference does not make a later "
                               "constexpr definition source-visible");
  const auto invalid = analyze(R"(
static constexpr bool enabled() { return 1; }
static int& choose(int& selected, int& ignored) {
  if constexpr (enabled()) { return selected; }
  else { return ignored; }
}
int main() { return 0; }
)");
  expect(!invalid.semanticValid, "a failed constexpr prerequisite cannot "
                                 "authorize an inferred branch contract");
  const auto cycle = analyze(R"(
static constexpr bool enabled() {
  if constexpr (enabled()) { return true; }
  else { return false; }
}
static int& choose(int& selected, int& ignored) {
  if constexpr (enabled()) { return selected; }
  else { return ignored; }
}
int main() { return 0; }
)");
  expect(!cycle.semanticValid,
         "a recursive constant prerequisite remains unavailable instead of "
         "publishing a provisional proof");
}

void testProjectedPatternLifetimes() {
  const auto result = analyze(R"(
enum Outcome<T> { ready(T value), failed(int code) };
class Result<T&> {
  Outcome<T&> state;
public:
  Result(int error) : state(Outcome<T&>::failed(error)) {}
  Result(T& value) : state(Outcome<T&>::ready(value)) {}
};
void inspect<lifetime a>(Result<int&<a>> value);
int main() { return 0; }
)");
  expectValid(result, "profile regions are projected from public arguments "
                      "even in a failure-only constructor");
  const auto *owner = record(result, "Result");
  expect(
      owner && owner->genericArgumentPatternLifetimes.size() == 1 &&
          owner->genericArgumentPatternLifetimes.front().lifetime &&
          owner->genericArgumentPatternLifetimes.front().implicit &&
          owner->genericArgumentPatterns.size() == 1 &&
          owner->genericArgumentPatterns.front().referenceLifetime ==
              lang::Lifetime::parameter(
                  owner->genericArgumentPatternLifetimes.front().id),
      "a normalized argument pattern owns a separate projected region binder");
  if (owner && !owner->genericArgumentPatternLifetimes.empty()) {
    const auto projected = lang::Lifetime::parameter(
        owner->genericArgumentPatternLifetimes.front().id);
    expect(owner->lifetimeContract.valid() &&
               std::find(owner->lifetimeContract.parameters.begin(),
                         owner->lifetimeContract.parameters.end(), projected) !=
                   owner->lifetimeContract.parameters.end() &&
               std::none_of(owner->genericParameters.begin(),
                            owner->genericParameters.end(),
                            [&](const auto &parameter) {
                              return parameter.id == projected.id;
                            }),
           "projected binders participate in formation without changing public "
           "generic arity");
    const auto *inspect = function(result, "inspect");
    expect(inspect && inspect->parameterTypes.size() == 1 &&
               inspect->parameterTypes.front().arguments.size() == 1 &&
               inspect->parameterTypes.front().arguments.front().kind ==
                   lang::SemanticType::Reference &&
               inspect->parameterTypes.front()
                       .arguments.front()
                       .referenceLifetime != projected,
           "public profile arguments retain their actual region independently "
           "of the declaration recipe");
  }
  const auto fixedMismatch = analyze(R"(
class Fixed<T&<static>> { int value = 0; public: Fixed() {} };
void invalid<lifetime a>(Fixed<int&<a>> value);
int main() { return 0; }
)");
  expect(!fixedMismatch.semanticValid &&
             std::any_of(fixedMismatch.diagnostics.begin(),
                         fixedMismatch.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2127";
                         }),
         "a structural profile match cannot replace a fixed static lifetime "
         "with a caller region");
}

void testEnumInitializerLifetimeComposition() {
  const auto result = analyze(R"(
enum Choice { first(int& value), second(int& value), missing(int code) };
enum Wrapped<T> { present(T value), missing(int code) };
struct Selected {
  Choice state;
  Selected(int& value) : state(Choice::first(value)) {}
};
struct Nested {
  Wrapped<Choice> state;
  Nested(int& value) : state(Wrapped<Choice>::present(Choice::second(value))) {}
  Nested(int code) : state(Wrapped<Choice>::missing(code)) {}
};
struct Copied {
  Choice state;
  Copied(Choice value) : state(value) {}
};
struct ThroughConstructor {
  Copied state;
  ThroughConstructor(int& value) : state(Copied(Choice::second(value))) {}
  ThroughConstructor(int code) : state(Copied(Choice::missing(code))) {}
};
enum Named<lifetime a> { present(int&<a> value), missing(int code) };
struct Explicit<lifetime a> {
  Named<a> state;
  Explicit(int&<a> value) : state(Named<a>::present(value)) {}
};
int main() { return 0; }
)");
  expectValid(result, "explicit enum initializer trees compose active payload "
                      "dependencies and exact nested guards");
  for (const auto name : {"Selected", "Nested", "ThroughConstructor"}) {
    const auto *owner = record(result, name);
    const auto *lifecycle =
        owner && owner->declaration
            ? result.semantics.findClassLifecycle(*owner->declaration)
            : nullptr;
    bool connected = false;
    if (lifecycle)
      for (const auto &constructor : lifecycle->constructors) {
        if (constructor.parameterTypes.size() != 1 ||
            constructor.parameterTypes.front().kind !=
                lang::SemanticType::Reference)
          continue;
        const auto input = constructor.parameterTypes.front().referenceLifetime;
        connected |= std::any_of(
            constructor.lifetimeContract.bounds.begin(),
            constructor.lifetimeContract.bounds.end(), [&](const auto &bound) {
              return bound.longer == input &&
                     std::any_of(
                         owner->genericParameters.begin(),
                         owner->genericParameters.end(),
                         [&](const auto &parameter) {
                           return parameter.lifetime && parameter.implicit &&
                                  bound.shorter ==
                                      lang::Lifetime::parameter(parameter.id);
                         });
            });
      }
    if (!connected) {
      std::cerr << "Enum initializer " << name << " owner slots:";
      if (owner)
        for (const auto &parameter : owner->genericParameters)
          if (parameter.lifetime)
            std::cerr << ' ' << parameter.id
                      << (parameter.implicit ? "i" : "e");
      std::cerr << '\n';
      if (lifecycle)
        for (const auto &constructor : lifecycle->constructors) {
          std::cerr << "  ctor " << constructor.id << " inputs:";
          for (const auto &type : constructor.parameterTypes)
            for (const auto region : lang::freeTypeLifetimes(type))
              std::cerr << ' ' << static_cast<unsigned>(region.kind) << ':'
                        << region.id;
          std::cerr << " bounds:";
          for (std::size_t index = 0;
               index < std::min<std::size_t>(
                           8, constructor.lifetimeContract.bounds.size());
               ++index) {
            const auto bound = constructor.lifetimeContract.bounds[index];
            std::cerr << ' ' << static_cast<unsigned>(bound.longer.kind) << ':'
                      << bound.longer.id << "->"
                      << static_cast<unsigned>(bound.shorter.kind) << ':'
                      << bound.shorter.id;
          }
          std::cerr << '\n';
        }
    }
    expect(connected,
           std::string("active enum input-to-anonymous-storage relation for ") +
               name);
  }
  const auto named = analyze(R"(
enum Named<lifetime a> { present(int&<a> value), missing(int code) };
struct Fixed<lifetime a> {
  Named<a> state;
  Fixed(int& value) : state(Named<a>::present(value)) {}
};
int main() { return 0; }
)");
  expect(!named.semanticValid, "enum initializer summaries do not invent "
                               "relationships into named owner lifetimes");
  const auto wrong = analyze(R"(
enum Left { present(int& value) };
enum Right { present(int& value) };
struct Holder { Left state; Holder(int& value) : state(Right::present(value)) {} };
int main() { return 0; }
)");
  expect(!wrong.semanticValid, "matching payload spelling does not substitute "
                               "a different enum declaration");

  const auto pair = analyze(R"(
enum Reading<lifetime a> { ready(int&<a> value), missing(int code) };
struct Pair<lifetime a, lifetime b> {
  Reading<a> first;
  Reading<b> second;
  Pair(Reading<a> left, Reading<b> right) : first(left), second(right) {}
  Pair(int code) : first(Reading<a>::missing(code)), second(Reading<b>::missing(code)) {}
};
void repeat<lifetime a>(Reading<a> source) {
  Pair<a, a> pair{source, source};
}
int main() { Pair<static, static> empty{0}; return 0; }
)");
  expectValid(
      pair,
      "independent conditional fields preserve complete constructor domains");
  const auto *pairOwner = record(pair, "Pair");
  const auto *pairLifecycle =
      pairOwner && pairOwner->declaration
          ? pair.semantics.findClassLifecycle(*pairOwner->declaration)
          : nullptr;
  bool completePair = false;
  bool completeEmpty = false;
  if (pairLifecycle)
    for (const auto &constructor : pairLifecycle->constructors) {
      if (!constructor.completeBorrow)
        continue;
      const auto &summary = *constructor.completeBorrow;
      if (constructor.parameterTypes.size() == 2)
        completePair |= summary.dependencies.size() == 2 &&
                        summary.possibleAlternatives.size() == 4 &&
                        summary.dependencies[0].resultFields.size() == 2 &&
                        summary.dependencies[1].resultFields.size() == 2 &&
                        summary.dependencies[0].resultFields.front() !=
                            summary.dependencies[1].resultFields.front();
      if (constructor.parameterTypes.size() == 1)
        completeEmpty |= summary.dependencies.empty() &&
                         summary.possibleAlternatives.size() == 2;
    }
  expect(
      completePair,
      "constructor publication retains both independent guarded field paths");
  expect(
      completeEmpty,
      "two selected empty fields publish two domains and no borrowed origin");

  const std::string exclusiveDeclaration = R"(
enum Exclusive<lifetime a> { ready(mut int&<a> value), missing(int code) };
struct Duplicated<lifetime a> {
  Exclusive<a> first;
  Exclusive<a> second;
  Duplicated(mut int&<a> value)
      : first(Exclusive<a>::ready(value)), second(Exclusive<a>::ready(value)) {}
};
)";
  for (const std::string main :
       {"int main() { return 0; }",
        "int main() { int value = 0; Duplicated<_> item{value}; return 0; }"}) {
    const auto exclusive = analyze(exclusiveDeclaration + main);
    expect(
        !exclusive.semanticValid &&
            std::any_of(
                exclusive.diagnostics.begin(), exclusive.diagnostics.end(),
                [](const auto &diagnostic) {
                  return diagnostic.code == "GTI-S2045" &&
                         diagnostic.message.find("duplicate an exclusive") !=
                             std::string::npos;
                }),
        "independent conditional fields cannot duplicate one exclusive source");
  }
}

void testProjectedOnlyPatternLifetimes() {
  const std::string declaration = R"gti(
struct Fixed<int&<source>> {
  using Borrow = int&<source>;
  int&<source> value;
  Fixed(int&<source> input) : value(input) {}
  int&<source> get() { return this.value; }
};
)gti";
  const auto result = analyze(declaration + R"gti(
using Alias<lifetime a> = Fixed<int&<a>>;
int&<a> read<lifetime a>(int&<a> input) {
  Alias<a> holder{input};
  return holder.get();
}
int main() { int value = 7; return read(value) - 7; }
)gti");
  expectValid(
      result,
      "a projected lifetime alone requires concrete class instantiation");
  const auto *owner = record(result, "Fixed");
  expect(owner && owner->hasGenericBindings() &&
             owner->genericParameters.empty() &&
             owner->genericArgumentPatterns.size() == 1 &&
             owner->genericArgumentPatternLifetimes.size() == 1,
         "projected-only bindings retain one type recipe and no extra public "
         "lifetime slot");
  const auto escape = analyze(declaration + R"gti(
int&<static> bad<lifetime a>(Fixed<int&<a>> input) { return input.get(); }
int main() { return 0; }
)gti");
  expect(!escape.semanticValid &&
             std::any_of(escape.diagnostics.begin(), escape.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2127";
                         }),
         "a projected-only owner cannot widen its stored borrow to static");
  for (const auto source : {"using Bad = Fixed<bool&<static>>;",
                            "using Bad = Fixed<mut int&<static>>;",
                            "using Bad = Fixed<int&<static>, static>;"}) {
    const auto invalid =
        analyze(declaration + source + " int main() { return 0; }");
    expect(invalid.syntaxValid && !invalid.semanticValid,
           "projected-only patterns preserve fixed referent/access and public "
           "arity");
  }
}

void testNamedProjectedPatternLifetimes() {
  const std::string declarations = R"(
class Borrowed<T&<source>> {
  T&<source> stored;
public:
  using Reference = T&<source>;
  Borrowed(T&<source> value) : stored(value) {}
  T&<source> get() { return this.stored; }
};
int&<a> extract<lifetime a>(Borrowed<int&<a>> value) { return value.get(); }
)";
  const auto result = analyze(declarations + R"(
int main() { int value = 7; Borrowed<int&> view{value}; return extract(view); }
)");
  expectValid(result, "a named projected region preserves the exact reference "
                      "shell through fields and methods");
  const auto *owner = record(result, "Borrowed");
  expect(owner && owner->genericParameters.size() == 1 &&
             !owner->genericParameters.front().lifetime &&
             owner->genericArgumentPatternLifetimes.size() == 1 &&
             owner->genericArgumentPatternLifetimes.front().lifetime &&
             !owner->genericArgumentPatternLifetimes.front().implicit &&
             owner->genericArgumentPatternLifetimes.front().name.lexeme ==
                 "source",
         "named projected binders do not become separately supplied lifetime "
         "arguments");
  if (owner && owner->genericArgumentPatternLifetimes.size() == 1)
    expect(owner->fields.size() == 1 &&
               owner->fields.front().type.referenceLifetime ==
                   lang::Lifetime::parameter(
                       owner->genericArgumentPatternLifetimes.front().id),
           "an explicitly related stored reference uses the projected binder");
  const auto escaping = analyze(declarations + R"(
int&<static> invalid<lifetime a>(Borrowed<int&<a>> value) { return value.get(); }
int main() { return 0; }
)");
  expect(
      !escaping.semanticValid &&
          std::any_of(escaping.diagnostics.begin(), escaping.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code == "GTI-S2127";
                      }),
      "a named projected lifetime does not widen the selected external region");
  const auto kindConflict =
      analyze("class Invalid<T&<T>> {}; int main() { return 0; }");
  expect(!kindConflict.semanticValid &&
             std::any_of(kindConflict.diagnostics.begin(),
                         kindConflict.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2128";
                         }),
         "a projected lifetime name cannot also bind a type");
  for (const char *source :
       {"class Duplicate<T&<a>, U&<a>> {}; class Duplicate<T&<a>, U&<b>> {}; "
        "int main() { return 0; }",
        "class Duplicate<R&<a>(T&<a>)> {}; class Duplicate<R&<b>(T&<c>)> {}; "
        "int main() { return 0; }"}) {
    const auto duplicate = analyze(source);
    expect(!duplicate.semanticValid &&
               std::any_of(duplicate.diagnostics.begin(),
                           duplicate.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2006";
                           }),
           "declaration families cannot dispatch on projected lifetime "
           "relationships");
  }
}

void testNominalFormation() {
  const std::string declarations = R"(
struct View<lifetime a> { int&<a> value; View(int&<a> input) : value(input) {} };
struct Holder<T, lifetime a> { T&<a> value; Holder(T&<a> input) : value(input) {} };
struct Envelope<T, lifetime a> { Holder<T, a> value; Envelope(Holder<T, a> input) : value(input) {} };
)";
  const auto result = analyze(declarations + R"(
void inspect<lifetime a, lifetime b>(Envelope<View<b>, a> value);
int main() { return 0; }
)");
  expectValid(result, "nested nominal formation premises are normalized before "
                      "signature publication");
  const auto *inspect = function(result, "inspect");
  expect(inspect && inspect->lifetimeContract.valid() &&
             std::any_of(inspect->lifetimeContract.typeBounds.begin(),
                         inspect->lifetimeContract.typeBounds.end(),
                         [](const auto &bound) {
                           return bound.type.kind ==
                                      lang::SemanticType::Class &&
                                  !bound.type.lifetimeArguments.empty();
                         }),
         "a signature receiving nested storage retains its substituted "
         "type-outlives premise");
  const auto invalid = analyze(declarations + R"(
void invalid<lifetime a>(View<a>& input) {
  Holder<View<a>, static>* impossible = nullptr;
}
int main() { return 0; }
)");
  expect(!invalid.semanticValid &&
             std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2127";
                         }),
         "even an uninitialized pointer target must satisfy its nominal "
         "lifetime formation requirements");
}

void testNativeInvocationLifetimes() {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto prelude = std::filesystem::weakly_canonical(root / "prelude.gti");
  const std::string declarations = R"(
namespace gti_internal { class text_view<lifetime storage> {}; }
namespace std { using string_view<lifetime storage> = gti_internal::text_view<storage>; }
)";
  const auto native = [&](std::string source) {
    return lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics})
        .analyze("native-invocation-lifetimes.gti", std::move(source),
                 {prelude}, {{prelude.string(), declarations}}, {root});
  };
  const auto result = native(R"(
using Count = (std::string_view) -> int32_t;
using Pair = (std::string_view, std::string_view) -> int32_t;
int32_t length(std::string_view value) { return 0; }
int32_t named<lifetime a>(std::string_view<a> value) { return 0; }
Count selected() { return length; }
Count namedTarget() { return named; }
struct Holder { Count callback = nullptr; Holder(Count value) : callback(value) {} };
int32_t invoke<lifetime a>(Count callback, std::string_view<a> value) {
  unsafe { return callback(value); }
}
int main() { Count callback = length; unsafe { return callback("text"); } }
)");
  expectValid(result, "native callback invocation lifetimes normalize and "
                      "apply without becoming pointer storage");
  const auto *count = alias(result, "Count");
  const auto *pair = alias(result, "Pair");
  const auto *owner = record(result, "Holder");
  expect(count && count->type.kind == lang::SemanticType::NativeFunction &&
             count->type.quantifiedLifetimes.size() == 1 &&
             lang::freeTypeLifetimes(count->type).empty(),
         "one elided native parameter introduces one invocation universal");
  expect(
      pair && pair->type.quantifiedLifetimes.size() == 2 &&
          pair->type.arguments[1].lifetimeArguments !=
              pair->type.arguments[2].lifetimeArguments,
      "independent native input annotations remain distinct invocation slots");
  expect(owner && owner->genericParameters.empty(),
         "a stored native pointer does not add anonymous owner regions for its "
         "future inputs");
  for (const auto source : {
           R"(using Count = (std::string_view) -> int32_t;
int32_t fixed(std::string_view<static> value) { return 0; }
Count invalid() { return fixed; } int main() { return 0; })",
           R"(using Count = (std::string_view<static>) -> int32_t;
int32_t invalid<lifetime a>(Count callback, std::string_view<a> value) {
  unsafe { return callback(value); }
} int main() { return 0; })"}) {
    const auto rejected = native(source);
    expect(!rejected.semanticValid,
           "native target adaptation and invocation preserve fixed static "
           "storage requirements");
  }
}

void testCStringRegions() {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto prelude = std::filesystem::weakly_canonical(root / "prelude.gti");
  // This owning capability test isolates the trusted declaration mechanism.
  // The complete standard library has separate source/runtime acceptance.
  const std::string capabilityPrelude = R"(
namespace gti_internal { class c_string<lifetime storage> {}; }
using c_string<lifetime storage> = gti_internal::c_string<storage>;
)";
  const auto analyzeCString = [&](std::string source) {
    return lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics})
        .analyze("cstring-lifetimes.gti", std::move(source), {prelude},
                 {{prelude.string(), capabilityPrelude}}, {root});
  };
  const auto result = analyzeCString(R"(
c_string<region> relayCString<lifetime region>(c_string<region> value) { return value; }
void inspectCString<lifetime named>(c_string<named> explicitValue, c_string elidedValue);
c_string<static> staticCString();
int main() { return 0; }
)");
  expectValid(
      result,
      "C-string borrowed storage has named, elided, and static lifetime slots");
  const auto *relay = function(result, "relayCString");
  expect(relay && relay->returnType.kind == lang::SemanticType::CString &&
             relay->returnType.lifetimeArguments.size() == 1 &&
             relay->returnType.lifetimeArguments ==
                 relay->parameterTypes.front().lifetimeArguments,
         "C-string aliases preserve their declared input/result region "
         "relationship");
  const auto *inspect = function(result, "inspectCString");
  expect(inspect && inspect->parameterTypes.size() == 2 &&
             inspect->parameterTypes[0].lifetimeArguments.size() == 1 &&
             inspect->parameterTypes[1].lifetimeArguments.size() == 1 &&
             inspect->parameterTypes[0].lifetimeArguments !=
                 inspect->parameterTypes[1].lifetimeArguments,
         "unrelated omitted C-string input regions normalize independently");
  const auto *staticResult = function(result, "staticCString");
  expect(staticResult && staticResult->returnType.lifetimeArguments ==
                             std::vector{lang::Lifetime::staticLifetime()},
         "C-string declarations retain an explicit static storage contract");
  const auto invalid = analyzeCString(
      "void invalid(c_string<int> text); int main() { return 0; }");
  expect(!invalid.semanticValid &&
             std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2128";
                         }),
         "C-string lifetime slots reject ordinary runtime type arguments");
}

void testLambdaLifetimeInference() {
  const auto result = analyze(R"(
int&<a> relay<lifetime a, lifetime b>(int&<a> selected, int&<b> ignored) {
  auto first = [](int& left, int& right) -> int& { return left; };
  return first(selected, ignored);
}
int main() {
  auto both = [](bool chooseFirst, int& left, int& right) -> int& {
    if (chooseFirst) { return left; }
    return right;
  };
  int left = 1;
  int right = 2;
  return both(true, left, right);
}
)");
  expectValid(result,
              "local lambdas infer complete parameter lifetime suppliers");
  const auto *main = function(result, "main");
  const auto *binding =
      main && main->declaration && main->declaration->body()
          ? lang::stmtAs<lang::VariableDecl>(
                main->declaration->body()->statements().front().get())
          : nullptr;
  const auto *syntax =
      binding ? lang::exprAs<lang::Lambda>(binding->initializer().get())
              : nullptr;
  const auto *lambda = syntax ? result.semantics.findLambda(*syntax) : nullptr;
  expect(lambda && lambda->lifetimeContract.valid() &&
             lambda->lifetimeContract.origin ==
                 lang::LifetimeContractOrigin::BodyInferred &&
             lambda->completeReturnBorrow &&
             lambda->completeReturnBorrow->dependencies.size() == 2,
         "lambda metadata records both productive suppliers in the shared "
         "dependency summary");

  for (const std::string source :
       {"int&<a> invalid<lifetime a, lifetime b>(int&<a> first, int&<b> "
        "second) { auto both = [](bool choice, int& left, int& right) -> int& "
        "{ if (choice) { return left; } return right; }; return both(false, "
        "first, second); } int main() { return 0; }",
        "int main() { auto dangling = []() -> int& { int local = 0; return "
        "local; }; return 0; }"}) {
    const auto invalid = analyze(source);
    expect(!invalid.semanticValid, "lambda inference preserves every supplier "
                                   "and cannot return body-local storage");
  }
}

void testLambdaLifetimeEnvironments() {
  const auto result = analyze(R"(
int&<a> ambient<lifetime a, lifetime b>(int&<a> selected, int&<b> ignored) {
  auto fixed = [](int&<a> value) -> int&<a> { return value; };
  return fixed(selected);
}
int&<a> externalCapture<lifetime a>(int&<a> source) {
  auto get = [&source]() -> int& { return source; };
  return get();
}
void acceptStatic<F>(F& callback) requires outlives<F, static> {}
int invoke<F>(F& callback) { return callback(); }
int main() {
  auto identity = [](int& value) -> int& { return value; };
  acceptStatic(identity);
  int left = 1;
  int right = 2;
  auto choose = [&left, &right](bool first) -> int& {
    if (first) { return left; }
    return right;
  };
  auto owned = [left]() -> int& { return left; };
  auto external = [&left]() -> int& { return left; };
  return choose(true) + owned() + invoke(external);
}
)");
  expectValid(result, "closure environments preserve ambient and captured "
                      "lifetime relationships");
  const auto *ambient = function(result, "ambient");
  const auto *binding =
      ambient && ambient->declaration && ambient->declaration->body()
          ? lang::stmtAs<lang::VariableDecl>(
                ambient->declaration->body()->statements().front().get())
          : nullptr;
  const auto *syntax =
      binding ? lang::exprAs<lang::Lambda>(binding->initializer().get())
              : nullptr;
  const auto *lambda = syntax ? result.semantics.findLambda(*syntax) : nullptr;
  const auto actual =
      syntax ? result.semantics.typeOf(*syntax) : lang::SemanticType::Unknown;
  expect(lambda && lambda->type.hasLambdaShape() &&
             !lambda->environmentLifetimes.empty() &&
             lambda->type.lifetimeArguments == lambda->environmentLifetimes &&
             actual.lifetimeArguments.size() ==
                 lambda->environmentLifetimes.size() &&
             actual.lifetimeArguments != lambda->environmentLifetimes,
         "canonical lambda environment slots are distinct from each closure "
         "value's fixed choices");
  for (const std::string source :
       {"int&<b> invalid<lifetime a, lifetime b>(int&<a> first, int&<b> "
        "second) { auto fixed = [](int&<a> value) -> int&<a> { return value; "
        "}; return fixed(second); } int main() { return 0; }",
        "int&<a> invalid<lifetime a>(int&<a> source) { int local = 1; auto get "
        "= [&local]() -> int& { return local; }; return get(); } int main() { "
        "return 0; }",
        "int&<a> invalid<lifetime a>(int&<a> source) { int local = 1; auto get "
        "= [local]() -> int& { return local; }; return get(); } int main() { "
        "return 0; }",
        "void acceptStatic<F>(F& callback) requires outlives<F, static> {} int "
        "main() { int local = 1; auto get = [&local]() -> int& { return local; "
        "}; acceptStatic(get); return 0; }"}) {
    const auto invalid = analyze(source);
    expect(!invalid.semanticValid,
           "fixed closure environments reject incompatible input, local escape "
           "and static storage promises");
  }
}
void testLambdaEnvironmentPremises() {
  const auto fixed = analyze(R"(
int needsStorage<T, lifetime a>() requires outlives<T, a> { return 1; }
int enclosing<T, lifetime a>(T&<a> input, int&<a> token) {
  auto body = [](int&<a> ignored) -> int { return needsStorage<T, a>(); };
  return body(token);
}
int main() { int input = 1; return enclosing(input, input) - 1; }
)");
  expectValid(fixed, "an explicitly scoped lambda preserves checked fixed "
                     "environment type premises");
  const auto *enclosing = function(fixed, "enclosing");
  const auto *binding =
      enclosing && enclosing->declaration && enclosing->declaration->body()
          ? lang::stmtAs<lang::VariableDecl>(
                enclosing->declaration->body()->statements().front().get())
          : nullptr;
  const auto *syntax =
      binding ? lang::exprAs<lang::Lambda>(binding->initializer().get())
              : nullptr;
  const auto *lambda = syntax ? fixed.semantics.findLambda(*syntax) : nullptr;
  const bool retained = lambda && lambda->environmentContract.valid() &&
                        !lambda->environmentContract.typeBounds.empty() &&
                        lambda->environmentApplication.complete &&
                        lambda->environmentApplication.bindings.size() ==
                            lambda->environmentLifetimes.size();
  expect(
      retained,
      "lambda formation records a separate complete environment application");
  const auto either = analyze(R"(
int either<T, U, lifetime a>() requires outlives<T, a> || outlives<U, a> { return 1; }
int enclosing<T, U, lifetime a>(int&<a> token)
    requires outlives<T, a> || outlives<U, a> {
  auto body = [](int&<a> ignored) -> int { return either<T, U, a>(); };
  return body(token);
}
int main() { int input = 1; return enclosing<int, int>(input) - 1; }
)");
  expectValid(
      either,
      "fixed environment formation preserves an entire disjunctive premise");
  for (
      const std::string source : {
          R"(int needsStorage<T, lifetime a>() requires outlives<T, a> { return 1; }
int invalid<T, lifetime a>(int&<a> token) {
  auto body = [](int&<a> ignored) -> int { return needsStorage<T, a>(); };
  return body(token);
}
int main() { return 0; })",
          R"(int needsStorage<T, lifetime a>() requires outlives<T, a> { return 1; }
int invalid<T, U, lifetime a>(int&<a> token)
    requires outlives<T, a> || outlives<U, a> {
  auto body = [](int&<a> ignored) -> int { return needsStorage<T, a>(); };
  return body(token);
}
int main() { return 0; })"}) {
    const auto invalid = analyze(source);
    expect(invalid.syntaxValid && !invalid.semanticValid,
           "fixed environments neither invent a type premise nor choose one OR "
           "branch");
  }
  const auto library = std::filesystem::weakly_canonical(
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib");
  const lang::PackageSourceRoot modules{
      .identity = "toolchain:std",
      .name = "std",
      .packageRoot = library,
      .sourceRoot = library,
      .modules = {{"meta", library / "std/meta.gti"},
                  {"detail_function_pointer_impl",
                   library / "std/detail/function_pointer.gti"}},
      .kind = lang::SourceOwnerKind::Toolchain,
      .implicitAlias = "std",
      .defaultModule = "meta"};
  const auto pointerSource = [](std::string_view signature) {
    return std::string(R"(import std.detail_function_pointer_impl;
int needsStorage<T, lifetime a>() requires outlives<T, a> { return 1; }
int enclosing<T, lifetime a>(T&<a> input, int&<a> token) {
  auto pointer = gti_internal::function_pointer_from_item<)") +
           std::string(signature) + R"(>(
      [](int&<a> ignored) -> int { return needsStorage<T, a>(); });
  return gti_internal::function_pointer_invoke(pointer, token);
}
int main() { int input = 1; return enclosing(input, input) - 1; })";
  };
  const auto checkPointer = [&](std::string_view signature) {
    const auto source = pointerSource(signature);
    const auto path = library / "lifetime-fixed-environment-pointer.gti";
    return lang::Frontend().analyze(
        path, source, {library / "prelude.gti", path},
        {{path.string(), source}}, {library}, {modules});
  };
  expectValid(checkPointer("int(int&<a>)"),
              "a pointer formed from a stateless lambda retains its checked "
              "fixed environment premise");
  const auto universal = checkPointer("for<lifetime b> int(int&<b>)");
  expect(universal.syntaxValid && !universal.semanticValid,
         "erasure cannot reinterpret a fixed lambda input as a fresh "
         "invocation universal");
}
void testForwardedPackBorrowPaths() {
  expect(lang::SemanticLifetimeTestAccess::packProjectionChecks(),
         "typed pack projection preserves repeated-element identity and "
         "rejects wrong or ancestor paths");
  expect(lang::SemanticLifetimeTestAccess::groupedReturnOrdinalChecks(),
         "return suppliers use flat leaf ordinals without flattening canonical "
         "identity or reference-to-pack shells");
  const std::string declarations = R"(
struct View<lifetime owner> {
  int&<owner> value;
  View(int&<owner> source) : value(source) {}
};
int&<b> select<lifetime a, lifetime b>(int&<a> ignored, int&<b> selected) {
  return selected;
}
int&<b> select<lifetime a, lifetime b, lifetime shell>(int&<a> ignored, View<b>&<shell> selected) {
  return selected.value;
}
class Relay<R(Args...)> {
public:
  Relay() = default;
  R invoke(Args... values) { return select(values...); }
};
using ScalarChoice = for<lifetime a, lifetime b> int&<b>(int&<a>, int&<b>);
using CarrierChoice = for<lifetime a, lifetime b, lifetime shell> int&<b>(int&<a>, View<b>&<shell>);
int&<b> scalarRelay<lifetime a, lifetime b>(int&<a> first, int&<b> second) {
  Relay<ScalarChoice> relay{};
  return relay.invoke(first, second);
}
int&<b> carrierRelay<lifetime a, lifetime b, lifetime shell>(int&<a> first, View<b>&<shell> second) {
  Relay<CarrierChoice> relay{};
  return relay.invoke(first, second);
}
)";
  const auto selected = analyze(declarations + R"(
int main() {
  int first = 3; int second = 8; View view{second};
  return scalarRelay(first, second) + carrierRelay(first, view) - 16;
}
)");
  expectValid(selected, "forwarded packs distinguish scalar reference entries "
                        "from reference-to-carrier external owners");
  const auto wrongOwner = analyze(declarations + R"(
int&<a> invalid<lifetime a, lifetime b>(int&<a> first, int&<b> second) {
  return scalarRelay(first, second);
}
int main() { return 0; }
)");
  expect(!wrongOwner.semanticValid, "equal-typed pack elements cannot "
                                    "substitute the wrong lifetime supplier");
  // Forming View<b>&<shell> already proves b outlives shell. Shortening the
  // external int borrow is valid; extending the physical View shell is not.
  const auto shellShortening = analyze(declarations + R"(
int&<shell> shorten<lifetime a, lifetime b, lifetime shell>(int&<a> first, View<b>&<shell> second) {
  return carrierRelay(first, second);
}
int main() { return 0; }
)");
  expectValid(shellShortening, "a carrier's external supplier may be shortened "
                               "to its checked shell lifetime");
  const std::string shellDeclarations = R"(
struct View<lifetime b> { int&<b> value; View(int&<b> input) : value(input) {} };
View<b>&<shell> shellIdentity<lifetime b, lifetime shell>(View<b>&<shell> value) { return value; }
View<b>&<shell> through<lifetime b, lifetime shell, Args...>(Args&<shell>... values) {
  return shellIdentity<b, shell>(values...);
}
)";
  const auto shellEscape = analyze(shellDeclarations + R"(
View<b>&<b> invalid<lifetime b, lifetime shell>(View<b>&<shell> value) {
  return through<b, shell>(value);
}
int main() { return 0; }
)");
  expect(shellEscape.syntaxValid && !shellEscape.semanticValid &&
             std::any_of(shellEscape.diagnostics.begin(),
                         shellEscape.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2017";
                         }),
         "a reference-to-carrier pack element never replaces its external "
         "owner with the carrier shell");
  const auto localShell = analyze(shellDeclarations + R"(
int global = 7;
View<static>&<static> invalid() { View<static> local{global}; return through<static>(local); }
int main() { return 0; }
)");
  expect(localShell.syntaxValid && !localShell.semanticValid,
         "a local carrier shell cannot escape with the static lifetime of its "
         "referent");
  const std::string leadingCarrier = R"(
struct View<lifetime a> { int&<a> value; View(int&<a> input) : value(input) {} };
View<a>&<shell> fixedShell<lifetime a, lifetime shell, Args...>(View<a>&<shell> first, Args&... ignored) {
  return first;
}
int&<a> fixedExternal<lifetime a, lifetime shell, Args...>(View<a>&<shell> first, Args&... ignored) {
  return first.value;
}
)";
  const auto leading = analyze(leadingCarrier + R"(
int main() {
  int value = 7; bool ignored = false; View view{value};
  return fixedShell(view).value + fixedShell(view, ignored).value +
         fixedExternal(view, ignored) - 21;
}
)");
  expectValid(leading, "known leading carrier references validate shell and "
                       "external paths before concrete trailing-pack replay");
  for (const auto name : {"fixedShell", "fixedExternal"}) {
    const auto *info = function(leading, name);
    const auto *binding = info && info->declaration
                              ? leading.semantics.findBinding(
                                    info->declaration->parameters().back())
                              : nullptr;
    expect(info && info->parameterTypes.size() == 2 &&
               info->parameterTypes.back().kind ==
                   lang::SemanticType::Reference &&
               binding && binding->type.kind == lang::SemanticType::TypePack &&
               !binding->type.concretePack &&
               info->parameterTypes.back().arguments.size() == 1 &&
               binding->type.genericParameterId == info->parameterTypes.back()
                                                       .arguments.front()
                                                       .genericParameterId,
           "source reference-pack binding metadata names the same symbolic "
           "group as its body scope");
  }
  const auto leadingEscape = analyze(leadingCarrier + R"(
int global = 7;
View<static>&<static> invalid() {
  View<static> local{global}; bool ignored = false;
  return fixedShell(local, ignored);
}
int main() { return 0; }
)");
  expect(leadingEscape.syntaxValid && !leadingEscape.semanticValid,
         "known leading carrier staging cannot turn a local physical shell "
         "into its static stored owner");
  const std::string selectedShell = R"(
struct View<lifetime a> { int&<a> value; View(int&<a> input) : value(input) {} };
View<a>&<shell> choose<lifetime a, lifetime shell>(View<a>&<shell> first, View<a>&<shell> second, bool left) {
  if (left) { return first; } return second;
}
View<a>&<shell> alias<lifetime a, lifetime shell>(View<a>&<shell> first, View<a>&<shell> second, bool left) {
  View<a>&<shell> selected = choose(first, second, left); return selected;
}
)";
  const auto aliases = analyze(selectedShell + R"(
int main() { int first = 3; int second = 8; View left{first}; View right{second}; return alias(left, right, false).value - 8; }
)");
  expectValid(aliases, "a retained carrier reference preserves all physical "
                       "shell suppliers separately from stored fields");
  const auto wrongShell = analyze(selectedShell + R"(
View<a>&<left> wrong<lifetime a, lifetime left, lifetime right>(View<a>&<left> first, View<a>&<right> second, bool leftValue) {
  View<a>& selected = choose(first, second, leftValue); return selected;
}
int main() { int value = 7; View first{value}; View second{value}; return wrong(first, second, false).value; }
)");
  expect(wrongShell.syntaxValid && !wrongShell.semanticValid,
         "a multi-supplier carrier reference cannot use one shell lifetime for "
         "its independent other supplier");
  const auto conditional = analyze(R"(
enum Reading<lifetime owner> { present(int&<owner> value), missing(int code) };
Reading<a> keep<lifetime a>(Reading<a> value) { return value; }
class Relay<R(Args...)> {
public:
  Relay() = default;
  R invoke(Args... values) { return keep(values...); }
};
using ReadingCall = for<lifetime a> Reading<a>(Reading<a>);
int main() {
  Relay<ReadingCall> relay{};
  int value = 7;
  Reading first = relay.invoke(Reading::present(value));
  Reading<static> empty = relay.invoke(Reading<static>::missing(0));
  switch (first) { case Reading::present(auto& selected): { return selected - 7; } case Reading::missing(auto& code): { return 1; } }
}
)");
  expectValid(conditional, "forwarded carrier packs preserve complete guarded "
                           "and empty alternatives");
  const auto consumed = analyze(R"(
struct Owned {
  Owned() = default;
  Owned(Owned& other) = delete;
  Owned(Owned&& other) = default;
  ~Owned() {}
};
void take(Owned value) {}
void twice<Args...>(Args... values) { take(values...); take(values...); }
int main() { twice(Owned()); return 0; }
)");
  expect(
      !consumed.semanticValid &&
          std::any_of(consumed.diagnostics.begin(), consumed.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.message.find("already been moved") !=
                               std::string::npos;
                      }),
      "entry projection cannot revive a pack consumed by an earlier forwarding "
      "call");
}

void testBorrowedFromPackOrdinals() {
  const auto syntax =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Parsing})
          .analyze("borrowed-from-pack-width.gti", R"(
[[borrowed_from(values)]] R packed<R, Args...>(int tag, Args... values);
[[borrowed_from(first)]] R fixed<R, Args...>(int& first, Args... values);
)");
  expect(syntax.syntaxValid && syntax.program.declarations().size() == 2,
         "borrowed_from ordinal fixture parses independently of body proof");
  if (!syntax.syntaxValid || syntax.program.declarations().size() != 2)
    return;
  const auto *pack =
      lang::stmtAs<lang::FunctionDecl>(syntax.program.declarations()[0].get());
  const auto *fixed =
      lang::stmtAs<lang::FunctionDecl>(syntax.program.declarations()[1].get());
  if (!pack || !fixed) {
    expect(false,
           "borrowed_from ordinal fixture retains both function declarations");
    return;
  }
  const auto checks =
      lang::SemanticLifetimeTestAccess::borrowedFromPackWidthChecks(*pack,
                                                                    *fixed);
  expect(checks.laterLane, "borrowed_from reaches a later eligible pack lane "
                           "in grouped and applied-flat signatures");
  expect(checks.ambiguous, "applied-flat borrowed_from packs cannot silently "
                           "select the first of multiple suppliers");
  expect(checks.empty,
         "empty borrowed_from packs have no supplier in either signature view");
  expect(
      checks.nested,
      "nested concrete pack leaves retain their exact borrowed_from ordinal");
  expect(checks.atomicReference,
         "borrowed_from keeps a reference to a pack as one physical lane");
  expect(checks.malformed,
         "unknown pack lanes cannot prove a complete borrowed_from selection");
  expect(checks.fixedPrefix,
         "a named fixed formal is unaffected by later eligible pack lanes");

  const std::string declarations = R"(
int global = 9;
int&<a> select<lifetime a>(int ignored, int&<a> value) { return value; }
int&<a> select<lifetime a>(int&<a> ignored, int&<a> value) { return value; }
int&<static> select() { return global; }
class Relay<R(Args...)> {
public:
  Relay() = default;
  [[borrowed_from(values)]] R invoke(int tag, Args... values) { return select(values...); }
};
)";
  const auto valid = analyze(declarations + R"(
using Selected = for<lifetime a> int&<a>(int, int&<a>);
int main() { int value = 7; Relay<Selected> relay{}; return relay.invoke(0, 4, value) - 7; }
)");
  expectValid(valid, "generic borrowed_from call metadata selects the sole "
                     "later reference lane");
  for (const std::string &source :
       {"using Selected = for<lifetime a> int&<a>(int&<a>, int&<a>); int "
        "main() { int first = 3; int second = 7; Relay<Selected> relay{}; "
        "return relay.invoke(0, first, second); }",
        "using Selected = int&<static>(); int main() { Relay<Selected> "
        "relay{}; return relay.invoke(0); }"}) {
    const auto invalid = analyze(declarations + source);
    expect(invalid.syntaxValid && !invalid.semanticValid &&
               std::any_of(invalid.diagnostics.begin(),
                           invalid.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2017";
                           }),
           "borrowed_from requires exactly one eligible supplier after "
           "concrete pack expansion");
  }
}

void testScopedFunctionBorrowQuery() {
  const std::string declarations = R"(
struct Scoped<R&<environment>(Args...), lifetime environment> {
  R&<environment> value;
  Scoped(R&<environment> input) : value(input) {}
  R&<environment> get() { return this.value; }
};
int&<a> read<lifetime a>(int&<a> input) {
  Scoped<int&<a>(int&), a> owner{input};
  return owner.get();
}
)";
  const auto valid = analyze(declarations + R"(
int main() { int input = 7; return read(input) - 7; }
)");
  expectValid(valid, "function summary queries alpha-match scoped invocation "
                     "binders while retaining fixed owner regions");
  const auto invalid = analyze(declarations + R"(
int&<b> invalid<lifetime a, lifetime b>(int&<a> input) { return read<a>(input); }
int main() { return 0; }
)");
  expect(invalid.syntaxValid && !invalid.semanticValid,
         "bound-scope equivalence cannot rename an independent free result "
         "region");
}

void testReferencePackReturnContracts() {
  const std::string declarations = R"(
int&<a> second<lifetime a>(int&<a> first, int&<a> selected) { return selected; }
int&<a> relay<lifetime a, Args...>(Args&<a>... values) { return second(values...); }
int&<a> fixed<lifetime a, Args...>(int&<a> first, Args&... ignored) { return first; }
)";
  const auto valid = analyze(declarations + R"(
int main() {
  int first = 3; int selected = 8; bool ignored = false;
  return relay(first, selected) + fixed(first, ignored) - 11;
}
)");
  expectValid(valid, "reference packs preserve declared result suppliers after "
                     "exact concrete expansion");
  const auto wrongRegion = analyze(declarations + R"(
int&<a> invalid<lifetime a, lifetime b>(int&<a> first, int&<b> secondValue) {
  return relay<a>(first, secondValue);
}
int main() { return 0; }
)");
  expect(wrongRegion.syntaxValid && !wrongRegion.semanticValid,
         "a fixed reference-pack result cannot escape an independent shorter "
         "element");
  const auto localEscape = analyze(declarations + R"(
int&<static> invalid<Args...>(Args&... values) {
  int local = 4;
  return fixed(local, values...);
}
int main() { int value = 1; invalid(value); return 0; }
)");
  expect(localEscape.syntaxValid && !localEscape.semanticValid,
         "concrete reference-pack return proof cannot publish a local owner as "
         "static");
  for (const std::string &returned : {"1", "missing()"}) {
    const auto sibling =
        analyze("void consume<Args...>(Args&... values) {}\n"
                "int&<a> invalid<lifetime a, Args...>(Args&<a>... values) { "
                "consume(values...); return " +
                returned + "; } int main() { return 0; }");
    expect(sibling.syntaxValid && !sibling.semanticValid,
           "one deferred pack call cannot authorize a sibling temporary or "
           "unknown return");
  }
  const auto concreteTemporary = analyze(R"(
int&<a> choose<lifetime a>(int&<a> value) { return value; }
int choose(bool& value) { return 1; }
int&<a> relay<lifetime a, Args...>(Args&<a>... values) { return (choose(values...)); }
int main() { int first = 1; bool second = false; int one = relay(first); int two = relay(second); return one + two; }
)");
  expect(concreteTemporary.syntaxValid && !concreteTemporary.semanticValid &&
             std::any_of(concreteTemporary.diagnostics.begin(),
                         concreteTemporary.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.message.find(
                                      "addressable value") != std::string::npos;
                         }),
         "a deferred call is rechecked as an actual temporary in each concrete "
         "pack instance");
  const auto trailingOrigin = analyze(R"(
int&<a> identity<lifetime a>(int&<a> value) { return value; }
int&<a> trailing<lifetime a, lifetime b, Args...>(int&<a> first, Args&<b>... values) {
  return identity(values...);
}
int&<a> invalid<lifetime a, lifetime b>(int&<a> first, int&<b> other) {
  return trailing<a, b>(first, other);
}
int main() { int first = 1; int other = 2; return invalid(first, other); }
)");
  expect(trailingOrigin.syntaxValid && !trailingOrigin.semanticValid &&
             std::any_of(trailingOrigin.diagnostics.begin(),
                         trailingOrigin.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2127";
                         }),
         "known leading formals cannot authorize an independent trailing pack "
         "supplier");
}

void testAliasConstructionLifetimeApplications() {
  const std::string declarations = R"(
struct Slots<T> {
  int count; Slots(int input) : count(input) {}
  int size() { return this.count; }
};
class Cursor<T> {
  Slots<T>& slots;
public:
  Cursor(Slots<T>& input) : slots(input) {}
  int read() { return this.slots.size(); }
};
struct Buffer<T> {
  Slots<T> data;
  using iterator = Cursor<T>;
  using nested = iterator;
  using borrow<lifetime a> = Slots<T>&<a>;
  Buffer(int input) : data(Slots<T>(input)) {}
  nested begin() { return nested(this.data); }
  borrow<a> access<lifetime a>(Slots<T>&<a> input) { return input; }
};
struct View<lifetime a> {
  int&<a> value; View(int&<a> input) : value(input) {}
};
using Inferred = View;
using Fixed = View<static>;
int&<a> independent<lifetime a, lifetime b>(int&<a> first, int&<b> second) {
  Inferred left = Inferred(first); Inferred right = Inferred(second);
  return left.value;
}
)";
  const auto valid = analyze(declarations + R"(
int main() {
  Buffer<int> buffer{7}; auto cursor = buffer.begin();
  Slots<int> separate{4}; int borrowed = buffer.access(separate).size();
  int first = 3; int second = 4;
  return cursor.read() + independent(first, second) + borrowed - 14;
}
)");
  expectValid(valid, "alias construction instantiates anonymous regions per "
                     "use while retaining the selected generic owner");
  expect(valid.hirValid, "class alias declarations retain their own lifetime "
                         "parameters in the concrete owner recipe");
  lang::SemanticVisitor visitor(lang::TargetInfo::host(), &valid.sourceGraph,
                                true, nullptr, &valid.sources);
  expect(visitor.check(valid.program),
         "class alias recipes resolve in their declaration source graph");
  for (const auto &declaration : valid.program.declarations()) {
    const auto *owner = lang::stmtAs<lang::ClassDecl>(declaration.get());
    if (!owner || owner->name().lexeme != "Buffer")
      continue;
    const auto *info = visitor.model().findClassType(*owner);
    if (!info) {
      expect(false, "alias owner metadata is available");
      continue;
    }
    const auto instance = visitor.analyzeClassFieldInitializers(
        info->id, {lang::SemanticType::Int32}, {});
    expect(instance.valid(), "concrete class alias validation retains each "
                             "alias's own parameter scope");
    for (const auto &member : owner->members()) {
      const auto *syntax = lang::stmtAs<lang::TypeAliasDecl>(member.get());
      if (!syntax)
        continue;
      const auto *alias = instance.model.findTypeAlias(*syntax);
      const auto regions = alias ? lang::freeTypeLifetimes(alias->type)
                                 : std::vector<lang::Lifetime>{};
      expect(alias && regions.size() == 1 &&
                 regions.front().kind == lang::Lifetime::Kind::Parameter &&
                 std::any_of(alias->genericParameters.begin(),
                             alias->genericParameters.end(),
                             [&](const auto &parameter) {
                               return parameter.lifetime &&
                                      parameter.id == regions.front().id;
                             }),
             "owner instantiation preserves anonymous, alias-to-alias, and "
             "explicit alias lifetime binders");
    }
  }
  const auto fixed = analyze(declarations + R"(
Fixed invalid<lifetime a>(int&<a> input) { return Fixed(input); }
int main() { return 0; }
)");
  expect(fixed.syntaxValid && !fixed.semanticValid,
         "alias construction cannot freshen an explicitly fixed static region");
  const auto escaped = analyze(declarations + R"(
View<static> invalid() { int local = 3; return Inferred(local); }
int main() { return 0; }
)");
  expect(escaped.syntaxValid && !escaped.semanticValid,
         "an inferred alias application still proves the result's escape "
         "boundary");
  const auto explicitEscape = analyze(declarations + R"(
Buffer<int>::borrow<static> invalid<lifetime a>(Slots<int>&<a> input) { return input; }
int main() { return 0; }
)");
  expect(explicitEscape.syntaxValid && !explicitEscape.semanticValid,
         "an explicit alias lifetime argument cannot be freshened to match a "
         "shorter input");
}

void testReceiverBeforeSymbolicPack() {
  const auto valid = analyze(R"(
struct Box {
  mut int value;
  Box(int input) : value(input) {}
  mut int& get<Args...>(Args... ignored) mut { return this.value; }
};
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  int&<a> take<Args...>(Args... ignored) && { return this.value; }
};
int main() {
  mut Box box{3}; box.get() = 5; box.get(false) = 7;
  int external = 11;
  return box.get(true, 2) + View(external).take(false) - 18;
}
)");
  expectValid(valid, "receiver storage and external reference paths remain "
                     "known before an unresolved trailing pack");
  const auto staticEscape = analyze(R"(
struct Box {
  int value; Box(int input) : value(input) {}
  int&<static> get<Args...>(Args... ignored) { return this.value; }
};
int main() { Box box{7}; return box.get(); }
)");
  expect(staticEscape.syntaxValid && !staticEscape.semanticValid,
         "known receiver staging cannot extend its physical storage to static");
  const auto consumedShell = analyze(R"(
struct Box {
  int value; Box(int input) : value(input) {}
  int& take<Args...>(Args... ignored) && { return this.value; }
};
int main() { return Box(7).take(false); }
)");
  expect(consumedShell.syntaxValid && !consumedShell.semanticValid,
         "a consumed receiver shell remains invalid before a trailing pack is "
         "resolved");
  const auto localSibling = analyze(R"(
struct Box {
  int value; Box(int input) : value(input) {}
  int& get<Args...>(Args... ignored) { int local = 7; return local; }
};
int main() { Box box{3}; return box.get(); }
)");
  expect(localSibling.syntaxValid && !localSibling.semanticValid,
         "a known receiver cannot authorize a sibling local return origin");
}

void testPhysicalReferenceCarrierInitializers() {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto check = [&](std::string source) {
    return lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics})
        .analyze("physical-reference-carrier.gti", std::move(source),
                 {root / "prelude.gti"}, {}, {root});
  };
  const std::string declarations = R"gti(
struct ViewRef<lifetime data, lifetime shell> {
  std::string_view<data>&<shell> value;
  ViewRef(std::string_view<data>&<shell> source) : value(source) {}
};
)gti";
  const auto result = check(declarations + R"gti(
ViewRef<data, shell> borrowed<lifetime data, lifetime shell>(std::string_view<data>&<shell> source) {
  return ViewRef<data, shell>(source);
}
int main() { std::string_view text = "word"; return borrowed(text).value.size() == 4 ? 0 : 1; }
)gti");
  expectValid(result, "a reference field stores the checked view object "
                      "independently of the view's contents");
  const auto *owner = record(result, "ViewRef");
  const auto *lifecycle =
      owner && owner->declaration
          ? result.semantics.findClassLifecycle(*owner->declaration)
          : nullptr;
  bool checked = false;
  if (lifecycle)
    for (const auto &constructor : lifecycle->constructors) {
      if (!constructor.declaration ||
          constructor.declaration->initializers().size() != 1)
        continue;
      const auto *initializer = result.semantics.findConstructorInitializer(
          constructor.declaration->initializers().front());
      if (!initializer || !initializer->completeBorrow ||
          initializer->completeBorrow->dependencies.size() != 1)
        continue;
      const auto &dependency =
          initializer->completeBorrow->dependencies.front();
      const auto *address = result.semantics.findLoan(dependency.parent);
      const auto *contents =
          address ? result.semantics.findLoan(address->storedDependency)
                  : nullptr;
      checked =
          dependency.designatedFields.empty() && dependency.protectsStorage &&
          address && address->kind == lang::SemanticLoanKind::Reference &&
          address->entry && contents &&
          contents->kind == lang::SemanticLoanKind::StoredValue &&
          contents->entry && address->id != contents->id &&
          !contents->protectsStorage && constructor.completeBorrow &&
          constructor.completeBorrow->dependencies.size() == 1 &&
          constructor.completeBorrow->dependencies.front().origin ==
              lang::BorrowOriginKind::Argument &&
          constructor.completeBorrow->dependencies.front().parameter == 0 &&
          constructor.completeBorrow->dependencies.front().sourceFields.empty();
    }
  expect(checked, "the initializer retains the physical Reference entry while "
                  "preserving its separate Stored contents entry");
  const auto recordReference = check(R"gti(
struct View<lifetime data> { int&<data> value; View(int&<data> source) : value(source) {} };
struct Reference<T, lifetime shell> { T&<shell> value; Reference(T&<shell> source) : value(source) {} };
Reference<View<data>, shell> borrowed<lifetime data, lifetime shell>(View<data>&<shell> source) {
  return Reference<View<data>, shell>(source);
}
int main() { int data = 4; View value{data}; auto reference = borrowed(value); return reference.value.value - 4; }
)gti");
  expectValid(recordReference, "the same physical initializer rule preserves a "
                               "reference to an unrelated borrowed record");
  for (const auto source :
       {"ViewRef<data,data> invalid<lifetime data,lifetime "
        "shell>(std::string_view<data>&<shell> source){return "
        "ViewRef<data,data>(source);}int main(){return 0;}",
        "ViewRef<static,static> invalid(){std::string_view<static> "
        "text=\"word\";return ViewRef<static,static>(text);}int main(){return "
        "0;}",
        "int main(){ViewRef<static,static> value{\"word\"};return 0;}",
        "ViewRef<static,shell> invalid<lifetime data,lifetime "
        "shell>(std::string_view<data>&<shell> source){return "
        "ViewRef<static,shell>(source);}int main(){return 0;}"}) {
    const auto invalid = check(declarations + source);
    expect(invalid.syntaxValid && !invalid.semanticValid,
           "stored contents cannot lengthen the physical shell or create "
           "static, local, temporary or unrelated reference authority");
  }
  const auto exclusive = check(R"gti(
struct Exclusive<lifetime data, lifetime shell> {
  mut std::string_view<data>&<shell> value;
  Exclusive(mut std::string_view<data>&<shell> source) : value(source) {}
};
Exclusive<data,shell> invalid<lifetime data,lifetime shell>(std::string_view<data>&<shell> source) {
  return Exclusive<data,shell>(source);
}
int main() { return 0; }
)gti");
  expect(exclusive.syntaxValid && !exclusive.semanticValid,
         "physical initializer selection cannot upgrade a shared reference to "
         "exclusive access");
  const std::string localRecord = R"gti(
import std.memory;
struct View<lifetime data> {
  int&<data> external; mut int local = 1;
  View(int&<data> source) : external(source) {}
};
struct Reference<T, lifetime shell> {
  T&<shell> value;
  Reference(T&<shell> source) : value(source) {}
};
void consume<T>(T input) {}
)gti";
  for (const auto operation :
       {"auto moved = std::move(view);", "consume(std::move(view));",
        "view.local = 9;"}) {
    const auto invalid = check(
        localRecord +
        "int main(){int data=4;mut View view{data};Reference reference{view};" +
        operation + "return reference.value.local;}");
    expect(invalid.syntaxValid && !invalid.semanticValid,
           "an independent physical object borrow still prevents move, "
           "consumption and mutation while its wrapper is used");
  }
  const auto reassigned = check(declarations + R"gti(
int main() {
  mut std::string_view text = "word";
  ViewRef reference{text};
  text = "other";
  return reference.value.size();
}
)gti");
  expect(reassigned.syntaxValid && !reassigned.semanticValid,
         "separating physical and contents parents does not permit "
         "reassignment of the borrowed object");
}

void testCopiedBorrowedCaptureOwners() {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  for (const bool consumeSource : {false, true}) {
    for (const bool writeReferent : {false, true}) {
      std::string source = R"gti(
void consume<T>(T value) {}
class Holder<F> { mut F callable; public: Holder(F f):callable(std::move(f)) {} int run() mut { return this.callable(); } };
Holder<F> store<F>(F f) { return Holder<F>(std::move(f)); }
struct View<lifetime a> { int&<a> value; View(int&<a> input):value(input) {} };
int main() {
  mut int value=7;
  View view{value};
  auto callback=[view]()->int { return view.value; };
)gti";
      if (consumeSource)
        source += "consume(std::move(view));\n";
      source += "mut auto stored=store(std::move(callback));\n";
      if (writeReferent)
        source += "value=9;\n";
      source += "return stored.run()-7; }\n";
      const auto checked =
          lang::Frontend({.stopAfter = lang::FrontendPhase::Mir})
              .analyze(root.parent_path() /
                           "tests/fixtures/copied-callback-owner.gti",
                       source, {root / "prelude.gti"}, {}, {root});
      if (writeReferent) {
        expect(!checked.semanticValid &&
                   std::any_of(checked.diagnostics.begin(),
                               checked.diagnostics.end(),
                               [](const auto &diagnostic) {
                                 return diagnostic.code == "GTI-S2017" &&
                                        diagnostic.message.find(
                                            "Cannot assign to storage") !=
                                            std::string::npos;
                               }),
               "copied callback protects the external owner after the source "
               "carrier is consumed");
      } else {
        expectValid(
            checked,
            "copied callback retains its own external reference dependencies");
        expect(checked.canGenerateCode(),
               "copied borrowed state survives complete MIR replay and source "
               "consumption");
      }
    }
  }
}
void testOwnedCallableAppliedRegions() {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto check = [&](std::string source) {
    return lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics})
        .analyze("owned-callable-applied-regions.gti", std::move(source),
                 {root / "prelude.gti"}, {}, {root});
  };
  const std::string declarations = R"gti(
import std.memory;
class Holder<F> {
  mut F callable;
public:
  Holder(F callable) : callable(std::move(callable)) {}
  int run() mut { return this.callable(); }
};
Holder<F> store<F>(F callable) { return Holder<F>(std::move(callable)); }
)gti";
  const auto valid = check(declarations + R"gti(
int main() {
  mut int value = 4;
  mut auto update = [mut &value]() -> int { value += 1; return value; };
  mut auto stored = store(std::move(update));
  return stored.run() - 5;
}
)gti");
  expectValid(valid, "owned callable field transport uses the selected "
                     "declaration's checked lifetime application");
  const auto [exact, rejected] =
      lang::SemanticLifetimeTestAccess::ownedTransportApplicationChecks(valid);
  expect(exact && rejected == 8,
         "transport retains exact field and free-region identity and rejects "
         "absent, conflicting or forged application facts");
  const auto independent = check(declarations + R"gti(
int main() {
  int left = 3; int right = 4;
  auto callback = [&left, &right]() -> int { return left + right; };
  mut auto stored = store(std::move(callback));
  return stored.run() - 7;
}
)gti");
  expectValid(independent, "an owned callable retains both independent "
                           "captured regions in its exact owner");
  const auto noTransfer = check(declarations + R"gti(
int main() {
  mut int value = 4;
  mut auto update = [mut &value]() -> int { value += 1; return value; };
  mut auto stored = store(update);
  return stored.run();
}
)gti");
  expect(noTransfer.syntaxValid && !noTransfer.semanticValid,
         "applying an owned callable's regions does not grant an implicit "
         "ownership transfer");
}

void testDependentCallableUseEvidence() {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto callable = [&](std::string source) {
    return lang::Frontend().analyze("callable-copy-lifetimes.gti",
                                    std::move(source), {root / "prelude.gti"},
                                    {}, {root});
  };
  const auto valid = callable(R"(
T duplicate<T>(T& value) {
  if constexpr (std::copy_constructible<T>) { return value; }
  else { return value.clone(); }
}
int ignore<F>(F& callback) { return 7; }
int identity(int input) { return input; }
int main() { auto copy = duplicate(identity); return copy(7) + ignore(copy) - 14; }
)");
  expectValid(valid, "a deferred constexpr body does not fabricate an unused "
                     "callable-parameter contract");
  const auto *duplicate = function(valid, "duplicate");
  expect(duplicate && duplicate->callableParameters.empty(),
         "unvisited dependent branches provide no proof that a callable "
         "parameter is unused");
  const auto *ignore = function(valid, "ignore");
  expect(ignore && ignore->callableParameters.size() == 1 &&
             ignore->callableParameters.front().boundary ==
                 lang::CallableBoundary::Confined,
         "a fully checked unused parameter retains its confined contract");
  const auto invalid = callable(R"(
int invoke<F>(F& callback) {
  if constexpr (std::copy_constructible<F>) { return callback(7); }
  else { return 0; }
}
int booleanOnly(bool value) { return 1; }
int main() { auto callback = booleanOnly; return invoke(callback); }
)");
  expect(invalid.syntaxValid && !invalid.semanticValid,
         "a selected dependent branch still checks the concrete callable's "
         "argument contract");
}

void testStatelessItemTemporaryBorrow() {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto callable = [&](std::string source) {
    return lang::Frontend().analyze("stateless-item-borrow.gti",
                                    std::move(source), {root / "prelude.gti"},
                                    {}, {root});
  };
  const std::string declarations = R"(
int identity(int input) { return input; }
int&<a> referenceIdentity<lifetime a>(int&<a> input) { return input; }
int global = 9;
int&<static> fixedReference(int ignored) { return global; }
F duplicate<F>(F& value) {
  if constexpr (std::copy_constructible<F>) { return value; }
  else { return value.clone(); }
}
)";
  const auto copied = callable(declarations + R"(
int main() {
  auto value = duplicate(identity); auto reference = duplicate(referenceIdentity);
  auto fixed = duplicate(fixedReference); int&<static> globalReference = fixed(0);
  int first = 3; int second = 7;
  return value(2) + reference(first) + reference(second) + globalReference - 21;
}
)");
  expectValid(copied, "a checked readonly temporary borrow may copy an exact "
                      "stateless item without a confined callable contract");
  const auto *duplicate = function(copied, "duplicate");
  expect(duplicate && duplicate->callableParameters.empty(),
         "copying a function item does not fabricate a confined "
         "invocation-only parameter");
  for (const auto &[source, reason] :
       std::vector<std::pair<std::string, std::string>>{
           {"F& escape<F>(F& value) { return value; } int main() { "
            "escape(identity); return 0; }",
            "a copied item value does not permit returning its borrowed "
            "temporary shell"},
           {"F* address<F>(F& value) { unsafe { return &value; } } int main() "
            "{ address(identity); return 0; }",
            "a raw pointer result cannot hide a reference to the temporary "
            "item"},
           {"F unknown<F>(F& value); int main() { unknown(identity); return 0; "
            "}",
            "a bodyless callable target cannot prove an ordinary temporary "
            "borrow"},
           {"void edit<F>(mut F& value) {} int main() { edit(identity); return "
            "0; }",
            "a fresh stateless item does not acquire mutable temporary access"},
           {"int main() { int local = 7; auto copy = duplicate([&local](int "
            "ignored) -> int { return local; }); return copy(0); }",
            "a capturing lambda does not inherit the stateless function-item "
            "borrow proof"},
           {"int main() { int local = 7; auto copy = "
            "duplicate(referenceIdentity); int&<static> invalid = copy(local); "
            "return invalid; }",
            "copied invocation quantifiers cannot become a fixed static "
            "result"}}) {
    const auto invalid = callable(declarations + source);
    expect(invalid.syntaxValid && !invalid.semanticValid, reason);
  }
}

void testScopedCallableResultProjectionContracts() {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto check = [&](const std::string &source) {
    return lang::Frontend(
               {.stopAfter = lang::FrontendPhase::Mir,
                .lowerReachableOnly = true,
                .lowerLibraryReachableOnly = true,
                .trustedBodies = lang::TrustedBodyAnalysis::Reachable})
        .analyze("scoped-callable-result.gti", "import std.meta; " + source,
                 {root / "prelude.gti"}, {}, {root});
  };
  expect(lang::SemanticLifetimeTestAccess::scopedProjectionCatalogCopies(),
         "private query recipes and normalized type facts survive independent "
         "catalog snapshots");
  const std::string identity = "int& identity(int& value){return value;} ";
  const std::string main = " int main(){int owner=42;auto same=identity;int& "
                           "result=borrow(same,owner);return result-42;}";
  const std::vector<std::pair<std::string, std::string>> valid{
      {"item",
       identity +
           "[[borrowed_from(value)]] std::meta::callable_return_type<F> "
           "borrow<F>(F& fn,int& value){return fn(value);}" +
           main},
      {"lambda", "[[borrowed_from(value)]] std::meta::callable_return_type<F> "
                 "borrow<F>(F& fn,int& value){return fn(value);} int "
                 "main(){int owner=42;auto same=[](int& value)->int&{return "
                 "value;};int& result=borrow(same,owner);return result-42;}"},
      {"literal-elided",
       "[[borrowed_from(value)]] std::meta::callable_return_type<int&(int&)> "
       "borrow(int& value){return value;} int main(){int owner=42;return "
       "borrow(owner)-42;}"},
      {"literal-scoped",
       "[[borrowed_from(value)]] std::meta::callable_return_type<for<lifetime "
       "a> int&<a>(int&<a>)> borrow(int& value){return value;} int main(){int "
       "owner=42;return borrow(owner)-42;}"},
      {"generic-alias",
       identity +
           "using Result<F> = std::meta::callable_return_type<F>;using "
           "Again<F> = Result<F>;[[borrowed_from(value)]] Again<F> "
           "borrow<F>(F& fn,int& value){return fn(value);}" +
           main},
      {"closed-alias",
       "using Signature=for<lifetime a> int&<a>(int&<a>);using "
       "Result=std::meta::callable_return_type<Signature>;[[borrowed_from("
       "value)]] Result borrow(int& value){return value;}int main(){int "
       "owner=42;return borrow(owner)-42;}"},
      {"member-alias", identity +
                           "struct Projection<F>{using "
                           "Result=std::meta::callable_return_type<F>;};[["
                           "borrowed_from(value)]] Projection<F>::Result "
                           "borrow<F>(F& fn,int& value){return fn(value);}" +
                           main},
      {"fixed-static",
       "int global=42;[[borrowed_from(value)]] "
       "std::meta::callable_return_type<int&<static>(int&)> borrow(int& "
       "value){return global;}int main(){int owner=0;int&<static> "
       "result=borrow(owner);return result-42;}"},
      {"free-with-bound",
       "[[borrowed_from(value)]] "
       "std::meta::callable_return_type<int&<a>(int&<a>)> borrow<lifetime "
       "a,lifetime b>(int&<b> value) requires outlives<b,a>{return value;}int "
       "main(){int owner=42;return borrow(owner)-42;}"}};
  for (const auto &[name, source] : valid) {
    const auto result = check(source);
    if (!result.semanticValid || !result.hirValid || !result.mirValid) {
      std::cerr << "scoped result profile: " << name << '\n';
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ':' << diagnostic.primary.line << ' '
                  << diagnostic.message << '\n';
    }
    expect(result.semanticValid && result.hirValid && result.mirValid,
           "scoped result provenance survives exact alias/instance "
           "normalization and independent executable proof");
    if (result.hirValid && name != "fixed-static" &&
        name != "free-with-bound") {
      bool related = false;
      for (const auto &instance : result.hir.functionInstances()) {
        if (!instance.source || instance.source->name().lexeme != "borrow" ||
            instance.parameterTypes.empty() ||
            instance.returnType.kind != lang::SemanticType::Reference)
          continue;
        const lang::LifetimeBound bound{
            instance.parameterTypes.back().referenceLifetime,
            instance.returnType.referenceLifetime};
        related = instance.lifetimeContract.valid() &&
                  std::find(instance.lifetimeContract.bounds.begin(),
                            instance.lifetimeContract.bounds.end(),
                            bound) != instance.lifetimeContract.bounds.end() &&
                  instance.completeReturnBorrow &&
                  !instance.completeReturnBorrow->dependencies.empty();
      }
      expect(related, "the selected declared input bounds the exposed query "
                      "binder before complete body proof");
    }
  }
  const std::vector<std::pair<std::string, std::string>> invalid{
      {"wrong-origin",
       identity + "[[borrowed_from(value)]] std::meta::callable_return_type<F> "
                  "borrow<F>(F& fn,int& value,int& other){return "
                  "fn(other);}int main(){int owner=42;int other=7;auto "
                  "fn=identity;return borrow(fn,owner,other);}"},
      {"local",
       "[[borrowed_from(value)]] std::meta::callable_return_type<int&(int&)> "
       "borrow(int& value){int local=0;return local;}int main(){int "
       "owner=0;return borrow(owner);}"},
      {"overlap",
       identity + "[[borrowed_from(value)]] std::meta::callable_return_type<F> "
                  "borrow<F>(F& fn,int& value){return fn(value);}int "
                  "main(){mut int owner=42;auto fn=identity;int& "
                  "result=borrow(fn,owner);owner=7;return result;}"},
      {"static-strengthening",
       identity + "[[borrowed_from(value)]] std::meta::callable_return_type<F> "
                  "borrow<F>(F& fn,int& value){return fn(value);}int "
                  "main(){int owner=0;auto fn=identity;int&<static> "
                  "bad=borrow(fn,owner);return bad;}"},
      {"free-missing-bound",
       "[[borrowed_from(value)]] "
       "std::meta::callable_return_type<int&<a>(int&<a>)> borrow<lifetime "
       "a,lifetime b>(int&<b> value){return value;}int main(){return 0;}"},
      {"alias-free-missing-bound",
       "using Result<lifetime a> = "
       "std::meta::callable_return_type<int&<a>(int&<a>)>;[[borrowed_from("
       "value)]] Result<a> borrow<lifetime a,lifetime b>(int&<b> value){return "
       "value;}int main(){return 0;}"},
      {"incompatible-alias",
       "using Result<lifetime a> = "
       "std::meta::callable_return_type<int&<a>(int&<a>)>;[[borrowed_from("
       "value)]] Result<static> borrow(int& value){return value;}int "
       "main(){int owner=0;return borrow(owner);}"},
      {"mutable-access",
       "[[borrowed_from(value)]] std::meta::callable_return_type<for<lifetime "
       "a> mut int&<a>(mut int&<a>)> borrow(int& value){return value;}int "
       "main(){int owner=0;return borrow(owner);}"},
      {"ordinary-alias", "int global=0;using Result<T> = T;Result<T> "
                         "borrow<T>(){return global;}int main(){int&<static> "
                         "bad=borrow<int&<static>>();return bad;}"}};
  for (const auto &[name, source] : invalid) {
    const auto result = check(source);
    if (result.semanticValid)
      std::cerr << "unexpected scoped result acceptance: " << name << '\n';
    expect(result.syntaxValid && !result.semanticValid,
           "projection provenance cannot invent free/static/access/origin "
           "authority or hidden generic references");
  }
}

void testCheckedProjectedCallableResults() {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto check = [&](std::string source) {
    return lang::Frontend(
               {.stopAfter = lang::FrontendPhase::Mir,
                .lowerReachableOnly = true,
                .lowerLibraryReachableOnly = true,
                .trustedBodies = lang::TrustedBodyAnalysis::Reachable})
        .analyze("checked-callable-result.gti", std::move(source),
                 {root / "prelude.gti"}, {}, {root});
  };
  const std::string declarations = R"(
import std.meta;
int stored = 7;
int&<static> map(int ignored) { return stored; }
)";
  const std::string projected =
      "std::meta::callable_return_type<F> run<F>(F& mapper)";
  for (const auto &[definition, invocation] :
       std::vector<std::pair<std::string, std::string>>{
           {projected + " { return mapper(1); }", "run(fn)"},
           {projected + " { return mapper(1); }", "run(map)"},
           {"using Result<F> = std::meta::callable_return_type<F>; "
            "Result<F> run<F>(F& mapper) { return mapper(1); }",
            "run(map)"},
           {"struct Worker { Worker() = default; " + projected +
                " && { return mapper(1); } };",
            "Worker().run(map)"},
           {projected +
                " { return mapper(1); } int run(bool other) { return 0; }",
            "run(map)"}}) {
    const auto valid = check(
        declarations + definition +
        " int main() { auto fn = map; int&<static> result = " + invocation +
        "; return result - 7; }");
    if (valid.semanticValid && (!valid.hirValid || !valid.mirValid))
      for (const auto &diagnostic : valid.diagnostics)
        std::cerr << diagnostic.code << " at " << diagnostic.primary.source
                  << ':' << diagnostic.primary.line << ": "
                  << diagnostic.message << '\n';
    expectValid(valid, "a resolved callable result recipe forms its exact "
                       "checked reference result");
    expect(valid.hirValid && valid.mirValid,
           "named, fresh, aliased, overloaded and consuming projected results "
           "retain exact executable contracts");
    bool checked = false;
    for (const auto &instance : valid.hir.functionInstances()) {
      if (!instance.source || instance.source->name().lexeme != "run" ||
          instance.typeArguments.empty())
        continue;
      checked =
          instance.returnType.kind == lang::SemanticType::Reference &&
          instance.returnType.referenceLifetime ==
              lang::Lifetime::staticLifetime() &&
          instance.lifetimeContract.valid() && instance.completeReturnBorrow &&
          instance.completeReturnBorrow->dependencies.size() == 1 &&
          instance.completeReturnBorrow->dependencies.front().origin ==
              lang::BorrowOriginKind::Global &&
          instance.completeReturnBorrow->dependencies.front().globalRoot != 0 &&
          instance.returnBorrowOrigin == lang::BorrowOriginKind::Global &&
          instance.returnBorrowPlace &&
          (instance.owner ? instance.returnBorrowFields ==
                                instance.completeReturnBorrow->dependencies
                          : instance.returnBorrowFields.empty()) &&
          instance.returnBorrowPlace->root ==
              instance.completeReturnBorrow->dependencies.front().globalRoot;
    }
    expect(checked, "complete body proof owns the exact global supplier "
                    "independently of the projected static result");
  }
  for (const auto &[source, reason] :
       std::vector<std::pair<std::string, std::string>>{
           {"T wrong<T>() { return stored; } int main() { int&<static> result "
            "= wrong<int&<static>>(); return result; }",
            "an ordinary generic parameter cannot acquire a hidden reference "
            "result"},
           {"using Identity<T> = T; Identity<T> wrong<T>() { return stored; } "
            "int main() { int&<static> result = wrong<int&<static>>(); return "
            "result; }",
            "an ordinary generic parameter alias cannot impersonate a callable "
            "result projection"},
           {projected +
                " { int local = 7; return local; } int main() { auto fn = map; "
                "int&<static> result = run(fn); return result; }",
            "a projected static result cannot publish local storage"},
           {"struct Worker { int value; Worker(int input) : value(input) {} " +
                projected +
                " && { return this.value; } }; int main() { auto fn = map; "
                "int&<static> result = Worker(7).run(fn); return result; }",
            "a projected result cannot publish a consumed receiver field"},
           {projected + " { return mapper(1); } int main() { mut int&<static> "
                        "result = run(map); return result; }",
            "a projected readonly reference cannot become mutable"},
           {projected + "; int main() { int&<static> result = run(map); return "
                        "result; }",
            "a bodyless projected result supplies no nonescaping temporary "
            "proof"},
           {"int&<a> tied<F, lifetime a>(F&<a> mapper) { return stored; } int "
            "main() { int&<static> result = tied(map); return result; }",
            "a global body supplier cannot strengthen an explicit result "
            "lifetime tied to a temporary input"},
           {"int& tied<F>(F& mapper) { return stored; } int main() { "
            "int&<static> result = tied(map); return result; }",
            "a global body supplier cannot strengthen an elided result "
            "lifetime tied to a temporary input"},
           {"F& expose<F>(F& mapper) { return mapper; } int main() { "
            "expose(map); return 0; }",
            "a complete result tied to the fresh callable shell remains "
            "rejected"},
           {"int* address(int ignored) { unsafe { return &stored; } } " +
                projected +
                " { return mapper(1); } int main() { run(address); return 0; }",
            "an untracked raw result cannot authorize a fresh callable "
            "reference input"}}) {
    const auto invalid = check(declarations + source);
    expect(invalid.syntaxValid && !invalid.semanticValid, reason);
  }
}

void testFreshCallableLifetimeArguments() {
  const std::string declarations = R"(
int global = 7;
int&<static> map(int ignored) { return global; }
int observe<F>(F& callback) { return callback(0); }
)";
  const auto valid = analyze(declarations + R"(
int main() { return observe(map) - 7; }
)");
  expectValid(valid, "a fresh function item's fixed result region survives its "
                     "checked call-local reference adaptation");
  const auto *main = function(valid, "main");
  const auto *returned =
      main && main->declaration && main->declaration->body()
          ? lang::stmtAs<lang::ReturnStmt>(
                main->declaration->body()->statements().front().get())
          : nullptr;
  const auto *difference =
      returned ? lang::exprAs<lang::Binary>(returned->value().get()) : nullptr;
  const auto *call =
      difference ? lang::exprAs<lang::Call>(difference->left().get()) : nullptr;
  const auto *item = call && call->arguments().size() == 1
                         ? valid.semantics.findFunctionItemSelection(
                               *call->arguments().front())
                         : nullptr;
  const auto *resultType = item ? item->type.functionItemReturnType() : nullptr;
  expect(item && item->declaration &&
             item->declaration->name().lexeme == "map" && resultType &&
             resultType->kind == lang::SemanticType::Reference &&
             resultType->referenceLifetime == lang::Lifetime::staticLifetime(),
         "a callable value-load context preserves the selected reference "
         "result and its static region");
  const auto incompatible = analyze(declarations + R"(
struct Token { Token() = default; };
Token observeToken<F>(F& callback) { return callback(0); }
int main() { Token value = observeToken(map); return 0; }
)");
  expect(incompatible.syntaxValid && !incompatible.semanticValid &&
             std::any_of(incompatible.diagnostics.begin(),
                         incompatible.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2090";
                         }),
         "contextual function-item selection cannot load an incompatible "
         "reference referent");
  const auto missingReference = analyze(R"(
int value(int ignored) { return 7; }
int observe<F>(F& callback) { int& selected = callback(0); return selected; }
int main() { return observe(value); }
)");
  expect(missingReference.syntaxValid && !missingReference.semanticValid,
         "a callable reference-result requirement cannot borrow a returned "
         "temporary value");
  const auto polymorphic = analyze(R"(
int&<a> identity<lifetime a>(int&<a> value) { return value; }
int invoke<F>(F& callback, int& value) { return callback(value); }
int main() { int first = 3; int second = 8; return invoke(identity, first) + invoke(identity, second) - 11; }
)");
  expectValid(polymorphic,
              "a function item's quantified signature is instantiated "
              "independently for each checked invocation");
  const auto extends = analyze(R"(
int&<a> identity<lifetime a>(int&<a> value) { return value; }
int&<static> invoke<F>(F& callback, int& value) { return callback(value); }
int main() { int value = 7; return invoke(identity, value); }
)");
  expect(extends.syntaxValid && !extends.semanticValid,
         "an invocation-polymorphic item cannot promote a caller's local input "
         "to fixed static storage");
  for (const std::string &source :
       {"F& escape<F>(F& callback) { return callback; } int main() { "
        "escape(map); return 0; }",
        "void edit<F>(mut F& callback) {} int main() { edit(map); return 0; "
        "}"}) {
    const auto invalid = analyze(declarations + source);
    expect(invalid.syntaxValid && !invalid.semanticValid,
           "checked region adaptation does not let a fresh callable shell "
           "escape or acquire mutable access");
  }
  const auto stronger = analyze(R"(
struct View<lifetime a> { int&<a> value; View(int&<a> input) : value(input) {} };
void needsStatic(View<static>& value) {}
int main() { int local = 7; needsStatic(View(local)); return 0; }
)");
  expect(stronger.syntaxValid && !stronger.semanticValid &&
             std::any_of(stronger.diagnostics.begin(),
                         stronger.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2127";
                         }),
         "fresh producer classification cannot supply an unproved stronger "
         "nested storage region");
}

void testReferencePackReborrows() {
  expect(lang::SemanticLifetimeTestAccess::mutableReferenceReborrowChecks(),
         "a checked mutable reborrow shortens only the outer region and "
         "retains invariant referent proof");
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto numeric = [&](std::string source) {
    // The primary fold retains its generic requirement; HIR demand checks each
    // concrete lane, including rejected bool instantiations.
    return lang::Frontend().analyze("numeric-reference-pack.gti",
                                    std::move(source), {root / "prelude.gti"},
                                    {}, {root});
  };
  const auto valid = numeric(R"(
void bump<T>(mut T& value) requires std::numeric<T> { value++; }
void update<Args...>(mut Args&... values) { (bump(values), ...); }
int main() { mut int left = 3; mut int right = 4; update(left, right); return left + right - 9; }
)");
  expectValid(
      valid,
      "mutable reference pack elements reborrow into each exact call region");
  const auto unsupported = numeric(R"(
void bump<T>(mut T& value) requires std::numeric<T> { value++; }
void update<Args...>(mut Args&... values) { (bump(values), ...); }
int main() { mut bool value = false; update(value); return 0; }
)");
  expect(unsupported.syntaxValid && !unsupported.semanticValid,
         "concrete fold expansion retains the increment helper's numeric "
         "requirement");
  const auto unconstrained = analyze(
      "void bump<T>(mut T& value) { value++; } int main() { return 0; }");
  expect(unconstrained.syntaxValid && !unconstrained.semanticValid,
         "an unconstrained generic reference does not establish a numeric "
         "mutation capability");
  const auto invariant = analyze(R"(
struct Cell<lifetime a> { int&<a> value; Cell(int&<a> input) : value(input) {} };
void use<lifetime b>(mut Cell<b>& value) {}
void relay<lifetime b, Args...>(mut Args&... values) { use<b>(values...); }
void invalid<lifetime a, lifetime b>(mut Cell<a>& value) requires outlives<a, b> { relay<b>(value); }
int main() { int input = 7; mut Cell value{input}; invalid(value); return 0; }
)");
  expect(invariant.syntaxValid && !invariant.semanticValid,
         "a shorter call reborrow cannot change the lifetime stored through a "
         "mutable referent");
}

void testSymbolicCallablePackReturns() {
  const auto library = std::filesystem::weakly_canonical(
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib");
  const lang::PackageSourceRoot modules{
      .identity = "toolchain:std",
      .name = "std",
      .packageRoot = library,
      .sourceRoot = library,
      .modules = {{"meta", library / "std/meta.gti"},
                  {"detail_function_pointer_impl",
                   library / "std/detail/function_pointer.gti"}},
      .kind = lang::SourceOwnerKind::Toolchain,
      .implicitAlias = "std",
      .defaultModule = "meta"};
  const auto check = [&](std::string source) {
    source.insert(0, "import std.detail_function_pointer_impl;\n");
    const auto path = library / "lifetime-symbolic-callable-pack.gti";
    return lang::Frontend().analyze(
        path, source, {library / "prelude.gti", path},
        {{path.string(), source}}, {library}, {modules});
  };
  const std::string pointer = R"(
class Relay<R&<result>(Args...)> {
  gti_internal::function_pointer<R&<result>(Args...)> target;
public:
  Relay<F>(F input)
      : target(gti_internal::function_pointer_from_item<R&<result>(Args...)>(input)) {}
  R&<result> invoke(Args... args) {
    return gti_internal::function_pointer_invoke(this.target, args...);
  }
};
using Selection = for<lifetime a> int&<a>(int&<a>, int&<a>, bool);
int&<a> choose<lifetime a>(int&<a> left, int&<a> right, bool first) {
  if (first) { return left; } return right;
}
)";
  expectValid(check(pointer + R"(
int main() {
  Relay<Selection> relay{choose}; int left = 3; int right = 8;
  return relay.invoke(left, right, false) - 8;
}
)"),
              "a symbolic pointer invocation defers its pack without "
              "fabricating reference suppliers");
  for (const std::string main :
       {"int main() { Relay<Selection> relay{choose}; return relay.invoke(); }",
        "int main() { Relay<Selection> relay{choose}; return "
        "relay.invoke(true, false, true); }"}) {
    const auto invalid = check(pointer + main);
    expect(invalid.syntaxValid && !invalid.semanticValid,
           "concrete pointer packs retain exact arity and reference-input "
           "requirements");
  }
  expectValid(check(R"(
class Procedure<Args...> {
  gti_internal::function_pointer<void(Args...)> target;
public:
  Procedure<F>(F input)
      : target(gti_internal::function_pointer_from_item<void(Args...)>(input)) {}
  void invoke(Args... args) {
    return gti_internal::function_pointer_invoke(this.target, args...);
  }
};
void complete() {}
int main() { Procedure<> procedure{complete}; procedure.invoke(); return 0; }
)"),
              "a concrete empty callable pack retains a genuine void result");
  const std::string deferred = R"(
class Forward<R&<result>(Args...)> {
public:
  Forward() = default;
  R&<result> invoke<F>(F& callable, Args... args) {
    auto relay = [](F& target, Args... values) -> R&<result> {
      return target(values...);
    };
    return relay(callable, args...);
  }
};
using Selection = for<lifetime a> int&<a>(int&<a>);
)";
  expectValid(check(deferred + R"(
int&<a> identity<lifetime a>(int&<a> value) { return value; }
int main() {
  Forward<Selection> forward{}; auto callable = identity; int value = 7;
  return forward.invoke(callable, value) - 7;
}
)"),
              "a deferred callable pack is replayed through its exact "
              "reference result contract");
  for (const std::string tail :
       {"int temporary(int value) { return value; } int main() { "
        "Forward<Selection> forward{}; auto callable = temporary; int value = "
        "7; return forward.invoke(callable, value); }",
        "mut int&<a> exclusive<lifetime a>(mut int&<a> value) { return value; "
        "} int main() { Forward<Selection> forward{}; auto callable = "
        "exclusive; int value = 7; return forward.invoke(callable, value); "
        "}"}) {
    const auto invalid = check(deferred + tail);
    expect(invalid.syntaxValid && !invalid.semanticValid,
           "deferred callable packs cannot publish a temporary or strengthen "
           "argument access");
  }
  auto stronger = deferred;
  const std::string original = "for<lifetime a> int&<a>(int&<a>)";
  stronger.replace(stronger.find(original), original.size(),
                   "for<lifetime a> int&<static>(int&<a>)");
  const auto staticResult = check(stronger + R"(
int&<a> identity<lifetime a>(int&<a> value) { return value; }
int main() {
  Forward<Selection> forward{}; auto callable = identity; int value = 7;
  return forward.invoke(callable, value);
}
)");
  expect(staticResult.syntaxValid && !staticResult.semanticValid,
         "a pending result conversion cannot promote an independent input "
         "region to static");
  auto sibling = deferred;
  const std::string forwarding = "return target(values...);";
  sibling.replace(sibling.find(forwarding), forwarding.size(),
                  "target(values...); return 0;");
  const auto unrelated = check(sibling + "int main() { return 0; }");
  expect(unrelated.syntaxValid && !unrelated.semanticValid,
         "a deferred lambda call cannot authorize a sibling temporary return");
  const auto missing =
      analyze("int& missing<Args...>(Args... args); int main() { return 0; }");
  expect(missing.syntaxValid && !missing.semanticValid,
         "a symbolic prototype still needs a normalized result lifetime "
         "relationship");
}

void testConsumingExternalResults() {
  const auto valid = analyze(R"(
int global = 4;
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
class Consumer<lifetime a> {
  View<a> external;
public:
  Consumer(int&<a> input) : external(View<a>(input)) {}
  int&<a> take() && { return this.external.value; }
  int&<a> operator()(int&<a> input) && { return input; }
  View<a> carrier() && { return this.external; }
};
struct Stateless {
  Stateless() = default;
  int&<a> argument<lifetime a>(int&<a> value) && { return value; }
  int&<static> persistent() && { return global; }
};
struct Exclusive<lifetime a> {
  mut int&<a> value;
  Exclusive(mut int&<a> input) : value(input) {}
  mut int&<a> release() && { return this.value; }
};
enum Result<lifetime a> { present(int&<a> value), empty(int code) };
struct Empty {
  Empty() = default;
  Result<static> take() && { return Result<static>::empty(0); }
};
int main() {
  int value = 7;
  int& first = Consumer(value).take();
  int& second = Consumer(value)(value);
  View copy = Consumer(value).carrier();
  int& third = Stateless().argument(value);
  int& fourth = Stateless().persistent();
  Result empty = Empty().take();
  mut int slot = 5;
  mut int& last = Exclusive(slot).release();
  last = 6;
  return first + second + copy.value + third + fourth + last - 38;
}
)");
  expectValid(valid,
              "consuming methods and call operators retain only complete "
              "external suppliers, including empty carrier alternatives");
  const auto sharedOperator = analyze(R"(
struct Shared<lifetime a> {
  int&<a> value;
  Shared(int&<a> input) : value(input) {}
  int&<a> operator()(int&<a> input) && { return input; }
};
int main() {
  int value = 7; int& retained = value;
  int& selected = Shared(value)(retained);
  return retained + selected - 14;
}
)");
  expectValid(sharedOperator, "consuming a fresh operator receiver preserves "
                              "shared access to its external supplier");
  const auto emptyAlternatives = analyze(R"(
enum Result<lifetime a> { present(int&<a> value), empty(int code) };
Result<static> makeEmpty() { return Result<static>::empty(0); }
struct Empty {
  Empty() = default;
  Result<static> take() && { return Result<static>::empty(0); }
};
int main() {
  Result direct = Result<static>::empty(0);
  Result free = makeEmpty();
  Result consumed = Empty().take();
  return 0;
}
)");
  expectValid(emptyAlternatives,
              "checked alternative transport preserves ownerless results "
              "across inferred binding regions");
  const auto borrowedAlternative = analyze(R"(
enum Result<lifetime a> { present(int&<a> value), empty(int code) };
Result<static> invalid<lifetime a>(int&<a> value, bool present) {
  if (present) { return Result<a>::present(value); }
  return Result<static>::empty(0);
}
int main() { int value = 7; Result result = invalid(value, true); return 0; }
)");
  expect(borrowedAlternative.syntaxValid && !borrowedAlternative.semanticValid,
         "an empty alternative cannot erase another possible result's shorter "
         "external owner");
  for (const std::string &source :
       {"struct Owned { int value; Owned() : value(1) {} int&<this> bad() && { "
        "return this.value; } }; int main() { return 0; }",
        "struct Owned { int value; Owned() : value(1) {} int&<this> "
        "operator()() && { return this.value; } }; int main() { return 0; }",
        "struct Owned { int value; Owned() : value(1) {} }; struct Outer { "
        "Owned value; Outer() : value() {} int&<this> bad() && { return "
        "this.value.value; } }; int main() { return 0; }",
        "struct View<lifetime a> { int&<a> value; View(int&<a> input) : "
        "value(input) {} }; struct Owned { int value; Owned() : value(1) {} "
        "View<this> bad() && { return View<this>(this.value); } }; int main() "
        "{ return 0; }",
        "struct Local { Local() = default; int&<static> bad() && { int value = "
        "1; return value; } }; int main() { return 0; }",
        "struct View<lifetime a> { int&<a> value; View(int&<a> input) : "
        "value(input) {} }; struct Mixed<lifetime a> { View<a> external; int "
        "owned; Mixed(int&<a> input) : external(input), owned(1) {} View<a> "
        "bad(bool first) && requires outlives<this, a> { if (first) { return "
        "this.external; } return View<a>(this.owned); } }; int main() { return "
        "0; }",
        "struct Mixed<lifetime a> { int&<a> external; int owned; Mixed(int&<a> "
        "input) : external(input), owned(1) {} int&<a> bad(bool first) && "
        "requires outlives<this, a> { if (first) { return this.external; } "
        "return this.owned; } }; int main() { return 0; }"}) {
    const auto invalid = analyze(source);
    expect(invalid.syntaxValid && !invalid.semanticValid,
           "consuming borrowed results reject receiver shells, owned fields "
           "and mixed external/owned return paths");
  }
  const auto exclusive = analyze(R"(
struct Exclusive<lifetime a> {
  mut int&<a> value;
  Exclusive(mut int&<a> input) : value(input) {}
  int&<a> operator()(int&<a> input) && { return input; }
};
int main() { mut int value = 1; int& result = Exclusive(value)(value); return result; }
)");
  expect(exclusive.syntaxValid && !exclusive.semanticValid &&
             std::any_of(exclusive.diagnostics.begin(),
                         exclusive.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2017" &&
                                  diagnostic.message.find("overlap") !=
                                      std::string::npos;
                         }),
         "consuming a fresh carrier preserves an exclusive external "
         "dependency's alias restrictions");
  const auto ordinary = analyze(R"(
struct View<lifetime a> { int&<a> value; View(int&<a> input) : value(input) {} };
View<a> keep<lifetime a>(View<a> input) { return input; }
int main() { int value = 7; View view{value}; View result = keep(view); return result.value - 7; }
)");
  expectValid(ordinary, "unconditional by-value carrier entries retain their "
                        "complete external paths");
  const auto *keep = function(ordinary, "keep");
  expect(keep && keep->completeReturnBorrow &&
             keep->completeReturnBorrow->dependencies.size() == 1 &&
             keep->completeReturnBorrow->dependencies.front().origin ==
                 lang::BorrowOriginKind::Argument &&
             !keep->completeReturnBorrow->dependencies.front()
                  .sourceFields.empty(),
         "ordinary carrier return metadata reuses the checked entry group "
         "instead of inferring an empty owner");
}

void testFreshReceiverStorageRegions() {
  const auto result = analyze(R"(
struct External<lifetime a> {
  int&<a> value;
  External(int&<a> input) : value(input) {}
  int&<a> release() && { return this.value; }
  int read() { return this.value; }
};
External<a> makeExternal<lifetime a>(int&<a> input) { return External<a>(input); }
int&<a> direct<lifetime a>(int&<a> input) { return External<a>(input).release(); }
int&<a> factory<lifetime a>(int&<a> input) { return makeExternal(input).release(); }
int&<a> grouped<lifetime a>(int&<a> input) { return (External<a>(input)).release(); }
int&<a> sequenced<lifetime a>(int&<a> input) { return (0, External<a>(input)).release(); }
int retained<lifetime a, lifetime shell>(External<a>&<shell> input) { return input.read(); }
int main() {
  int value = 7; External held{value};
  return direct(value) + factory(value) + grouped(value) + sequenced(value) + retained(held) - 35;
}
)");
  expectValid(result, "fresh receiver shells have physical origins independent "
                      "of their external suppliers");
  for (const auto name :
       {"direct", "factory", "grouped", "sequenced", "retained"}) {
    const auto *info = function(result, name);
    const auto *returned =
        info && info->declaration && info->declaration->body()
            ? lang::stmtAs<lang::ReturnStmt>(
                  info->declaration->body()->statements().front().get())
            : nullptr;
    const auto *call =
        returned ? lang::exprAs<lang::Call>(returned->value().get()) : nullptr;
    const auto *member =
        call ? lang::exprAs<lang::Get>(call->callee().get()) : nullptr;
    const auto *selected = call ? result.semantics.findCall(*call) : nullptr;
    const bool fresh = std::string_view(name) != "retained";
    const bool exactFreshOrigin =
        selected && member &&
        std::any_of(
            selected->lifetimeApplication.origins.begin(),
            selected->lifetimeApplication.origins.end(),
            [&](const auto &origin) {
              return origin.source == member->object().get() &&
                     !origin.place.valid() &&
                     origin.region.kind == lang::Lifetime::Kind::Local &&
                     origin.region != selected->returnType.referenceLifetime;
            });
    expect(selected && selected->lifetimeApplication.complete &&
               exactFreshOrigin == fresh,
           "only checked fresh receiver expressions publish a separate "
           "expression-local shell witness");
  }
  const auto stronger = analyze(R"(
int global = 7;
struct External<lifetime a> {
  int&<a> value;
  External(int&<a> input) : value(input) {}
  int requireStaticShell() requires outlives<this, static> { return this.value; }
};
int main() { return External<static>(global).requireStaticShell(); }
)");
  expect(stronger.syntaxValid && !stronger.semanticValid &&
             std::any_of(stronger.diagnostics.begin(),
                         stronger.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2127";
                         }),
         "a static external referent does not make a fresh receiver's physical "
         "shell static");
}

void testEvaluatedReferenceReceiverRegions() {
  const auto result = analyze(R"(
struct View<lifetime storage> {
  int&<storage> value;
  View(int&<storage> input) : value(input) {}
  int read() { return this.value; }
  int touch() mut { return this.value; }
};
T&<a> forward_reference<T, lifetime a>(T&<a> input) requires outlives<T, a> {
  return input;
}
int direct<lifetime storage, lifetime shell>(View<storage>&<shell> input) {
  return input.read();
}
int retained<lifetime storage, lifetime shell>(View<storage>&<shell> input) {
  View<storage>&<shell> same = forward_reference(input);
  return same.read();
}
int forwarded<lifetime storage, lifetime shell>(View<storage>&<shell> input) {
  return forward_reference(input).read();
}
int grouped<lifetime storage, lifetime shell>(View<storage>&<shell> input) {
  return (input).read();
}
int sequenced<lifetime storage, lifetime shell>(View<storage>&<shell> input) {
  return (0, input).read();
}
int exclusive<lifetime storage, lifetime shell>(mut View<storage>&<shell> input) {
  return input.touch();
}
int main() {
  int input = 7; mut View value{input};
  return direct(value) + retained(value) + forwarded(value) + grouped(value) +
         sequenced(value) + exclusive(value) - 42;
}
)");
  expectValid(result, "a reference receiver applies its checked outer region "
                      "independently of stored suppliers");
  for (const auto name : {"direct", "retained", "forwarded", "grouped",
                          "sequenced", "exclusive"}) {
    const auto *info = function(result, name);
    const auto *returned =
        info && info->declaration && info->declaration->body()
            ? lang::stmtAs<lang::ReturnStmt>(
                  info->declaration->body()->statements().back().get())
            : nullptr;
    const auto *call =
        returned ? lang::exprAs<lang::Call>(returned->value().get()) : nullptr;
    const auto *member =
        call ? lang::exprAs<lang::Get>(call->callee().get()) : nullptr;
    const auto *selected = call ? result.semantics.findCall(*call) : nullptr;
    const auto *target =
        selected ? result.semantics.findFunction(selected->function) : nullptr;
    auto outer = info && !info->parameterTypes.empty()
                     ? info->parameterTypes.front().referenceLifetime
                     : lang::Lifetime{};
    if (const auto *forwarded =
            member ? lang::exprAs<lang::Call>(member->object().get()) : nullptr)
      if (const auto *identity = result.semantics.findCall(*forwarded))
        outer = identity->returnType.referenceLifetime;
    lang::Lifetime appliedReceiver;
    if (selected && target)
      for (const auto &[parameter, actual] :
           selected->lifetimeApplication.bindings)
        if (parameter == target->lifetimeContract.receiver)
          appliedReceiver = actual;
    const bool exactOuter =
        selected && outer.specified() && appliedReceiver.specified() &&
        std::find(selected->lifetimeApplication.obligations.begin(),
                  selected->lifetimeApplication.obligations.end(),
                  lang::LifetimeBound{outer, appliedReceiver}) !=
            selected->lifetimeApplication.obligations.end();
    const bool inventedOrigin =
        selected && member &&
        std::any_of(selected->lifetimeApplication.origins.begin(),
                    selected->lifetimeApplication.origins.end(),
                    [&](const auto &origin) {
                      return origin.source == member->object().get();
                    });
    expect(selected && selected->lifetimeApplication.complete && exactOuter &&
               !inventedOrigin,
           "reference receiver application retains the evaluated outer region "
           "without inventing an owner place");
  }
  for (const auto source : {R"(
struct View<lifetime storage> {
  int&<storage> value;
  int read() requires outlives<this, storage> { return this.value; }
};
T&<a> forward_reference<T, lifetime a>(T&<a> input) requires outlives<T, a> { return input; }
int invalid<lifetime storage, lifetime shell>(View<storage>&<shell> input) {
  View<storage>&<shell> same = forward_reference(input);
  return same.read();
}
int main() { return 0; }
)",
                            R"(
struct View<lifetime storage> {
  int&<storage> value;
  int read() requires outlives<this, static> { return this.value; }
};
T&<a> forward_reference<T, lifetime a>(T&<a> input) requires outlives<T, a> { return input; }
int invalid<lifetime shell>(View<static>&<shell> input) { return forward_reference(input).read(); }
int main() { return 0; }
)"}) {
    const auto invalid = analyze(source);
    expect(invalid.syntaxValid && !invalid.semanticValid &&
               std::any_of(invalid.diagnostics.begin(),
                           invalid.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2127";
                           }),
           "a stored external region never extends a shorter checked receiver "
           "shell");
  }
}

void testFixedArityPackCallSelection() {
  const auto result = analyze(R"(
int choose(int first, int second = 5) { return first + second; }
int choose(bool first, int second) { return second; }
int defaulted(int value = 9) { return value; }
int relay<Args...>(Args... values) { return choose(values...); }
int emptyRelay<Args...>(Args... values) { return defaulted(values...); }
int main() { return relay(3) + relay(true, 4) + emptyRelay() - 21; }
)");
  expectValid(result, "fixed overload selection and default arguments use "
                      "flattened pack arity, including zero lanes");
  const auto wrongArity = analyze(R"(
int fixed(int first, int second) { return first + second; }
int relay<Args...>(Args... values) { return fixed(values...); }
int main() { return relay(3); }
)");
  expect(!wrongArity.semanticValid, "a concrete forwarded pack still satisfies "
                                    "the selected fixed target arity");
}

void testAnonymousStructuralDestinationRegions() {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto check = [&](std::string source,
                         lang::FrontendPhase phase =
                             lang::FrontendPhase::Semantics) {
    return lang::Frontend({.stopAfter = phase})
        .analyze("anonymous-destination.gti", std::move(source),
                 {root / "prelude.gti"}, {}, {root});
  };
  const auto hidden = check(R"gti(
struct View { int& value; View(int& input) : value(input) {} };
int main() {
  int input = 7;
  View original{input}; View copied{original};
  View moved{std::move(original)}; auto called = View(copied);
  return copied.value + moved.value + called.value - 21;
}
)gti");
  expectValid(hidden, "anonymous typed destinations infer the exact structural "
                      "source with no explicit policy");
  const auto *main = function(hidden, "main");
  const auto *body =
      main && main->declaration ? main->declaration->body().get() : nullptr;
  const auto *original =
      body && body->statements().size() > 2
          ? lang::stmtAs<lang::VariableDecl>(body->statements()[1].get())
          : nullptr;
  const auto *copy =
      body && body->statements().size() > 2
          ? lang::stmtAs<lang::VariableDecl>(body->statements()[2].get())
          : nullptr;
  const auto *source =
      original ? hidden.semantics.findBinding(*original) : nullptr;
  const auto *destination =
      copy ? hidden.semantics.findBinding(*copy) : nullptr;
  const auto *selection =
      copy && copy->initializer()
          ? hidden.semantics.findConstruction(*copy->initializer())
          : nullptr;
  const auto *graph = body ? hidden.semantics.findLifetimeBody(*body) : nullptr;
  if (source && destination && selection && graph &&
      source->type.lifetimeArguments.size() == 1 &&
      destination->type.lifetimeArguments.size() == 1) {
    const auto from = source->type.lifetimeArguments.front();
    const auto to = destination->type.lifetimeArguments.front();
    expect(from != to && selection->constructedType == source->type &&
               std::find(graph->obligations.begin(), graph->obligations.end(),
                         lang::LifetimeBound{from, to}) !=
                   graph->obligations.end() &&
               std::find(graph->obligations.begin(), graph->obligations.end(),
                         lang::LifetimeBound{to, from}) !=
                   graph->obligations.end(),
           "structural destination inference retains the written identity and "
           "proves both region directions");
  } else {
    expect(false, "anonymous structural copy retains its exact binding, "
                  "selection and lifetime graph");
  }
  expectValid(check(R"gti(
struct View<T> {
  T& value; View(T& input) : value(input) {}
  View(View<T>& other) = default; View(View<T>&& other) = default;
};
int main() {
  int input = 7; View<int> original{input}; View<int> copied{original};
  View<int> moved{std::move(original)}; auto called = View<int>(copied);
  return copied.value + moved.value + called.value - 21;
}
)gti"),
              "explicit structural policies retain anonymous typed copy and "
              "actual named-source move");
  const std::string cell = R"gti(
struct Cell<lifetime a> { int&<a> value; Cell(int&<a> input) : value(input) {} };
)gti";
  expectValid(check(cell + R"gti(
int&<a> read<lifetime a>(int&<a> input) {
  Cell<a> original{input}; Cell copied{original}; return copied.value;
}
int main() { int input = 7; return read(input) - 7; }
)gti"),
              "an anonymous copied destination retains the declared external "
              "reference result");
  expectValid(check(cell + "int main(){int input=7;Cell original{input};auto "
                           "copy=Cell<_>(original);return copy.value-7;}"),
              "a written anonymous type argument uses only that construction's "
              "inference inventory");
  const std::string pair = R"gti(
struct Pair<lifetime a, lifetime b> {
  int&<a> first; int&<b> second;
  Pair(int&<a> x, int&<b> y) : first(x), second(y) {}
};
using Equal<lifetime a> = Pair<a, a>;
)gti";
  expectValid(check(pair + R"gti(
int read<lifetime a>(int&<a> input) {
  Pair<a, a> source{input, input}; Equal copy{source}; return copy.first + copy.second;
}
int main() { int input = 7; return read(input) - 14; }
)gti"),
              "repeated anonymous destination slots retain one correlated "
              "source region");
  for (const std::string &invalid :
       {pair + "void reject<lifetime a,lifetime b>(Pair<a,b>& source){Equal "
               "copy{source};}int main(){return 0;}",
        cell + "int main(){int input=7;Cell source{input};Cell<static> "
               "copy{source};return 0;}",
        cell + "void reject<lifetime a,lifetime b>(Cell<b>& source){Cell<a> "
               "copy{source};}int main(){return 0;}",
        cell + "struct View{int& value;View(int& input):value(input){}};int "
               "main(){int input=7;View source{input};Cell copy{source};return "
               "0;}"}) {
    const auto result = check(invalid);
    expect(result.syntaxValid && !result.semanticValid &&
               std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2012";
                           }),
           "destination inference preserves fixed, repeated and unrelated "
           "structural identities");
  }
  const std::string fixedGeneric = R"gti(
int construct_fixed<T, Args...>(Args... values) { auto copy = T(values...); return 0; }
)gti";
  const auto exact = check(fixedGeneric + R"gti(
struct Plain { int value; Plain(int input) : value(input) {} };
int main() { Plain source{7}; return construct_fixed<Plain>(source); }
)gti",
                           lang::FrontendPhase::Hir);
  expect(exact.semanticValid && exact.hirValid,
         "a concrete value-only generic construction accepts its unchanged "
         "exact target");
  // These calls demand the concrete body: stopping at declarations would not
  // exercise the construction hidden behind an unresolved source pack.
  for (const auto target : {"Cell<static>", "Cell<_>"}) {
    const auto fixed = check(cell + fixedGeneric +
                                 "int main(){int input=7;Cell "
                                 "source{input};return construct_fixed<" +
                                 target + ">(source);}",
                             lang::FrontendPhase::Hir);
    expect(fixed.syntaxValid && !fixed.hirValid &&
               std::any_of(fixed.diagnostics.begin(), fixed.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2012";
                           }) &&
               std::none_of(fixed.diagnostics.begin(), fixed.diagnostics.end(),
                            [](const auto &diagnostic) {
                              return diagnostic.code == "GTI-S2029";
                            }),
           "a substituted generic target does not acquire inference authority "
           "from the source application");
  }
  const auto deleted =
      check("struct View{int& value;View(int& "
            "input):value(input){}View(View&)=delete;};int main(){int "
            "input=7;View source{input};View copy{source};return 0;}");
  expect(!deleted.semanticValid &&
             std::any_of(deleted.diagnostics.begin(), deleted.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2020";
                         }),
         "successful anonymous destination inference still reaches the deleted "
         "copy policy");
  expect(
      !check(
           "struct View{int& value;View(int& "
           "input):value(input){}View(View&&)=delete;};int main(){int "
           "input=7;View source{input};View copy{std::move(source)};return 0;}")
           .semanticValid,
      "anonymous destination inference never permits a deleted named-source "
      "move");
  expectValid(
      check(
          "struct View{int& value;View(int& "
          "input):value(input){}View(View&)=delete;View(View&&)=delete;};int "
          "main(){int input=7;View value{View(input)};return value.value-7;}"),
      "fresh-result elision remains independent of deleted copy and move "
      "policies");
  expect(
      !check(
           cell +
           "Cell<static> invalid<lifetime a>(int&<a> input){Cell "
           "source{input};Cell copy{source};return copy;}int main(){return 0;}")
           .semanticValid,
      "inferred destination equality cannot extend a local or generic owner to "
      "static storage");
}

void testStructuralSpecialMemberPolicies() {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto withMove = [&](std::string source) {
    return lang::Frontend().analyze("structural-policy.gti", std::move(source),
                                    {root / "prelude.gti"}, {}, {root});
  };
  const auto scalar = withMove(R"(
struct Plain {
  int value;
  Plain(int input) : value(input) {}
  Plain(Plain& other) = default;
  Plain(Plain&& other) = default;
};
int main() {
  Plain original{3}; Plain copied{original};
  Plain source{4}; Plain moved{std::move(source)};
  return copied.value + moved.value - 7;
}
)");
  expectValid(scalar, "explicit defaulted copy and move preserve the generated "
                      "scalar operation");
  unsigned transfers = 0;
  if (const auto *main = function(scalar, "main");
      main && main->declaration->body())
    for (const auto &statement : main->declaration->body()->statements())
      if (const auto *binding =
              lang::stmtAs<lang::VariableDecl>(statement.get());
          binding && binding->initializer())
        if (const auto *selected =
                scalar.semantics.findConstruction(*binding->initializer());
            selected && selected->kind != lang::ConstructorKind::Ordinary) {
          ++transfers;
          expect(selected->constructor != 0 && selected->declaration &&
                     selected->parameterTypes.size() == 1 &&
                     selected->parameterTypes.front().kind ==
                         lang::SemanticType::Reference &&
                     !selected->parameterTypes.front()
                          .referenceLifetime.specified() &&
                     selected->lifetimeApplication ==
                         lang::SemanticLifetimeApplication{},
                 "a proved structural policy retains declaration identity "
                 "without publishing a fictitious invocation");
        }
  expect(transfers == 2,
         "both explicit structural transfer modes retain checked selections");

  const auto borrowed = analyze(R"(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  View(View<a>& other) requires outlives<int, a> = default;
  View(View<a>&& other) = default;
};
int read<lifetime a>(int&<a> input) {
  View<a> original{input}; View<a> copied{original}; View<a> moved{View<a>(input)};
  return copied.value + moved.value;
}
int main() { int value = 7; return read(value) - 14; }
)");
  expectValid(borrowed, "structural policy proof preserves fixed owner regions "
                        "and redundant nominal formation facts");

  // Auto keeps the existing hidden owner application during transfer; the
  // explicit owner case also exercises selected constructor metadata.
  for (const std::string source :
       {"struct View { int& value; View(int& input):value(input){} View(View& "
        "other)=default; View(View&& other)=default; }; int main(){int "
        "input=7; View original{input}; auto copied=original; auto "
        "moved=std::move(original); return copied.value+moved.value-14;}",
        "struct View<T> { T& value; View(T& input):value(input){} "
        "View(View<T>& other)=default; View(View<T>&& other)=default; }; int "
        "main(){int input=7; View<int> original{input}; auto copied=original; "
        "auto moved=std::move(original); return copied.value+moved.value-14;}",
        "struct View<lifetime a> { int&<a> value; View(int&<a> "
        "input):value(input){} View(View& other)=default; View(View&& "
        "other)=default; }; int read<lifetime a>(int&<a> input){View<a> "
        "original{input}; View<a> copied{original}; View<a> "
        "moved{std::move(original)}; return copied.value+moved.value;} int "
        "main(){int value=7;return read(value)-14;}"})
    expectValid(withMove(source),
                "exact self policies retain hidden and omitted owner regions "
                "without requiring explicit spelling");
  for (const std::string source :
       {"struct View<lifetime a> { int&<a> value; View(int&<a> "
        "input):value(input){} View(View<static>& other)=default; }; int "
        "main(){return 0;}",
        "struct View<lifetime a, lifetime b> { int&<a> first; int&<b> second; "
        "View(int&<a> x,int&<b> y):first(x),second(y){} View(View<b,a>& "
        "other)=default; }; int main(){return 0;}"})
    expect(!analyze(source).semanticValid,
           "contextual self elision never changes written static or "
           "differently correlated owner regions");

  for (const auto &[source, parameter] :
       std::array<std::pair<std::string_view, std::string_view>, 3>{
           {{"struct Plain { int value; Plain(int input):value(input){} "
             "Plain(Plain&<static> other) = default; }; int main() { return 0; "
             "}",
             "Plain&"},
            {"struct View<lifetime a> { int&<a> value; View(int&<a> "
             "input):value(input){} View(View<a>&<a> other) = default; }; int "
             "main() { return 0; }",
             "View<a>&"},
            {"struct View<lifetime a> { int&<a> value; View(int&<a> "
             "input):value(input){} View(View<a>& other) requires outlives<a, "
             "static> = default; }; int main() { return 0; }",
             "View<a>&"}}}) {
    const auto rejected = analyze(source);
    const auto diagnostic = std::find_if(
        rejected.diagnostics.begin(), rejected.diagnostics.end(),
        [](const auto &item) {
          return item.code == "GTI-S2020" &&
                 item.message.find("structural lifetime contract") !=
                     std::string::npos;
        });
    expect(!rejected.semanticValid &&
               diagnostic != rejected.diagnostics.end() &&
               std::filesystem::path(diagnostic->primary.source).filename() ==
                   "lifetime-resolution.gti" &&
               diagnostic->primary.start == source.find(parameter),
           "stronger default policies reject at their written source parameter "
           "before any invocation");
  }
  for (const auto source :
       {"struct Plain { int value; Plain(int input):value(input){} "
        "Plain(Plain& other)=delete; }; int main(){Plain source{1}; Plain "
        "copy{source}; return copy.value;}",
        "struct Plain { int value; Plain(Plain& other):value(other.value){} }; "
        "int main(){return 0;}"})
    expect(!analyze(source).semanticValid,
           "deleted policies and unsupported custom copy bodies retain their "
           "existing rejection");
  expect(
      !withMove("struct Plain { int value; Plain(int input):value(input){} "
                "Plain(Plain&& other)=delete; }; int main(){Plain source{1}; "
                "Plain value{std::move(source)}; return value.value;}")
           .semanticValid,
      "a deleted move rejects actual transfer from a named source, "
      "independently of direct-result elision");
}

void testConstructorRelationsAfterFieldIdentity() {
  const auto result = analyze(R"(
struct Early {
  Early(int& first, int& second) : left(first), right(second) {}
  int& left;
  int& right;
};
struct Generic<T> {
  Generic(T& input) : value(input) {}
  T& value;
};
struct Explicit<lifetime stored, lifetime input> {
  int&<stored> value;
  Explicit(int&<input> source) requires outlives<input, stored> : value(source) {}
};
int main() {
  int first = 3; int second = 8;
  Early early{first, second}; Generic<int> generic{first};
  return early.left + early.right + generic.value - 14;
}
)");
  expectValid(result, "anonymous constructor input relations are finalized "
                      "after canonical field identities exist");
  for (const auto name : {"Early", "Generic", "Explicit"}) {
    const auto *owner = record(result, name);
    const auto *lifecycle =
        owner && owner->declaration
            ? result.semantics.findClassLifecycle(*owner->declaration)
            : nullptr;
    expect(lifecycle && lifecycle->constructors.size() == 1,
           "each constructor retains one normalized declaration contract");
    if (!owner || !lifecycle || lifecycle->constructors.size() != 1)
      continue;
    const auto &constructor = lifecycle->constructors.front();
    std::vector<lang::Lifetime> binders;
    for (const auto region : constructor.lifetimeContract.parameters)
      if (std::find(binders.begin(), binders.end(), region) == binders.end())
        binders.push_back(region);
    expect(binders.size() == constructor.lifetimeContract.parameters.size(),
           "finalizing initializer relations never allocates duplicate "
           "lifetime binders");
    const auto count =
        std::min(owner->fields.size(), constructor.parameterTypes.size());
    for (std::size_t index = 0; index < count; ++index) {
      const lang::LifetimeBound expected{
          constructor.parameterTypes[index].referenceLifetime,
          owner->fields[index].type.referenceLifetime};
      expect(std::find(constructor.lifetimeContract.bounds.begin(),
                       constructor.lifetimeContract.bounds.end(),
                       expected) != constructor.lifetimeContract.bounds.end(),
             "the finalized contract retains each exact input-to-stored-region "
             "edge");
    }
  }
}

void testSelectedBaseConstructorInputs() {
  const std::string declarations = R"(
struct RegionBase<lifetime a> {
  int&<a> value;
  int marker;
  RegionBase<lifetime input>(int&<input> source, int selected = 7)
      requires outlives<input, a> : value(source), marker(selected) {}
};
struct RegionDerived<lifetime a> : public RegionBase<a> {
  RegionDerived(int&<a> source) : RegionBase<a>(source) {}
};
int&<a> relay<lifetime a>(int&<a> source) {
  RegionDerived<a> derived{source};
  return derived.value;
}


)";
  const auto selected = analyze(declarations + R"(
int main() { int source = 5; RegionDerived derived{source}; return relay(source) + derived.marker - 12; }
)");
  expectValid(selected, "selected base constructors preserve lifetime-only "
                        "binders and plain trailing defaults");
  const auto escape = analyze(declarations + R"(
int main() { int source = 5; RegionDerived<static> escaped{source}; return escaped.value; }
)");
  expect(!escape.semanticValid, "selected base constructor inference cannot "
                                "lengthen a fixed static owner region");
  const auto fixedPack = analyze(R"(
struct SumBase {
  int value;
  SumBase(int first, int second) : value(first + second) {}
};
struct SumDerived<Args...> : public SumBase {
  SumDerived(Args... values) : SumBase(values...) {}
};
int main() { SumDerived<int, int> value{3, 8}; return value.value - 11; }
)");
  expectValid(fixedPack, "a forwarded concrete pack selects a fixed-arity base "
                         "constructor by flattened inputs");
  const auto borrowedDefault = analyze(R"(
int global = 3;
struct Base<lifetime a> {
  int&<a> value;
  Base(int&<a> input = global) : value(input) {}
};
int main() { return 0; }
)");
  expect(!borrowedDefault.semanticValid &&
             std::any_of(borrowedDefault.diagnostics.begin(),
                         borrowedDefault.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2077";
                         }),
         "borrowing constructor defaults remain explicit under S2077");
  const auto exclusive = analyze(R"(
struct Base<lifetime a> {
  mut int&<a> first; mut int&<a> second;
  Base(mut int&<a> left, mut int&<a> right) : first(left), second(right) {}
};
struct Derived<lifetime a> : public Base<a> {
  Derived(mut int&<a> input) : Base<a>(input, input) {}
};
int main() { return 0; }
)");
  expect(!exclusive.semanticValid, "base initialization cannot duplicate one "
                                   "exclusive source across selected inputs");
}

void testCompleteOrdinaryConstructorInputs() {
  const auto result = analyze(R"(
int&<a> choose<lifetime a>(int&<a> left, int&<a> right, bool first) {
  if (first) { return left; }
  return right;
}
struct Selected<lifetime a> {
  int&<a> value;
  Selected(int&<a> left, int&<a> right, bool first)
      : value(choose(left, right, first)) {}
};
int main() {
  int left = 3; int right = 8;
  Selected selected{left, right, false};
  return selected.value - 8;
}
)");
  expectValid(result, "an unconditional constructor preserves every supplier "
                      "of its checked scalar initializer");
  const auto *owner = record(result, "Selected");
  const auto *lifecycle =
      owner && owner->declaration
          ? result.semantics.findClassLifecycle(*owner->declaration)
          : nullptr;
  const auto *summary = lifecycle && lifecycle->constructors.size() == 1 &&
                                lifecycle->constructors.front().completeBorrow
                            ? &*lifecycle->constructors.front().completeBorrow
                            : nullptr;
  expect(summary && summary->dependencies.size() == 2 &&
             summary->dependencies[0].resultFields ==
                 summary->dependencies[1].resultFields &&
             summary->dependencies[0].parameter !=
                 summary->dependencies[1].parameter,
         "one stored reference can retain two distinct permitted formal "
         "suppliers");
  for (const auto source : {
           R"(struct Local { int& value; Local(int input) : value(input) {} };
int main() { Local value{3}; return value.value; })",
           R"(mut int&<a> relay<lifetime a>(mut int&<a> input) { return input; }
struct Duplicate<lifetime a> {
  mut int&<a> first; mut int&<a> second;
  Duplicate(mut int&<a> input) : first(relay(input)), second(relay(input)) {}
};
int main() { mut int source = 3; Duplicate value{source}; return value.first; })"}) {
    const auto rejected = analyze(source);
    expect(!rejected.semanticValid,
           "complete initializer proof rejects local storage escape and "
           "duplicated exclusive origins");
  }
}

void testCheckedReferenceErasureLifetimeAuthority() {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto prelude = std::filesystem::weakly_canonical(root / "prelude.gti");
  const auto capabilities = std::filesystem::weakly_canonical(
      root / "std/detail/memory_capabilities.gti");
  const auto entry = std::filesystem::weakly_canonical(
      root / "__trusted_tests__/lifetime-erasure.gti");
  const std::string capabilitySource = R"(
export module detail.memory_capabilities;
namespace gti_internal {
export class erased_object {};
export erased_object& erase_reference<T>(T& object);
export mut erased_object& erase_reference_mut<T>(mut T& object);
export unsafe T&<a> restore_reference<T, lifetime a>(erased_object&<a> object) requires outlives<T, a>;
export unsafe mut T&<a> restore_reference_mut<T, lifetime a>(mut erased_object&<a> object) requires outlives<T, a>;
}
)";
  const auto check = [&](std::string source) {
    source.insert(0, "import \"../std/detail/memory_capabilities.gti\";\n");
    return lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics})
        .analyze(entry, source, {prelude, entry},
                 {{prelude.string(), ""},
                  {capabilities.string(), capabilitySource},
                  {entry.string(), source}},
                 {root});
  };
  const std::string wrappers = R"(
using Opaque = gti_internal::erased_object;
Opaque& erase<T>(T& source) { return gti_internal::erase_reference(source); }
mut Opaque& erase_mut<T>(mut T& source) { return gti_internal::erase_reference_mut(source); }
unsafe T&<a> restore<T, lifetime a>(Opaque&<a> source) requires outlives<T, a> {
  unsafe { return gti_internal::restore_reference<T>(source); }
}
unsafe mut T&<a> restore_mut<T, lifetime a>(mut Opaque&<a> source) requires outlives<T, a> {
  unsafe { return gti_internal::restore_reference_mut<T>(source); }
}
)";
  const auto valid = check(wrappers + R"(
int main() {
  mut int value = 3;
  unsafe { restore_mut<int>(erase_mut(value)) = 7; return restore<int>(erase(value)) - 7; }
}
)");
  expectValid(valid, "checked type erasure preserves the physical input "
                     "lifetime through generic source wrappers");
  const auto *restoration = function(valid, "restore");
  expect(restoration && restoration->restoresReferenceResult,
         "a checked restoration wrapper retains authority across its applied "
         "outer lifetime");
  const auto escaping = check(wrappers + R"(
Opaque&<static> invalid() { int local = 3; return erase(local); }
int main() { return 0; }
)");
  expect(!escaping.semanticValid, "checked erasure cannot make a local object "
                                  "satisfy a static return lifetime");
  const auto wrongAccess = check(wrappers + R"(
mut Opaque& invalid(int& source) { return erase_mut(source); }
int main() { return 0; }
)");
  expect(
      !wrongAccess.semanticValid,
      "checked erasure cannot manufacture mutable access to a shared source");
  const auto localShell = check(wrappers + R"(
struct Borrowed<lifetime storage> {
  int&<storage> value;
  Borrowed(int&<storage> source) : value(source) {}
};
Opaque&<storage> invalid<lifetime storage>(int&<storage> source) {
  Borrowed<storage> local{source};
  return erase(local);
}
int main() { return 0; }
)");
  expect(localShell.syntaxValid && !localShell.semanticValid,
         "erasing a local borrowed carrier retains its shell lifetime, not its "
         "external owner's lifetime");
  const auto unrelatedTarget = check(wrappers + R"(
struct Other { Other() = default; };
Other& invalid<T>(T& source) { return erase(source); }
int main() { return 0; }
)");
  expect(unrelatedTarget.syntaxValid && !unrelatedTarget.semanticValid,
         "opaque physical-view proof does not authorize unrelated ordinary "
         "referent types");
  const std::string borrowed = R"(
struct Borrowed<lifetime storage> {
  int&<storage> value;
  Borrowed(int&<storage> source) : value(source) {}
};
)";
  const auto roundTripCarrier = check(wrappers + R"(
struct Pair<lifetime a, lifetime b> {
  int&<a> left; int&<b> right;
  Pair(int&<a> first, int&<b> second) : left(first), right(second) {}
};
T&<shell> round_trip<T, lifetime shell>(T&<shell> source) {
  unsafe { return restore<T, shell>(erase(source)); }
}
mut T&<shell> round_trip_mut<T, lifetime shell>(mut T&<shell> source) {
  unsafe { return restore_mut<T, shell>(erase_mut(source)); }
}
int main() {
  int left = 3; int right = 7;
  mut Pair carrier{left, right};
  mut Pair& changed = round_trip_mut(carrier);
  Pair& same = round_trip(changed);
  return same.left + same.right - 10;
}
)");
  expectValid(roundTripCarrier,
              "checked erasure maps the physical carrier reference instead of "
              "its independent stored fields");
  const auto validBorrowed = check(wrappers + borrowed + R"(
unsafe Borrowed<storage>&<shell> recover<lifetime storage, lifetime shell>(Opaque&<shell> source)
    requires outlives<storage, shell> {
  unsafe { return restore<Borrowed<storage>, shell>(source); }
}
int main() { return 0; }
)");
  expectValid(validBorrowed, "restoration of borrowed T uses its independently "
                             "proved type-outlives relationship");
  const auto shortBorrowed = check(wrappers + borrowed + R"(
unsafe Borrowed<storage>&<static> invalid<lifetime storage>(Opaque&<static> source) {
  unsafe { return restore<Borrowed<storage>, static>(source); }
}
int main() { return 0; }
)");
  expect(shortBorrowed.syntaxValid && !shortBorrowed.semanticValid,
         "a static opaque witness cannot make an independent nested borrow "
         "static");
  const auto unknownReferent = check(wrappers + R"(
unsafe T&<a> invalid<T, lifetime a>(Opaque&<a> source) {
  unsafe { return restore<T, a>(source); }
}
int main() { return 0; }
)");
  expect(unknownReferent.syntaxValid && !unknownReferent.semanticValid,
         "unsafe restoration still requires validity of an opaque generic "
         "referent for the output lifetime");
}

void testOwnedPointeeLifetimeAuthority() {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto prelude = std::filesystem::weakly_canonical(root / "prelude.gti");
  const auto capabilities = std::filesystem::weakly_canonical(
      root / "std/detail/memory_capabilities.gti");
  const auto entry = std::filesystem::weakly_canonical(
      root / "__trusted_tests__/lifetime-owned-pointee.gti");
  const std::string capabilitySource = R"(
export module detail.memory_capabilities;
namespace gti_internal {
export class unique_owner<T> {};
export unique_owner<T> allocate_unique_owner<T, Args...>(Args... values);
export T& unique_owner_borrow<T>(unique_owner<T>& value);
export mut T& unique_owner_borrow_mut<T>(mut unique_owner<T>& value);
}
)";
  const std::string declarations = R"(
class Vessel<T> {
  mut gti_internal::unique_owner<T> storage = nullptr;
public:
  Vessel() : storage(gti_internal::allocate_unique_owner<T>()) {}
  T& read() { return gti_internal::unique_owner_borrow(this.storage); }
  mut T& edit() mut { return gti_internal::unique_owner_borrow_mut(this.storage); }
};
)";
  const auto check = [&](std::string source) {
    source.insert(0, "import \"../std/detail/memory_capabilities.gti\";\n");
    return lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics})
        .analyze(entry, source, {prelude, entry},
                 {{prelude.string(), ""},
                  {capabilities.string(), capabilitySource},
                  {entry.string(), source}},
                 {root});
  };
  const auto valid = check(declarations + R"(
int main() { mut Vessel<int> owner{}; mut int& element = owner.edit(); element = 7; return owner.read() - 7; }
)");
  expectValid(valid, "an unrelated generic owning accessor keeps checked "
                     "physical pointee authority despite opaque T");
  const auto escaping = check(declarations + R"(
int& invalid() { Vessel<int> local{}; return local.read(); }
int main() { return 0; }
)");
  expect(!escaping.semanticValid, "a checked owning projection cannot make a "
                                  "local owner outlive its returned borrow");
  const auto wrongAccess = check(R"(
class Vessel<T> {
  gti_internal::unique_owner<T> storage = nullptr;
public:
  mut T& invalid() { return gti_internal::unique_owner_borrow(this.storage); }
};
int main() { return 0; }
)");
  expect(!wrongAccess.semanticValid,
         "physical owned-pointee authority cannot strengthen the trusted "
         "borrow operation's access");
  const auto consumedPointee = check(R"(
class Vessel<T> {
  gti_internal::unique_owner<T> storage = nullptr;
public:
  T&<this> invalid() && { return gti_internal::unique_owner_borrow(this.storage); }
};
int main() { return 0; }
)");
  expect(consumedPointee.syntaxValid && !consumedPointee.semanticValid,
         "a consuming checked owner cannot return storage destroyed with its "
         "owned pointee");
}
void testSharedStoredReferenceCleanupCopy() {
  const std::string declarations = R"gti(
mut int observed = 0;
int global = 7;
int otherGlobal = 11;
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
  ~View() { observed = observed + this.value; }
};
struct Pair<lifetime a, lifetime b> : public View<a> {
  int&<b> other;
  Pair(int&<a> first, int&<b> second) : View<a>(first), other(second) {}
  ~Pair() { observed = observed + this.other; }
};
)gti";
  const auto check = [&](std::string body) {
    return lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics})
        .analyze("shared-reference-cleanup-copy.gti", declarations + body);
  };
  const auto valid = check(R"gti(
int&<a> direct<lifetime a>(int&<a> input) {
  View view{input};
  return view.value;
}
int&<a> inherited<lifetime a>(int&<a> input) {
  int local = 9;
  Pair pair{input, local};
  return pair.value;
}
int&<static> permanent() {
  View<static> view{global};
  return view.value;
}
int main() { int value = 7; return direct(value) + inherited(value) + permanent() - 21; }
)gti");
  expectValid(valid, "copying an exact shared reference field retains its "
                     "independent supplier through carrier cleanup");
  const auto *permanent = function(valid, "permanent");
  const auto *global = permanent && permanent->returnBorrowPlace
                           ? valid.semantics.database().findSymbol(
                                 permanent->returnBorrowPlace->root)
                           : nullptr;
  expect(global && global->name == "global" &&
             permanent->completeReturnBorrow &&
             permanent->completeReturnBorrow->dependencies.size() == 1 &&
             permanent->completeReturnBorrow->dependencies.front().globalRoot ==
                 global->id,
         "a checked static return retains the exact global used by its field "
         "initializer, not another same-type global");
  expect(permanent &&
             lang::SemanticLifetimeTestAccess::exactStaticReturnSupplier(
                 *permanent),
         "the shared static supplier predicate recognizes the complete checked "
         "body summary");
  if (permanent && permanent->completeReturnBorrow &&
      !permanent->completeReturnBorrow->dependencies.empty()) {
    for (int mutation = 0; mutation != 9; ++mutation) {
      auto changed = *permanent;
      switch (mutation) {
      case 0:
        ++changed.completeReturnBorrow->dependencies.front().globalRoot;
        break;
      case 1:
        changed.completeReturnBorrow.reset();
        break;
      case 2:
        changed.completeReturnBorrow->dependencies.clear();
        break;
      case 3:
        changed.returnBorrowPlace->root = 0;
        break;
      case 4:
        changed.returnBorrowAccess = lang::AccessMode::Mutable;
        break;
      case 5:
        changed.completeReturnBorrow->dependencies.push_back(
            changed.completeReturnBorrow->dependencies.front());
        ++changed.completeReturnBorrow->dependencies.back().globalRoot;
        break;
      case 6:
        changed.completeReturnBorrow->dependencies.front()
            .sourceFields.push_back(
                lang::fieldProjection(global ? global->id : 1));
        break;
      case 7:
        changed.returnType.referenceLifetime = {};
        break;
      case 8:
        changed.completeReturnBorrow->possibleAlternatives.push_back({});
        break;
      }
      expect(
          !lang::SemanticLifetimeTestAccess::exactStaticReturnSupplier(changed),
          "preserving a static supplier rejects wrong globals, unknown "
          "summaries, access/path drift, alternatives and nonstatic results");
    }
  }
  for (const auto name : {"direct", "inherited"}) {
    const auto *target = function(valid, name);
    const auto *body = target && target->declaration
                           ? target->declaration->body().get()
                           : nullptr;
    const auto *returned =
        body && !body->statements().empty()
            ? lang::stmtAs<lang::ReturnStmt>(body->statements().back().get())
            : nullptr;
    const auto *complete =
        returned ? valid.semantics.findBorrowDependencySet(*returned->value())
                 : nullptr;
    const auto *place =
        returned ? valid.semantics.findPlace(*returned->value()) : nullptr;
    const auto *group =
        complete && complete->dependencies.size() == 1
            ? valid.semantics.findLoan(complete->dependencies.front().parent)
            : nullptr;
    const auto *parent =
        group ? valid.semantics.findLoan(group->parent) : nullptr;
    expect(group && group->kind == lang::SemanticLoanKind::StoredValue &&
               group->lifecycleBound && place &&
               group->designatedFields == place->projections && parent &&
               parent->entry &&
               parent->kind == lang::SemanticLoanKind::Reference &&
               parent->access == lang::AccessMode::ReadOnly &&
               target->completeReturnBorrow &&
               target->completeReturnBorrow->dependencies.size() == 1 &&
               target->completeReturnBorrow->dependencies.front().origin ==
                   lang::BorrowOriginKind::Argument &&
               target->completeReturnBorrow->dependencies.front().parameter ==
                   0,
           "the field read keeps its cleanup-bound Stored parent while the "
           "return retains only the selected external supplier");
    if (std::string_view(name) == "inherited")
      expect(group && group->additionalDependencies.size() == 1 &&
                 group->additionalDependencies.front().parent == 0 &&
                 group->additionalDependencies.front().place !=
                     complete->dependencies.front().place,
             "an unrelated local sibling remains in the carrier cleanup group "
             "without becoming a returned supplier");
  }
  for (
      const auto source :
      {R"gti(int&<static> invalid() { int local = 7; View view{local}; return view.value; })gti",
       R"gti(struct Owner { int value = 7; ~Owner() {} }; int&<static> invalid() { Owner owner{}; View view{owner.value}; return view.value; })gti",
       R"gti(struct Owner { int value = 7; ~Owner() {} }; int&<static> invalid() { Owner owner{}; return owner.value; })gti",
       R"gti(struct Exclusive<lifetime a> { mut int&<a> value; Exclusive(mut int&<a> input) : value(input) {} ~Exclusive() {} }; int&<a> invalid<lifetime a>(mut int&<a> input) { Exclusive view{input}; return view.value; })gti",
       R"gti(int&<longer> invalid<lifetime longer, lifetime shorter>(int&<longer> input) requires outlives<longer, shorter> { View<shorter> view{input}; return view.value; })gti"}) {
    const auto rejected = check(source);
    expect(rejected.syntaxValid && !rejected.semanticValid,
           "cleanup copying cannot extend local/owned storage, detach an "
           "exclusive parent, or recover a shortened field's longer input "
           "lifetime");
  }
  const auto beforeDefinition = check(R"gti(
int&<static> later();
int use() { return later(); }
int&<static> later() { View<static> view{global}; return view.value; }
)gti");
  expectValid(beforeDefinition,
              "an available source definition supplies its checked static "
              "origin through the existing body query");
  const auto generic =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Hir})
          .analyze("generic-static-reference-cleanup.gti", declarations + R"gti(
int&<static> selected<T>(T unused) { View<static> view{global}; return view.value; }
int main() { return selected(1) + selected(true) - 14; }
)gti");
  expectValid(generic, "concrete generic instances retain the complete source "
                       "body's exact static global supplier");
  expect(generic.hir.valid(), "generic static supplier contracts remain "
                              "coherent through concrete HIR lowering");
  if (const auto *selected = function(generic, "selected")) {
    std::size_t instances = 0;
    for (const auto &instance : generic.hir.functionInstances())
      if (instance.declaration == selected->id) {
        ++instances;
        expect(instance.returnBorrowOrigin == lang::BorrowOriginKind::Global &&
                   instance.returnBorrowPlace == selected->returnBorrowPlace &&
                   instance.completeReturnBorrow ==
                       selected->completeReturnBorrow,
               "each applied type preserves the same compact and complete "
               "global return identity");
      }
    expect(instances == 2, "both independent concrete type substitutions "
                           "exercise static supplier refresh");
  }
  for (
      const auto source :
      {R"gti(int&<static> unavailable();)gti",
       R"gti(int&<static> invalid(bool first) { View<static> left{global}; View<static> right{otherGlobal}; if (first) { return left.value; } return right.value; })gti",
       R"gti(int&<static> invalid() { return unknownValue; })gti",
       R"gti(int&<static> invalid() { return invalid(); })gti",
       R"gti(int&<static> invalid<T>(T unused, bool first) { if (first) { return global; } return otherGlobal; } int main() { return invalid(1, true); })gti",
       R"gti(int&<static> invalid<T>(T unused) { return unknownValue; } int main() { return invalid(1); })gti"}) {
    const auto rejected = check(source);
    expect(rejected.syntaxValid && !rejected.semanticValid,
           "a static result does not invent origin authority for absent "
           "definitions, differing globals, unknown bodies or recursive "
           "summaries");
  }
}

void testStoredFieldReturnRegions() {
  const std::string declarations = R"gti(
struct View<lifetime a> {
  int&<a> value;
  View(int&<a> input) : value(input) {}
};
struct Derived<lifetime a> : public View<a> {
  Derived(int&<a> input) : View<a>(input) {}
};
)gti";
  const auto check = [&](std::string body) {
    return lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics})
        .analyze("stored-field-return-regions.gti", declarations + body);
  };
  const auto inferred = check(R"gti(
int&<a> external<lifetime a>(int&<a> input) {
  View view{input};
  return view.value;
}
int&<a> inherited<lifetime a>(int&<a> input) {
  Derived view{input};
  return view.value;
}
int main() { int input = 7; return external(input) + inherited(input) - 14; }
)gti");
  expectValid(inferred, "inferred direct and inherited stored-field regions "
                        "satisfy the exact returned region");
  for (const auto name : {"external", "inherited"}) {
    const auto *target = function(inferred, name);
    const auto *body = target && target->declaration
                           ? target->declaration->body().get()
                           : nullptr;
    const auto *local =
        body && !body->statements().empty()
            ? lang::stmtAs<lang::VariableDecl>(body->statements().front().get())
            : nullptr;
    const auto *binding =
        local ? inferred.semantics.findBinding(*local) : nullptr;
    const auto *lifetimes =
        body ? inferred.semantics.findLifetimeBody(*body) : nullptr;
    const auto field = binding && binding->type.lifetimeArguments.size() == 1
                           ? binding->type.lifetimeArguments.front()
                           : lang::Lifetime{};
    const auto result =
        target ? target->returnType.referenceLifetime : lang::Lifetime{};
    expect(field.specified() && result.specified() && lifetimes &&
               std::find(lifetimes->obligations.begin(),
                         lifetimes->obligations.end(),
                         lang::LifetimeBound{field, result}) !=
                   lifetimes->obligations.end(),
           "a stored-field return publishes its instantiated field region "
           "edge, not only its original constructor owner");
  }
  for (const auto type : {"View", "Derived"}) {
    const auto shortened = check(std::string(R"gti(
int&<longer> invalid<lifetime longer, lifetime shorter>(int&<longer> input)
    requires outlives<longer, shorter> {
  )gti") + type + R"gti(<shorter> view{input};
  return view.value;
}
)gti");
    expect(!shortened.semanticValid,
           "an explicitly shortened direct or inherited field cannot recover "
           "its constructor input's longer region");
  }
  const auto bounded = check(R"gti(
int&<shorter> selected<lifetime longer, lifetime shorter>(int&<longer> input)
    requires outlives<longer, shorter> {
  View<shorter> view{input};
  return view.value;
}
)gti");
  expectValid(bounded, "a stored field remains usable within its explicitly "
                       "declared shorter region");
}

void testComposedOwnedReturnFields() {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto check = [&](const std::string &source) {
    return lang::Frontend({.stopAfter = lang::FrontendPhase::Hir})
        .analyze("composed-owned-return.gti", source, {root / "prelude.gti"},
                 {}, {root});
  };
  const std::string pair = R"gti(
import std.memory;
struct Pair<lifetime a, lifetime b> {
  int&<a> left; int&<b> right;
  Pair(int&<a> first,int&<b> second):left(first),right(second) {}
};
)gti";
  const std::string box = pair + R"gti(
struct Box<T> { T value; Box(T input):value(std::move(input)) {} };
)gti";
  const std::string relay = R"gti(
Box<T> relay<T>(T input) { return Box<T>(std::move(input)); }
)gti";
  const std::string main = R"gti(
int main() {
  int left=4; int right=9; Pair pair{left,right};
  auto result=relay(std::move(pair));
  return result.value.left*10+result.value.right-49;
}
)gti";
  const std::vector<std::pair<std::string, std::string>> valid{
      {"record", box + relay + main},
      {"annotated", box + "[[borrowed_from(input)]] " + relay + main},
      {"callback", R"gti(
import std.memory;
class Holder<F> {
  mut F callable;
public:
  Holder(F input):callable(std::move(input)) {}
  int run() mut { return this.callable(); }
};
Holder<F> relay<F>(F input) { return Holder<F>(std::move(input)); }
int main() {
  int left=4; int right=9;
  auto callback=[&left,&right]()->int { return left*10+right; };
  mut auto result=relay(std::move(callback));
  return result.run()-49;
}
)gti"}};
  for (const auto &[name, source] : valid) {
    const auto result = check(source);
    expectValid(result, "an owned result layout preserves all external fields "
                        "of its selected input: " +
                            name);
    bool complete = false;
    for (const auto &instance : result.hir.functionInstances())
      if (instance.source && instance.source->name().lexeme == "relay" &&
          instance.completeReturnBorrow &&
          instance.completeReturnBorrow->dependencies.size() == 2)
        complete = std::all_of(
            instance.completeReturnBorrow->dependencies.begin(),
            instance.completeReturnBorrow->dependencies.end(),
            [](const auto &dependency) {
              return dependency.origin == lang::BorrowOriginKind::Argument &&
                     dependency.parameter == 0 &&
                     !dependency.sourceFields.empty() &&
                     !dependency.resultFields.empty() &&
                     dependency.access == lang::AccessMode::ReadOnly;
            });
    expect(result.hirValid && complete,
           "a concrete relay publishes both typed external suppliers without "
           "the consumed shell");
  }
  const std::vector<std::pair<std::string, std::string>> invalid{
      {"first-owner-write", box + relay + R"gti(
int main() { mut int left=4; mut int right=9; Pair pair{left,right};
  auto result=relay(std::move(pair)); left=5; return result.value.left+result.value.right; }
)gti"},
      {"second-owner-write", box + relay + R"gti(
int main() { mut int left=4; mut int right=9; Pair pair{left,right};
  auto result=relay(std::move(pair)); right=5; return result.value.left+result.value.right; }
)gti"},
      {"local-static-escape", box + R"gti(
Box<Pair<static,static>> invalid() { int left=4; int right=9; Pair pair{left,right};
  return Box<Pair<static,static>>(std::move(pair)); }
int main() { auto result=invalid(); return result.value.left+result.value.right; }
)gti"},
      {"consumed-shell-escape", pair + R"gti(
struct Box<T> { T& value; Box(T& input):value(input) {} };
Box<T> invalid<T>(T input) { return Box<T>(input); }
int main() { int left=4; int right=9; Pair pair{left,right};
  auto result=invalid(std::move(pair)); return result.value.left+result.value.right; }
)gti"},
      {"different-annotated-input", box + R"gti(
[[borrowed_from(input)]] Box<T> invalid<T>(T input,T other) {
  return Box<T>(std::move(other));
}
int main() { int left=4; int right=9; Pair first{left,right}; Pair other{left,right};
  auto result=invalid(std::move(first),std::move(other)); return result.value.left+result.value.right; }
)gti"}};
  for (const auto &[name, source] : invalid) {
    const auto result = check(source);
    expect(
        result.sourceValid && result.syntaxValid && !result.semanticValid,
        "complete owned return mapping rejects invalid source permissions: " +
            name);
  }
}

void testSemanticReferenceSupplierQueries() {
  const auto source =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics})
          .analyze("semantic-reference-supplier-query.gti", R"gti(
struct Leaf<lifetime a> { int&<a> external; Leaf(int&<a> input) : external(input) {} };
struct Holder<lifetime a, lifetime owner> {
  Leaf<a>&<owner> value;
  Holder(Leaf<a>&<owner> input) : value(input) {}
};
)gti");
  expectValid(source, "direct reference storage remains valid independently of "
                      "transitive supplier queries");
  const auto query =
      lang::SemanticLifetimeTestAccess::referenceSuppliers(source, "Holder");
  expect(
      query && query->status == lang::ReferenceSupplierQueryStatus::Complete &&
          query->paths.size() == 2 && query->paths[0].fields.size() == 1 &&
          query->paths[0].crossings.empty() &&
          query->paths[1].fields.size() == 2 &&
          query->paths[1].crossings.size() == 1 &&
          query->paths[1].crossings.front().fields == query->paths[0].fields &&
          query->paths[1].crossings.front().reference ==
              query->paths[0].reference &&
          query->paths[0].reference.referenceLifetime !=
              query->paths[1].reference.referenceLifetime,
      "the semantic adapter keeps exact declared outer-reference and inner "
      "supplier regions as separate paths");
}

void testStagedReferenceContentsAdapters() {
  const auto source =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics})
          .analyze("staged-reference-contents-adapters.gti", R"gti(
mut int effectCounter = 0;
int writeGlobal() { effectCounter++; return effectCounter; }
int writeRaw(mut int* target) { unsafe { *target = 1; } return 0; }
struct Effect {
  mut int value;
  Effect(int input) : value(input) {}
  int read() { return this.value; }
  int update() mut { this.value++; return this.value; }
  int operator+(int other) { return this.value + other; }
};
using NativeEffect = () -> int;
int effect_mutable(mut Effect& target) { return target.update(); }
int effect_global() { return writeGlobal(); }
int effect_raw(mut int* target) { return writeRaw(target); }
int effect_unknown(NativeEffect target) { unsafe { return target(); } }
int effect_readonly(Effect& target) { return target.read(); }
int effect_operator(mut Effect& target) { return target + 1; }
Effect effect_construction() { return Effect(1); }
int effect_alias() { return writeGlobal(); }
struct Leaf<lifetime a> {
  int&<a> external;
  Leaf(int&<a> input) : external(input) {}
  int&<a> get() { return this.external; }
  int&<a> operator()() { return this.external; }
};
struct Holder<lifetime a, lifetime owner> {
  Leaf<a>&<owner> value;
  Holder(Leaf<a>&<owner> input) : value(input) {}
};
int direct<lifetime a, lifetime owner, lifetime shell>(Holder<a, owner>&<shell> holder) {
  return holder.value.external;
}
int forwarded<lifetime a, lifetime owner, lifetime shell>(Holder<a, owner>&<shell> holder) {
  return holder.value.get();
}
int callback<lifetime a, lifetime owner, lifetime shell>(Holder<a, owner>&<shell> holder) {
  return holder.value();
}
struct Exclusive<lifetime a> {
  mut int&<a> external;
  Exclusive(mut int&<a> input) : external(input) {}
};
enum Choice<lifetime a> { item(int&<a> value), empty(int code) };
struct Reference<T, lifetime owner> {
  T&<owner> value;
  Reference(T&<owner> input) : value(input) {}
};
Exclusive<a>&<owner> exclusive<lifetime a, lifetime owner, lifetime shell>(
    Reference<Exclusive<a>, owner>&<shell> holder) { return holder.value; }
Choice<a>&<owner> conditional<lifetime a, lifetime owner, lifetime shell>(
    Reference<Choice<a>, owner>&<shell> holder) { return holder.value; }
)gti");
  expectValid(source, "staged shared contents adapters start from checked "
                      "direct, forwarded and typed-callback reads");
  const auto checks =
      lang::SemanticLifetimeTestAccess::referenceContentsAdapters(source);
  if (checks.formed != 3 || checks.normalized != 3 ||
      checks.addressPreserved != 3 || checks.staleRejected != 3 ||
      checks.missingReadRejected != 3 || checks.cleanupReadRejected != 3 ||
      checks.staleCallRejected != 2 || checks.unsupportedRejected != 2 ||
      checks.deltaIsolated != 3 || !checks.entryReused ||
      checks.callInvalidated != 21 || checks.noncallPreserved != 3 ||
      checks.aliasPreserved != 3 || checks.reevaluationReset != 3)
    std::cerr << "staged contents adapters formed=" << checks.formed
              << " normalized=" << checks.normalized
              << " address=" << checks.addressPreserved
              << " stale=" << checks.staleRejected
              << " inactive=" << checks.missingReadRejected
              << " cleanup=" << checks.cleanupReadRejected
              << " staleCall=" << checks.staleCallRejected
              << " unsupported=" << checks.unsupportedRejected
              << " delta=" << checks.deltaIsolated
              << " entry=" << checks.entryReused
              << " calls=" << checks.callInvalidated
              << " noncall=" << checks.noncallPreserved
              << " alias=" << checks.aliasPreserved
              << " reevaluation=" << checks.reevaluationReset << '\n';
  expect(checks.formed == 3 && checks.normalized == 3,
         "direct fields, method receivers and typed callback receivers share "
         "one exact opening and formal supplier normalization");
  expect(checks.addressPreserved == 3 && checks.entryReused,
         "contents preparation preserves scalar address facts and reuses "
         "independently checked entry Stored authority");
  expect(checks.staleRejected == 3 && checks.missingReadRejected == 3 &&
             checks.cleanupReadRejected == 3 && checks.staleCallRejected == 2,
         "unavailable or cleanup-bound reads cannot form contents and "
         "invalidated calls cannot fall back to an address fact");
  expect(checks.unsupportedRejected == 2,
         "formation needs retained exclusive permission or evaluated "
         "conditional selection before opening those contents");
  expect(checks.deltaIsolated == 3,
         "contents indices stay absent without openings, concrete instances "
         "isolate IDs and speculation erases only its copied index");
  expect(checks.callInvalidated == 21 && checks.noncallPreserved == 3 &&
             checks.aliasPreserved == 3,
         "call effects invalidate current contents across mutable, global, raw "
         "and unknown targets without repeating aliases or retiring captured "
         "snapshots");
  expect(checks.reevaluationReset == 3,
         "re-evaluating an AST clears the old contents index and forms a new "
         "checked snapshot without changing the previous copy");
}

void testStagedReferenceContentsSources() {
  const auto source =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics})
          .analyze("staged-reference-contents-sources.gti", R"gti(
struct Leaf<lifetime a> { int&<a> external; Leaf(int&<a> input) : external(input) {} };
struct Holder<lifetime a, lifetime owner> {
  Leaf<a>&<owner> value;
  Holder(Leaf<a>&<owner> input) : value(input) {}
};
int read<lifetime a, lifetime owner, lifetime shell>(Holder<a, owner>&<shell> holder) {
  return holder.value.external + holder.value.external;
}
Leaf<a>&<owner> choose<lifetime a, lifetime owner>(Leaf<a>&<owner> input) { return input; }
int indirect<lifetime a, lifetime owner>(Leaf<a>&<owner> input) { return choose(input).external; }
)gti");
  expectValid(
      source,
      "staged contents checks use already checked ordinary reference reads");
  const auto checks =
      lang::SemanticLifetimeTestAccess::referenceContentsChecks(source);
  expect(checks.selectors, "separate reads of one checked reference slot "
                           "derive the same exact selector");
  expect(checks.normalized, "a tagged contents supplier normalizes through its "
                            "actual formal selector");
  expect(
      checks.aliasing,
      "different opening IDs never prove shared external referents disjoint");
  expect(checks.immutableSnapshot, "slot invalidation forgets current contents "
                                   "while preserving copied shared authority");
  expect(checks.malformed, "mixed sources, wrong fields, exclusive copies and "
                           "forged read/cleanup authority reject");
  expect(checks.callOwnerRejected, "a call's lifetime owner is not fabricated "
                                   "into its actual reference selector");
  const auto inherited =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics})
          .analyze("staged-inherited-reference-contents.gti", R"gti(
struct Leaf<lifetime a> { int&<a> external; Leaf(int&<a> input) : external(input) {} };
struct Storage<lifetime a, lifetime owner> {
  Leaf<a>&<owner> value;
  Storage(Leaf<a>&<owner> input) : value(input) {}
};
struct Holder<lifetime a, lifetime owner> : public Storage<a, owner> {
  Holder(Leaf<a>&<owner> input) : Storage<a, owner>(input) {}
};
int read<lifetime a, lifetime owner, lifetime shell>(Holder<a, owner>&<shell> holder) {
  return holder.value.external + holder.value.external;
}
Leaf<a>&<owner> choose<lifetime a, lifetime owner>(Leaf<a>&<owner> input) { return input; }
int indirect<lifetime a, lifetime owner>(Leaf<a>&<owner> input) { return choose(input).external; }
)gti");
  expectValid(inherited, "staged inherited contents queries use the exact "
                         "instantiated storage-base view");
  const auto inheritedChecks =
      lang::SemanticLifetimeTestAccess::referenceContentsChecks(inherited, 2);
  expect(inheritedChecks.selectors && inheritedChecks.normalized &&
             inheritedChecks.immutableSnapshot && inheritedChecks.aliasing &&
             inheritedChecks.malformed && inheritedChecks.callOwnerRejected,
         "an inherited selector preserves its typed Base prefix, independent "
         "regions and the same read/alias/cleanup proof gates");
}
} // namespace

int main() {
  testAnonymousStructuralDestinationRegions();
  testStructuralSpecialMemberPolicies();
  testSharedStoredReferenceCleanupCopy();
  testStagedReferenceContentsAdapters();
  testStagedReferenceContentsSources();
  testSemanticReferenceSupplierQueries();
  testStoredFieldReturnRegions();
  testForwardedPackBorrowPaths();
  testBorrowedFromPackOrdinals();
  testScopedFunctionBorrowQuery();
  testReferencePackReturnContracts();
  testAliasConstructionLifetimeApplications();
  testReceiverBeforeSymbolicPack();
  testPhysicalReferenceCarrierInitializers();
  testCopiedBorrowedCaptureOwners();
  testComposedOwnedReturnFields();
  testOwnedCallableAppliedRegions();
  testDependentCallableUseEvidence();
  testStatelessItemTemporaryBorrow();
  testScopedCallableResultProjectionContracts();
  testCheckedProjectedCallableResults();
  testFreshCallableLifetimeArguments();
  testReferencePackReborrows();
  testSymbolicCallablePackReturns();
  testConsumingExternalResults();
  testFreshReceiverStorageRegions();
  testEvaluatedReferenceReceiverRegions();
  testFixedArityPackCallSelection();
  testSelectedBaseConstructorInputs();
  testCompleteOrdinaryConstructorInputs();
  testConstructorRelationsAfterFieldIdentity();
  testOwnedPointeeLifetimeAuthority();
  testCheckedReferenceErasureLifetimeAuthority();
  testNominalArguments();
  testTransparentAliases();
  testHigherRankedAlias();
  testScopedNominalCallableFormation();
  testAssociatedLifetimeFamily();
  testBinderDiagnostics();
  testProtocolOperationBinders();
  testExplicitFunctionKinds();
  testAnonymousNominalRegions();
  testNominalVariance();
  testSpecializationLifetimeShape();
  testExecutableLifetimeIdentity();
  testCallablePatternScopes();
  testCallablePatternLambdaScopes();
  testStructuralDefaultCallableScopes();
  testInferredCarrierRegions();
  testProjectedPatternLifetimes();
  testEnumInitializerLifetimeComposition();
  testProjectedOnlyPatternLifetimes();
  testNamedProjectedPatternLifetimes();
  testNominalFormation();
  testCStringRegions();
  testNativeInvocationLifetimes();
  testPrivateLifetimeInference();
  testPrivateLifetimeWorklist();
  testInstantiatedReferenceIdentity();
  testLambdaLifetimeInference();
  testLambdaLifetimeEnvironments();
  testLambdaEnvironmentPremises();
  return failures == 0 ? 0 : 1;
}
