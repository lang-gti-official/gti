#include "semantic_analyzer_impl.h"

namespace lang {

namespace {
std::optional<ReceiverMutability>
signatureClassReceiver(const ClassDecl &owner) {
  const auto &patterns = owner.genericArgumentPatterns();
  if (patterns.size() != 1 || !patterns.front().isFunctionSignature() ||
      owner.isForwardDeclaration() ||
      (owner.kind() != ClassKind::Class && owner.kind() != ClassKind::Struct)) {
    return std::nullopt;
  }
  const auto &qualifier = patterns.front().functionSignatureQualifier;
  return !qualifier                   ? ReceiverMutability::ReadOnly
         : qualifier->lexeme == "mut" ? ReceiverMutability::Mutable
                                      : ReceiverMutability::Consuming;
}
} // namespace

auto SemanticVisitor::Impl::collectRootNativeStorageSymbols(
    const StmtList &statements) -> void {
  for (const StmtPtr &statement : statements) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        collectRootNativeStorageSymbols(*branch);
      }
      continue;
    }
    if (const auto *variable = stmtAs<VariableDecl>(statement.get())) {
      rootNativeStorageSymbols.emplace(variable->name().lexeme, variable);
    }
  }
}

auto SemanticVisitor::Impl::registerGlobalBorrowStorage(
    const StmtList &statements, std::vector<std::string> scope) -> void {
  for (const StmtPtr &statement : statements) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        registerGlobalBorrowStorage(*branch, scope);
      }
      continue;
    }
    if (const auto *externC = stmtAs<ExternCDecl>(statement.get())) {
      registerGlobalBorrowStorage(externC->declarations(), scope);
      continue;
    }
    if (const auto *namespaceDecl = stmtAs<NamespaceDecl>(statement.get())) {
      scope.emplace_back(namespaceDecl->name().lexeme);
      registerGlobalBorrowStorage(namespaceDecl->declarations(), scope);
      scope.pop_back();
      continue;
    }
    const auto *variable = stmtAs<VariableDecl>(statement.get());
    if (variable == nullptr) {
      continue;
    }

    const SourceUnitId sourceUnit = sourceUnitFor(variable->name());
    const SemanticType type = typeOf(
        variable->type(),
        variable->isMutable() ? Mutability::Mutable : Mutability::Immutable,
        scope);
    const std::string name = qualifiedName(scope, variable->name().lexeme);
    SymbolId symbol = symbolForDeclaration(variable->name());
    if (symbol == 0) {
      symbol = recordToolingSymbol(variable->name(), SymbolKind::GlobalVariable,
                                   name, type, variable->isMutable(), true,
                                   AccessModifier::Public, false,
                                   variable->isStatic());
    }
    globalBorrowStorage[name].push_back(
        {.declaration = variable,
         .sourceUnit = sourceUnit,
         .type = type,
         .symbol = symbol,
         .internalLinkage = variable->isStatic()});
  }
}

auto SemanticVisitor::Impl::findGlobalBorrowStorage(
    std::string_view qualified) const -> const GlobalBorrowStorageInfo * {
  const auto found = globalBorrowStorage.find(std::string(qualified));
  if (found == globalBorrowStorage.end()) {
    return nullptr;
  }
  const GlobalBorrowStorageInfo *result = nullptr;
  for (const GlobalBorrowStorageInfo &candidate : found->second) {
    const bool visible =
        candidate.internalLinkage
            ? candidate.sourceUnit == currentSourceUnit
            : declarationVisible(candidate.sourceUnit,
                                 candidate.declaration->name());
    if (!visible || candidate.symbol == 0) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = &candidate;
  }
  return result;
}

auto SemanticVisitor::Impl::resolveGlobalBorrowStorage(
    const ExprPtr &expression) const -> const GlobalBorrowStorageInfo * {
  if (!expression) {
    return nullptr;
  }
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    return resolveGlobalBorrowStorage(grouping->expression());
  }
  if (const auto *variable = exprAs<Variable>(expression.get())) {
    for (std::size_t depth = currentNamespace.size() + 1; depth > 0; --depth) {
      if (const GlobalBorrowStorageInfo *storage =
              findGlobalBorrowStorage(qualifiedName(currentNamespace, depth - 1,
                                                    variable->name().lexeme))) {
        return storage;
      }
    }
    return nullptr;
  }
  const auto *qualified = exprAs<QualifiedName>(expression.get());
  if (qualified == nullptr || qualified->name().segments.size() < 2) {
    return nullptr;
  }
  const NamePath namespacePath(
      std::vector<Token>(qualified->name().segments.begin(),
                         qualified->name().segments.end() - 1));
  const std::optional<std::string> resolvedNamespace =
      resolveNamespacePath(namespacePath, currentNamespace);
  return resolvedNamespace
             ? findGlobalBorrowStorage(*resolvedNamespace +
                                       "::" + qualified->name().last().lexeme)
             : nullptr;
}

auto SemanticVisitor::Impl::predeclaredGlobalBorrowPlace(
    const ExprPtr &expression) const -> std::optional<BorrowOriginPlace> {
  if (!expression) {
    return std::nullopt;
  }
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    return predeclaredGlobalBorrowPlace(grouping->expression());
  }
  if (const auto *unary = exprAs<Unary>(expression.get());
      unary != nullptr && unary->oper().kind == TokenKind::STAR) {
    std::optional<BorrowOriginPlace> result =
        predeclaredGlobalBorrowPlace(unary->right());
    if (result) {
      result->projections.push_back({.kind = PlaceProjectionKind::Dereference});
    }
    return result;
  }
  const GlobalBorrowStorageInfo *storage =
      resolveGlobalBorrowStorage(expression);
  return storage == nullptr ? std::nullopt
                            : std::optional<BorrowOriginPlace>(
                                  BorrowOriginPlace{.root = storage->symbol});
}

auto SemanticVisitor::Impl::sourceUnitFor(const Token &token) const
    -> SourceUnitId {
  return sourceGraph == nullptr ? 0
                                : sourceGraph->sourceUnitForPath(token.source);
}

bool SemanticVisitor::Impl::declarationVisible(SourceUnitId declaration,
                                               const Token &name) const {
  return sourceGraph == nullptr ||
         sourceGraph->isDeclarationVisible(currentSourceUnit, declaration,
                                           name.position);
}

void SemanticVisitor::Impl::validateModuleExports(const StmtList &statements,
                                                  bool inherited) {
  const auto reject = [&](const Token &token, std::string message) {
    diagnostics.push_back(
        makeDiagnostic(DiagnosticId::InvalidModuleExport, token, {message}));
  };
  for (const StmtPtr &statement : statements) {
    if (!statement) {
      continue;
    }
    const bool exported = inherited || statement->exportKeyword().has_value();
    if (statement->exportKeyword()) {
      const Token &keyword = *statement->exportKeyword();
      const SourceUnit *unit =
          sourceGraph == nullptr
              ? nullptr
              : sourceGraph->findUnit(sourceUnitFor(keyword));
      if (unit == nullptr || !unit->moduleDeclaration) {
        reject(keyword, "An exported declaration requires an 'export module "
                        "name;' interface.");
      }
    }
    if (const auto *space = stmtAs<NamespaceDecl>(statement.get())) {
      validateModuleExports(space->declarations(), exported);
      continue;
    }
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (statement->exportKeyword()) {
        reject(*statement->exportKeyword(),
               "Export declarations inside a conditional branch, not the "
               "conditional directive itself.");
      }
      if (const StmtList *branch = conditional->activeBranch(target)) {
        validateModuleExports(*branch, inherited);
      }
      continue;
    }
    if (const auto *linkage = stmtAs<ExternCDecl>(statement.get())) {
      validateModuleExports(linkage->declarations(), exported);
      continue;
    }
    if (!exported) {
      continue;
    }
    const Token *name = nullptr;
    bool internal = false;
    if (const auto *function = stmtAs<FunctionDecl>(statement.get())) {
      name = &function->name();
      internal = function->isStatic();
    } else if (const auto *variable = stmtAs<VariableDecl>(statement.get())) {
      name = &variable->name();
      internal = variable->isStatic();
    } else if (const auto *type = stmtAs<ClassDecl>(statement.get())) {
      name = &type->name();
    } else if (const auto *type = stmtAs<EnumDecl>(statement.get())) {
      name = &type->name();
    } else if (const auto *alias = stmtAs<TypeAliasDecl>(statement.get())) {
      name = &alias->name();
    } else if (const auto *alias =
                   stmtAs<NamespaceAliasDecl>(statement.get())) {
      name = &alias->name();
    } else if (const auto *conceptDecl = stmtAs<ConceptDecl>(statement.get())) {
      name = &conceptDecl->name();
    }
    if (internal && name != nullptr) {
      reject(*name, "A source-local static declaration cannot be exported.");
    } else if (name == nullptr && statement->exportKeyword()) {
      reject(*statement->exportKeyword(),
             "Only namespace-scope API declarations can be exported.");
    }
  }
}

auto SemanticVisitor::Impl::isCompilerTrustedUnit(SourceUnitId sourceUnit) const
    -> bool {
  return sourceGraph != nullptr && sourceGraph->isCompilerTrusted(sourceUnit);
}

auto SemanticVisitor::Impl::isCompilerTrustedRequester(const Token &use) const
    -> bool {
  const SourceUnitId requester = sourceUnitFor(use);
  return isCompilerTrustedUnit(requester == 0 ? currentSourceUnit : requester);
}

auto SemanticVisitor::Impl::isCompilerPrivateQualifiedName(
    std::string_view name) -> bool {
  constexpr std::string_view root = "gti_internal";
  return name == root ||
         (name.starts_with(root) && name.size() > root.size() + 1 &&
          name.substr(root.size(), 2) == "::");
}

auto SemanticVisitor::Impl::isCompilerPrivateScope(
    const std::vector<std::string> &scope) -> bool {
  return !scope.empty() && scope.front() == "gti_internal";
}

auto SemanticVisitor::Impl::compilerPrivateDeclaration(
    SourceUnitId sourceUnit, const std::vector<std::string> &scope) const
    -> bool {
  (void)sourceUnit;
  return isCompilerPrivateScope(scope);
}

auto SemanticVisitor::Impl::compilerPrivatePathDenied(
    const NamePath &path) const -> bool {
  if (path.segments.empty() ||
      isCompilerTrustedRequester(path.segments.front())) {
    return false;
  }
  if (path.segments.front().lexeme == "gti_internal") {
    return true;
  }
  SourceUnitId requester = sourceUnitFor(path.segments.front());
  if (requester == 0) {
    requester = currentSourceUnit;
  }
  const auto unitAliases = visibleNamespaceAliases.find(requester);
  if (unitAliases == visibleNamespaceAliases.end()) {
    return false;
  }
  for (std::size_t depth = currentNamespace.size() + 1; depth > 0; --depth) {
    const auto alias = unitAliases->second.find(qualifiedName(
        currentNamespace, depth - 1, path.segments.front().lexeme));
    if (alias != unitAliases->second.end()) {
      return alias->second.compilerPrivate;
    }
  }
  return false;
}

auto SemanticVisitor::Impl::reportCompilerPrivatePath(const NamePath &path)
    -> bool {
  if (!compilerPrivatePathDenied(path)) {
    return false;
  }
  const Token &location = path.segments.front();
  reportCompilerPrivateAccess(location, pathSpelling(path));
  return true;
}

auto SemanticVisitor::Impl::reportCompilerPrivateAccess(const Token &location,
                                                        std::string name)
    -> void {
  const std::string key =
      location.source + ':' + std::to_string(location.position);
  if (reportedCompilerPrivateAccess.insert(key).second) {
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2058", DiagnosticPhase::Semantics, location,
                       "Compiler-private name '" + std::move(name) +
                           "' is unavailable to application source.");
    if (compilerPrivateNamespaceSymbol != 0) {
      if (const SymbolRecord *declaration = semanticModel.database().findSymbol(
              compilerPrivateNamespaceSymbol)) {
        diagnostic.related.push_back(
            {declaration->declarationSpan,
             "The trusted prelude owns this private namespace."});
      }
    }
    diagnostic.hints.emplace_back(
        "Use the public std facility that wraps this compiler capability.");
    diagnostics.emplace_back(std::move(diagnostic));
  }
}

auto SemanticVisitor::Impl::reportCompilerPrivateDeclaration(const Token &name)
    -> void {
  const std::string key = name.source + ':' + std::to_string(name.position);
  if (!reportedCompilerPrivateAccess.insert(key).second) {
    return;
  }
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2058", DiagnosticPhase::Semantics, name,
      "Namespace 'gti_internal' is reserved for compiler-trusted "
      "standard-library units.");
  diagnostic.hints.emplace_back(
      "Declare application APIs outside gti_internal and use public std "
      "wrappers for compiler capabilities.");
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::publishNamespace(const std::string &name,
                                             SourceUnitId declaration,
                                             const Token &token) -> void {
  forEachSourceConsumer(
      declaration, isCompilerPrivateQualifiedName(name), token.position,
      [&](SourceUnitId consumer) { visibleNamespaces[consumer].insert(name); });
}

auto SemanticVisitor::Impl::publishNamespaceAlias(
    const std::string &name, const NamespaceAliasInfo &alias,
    const Token &token) -> void {
  forEachSourceConsumer(alias.sourceUnit, alias.compilerPrivate, token.position,
                        [&](SourceUnitId consumer) {
                          visibleNamespaceAliases[consumer].insert_or_assign(
                              name, alias);
                        });
}

auto SemanticVisitor::Impl::publishTypeAlias(const std::string &name,
                                             TypeAliasId id,
                                             SourceUnitId declaration,
                                             bool compilerPrivate) -> void {
  forEachSourceConsumer(
      declaration, compilerPrivate || isCompilerPrivateQualifiedName(name),
      typeAliases[id - 1].declaration->name().position,
      [&](SourceUnitId consumer) {
        visibleTypeAliasIds[consumer].insert_or_assign(name, id);
      });
}

auto SemanticVisitor::Impl::publishConcept(const std::string &name,
                                           ConceptId id,
                                           SourceUnitId declaration,
                                           const Token &token) -> void {
  forEachSourceConsumer(declaration, isCompilerPrivateQualifiedName(name),
                        token.position, [&](SourceUnitId consumer) {
                          visibleConceptIds[consumer].insert_or_assign(name,
                                                                       id);
                        });
}

auto SemanticVisitor::Impl::publishClass(const std::string &name, ClassId id,
                                         SourceUnitId declaration,
                                         const Token &token) -> void {
  forEachSourceConsumer(declaration, isCompilerPrivateQualifiedName(name),
                        token.position, [&](SourceUnitId consumer) {
                          visibleClassIds[consumer].insert_or_assign(name, id);
                        });
}

auto SemanticVisitor::Impl::publishEnum(const std::string &name, EnumId id,
                                        SourceUnitId declaration,
                                        const Token &token) -> void {
  forEachSourceConsumer(declaration, isCompilerPrivateQualifiedName(name),
                        token.position, [&](SourceUnitId consumer) {
                          visibleEnumIds[consumer].insert_or_assign(name, id);
                        });
}

auto SemanticVisitor::Impl::activeNamespaceSymbols()
    -> std::unordered_map<std::string, Symbol> & {
  return catalog.sealed ? valueFlow.namespaceSymbols : catalog.namespaceSymbols;
}

auto SemanticVisitor::Impl::activeNamespaceSymbols() const
    -> const std::unordered_map<std::string, Symbol> & {
  return catalog.sealed ? valueFlow.namespaceSymbols : catalog.namespaceSymbols;
}

auto SemanticVisitor::Impl::publishNamespaceSymbol(const std::string &name,
                                                   const Symbol &symbol)
    -> void {
  if (symbol.compilerPrivate && !isCompilerTrustedUnit(symbol.sourceUnit) &&
      symbol.type != SemanticType::Function) {
    return;
  }
  if (symbol.internalLinkage && sourceGraph != nullptr &&
      symbol.sourceUnit != 0) {
    activeVisibleNamespaceSymbols()[symbol.sourceUnit].insert_or_assign(name,
                                                                        symbol);
    return;
  }
  forEachSourceConsumer(
      symbol.sourceUnit, symbol.compilerPrivate, symbol.declaration.position,
      [&](SourceUnitId consumer) {
        Symbol published = symbol;
        if (published.type == SemanticType::Function &&
            sourceGraph != nullptr) {
          std::erase_if(published.overloads,
                        [&](const CallableCandidate &overload) {
                          return overload.declaration != nullptr &&
                                 !sourceGraph->isDeclarationVisible(
                                     consumer, overload.record().sourceUnit,
                                     overload.declaration->name().position);
                        });
          if (published.overloads.empty()) {
            return;
          }
        }
        if (!isCompilerTrustedUnit(consumer) &&
            published.type == SemanticType::Function) {
          std::erase_if(published.overloads,
                        [](const CallableCandidate &overload) {
                          return overload.compilerPrivate;
                        });
          if (published.overloads.empty()) {
            return;
          }
          published.compilerPrivate = false;
        }
        auto &symbols = activeVisibleNamespaceSymbols()[consumer];
        const auto existing = symbols.find(name);
        if (existing == symbols.end()) {
          symbols.emplace(name, std::move(published));
          return;
        }
        if (existing->second.type != SemanticType::Function ||
            published.type != SemanticType::Function) {
          return;
        }
        existing->second.overloads.insert(existing->second.overloads.end(),
                                          published.overloads.begin(),
                                          published.overloads.end());
        existing->second.compilerPrivate =
            !existing->second.overloads.empty() &&
            std::all_of(existing->second.overloads.begin(),
                        existing->second.overloads.end(),
                        [](const CallableCandidate &overload) {
                          return overload.compilerPrivate;
                        });
      });
}

auto SemanticVisitor::Impl::namespaceIsVisible(const std::string &name) const
    -> bool {
  if (sourceGraph == nullptr || currentSourceUnit == 0) {
    return namespaces.contains(name);
  }
  const auto found = visibleNamespaces.find(currentSourceUnit);
  return found != visibleNamespaces.end() && found->second.contains(name);
}

auto SemanticVisitor::Impl::findNamespaceAlias(const std::string &name) const
    -> const NamespaceAliasInfo * {
  if (sourceGraph == nullptr || currentSourceUnit == 0) {
    const auto found = namespaceAliases.find(name);
    return found == namespaceAliases.end() ? nullptr : &found->second;
  }
  const auto unitAliases = visibleNamespaceAliases.find(currentSourceUnit);
  if (unitAliases == visibleNamespaceAliases.end()) {
    return nullptr;
  }
  const auto found = unitAliases->second.find(name);
  return found == unitAliases->second.end() ? nullptr : &found->second;
}

auto SemanticVisitor::Impl::currentNamespaceSymbols() const
    -> const std::unordered_map<std::string, Symbol> & {
  if (sourceGraph == nullptr || currentSourceUnit == 0) {
    return activeNamespaceSymbols();
  }
  const auto &visible = activeVisibleNamespaceSymbols();
  const auto found = visible.find(currentSourceUnit);
  if (found != visible.end()) {
    return found->second;
  }
  static const std::unordered_map<std::string, Symbol> empty;
  return empty;
}

auto SemanticVisitor::Impl::currentNamespaceSymbolsMutable()
    -> std::unordered_map<std::string, Symbol> * {
  if (sourceGraph == nullptr || currentSourceUnit == 0) {
    return &activeNamespaceSymbols();
  }
  auto &visible = activeVisibleNamespaceSymbols();
  const auto found = visible.find(currentSourceUnit);
  return found == visible.end() ? nullptr : &found->second;
}

auto SemanticVisitor::Impl::currentClassIds() const
    -> const std::unordered_map<std::string, ClassId> & {
  if (sourceGraph == nullptr || currentSourceUnit == 0) {
    return classIds;
  }
  const auto found = visibleClassIds.find(currentSourceUnit);
  if (found != visibleClassIds.end()) {
    return found->second;
  }
  static const std::unordered_map<std::string, ClassId> empty;
  return empty;
}

auto SemanticVisitor::Impl::currentEnumIds() const
    -> const std::unordered_map<std::string, EnumId> & {
  if (sourceGraph == nullptr || currentSourceUnit == 0) {
    return enumIds;
  }
  const auto found = visibleEnumIds.find(currentSourceUnit);
  if (found != visibleEnumIds.end()) {
    return found->second;
  }
  static const std::unordered_map<std::string, EnumId> empty;
  return empty;
}

auto SemanticVisitor::Impl::currentTypeAliasIds() const
    -> const std::unordered_map<std::string, TypeAliasId> & {
  if (sourceGraph == nullptr || currentSourceUnit == 0) {
    return typeAliasIds;
  }
  const auto found = visibleTypeAliasIds.find(currentSourceUnit);
  if (found != visibleTypeAliasIds.end()) {
    return found->second;
  }
  static const std::unordered_map<std::string, TypeAliasId> empty;
  return empty;
}

auto SemanticVisitor::Impl::currentConceptIds() const
    -> const std::unordered_map<std::string, ConceptId> & {
  if (sourceGraph == nullptr || currentSourceUnit == 0) {
    return conceptIds;
  }
  const auto found = visibleConceptIds.find(currentSourceUnit);
  if (found != visibleConceptIds.end()) {
    return found->second;
  }
  static const std::unordered_map<std::string, ConceptId> empty;
  return empty;
}

auto SemanticVisitor::Impl::registerNamespaces(const StmtList &statements,
                                               std::vector<std::string> scope)
    -> void {
  for (const StmtPtr &statement : statements) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        registerNamespaces(*branch, scope);
      }
      continue;
    }
    const auto *namespaceDecl = stmtAs<NamespaceDecl>(statement.get());
    if (namespaceDecl == nullptr) {
      continue;
    }
    currentSourceUnit = sourceUnitFor(namespaceDecl->name());
    const std::string name = qualifiedName(scope, namespaceDecl->name().lexeme);
    const bool rootCompilerPrivate =
        scope.empty() && namespaceDecl->name().lexeme == "gti_internal";
    if (rootCompilerPrivate && !isCompilerTrustedUnit(currentSourceUnit)) {
      reportCompilerPrivateDeclaration(namespaceDecl->name());
    }
    namespaces.insert(name);
    const SymbolId symbol =
        recordToolingSymbol(namespaceDecl->name(), SymbolKind::Namespace, name,
                            SemanticType::Unknown);
    namespaceToolingSymbols.try_emplace(name, symbol);
    if (rootCompilerPrivate && isPreludeUnit(currentSourceUnit)) {
      compilerPrivateNamespaceSymbol = symbol;
    }
    publishNamespace(name, currentSourceUnit, namespaceDecl->name());
    scope.emplace_back(namespaceDecl->name().lexeme);
    registerNamespaces(namespaceDecl->declarations(), scope);
    scope.pop_back();
  }
}

auto SemanticVisitor::Impl::registerNamespaceAliases(
    const StmtList &statements, std::vector<std::string> scope) -> void {
  for (const StmtPtr &statement : statements) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        registerNamespaceAliases(*branch, scope);
      }
    } else if (const auto *alias =
                   stmtAs<NamespaceAliasDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(alias->name());
      if ((!scope.empty() && isCompilerPrivateScope(scope)) ||
          (scope.empty() && alias->name().lexeme == "gti_internal")) {
        if (!isCompilerTrustedUnit(currentSourceUnit)) {
          reportCompilerPrivateDeclaration(alias->name());
          continue;
        }
      }
      if (reportCompilerPrivatePath(alias->target())) {
        const std::string name = qualifiedName(scope, alias->name().lexeme);
        NamespaceAliasInfo poisoned{.target = "gti_internal",
                                    .sourceUnit = currentSourceUnit,
                                    .compilerPrivate = true};
        namespaceAliases.insert_or_assign(name, poisoned);
        visibleNamespaceAliases[currentSourceUnit].insert_or_assign(
            name, std::move(poisoned));
        continue;
      }
      const std::optional<std::string> targetNamespace =
          resolveNamespacePath(alias->target(), scope);
      if (!targetNamespace) {
        report(alias->target().last(), "Unknown namespace in alias target.");
        diagnostics.back().missingIncludeName = alias->target().last().lexeme;
        continue;
      }

      const std::string name = qualifiedName(scope, alias->name().lexeme);
      if (namespaces.contains(name) || namespaceAliases.contains(name)) {
        report(alias->name(),
               "Duplicate declaration of '" + alias->name().lexeme + "'.");
        continue;
      }
      NamespaceAliasInfo info{
          .target = *targetNamespace,
          .sourceUnit = currentSourceUnit,
          .compilerPrivate = isCompilerPrivateQualifiedName(name) ||
                             isCompilerPrivateQualifiedName(*targetNamespace)};
      namespaceAliases.emplace(name, info);
      const SymbolId symbol =
          recordToolingSymbol(alias->name(), SymbolKind::NamespaceAlias, name,
                              SemanticType::Unknown);
      namespaceToolingSymbols.try_emplace(name, symbol);
      publishNamespaceAlias(name, info, alias->name());
    } else if (const auto *namespaceDecl =
                   stmtAs<NamespaceDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(namespaceDecl->name());
      scope.emplace_back(namespaceDecl->name().lexeme);
      registerNamespaceAliases(namespaceDecl->declarations(), scope);
      scope.pop_back();
    }
  }
}

auto SemanticVisitor::Impl::registerTypeAlias(
    const TypeAliasDecl &alias, const std::vector<std::string> &lookupScope,
    const std::vector<std::string> &namespaceScope, const ClassDecl *owner,
    AccessModifier access) -> void {
  currentSourceUnit = sourceUnitFor(alias.name());
  const std::string qualified = qualifiedName(lookupScope, alias.name().lexeme);
  const auto duplicate =
      std::find_if(typeAliases.begin(), typeAliases.end(),
                   [&](const RegisteredTypeAlias &other) {
                     return other.qualifiedName == qualified &&
                            other.ownerDeclaration == owner;
                   });
  if (namespaces.contains(qualified) || namespaceAliases.contains(qualified) ||
      duplicate != typeAliases.end()) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2006", DiagnosticPhase::Semantics, alias.name(),
        "Duplicate declaration of '" + alias.name().lexeme + "'.");
    if (duplicate != typeAliases.end()) {
      diagnostic.related.push_back({tokenSpan(duplicate->declaration->name()),
                                    "Previous declaration is here."});
    }
    diagnostics.emplace_back(std::move(diagnostic));
    return;
  }
  if (owner != nullptr && alias.name().lexeme == owner->name().lexeme) {
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2006", DiagnosticPhase::Semantics, alias.name(),
                       "Class-scoped type alias '" + alias.name().lexeme +
                           "' cannot have the same name as its owning type.");
    diagnostic.related.push_back(
        {tokenSpan(owner->name()), "Owning type is declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
    return;
  }

  const TypeAliasId id = typeAliases.size() + 1;
  if (owner == nullptr) {
    typeAliasIds.emplace(qualified, id);
  }
  typeAliases.push_back({.sourceUnit = currentSourceUnit,
                         .declaration = &alias,
                         .ownerDeclaration = owner,
                         .qualifiedName = qualified,
                         .namespaceScope = namespaceScope,
                         .lookupScope = lookupScope,
                         .access = access,
                         .compilerPrivate = compilerPrivateDeclaration(
                             currentSourceUnit, namespaceScope)});
  if (owner == nullptr) {
    publishTypeAlias(qualified, id, currentSourceUnit,
                     typeAliases.back().compilerPrivate);
  }
}

auto SemanticVisitor::Impl::registerClassTypeAliases(
    const StmtList &members, const std::vector<std::string> &scope,
    const ClassDecl &owner, AccessModifier &access) -> void {
  for (const StmtPtr &statement : members) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        registerClassTypeAliases(*branch, scope, owner, access);
      }
      continue;
    }
    if (const auto *specifier = stmtAs<AccessSpecifierDecl>(statement.get())) {
      access = specifier->modifier();
      continue;
    }
    if (const auto *alias = stmtAs<TypeAliasDecl>(statement.get())) {
      registerTypeAlias(
          *alias, scope,
          std::vector<std::string>(scope.begin(), scope.end() - 1), &owner,
          access);
    }
  }
}

auto SemanticVisitor::Impl::registerTypeAliases(const StmtList &statements,
                                                std::vector<std::string> scope)
    -> void {
  for (const StmtPtr &statement : statements) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        registerTypeAliases(*branch, scope);
      }
    } else if (const auto *alias = stmtAs<TypeAliasDecl>(statement.get())) {
      registerTypeAlias(*alias, scope, scope, nullptr, AccessModifier::Public);
    } else if (const auto *classDecl = stmtAs<ClassDecl>(statement.get())) {
      if (classDecl->isForwardDeclaration()) {
        continue;
      }
      std::vector<std::string> memberScope = scope;
      memberScope.emplace_back(classDecl->name().lexeme);
      AccessModifier access = classDecl->kind() == ClassKind::Class
                                  ? AccessModifier::Private
                                  : AccessModifier::Public;
      registerClassTypeAliases(classDecl->members(), memberScope, *classDecl,
                               access);
    } else if (const auto *namespaceDecl =
                   stmtAs<NamespaceDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(namespaceDecl->name());
      scope.emplace_back(namespaceDecl->name().lexeme);
      registerTypeAliases(namespaceDecl->declarations(), scope);
      scope.pop_back();
    }
  }
}

auto SemanticVisitor::Impl::resolveTypeAliases() -> void {
  // Alias identities are needed by member/signature registration. Structural
  // constraints, however, require the completed member/lifecycle inventory.
  deferringTypeAliasConstraints = true;
  for (RegisteredTypeAlias &alias : typeAliases) {
    if (alias.ownerDeclaration == nullptr) {
      continue;
    }
    const auto owner = classDeclIds.find(alias.ownerDeclaration);
    if (owner == classDeclIds.end()) {
      report(alias.declaration->name(),
             "Class-scoped type aliases are not supported in exact class "
             "specializations.",
             "GTI-S2030");
      alias.resolution = TypeAliasResolution::Invalid;
      continue;
    }
    alias.ownerClass = owner->second;
    alias.compilerPrivate =
        alias.compilerPrivate || classInfo(alias.ownerClass).compilerPrivate;
  }
  for (TypeAliasId id = 1; id <= typeAliases.size(); ++id) {
    (void)resolveTypeAlias(id, nullptr);
  }
  for (TypeAliasId id = 1; id <= typeAliases.size(); ++id) {
    const RegisteredTypeAlias &alias = typeAliases[id - 1];
    if (alias.ownerClass == 0 ||
        alias.resolution != TypeAliasResolution::Resolved) {
      continue;
    }
    ClassInfo &owner = classInfo(alias.ownerClass);
    owner.typeAliases.emplace(
        alias.declaration->name().lexeme,
        MemberTypeAliasInfo{.id = id,
                            .declaration = alias.declaration,
                            .type = alias.type,
                            .qualifiedName = alias.qualifiedName,
                            .ownerClass = alias.ownerClass,
                            .access = alias.access,
                            .compilerPrivate = alias.compilerPrivate});
  }
  visibleTypeAliasIds.clear();
  for (TypeAliasId id = 1; id <= typeAliases.size(); ++id) {
    const RegisteredTypeAlias &alias = typeAliases[id - 1];
    if (alias.ownerDeclaration == nullptr) {
      publishTypeAlias(alias.qualifiedName, id, alias.sourceUnit,
                       alias.compilerPrivate);
    }
  }
  deferringTypeAliasConstraints = false;
}

auto SemanticVisitor::Impl::validateTypeAliasConstraints() -> void {
  const SourceUnitId enclosingUnit = currentSourceUnit;
  const auto enclosingNamespace = currentNamespace;
  const auto enclosingClass = currentClass;
  for (const RegisteredTypeAlias &alias : typeAliases) {
    if (alias.resolution != TypeAliasResolution::Resolved) {
      continue;
    }
    currentSourceUnit = alias.sourceUnit;
    currentNamespace = alias.lookupScope;
    currentClass = alias.ownerClass == 0
                       ? std::nullopt
                       : std::optional<ClassId>(alias.ownerClass);
    if (currentClass) {
      beginTypeParameterScope(classInfo(*currentClass).genericParameters);
    }
    validateType(alias.declaration->target(),
                 alias.declaration->target().isNativeFunction());
    if (currentClass) {
      endTypeParameterScope();
    }
  }
  currentSourceUnit = enclosingUnit;
  currentNamespace = enclosingNamespace;
  currentClass = enclosingClass;
}

auto SemanticVisitor::Impl::registerConcepts(const StmtList &statements,
                                             std::vector<std::string> scope)
    -> void {
  for (const StmtPtr &statement : statements) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        registerConcepts(*branch, scope);
      }
    } else if (const auto *declaration = stmtAs<ConceptDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(declaration->name());
      const std::string qualified =
          qualifiedName(scope, declaration->name().lexeme);
      if (namespaces.contains(qualified) ||
          namespaceAliases.contains(qualified) ||
          typeAliasIds.contains(qualified) || enumIds.contains(qualified) ||
          classIds.contains(qualified) || conceptIds.contains(qualified)) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2006", DiagnosticPhase::Semantics, declaration->name(),
            "Duplicate declaration of '" + declaration->name().lexeme + "'.");
        if (const auto existing = conceptIds.find(qualified);
            existing != conceptIds.end()) {
          diagnostic.related.push_back(
              {tokenSpan(concepts[existing->second - 1].declaration->name()),
               "Previous concept declaration is here."});
        }
        diagnostics.emplace_back(std::move(diagnostic));
        continue;
      }
      std::unordered_set<std::string> parameterNames;
      for (const Token &parameter : declaration->typeParameters()) {
        if (parameter.lexeme == declaration->name().lexeme) {
          report(parameter,
                 "Concept type parameter cannot have the same name as the "
                 "concept.",
                 "GTI-S2049");
        }
        if (!parameterNames.insert(parameter.lexeme).second) {
          report(parameter,
                 "Duplicate concept type parameter '" + parameter.lexeme + "'.",
                 "GTI-S2049");
        }
      }

      const ConceptId id = concepts.size() + 1;
      const SymbolId symbol =
          recordToolingSymbol(declaration->name(), SymbolKind::Concept,
                              qualified, SemanticType::Unknown);
      std::vector<SymbolId> parameterSymbols;
      std::vector<GenericParameterId> parameterIds;
      parameterSymbols.reserve(declaration->typeParameters().size());
      for (const Token &parameter : declaration->typeParameters()) {
        const GenericParameterId parameterId = nextGenericParameterId++;
        parameterIds.push_back(parameterId);
        parameterSymbols.emplace_back(recordToolingSymbol(
            parameter, SymbolKind::TypeParameter,
            qualified + "::" + parameter.lexeme,
            SemanticType::typeParameter(parameterId), false, false));
      }
      conceptIds.emplace(qualified, id);
      conceptDeclIds.emplace(declaration, id);
      publishConcept(qualified, id, currentSourceUnit, declaration->name());
      concepts.push_back({.id = id,
                          .sourceUnit = currentSourceUnit,
                          .declaration = declaration,
                          .qualifiedName = qualified,
                          .namespaceScope = scope,
                          .symbol = symbol,
                          .parameterSymbols = std::move(parameterSymbols),
                          .parameterIds = std::move(parameterIds),
                          .compilerPrivate = compilerPrivateDeclaration(
                              currentSourceUnit, scope)});
    } else if (const auto *namespaceDeclaration =
                   stmtAs<NamespaceDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(namespaceDeclaration->name());
      scope.emplace_back(namespaceDeclaration->name().lexeme);
      registerConcepts(namespaceDeclaration->declarations(), scope);
      scope.pop_back();
    }
  }
}

auto SemanticVisitor::Impl::resolveConcepts() -> void {
  for (ConceptId id = 1; id <= concepts.size(); ++id) {
    (void)resolveConcept(id, nullptr);
  }
}

auto SemanticVisitor::Impl::resolveConcept(ConceptId id, const Token *use)
    -> GenericConstraintSet {
  if (id == 0 || id > concepts.size()) {
    return constraintBit(GenericConstraintKind::Invalid);
  }
  RegisteredConcept &conceptInfo = concepts[id - 1];
  if (conceptInfo.resolution == ConceptResolution::Resolved) {
    return conceptInfo.constraints;
  }
  if (conceptInfo.resolution == ConceptResolution::Invalid) {
    return constraintBit(GenericConstraintKind::Invalid);
  }
  if (conceptInfo.resolution == ConceptResolution::Resolving) {
    const Token &location =
        use == nullptr ? conceptInfo.declaration->name() : *use;
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2049", DiagnosticPhase::Semantics, location,
                       "Concept definition cycle involving '" +
                           conceptInfo.qualifiedName + "'.");
    diagnostic.related.push_back(
        {tokenSpan(conceptInfo.declaration->name()),
         "Concept participating in the cycle is declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
    conceptInfo.resolution = ConceptResolution::Invalid;
    conceptInfo.constraints = constraintBit(GenericConstraintKind::Invalid);
    conceptInfo.parameterConstraints.assign(
        conceptInfo.declaration->typeParameters().size(),
        constraintBit(GenericConstraintKind::Invalid));
    return conceptInfo.constraints;
  }

  conceptInfo.resolution = ConceptResolution::Resolving;
  const SourceUnitId enclosingSourceUnit = currentSourceUnit;
  const std::vector<std::string> enclosingNamespace = currentNamespace;
  currentSourceUnit = conceptInfo.sourceUnit;
  currentNamespace = conceptInfo.namespaceScope;

  const ConceptDecl &declaration = *conceptInfo.declaration;
  typeParameterScopes.emplace_back();
  typePackScopes.emplace_back();
  for (std::size_t index = 0; index < declaration.typeParameters().size();
       ++index)
    typeParameterScopes.back().emplace(
        declaration.typeParameters()[index].lexeme,
        SemanticType::typeParameter(conceptInfo.parameterIds[index]));
  std::vector<GenericConstraintSet> parameterConstraints(
      declaration.typeParameters().size(), 0);
  std::vector<RegisteredConcept::StructuralPattern> structuralConstraints;
  conceptInfo.definitionRequirements.clear();
  conceptInfo.definitionExpression = nullptr;
  conceptInfo.hasStructuralRequirements = false;
  bool valid = true;
  if (declaration.compilerBinding()) {
    const CompilerConstraintBinding &binding = *declaration.compilerBinding();
    const bool trustedAtomicConstraint =
        (isAtomicLibraryUnit(conceptInfo.sourceUnit) &&
         binding.name == "atomic_scalar") ||
        (isMetaLibraryUnit(conceptInfo.sourceUnit) &&
         binding.name == "meta_reflectable");
    const bool trustedMemoryConstraint =
        (isMemoryLibraryUnit(conceptInfo.sourceUnit) &&
         (binding.name == "object_layout" ||
          binding.name == "independent_object")) ||
        (isPolymorphicLibraryUnit(conceptInfo.sourceUnit) &&
         binding.name == "inline_polymorphic_payload");
    const bool trustedIteratorConstraint =
        isIteratorLibraryUnit(conceptInfo.sourceUnit) &&
        (binding.name == "input_iterator" || binding.name == "sentinel_for" ||
         binding.name == "dereferences_to" ||
         binding.name == "writable_iterator" || binding.name == "writes_to" ||
         binding.name == "bidirectional_iterator" ||
         binding.name == "random_access_iterator" ||
         binding.name == "subscripts_to_mut" ||
         binding.name == "sized_sentinel_for" ||
         binding.name == "input_range" || binding.name == "range_of" ||
         binding.name == "mutable_range_of" || binding.name == "sized_range");
    const bool trustedFunctionalConstraint =
        isFunctionalLibraryUnit(conceptInfo.sourceUnit) &&
        (binding.name == "read_callable" || binding.name == "mut_callable" ||
         binding.name == "once_callable" ||
         binding.name == "read_binary_callable" ||
         binding.name == "mut_binary_callable" ||
         binding.name == "once_binary_callable" ||
         binding.name == "once_action" ||
         binding.name == "indirect_predicate" ||
         binding.name == "indirect_operation" ||
         binding.name == "range_predicate" ||
         binding.name == "range_operation");
    if ((!isPreludeUnit(conceptInfo.sourceUnit) && !trustedAtomicConstraint &&
         !trustedIteratorConstraint && !trustedFunctionalConstraint &&
         !trustedMemoryConstraint) ||
        conceptInfo.namespaceScope !=
            std::vector<std::string>{"gti_internal"}) {
      report(binding.attribute,
             "@compiler_constraint is reserved for compiler-owned concepts "
             "declared in their trusted standard-library unit's "
             "gti_internal namespace.",
             "GTI-S2049");
      valid = false;
    } else if (const std::optional<GenericConstraintKind> kind =
                   compilerConstraint(binding.name)) {
      if (declaration.typeParameters().size() != 1) {
        report(binding.attribute,
               "Compiler constraint binding '" + binding.name +
                   "' requires exactly one type parameter.",
               "GTI-S2049");
        valid = false;
      } else {
        parameterConstraints.front() = constraintBit(*kind);
      }
    } else if (const std::optional<CompilerStructuralConstraintRecord>
                   structural = compilerStructuralConstraint(binding.name)) {
      if (declaration.typeParameters().size() != structural->arity) {
        report(binding.attribute,
               "Compiler structural constraint binding '" + binding.name +
                   "' requires " + std::to_string(structural->arity) +
                   " type parameter" + (structural->arity == 1 ? "." : "s."),
               "GTI-S2049");
        valid = false;
      } else {
        RegisteredConcept::StructuralPattern pattern;
        pattern.kind = structural->kind;
        for (std::size_t index = 0; index < structural->arity; ++index) {
          pattern.arguments.push_back({.parameter = index});
        }
        structuralConstraints.emplace_back(std::move(pattern));
      }
    } else {
      report(binding.attribute,
             "Unknown compiler constraint binding '" + binding.name + "'.",
             "GTI-S2049");
      valid = false;
    }
  } else {
    conceptInfo.definitionExpression = &declaration.constraintExpression();
    std::vector<std::vector<GenericConstraintSet>> leafParameterConstraints(
        declaration.requirements().size(),
        std::vector<GenericConstraintSet>(declaration.typeParameters().size(),
                                          0));
    std::vector<std::vector<RegisteredConcept::StructuralPattern>>
        leafStructuralConstraints(declaration.requirements().size());
    std::vector<bool> leafHasStructural(declaration.requirements().size(),
                                        false);
    for (std::size_t requirementIndex = 0;
         requirementIndex < declaration.requirements().size();
         ++requirementIndex) {
      const ConceptApplication &requirement =
          declaration.requirements()[requirementIndex];
      std::vector<RegisteredConcept::StructuralPattern::Argument> argumentMap;
      argumentMap.reserve(requirement.arguments.size());
      for (const TypeRef &argument : requirement.arguments) {
        const Token &argumentToken = argument.name.last();
        const auto parameter = std::find_if(
            declaration.typeParameters().begin(),
            declaration.typeParameters().end(), [&](const Token &candidate) {
              return argument.arguments.empty() && argument.pointers.empty() &&
                     !argument.reference && argument.arrayExtents.empty() &&
                     candidate.lexeme == argumentToken.lexeme;
            });
        if (parameter == declaration.typeParameters().end()) {
          validateType(argument);
          const SemanticType concrete = typeOf(argument);
          if (concrete == SemanticType::Unknown) {
            report(argumentToken,
                   "Concept requirement argument must name a visible type "
                   "or a compound type formed from the concept's parameters.",
                   "GTI-S2049");
            argumentMap.push_back({});
            valid = false;
            continue;
          }
          argumentMap.push_back({.concrete = concrete});
          recordTypeUse(argument);
          continue;
        }
        const std::size_t index = static_cast<std::size_t>(
            std::distance(declaration.typeParameters().begin(), parameter));
        argumentMap.push_back({.parameter = index});
        if (index < conceptInfo.parameterSymbols.size()) {
          semanticModel.recordOccurrence(
              {.sourceUnit = currentSourceUnit,
               .span = tokenSpan(argumentToken),
               .kind = SemanticOccurrenceKind::Symbol,
               .symbol = conceptInfo.parameterSymbols[index],
               .roles = OccurrenceRole::Reference | OccurrenceRole::TypeUse,
               .name = argumentToken.lexeme});
        }
      }
      if (requirement.memberRequirement) {
        recordMemberRequirementReceiver(*requirement.memberRequirement,
                                        SemanticType::Unknown);
        RegisteredConcept::StructuralPattern pattern;
        pattern.kind = StructuralConstraintKind::ReadMember;
        pattern.arguments = argumentMap;
        pattern.memberName = requirement.memberRequirement->member().lexeme;
        pattern.mutableReceiver = requirement.memberRequirement->receiver()
                                      .referenceMutability.has_value();
        pattern.requiresUnsafe =
            requirement.memberRequirement->unsafeKeyword().has_value();
        if (!pattern.requiresUnsafe) {
          auto weaker = pattern;
          weaker.requiresUnsafe = true;
          leafStructuralConstraints[requirementIndex].push_back(
              std::move(weaker));
        }
        leafStructuralConstraints[requirementIndex].push_back(
            std::move(pattern));
        leafHasStructural[requirementIndex] = true;
        conceptInfo.definitionRequirements.push_back(
            {.syntax = &requirement,
             .arguments = std::move(argumentMap),
             .expressionApplication = requirementIndex});
        continue;
      }
      if (reportCompilerPrivatePath(requirement.name)) {
        valid = false;
        continue;
      }
      recordQualifiedPathUses(requirement.name);

      const std::optional<ConceptId> dependency =
          resolveConceptPath(requirement.name, conceptInfo.namespaceScope);
      if (!dependency) {
        const std::optional<ConceptId> global = resolveConceptPathGlobally(
            requirement.name, conceptInfo.namespaceScope);
        if (global && *global != 0 && *global <= concepts.size() &&
            reportInvisibleDeclaration(
                requirement.name.last(), pathSpelling(requirement.name),
                concepts[*global - 1].declaration->name(),
                concepts[*global - 1].sourceUnit)) {
          valid = false;
          continue;
        }
        report(requirement.name.last(),
               "Unknown concept '" + pathSpelling(requirement.name) + "'.",
               "GTI-S2049");
        diagnostics.back().missingIncludeName = requirement.name.last().lexeme;
        valid = false;
        continue;
      }
      RegisteredConcept &required = concepts[*dependency - 1];
      semanticModel.recordOccurrence(
          {.sourceUnit = currentSourceUnit,
           .span = tokenSpan(requirement.name.last()),
           .kind = SemanticOccurrenceKind::Symbol,
           .symbol = required.symbol,
           .roles = OccurrenceRole::Reference | OccurrenceRole::TypeUse,
           .name = requirement.name.last().lexeme});
      if (required.declaration->compilerBinding() &&
          conceptInfo.sourceUnit != required.sourceUnit) {
        report(
            requirement.name.last(),
            "Compiler capability concept '" + required.qualifiedName +
                "' is private to its standard-library unit; compose a public "
                "concept instead.",
            "GTI-S2049");
        valid = false;
        continue;
      }
      const GenericConstraintSet resolved =
          resolveConcept(*dependency, &requirement.name.last());
      if (invalidConstraintSet(resolved)) {
        valid = false;
        continue;
      }
      if (argumentMap.size() != required.declaration->typeParameters().size()) {
        report(
            requirement.name.last(),
            "Concept '" + required.qualifiedName + "' requires " +
                std::to_string(required.declaration->typeParameters().size()) +
                " type argument" +
                (required.declaration->typeParameters().size() == 1 ? "."
                                                                    : "s."),
            "GTI-S2049");
        valid = false;
        continue;
      }
      conceptInfo.definitionRequirements.push_back(
          {.conceptId = *dependency,
           .syntax = &requirement,
           .arguments = argumentMap,
           .expressionApplication = requirementIndex});
      leafHasStructural[requirementIndex] =
          required.hasStructuralRequirements ||
          !required.structuralConstraints.empty();
      for (std::size_t index = 0; index < required.parameterConstraints.size();
           ++index) {
        const auto &mappedArgument = argumentMap[index];
        if (mappedArgument.parameter) {
          leafParameterConstraints[requirementIndex]
                                  [*mappedArgument.parameter] |=
              required.parameterConstraints[index];
        }
      }
      for (const RegisteredConcept::StructuralPattern &pattern :
           required.structuralConstraints) {
        RegisteredConcept::StructuralPattern mapped;
        mapped.kind = pattern.kind;
        mapped.memberName = pattern.memberName;
        mapped.mutableReceiver = pattern.mutableReceiver;
        mapped.requiresUnsafe = pattern.requiresUnsafe;
        std::vector<SemanticType> mappedTypes;
        for (const auto &argument : argumentMap)
          mappedTypes.push_back(
              argument.parameter
                  ? SemanticType::typeParameter(
                        conceptInfo.parameterIds[*argument.parameter])
                  : argument.concrete);
        const auto substitution = conceptSubstitution(required, mappedTypes);
        for (const RegisteredConcept::StructuralPattern::Argument &argument :
             pattern.arguments) {
          if (!argument.parameter) {
            mapped.arguments.push_back(
                {.concrete = substituteType(argument.concrete, substitution)});
            continue;
          }
          if (*argument.parameter >= argumentMap.size()) {
            valid = false;
            continue;
          }
          mapped.arguments.push_back(argumentMap[*argument.parameter]);
        }
        if (mapped.arguments.size() == pattern.arguments.size()) {
          for (StructuralConstraintKind guaranteed :
               guaranteedStructuralKinds(mapped.kind)) {
            RegisteredConcept::StructuralPattern implied = mapped;
            implied.kind = guaranteed;
            auto &leaf = leafStructuralConstraints[requirementIndex];
            if (std::find(leaf.begin(), leaf.end(), implied) == leaf.end()) {
              leaf.emplace_back(std::move(implied));
            }
          }
        }
      }
    }
    if (declaration.requirements().empty()) {
      report(declaration.name(),
             "A source-defined concept requires at least one capability.",
             "GTI-S2049");
      valid = false;
    }

    struct Guarantees {
      std::vector<GenericConstraintSet> parameters;
      std::vector<RegisteredConcept::StructuralPattern> structural;
      bool hasStructural = false;
    };
    const auto appendUnique = [](auto &destination, const auto &source) {
      for (const auto &value : source) {
        if (std::find(destination.begin(), destination.end(), value) ==
            destination.end()) {
          destination.push_back(value);
        }
      }
    };
    const ConstraintExpression &expression = declaration.constraintExpression();
    std::function<Guarantees(std::size_t)> guaranteesFor =
        [&](std::size_t nodeIndex) -> Guarantees {
      Guarantees result{.parameters = std::vector<GenericConstraintSet>(
                            declaration.typeParameters().size(), 0)};
      if (nodeIndex >= expression.nodes.size()) {
        valid = false;
        return result;
      }
      const ConstraintExpressionNode &node = expression.nodes[nodeIndex];
      if (node.kind == ConstraintExpressionKind::Application) {
        if (node.application >= leafParameterConstraints.size()) {
          valid = false;
          return result;
        }
        result.parameters = leafParameterConstraints[node.application];
        result.structural = leafStructuralConstraints[node.application];
        result.hasStructural = leafHasStructural[node.application];
        return result;
      }
      Guarantees left = guaranteesFor(node.left);
      Guarantees right = guaranteesFor(node.right);
      result.hasStructural = left.hasStructural || right.hasStructural;
      if (node.kind == ConstraintExpressionKind::Conjunction) {
        for (std::size_t index = 0; index < result.parameters.size(); ++index) {
          result.parameters[index] =
              left.parameters[index] | right.parameters[index];
        }
        result.structural = std::move(left.structural);
        appendUnique(result.structural, right.structural);
        return result;
      }
      for (std::size_t index = 0; index < result.parameters.size(); ++index) {
        result.parameters[index] =
            left.parameters[index] & right.parameters[index];
      }
      for (const RegisteredConcept::StructuralPattern &candidate :
           left.structural) {
        if (std::find(right.structural.begin(), right.structural.end(),
                      candidate) != right.structural.end()) {
          result.structural.push_back(candidate);
        }
      }
      return result;
    };
    if (!expression.empty() &&
        expression.root != ConstraintExpressionNode::invalidIndex) {
      Guarantees guarantees = guaranteesFor(expression.root);
      parameterConstraints = std::move(guarantees.parameters);
      structuralConstraints = std::move(guarantees.structural);
      conceptInfo.hasStructuralRequirements = guarantees.hasStructural;
    }
  }

  currentSourceUnit = enclosingSourceUnit;
  currentNamespace = enclosingNamespace;
  typeParameterScopes.pop_back();
  typePackScopes.pop_back();
  conceptInfo.parameterConstraints = std::move(parameterConstraints);
  conceptInfo.structuralConstraints = std::move(structuralConstraints);
  if (declaration.compilerBinding()) {
    conceptInfo.hasStructuralRequirements =
        !conceptInfo.structuralConstraints.empty();
  }
  conceptInfo.constraints =
      valid && conceptInfo.parameterConstraints.size() == 1
          ? conceptInfo.parameterConstraints.front()
          : (valid ? 0 : constraintBit(GenericConstraintKind::Invalid));
  if (!valid) {
    for (GenericConstraintSet &constraints : conceptInfo.parameterConstraints) {
      constraints |= constraintBit(GenericConstraintKind::Invalid);
    }
    conceptInfo.structuralConstraints.clear();
  }
  conceptInfo.resolution =
      valid ? ConceptResolution::Resolved : ConceptResolution::Invalid;
  return conceptInfo.constraints;
}

auto SemanticVisitor::Impl::resolveTypeAliasDependencies(
    const TypeRef &type, const std::vector<std::string> &scope) -> void {
  if (type.qualifiedOwner) {
    resolveTypeAliasDependencies(*type.qualifiedOwner, scope);
  } else if (type.name.segments.size() > 1) {
    TypeRef qualifier = type;
    qualifier.name.segments.pop_back();
    qualifier.arguments.clear();
    resolveTypeAliasDependencies(qualifier, scope);
  }
  if (type.name.last().kind == TokenKind::IDENTIFIER &&
      !resolveTypeParameter(type.name)) {
    if (const std::optional<ResolvedTypeAlias> dependency =
            resolveTypeAliasPath(type, scope)) {
      (void)resolveTypeAlias(dependency->id, &type.name.last());
    }
  }
  for (const TypeRef &argument : type.arguments) {
    resolveTypeAliasDependencies(argument, scope);
  }
}

auto SemanticVisitor::Impl::resolveTypeAlias(TypeAliasId id, const Token *use)
    -> SemanticType {
  if (id == 0 || id > typeAliases.size()) {
    return SemanticType::Unknown;
  }
  RegisteredTypeAlias &alias = typeAliases[id - 1];
  if (alias.resolution == TypeAliasResolution::Resolved) {
    return alias.type;
  }
  if (alias.resolution == TypeAliasResolution::Invalid) {
    return SemanticType::Unknown;
  }
  if (alias.resolution == TypeAliasResolution::Resolving) {
    const Token &location = use == nullptr ? alias.declaration->name() : *use;
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2030", DiagnosticPhase::Semantics, location,
        "Type alias cycle involving '" + alias.qualifiedName + "'.");
    diagnostic.related.push_back(
        {tokenSpan(alias.declaration->name()),
         "Alias participating in the cycle is declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
    alias.resolution = TypeAliasResolution::Invalid;
    return SemanticType::Unknown;
  }

  alias.resolution = TypeAliasResolution::Resolving;
  const SourceUnitId enclosingSourceUnit = currentSourceUnit;
  const std::vector<std::string> enclosingNamespace = currentNamespace;
  const std::optional<ClassId> enclosingClass = currentClass;
  currentSourceUnit = alias.sourceUnit;
  currentNamespace = alias.lookupScope;
  if (alias.ownerClass != 0) {
    currentClass = alias.ownerClass;
    beginTypeParameterScope(classInfo(alias.ownerClass).genericParameters);
  }

  const std::size_t diagnosticsBefore = diagnostics.size();
  const TypeRef &target = alias.declaration->target();
  resolveTypeAliasDependencies(target, alias.lookupScope);
  bool targetFormValid = true;
  if (target.name.last().kind == TokenKind::AUTO) {
    report(target.name.last(), "A type alias requires an explicit target type.",
           "GTI-S2030");
    targetFormValid = false;
  }
  if (target.reference) {
    report(*target.reference,
           "Reference aliases are not supported; keep '&' explicit at each "
           "borrow site.",
           "GTI-S2030");
    targetFormValid = false;
  }
  if (alias.ownerClass != 0 && target.isNativeFunction()) {
    report(target.name.last(),
           "Native callback aliases must remain at namespace scope; a "
           "class-scoped alias may name only an ordinary GTI type.",
           "GTI-S2076");
    targetFormValid = false;
  }
  if (targetFormValid) {
    validateType(target, target.isNativeFunction());
  }
  const SemanticType resolved = typeOf(target, alias.lookupScope);
  if (targetFormValid && !isCompilerTrustedUnit(alias.sourceUnit) &&
      semanticModel.isCompilerPrivateType(resolved)) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2058", DiagnosticPhase::Semantics, target.name.last(),
        "Compiler-private type alias target is unavailable to application "
        "source.");
    diagnostic.hints.emplace_back(
        "Use the public std wrapper that owns this compiler capability.");
    diagnostics.emplace_back(std::move(diagnostic));
    targetFormValid = false;
  }
  const bool valid = alias.resolution != TypeAliasResolution::Invalid &&
                     diagnostics.size() == diagnosticsBefore &&
                     resolved != SemanticType::Unknown && targetFormValid;
  if (valid) {
    alias.type = resolved;
    alias.compilerPrivate =
        alias.compilerPrivate || semanticModel.isCompilerPrivateType(resolved);
    alias.resolution = TypeAliasResolution::Resolved;
    semanticModel.record(
        *alias.declaration,
        TypeAliasInfo{.id = id,
                      .sourceUnit = alias.sourceUnit,
                      .declaration = alias.declaration,
                      .qualifiedName = alias.qualifiedName,
                      .type = alias.type,
                      .compilerPrivate = alias.compilerPrivate});
  } else {
    alias.resolution = TypeAliasResolution::Invalid;
  }

  if (alias.ownerClass != 0) {
    endTypeParameterScope();
  }
  currentClass = enclosingClass;
  currentNamespace = enclosingNamespace;
  currentSourceUnit = enclosingSourceUnit;
  return valid ? alias.type : SemanticType::Unknown;
}

auto SemanticVisitor::Impl::enumUnderlyingType(
    const std::optional<TypeRef> &type) -> SemanticType {
  if (!type) {
    return SemanticType::Int32;
  }
  switch (type->name.last().kind) {
  case TokenKind::INT:
  case TokenKind::INT32:
    return SemanticType::Int32;
  case TokenKind::INT8:
    return SemanticType::Int8;
  case TokenKind::INT16:
    return SemanticType::Int16;
  case TokenKind::INT64:
    return SemanticType::Int64;
  case TokenKind::UINT:
  case TokenKind::UINT32:
    return SemanticType::UInt32;
  case TokenKind::UINT8:
    return SemanticType::UInt8;
  case TokenKind::UINT16:
    return SemanticType::UInt16;
  case TokenKind::UINT64:
    return SemanticType::UInt64;
  default:
    return SemanticType::Unknown;
  }
}

auto SemanticVisitor::Impl::validEnumUnderlyingSyntax(const TypeRef &type)
    -> bool {
  return type.name.segments.size() == 1 && type.arguments.empty() &&
         type.arrayExtents.empty() && !type.reference &&
         enumUnderlyingType(type) != SemanticType::Unknown;
}

auto SemanticVisitor::Impl::nextEnumConstant(EnumConstant value)
    -> std::optional<EnumConstant> {
  if (value.negative) {
    if (value.magnitude <= 1) {
      return EnumConstant{};
    }
    --value.magnitude;
    return value;
  }
  if (value.magnitude == std::numeric_limits<std::uint64_t>::max()) {
    return std::nullopt;
  }
  ++value.magnitude;
  return value;
}

auto SemanticVisitor::Impl::registerEnums(const StmtList &statements,
                                          std::vector<std::string> scope)
    -> void {
  for (const StmtPtr &statement : statements) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        registerEnums(*branch, scope);
      }
      continue;
    }
    if (const auto *namespaceDecl = stmtAs<NamespaceDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(namespaceDecl->name());
      scope.emplace_back(namespaceDecl->name().lexeme);
      registerEnums(namespaceDecl->declarations(), scope);
      scope.pop_back();
      continue;
    }
    const auto *enumDecl = stmtAs<EnumDecl>(statement.get());
    if (enumDecl == nullptr) {
      continue;
    }

    currentSourceUnit = sourceUnitFor(enumDecl->name());
    const std::string qualified = qualifiedName(scope, enumDecl->name().lexeme);
    if (namespaces.contains(qualified) ||
        namespaceAliases.contains(qualified) ||
        typeAliasIds.contains(qualified) || enumIds.contains(qualified)) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2006", DiagnosticPhase::Semantics, enumDecl->name(),
          "Duplicate declaration of '" + enumDecl->name().lexeme + "'.");
      if (const auto existing = enumIds.find(qualified);
          existing != enumIds.end()) {
        diagnostic.related.push_back(
            {tokenSpan(enums[existing->second - 1].name),
             "Previous declaration is here."});
      }
      diagnostics.emplace_back(std::move(diagnostic));
      continue;
    }

    const bool payload = std::any_of(enumDecl->enumerators().begin(),
                                     enumDecl->enumerators().end(),
                                     [](const EnumeratorDecl &enumerator) {
                                       return !enumerator.payload.empty();
                                     });
    if (!payload && !enumDecl->genericParameters().empty()) {
      report(enumDecl->name(),
             "Generic enum declarations require a payload alternative.",
             "GTI-S2067");
    }
    if (payload && enumDecl->underlyingType()) {
      report(enumDecl->underlyingType()->name.last(),
             "Payload enums use compiler-defined variant indices and "
             "cannot declare an integral backing type.",
             "GTI-S2067");
    }

    SemanticType underlying = enumUnderlyingType(enumDecl->underlyingType());
    if (enumDecl->underlyingType() &&
        !validEnumUnderlyingSyntax(*enumDecl->underlyingType())) {
      report(enumDecl->underlyingType()->name.last(),
             "A scoped enum backing type must be a fixed integral primitive "
             "such as int32_t or uint8_t.",
             "GTI-S2036");
      underlying = SemanticType::Int32;
    }

    const EnumId id = enums.size() + 1;
    enumIds.emplace(qualified, id);
    publishEnum(qualified, id, currentSourceUnit, enumDecl->name());
    enums.push_back(EnumInfo{.id = id,
                             .sourceUnit = currentSourceUnit,
                             .declaration = enumDecl,
                             .name = enumDecl->name(),
                             .namespaceScope = scope,
                             .underlyingType = underlying,
                             .payload = payload,
                             .compilerPrivate = compilerPrivateDeclaration(
                                 currentSourceUnit, scope)});
    EnumInfo &info = enums.back();
    std::vector<EnumeratorInfo> modelEnumerators;
    modelEnumerators.reserve(enumDecl->enumerators().size());

    std::optional<EnumConstant> nextValue = EnumConstant{};
    for (std::size_t variantIndex = 0;
         variantIndex < enumDecl->enumerators().size(); ++variantIndex) {
      const EnumeratorDecl &enumerator = enumDecl->enumerators()[variantIndex];
      EnumConstant value = nextValue.value_or(EnumConstant{});
      bool valueKnown = nextValue.has_value();
      if (payload) {
        value = {.magnitude = variantIndex};
        valueKnown = true;
        if (enumerator.initializer) {
          report(expressionToken(enumerator.initializer),
                 "Payload enum variants cannot declare numeric values; "
                 "their closed declaration order defines the tag.",
                 "GTI-S2067");
        }
      } else if (enumerator.initializer) {
        const std::optional<IntegerConstant> constant =
            integerConstant(enumerator.initializer.get());
        if (!constant) {
          report(expressionToken(enumerator.initializer),
                 "Scoped enum values must currently be signed integer "
                 "literals.",
                 "GTI-S2036");
          valueKnown = false;
        } else {
          value = {.negative = constant->negative,
                   .magnitude = constant->magnitude};
          valueKnown = true;
        }
      } else if (!nextValue) {
        report(enumerator.name,
               "Implicit scoped enum value cannot be determined from the "
               "previous enumerator.",
               "GTI-S2036");
      }

      if (valueKnown &&
          !integerFits(underlying,
                       IntegerConstant{.negative = value.negative,
                                       .magnitude = value.magnitude})) {
        report(enumerator.name,
               "Scoped enum value does not fit its backing type.", "GTI-S2036");
      }

      const auto [inserted, success] = info.enumerators.emplace(
          enumerator.name.lexeme,
          EnumeratorRecord{
              .declaration = &enumerator,
              .value = value,
              .variantIndex = variantIndex,
              .symbol = Symbol{.type = SemanticType::enumType(id),
                               .sourceUnit = currentSourceUnit,
                               .assignable = false,
                               .declaration = enumerator.name,
                               .compilerPrivate = info.compilerPrivate}});
      if (!success) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2006", DiagnosticPhase::Semantics, enumerator.name,
            "Duplicate enumerator '" + enumerator.name.lexeme + "'.");
        diagnostic.related.push_back(
            {tokenSpan(inserted->second.declaration->name),
             "Previous enumerator is here."});
        diagnostics.emplace_back(std::move(diagnostic));
      }
      modelEnumerators.push_back(
          {.declaration = &enumerator,
           .value = value,
           .explicitValue = enumerator.initializer != nullptr,
           .variantIndex = variantIndex});
      nextValue = payload
                      ? std::optional<EnumConstant>{EnumConstant{
                            .magnitude = variantIndex + 1}}
                      : (valueKnown ? nextEnumConstant(value) : std::nullopt);
    }

    semanticModel.recordEnumType(
        *enumDecl, EnumTypeInfo{.id = id,
                                .sourceUnit = currentSourceUnit,
                                .declaration = enumDecl,
                                .qualifiedName = qualified,
                                .namespaceScope = scope,
                                .underlyingType = underlying,
                                .enumerators = std::move(modelEnumerators),
                                .payload = payload,
                                .compilerPrivate = info.compilerPrivate});
  }
}

auto SemanticVisitor::Impl::payloadTypeAllowed(const SemanticType &type,
                                               EnumId owner) const -> bool {
  switch (type.kind) {
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
  case SemanticType::StringView:
  case SemanticType::NullPtr:
  case SemanticType::RawPointer:
    return true;
  case SemanticType::Enum:
    return type.enumId != owner && type.enumId != 0 &&
           type.enumId <= enums.size() && !enums[type.enumId - 1].payload;
  case SemanticType::Class:
    // The cleanup-free class-payload family: an ordinary concrete class
    // value may live inside an alternative. Active-cleanup and
    // borrowed-state payloads are rejected at construction, where class
    // traits are complete; the enum's own traits fold the payload's
    // copy/move/ownership facts, so a move-only payload makes the enum
    // value move-only.
    return type.classId != 0;
  default:
    return false;
  }
}

auto SemanticVisitor::Impl::registerEnumGenericParameters() -> void {
  const auto enclosingNamespace = currentNamespace;
  for (auto &info : enums) {
    if (!info.payload || info.declaration == nullptr)
      continue;
    currentSourceUnit = info.sourceUnit;
    currentNamespace = info.namespaceScope;
    info.genericParameters =
        makeGenericParameters(info.declaration->genericParameters(), info.name,
                              info.namespaceScope, true, false);
    for (std::size_t index = 0; index < info.genericParameters.size();
         ++index) {
      const auto &parameter = info.genericParameters[index];
      if (parameter.pack &&
          (index + 1 != info.genericParameters.size() ||
           genericValueParameterCount(info.genericParameters) != 0))
        report(parameter.name,
               "A nominal type pack must be the final generic parameter and "
               "cannot be combined with value parameters.",
               "GTI-S2023");
    }
  }
  currentNamespace = enclosingNamespace;
}

auto SemanticVisitor::Impl::resolveEnumGenericDefaults(EnumId id) -> void {
  if (id == 0 || id > enums.size())
    return;
  auto &info = enums[id - 1];
  if (info.defaultsState == EnumInfo::DefaultsState::Resolved ||
      info.defaultsState == EnumInfo::DefaultsState::Invalid)
    return;
  if (info.defaultsState == EnumInfo::DefaultsState::Resolving) {
    report(info.name, "Generic enum defaults form a recursive dependency.",
           "GTI-S2023");
    info.defaultsState = EnumInfo::DefaultsState::Invalid;
    return;
  }
  if (!info.payload || !info.declaration) {
    info.defaultsState = EnumInfo::DefaultsState::Resolved;
    return;
  }
  info.defaultsState = EnumInfo::DefaultsState::Resolving;
  const auto enclosingNamespace = currentNamespace;
  const auto enclosingSource = currentSourceUnit;
  auto enclosingTypes = std::move(typeParameterScopes);
  auto enclosingValues = std::move(valueParameterScopes);
  auto enclosingPacks = std::move(typePackScopes);
  auto enclosingRequirements = std::move(requirementScopes);
  typeParameterScopes.clear();
  valueParameterScopes.clear();
  typePackScopes.clear();
  requirementScopes.clear();
  currentNamespace = info.namespaceScope;
  currentSourceUnit = info.sourceUnit;
  const auto before = diagnostics.size();
  resolveGenericParameterDefaults(info.declaration->genericParameters(),
                                  info.genericParameters, info.namespaceScope);
  info.defaultsState = diagnostics.size() == before
                           ? EnumInfo::DefaultsState::Resolved
                           : EnumInfo::DefaultsState::Invalid;
  currentNamespace = enclosingNamespace;
  currentSourceUnit = enclosingSource;
  typeParameterScopes = std::move(enclosingTypes);
  valueParameterScopes = std::move(enclosingValues);
  typePackScopes = std::move(enclosingPacks);
  requirementScopes = std::move(enclosingRequirements);
}

auto SemanticVisitor::Impl::resolveEnumPayloads() -> void {
  const std::vector<std::string> enclosingNamespace = currentNamespace;
  for (EnumInfo &info : enums) {
    if (!info.payload || info.declaration == nullptr) {
      continue;
    }
    currentSourceUnit = info.sourceUnit;
    currentNamespace = info.namespaceScope;
    beginTypeParameterScope(info.genericParameters);
    std::vector<EnumeratorInfo> modelEnumerators;
    modelEnumerators.reserve(info.declaration->enumerators().size());
    for (std::size_t variantIndex = 0;
         variantIndex < info.declaration->enumerators().size();
         ++variantIndex) {
      const EnumeratorDecl &enumerator =
          info.declaration->enumerators()[variantIndex];
      const auto found = info.enumerators.find(enumerator.name.lexeme);
      std::vector<SemanticType> payloadTypes;
      payloadTypes.reserve(enumerator.payload.size());
      std::unordered_set<std::string> payloadNames;
      for (const Parameter &parameter : enumerator.payload) {
        validateType(parameter.type);
        SemanticType type = typeOf(parameter, info.namespaceScope);
        const Token &location = parameter.name.lexeme.empty()
                                    ? parameter.type.name.last()
                                    : parameter.name;
        if (parameter.name.lexeme.empty()) {
          report(location,
                 "Payload enum fields require names so switch patterns can "
                 "bind them.",
                 "GTI-S2067");
        } else if (!payloadNames.insert(parameter.name.lexeme).second) {
          report(parameter.name,
                 "Duplicate payload field name '" + parameter.name.lexeme +
                     "' in variant '" + enumerator.name.lexeme + "'.",
                 "GTI-S2067");
        }
        if (parameter.mutability == Mutability::Mutable) {
          report(location,
                 "Payload enum fields are values and do not take 'mut'.",
                 "GTI-S2067");
        }
        if (parameter.pack) {
          report(*parameter.pack, "Payload enum fields cannot be variadic.",
                 "GTI-S2067");
        }
        if (parameter.type.reference || !parameter.type.arrayExtents.empty()) {
          report(location,
                 "The first payload-enum family does not store references "
                 "or fixed arrays.",
                 "GTI-S2067");
        } else if (type != SemanticType::Unknown &&
                   !containsOpenTypeParameter(type) &&
                   !payloadTypeAllowed(type, info.id)) {
          report(location,
                 "Payload field type '" + typeSpelling(type) +
                     "' is outside the passive payload family.",
                 "GTI-S2067");
        }
        // Class payloads with active cleanup or borrowed state are rejected
        // at construction, where class traits are complete; enum payload
        // resolution runs before member metadata exists.
        payloadTypes.push_back(type);
      }
      if (found != info.enumerators.end()) {
        found->second.payloadTypes = payloadTypes;
      }
      modelEnumerators.push_back(
          {.declaration = &enumerator,
           .value = {.magnitude = variantIndex},
           .explicitValue = enumerator.initializer != nullptr,
           .variantIndex = variantIndex,
           .payloadTypes = std::move(payloadTypes)});
    }
    semanticModel.recordEnumType(
        *info.declaration,
        EnumTypeInfo{.id = info.id,
                     .sourceUnit = info.sourceUnit,
                     .declaration = info.declaration,
                     .qualifiedName =
                         qualifiedName(info.namespaceScope, info.name.lexeme),
                     .namespaceScope = info.namespaceScope,
                     .genericParameters = info.genericParameters,
                     .underlyingType = info.underlyingType,
                     .enumerators = std::move(modelEnumerators),
                     .payload = true,
                     .compilerPrivate = info.compilerPrivate});
    endTypeParameterScope();
  }
  currentNamespace = enclosingNamespace;
}

auto SemanticVisitor::Impl::validateConcreteEnumPayloads(
    const SemanticType &type, const Token &site) -> void {
  std::vector<SemanticType> visiting;
  const auto validate = [&](const auto &self,
                            const SemanticType &current) -> void {
    if (std::find(visiting.begin(), visiting.end(), current) != visiting.end())
      return;
    visiting.push_back(current);
    for (const auto &argument : current.arguments)
      self(self, argument);
    if (current.kind == SemanticType::Enum) {
      if (const auto info = enumTypeFor(current)) {
        for (const auto &variant : info->enumerators) {
          for (std::size_t index = 0; index < variant.payloadTypes.size();
               ++index) {
            const auto &payload = variant.payloadTypes[index];
            if (containsOpenTypeParameter(payload) ||
                payload == SemanticType::Unknown)
              continue;
            if (!payloadTypeAllowed(payload, current.enumId) ||
                typeTraits(payload).containsBorrowedState) {
              auto diagnostic = makeDiagnostic(
                  "GTI-S2067", DiagnosticPhase::Semantics, site,
                  "Payload field type '" + typeSpelling(payload) +
                      "' is not a supported owning or passive enum payload.");
              if (variant.declaration &&
                  index < variant.declaration->payload.size())
                diagnostic.related.push_back(
                    {tokenSpan(
                         variant.declaration->payload[index].type.name.last()),
                     "Payload field declared here."});
              diagnostics.push_back(std::move(diagnostic));
            }
          }
        }
      }
    }
    visiting.pop_back();
  };
  validate(validate, type);
}

auto SemanticVisitor::Impl::registerClassAttributes(
    const ClassDecl &declaration,
    const std::vector<GenericParameterInfo> &parameters)
    -> CapabilityPolicyRegistration {
  CapabilityPolicyRegistration result;
  const bool interface = declaration.kind() == ClassKind::Interface;
  const auto reportInvalid = [&](const Token &attribute, std::string message) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2059", DiagnosticPhase::Semantics, attribute, std::move(message));
    diagnostic.hints.emplace_back(
        interface ? "Interfaces may declare [[transfer]] and [[share]] "
                    "requirements."
                  : "Classes and structs may use [[no_transfer]], "
                    "[[no_share]], "
                    "[[unsafe_transfer]], and [[unsafe_share]], optionally "
                    "conditioning an unsafe assertion on a type parameter "
                    "with [[unsafe_transfer(T)]] or [[unsafe_share(T)]]; a "
                    "passive "
                    "struct may instead opt into [[c_abi]], while an "
                    "incomplete native handle uses [[c_opaque]]. A struct "
                    "declared [[attribute]] annotates other declarations, "
                    "and [[reflect_private]] opens private fields to "
                    "reflected access.");
    diagnostics.emplace_back(std::move(diagnostic));
  };
  const auto setPolicy = [&](const Token &attribute, bool transfer,
                             ConcurrencyCapabilityPolicy policy) {
    std::optional<Token> &source =
        transfer ? result.transferSource : result.shareSource;
    ConcurrencyCapabilityPolicy &stored =
        transfer ? result.transfer : result.share;
    if (source) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2059", DiagnosticPhase::Semantics, attribute,
          std::string("Concurrency capability '") +
              (transfer ? "transfer" : "share") +
              "' is declared more than once or with conflicting policy.");
      diagnostic.related.push_back(
          {tokenSpan(*source), "The first policy is declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
      return;
    }
    source = attribute;
    stored = policy;
  };

  for (const ClassAttribute &syntax : declaration.attributes()) {
    const Token &attribute = syntax.name;
    if (syntax.capabilityParameter) {
      const Token &parameter = *syntax.capabilityParameter;
      if (interface || (attribute.lexeme != "unsafe_transfer" &&
                        attribute.lexeme != "unsafe_share")) {
        reportInvalid(
            attribute,
            "Conditional unsafe assertions apply only to classes and structs.");
        continue;
      }
      const auto found =
          std::find_if(parameters.begin(), parameters.end(),
                       [&](const GenericParameterInfo &candidate) {
                         return candidate.name.lexeme == parameter.lexeme;
                       });
      if (found == parameters.end() || found->value || found->pack) {
        reportInvalid(
            parameter,
            "A conditional unsafe assertion must name one non-pack type "
            "parameter of this declaration.");
        continue;
      }
      const bool transfer = attribute.lexeme == "unsafe_transfer";
      const bool duplicate = transfer ? result.transferSource.has_value()
                                      : result.shareSource.has_value();
      setPolicy(attribute, transfer,
                ConcurrencyCapabilityPolicy::UnsafeConditional);
      if (!duplicate) {
        (transfer ? result.transferCondition : result.shareCondition) =
            SemanticType::typeParameter(found->id);
      }
      semanticModel.recordOccurrence(
          {.sourceUnit = currentSourceUnit,
           .span = tokenSpan(parameter),
           .kind = SemanticOccurrenceKind::Symbol,
           .symbol = symbolForDeclaration(found->name),
           .roles = OccurrenceRole::Reference | OccurrenceRole::TypeUse,
           .name = parameter.lexeme,
           .type = SemanticType::typeParameter(found->id)});
      continue;
    }
    if (attribute.lexeme == "c_abi") {
      if (result.cAbiSource) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2064", DiagnosticPhase::Semantics, attribute,
            "The C ABI record attribute is declared more than once.");
        diagnostic.related.push_back(
            {tokenSpan(*result.cAbiSource),
             "The first [[c_abi]] attribute is here."});
        diagnostics.emplace_back(std::move(diagnostic));
        continue;
      }
      result.cAbiRecord = true;
      result.cAbiSource = attribute;
      if (declaration.kind() != ClassKind::Struct) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2064", DiagnosticPhase::Semantics, attribute,
            "[[c_abi]] applies only to passive struct declarations.");
        diagnostic.hints.emplace_back(
            "Use 'struct' and keep ordinary classes and interfaces out of "
            "the C ABI.");
        diagnostics.emplace_back(std::move(diagnostic));
      }
    } else if (attribute.lexeme == "c_opaque") {
      if (result.cOpaqueSource) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2065", DiagnosticPhase::Semantics, attribute,
            "The opaque C handle attribute is declared more than once.");
        diagnostic.related.push_back(
            {tokenSpan(*result.cOpaqueSource),
             "The first [[c_opaque]] attribute is here."});
        diagnostics.emplace_back(std::move(diagnostic));
        continue;
      }
      result.cOpaqueHandle = true;
      result.cOpaqueSource = attribute;
      if (declaration.kind() != ClassKind::Struct) {
        Diagnostic diagnostic =
            makeDiagnostic("GTI-S2065", DiagnosticPhase::Semantics, attribute,
                           "[[c_opaque]] applies only to an incomplete struct "
                           "declaration.");
        diagnostic.hints.emplace_back(
            "Declare the native handle as '[[c_opaque]] struct Name;'.");
        diagnostics.emplace_back(std::move(diagnostic));
      }
    } else if (attribute.lexeme == "attribute" ||
               attribute.lexeme == "reflect_private") {
      const bool attributeTag = attribute.lexeme == "attribute";
      std::optional<Token> &source =
          attributeTag ? result.attributeSource : result.reflectPrivateSource;
      if (source) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2059", DiagnosticPhase::Semantics, attribute,
            "[[" + attribute.lexeme + "]] is declared more than once.");
        diagnostic.related.push_back(
            {tokenSpan(*source), "The first attribute is here."});
        diagnostics.emplace_back(std::move(diagnostic));
        continue;
      }
      if (interface) {
        reportInvalid(attribute, "[[" + attribute.lexeme +
                                     "]] applies to classes and structs, not "
                                     "to interfaces.");
        continue;
      }
      if (attributeTag && declaration.kind() != ClassKind::Struct) {
        report(attribute,
               "[[attribute]] applies only to struct declarations: an "
               "attribute type is a struct with public value fields, "
               "declaration defaults, and no declared constructors or bases.",
               "GTI-S2113");
        continue;
      }
      source = attribute;
      (attributeTag ? result.attributeType : result.reflectPrivate) = true;
    } else if (attribute.lexeme == "transfer") {
      if (!interface) {
        reportInvalid(attribute,
                      "'transfer' is only valid as an interface capability "
                      "requirement.");
      } else {
        setPolicy(attribute, true, ConcurrencyCapabilityPolicy::Required);
      }
    } else if (attribute.lexeme == "share") {
      if (!interface) {
        reportInvalid(attribute,
                      "'share' is only valid as an interface capability "
                      "requirement.");
      } else {
        setPolicy(attribute, false, ConcurrencyCapabilityPolicy::Required);
      }
    } else if (attribute.lexeme == "no_transfer") {
      if (interface) {
        reportInvalid(attribute,
                      "Interfaces have no transfer capability unless they "
                      "declare [[transfer]].");
      } else {
        setPolicy(attribute, true, ConcurrencyCapabilityPolicy::Denied);
      }
    } else if (attribute.lexeme == "no_share") {
      if (interface) {
        reportInvalid(attribute,
                      "Interfaces have no sharing capability unless they "
                      "declare [[share]].");
      } else {
        setPolicy(attribute, false, ConcurrencyCapabilityPolicy::Denied);
      }
    } else if (attribute.lexeme == "unsafe_transfer") {
      if (interface) {
        reportInvalid(attribute,
                      "Interfaces declare requirements; an unsafe transfer "
                      "assertion belongs on an implementing class or "
                      "struct.");
      } else {
        setPolicy(attribute, true, ConcurrencyCapabilityPolicy::UnsafeAsserted);
      }
    } else if (attribute.lexeme == "unsafe_share") {
      if (interface) {
        reportInvalid(attribute,
                      "Interfaces declare requirements; an unsafe sharing "
                      "assertion belongs on an implementing class or "
                      "struct.");
      } else {
        setPolicy(attribute, false,
                  ConcurrencyCapabilityPolicy::UnsafeAsserted);
      }
    } else {
      reportInvalid(attribute, "Unknown concurrency capability attribute '" +
                                   attribute.lexeme + "'.");
    }
  }
  if (result.cAbiSource && result.cOpaqueSource) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2065", DiagnosticPhase::Semantics, *result.cOpaqueSource,
        "An opaque C handle cannot also be a layout-stable [[c_abi]] "
        "record.");
    diagnostic.related.push_back(
        {tokenSpan(*result.cAbiSource), "Conflicting [[c_abi]] is here."});
    diagnostic.hints.emplace_back(
        "Use [[c_opaque]] for an incomplete pointer-only handle or "
        "[[c_abi]] for a complete passive record.");
    diagnostics.emplace_back(std::move(diagnostic));
  }
  if (result.cAbiSource && (result.transferSource || result.shareSource)) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2064", DiagnosticPhase::Semantics, *result.cAbiSource,
        "[[c_abi]] cannot be combined with concurrency capability "
        "attributes in the bounded native-record contract.");
    if (result.transferSource) {
      diagnostic.related.push_back(
          {tokenSpan(*result.transferSource),
           "Conflicting capability policy is declared here."});
    }
    if (result.shareSource) {
      diagnostic.related.push_back(
          {tokenSpan(*result.shareSource),
           "Conflicting capability policy is declared here."});
    }
    diagnostic.hints.emplace_back(
        "Wrap the passive C record in an ordinary GTI type when a nominal "
        "concurrency policy is required.");
    diagnostics.emplace_back(std::move(diagnostic));
  }
  if (result.cOpaqueSource && (result.transferSource || result.shareSource)) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2065", DiagnosticPhase::Semantics, *result.cOpaqueSource,
        "[[c_opaque]] cannot be combined with concurrency capability "
        "attributes; the native handle pointer owns no GTI value.");
    if (result.transferSource) {
      diagnostic.related.push_back(
          {tokenSpan(*result.transferSource),
           "Conflicting capability policy is declared here."});
    }
    if (result.shareSource) {
      diagnostic.related.push_back(
          {tokenSpan(*result.shareSource),
           "Conflicting capability policy is declared here."});
    }
    diagnostic.hints.emplace_back(
        "Put ownership and concurrency policy on an ordinary GTI wrapper, "
        "not on the opaque C declaration.");
    diagnostics.emplace_back(std::move(diagnostic));
  }
  return result;
}

auto SemanticVisitor::Impl::makeClassPatternParameters(
    const ClassDecl &declaration, const std::vector<std::string> &scope)
    -> std::vector<GenericParameterInfo> {
  std::vector<GenericParameter> parameters;
  const auto collect = [&](const auto &self, const TypeRef &type) -> void {
    if (type.qualifiedOwner) {
      self(self, *type.qualifiedOwner);
    }
    if (type.isFunctionSignature()) {
      self(self, type.arguments.back());
      for (std::size_t index = 0; index + 1 < type.arguments.size(); ++index) {
        self(self, type.arguments[index]);
      }
      return;
    }
    if (type.name.segments.size() == 1 &&
        type.name.last().kind == TokenKind::IDENTIFIER &&
        type.arguments.empty() &&
        type.genericArgumentSyntax != GenericArgumentSyntax::Value) {
      const auto found = std::find_if(
          parameters.begin(), parameters.end(), [&](const auto &parameter) {
            return parameter.name.lexeme == type.name.last().lexeme;
          });
      if (found == parameters.end()) {
        parameters.push_back(
            {.name = type.name.last(), .pack = type.packExpansion});
      } else if (found->pack.has_value() != type.packExpansion.has_value()) {
        report(type.name.last(),
               "A pattern parameter cannot be both a type and a type pack.",
               "GTI-S2023");
      }
    }
    for (const TypeRef &argument : type.arguments) {
      self(self, argument);
    }
  };
  for (const TypeRef &pattern : declaration.genericArgumentPatterns()) {
    collect(collect, pattern);
  }
  if (parameters.empty()) {
    report(declaration.name(),
           "A generic class argument pattern must declare a type parameter.",
           "GTI-S2023");
  }
  // Binding order is independent of public argument order. The ordinary
  // generic model retains its one-final-pack invariant.
  std::stable_partition(parameters.begin(), parameters.end(),
                        [](const auto &parameter) { return !parameter.pack; });
  return makeGenericParameters(parameters, declaration.name(), scope);
}

auto SemanticVisitor::Impl::resolveClassArgumentPatterns(ClassId id) -> void {
  ClassInfo &owner = classInfo(id);
  if (owner.declaration->genericArgumentPatterns().empty() ||
      owner.argumentPatternsResolved) {
    return;
  }
  if (owner.argumentPatternsResolving) {
    report(owner.name, "Generic class argument pattern forms a type cycle.",
           "GTI-S2023");
    return;
  }
  owner.argumentPatternsResolving = true;
  const auto savedNamespace = currentNamespace;
  const auto savedClass = currentClass;
  const auto savedSource = currentSourceUnit;
  currentNamespace = owner.namespaceScope;
  currentClass = id;
  currentSourceUnit = owner.sourceUnit;
  beginTypeParameterScope(owner.genericParameters);
  for (const TypeRef &pattern : owner.declaration->genericArgumentPatterns()) {
    resolveTypeAliasDependencies(pattern, owner.namespaceScope);
    validateType(pattern);
    owner.genericArgumentPatterns.push_back(typeOf(pattern));
  }
  endTypeParameterScope();
  currentNamespace = savedNamespace;
  currentClass = savedClass;
  currentSourceUnit = savedSource;

  const auto project = [&](const auto &self, const SemanticType &type,
                           std::vector<std::size_t> path) -> void {
    if (type.kind == SemanticType::TypeParameter ||
        type.kind == SemanticType::TypePack) {
      const auto found = std::find_if(
          owner.genericParameters.begin(), owner.genericParameters.end(),
          [&](const auto &parameter) {
            return parameter.id == type.genericParameterId;
          });
      if (found != owner.genericParameters.end() &&
          found->typeArgumentPath.empty()) {
        found->typeArgumentPath = std::move(path);
      }
      return;
    }
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      auto child = path;
      child.push_back(index);
      self(self, type.arguments[index], std::move(child));
    }
  };
  for (std::size_t index = 0; index < owner.genericArgumentPatterns.size();
       ++index) {
    project(project, owner.genericArgumentPatterns[index], {index});
  }
  owner.argumentPatternsResolving = false;
  owner.argumentPatternsResolved = true;
}

auto SemanticVisitor::Impl::classArgumentPatternsMatch(
    const ClassInfo &owner, const std::vector<SemanticType> &arguments) const
    -> bool {
  if (arguments.size() != owner.genericArgumentPatterns.size()) {
    return false;
  }
  const auto substitution =
      classSubstitution(SemanticType::classType(owner.id, arguments));
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto &pattern = owner.genericArgumentPatterns[index];
    if (pattern.kind == SemanticType::FunctionSignature &&
        arguments[index].kind == SemanticType::FunctionSignature &&
        pattern.signatureReceiver != arguments[index].signatureReceiver) {
      return false;
    }
    // Symbolic generic bodies are rechecked with concrete arguments. Preserve
    // their public identity rather than inventing components of an opaque S.
    if (containsOpenTypeParameter(arguments[index])) {
      continue;
    }
    if (substituteType(owner.genericArgumentPatterns[index], substitution) !=
        arguments[index]) {
      return false;
    }
  }
  return true;
}

auto SemanticVisitor::Impl::registerClasses(const StmtList &statements,
                                            std::vector<std::string> scope)
    -> void {
  for (const StmtPtr &statement : statements) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        registerClasses(*branch, scope);
      }
    } else if (const auto *variable = stmtAs<VariableDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(variable->name());
      recordGenericDefaultConstant(*variable, scope);
    } else if (const auto *classDecl = stmtAs<ClassDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(classDecl->name());
      if (classDecl->isExactSpecialization()) {
        continue;
      }
      const std::string qualified =
          qualifiedName(scope, classDecl->name().lexeme);
      ClassId familyPrimary = 0;
      if (const auto existing = classIds.find(qualified);
          existing != classIds.end()) {
        const ClassInfo &first = classInfo(existing->second);
        const auto receiver = signatureClassReceiver(*classDecl);
        if (receiver && signatureClassReceiver(*first.declaration) &&
            first.kind == classDecl->kind() &&
            samePackageOwnershipDomain(first.sourceUnit, currentSourceUnit) &&
            std::none_of(first.signatureFamilyMembers.begin(),
                         first.signatureFamilyMembers.end(),
                         [&](ClassId member) {
                           return signatureClassReceiver(
                                      *classInfo(member).declaration) ==
                                  receiver;
                         }) &&
            signatureClassReceiver(*first.declaration) != receiver) {
          familyPrimary = existing->second;
        }
      }
      if (namespaces.contains(qualified) ||
          namespaceAliases.contains(qualified) ||
          typeAliasIds.contains(qualified) || enumIds.contains(qualified) ||
          (classIds.contains(qualified) && familyPrimary == 0) ||
          conceptIds.contains(qualified)) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2006", DiagnosticPhase::Semantics, classDecl->name(),
            "Duplicate declaration of '" + classDecl->name().lexeme + "'.");
        if (const auto existing = classIds.find(qualified);
            existing != classIds.end()) {
          diagnostic.related.push_back(
              {tokenSpan(classInfo(existing->second).name),
               "Previous declaration is here."});
        }
        diagnostics.emplace_back(std::move(diagnostic));
        continue;
      }

      const ClassId id = classes.size() + 1;
      std::vector<GenericParameterInfo> genericParameters =
          classDecl->genericArgumentPatterns().empty()
              ? makeGenericParameters(classDecl->genericParameters(),
                                      classDecl->name(), scope, true, false)
              : makeClassPatternParameters(*classDecl, scope);
      for (std::size_t index = 0; index < genericParameters.size(); ++index) {
        if (genericParameters[index].pack &&
            (index + 1 != genericParameters.size() ||
             genericValueParameterCount(genericParameters) != 0)) {
          report(genericParameters[index].name,
                 "A class type pack must be the final generic parameter and "
                 "cannot be combined with value parameters.",
                 "GTI-S2023");
        }
      }
      if (classDecl->kind() == ClassKind::Union) {
        if (!genericParameters.empty() ||
            !classDecl->genericArgumentPatterns().empty()) {
          report(classDecl->name(),
                 "Unions cannot declare generic parameters in the passive "
                 "storage model.",
                 "GTI-S2066");
        }
        if (classDecl->isForwardDeclaration()) {
          report(classDecl->name(),
                 "A union must define its storage fields; forward "
                 "declarations are not supported.",
                 "GTI-S2066");
        }
      }
      classIds.emplace(qualified, id);
      publishClass(qualified, familyPrimary == 0 ? id : familyPrimary,
                   currentSourceUnit, classDecl->name());
      classDeclIds.emplace(classDecl, id);
      CompilerCapabilityTypeKind compilerCapability =
          CompilerCapabilityTypeKind::None;
      const bool trustedPreludeCapabilityScope =
          isPreludeUnit(currentSourceUnit) &&
          scope == std::vector<std::string>{"gti_internal"};
      const bool trustedThreadCapabilityScope =
          isThreadLibraryUnit(currentSourceUnit) &&
          scope == std::vector<std::string>{"gti_internal"};
      const bool trustedAtomicCapabilityScope =
          isAtomicLibraryUnit(currentSourceUnit) &&
          scope == std::vector<std::string>{"gti_internal"};
      const bool trustedMutexCapabilityScope =
          isMutexLibraryUnit(currentSourceUnit) &&
          scope == std::vector<std::string>{"gti_internal"};
      const bool trustedMemoryCapabilityScope =
          isMemoryLibraryUnit(currentSourceUnit) &&
          scope == std::vector<std::string>{"gti_internal"};
      const bool trustedOptionalCapabilityScope =
          isOptionalLibraryUnit(currentSourceUnit) &&
          scope == std::vector<std::string>{"gti_internal"};
      const bool trustedPolymorphicCapabilityScope =
          isPolymorphicLibraryUnit(currentSourceUnit) &&
          scope == std::vector<std::string>{"gti_internal"};
      const auto &generics = classDecl->genericParameters();
      if (trustedMemoryCapabilityScope &&
          classDecl->genericParameters().size() == 1 &&
          !classDecl->genericParameters().front().valueType &&
          !classDecl->genericParameters().front().pack) {
        if (classDecl->name().lexeme == "unique_owner") {
          compilerCapability = CompilerCapabilityTypeKind::UniqueOwner;
        } else if (classDecl->name().lexeme == "shared_owner") {
          compilerCapability = CompilerCapabilityTypeKind::SharedOwner;
        } else if (classDecl->name().lexeme == "uninitialized_storage") {
          compilerCapability = CompilerCapabilityTypeKind::UninitializedStorage;
        }
      } else if (trustedOptionalCapabilityScope &&
                 classDecl->genericParameters().size() == 1 &&
                 !classDecl->genericParameters().front().valueType &&
                 !classDecl->genericParameters().front().pack &&
                 classDecl->name().lexeme == "optional_storage") {
        compilerCapability = CompilerCapabilityTypeKind::OptionalStorage;
      } else if (trustedPolymorphicCapabilityScope && generics.size() == 3 &&
                 !generics[0].valueType && !generics[0].pack &&
                 generics[1].valueType && !generics[1].pack &&
                 generics[2].valueType && !generics[2].pack &&
                 classDecl->name().lexeme == "polymorphic_storage") {
        compilerCapability = CompilerCapabilityTypeKind::PolymorphicStorage;
      } else if (trustedPreludeCapabilityScope &&
                 classDecl->genericParameters().empty() &&
                 classDecl->name().lexeme == "text_view") {
        compilerCapability = CompilerCapabilityTypeKind::TextView;
      } else if (trustedPreludeCapabilityScope &&
                 classDecl->genericParameters().empty() &&
                 classDecl->name().lexeme == "c_string") {
        compilerCapability = CompilerCapabilityTypeKind::CString;
      } else if (trustedPreludeCapabilityScope &&
                 classDecl->genericParameters().empty() &&
                 parseNativeCScalarKind(classDecl->name().lexeme)) {
        compilerCapability = CompilerCapabilityTypeKind::NativeCScalar;
      } else if (trustedThreadCapabilityScope &&
                 classDecl->genericParameters().empty() &&
                 classDecl->name().lexeme == "thread_handle") {
        compilerCapability = CompilerCapabilityTypeKind::ThreadHandle;
      } else if (trustedThreadCapabilityScope &&
                 classDecl->genericParameters().size() == 1 &&
                 !classDecl->genericParameters().front().valueType &&
                 !classDecl->genericParameters().front().pack &&
                 classDecl->name().lexeme == "thread_result_handle") {
        compilerCapability = CompilerCapabilityTypeKind::ThreadHandle;
      } else if (trustedAtomicCapabilityScope &&
                 classDecl->genericParameters().size() == 1 &&
                 !classDecl->genericParameters().front().valueType &&
                 !classDecl->genericParameters().front().pack &&
                 classDecl->name().lexeme == "atomic_storage") {
        compilerCapability = CompilerCapabilityTypeKind::AtomicStorage;
      } else if (trustedMutexCapabilityScope &&
                 classDecl->genericParameters().size() == 1 &&
                 !classDecl->genericParameters().front().valueType &&
                 !classDecl->genericParameters().front().pack &&
                 classDecl->name().lexeme == "mutex_storage") {
        compilerCapability = CompilerCapabilityTypeKind::MutexStorage;
      } else if (trustedMutexCapabilityScope &&
                 classDecl->genericParameters().size() == 1 &&
                 !classDecl->genericParameters().front().valueType &&
                 !classDecl->genericParameters().front().pack &&
                 classDecl->name().lexeme == "mutex_lock_token") {
        compilerCapability = CompilerCapabilityTypeKind::MutexLockToken;
      }
      CapabilityPolicyRegistration capabilityPolicies;
      if (classDecl->kind() == ClassKind::Union) {
        for (const ClassAttribute &attribute : classDecl->attributes()) {
          report(attribute.name,
                 "Unions do not accept class capability or native-record "
                 "attributes.",
                 "GTI-S2066");
        }
      } else {
        capabilityPolicies =
            registerClassAttributes(*classDecl, genericParameters);
      }
      classes.push_back(ClassInfo{
          .id = id,
          .sourceUnit = currentSourceUnit,
          .declaration = classDecl,
          .name = classDecl->name(),
          .kind = classDecl->kind(),
          .namespaceScope = scope,
          .signatureFamilyPrimary = familyPrimary,
          .genericParameters = std::move(genericParameters),
          .cAbiRecord = capabilityPolicies.cAbiRecord,
          .cAbiAttribute = capabilityPolicies.cAbiSource,
          .cOpaqueHandle = capabilityPolicies.cOpaqueHandle,
          .cOpaqueAttribute = capabilityPolicies.cOpaqueSource,
          .attributeType = capabilityPolicies.attributeType,
          .attributeAttribute = capabilityPolicies.attributeSource,
          .reflectPrivate = capabilityPolicies.reflectPrivate,
          .reflectPrivateAttribute = capabilityPolicies.reflectPrivateSource,
          .transferPolicy = capabilityPolicies.transfer,
          .sharePolicy = capabilityPolicies.share,
          .transferPolicySource = capabilityPolicies.transferSource,
          .sharePolicySource = capabilityPolicies.shareSource,
          .transferCondition = capabilityPolicies.transferCondition,
          .shareCondition = capabilityPolicies.shareCondition,
          .compilerPrivate =
              compilerPrivateDeclaration(currentSourceUnit, scope),
          .compilerCapability = compilerCapability,
          .nativeCScalar =
              compilerCapability == CompilerCapabilityTypeKind::NativeCScalar
                  ? *parseNativeCScalarKind(classDecl->name().lexeme)
                  : NativeCScalarKind::None});
      if (const std::optional<CompilerTypeBinding> &binding =
              classDecl->compilerTypeBinding()) {
        const std::optional<CompilerTypeKind> kind =
            compilerTypeKind(binding->name);
        if (!kind || !isMetaLibraryUnit(currentSourceUnit)) {
          report(binding->attribute,
                 "@compiler_type is reserved for compiler-owned classes "
                 "declared in their trusted standard-library unit.",
                 "GTI-S2049");
        } else {
          classes.back().compilerType = *kind;
          compilerTypeClasses.insert_or_assign(*kind, id);
        }
      }
      if (familyPrimary != 0) {
        auto &members = classInfo(familyPrimary).signatureFamilyMembers;
        if (members.empty()) {
          members.push_back(familyPrimary);
        }
        members.push_back(id);
      }
    } else if (const auto *namespaceDecl =
                   stmtAs<NamespaceDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(namespaceDecl->name());
      scope.emplace_back(namespaceDecl->name().lexeme);
      registerClasses(namespaceDecl->declarations(), scope);
      scope.pop_back();
    }
  }
}

auto SemanticVisitor::Impl::resolveClassGenericDefaults(ClassId id) -> void {
  if (id == 0 || id > classes.size())
    return;
  auto &info = classInfo(id);
  if (info.defaultsState == ClassInfo::DefaultsState::Resolved ||
      info.defaultsState == ClassInfo::DefaultsState::Invalid)
    return;
  if (info.defaultsState == ClassInfo::DefaultsState::Resolving) {
    report(info.name, "Generic class defaults form a recursive dependency.",
           "GTI-S2023");
    info.defaultsState = ClassInfo::DefaultsState::Invalid;
    return;
  }
  if (!info.declaration || info.genericParameters.empty() ||
      !info.declaration->genericArgumentPatterns().empty()) {
    info.defaultsState = ClassInfo::DefaultsState::Resolved;
    return;
  }
  info.defaultsState = ClassInfo::DefaultsState::Resolving;
  const auto enclosingNamespace = currentNamespace;
  const auto enclosingSource = currentSourceUnit;
  auto enclosingTypes = std::move(typeParameterScopes);
  auto enclosingValues = std::move(valueParameterScopes);
  auto enclosingPacks = std::move(typePackScopes);
  auto enclosingRequirements = std::move(requirementScopes);
  typeParameterScopes.clear();
  valueParameterScopes.clear();
  typePackScopes.clear();
  requirementScopes.clear();
  currentNamespace = info.namespaceScope;
  currentSourceUnit = info.sourceUnit;
  const auto before = diagnostics.size();
  resolveGenericParameterDefaults(info.declaration->genericParameters(),
                                  info.genericParameters, info.namespaceScope);
  info.defaultsState = diagnostics.size() == before
                           ? ClassInfo::DefaultsState::Resolved
                           : ClassInfo::DefaultsState::Invalid;
  currentNamespace = enclosingNamespace;
  currentSourceUnit = enclosingSource;
  typeParameterScopes = std::move(enclosingTypes);
  valueParameterScopes = std::move(enclosingValues);
  typePackScopes = std::move(enclosingPacks);
  requirementScopes = std::move(enclosingRequirements);
}

auto SemanticVisitor::Impl::canonicalExactSpecializationType(
    SemanticType type) const -> SemanticType {
  for (SemanticType &argument : type.arguments) {
    argument = canonicalExactSpecializationType(std::move(argument));
  }
  for (SemanticType &argument : type.lambdaEnclosingClassTypes) {
    argument = canonicalExactSpecializationType(std::move(argument));
  }
  for (SemanticType &argument : type.lambdaEnclosingFunctionTypes) {
    argument = canonicalExactSpecializationType(std::move(argument));
  }
  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size()) {
    return type;
  }
  const ClassInfo &owner = classInfo(type.classId);
  if (owner.exactSpecializationPrimary == 0) {
    return type;
  }
  return canonicalExactSpecializationType(SemanticType::classType(
      owner.exactSpecializationPrimary, owner.exactTypeArguments,
      owner.exactValueArguments));
}

auto SemanticVisitor::Impl::selectSignatureClassDefinition(
    ClassId id, const std::vector<SemanticType> &arguments) const -> ClassId {
  if (id == 0 || id > classes.size() || arguments.size() != 1 ||
      arguments.front().kind != SemanticType::FunctionSignature) {
    return id;
  }
  const auto primary = classInfo(id).signatureFamilyPrimary;
  const auto &family = classInfo(primary == 0 ? id : primary);
  for (ClassId member : family.signatureFamilyMembers) {
    const auto &candidate = classInfo(member);
    if (signatureClassReceiver(*candidate.declaration) ==
        arguments.front().signatureReceiver) {
      return member;
    }
  }
  return id;
}

auto SemanticVisitor::Impl::compilerTypeKind(std::string_view binding)
    -> std::optional<CompilerTypeKind> {
  if (binding == "meta_list") {
    return CompilerTypeKind::MetaList;
  }
  if (binding == "meta_field") {
    return CompilerTypeKind::MetaField;
  }
  if (binding == "meta_type") {
    return CompilerTypeKind::MetaType;
  }
  if (binding == "meta_field_list") {
    return CompilerTypeKind::MetaFieldList;
  }
  if (binding == "meta_field_type") {
    return CompilerTypeKind::MetaFieldType;
  }
  if (binding == "meta_field_attributes") {
    return CompilerTypeKind::MetaFieldAttributes;
  }
  if (binding == "meta_type_attributes") {
    return CompilerTypeKind::MetaTypeAttributes;
  }
  if (binding == "meta_enumerator") {
    return CompilerTypeKind::MetaEnumerator;
  }
  if (binding == "meta_payload_field") {
    return CompilerTypeKind::MetaPayloadField;
  }
  if (binding == "meta_enumerator_list") {
    return CompilerTypeKind::MetaEnumeratorList;
  }
  if (binding == "meta_payload_list") {
    return CompilerTypeKind::MetaPayloadList;
  }
  if (binding == "meta_payload_type") {
    return CompilerTypeKind::MetaPayloadType;
  }
  if (binding == "meta_base_list") {
    return CompilerTypeKind::MetaBaseList;
  }
  if (binding == "meta_argument_list") {
    return CompilerTypeKind::MetaArgumentList;
  }
  static constexpr std::pair<std::string_view, CompilerTypeKind> callables[] = {
      {"meta_method", CompilerTypeKind::MetaMethod},
      {"meta_parameter", CompilerTypeKind::MetaParameter},
      {"meta_constructor", CompilerTypeKind::MetaConstructor},
      {"meta_constructor_parameter",
       CompilerTypeKind::MetaConstructorParameter},
      {"meta_method_list", CompilerTypeKind::MetaMethodList},
      {"meta_operator_list", CompilerTypeKind::MetaOperatorList},
      {"meta_parameter_list", CompilerTypeKind::MetaParameterList},
      {"meta_parameter_type", CompilerTypeKind::MetaParameterType},
      {"meta_return_type", CompilerTypeKind::MetaReturnType},
      {"meta_constructor_list", CompilerTypeKind::MetaConstructorList},
      {"meta_constructor_parameter_list",
       CompilerTypeKind::MetaConstructorParameterList},
      {"meta_constructor_parameter_type",
       CompilerTypeKind::MetaConstructorParameterType},
      {"meta_list_first", CompilerTypeKind::MetaListFirst},
      {"meta_list_rest", CompilerTypeKind::MetaListRest},
  };
  for (const auto &[name, kind] : callables) {
    if (binding == name) {
      return kind;
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::compilerTypeClass(CompilerTypeKind kind) const
    -> ClassId {
  const auto found = compilerTypeClasses.find(kind);
  return found == compilerTypeClasses.end() ? 0 : found->second;
}

auto SemanticVisitor::Impl::metaFieldCount(const SemanticType &type) const
    -> std::uint64_t {
  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size()) {
    return 0;
  }
  return classInfo(type.classId).fields.size();
}

auto SemanticVisitor::Impl::metaTypeKind(const SemanticType &type) const
    -> std::uint64_t {
  // Mirrors `std::meta::kind` in stdlib/std/meta.gti.
  switch (type.kind) {
  case SemanticType::Class: {
    if (type.classId == 0 || type.classId > classes.size()) {
      return 0;
    }
    switch (classInfo(type.classId).kind) {
    case ClassKind::Struct:
      return 1;
    case ClassKind::Class:
      return 2;
    case ClassKind::Interface:
      return 3;
    case ClassKind::Union:
      return 4;
    }
    return 0;
  }
  case SemanticType::Enum:
    return 5;
  case SemanticType::Int8:
  case SemanticType::Int16:
  case SemanticType::Int32:
  case SemanticType::Int64:
  case SemanticType::UInt8:
  case SemanticType::UInt16:
  case SemanticType::UInt32:
  case SemanticType::UInt64:
    return 6;
  case SemanticType::Float:
  case SemanticType::Double:
    return 7;
  case SemanticType::Bool:
    return 8;
  case SemanticType::Char:
    return 9;
  case SemanticType::StringView:
  case SemanticType::CString:
    return 10;
  case SemanticType::Reference:
    return 11;
  case SemanticType::Array:
    return 12;
  case SemanticType::RawPointer:
    return 13;
  case SemanticType::UniqueOwner:
    return 14;
  case SemanticType::SharedPointer:
    return 15;
  case SemanticType::Function:
  case SemanticType::FunctionItem:
  case SemanticType::NativeFunction:
  case SemanticType::Lambda:
    return 16;
  case SemanticType::Void:
    return 17;
  default:
    return 0;
  }
}

auto SemanticVisitor::Impl::metaFieldFacts(const SemanticType &owner,
                                           std::uint64_t index) const
    -> std::optional<MetaFieldFacts> {
  if (owner.kind != SemanticType::Class || owner.classId == 0 ||
      owner.classId > classes.size()) {
    return std::nullopt;
  }
  const ClassInfo &info = classInfo(owner.classId);
  if (index >= info.fields.size() ||
      info.fields[index].declaration == nullptr) {
    return std::nullopt;
  }
  const VariableDecl &declaration = *info.fields[index].declaration;
  const auto member = info.members.find(declaration.name().lexeme);
  if (member == info.members.end()) {
    return std::nullopt;
  }
  return MetaFieldFacts{
      .name = declaration.name().lexeme,
      .isPublic = member->second.access == AccessModifier::Public,
      .type = substituteSymbol(member->second.symbol, owner).type,
      .declaration = &declaration};
}

auto SemanticVisitor::Impl::rewriteCompilerType(SemanticType type) const
    -> SemanticType {
  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size()) {
    return type;
  }
  const ClassInfo &info = classInfo(type.classId);
  switch (info.compilerType) {
  case CompilerTypeKind::MetaFieldList: {
    const ClassId list = compilerTypeClass(CompilerTypeKind::MetaList);
    const ClassId field = compilerTypeClass(CompilerTypeKind::MetaField);
    if (type.arguments.size() != 1 || list == 0 || field == 0 ||
        containsOpenTypeParameter(type.arguments.front())) {
      return type;
    }
    const SemanticType &subject = type.arguments.front();
    std::vector<SemanticType> elements;
    const std::uint64_t count = metaFieldCount(subject);
    elements.reserve(count);
    for (std::uint64_t index = 0; index < count; ++index) {
      elements.push_back(SemanticType::classType(
          field, {subject}, {CompileTimeValue::uint64(index)}));
    }
    return SemanticType::classType(list, std::move(elements));
  }
  case CompilerTypeKind::MetaFieldType: {
    if (type.arguments.size() != 1 || type.valueArguments.size() != 1 ||
        type.valueArguments.front().kind != CompileTimeValue::UInt64 ||
        containsOpenTypeParameter(type.arguments.front())) {
      return type;
    }
    const std::optional<MetaFieldFacts> facts = metaFieldFacts(
        type.arguments.front(), type.valueArguments.front().value);
    return facts ? facts->type : type;
  }
  case CompilerTypeKind::MetaFieldAttributes:
  case CompilerTypeKind::MetaTypeAttributes: {
    const ClassId list = compilerTypeClass(CompilerTypeKind::MetaList);
    const bool field =
        info.compilerType == CompilerTypeKind::MetaFieldAttributes;
    if (list == 0 || type.arguments.size() != 1 ||
        containsOpenTypeParameter(type.arguments.front()) ||
        (field &&
         (type.valueArguments.size() != 1 ||
          type.valueArguments.front().kind != CompileTimeValue::UInt64))) {
      return type;
    }
    const std::vector<ResolvedDeclarationAttribute> *attributes =
        declarationAttributesOf(
            type.arguments.front(),
            field ? std::optional(type.valueArguments.front().value)
                  : std::nullopt);
    if (attributes == nullptr) {
      return type;
    }
    std::vector<SemanticType> elements;
    elements.reserve(attributes->size());
    for (const ResolvedDeclarationAttribute &attribute : *attributes) {
      elements.push_back(attribute.exactType);
    }
    return SemanticType::classType(list, std::move(elements));
  }
  case CompilerTypeKind::MetaEnumeratorList: {
    const ClassId list = compilerTypeClass(CompilerTypeKind::MetaList);
    const ClassId enumerator =
        compilerTypeClass(CompilerTypeKind::MetaEnumerator);
    if (list == 0 || enumerator == 0 || type.arguments.size() != 1 ||
        containsOpenTypeParameter(type.arguments.front())) {
      return type;
    }
    const SemanticType &subject = type.arguments.front();
    std::vector<SemanticType> elements;
    const std::uint64_t count = metaEnumeratorCount(subject);
    elements.reserve(count);
    for (std::uint64_t index = 0; index < count; ++index) {
      elements.push_back(SemanticType::classType(
          enumerator, {subject}, {CompileTimeValue::uint64(index)}));
    }
    return SemanticType::classType(list, std::move(elements));
  }
  case CompilerTypeKind::MetaPayloadList: {
    const ClassId list = compilerTypeClass(CompilerTypeKind::MetaList);
    const ClassId payload =
        compilerTypeClass(CompilerTypeKind::MetaPayloadField);
    if (list == 0 || payload == 0 || type.arguments.size() != 1 ||
        type.valueArguments.size() != 1 ||
        type.valueArguments.front().kind != CompileTimeValue::UInt64 ||
        containsOpenTypeParameter(type.arguments.front())) {
      return type;
    }
    const SemanticType &subject = type.arguments.front();
    const CompileTimeValue &variant = type.valueArguments.front();
    const std::optional<MetaEnumeratorFacts> facts =
        metaEnumeratorFacts(subject, variant.value);
    if (!facts) {
      return type;
    }
    std::vector<SemanticType> elements;
    elements.reserve(facts->payloadTypes.size());
    for (std::uint64_t index = 0; index < facts->payloadTypes.size(); ++index) {
      elements.push_back(SemanticType::classType(
          payload, {subject}, {variant, CompileTimeValue::uint64(index)}));
    }
    return SemanticType::classType(list, std::move(elements));
  }
  case CompilerTypeKind::MetaPayloadType: {
    if (type.arguments.size() != 1 || type.valueArguments.size() != 2 ||
        type.valueArguments[0].kind != CompileTimeValue::UInt64 ||
        type.valueArguments[1].kind != CompileTimeValue::UInt64 ||
        containsOpenTypeParameter(type.arguments.front())) {
      return type;
    }
    const std::optional<MetaEnumeratorFacts> facts = metaEnumeratorFacts(
        type.arguments.front(), type.valueArguments[0].value);
    const std::uint64_t position = type.valueArguments[1].value;
    return facts && position < facts->payloadTypes.size()
               ? facts->payloadTypes[position]
               : type;
  }
  case CompilerTypeKind::MetaBaseList:
  case CompilerTypeKind::MetaArgumentList: {
    const ClassId list = compilerTypeClass(CompilerTypeKind::MetaList);
    const ClassId metaType = compilerTypeClass(CompilerTypeKind::MetaType);
    if (list == 0 || metaType == 0 || type.arguments.size() != 1 ||
        containsOpenTypeParameter(type.arguments.front())) {
      return type;
    }
    const SemanticType &subject = type.arguments.front();
    std::vector<SemanticType> described;
    if (info.compilerType == CompilerTypeKind::MetaBaseList) {
      if (const ClassInfo *owner = subject.kind == SemanticType::Class
                                       ? classInfo(subject)
                                       : nullptr) {
        const GenericSubstitution substitution = classSubstitution(subject);
        for (const ClassBaseTypeInfo &base : owner->bases) {
          described.push_back(substituteType(base.type, substitution));
        }
      }
    } else {
      described = metaTypeArguments(subject);
    }
    std::vector<SemanticType> elements;
    elements.reserve(described.size());
    for (SemanticType &element : described) {
      elements.push_back(
          SemanticType::classType(metaType, {std::move(element)}));
    }
    return SemanticType::classType(list, std::move(elements));
  }
  case CompilerTypeKind::MetaMethodList:
  case CompilerTypeKind::MetaOperatorList:
  case CompilerTypeKind::MetaConstructorList: {
    const ClassId list = compilerTypeClass(CompilerTypeKind::MetaList);
    const bool constructors =
        info.compilerType == CompilerTypeKind::MetaConstructorList;
    const ClassId element =
        compilerTypeClass(constructors ? CompilerTypeKind::MetaConstructor
                                       : CompilerTypeKind::MetaMethod);
    if (list == 0 || element == 0 || type.arguments.size() != 1 ||
        containsOpenTypeParameter(type.arguments.front())) {
      return type;
    }
    const SemanticType &subject = type.arguments.front();
    std::vector<SemanticType> elements;
    if (constructors) {
      const ClassInfo *owner =
          subject.kind == SemanticType::Class ? classInfo(subject) : nullptr;
      const std::uint64_t count =
          owner == nullptr ? 0 : owner->constructors.size();
      for (std::uint64_t index = 0; index < count; ++index) {
        elements.push_back(SemanticType::classType(
            element, {subject}, {CompileTimeValue::uint64(index)}));
      }
    } else {
      const std::vector<const FunctionDecl *> methods =
          metaMethodDeclarations(subject);
      for (std::uint64_t index = 0; index < methods.size(); ++index) {
        if (info.compilerType == CompilerTypeKind::MetaOperatorList &&
            !methods[index]->operatorName()) {
          continue;
        }
        elements.push_back(SemanticType::classType(
            element, {subject}, {CompileTimeValue::uint64(index)}));
      }
    }
    return SemanticType::classType(list, std::move(elements));
  }
  case CompilerTypeKind::MetaParameterList:
  case CompilerTypeKind::MetaConstructorParameterList: {
    const ClassId list = compilerTypeClass(CompilerTypeKind::MetaList);
    const bool constructors =
        info.compilerType == CompilerTypeKind::MetaConstructorParameterList;
    const ClassId element = compilerTypeClass(
        constructors ? CompilerTypeKind::MetaConstructorParameter
                     : CompilerTypeKind::MetaParameter);
    if (list == 0 || element == 0 || type.arguments.size() != 1 ||
        type.valueArguments.size() != 1 ||
        type.valueArguments.front().kind != CompileTimeValue::UInt64 ||
        containsOpenTypeParameter(type.arguments.front())) {
      return type;
    }
    const SemanticType &subject = type.arguments.front();
    const CompileTimeValue &callable = type.valueArguments.front();
    std::size_t count = 0;
    if (constructors) {
      const std::optional<MetaConstructorFacts> facts =
          metaConstructorFacts(subject, callable.value);
      if (!facts) {
        return type;
      }
      count = facts->parameterTypes.size();
    } else {
      const std::optional<MetaMethodFacts> facts =
          metaMethodFacts(subject, callable.value);
      if (!facts) {
        return type;
      }
      count = facts->parameterTypes.size();
    }
    std::vector<SemanticType> elements;
    for (std::uint64_t index = 0; index < count; ++index) {
      elements.push_back(SemanticType::classType(
          element, {subject}, {callable, CompileTimeValue::uint64(index)}));
    }
    return SemanticType::classType(list, std::move(elements));
  }
  case CompilerTypeKind::MetaParameterType:
  case CompilerTypeKind::MetaConstructorParameterType: {
    if (type.arguments.size() != 1 || type.valueArguments.size() != 2 ||
        type.valueArguments[0].kind != CompileTimeValue::UInt64 ||
        type.valueArguments[1].kind != CompileTimeValue::UInt64 ||
        containsOpenTypeParameter(type.arguments.front())) {
      return type;
    }
    const std::uint64_t position = type.valueArguments[1].value;
    if (info.compilerType == CompilerTypeKind::MetaConstructorParameterType) {
      const std::optional<MetaConstructorFacts> facts = metaConstructorFacts(
          type.arguments.front(), type.valueArguments[0].value);
      return facts && position < facts->parameterTypes.size()
                 ? facts->parameterTypes[position]
                 : type;
    }
    const std::optional<MetaMethodFacts> facts =
        metaMethodFacts(type.arguments.front(), type.valueArguments[0].value);
    return facts && position < facts->parameterTypes.size()
               ? facts->parameterTypes[position]
               : type;
  }
  case CompilerTypeKind::MetaListFirst:
  case CompilerTypeKind::MetaListRest: {
    const ClassId list = compilerTypeClass(CompilerTypeKind::MetaList);
    if (list == 0 || type.arguments.size() != 1 ||
        containsOpenTypeParameter(type.arguments.front())) {
      return type;
    }
    const std::optional<std::vector<SemanticType>> elements =
        compileTimeSequenceElements(type.arguments.front());
    if (!elements || elements->empty()) {
      return type;
    }
    if (info.compilerType == CompilerTypeKind::MetaListFirst) {
      return elements->front();
    }
    return SemanticType::classType(
        list,
        std::vector<SemanticType>(elements->begin() + 1, elements->end()));
  }
  case CompilerTypeKind::MetaReturnType: {
    if (type.arguments.size() != 1 || type.valueArguments.size() != 1 ||
        type.valueArguments.front().kind != CompileTimeValue::UInt64 ||
        containsOpenTypeParameter(type.arguments.front())) {
      return type;
    }
    const std::optional<MetaMethodFacts> facts = metaMethodFacts(
        type.arguments.front(), type.valueArguments.front().value);
    return facts ? facts->returnType : type;
  }
  default:
    return type;
  }
}

auto SemanticVisitor::Impl::metaMethodDeclarations(
    const SemanticType &type) const -> std::vector<const FunctionDecl *> {
  std::vector<const FunctionDecl *> methods;
  const ClassInfo *owner =
      type.kind == SemanticType::Class ? classInfo(type) : nullptr;
  if (owner == nullptr || owner->declaration == nullptr) {
    return methods;
  }
  const auto visit = [&](const auto &self, const StmtList &members) -> void {
    for (const StmtPtr &statement : members) {
      if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
        if (const StmtList *active = conditional->activeBranch(target)) {
          self(self, *active);
        }
      } else if (const auto *function = stmtAs<FunctionDecl>(statement.get())) {
        methods.push_back(function);
      }
    }
  };
  visit(visit, owner->declaration->members());
  return methods;
}

auto SemanticVisitor::Impl::metaMethodFacts(const SemanticType &type,
                                            std::uint64_t index) const
    -> std::optional<MetaMethodFacts> {
  const std::vector<const FunctionDecl *> methods =
      metaMethodDeclarations(type);
  if (index >= methods.size()) {
    return std::nullopt;
  }
  const FunctionDecl &declaration = *methods[index];
  const FunctionInfo *function = semanticModel.findFunction(declaration);
  const ClassInfo *owner = classInfo(type);
  if (function == nullptr || owner == nullptr) {
    return std::nullopt;
  }
  const GenericSubstitution substitution = classSubstitution(type);
  MetaMethodFacts facts{
      .name = declaration.operatorName()
                  ? "operator" + declaration.operatorName()->symbol.lexeme
                  : declaration.name().lexeme,
      .declaration = &declaration,
      .staticMember = function->staticMember,
      .operatorMethod = declaration.operatorName().has_value(),
      .constexprFunction = function->constexprFunction,
      .virtualMethod = function->virtualMethod,
      .mutableReceiver =
          declaration.receiverMutability() != ReceiverMutability::ReadOnly,
      .returnType = substituteType(function->returnType, substitution)};
  const auto member = owner->members.find(declaration.name().lexeme);
  if (member != owner->members.end()) {
    for (const CallableCandidate &overload : member->second.symbol.overloads) {
      if (overload.declaration == &declaration) {
        facts.publicAccess = overload.access == AccessModifier::Public;
      }
    }
  }
  for (const SemanticType &parameter : function->parameterTypes) {
    facts.parameterTypes.push_back(substituteType(parameter, substitution));
  }
  for (const Parameter &parameter : declaration.parameters()) {
    facts.parameterNames.push_back(parameter.name.lexeme);
  }
  return facts;
}

auto SemanticVisitor::Impl::metaConstructorFacts(const SemanticType &type,
                                                 std::uint64_t index) const
    -> std::optional<MetaConstructorFacts> {
  const ClassInfo *owner =
      type.kind == SemanticType::Class ? classInfo(type) : nullptr;
  if (owner == nullptr || index >= owner->constructors.size() ||
      owner->constructors[index].declaration == nullptr) {
    return std::nullopt;
  }
  const ConstructorInfo &constructor = owner->constructors[index];
  const GenericSubstitution substitution = classSubstitution(type);
  MetaConstructorFacts facts{.declaration = constructor.declaration};
  for (const SemanticType &parameter : constructor.parameterTypes) {
    facts.parameterTypes.push_back(substituteType(parameter, substitution));
  }
  for (const Parameter &parameter : constructor.declaration->parameters()) {
    facts.parameterNames.push_back(parameter.name.lexeme);
  }
  return facts;
}

auto SemanticVisitor::Impl::metaDeclarationLine(const SemanticType &type,
                                                std::uint64_t kind,
                                                std::uint64_t index) const
    -> std::optional<std::uint64_t> {
  const auto line = [](const Token &token) {
    return std::optional(static_cast<std::uint64_t>(std::max(token.line, 0)));
  };
  switch (kind) {
  case 0:
    if (type.kind == SemanticType::Class) {
      const ClassInfo *owner = classInfo(type);
      return owner == nullptr ? std::nullopt : line(owner->name);
    }
    if (type.kind == SemanticType::Enum && type.enumId != 0 &&
        type.enumId <= enums.size()) {
      return line(enums[type.enumId - 1].name);
    }
    return std::nullopt;
  case 1: {
    const std::optional<MetaFieldFacts> facts = metaFieldFacts(type, index);
    return facts && facts->declaration != nullptr
               ? line(facts->declaration->name())
               : std::nullopt;
  }
  case 2: {
    if (type.kind != SemanticType::Enum) {
      return std::nullopt;
    }
    const std::optional<EnumTypeInfo> info = enumTypeFor(type);
    return info && index < info->enumerators.size() &&
                   info->enumerators[index].declaration != nullptr
               ? line(info->enumerators[index].declaration->name)
               : std::nullopt;
  }
  case 3: {
    const std::optional<MetaMethodFacts> facts = metaMethodFacts(type, index);
    return facts ? line(facts->declaration->name()) : std::nullopt;
  }
  case 4: {
    const std::optional<MetaConstructorFacts> facts =
        metaConstructorFacts(type, index);
    return facts ? line(facts->declaration->name()) : std::nullopt;
  }
  default:
    return std::nullopt;
  }
}

auto SemanticVisitor::Impl::metaDeclarationFile(const SemanticType &type) const
    -> std::string {
  SourceUnitId unit = 0;
  if (type.kind == SemanticType::Class) {
    const ClassInfo *owner = classInfo(type);
    unit = owner == nullptr ? 0 : owner->sourceUnit;
  } else if (type.kind == SemanticType::Enum && type.enumId != 0 &&
             type.enumId <= enums.size()) {
    unit = enums[type.enumId - 1].sourceUnit;
  }
  if (unit == 0 || sourceGraph == nullptr) {
    return "";
  }
  const SourceUnit *source = sourceGraph->findUnit(unit);
  return source == nullptr ? "" : source->path.string();
}

auto SemanticVisitor::Impl::isSameOrDerivedClass(
    const SemanticType &type, const SemanticType &owner) const -> bool {
  SemanticType current = type;
  for (std::size_t depth = 0; depth < 64; ++depth) {
    if (current == owner) {
      return true;
    }
    const ClassInfo *info =
        current.kind == SemanticType::Class ? classInfo(current) : nullptr;
    if (info == nullptr || info->bases.empty()) {
      return false;
    }
    // GTI inheritance is public and single along the class chain; interfaces
    // carry no fields, so the first non-interface base is the chain.
    const GenericSubstitution substitution = classSubstitution(current);
    SemanticType next = SemanticType::Unknown;
    for (const ClassBaseTypeInfo &base : info->bases) {
      if (!base.interface) {
        next = substituteType(base.type, substitution);
        break;
      }
    }
    if (next == SemanticType::Unknown) {
      return false;
    }
    current = next;
  }
  return false;
}

auto SemanticVisitor::Impl::metaEnumeratorCount(const SemanticType &type) const
    -> std::uint64_t {
  if (type.kind != SemanticType::Enum) {
    return 0;
  }
  const std::optional<EnumTypeInfo> info = enumTypeFor(type);
  return info ? info->enumerators.size() : 0;
}

auto SemanticVisitor::Impl::metaEnumeratorFacts(const SemanticType &type,
                                                std::uint64_t index) const
    -> std::optional<MetaEnumeratorFacts> {
  if (type.kind != SemanticType::Enum) {
    return std::nullopt;
  }
  const std::optional<EnumTypeInfo> info = enumTypeFor(type);
  if (!info || index >= info->enumerators.size() ||
      info->enumerators[index].declaration == nullptr) {
    return std::nullopt;
  }
  const EnumeratorInfo &enumerator = info->enumerators[index];
  const std::optional<CheckedIntegerDomain> domain =
      constantIntegerDomain(info->underlyingType);
  MetaEnumeratorFacts facts{
      .name = enumerator.declaration->name.lexeme,
      .value = ConstantValue{ConstantEnum{
          .enumId = type.enumId,
          .value =
              ConstantInteger{.negative = enumerator.value.negative,
                              .magnitude = enumerator.value.magnitude,
                              .domain =
                                  domain ? *domain
                                         : CheckedIntegerDomain{.width = 32}}}},
      .payloadTypes = enumerator.payloadTypes,
      .payloadEnum = info->payload};
  for (const Parameter &parameter : enumerator.declaration->payload) {
    facts.payloadNames.push_back(parameter.name.lexeme);
  }
  return facts;
}

auto SemanticVisitor::Impl::metaTypeArguments(const SemanticType &type) const
    -> std::vector<SemanticType> {
  std::vector<SemanticType> arguments;
  if (type.kind != SemanticType::Class && type.kind != SemanticType::Enum) {
    return arguments;
  }
  for (const SemanticType &argument : type.arguments) {
    if (argument.kind == SemanticType::TypePack && argument.concretePack) {
      arguments.insert(arguments.end(), argument.arguments.begin(),
                       argument.arguments.end());
    } else {
      arguments.push_back(argument);
    }
  }
  return arguments;
}

auto SemanticVisitor::Impl::declarationAttributesOf(
    const SemanticType &subject, std::optional<std::uint64_t> index) const
    -> const std::vector<ResolvedDeclarationAttribute> * {
  if (subject.kind != SemanticType::Class || subject.classId == 0 ||
      subject.classId > classes.size()) {
    return nullptr;
  }
  const Stmt *owner = nullptr;
  if (index) {
    const std::optional<MetaFieldFacts> facts = metaFieldFacts(subject, *index);
    owner = facts ? facts->declaration : nullptr;
  } else {
    owner = classInfo(subject.classId).declaration;
  }
  if (owner == nullptr) {
    return nullptr;
  }
  const std::vector<ResolvedDeclarationAttribute> *attributes =
      semanticModel.findDeclarationAttributes(*owner);
  return attributes == nullptr ? &noDeclarationAttributes : attributes;
}

auto SemanticVisitor::Impl::isReflectableType(const SemanticType &type) const
    -> bool {
  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size()) {
    return false;
  }
  const ClassInfo &info = classInfo(type.classId);
  if (info.kind != ClassKind::Struct && info.kind != ClassKind::Class) {
    return false;
  }
  if (info.reflectPrivate) {
    return true;
  }
  return std::all_of(
      info.fields.begin(), info.fields.end(), [&](const FieldInfo &field) {
        if (field.declaration == nullptr) {
          return false;
        }
        const auto member = info.members.find(field.declaration->name().lexeme);
        return member != info.members.end() &&
               member->second.access == AccessModifier::Public;
      });
}

// Declared attributes are constants of the attribute struct built by
// positional or designated initialization of its fields; a field without an
// argument takes its declaration default, which must be a literal because the
// struct's own body has not been analysed yet when attributes are evaluated.
auto SemanticVisitor::Impl::analyzeDeclarationAttributes() -> void {
  const std::vector<std::string> enclosingNamespace = currentNamespace;
  const SourceUnitId enclosingUnit = currentSourceUnit;
  const std::optional<ClassId> enclosingClass = currentClass;
  // Analysing an argument may register generic instances, so iterate by
  // index and copy what the loop body needs before analysing.
  for (std::size_t position = 0; position < classes.size(); ++position) {
    const ClassDecl *declaration = classes[position].declaration;
    if (declaration == nullptr ||
        !analyzedDeclarationAttributeOwners.insert(declaration).second) {
      continue;
    }
    const std::vector<std::string> scope = classes[position].namespaceScope;
    const SourceUnitId unit = classes[position].sourceUnit;
    std::vector<const VariableDecl *> fields;
    for (const FieldInfo &field : classes[position].fields) {
      if (field.declaration != nullptr &&
          !field.declaration->declarationAttributes().empty()) {
        fields.push_back(field.declaration);
      }
    }
    if (declaration->declarationAttributes().empty() && fields.empty()) {
      continue;
    }
    enterDeclarationContext(scope, unit, std::nullopt);
    analyzeDeclarationAttributeList(declaration->declarationAttributes(),
                                    *declaration);
    for (const VariableDecl *field : fields) {
      analyzeDeclarationAttributeList(field->declarationAttributes(), *field);
    }
    exitDeclarationContext(std::nullopt);
  }
  currentNamespace = enclosingNamespace;
  currentSourceUnit = enclosingUnit;
  currentClass = enclosingClass;
}

auto SemanticVisitor::Impl::analyzeDeclarationAttributeList(
    const std::vector<DeclarationAttribute> &attributes, const Stmt &owner)
    -> void {
  std::vector<ResolvedDeclarationAttribute> resolved;
  for (const DeclarationAttribute &syntax : attributes) {
    const Token &anchor = syntax.name.last();
    const std::string spelling = pathSpelling(syntax.name);
    const TypeRef reference(syntax.name);
    validateType(reference);
    const SemanticType type = typeOf(reference);
    if (type == SemanticType::Unknown) {
      continue;
    }
    const ClassInfo *attribute =
        type.kind == SemanticType::Class ? classInfo(type) : nullptr;
    if (attribute == nullptr || !attribute->attributeType) {
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2113", DiagnosticPhase::Semantics, anchor,
                         "'" + spelling + "' is not an attribute type.");
      diagnostic.hints.emplace_back(
          "Declare the struct with [[attribute]] to use it as a declaration "
          "attribute.");
      if (attribute != nullptr) {
        diagnostic.related.push_back(
            {tokenSpan(attribute->name), "Type declared here."});
      }
      diagnostics.emplace_back(std::move(diagnostic));
      continue;
    }
    if (!attribute->genericParameters.empty() ||
        (!attribute->fields.empty() && !attribute->designatedConstructor)) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2113", DiagnosticPhase::Semantics, anchor,
          "Attribute type '" + spelling +
              "' must be a non-generic struct with public value fields and "
              "no declared constructors or bases.");
      diagnostic.related.push_back(
          {tokenSpan(attribute->name), "Attribute type declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
      continue;
    }
    const auto duplicate =
        std::find_if(resolved.begin(), resolved.end(),
                     [&](const ResolvedDeclarationAttribute &candidate) {
                       return candidate.type == attribute->id;
                     });
    if (duplicate != resolved.end()) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2115", DiagnosticPhase::Semantics, anchor,
          "Attribute '" + spelling +
              "' is applied more than once to this declaration.");
      diagnostic.related.push_back(
          {duplicate->span, "The first application is here."});
      diagnostics.emplace_back(std::move(diagnostic));
      continue;
    }
    const auto invalidArguments = [&](const Token &at, std::string message) {
      report(at, std::move(message), "GTI-S2114");
    };
    std::vector<const ExprPtr *> inputs(attribute->fields.size(), nullptr);
    bool valid = true;
    if (syntax.designators.empty()) {
      if (syntax.arguments.size() > attribute->fields.size()) {
        invalidArguments(
            syntax.paren,
            "Attribute '" + spelling + "' takes at most " +
                std::to_string(attribute->fields.size()) + " argument(s); " +
                std::to_string(syntax.arguments.size()) + " were given.");
        continue;
      }
      for (std::size_t index = 0; index < syntax.arguments.size(); ++index) {
        inputs[index] = &syntax.arguments[index];
      }
    } else {
      std::optional<std::size_t> previous;
      for (std::size_t index = 0; index < syntax.designators.size(); ++index) {
        const Token &name = syntax.designators[index];
        const auto found = std::find_if(
            attribute->fields.begin(), attribute->fields.end(),
            [&](const FieldInfo &field) {
              return field.declaration != nullptr &&
                     field.declaration->name().lexeme == name.lexeme;
            });
        if (found == attribute->fields.end()) {
          invalidArguments(name, "Unknown field '" + name.lexeme +
                                     "' in attribute type '" + spelling + "'.");
          valid = false;
          continue;
        }
        const std::size_t position =
            static_cast<std::size_t>(found - attribute->fields.begin());
        if (inputs[position] != nullptr || (previous && position < *previous)) {
          invalidArguments(
              name, inputs[position] != nullptr
                        ? "Duplicate argument for attribute field '" +
                              name.lexeme + "'."
                        : "Designated attribute arguments must follow the "
                          "attribute type's field order.");
          valid = false;
          continue;
        }
        previous = position;
        inputs[position] = &syntax.arguments[index];
      }
    }
    if (!valid) {
      continue;
    }
    ConstexprInterpreter context = makeConstexprInterpreter();
    ConstantAggregate aggregate{.kind = ConstantAggregateKind::Object,
                                .identity = attribute->id,
                                .exactType =
                                    std::make_shared<const SemanticType>(type)};
    aggregate.fields.reserve(attribute->fields.size());
    for (std::size_t index = 0; index < attribute->fields.size(); ++index) {
      const VariableDecl *field = attribute->fields[index].declaration;
      const auto member = field == nullptr
                              ? attribute->members.end()
                              : attribute->members.find(field->name().lexeme);
      if (member == attribute->members.end()) {
        invalidArguments(anchor, "Attribute type '" + spelling +
                                     "' has a field the compiler could not "
                                     "resolve.");
        valid = false;
        break;
      }
      const SemanticType fieldType = member->second.symbol.type;
      std::optional<ConstantValue> value;
      if (inputs[index] != nullptr) {
        const ExprPtr &argument = *inputs[index];
        if (analyzeInitializer(argument, fieldType) == SemanticType::Unknown) {
          valid = false;
          break;
        }
        ConstexprEvaluation evaluated =
            evaluateConstexprExpression(*argument, context);
        if (!evaluated) {
          invalidArguments(
              evaluated.location.lexeme.empty() ? expressionToken(*argument)
                                                : evaluated.location,
              "The argument for attribute field '" + field->name().lexeme +
                  "' must be a constant expression." +
                  (evaluated.detail.empty() ? "" : " " + evaluated.detail));
          valid = false;
          break;
        }
        const ConstantEvaluation converted =
            convertConstantToType(*evaluated.value, fieldType);
        if (!converted) {
          invalidArguments(expressionToken(*argument),
                           "The argument for attribute field '" +
                               field->name().lexeme +
                               "' is not a constant of type '" +
                               typeSpelling(fieldType) + "'.");
          valid = false;
          break;
        }
        value = *converted.value;
      } else {
        const Expr *initializer = field->initializer().get();
        const auto *literal = exprAs<LiteralExpr>(initializer);
        if (initializer == nullptr) {
          invalidArguments(syntax.paren,
                           "Attribute field '" + field->name().lexeme +
                               "' has no argument and no declaration default.");
          valid = false;
          break;
        }
        if (literal == nullptr) {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2114", DiagnosticPhase::Semantics, syntax.paren,
              "Attribute field '" + field->name().lexeme +
                  "' can be omitted only when its declaration default is a "
                  "literal.");
          diagnostic.related.push_back(
              {tokenSpan(field->name()), "Field declared here."});
          diagnostics.emplace_back(std::move(diagnostic));
          valid = false;
          break;
        }
        const ConstantEvaluation evaluated = evaluateConstantLiteral(
            literal->value(), constantIntegerDomain(fieldType));
        const ConstantEvaluation converted =
            evaluated ? convertConstantToType(*evaluated.value, fieldType)
                      : evaluated;
        if (!converted) {
          invalidArguments(field->name(),
                           "The declaration default of attribute field '" +
                               field->name().lexeme +
                               "' is not a constant of its type.");
          valid = false;
          break;
        }
        value = *converted.value;
      }
      aggregate.fields.push_back(
          {.symbol = toolingSymbolFor(member->second.symbol),
           .value = std::move(*value)});
    }
    if (!valid) {
      continue;
    }
    resolved.push_back(
        {.type = attribute->id,
         .exactType = type,
         .value = ConstantValue{makeConstantAggregate(std::move(aggregate))},
         .span = tokenSpan(anchor),
         .syntax = &syntax});
  }
  if (!resolved.empty()) {
    semanticModel.recordDeclarationAttributes(owner, std::move(resolved));
  }
}

auto SemanticVisitor::Impl::compileTimeSequenceElements(
    const SemanticType &type) const
    -> std::optional<std::vector<SemanticType>> {
  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size()) {
    return std::nullopt;
  }
  const ClassInfo &info = classInfo(type.classId);
  if (info.genericParameters.size() != 1 ||
      !info.genericParameters.front().pack ||
      info.genericParameters.front().value) {
    return std::nullopt;
  }
  std::vector<SemanticType> elements;
  for (const SemanticType &argument : type.arguments) {
    if (argument.kind == SemanticType::TypePack) {
      if (!argument.concretePack) {
        return std::nullopt;
      }
      elements.insert(elements.end(), argument.arguments.begin(),
                      argument.arguments.end());
    } else {
      elements.push_back(argument);
    }
  }
  return elements;
}

auto SemanticVisitor::Impl::isConstexprDefaultConstructible(
    const SemanticType &type) const -> bool {
  switch (type.kind) {
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
    return true;
  case SemanticType::Class: {
    if (type.classId == 0 || type.classId > classes.size()) {
      return false;
    }
    const ConstructorInfo *constructor =
        defaultConstructor(classInfo(type.classId));
    return constructor != nullptr && constructor->declaration != nullptr &&
           constructor->declaration->isConstexpr() &&
           constructor->access == AccessModifier::Public;
  }
  default:
    return false;
  }
}

auto SemanticVisitor::Impl::selectExactClassSpecialization(
    SemanticType type) const -> SemanticType {
  for (SemanticType &argument : type.arguments) {
    argument = selectExactClassSpecialization(std::move(argument));
  }
  for (SemanticType &argument : type.lambdaEnclosingClassTypes) {
    argument = selectExactClassSpecialization(std::move(argument));
  }
  for (SemanticType &argument : type.lambdaEnclosingFunctionTypes) {
    argument = selectExactClassSpecialization(std::move(argument));
  }
  type = rewriteCompilerType(std::move(type));
  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size() ||
      classInfo(type.classId).exactSpecializationPrimary != 0) {
    return type;
  }
  type.classId = selectSignatureClassDefinition(type.classId, type.arguments);
  // The root is already a primary, so only its own recorded specializations
  // can match; skip canonicalizing the full key for every nested class.
  if (std::none_of(exactClassSpecializations.begin(),
                   exactClassSpecializations.end(),
                   [&](const ExactClassSpecialization &candidate) {
                     return candidate.primary == type.classId;
                   })) {
    return type;
  }
  const SemanticType key = canonicalExactSpecializationType(type);
  const auto found = std::find_if(
      exactClassSpecializations.begin(), exactClassSpecializations.end(),
      [&](const ExactClassSpecialization &candidate) {
        return candidate.primary == key.classId &&
               candidate.typeArguments == key.arguments &&
               candidate.valueArguments == key.valueArguments;
      });
  return found == exactClassSpecializations.end()
             ? type
             : SemanticType::classType(found->specialization);
}

auto SemanticVisitor::Impl::isConcreteExactSpecializationType(
    const SemanticType &type) -> bool {
  if (type.kind == SemanticType::Unknown ||
      type.kind == SemanticType::TypeParameter ||
      type.kind == SemanticType::TypePack ||
      type.kind == SemanticType::TypeName || type.arrayLengthParameterId != 0) {
    return false;
  }
  if (std::any_of(type.valueArguments.begin(), type.valueArguments.end(),
                  [](const CompileTimeValue &value) {
                    return value.kind != CompileTimeValue::UInt64;
                  })) {
    return false;
  }
  return std::all_of(type.arguments.begin(), type.arguments.end(),
                     isConcreteExactSpecializationType) &&
         std::all_of(type.lambdaEnclosingClassTypes.begin(),
                     type.lambdaEnclosingClassTypes.end(),
                     isConcreteExactSpecializationType) &&
         std::all_of(type.lambdaEnclosingFunctionTypes.begin(),
                     type.lambdaEnclosingFunctionTypes.end(),
                     isConcreteExactSpecializationType);
}

auto SemanticVisitor::Impl::samePackageOwnershipDomain(SourceUnitId left,
                                                       SourceUnitId right) const
    -> bool {
  if (sourceGraph == nullptr || left == right) {
    return true;
  }
  const SourceUnit *leftUnit = sourceGraph->findUnit(left);
  const SourceUnit *rightUnit = sourceGraph->findUnit(right);
  if (leftUnit == nullptr || rightUnit == nullptr) {
    return false;
  }
  if (leftUnit->role == SourceUnitRole::StandardLibrary &&
      rightUnit->role == SourceUnitRole::StandardLibrary) {
    return true;
  }
  if ((leftUnit->prelude || leftUnit->role == SourceUnitRole::Prelude) &&
      (rightUnit->prelude || rightUnit->role == SourceUnitRole::Prelude)) {
    return true;
  }
  if (leftUnit->packageIdentity && rightUnit->packageIdentity) {
    return leftUnit->packageIdentity == rightUnit->packageIdentity;
  }
  return leftUnit->role == SourceUnitRole::Application &&
         rightUnit->role == SourceUnitRole::Application &&
         !leftUnit->packageIdentity && !rightUnit->packageIdentity;
}

auto SemanticVisitor::Impl::ownsNominalSpecializationArgument(
    SourceUnitId sourceUnit, const SemanticType &argument) const -> bool {
  const SemanticType canonical = canonicalExactSpecializationType(argument);
  if (canonical.kind == SemanticType::Class && canonical.classId != 0 &&
      canonical.classId <= classes.size()) {
    return samePackageOwnershipDomain(sourceUnit,
                                      classInfo(canonical.classId).sourceUnit);
  }
  if (canonical.kind == SemanticType::Enum && canonical.enumId != 0 &&
      canonical.enumId <= enums.size()) {
    return samePackageOwnershipDomain(sourceUnit,
                                      enums[canonical.enumId - 1].sourceUnit);
  }
  return false;
}

auto SemanticVisitor::Impl::reportExactSpecializationError(
    const ClassDecl &declaration, std::string message, const ClassInfo *primary)
    -> void {
  Diagnostic diagnostic =
      makeDiagnostic("GTI-S2078", DiagnosticPhase::Semantics,
                     declaration.name(), std::move(message));
  if (primary != nullptr) {
    diagnostic.related.push_back(
        {tokenSpan(primary->name), "Primary class declared here."});
  }
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::registerClassSpecializations(
    const StmtList &statements, std::vector<std::string> scope) -> void {
  const std::vector<std::string> enclosingNamespace = currentNamespace;
  for (const StmtPtr &statement : statements) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        registerClassSpecializations(*branch, scope);
      }
      continue;
    }
    if (const auto *namespaceDecl = stmtAs<NamespaceDecl>(statement.get())) {
      scope.emplace_back(namespaceDecl->name().lexeme);
      registerClassSpecializations(namespaceDecl->declarations(), scope);
      scope.pop_back();
      continue;
    }
    const auto *declaration = stmtAs<ClassDecl>(statement.get());
    if (declaration == nullptr) {
      continue;
    }
    if (!declaration->exactSpecialization()) {
      continue;
    }

    currentSourceUnit = sourceUnitFor(declaration->name());
    currentNamespace = scope;
    const TypeRef &targetType = *declaration->exactSpecialization();
    bool valid = true;
    if (!scope.empty()) {
      reportExactSpecializationError(
          *declaration,
          "An exact class specialization must be declared at package "
          "scope with a qualified primary name.");
      valid = false;
    }
    if (declaration->kind() != ClassKind::Class &&
        declaration->kind() != ClassKind::Struct) {
      reportExactSpecializationError(
          *declaration, "Only class and struct primaries support exact "
                        "specialization.");
      valid = false;
    }
    if (declaration->isForwardDeclaration()) {
      reportExactSpecializationError(
          *declaration, "An exact class specialization must provide a complete "
                        "definition.");
      valid = false;
    }
    if (!declaration->genericParameters().empty()) {
      reportExactSpecializationError(
          *declaration, "An exact class specialization cannot declare generic "
                        "parameters.");
      valid = false;
    }
    if (targetType.name.segments.size() < 2) {
      reportExactSpecializationError(
          *declaration,
          "An exact class specialization must use a qualified primary "
          "name.");
      valid = false;
    }
    if (reportCompilerPrivatePath(targetType.name)) {
      valid = false;
    }

    std::optional<ClassId> primaryId = resolveClassPath(targetType.name, {});
    if (!primaryId || *primaryId == 0 || *primaryId > classes.size()) {
      reportExactSpecializationError(*declaration,
                                     "Unknown or invisible generic primary '" +
                                         pathSpelling(targetType.name) + "'.");
      continue;
    }
    ClassInfo &primary = classInfo(*primaryId);
    if (primary.compilerPrivate) {
      reportExactSpecializationError(
          *declaration,
          "Compiler-private classes cannot be specialized by source "
          "code.",
          &primary);
      valid = false;
    }
    if (primary.genericParameters.empty()) {
      reportExactSpecializationError(
          *declaration,
          "Class '" + pathSpelling(targetType.name) +
              "' is not generic and cannot be specialized.",
          &primary);
      valid = false;
    }
    if ((primary.kind != ClassKind::Class &&
         primary.kind != ClassKind::Struct) ||
        declaration->kind() != primary.kind) {
      reportExactSpecializationError(
          *declaration,
          "The specialization must use the primary declaration's '" +
              std::string(classKindSpelling(primary.kind)) + "' kind.",
          &primary);
      valid = false;
    }

    const ResolvedClassArguments resolved = resolveClassArguments(
        *primaryId, targetType.arguments, declaration->name(), "GTI-S2078");
    valid = resolved.valid && valid;
    if (!resolved.valid) {
      continue;
    }
    primaryId = selectSignatureClassDefinition(*primaryId, resolved.types);
    std::vector<SemanticType> typeArguments = resolved.types;
    for (SemanticType &argument : typeArguments) {
      argument = canonicalExactSpecializationType(std::move(argument));
      if (!isConcreteExactSpecializationType(argument)) {
        reportExactSpecializationError(
            *declaration,
            "Exact specialization arguments must be fully concrete.", &primary);
        valid = false;
        break;
      }
    }
    std::vector<CompileTimeValue> valueArguments = resolved.values;
    if (std::any_of(valueArguments.begin(), valueArguments.end(),
                    [](const CompileTimeValue &value) {
                      return value.kind != CompileTimeValue::UInt64;
                    })) {
      reportExactSpecializationError(
          *declaration,
          "Exact specialization value arguments must be concrete uint64_t "
          "constants.",
          &primary);
      valid = false;
    }

    const bool ownsPrimary =
        samePackageOwnershipDomain(currentSourceUnit, primary.sourceUnit);
    const bool ownsArgument = std::any_of(
        typeArguments.begin(), typeArguments.end(),
        [&](const SemanticType &argument) {
          return ownsNominalSpecializationArgument(currentSourceUnit, argument);
        });
    if (!ownsPrimary && !ownsArgument) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2078", DiagnosticPhase::Semantics, declaration->name(),
          "This package cannot define the exact specialization '" +
              typeRefSpelling(targetType) +
              "'; it owns neither the primary nor a top-level nominal "
              "type argument.");
      diagnostic.related.push_back(
          {tokenSpan(primary.name), "Primary class declared here."});
      diagnostic.hints.emplace_back(
          "Define the specialization in the primary's package or use a "
          "nominal argument type owned by this package.");
      diagnostics.emplace_back(std::move(diagnostic));
      valid = false;
    }

    const auto duplicate = std::find_if(
        exactClassSpecializations.begin(), exactClassSpecializations.end(),
        [&](const ExactClassSpecialization &candidate) {
          return candidate.primary == *primaryId &&
                 candidate.typeArguments == typeArguments &&
                 candidate.valueArguments == valueArguments;
        });
    if (duplicate != exactClassSpecializations.end()) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2078", DiagnosticPhase::Semantics, declaration->name(),
          "Duplicate exact specialization of '" + typeRefSpelling(targetType) +
              "'.");
      if (duplicate->specialization != 0 &&
          duplicate->specialization <= classes.size()) {
        diagnostic.related.push_back(
            {tokenSpan(classInfo(duplicate->specialization).name),
             "Previous exact specialization is here."});
      }
      diagnostics.emplace_back(std::move(diagnostic));
      valid = false;
    }
    if (!valid) {
      continue;
    }

    const CapabilityPolicyRegistration capabilityPolicies =
        registerClassAttributes(*declaration);
    if (capabilityPolicies.cAbiRecord || capabilityPolicies.cOpaqueHandle) {
      reportExactSpecializationError(
          *declaration,
          "Exact specializations cannot declare native-record "
          "representation attributes.",
          &primary);
      continue;
    }

    const ClassId specializationId = classes.size() + 1;
    classDeclIds.emplace(declaration, specializationId);
    classes.push_back(ClassInfo{
        .id = specializationId,
        .sourceUnit = currentSourceUnit,
        .declaration = declaration,
        .name = declaration->name(),
        .kind = declaration->kind(),
        .namespaceScope = primary.namespaceScope,
        .exactSpecializationPrimary = *primaryId,
        .exactTypeArguments = typeArguments,
        .exactValueArguments = valueArguments,
        .attributeType = capabilityPolicies.attributeType,
        .attributeAttribute = capabilityPolicies.attributeSource,
        .reflectPrivate = capabilityPolicies.reflectPrivate,
        .reflectPrivateAttribute = capabilityPolicies.reflectPrivateSource,
        .transferPolicy = capabilityPolicies.transfer,
        .sharePolicy = capabilityPolicies.share,
        .transferPolicySource = capabilityPolicies.transferSource,
        .sharePolicySource = capabilityPolicies.shareSource});
    exactClassSpecializations.push_back(
        {.primary = *primaryId,
         .specialization = specializationId,
         .typeArguments = std::move(typeArguments),
         .valueArguments = std::move(valueArguments)});
  }
  currentNamespace = enclosingNamespace;
}

auto SemanticVisitor::Impl::resolveClassInheritance() -> void {
  for (ClassInfo &owner : classes) {
    if (owner.declaration == nullptr) {
      continue;
    }
    currentSourceUnit = owner.sourceUnit;
    beginTypeParameterScope(owner.genericParameters);
    if (owner.kind == ClassKind::Union) {
      if (!owner.declaration->bases().empty()) {
        report(owner.declaration->bases().front().type.name.last(),
               "A union cannot declare base types.", "GTI-S2066");
      }
      endTypeParameterScope();
      continue;
    }
    bool hasConcreteBase = false;
    std::unordered_set<ClassId> directBases;
    for (const BaseSpecifier &syntax : owner.declaration->bases()) {
      const Token &location = syntax.type.name.last();
      if (!syntax.access || syntax.access->kind != TokenKind::PUBLIC) {
        Diagnostic diagnostic =
            makeDiagnostic("GTI-S2040", DiagnosticPhase::Semantics,
                           syntax.access ? *syntax.access : location,
                           "Inheritance must be public; write 'public " +
                               typeRefSpelling(syntax.type) + "'.");
        diagnostic.hints.emplace_back(
            "GTI uses inheritance only for substitutable base contracts; "
            "use a field for private implementation reuse.");
        diagnostics.emplace_back(std::move(diagnostic));
      }

      const SemanticType baseType = typeOf(syntax.type, owner.namespaceScope);
      if (baseType.kind != SemanticType::Class || baseType.classId == 0 ||
          baseType.classId > classes.size()) {
        if (baseType != SemanticType::Unknown) {
          report(location, "Base type must name a class, struct, or interface.",
                 "GTI-S2040");
        }
        continue;
      }
      if (baseType.classId == owner.id) {
        report(location, "A type cannot inherit from itself.", "GTI-S2040");
        continue;
      }
      if (!directBases.insert(baseType.classId).second) {
        report(location,
               "Base type '" + typeSpelling(baseType) +
                   "' is listed more than once.",
               "GTI-S2040");
        continue;
      }

      const ClassInfo &base = classInfo(baseType.classId);
      if (base.kind == ClassKind::Union) {
        report(location, "A union cannot be used as a base type.", "GTI-S2066");
        continue;
      }
      const bool interfaceBase = base.kind == ClassKind::Interface;
      if (owner.kind == ClassKind::Interface && !interfaceBase) {
        report(location, "An interface can inherit only from other interfaces.",
               "GTI-S2040");
        continue;
      }
      if (!interfaceBase) {
        if (hasConcreteBase) {
          report(location,
                 "A class or struct can have only one state-bearing base; "
                 "additional bases must be interfaces.",
                 "GTI-S2040");
          continue;
        }
        hasConcreteBase = true;
      }
      owner.bases.push_back(
          {.syntax = &syntax, .type = baseType, .interface = interfaceBase});
    }
    endTypeParameterScope();
  }

  std::vector<std::uint8_t> state(classes.size(), 0);
  for (const ClassInfo &owner : classes) {
    detectInheritanceCycle(owner.id, state);
  }
  for (const ClassInfo &owner : classes) {
    std::unordered_set<ClassId> ancestors;
    collectUniqueAncestors(owner, owner.id, ancestors);
  }
}

auto SemanticVisitor::Impl::detectInheritanceCycle(
    ClassId id, std::vector<std::uint8_t> &state) -> void {
  if (id == 0 || id > classes.size() || state[id - 1] == 2) {
    return;
  }
  if (state[id - 1] == 1) {
    return;
  }
  state[id - 1] = 1;
  const ClassInfo &owner = classInfo(id);
  for (const ClassBaseTypeInfo &base : owner.bases) {
    if (base.type.classId == 0 || base.type.classId > classes.size()) {
      continue;
    }
    if (state[base.type.classId - 1] == 1) {
      const Token &location =
          base.syntax == nullptr ? owner.name : base.syntax->type.name.last();
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2040", DiagnosticPhase::Semantics, location,
          "Inheritance cycle involving '" + owner.name.lexeme + "'.");
      diagnostic.related.push_back(
          {tokenSpan(classInfo(base.type.classId).name),
           "Cyclic base type declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
      continue;
    }
    detectInheritanceCycle(base.type.classId, state);
  }
  state[id - 1] = 2;
}

auto SemanticVisitor::Impl::collectUniqueAncestors(
    const ClassInfo &current, ClassId root,
    std::unordered_set<ClassId> &ancestors) -> void {
  for (const ClassBaseTypeInfo &base : current.bases) {
    if (base.type.classId == 0 || base.type.classId > classes.size()) {
      continue;
    }
    if (!ancestors.insert(base.type.classId).second) {
      const Token &location =
          base.syntax == nullptr ? current.name : base.syntax->type.name.last();
      report(location,
             "Base '" + classInfo(base.type.classId).name.lexeme +
                 "' is inherited through more than one path; inheritance "
                 "diamonds are not supported.",
             "GTI-S2040");
      continue;
    }
    if (base.type.classId != root) {
      collectUniqueAncestors(classInfo(base.type.classId), root, ancestors);
    }
  }
}

auto SemanticVisitor::Impl::registerFunctionGenericParameters(
    const StmtList &statements, std::vector<std::string> scope,
    bool classMember) -> void {
  for (const StmtPtr &statement : statements) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        registerFunctionGenericParameters(*branch, scope, classMember);
      }
    } else if (const auto *externC = stmtAs<ExternCDecl>(statement.get())) {
      registerFunctionGenericParameters(externC->declarations(), scope, false);
    } else if (const auto *variable = stmtAs<VariableDecl>(statement.get())) {
      if (!classMember) {
        currentSourceUnit = sourceUnitFor(variable->name());
        recordGenericDefaultConstant(*variable, scope);
      }
    } else if (const auto *function = stmtAs<FunctionDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(function->name());
      std::vector<GenericParameterInfo> genericParameters =
          makeGenericParameters(function->genericParameters(), function->name(),
                                scope);
      functionGenericParameters.emplace(function, genericParameters);
      const IntrinsicKind intrinsic = trustedIntrinsicKind(
          currentSourceUnit, scope, function->name().lexeme);
      (void)catalog.registerCallable(CallableDeclaration{
          .id = nextFunctionId++,
          .sourceUnit = currentSourceUnit,
          .syntax = function,
          .qualifiedName = qualifiedName(scope, function->name().lexeme),
          .namespaceScope = scope,
          .requiredParameterCount =
              requiredParameterCount(function->parameters()),
          .defaultArgumentEffectOwners =
              allocateDefaultArgumentEffectOwners(function->parameters()),
          .genericParameters = std::move(genericParameters),
          .parameterPack = !function->parameters().empty() &&
                           function->parameters().back().pack.has_value(),
          .entryPoint = !classMember && scope.empty() &&
                        function->name().lexeme == "main" &&
                        (sourceGraph == nullptr ||
                         currentSourceUnit == sourceGraph->entryUnit()),
          .staticMember = classMember && function->isStatic(),
          .internalLinkage = !classMember && function->isStatic(),
          .constexprFunction = function->isConstexpr(),
          .requiresUnsafe = function->unsafeKeyword().has_value(),
          .neverReturns = function->hasFunctionAttribute("noreturn"),
          .tracksCaller = function->hasFunctionAttribute("track_caller"),
          .discardableResult = function->hasFunctionAttribute("discardable"),
          .linkage = function->linkage(),
          .externalSymbol =
              function->hasCLinkage()
                  ? function->nativeLinkName()
                        ? std::string(function->nativeLinkName()->name())
                        : function->name().lexeme
                  : std::string{},
          .cArrayCountParameter = nativeCArrayCountParameter(*function),
          .intrinsic = intrinsic,
          .compilerPrivate =
              compilerPrivateDeclaration(currentSourceUnit, scope)});
    } else if (const auto *classDecl = stmtAs<ClassDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(classDecl->name());
      const auto registered = classDeclIds.find(classDecl);
      if (registered == classDeclIds.end()) {
        continue;
      }
      const ClassInfo &owner = classInfo(registered->second);
      std::vector<std::string> memberScope = owner.namespaceScope;
      memberScope.emplace_back(owner.name.lexeme);
      registerFunctionGenericParameters(classDecl->members(),
                                        std::move(memberScope), true);
    } else if (const auto *namespaceDecl =
                   stmtAs<NamespaceDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(namespaceDecl->name());
      scope.emplace_back(namespaceDecl->name().lexeme);
      registerFunctionGenericParameters(namespaceDecl->declarations(), scope,
                                        false);
      scope.pop_back();
    }
  }
}

auto SemanticVisitor::Impl::nativeCArrayCountParameter(
    const FunctionDecl &function) -> std::optional<std::size_t> {
  if (!function.nativeCArray()) {
    return std::nullopt;
  }
  const auto found =
      std::find_if(function.parameters().begin(), function.parameters().end(),
                   [&](const Parameter &parameter) {
                     return parameter.name.lexeme ==
                            function.nativeCArray()->countParameter.lexeme;
                   });
  if (found == function.parameters().end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(
      std::distance(function.parameters().begin(), found));
}

auto SemanticVisitor::Impl::registerNamespaceSymbols(
    const StmtList &statements, std::vector<std::string> scope) -> void {
  for (const StmtPtr &statement : statements) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        registerNamespaceSymbols(*branch, scope);
      }
    } else if (const auto *externC = stmtAs<ExternCDecl>(statement.get())) {
      registerNamespaceSymbols(externC->declarations(), scope);
    } else if (const auto *function = stmtAs<FunctionDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(function->name());
      Symbol symbol = functionSymbol(*function, scope);
      if (!symbol.overloads.empty()) {
        publishCallableDeclaration(*function, symbol.overloads.front(), scope,
                                   0);
      }
      declareNamespaceSymbol(scope, function->name(), std::move(symbol));
    } else if (const auto *classDecl = stmtAs<ClassDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(classDecl->name());
      if (classDecl->isExactSpecialization()) {
        continue;
      }
      if (const auto found = classDeclIds.find(classDecl);
          found != classDeclIds.end()) {
        const auto family = classInfo(found->second).signatureFamilyPrimary;
        if (family != 0) {
          publishNamespaceSymbol(qualifiedName(scope, classDecl->name().lexeme),
                                 Symbol{.type = SemanticType::typeName(family),
                                        .sourceUnit = currentSourceUnit,
                                        .declaration = classInfo(family).name});
          continue;
        }
        declareNamespaceSymbol(scope, classDecl->name(),
                               SemanticType::typeName(found->second), false);
      }
    } else if (const auto *namespaceDecl =
                   stmtAs<NamespaceDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(namespaceDecl->name());
      scope.emplace_back(namespaceDecl->name().lexeme);
      registerNamespaceSymbols(namespaceDecl->declarations(), scope);
      scope.pop_back();
    }
  }
}

auto SemanticVisitor::Impl::collectClassMembers(const StmtList &statements,
                                                std::vector<std::string> scope)
    -> void {
  for (const StmtPtr &statement : statements) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        collectClassMembers(*branch, scope);
      }
    } else if (const auto *classDecl = stmtAs<ClassDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(classDecl->name());
      const auto found = classDeclIds.find(classDecl);
      if (found == classDeclIds.end()) {
        continue;
      }
      ClassInfo &info = classInfo(found->second);
      AccessModifier access = info.kind == ClassKind::Class
                                  ? AccessModifier::Private
                                  : AccessModifier::Public;
      const std::optional<ClassId> enclosingClass = currentClass;
      currentClass = info.id;
      beginTypeParameterScope(info.genericParameters);
      collectMembers(classDecl->members(), info, access);
      endTypeParameterScope();
      currentClass = enclosingClass;
    } else if (const auto *namespaceDecl =
                   stmtAs<NamespaceDecl>(statement.get())) {
      currentSourceUnit = sourceUnitFor(namespaceDecl->name());
      scope.emplace_back(namespaceDecl->name().lexeme);
      collectClassMembers(namespaceDecl->declarations(), scope);
      scope.pop_back();
    }
  }
}

auto SemanticVisitor::Impl::byValueTypeReachesClass(
    const SemanticType &type, const SemanticType &target,
    std::vector<SemanticType> &visiting,
    std::vector<SemanticType> &visitingEnums, bool &exhausted) const -> bool {
  if (type.kind == SemanticType::Array && type.arguments.size() == 1) {
    return byValueTypeReachesClass(type.arguments.front(), target, visiting,
                                   visitingEnums, exhausted);
  }
  if (type.kind == SemanticType::Enum && type.enumId != 0 &&
      type.enumId <= enums.size()) {
    if (std::find(visitingEnums.begin(), visitingEnums.end(), type) !=
        visitingEnums.end()) {
      return false;
    }
    visitingEnums.push_back(type);
    bool reaches = false;
    const auto concrete = enumTypeFor(type);
    if (!concrete) {
      visitingEnums.pop_back();
      return false;
    }
    for (const auto &enumerator : concrete->enumerators) {
      for (const SemanticType &payload : enumerator.payloadTypes) {
        // Preserve the admitted payload family and exact enum-instance guard;
        // an invalid symbolic sum must not expand during class layout proof.
        if (payloadTypeAllowed(payload, type.enumId))
          reaches =
              reaches || byValueTypeReachesClass(payload, target, visiting,
                                                 visitingEnums, exhausted);
      }
    }
    visitingEnums.pop_back();
    return reaches;
  }
  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size()) {
    return false;
  }
  if (type == target) {
    return true;
  }
  if (std::any_of(type.valueArguments.begin(), type.valueArguments.end(),
                  [](const auto &value) {
                    return value.kind != CompileTimeValue::UInt64;
                  })) {
    // Open arithmetic is deferred to exact owner substitutions, not mistaken
    // for a declaration-ID self edge.
    return false;
  }
  if (std::find(visiting.begin(), visiting.end(), type) != visiting.end())
    return true;
  if (visiting.size() >= 64) {
    exhausted = true;
    return false;
  }
  visiting.push_back(type);

  const ClassInfo &nested = classInfo(type.classId);
  const GenericSubstitution substitution = classSubstitution(type);
  for (const FieldInfo &field : nested.fields) {
    if (field.declaration == nullptr) {
      continue;
    }
    const auto member = nested.members.find(field.declaration->name().lexeme);
    if (member == nested.members.end()) {
      continue;
    }
    const SemanticType fieldType =
        substituteType(member->second.symbol.type, substitution);
    if (byValueTypeReachesClass(fieldType, target, visiting, visitingEnums,
                                exhausted)) {
      visiting.pop_back();
      return true;
    }
  }

  visiting.pop_back();
  return false;
}

auto SemanticVisitor::Impl::validateOrdinaryClassLayouts() -> void {
  for (const ClassInfo &owner : classes) {
    validateOrdinaryClassLayout(openClassType(owner.id));
  }
}

auto SemanticVisitor::Impl::validateOrdinaryClassLayout(
    const SemanticType &ownerType) -> void {
  if (ownerType.kind != SemanticType::Class || ownerType.classId == 0 ||
      ownerType.classId > classes.size() ||
      std::find(requestSession.invalidOrdinaryLayouts.begin(),
                requestSession.invalidOrdinaryLayouts.end(),
                ownerType) != requestSession.invalidOrdinaryLayouts.end())
    return;
  const ClassInfo &owner = classInfo(ownerType.classId);
  if (catalog.sealed &&
      std::any_of(owner.fields.begin(), owner.fields.end(),
                  [&](const FieldInfo &field) {
                    return recursiveLayoutFields.contains(field.declaration);
                  })) {
    requestSession.invalidOrdinaryLayouts.push_back(ownerType);
    return;
  }
  if (owner.declaration == nullptr ||
      owner.declaration->isForwardDeclaration() ||
      owner.kind == ClassKind::Interface || owner.kind == ClassKind::Union ||
      owner.cAbiRecord || owner.cOpaqueHandle) {
    return;
  }

  const GenericSubstitution substitution = classSubstitution(ownerType);
  std::string ownerSpelling = owner.name.lexeme;
  if (!owner.genericParameters.empty()) {
    ownerSpelling += '<';
    for (std::size_t index = 0; index < owner.genericParameters.size();
         ++index) {
      if (index != 0) {
        ownerSpelling += ", ";
      }
      ownerSpelling += owner.genericParameters[index].name.lexeme;
      if (owner.genericParameters[index].pack) {
        ownerSpelling += "...";
      }
    }
    ownerSpelling += '>';
  }
  for (const FieldInfo &field : owner.fields) {
    if (field.declaration == nullptr) {
      continue;
    }
    const auto member = owner.members.find(field.declaration->name().lexeme);
    if (member == owner.members.end()) {
      continue;
    }
    const SemanticType fieldType =
        substituteType(member->second.symbol.type, substitution);
    std::vector<SemanticType> visiting{ownerType};
    std::vector<SemanticType> visitingEnums;
    bool exhausted = false;
    const bool cycle = byValueTypeReachesClass(fieldType, ownerType, visiting,
                                               visitingEnums, exhausted);
    if (!cycle && !exhausted) {
      continue;
    }
    requestSession.invalidOrdinaryLayouts.push_back(ownerType);
    if (exhausted) {
      diagnostics.push_back(
          makeDiagnostic(DiagnosticId::GenericInstantiationLimit,
                         tokenSpan(field.declaration->name()),
                         {"concrete-layout-depth", "64"}));
      return;
    }
    if (!catalog.sealed)
      recursiveLayoutFields.insert(field.declaration);
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2082", DiagnosticPhase::Semantics,
                       field.declaration->type().name.last(),
                       "Type '" + ownerSpelling +
                           "' cannot contain itself by value through field '" +
                           field.declaration->name().lexeme + "'.");
    diagnostic.related.push_back(
        {tokenSpan(owner.name),
         "The recursively contained type is declared here."});
    diagnostic.hints.emplace_back(
        "Break the recursive layout with an owning standard-library "
        "indirection such as std::unique_ptr<" +
        ownerSpelling +
        ">, or use a reference when another object owns the value.");
    diagnostics.emplace_back(std::move(diagnostic));
  }
}

auto SemanticVisitor::Impl::refreshFunctionPolymorphism(
    const CallableCandidate &candidate) -> void {
  if (candidate.declaration == nullptr) {
    return;
  }
  if (CallableDeclaration *declaration =
          catalog.mutableCallable(*candidate.declaration)) {
    declaration->virtualMethod = candidate.virtualMethod;
    declaration->pureVirtual = candidate.pureVirtual;
    declaration->overrideMethod = candidate.overrideMethod;
    declaration->virtualRoots = candidate.virtualRoots;
  }
  const FunctionInfo *existing =
      semanticModel.findFunction(*candidate.declaration);
  if (existing == nullptr) {
    return;
  }
  FunctionInfo updated = *existing;
  updated.virtualMethod = candidate.virtualMethod;
  updated.pureVirtual = candidate.pureVirtual;
  updated.overrideMethod = candidate.overrideMethod;
  updated.virtualRoots = candidate.virtualRoots;
  semanticModel.record(*candidate.declaration, std::move(updated));
}

auto SemanticVisitor::Impl::refreshOverloadPrivacy(Symbol &symbol) -> void {
  if (symbol.type == SemanticType::Function) {
    symbol.compilerPrivate =
        !symbol.overloads.empty() &&
        std::all_of(symbol.overloads.begin(), symbol.overloads.end(),
                    [](const CallableCandidate &overload) {
                      return overload.compilerPrivate;
                    });
  }
}

auto SemanticVisitor::Impl::mergeInheritedMember(ClassInfo &owner,
                                                 const std::string &name,
                                                 MemberInfo incoming) -> void {
  if (const auto alias = owner.typeAliases.find(name);
      alias != owner.typeAliases.end()) {
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2043", DiagnosticPhase::Semantics, owner.name,
                       "Inherited member name '" + name +
                           "' conflicts with a class-scoped type alias in '" +
                           owner.name.lexeme + "'.");
    diagnostic.related.push_back({tokenSpan(alias->second.declaration->name()),
                                  "Type alias is declared here."});
    diagnostic.related.push_back({tokenSpan(incoming.symbol.declaration),
                                  "Conflicting member is declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
    return;
  }
  const auto found = owner.members.find(name);
  if (found == owner.members.end()) {
    owner.members.emplace(name, std::move(incoming));
    return;
  }
  MemberInfo &existing = found->second;
  if (existing.symbol.type != SemanticType::Function ||
      incoming.symbol.type != SemanticType::Function) {
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2043", DiagnosticPhase::Semantics, owner.name,
                       "Inherited member name '" + name +
                           "' is ambiguous in '" + owner.name.lexeme + "'.");
    diagnostic.related.push_back(
        {tokenSpan(existing.symbol.declaration), "First member is here."});
    diagnostic.related.push_back({tokenSpan(incoming.symbol.declaration),
                                  "Conflicting member is here."});
    diagnostics.emplace_back(std::move(diagnostic));
    return;
  }
  for (CallableCandidate &candidate : incoming.symbol.overloads) {
    const bool duplicateIdentity = std::any_of(
        existing.symbol.overloads.begin(), existing.symbol.overloads.end(),
        [&](const CallableCandidate &previous) {
          return previous.id != 0 && previous.id == candidate.id;
        });
    if (!duplicateIdentity) {
      existing.symbol.overloads.emplace_back(std::move(candidate));
    }
  }
  refreshOverloadPrivacy(existing.symbol);
}

auto SemanticVisitor::Impl::mergeDeclaredMember(ClassInfo &owner,
                                                const std::string &name,
                                                MemberInfo local) -> void {
  if (const auto alias = owner.typeAliases.find(name);
      alias != owner.typeAliases.end()) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2006", DiagnosticPhase::Semantics, local.symbol.declaration,
        "Duplicate member declaration of '" + name +
            "'; the name already denotes a class-scoped type alias.");
    diagnostic.related.push_back({tokenSpan(alias->second.declaration->name()),
                                  "Type alias is declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
    return;
  }
  const auto found = owner.members.find(name);
  if (found == owner.members.end()) {
    for (const CallableCandidate &candidate : local.symbol.overloads) {
      if (candidate.overrideMethod) {
        report(candidate.declaration->overrideKeyword().value(),
               "Method marked 'override' has no inherited virtual method "
               "with the same exact signature.",
               "GTI-S2042");
      }
      refreshFunctionPolymorphism(candidate);
    }
    owner.members.emplace(name, std::move(local));
    return;
  }

  MemberInfo &inherited = found->second;
  if (inherited.symbol.type != SemanticType::Function ||
      local.symbol.type != SemanticType::Function) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2043", DiagnosticPhase::Semantics, local.symbol.declaration,
        "Member '" + name +
            "' hides an inherited member; inherited data "
            "members cannot be redeclared.");
    diagnostic.related.push_back(
        {tokenSpan(inherited.symbol.declaration), "Inherited member is here."});
    diagnostics.emplace_back(std::move(diagnostic));
    return;
  }

  for (CallableCandidate &candidate : local.symbol.overloads) {
    std::vector<std::size_t> matches;
    for (std::size_t index = 0; index < inherited.symbol.overloads.size();
         ++index) {
      if (sameFunctionSignature(inherited.symbol.overloads[index], candidate)) {
        matches.push_back(index);
      }
    }

    if (matches.empty()) {
      if (candidate.overrideMethod) {
        report(candidate.declaration->overrideKeyword().value(),
               "Method marked 'override' has no inherited virtual method "
               "with the same exact signature.",
               "GTI-S2042");
      }
      inherited.symbol.overloads.emplace_back(std::move(candidate));
      refreshFunctionPolymorphism(inherited.symbol.overloads.back());
      continue;
    }

    bool validOverride = true;
    std::vector<FunctionId> roots;
    for (std::size_t index : matches) {
      const CallableCandidate &base = inherited.symbol.overloads[index];
      if (!base.virtualMethod || base.staticMember) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2042", DiagnosticPhase::Semantics,
            candidate.declaration->name(),
            "Method has the same signature as a non-virtual base method; "
            "GTI does not permit accidental method hiding.");
        diagnostic.related.push_back(
            {tokenSpan(base.declaration->name()), "Base method is here."});
        diagnostics.emplace_back(std::move(diagnostic));
        validOverride = false;
      }
      if (candidate.returnType != base.returnType) {
        Diagnostic diagnostic =
            makeDiagnostic("GTI-S2042", DiagnosticPhase::Semantics,
                           candidate.declaration->returnType().name.last(),
                           "Override return type must exactly match '" +
                               typeSpelling(base.returnType) + "'.");
        diagnostic.related.push_back(
            {tokenSpan(base.declaration->returnType().name.last()),
             "Base return type is declared here."});
        diagnostics.emplace_back(std::move(diagnostic));
        validOverride = false;
      }
      if (candidate.returnType.kind == SemanticType::Reference &&
          (candidate.returnBorrowOrigin != base.returnBorrowOrigin ||
           candidate.returnBorrowAccess != base.returnBorrowAccess ||
           (candidate.returnBorrowOrigin == BorrowOriginKind::Argument &&
            candidate.returnBorrowParameter != base.returnBorrowParameter))) {
        Diagnostic diagnostic =
            makeDiagnostic("GTI-S2042", DiagnosticPhase::Semantics,
                           candidate.declaration->name(),
                           "Override reference lifetime must use the same "
                           "borrow origin as the base declaration.");
        diagnostic.related.push_back(
            {tokenSpan(base.declaration->name()),
             "Base borrow-origin contract is declared here."});
        diagnostics.emplace_back(std::move(diagnostic));
        validOverride = false;
      }
      roots.insert(roots.end(), base.virtualRoots.begin(),
                   base.virtualRoots.end());
    }
    if (!candidate.overrideMethod) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2042", DiagnosticPhase::Semantics,
          candidate.declaration->name(),
          "Overriding methods must use the C++-familiar 'override' "
          "specifier.");
      diagnostic.related.push_back(
          {tokenSpan(
               inherited.symbol.overloads[matches.front()].declaration->name()),
           "Virtual base method is declared here."});
      diagnostic.hints.emplace_back("Add 'override' after the parameter list.");
      diagnostics.emplace_back(std::move(diagnostic));
    }
    if (validOverride) {
      std::sort(roots.begin(), roots.end());
      roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
      candidate.virtualMethod = true;
      candidate.virtualRoots = std::move(roots);
    }

    std::erase_if(inherited.symbol.overloads,
                  [&](const CallableCandidate &base) {
                    return sameFunctionSignature(base, candidate);
                  });
    inherited.symbol.overloads.emplace_back(std::move(candidate));
    refreshFunctionPolymorphism(inherited.symbol.overloads.back());
  }
  inherited.symbol.declaration = local.symbol.declaration;
  inherited.symbol.ownerClass = owner.id;
  refreshOverloadPrivacy(inherited.symbol);
}

auto SemanticVisitor::Impl::buildInheritedMembers(
    ClassId id, std::vector<std::uint8_t> &state) -> void {
  if (id == 0 || id > classes.size() || state[id - 1] == 2) {
    return;
  }
  if (state[id - 1] == 1) {
    return;
  }
  state[id - 1] = 1;
  ClassInfo &owner = classInfo(id);
  std::unordered_map<std::string, MemberInfo> declared =
      std::move(owner.members);
  owner.members.clear();

  for (const ClassBaseTypeInfo &base : owner.bases) {
    buildInheritedMembers(base.type.classId, state);
    const ClassInfo &baseOwner = classInfo(base.type.classId);
    owner.polymorphic = owner.polymorphic || baseOwner.polymorphic;
    for (const auto &[name, member] : baseOwner.members) {
      MemberInfo inherited = member;
      inherited.symbol = substituteSymbol(member.symbol, base.type);
      mergeInheritedMember(owner, name, std::move(inherited));
    }
  }

  for (auto &[name, member] : declared) {
    mergeDeclaredMember(owner, name, std::move(member));
  }

  owner.abstract = owner.kind == ClassKind::Interface;
  for (const auto &[name, member] : owner.members) {
    (void)name;
    if (member.symbol.type != SemanticType::Function) {
      continue;
    }
    for (const CallableCandidate &candidate : member.symbol.overloads) {
      owner.polymorphic = owner.polymorphic || candidate.virtualMethod;
      owner.abstract = owner.abstract || candidate.pureVirtual;
    }
    for (std::size_t left = 0; left < member.symbol.overloads.size(); ++left) {
      for (std::size_t right = left + 1; right < member.symbol.overloads.size();
           ++right) {
        const CallableCandidate &first = member.symbol.overloads[left];
        const CallableCandidate &second = member.symbol.overloads[right];
        if (!sameFunctionSignature(first, second)) {
          continue;
        }
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2043", DiagnosticPhase::Semantics, owner.name,
            "Inherited virtual contract '" + name +
                "' is ambiguous; redeclare one exact method with "
                "'override' to unify it.");
        diagnostic.related.push_back(
            {tokenSpan(first.declaration->name()), "First contract is here."});
        diagnostic.related.push_back({tokenSpan(second.declaration->name()),
                                      "Second contract is here."});
        diagnostics.emplace_back(std::move(diagnostic));
      }
    }
  }
  state[id - 1] = 2;
}

auto SemanticVisitor::Impl::resolveInheritedMembers() -> void {
  std::vector<std::uint8_t> state(classes.size(), 0);
  for (const ClassInfo &owner : classes) {
    buildInheritedMembers(owner.id, state);
  }
}

auto SemanticVisitor::Impl::concreteBase(const ClassInfo &owner)
    -> const ClassBaseTypeInfo * {
  const auto found = std::find_if(
      owner.bases.begin(), owner.bases.end(),
      [](const ClassBaseTypeInfo &base) { return !base.interface; });
  return found == owner.bases.end() ? nullptr : &*found;
}

auto SemanticVisitor::Impl::defaultConstructor(const ClassInfo &owner)
    -> const ConstructorInfo * {
  const ConstructorInfo *result = nullptr;
  for (const ConstructorInfo &constructor : owner.constructors) {
    if (constructor.requiredParameterCount != 0) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = &constructor;
  }
  return result;
}

auto SemanticVisitor::Impl::defaultConstructorCandidateCount(
    const ClassInfo &owner) -> std::size_t {
  return static_cast<std::size_t>(
      std::count_if(owner.constructors.begin(), owner.constructors.end(),
                    [](const ConstructorInfo &constructor) {
                      return constructor.requiredParameterCount == 0;
                    }));
}

auto SemanticVisitor::Impl::requestsStructuralDefaultConstruction(
    const ConstructorInfo &constructor) -> bool {
  if (constructor.kind != ConstructorKind::Ordinary ||
      constructor.declaration == nullptr ||
      !constructor.declaration->specifier() ||
      constructor.declaration->specifier()->kind !=
          SpecialMemberSpecifierKind::Defaulted) {
    return false;
  }
  const ConstructorDecl &declaration = *constructor.declaration;
  return declaration.genericParameters().empty() &&
         declaration.parameters().empty() && !declaration.requiresClause() &&
         declaration.initializers().empty();
}

auto SemanticVisitor::Impl::hasSourceDestructorBody(const ClassInfo &owner)
    -> bool {
  return owner.destructor && owner.destructor->declaration != nullptr &&
         owner.destructor->declaration->body() != nullptr;
}

auto SemanticVisitor::Impl::classCanUseStructuralDefaultConstruction(
    const ClassInfo &owner) const -> bool {
  return classCanUseStructuralDefaultConstruction(openClassType(owner.id));
}

auto SemanticVisitor::Impl::classCanUseStructuralDefaultConstruction(
    const SemanticType &type, std::vector<SemanticType> &visiting) const
    -> bool {
  const ClassInfo *owner = classInfo(type);
  // Abstract owners still need constructor availability as base subobjects.
  // Object-position admission and nominal fields reject abstract values.
  if (owner == nullptr)
    return false;
  if (owner->kind == ClassKind::Union)
    return owner->unionLayout.has_value();
  if (visiting.size() >= 64 ||
      std::find(visiting.begin(), visiting.end(), type) != visiting.end())
    return false;
  visiting.push_back(type);
  const GenericSubstitution substitution = classSubstitution(type);
  const auto available = [&](const SemanticType &fieldType,
                             bool baseSubobject = false) {
    const ClassInfo *fieldOwner = classInfo(fieldType);
    if (fieldOwner == nullptr || (fieldOwner->abstract && !baseSubobject))
      return false;
    // An open value expression is a deferred instance obligation, not a
    // usable construction operation. Concrete analysis checks it afresh.
    if (std::any_of(fieldType.valueArguments.begin(),
                    fieldType.valueArguments.end(),
                    [](const CompileTimeValue &value) {
                      return value.kind != CompileTimeValue::Kind::UInt64;
                    }))
      return true;
    const ConstructorInfo *constructor = defaultConstructor(*fieldOwner);
    if (constructor != nullptr) {
      return constructor->access == AccessModifier::Public &&
             resolveDefaultConstruction(fieldType, baseSubobject) &&
             (!requestsStructuralDefaultConstruction(*constructor) ||
              classCanUseStructuralDefaultConstruction(fieldType, visiting));
    }
    const bool declaredDefaults = std::all_of(
        fieldOwner->fields.begin(), fieldOwner->fields.end(),
        [](const FieldInfo &field) {
          return field.declaration && field.declaration->initializer();
        });
    return defaultConstructorCandidateCount(*fieldOwner) == 0 &&
           (fieldOwner->constructors.empty() || declaredDefaults) &&
           classCanUseStructuralDefaultConstruction(fieldType, visiting);
  };
  bool valid = true;
  for (const FieldInfo &field : owner->fields) {
    if (field.declaration == nullptr) {
      valid = false;
      break;
    }
    if (field.declaration->initializer())
      continue;
    const auto member = owner->members.find(field.declaration->name().lexeme);
    if (member == owner->members.end() ||
        member->second.symbol.type.kind != SemanticType::Class ||
        !available(substituteType(member->second.symbol.type, substitution))) {
      valid = false;
      break;
    }
  }
  if (valid) {
    if (const ClassBaseTypeInfo *base = concreteBase(*owner))
      valid = available(substituteType(base->type, substitution), true);
  }
  visiting.pop_back();
  return valid;
}

auto SemanticVisitor::Impl::classCanUseStructuralDefaultConstruction(
    const SemanticType &type) const -> bool {
  std::vector<SemanticType> visiting;
  return classCanUseStructuralDefaultConstruction(type, visiting);
}

auto SemanticVisitor::Impl::classCanGenerateDefaultConstructor(
    const SemanticType &type) const -> bool {
  const ClassInfo *owner = classInfo(type);
  const bool declaredDefaults =
      owner != nullptr &&
      std::all_of(owner->fields.begin(), owner->fields.end(),
                  [](const FieldInfo &field) {
                    return field.declaration &&
                           field.declaration->initializer();
                  });
  // Preserve the established generated overload for explicitly initialized
  // state. Newly supplied omitted-nominal defaults cannot manufacture another
  // construction entry point beside an owner's declared argument-taking API.
  return owner != nullptr && defaultConstructorCandidateCount(*owner) == 0 &&
         (owner->constructors.empty() || declaredDefaults) &&
         classCanUseStructuralDefaultConstruction(type);
}

auto SemanticVisitor::Impl::classCanGenerateDefaultConstructor(
    const ClassInfo &owner) const -> bool {
  return classCanGenerateDefaultConstructor(openClassType(owner.id));
}

auto SemanticVisitor::Impl::baseHasAccessibleDefaultConstructor(
    const ClassBaseTypeInfo &base, ClassId derived) const -> bool {
  if (base.type.classId == 0 || base.type.classId > classes.size()) {
    return false;
  }
  const ClassInfo &owner = classInfo(base.type.classId);
  if (const ConstructorInfo *declared = defaultConstructor(owner)) {
    if (declared->access != AccessModifier::Public && owner.id != derived) {
      return false;
    }
    return !requestsStructuralDefaultConstruction(*declared) ||
           classCanUseStructuralDefaultConstruction(owner);
  }
  return classCanGenerateDefaultConstructor(owner);
}

auto SemanticVisitor::Impl::analyzeBaseConstructorInitializer(
    const ConstructorInitializer &initializer, const SemanticType &baseType)
    -> void {
  if (baseType.kind != SemanticType::Class || baseType.classId == 0 ||
      baseType.classId > classes.size()) {
    return;
  }
  const ClassInfo &baseOwner = classInfo(baseType.classId);
  const bool enclosingConstructorInitializer = analyzingConstructorInitializer;
  analyzingConstructorInitializer = true;
  std::vector<SemanticType> argumentTypes;
  argumentTypes.reserve(initializer.arguments.size());
  for (const ExprPtr &argument : initializer.arguments) {
    argumentTypes.emplace_back(analyze(argument));
  }
  analyzingConstructorInitializer = enclosingConstructorInitializer;

  const GenericSubstitution substitution = classSubstitution(baseType);
  struct ViableConstructor {
    const ConstructorInfo *constructor = nullptr;
    std::vector<SemanticType> parameterTypes;
    bool generatedDefault = false;
  };
  std::vector<ViableConstructor> viable;
  const Token &location = initializer.target.name.last();
  for (const ConstructorInfo &constructor : baseOwner.constructors) {
    if (!isCompilerTrustedRequester(location) && constructor.compilerPrivate) {
      continue;
    }
    if (argumentTypes.size() < constructor.requiredParameterCount ||
        argumentTypes.size() > constructor.parameterTypes.size()) {
      continue;
    }
    std::vector<SemanticType> parameterTypes;
    parameterTypes.reserve(constructor.parameterTypes.size());
    bool exact = true;
    for (std::size_t index = 0; index < constructor.parameterTypes.size();
         ++index) {
      const SemanticType parameter =
          substituteType(constructor.parameterTypes[index], substitution);
      parameterTypes.emplace_back(parameter);
      if (index >= argumentTypes.size()) {
        continue;
      }
      if (!callArgumentMatches(parameter, argumentTypes[index],
                               initializer.arguments[index])) {
        exact = false;
        break;
      }
    }
    if (exact) {
      viable.push_back({&constructor, std::move(parameterTypes),
                        requestsStructuralDefaultConstruction(constructor)});
    }
  }
  if (initializer.arguments.empty() &&
      defaultConstructor(baseOwner) == nullptr &&
      classCanGenerateDefaultConstructor(baseOwner)) {
    viable.push_back({nullptr, {}, true});
  }

  retainBestCallConversionMatches(viable, argumentTypes,
                                  [](const ViableConstructor &candidate)
                                      -> const std::vector<SemanticType> & {
                                    return candidate.parameterTypes;
                                  });

  const bool hasUnknownArgument = std::any_of(
      argumentTypes.begin(), argumentTypes.end(),
      [](const SemanticType &type) { return type == SemanticType::Unknown; });
  if (viable.size() != 1) {
    if (!hasUnknownArgument) {
      std::string arguments;
      for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
        if (index != 0) {
          arguments += ", ";
        }
        arguments += typeSpelling(argumentTypes[index]);
      }
      Diagnostic diagnostic = makeDiagnostic(
          viable.empty() ? "GTI-S2012" : "GTI-S2013",
          DiagnosticPhase::Semantics, location,
          viable.empty()
              ? "No constructor of base '" + typeSpelling(baseType) +
                    "' exactly matches argument types (" + arguments + ")."
              : "Base construction of '" + typeSpelling(baseType) +
                    "' is ambiguous.");
      for (const ConstructorInfo &candidate : baseOwner.constructors) {
        if (candidate.declaration != nullptr &&
            (isCompilerTrustedRequester(location) ||
             !candidate.compilerPrivate)) {
          diagnostic.related.push_back(
              {tokenSpan(candidate.declaration->name()),
               "Candidate: " + constructorSignatureSpelling(candidate)});
        }
      }
      diagnostics.emplace_back(std::move(diagnostic));
    }
    return;
  }

  const ViableConstructor &selected = viable.front();
  if (selected.constructor != nullptr &&
      selected.constructor->access != AccessModifier::Public) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2007", DiagnosticPhase::Semantics, location,
        "Constructor of base '" + typeSpelling(baseType) + "' is private.");
    diagnostic.related.push_back(
        {tokenSpan(selected.constructor->declaration->name()),
         "Constructor declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }
  semanticModel.record(
      initializer,
      ResolvedConstructorInitializerInfo{
          .kind = ConstructorInitializerTargetKind::Base,
          .targetType = baseType,
          .constructor =
              selected.constructor == nullptr ? 0 : selected.constructor->id,
          .declaration = selected.constructor == nullptr
                             ? nullptr
                             : selected.constructor->declaration,
          .parameterTypes = selected.parameterTypes,
          .defaultArguments =
              selected.constructor == nullptr ||
                      selected.constructor->declaration == nullptr
                  ? std::vector<const Expr *>{}
                  : omittedDefaultArguments(
                        selected.constructor->declaration->parameters(),
                        initializer.arguments.size()),
          .generatedDefault = selected.generatedDefault});
  recordProgramArgumentBoundaryEffects(selected.parameterTypes,
                                       initializer.arguments);
  const SourceSpan span = tokenSpan(location);
  recordProgramConstructorDefaultArgumentEffects(
      selected.constructor, initializer.arguments.size(), span);
  if (selected.generatedDefault) {
    recordProgramDefaultConstructionEffects(baseType, span);
  } else {
    addProgramEffectEdge({.kind = ProgramEffectOwnerKind::FieldInitializers,
                          .id = baseType.classId},
                         span);
    recordProgramSelectedConstructorEffect(selected.constructor, baseType,
                                           span);
  }
}

auto SemanticVisitor::Impl::reportCAbiRecord(const Token &location,
                                             std::string message) -> void {
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2064", DiagnosticPhase::Semantics, location, std::move(message));
  diagnostic.hints.emplace_back(
      "A [[c_abi]] struct is a non-generic, inheritance-free passive record "
      "containing only public instance fields of admitted C scalar types, "
      "recursive raw pointers, native callbacks, fixed arrays, or other "
      "[[c_abi]] "
      "records.");
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::isC17Keyword(std::string_view spelling) -> bool {
  static constexpr std::string_view keywords[]{
      "_Alignas",      "_Alignof",  "_Atomic",
      "_Bool",         "_Complex",  "_Generic",
      "_Imaginary",    "_Noreturn", "_Static_assert",
      "_Thread_local", "auto",      "break",
      "case",          "char",      "const",
      "continue",      "default",   "do",
      "double",        "else",      "enum",
      "extern",        "float",     "for",
      "goto",          "if",        "inline",
      "int",           "long",      "register",
      "restrict",      "return",    "short",
      "signed",        "sizeof",    "static",
      "struct",        "switch",    "typedef",
      "union",         "unsigned",  "void",
      "volatile",      "while",
  };
  return std::find(std::begin(keywords), std::end(keywords), spelling) !=
         std::end(keywords);
}

auto SemanticVisitor::Impl::isNativeBridgeSupportTypeName(
    std::string_view spelling) -> bool {
  static constexpr std::string_view names[]{
      "FILE",           "gti_c_string_view", "ptrdiff_t",     "size_t",
      "max_align_t",    "wchar_t",           "int8_t",        "int16_t",
      "int32_t",        "int64_t",           "uint8_t",       "uint16_t",
      "uint32_t",       "uint64_t",          "int_least8_t",  "int_least16_t",
      "int_least32_t",  "int_least64_t",     "uint_least8_t", "uint_least16_t",
      "uint_least32_t", "uint_least64_t",    "int_fast8_t",   "int_fast16_t",
      "int_fast32_t",   "int_fast64_t",      "uint_fast8_t",  "uint_fast16_t",
      "uint_fast32_t",  "uint_fast64_t",     "intptr_t",      "uintptr_t",
      "intmax_t",       "uintmax_t",
  };
  return std::find(std::begin(names), std::end(names), spelling) !=
         std::end(names);
}

auto SemanticVisitor::Impl::isNativeBridgeObjectMacroName(
    std::string_view spelling) -> bool {
  static constexpr std::string_view names[]{
      "BUFSIZ",
      "EOF",
      "FILENAME_MAX",
      "FOPEN_MAX",
      "GTI_NATIVE_HEADER_NO_SOURCE_NAMES",
      "L_tmpnam",
      "NULL",
      "SEEK_CUR",
      "SEEK_END",
      "SEEK_SET",
      "SIZE_MAX",
      "TMP_MAX",
      "stderr",
      "stdin",
      "stdout",
  };
  if (std::find(std::begin(names), std::end(names), spelling) !=
      std::end(names)) {
    return true;
  }
  static constexpr std::string_view signedPrefixes[]{
      "INT8",        "INT16",       "INT32",       "INT64",     "INT_LEAST8",
      "INT_LEAST16", "INT_LEAST32", "INT_LEAST64", "INT_FAST8", "INT_FAST16",
      "INT_FAST32",  "INT_FAST64",  "INTPTR",      "INTMAX",    "PTRDIFF",
      "SIG_ATOMIC",  "WCHAR",       "WINT",
  };
  static constexpr std::string_view unsignedPrefixes[]{
      "UINT8",       "UINT16",       "UINT32",       "UINT64",
      "UINT_LEAST8", "UINT_LEAST16", "UINT_LEAST32", "UINT_LEAST64",
      "UINT_FAST8",  "UINT_FAST16",  "UINT_FAST32",  "UINT_FAST64",
      "UINTPTR",     "UINTMAX",
  };
  const auto matchesSuffix = [&](std::span<const std::string_view> prefixes,
                                 std::string_view suffix) {
    return std::any_of(
        prefixes.begin(), prefixes.end(), [&](std::string_view prefix) {
          return spelling.size() == prefix.size() + suffix.size() &&
                 spelling.starts_with(prefix) && spelling.ends_with(suffix);
        });
  };
  return matchesSuffix(signedPrefixes, "_MIN") ||
         matchesSuffix(signedPrefixes, "_MAX") ||
         matchesSuffix(unsignedPrefixes, "_MAX");
}

auto SemanticVisitor::Impl::isNativeBridgeFunctionMacroName(
    std::string_view spelling) -> bool {
  if (spelling == "offsetof") {
    return true;
  }
  static constexpr std::string_view prefixes[]{
      "INT8",   "INT16",  "INT32",  "INT64",  "UINT8",
      "UINT16", "UINT32", "UINT64", "INTMAX", "UINTMAX",
  };
  return std::any_of(
      std::begin(prefixes), std::end(prefixes), [&](std::string_view prefix) {
        return spelling.size() == prefix.size() + 2 &&
               spelling.starts_with(prefix) && spelling.ends_with("_C");
      });
}

auto SemanticVisitor::Impl::reservedNativeIdentifierConflict(
    std::string_view spelling, bool occupiesCFileScope)
    -> std::optional<std::string> {
  if (spelling.size() >= 2 && spelling.front() == '_' &&
      (spelling[1] == '_' || (spelling[1] >= 'A' && spelling[1] <= 'Z'))) {
    return "an implementation-reserved identifier spelling";
  }
  if (spelling.find("__") != std::string_view::npos) {
    return "an implementation-reserved C++ identifier spelling";
  }
  if (occupiesCFileScope && spelling.starts_with('_')) {
    return "an implementation-reserved C file-scope identifier spelling";
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::nativeCIdentifierConflict(
    std::string_view spelling, bool exposedWithExactCSpelling,
    bool occupiesCOrdinaryNamespace, bool followedByLeftParen)
    -> std::optional<std::string> {
  if (exposedWithExactCSpelling && isC17Keyword(spelling)) {
    return "the C17 keyword '" + std::string(spelling) + "'";
  }
  if (const std::optional<std::string> conflict =
          reservedNativeIdentifierConflict(spelling,
                                           occupiesCOrdinaryNamespace)) {
    return conflict;
  }
  if (isNativeBridgeObjectMacroName(spelling) ||
      (followedByLeftParen && isNativeBridgeFunctionMacroName(spelling))) {
    return "the support macro '" + std::string(spelling) +
           "' provided by the generated bridge headers";
  }
  if (!occupiesCOrdinaryNamespace) {
    return std::nullopt;
  }
  if (isNativeBridgeSupportTypeName(spelling)) {
    return "the support type name '" + std::string(spelling) +
           "' provided by the generated bridge headers";
  }
  if (spelling.starts_with("gti_cabi_")) {
    return "the reserved generated native-record prefix 'gti_cabi_'";
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::validateNativeCIdentifier(
    const Token &identifier, std::string_view diagnosticCode,
    std::string_view entity, bool exposedWithExactCSpelling,
    bool occupiesCOrdinaryNamespace, bool followedByLeftParen) -> bool {
  return validateNativeCIdentifierSpelling(
      identifier.lexeme, identifier, diagnosticCode, entity,
      exposedWithExactCSpelling, occupiesCOrdinaryNamespace,
      followedByLeftParen);
}

auto SemanticVisitor::Impl::isPortableNativeCIdentifier(
    std::string_view spelling) -> bool {
  const auto initial = [](char value) {
    return value == '_' || (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z');
  };
  const auto continuation = [&](char value) {
    return initial(value) || (value >= '0' && value <= '9');
  };
  return !spelling.empty() && initial(spelling.front()) &&
         std::all_of(spelling.begin() + 1, spelling.end(), continuation);
}

auto SemanticVisitor::Impl::validateNativeCIdentifierSpelling(
    std::string_view spelling, const Token &location,
    std::string_view diagnosticCode, std::string_view entity,
    bool exposedWithExactCSpelling, bool occupiesCOrdinaryNamespace,
    bool followedByLeftParen) -> bool {
  if (!isPortableNativeCIdentifier(spelling)) {
    Diagnostic diagnostic = makeDiagnostic(
        std::string(diagnosticCode), DiagnosticPhase::Semantics, location,
        std::string(entity) + " '" + std::string(spelling) +
            "' is not a portable C identifier.");
    diagnostic.hints.emplace_back(
        "Use an ASCII identifier beginning with a letter or underscore and "
        "containing only letters, digits, and underscores.");
    diagnostics.emplace_back(std::move(diagnostic));
    return false;
  }
  const std::optional<std::string> conflict = nativeCIdentifierConflict(
      spelling, exposedWithExactCSpelling, occupiesCOrdinaryNamespace,
      followedByLeftParen);
  if (!conflict) {
    return true;
  }
  Diagnostic diagnostic = makeDiagnostic(
      std::string(diagnosticCode), DiagnosticPhase::Semantics, location,
      std::string(entity) + " '" + std::string(spelling) + "' conflicts with " +
          *conflict + ".");
  diagnostic.hints.emplace_back(
      "Rename this native-facing identifier; generated native declarations "
      "preserve this spelling and cannot safely rewrite the ABI surface.");
  diagnostics.emplace_back(std::move(diagnostic));
  return false;
}

auto SemanticVisitor::Impl::validateNativeFacingNamespaces(
    const StmtList &declarations, std::size_t depth) -> bool {
  const SourceUnitId enclosingSourceUnit = currentSourceUnit;
  bool containsNativeSurface = false;
  for (const StmtPtr &statement : declarations) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        containsNativeSurface =
            validateNativeFacingNamespaces(*branch, depth) ||
            containsNativeSurface;
      }
      continue;
    }
    if (const auto *nameSpace = stmtAs<NamespaceDecl>(statement.get())) {
      const bool childContainsNative =
          validateNativeFacingNamespaces(nameSpace->declarations(), depth + 1);
      if (childContainsNative) {
        currentSourceUnit = sourceUnitFor(nameSpace->name());
        (void)validateNativeCIdentifier(nameSpace->name(), "GTI-S2054",
                                        "Native-facing namespace component",
                                        false, depth == 0);
        containsNativeSurface = true;
      }
      continue;
    }
    if (const auto *classDeclaration = stmtAs<ClassDecl>(statement.get())) {
      const auto found = classDeclIds.find(classDeclaration);
      if (found != classDeclIds.end()) {
        const ClassInfo &owner = classInfo(found->second);
        containsNativeSurface =
            containsNativeSurface ||
            (!owner.compilerPrivate &&
             ((owner.cAbiRecord && owner.cAbiLayout.has_value()) ||
              owner.cOpaqueHandle));
      }
      continue;
    }
    const auto *externC = stmtAs<ExternCDecl>(statement.get());
    if (externC == nullptr) {
      continue;
    }
    for (const StmtPtr &nativeDeclaration : externC->declarations()) {
      const auto *function = stmtAs<FunctionDecl>(nativeDeclaration.get());
      const FunctionInfo *info =
          function == nullptr ? nullptr : semanticModel.findFunction(*function);
      if (info != nullptr && info->linkage == LanguageLinkage::C &&
          !info->compilerPrivate) {
        containsNativeSurface = true;
        break;
      }
    }
  }
  currentSourceUnit = enclosingSourceUnit;
  return containsNativeSurface;
}

auto SemanticVisitor::Impl::reportCOpaqueHandle(const Token &location,
                                                std::string message) -> void {
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2065", DiagnosticPhase::Semantics, location, std::move(message));
  diagnostic.hints.emplace_back(
      "Declare an incomplete native handle as '[[c_opaque]] struct Name;' "
      "and use it only through 'Name*' or 'const Name*'.");
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::rejectCOpaquePointeeOperation(
    const SemanticType &pointer, const Token &location,
    std::string_view operation) -> bool {
  if (pointer.kind != SemanticType::RawPointer ||
      pointer.arguments.size() != 1 ||
      !isCOpaqueHandleType(pointer.arguments.front())) {
    return false;
  }
  const SemanticType &pointee = pointer.arguments.front();
  const ClassInfo &owner = classInfo(pointee.classId);
  Diagnostic diagnostic =
      makeDiagnostic("GTI-S2065", DiagnosticPhase::Semantics, location,
                     "Opaque C handle '" + typeSpelling(pointee) +
                         "' is incomplete and does not support " +
                         std::string(operation) + ".");
  if (owner.cOpaqueAttribute) {
    diagnostic.related.push_back(
        {tokenSpan(*owner.cOpaqueAttribute),
         "The pointer-only opaque handle is declared here."});
  }
  diagnostic.hints.emplace_back(
      "An opaque-handle pointer may be initialized, copied, assigned, "
      "compared with a compatible pointer or nullptr, passed, and returned; "
      "its hidden pointee cannot be accessed even inside unsafe.");
  diagnostics.emplace_back(std::move(diagnostic));
  return true;
}

auto SemanticVisitor::Impl::validateCOpaqueHandles() -> void {
  for (ClassInfo &owner : classes) {
    if (owner.declaration == nullptr) {
      continue;
    }
    if (owner.declaration->isForwardDeclaration() && !owner.cOpaqueHandle) {
      reportCOpaqueHandle(
          owner.name,
          "Incomplete type declaration '" + owner.name.lexeme +
              "' requires the [[c_opaque]] native-handle contract.");
      continue;
    }
    if (!owner.cOpaqueHandle) {
      continue;
    }
    (void)validateNativeCIdentifier(
        owner.name, "GTI-S2065", "Opaque C handle name",
        owner.namespaceScope.empty(), owner.namespaceScope.empty());
    if (owner.kind != ClassKind::Struct) {
      continue;
    }
    if (!owner.declaration->isForwardDeclaration()) {
      reportCOpaqueHandle(
          owner.name, "Opaque C handle '" + owner.name.lexeme +
                          "' must be incomplete and cannot define a GTI body.");
    }
    if (!owner.genericParameters.empty()) {
      reportCOpaqueHandle(owner.genericParameters.front().name,
                          "Opaque C handle '" + owner.name.lexeme +
                              "' cannot declare generic parameters.");
    }
    if (!owner.declaration->bases().empty()) {
      reportCOpaqueHandle(owner.declaration->bases().front().type.name.last(),
                          "Opaque C handle '" + owner.name.lexeme +
                              "' cannot declare base types.");
    }
  }
}

auto SemanticVisitor::Impl::validateCAbiAccessSpecifiers(
    const StmtList &members, const ClassInfo &owner, bool &valid) -> void {
  for (const StmtPtr &statement : members) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        validateCAbiAccessSpecifiers(*branch, owner, valid);
      }
      continue;
    }
    if (const auto *specifier = stmtAs<AccessSpecifierDecl>(statement.get())) {
      reportCAbiRecord(
          specifier->keyword(),
          "C ABI record '" + owner.name.lexeme +
              "' cannot declare access sections; every field is public.");
      valid = false;
    }
  }
}

auto SemanticVisitor::Impl::cAbiPointerPointeeAllowed(
    const SemanticType &type) const -> bool {
  if (type == SemanticType::Void || isCAbiScalar(type) ||
      type == SemanticType::CString || isCAbiNativeFunction(type)) {
    return true;
  }
  if (type.kind == SemanticType::RawPointer && type.arguments.size() == 1)
    return cAbiPointerPointeeAllowed(type.arguments.front());
  return type.kind == SemanticType::Class && type.classId != 0 &&
         type.classId <= classes.size() &&
         (classInfo(type.classId).cAbiRecord ||
          classInfo(type.classId).cOpaqueHandle);
}

auto SemanticVisitor::Impl::cAbiFieldTypeAllowed(const SemanticType &type) const
    -> bool {
  if (isCAbiScalar(type) || isCAbiNativeFunction(type)) {
    return true;
  }
  if (type.kind == SemanticType::Array && type.arguments.size() == 1 &&
      type.arrayLength != 0 && type.arrayLengthParameterId == 0) {
    return cAbiFieldTypeAllowed(type.arguments.front());
  }
  if (type.kind == SemanticType::RawPointer && type.arguments.size() == 1) {
    return cAbiPointerPointeeAllowed(type.arguments.front());
  }
  return type.kind == SemanticType::Class && type.classId != 0 &&
         type.classId <= classes.size() && classInfo(type.classId).cAbiRecord;
}

auto SemanticVisitor::Impl::validateCAbiRecordShape(ClassInfo &owner) -> bool {
  if (!owner.cAbiRecord || owner.declaration == nullptr) {
    return false;
  }
  bool valid = owner.kind == ClassKind::Struct;
  valid = validateNativeCIdentifier(
              owner.name, "GTI-S2064", "C ABI record name",
              owner.namespaceScope.empty(), owner.namespaceScope.empty()) &&
          valid;
  if (!owner.genericParameters.empty()) {
    reportCAbiRecord(owner.genericParameters.front().name,
                     "C ABI record '" + owner.name.lexeme +
                         "' cannot declare generic parameters.");
    valid = false;
  }
  for (const ClassBaseTypeInfo &base : owner.bases) {
    reportCAbiRecord(base.syntax == nullptr ? owner.name
                                            : base.syntax->type.name.last(),
                     "C ABI record '" + owner.name.lexeme +
                         "' cannot inherit from another type.");
    valid = false;
  }
  validateCAbiAccessSpecifiers(owner.declaration->members(), owner, valid);

  for (const ConstructorInfo &constructor : owner.constructors) {
    if (constructor.declaration != nullptr) {
      reportCAbiRecord(constructor.declaration->name(),
                       "C ABI record '" + owner.name.lexeme +
                           "' cannot declare constructors.");
      valid = false;
    }
  }
  for (const std::optional<ConstructorInfo> *special :
       {&owner.copyConstructor, &owner.moveConstructor}) {
    if (*special && (*special)->declaration != nullptr) {
      reportCAbiRecord((*special)->declaration->name(),
                       "C ABI record '" + owner.name.lexeme +
                           "' cannot declare copy or move policies.");
      valid = false;
    }
  }
  if (owner.destructor && owner.destructor->declaration != nullptr) {
    reportCAbiRecord(owner.destructor->declaration->tilde(),
                     "C ABI record '" + owner.name.lexeme +
                         "' cannot declare cleanup.");
    valid = false;
  }
  for (const auto &[_, member] : owner.members) {
    if (member.symbol.type != SemanticType::Function) {
      continue;
    }
    for (const CallableCandidate &function : member.symbol.overloads) {
      if (function.declaration != nullptr) {
        reportCAbiRecord(function.declaration->name(),
                         "C ABI record '" + owner.name.lexeme +
                             "' cannot declare methods.");
        valid = false;
      }
    }
  }
  for (const FieldInfo &field : owner.staticFields) {
    if (field.declaration != nullptr) {
      reportCAbiRecord(field.declaration->name(),
                       "C ABI record '" + owner.name.lexeme +
                           "' cannot declare static fields.");
      valid = false;
    }
  }
  if (owner.fields.empty()) {
    reportCAbiRecord(owner.name,
                     "C ABI record '" + owner.name.lexeme +
                         "' must contain at least one field; empty C "
                         "records have no portable object representation.");
    valid = false;
  }
  for (const FieldInfo &field : owner.fields) {
    if (field.declaration == nullptr) {
      valid = false;
      continue;
    }
    valid = validateNativeCIdentifier(field.declaration->name(), "GTI-S2064",
                                      "C ABI record field name", true, false) &&
            valid;
    const auto member = owner.members.find(field.declaration->name().lexeme);
    if (member == owner.members.end()) {
      valid = false;
      continue;
    }
    if (member->second.access != AccessModifier::Public) {
      reportCAbiRecord(field.declaration->name(),
                       "Every field of C ABI record '" + owner.name.lexeme +
                           "' must be public.");
      valid = false;
    }
    if (field.declaration->initializer()) {
      reportCAbiRecord(
          field.declaration->name(),
          "Field '" + field.declaration->name().lexeme +
              "' cannot have a GTI initializer in C ABI record '" +
              owner.name.lexeme +
              "'; initialize representation values explicitly in a safe "
              "wrapper or native factory.");
      valid = false;
    }
    const SemanticType &fieldType = member->second.symbol.type;
    if (!field.declaration->type().arrayExtents.empty()) {
      bool extentsValid = true;
      const SemanticType *extentType = &fieldType;
      for (const ArrayExtentExprPtr &extent :
           field.declaration->type().arrayExtents) {
        const bool concrete = extentType->kind == SemanticType::Array &&
                              extentType->arguments.size() == 1 &&
                              extentType->arrayLength != 0 &&
                              extentType->arrayLengthParameterId == 0;
        if (!concrete) {
          const Token &location = extent
                                      ? lastArrayExtentToken(*extent)
                                      : field.declaration->type().name.last();
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2069", DiagnosticPhase::Semantics, location,
              "Fixed-array field '" + field.declaration->name().lexeme +
                  "' in C ABI record '" + owner.name.lexeme +
                  "' requires a positive concrete extent.");
          diagnostic.hints.emplace_back(
              "Use a non-zero integer constant extent; symbolic and "
              "negative extents cannot define a portable C record layout.");
          diagnostics.emplace_back(std::move(diagnostic));
          extentsValid = false;
        }
        if (extentType->kind == SemanticType::Array &&
            extentType->arguments.size() == 1) {
          extentType = &extentType->arguments.front();
        }
      }
      const SemanticType *elementType = &fieldType;
      while (elementType->kind == SemanticType::Array &&
             elementType->arguments.size() == 1) {
        elementType = &elementType->arguments.front();
      }
      if (fieldType != SemanticType::Unknown &&
          !cAbiFieldTypeAllowed(*elementType)) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2070", DiagnosticPhase::Semantics,
            field.declaration->type().name.last(),
            "Fixed-array field '" + field.declaration->name().lexeme +
                "' in C ABI record '" + owner.name.lexeme +
                "' has an element type outside the bounded C ABI field "
                "set.");
        diagnostic.hints.emplace_back(
            "Use fixed-width integer or floating elements, one-level raw "
            "pointers, or valid passive C ABI records.");
        diagnostics.emplace_back(std::move(diagnostic));
        valid = false;
      }
      valid = extentsValid && valid;
      continue;
    }
    if (fieldType == SemanticType::Unknown) {
      valid = false;
      continue;
    }
    if (!cAbiFieldTypeAllowed(fieldType)) {
      reportCAbiRecord(
          field.declaration->type().name.last(),
          "Field '" + field.declaration->name().lexeme + "' has type '" +
              typeSpelling(fieldType) +
              "', which is outside the bounded C ABI record field set.");
      valid = false;
    }
  }
  return valid;
}

auto SemanticVisitor::Impl::alignCAbiOffset(std::uint64_t value,
                                            std::uint32_t alignment,
                                            std::uint64_t &result) -> bool {
  if (alignment == 0) {
    return false;
  }
  const std::uint64_t remainder = value % alignment;
  const std::uint64_t padding = remainder == 0 ? 0 : alignment - remainder;
  if (value > std::numeric_limits<std::uint64_t>::max() - padding) {
    return false;
  }
  result = value + padding;
  return true;
}

auto SemanticVisitor::Impl::computeCAbiRecordLayout(
    ClassId id, std::vector<std::uint8_t> &state,
    const std::vector<bool> &shapeValid) -> std::optional<CAbiRecordLayout> {
  if (id == 0 || id > classes.size() || !shapeValid[id - 1]) {
    return std::nullopt;
  }
  ClassInfo &owner = classInfo(id);
  if (state[id - 1] == 2) {
    return owner.cAbiLayout;
  }
  if (state[id - 1] == 3) {
    return std::nullopt;
  }
  state[id - 1] = 1;
  CAbiRecordLayout layout{.abiAlignmentBytes = 1};
  layout.fields.reserve(owner.fields.size());

  for (const FieldInfo &field : owner.fields) {
    if (field.declaration == nullptr) {
      state[id - 1] = 3;
      return std::nullopt;
    }
    const auto member = owner.members.find(field.declaration->name().lexeme);
    if (member == owner.members.end()) {
      state[id - 1] = 3;
      return std::nullopt;
    }
    const SemanticType &fieldType = member->second.symbol.type;
    std::uint64_t fieldSize = 0;
    std::uint32_t fieldAlignment = 0;
    // A fixed-array field whose ultimate element is a by-value record takes
    // its element facts from this pass: the general target-layout evaluator
    // reads the published semantic model, which does not carry record
    // layouts while this computation is still producing them.
    const SemanticType *arrayElement = nullptr;
    std::uint64_t arrayElementCount = 1;
    bool arrayCountOverflow = false;
    if (fieldType.kind == SemanticType::Array) {
      const SemanticType *node = &fieldType;
      while (node->kind == SemanticType::Array && node->arguments.size() == 1 &&
             node->arrayLength != 0 && node->arrayLengthParameterId == 0) {
        if (arrayElementCount >
            std::numeric_limits<std::uint64_t>::max() / node->arrayLength) {
          arrayCountOverflow = true;
          break;
        }
        arrayElementCount *= node->arrayLength;
        node = &node->arguments.front();
      }
      if (node->kind == SemanticType::Class) {
        arrayElement = node;
      }
    }
    if (arrayElement != nullptr && arrayElement->classId != 0 &&
        arrayElement->classId <= classes.size()) {
      if (arrayCountOverflow) {
        reportCAbiRecord(field.declaration->name(),
                         "Layout of C ABI record '" + owner.name.lexeme +
                             "' exceeds uint64_t while placing field '" +
                             field.declaration->name().lexeme + "'.");
        state[id - 1] = 3;
        return std::nullopt;
      }
      if (state[arrayElement->classId - 1] == 1) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2064", DiagnosticPhase::Semantics,
            field.declaration->type().name.last(),
            "C ABI records cannot contain a recursive by-value field.");
        diagnostic.related.push_back(
            {tokenSpan(classInfo(arrayElement->classId).name),
             "The recursive record is declared here; use a one-level raw "
             "pointer for an opaque or linked record edge."});
        diagnostic.hints.emplace_back(
            "Replace the by-value recursion with a one-level raw pointer, "
            "or move the recursive ownership policy into a safe GTI "
            "wrapper.");
        diagnostics.emplace_back(std::move(diagnostic));
        state[id - 1] = 3;
        return std::nullopt;
      }
      const std::optional<CAbiRecordLayout> element =
          computeCAbiRecordLayout(arrayElement->classId, state, shapeValid);
      if (!element) {
        state[id - 1] = 3;
        return std::nullopt;
      }
      if (element->sizeBytes != 0 &&
          arrayElementCount >
              std::numeric_limits<std::uint64_t>::max() / element->sizeBytes) {
        reportCAbiRecord(field.declaration->name(),
                         "Layout of C ABI record '" + owner.name.lexeme +
                             "' exceeds uint64_t while placing field '" +
                             field.declaration->name().lexeme + "'.");
        state[id - 1] = 3;
        return std::nullopt;
      }
      fieldSize = element->sizeBytes * arrayElementCount;
      fieldAlignment = element->abiAlignmentBytes;
    } else if (isCAbiScalar(fieldType) ||
               fieldType.kind == SemanticType::RawPointer ||
               fieldType.kind == SemanticType::NativeFunction ||
               fieldType.kind == SemanticType::Array) {
      const LayoutEvaluation fieldLayout = evaluateLayout(fieldType);
      if (!fieldLayout) {
        reportCAbiRecord(field.declaration->type().name.last(),
                         "Target layout facts are unavailable for field '" +
                             field.declaration->name().lexeme + "'.");
        state[id - 1] = 3;
        return std::nullopt;
      }
      fieldSize = fieldLayout.sizeBytes;
      fieldAlignment = fieldLayout.abiAlignmentBytes;
    } else if (fieldType.kind == SemanticType::Class &&
               fieldType.classId != 0 && fieldType.classId <= classes.size()) {
      if (state[fieldType.classId - 1] == 1) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2064", DiagnosticPhase::Semantics,
            field.declaration->type().name.last(),
            "C ABI records cannot contain a recursive by-value field.");
        diagnostic.related.push_back(
            {tokenSpan(classInfo(fieldType.classId).name),
             "The recursive record is declared here; use a one-level raw "
             "pointer for an opaque or linked record edge."});
        diagnostic.hints.emplace_back(
            "Replace the by-value recursion with a one-level raw pointer, "
            "or move the recursive ownership policy into a safe GTI "
            "wrapper.");
        diagnostics.emplace_back(std::move(diagnostic));
        state[id - 1] = 3;
        return std::nullopt;
      }
      const std::optional<CAbiRecordLayout> nested =
          computeCAbiRecordLayout(fieldType.classId, state, shapeValid);
      if (!nested) {
        state[id - 1] = 3;
        return std::nullopt;
      }
      fieldSize = nested->sizeBytes;
      fieldAlignment = nested->abiAlignmentBytes;
    } else {
      state[id - 1] = 3;
      return std::nullopt;
    }

    std::uint64_t fieldOffset = 0;
    if (!alignCAbiOffset(layout.sizeBytes, fieldAlignment, fieldOffset) ||
        fieldOffset > std::numeric_limits<std::uint64_t>::max() - fieldSize) {
      reportCAbiRecord(field.declaration->name(),
                       "Layout of C ABI record '" + owner.name.lexeme +
                           "' exceeds uint64_t while placing field '" +
                           field.declaration->name().lexeme + "'.");
      state[id - 1] = 3;
      return std::nullopt;
    }
    layout.fields.push_back({.declaration = field.declaration,
                             .type = fieldType,
                             .offsetBytes = fieldOffset,
                             .sizeBytes = fieldSize,
                             .abiAlignmentBytes = fieldAlignment});
    layout.sizeBytes = fieldOffset + fieldSize;
    layout.abiAlignmentBytes =
        std::max(layout.abiAlignmentBytes, fieldAlignment);
  }

  std::uint64_t finalSize = 0;
  if (!alignCAbiOffset(layout.sizeBytes, layout.abiAlignmentBytes, finalSize)) {
    reportCAbiRecord(owner.name, "Tail padding of C ABI record '" +
                                     owner.name.lexeme + "' exceeds uint64_t.");
    state[id - 1] = 3;
    return std::nullopt;
  }
  layout.sizeBytes = finalSize;
  owner.cAbiLayout = layout;
  state[id - 1] = 2;
  return layout;
}

auto SemanticVisitor::Impl::reportUnion(const Token &location,
                                        std::string message) -> void {
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2066", DiagnosticPhase::Semantics, location, std::move(message));
  diagnostic.hints.emplace_back(
      "Keep native unions passive and use them only inside an 'unsafe' "
      "block; use a payload enum when the active alternative must be "
      "tracked safely.");
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::unionFieldTypeAllowed(
    const SemanticType &type) const -> bool {
  if (layoutScalarKind(type) || type.kind == SemanticType::Enum) {
    if (type.kind != SemanticType::Enum) {
      return true;
    }
    const EnumTypeInfo *enumeration = semanticModel.findEnumType(type.enumId);
    return enumeration != nullptr && !enumeration->payload;
  }
  if (type.kind == SemanticType::Array && type.arguments.size() == 1 &&
      type.arrayLength != 0 && type.arrayLengthParameterId == 0) {
    return unionFieldTypeAllowed(type.arguments.front());
  }
  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size() || !type.arguments.empty() ||
      !type.valueArguments.empty()) {
    return false;
  }
  const ClassInfo &fieldOwner = classInfo(type.classId);
  return fieldOwner.kind == ClassKind::Union ||
         (fieldOwner.cAbiRecord && fieldOwner.cAbiLayout.has_value());
}

auto SemanticVisitor::Impl::validateUnionShape(ClassInfo &owner) -> bool {
  if (owner.kind != ClassKind::Union || owner.declaration == nullptr) {
    return false;
  }
  bool valid = !owner.declaration->isForwardDeclaration() &&
               owner.genericParameters.empty() && owner.bases.empty();
  if (owner.fields.empty()) {
    reportUnion(owner.name, "Union '" + owner.name.lexeme +
                                "' must contain at least one field.");
    valid = false;
  }
  for (const FieldInfo &field : owner.fields) {
    if (field.declaration == nullptr) {
      valid = false;
      continue;
    }
    const auto member = owner.members.find(field.declaration->name().lexeme);
    if (member == owner.members.end()) {
      valid = false;
      continue;
    }
    if (member->second.access != AccessModifier::Public) {
      reportUnion(field.declaration->name(), "Every field of union '" +
                                                 owner.name.lexeme +
                                                 "' must be public.");
      valid = false;
    }
    if (field.declaration->initializer()) {
      reportUnion(field.declaration->name(),
                  "Union field '" + field.declaration->name().lexeme +
                      "' cannot have an initializer.");
      valid = false;
    }
    const SemanticType &fieldType = member->second.symbol.type;
    if (fieldType.kind == SemanticType::Reference) {
      reportUnion(field.declaration->type().name.last(),
                  "Union fields cannot store references.");
      valid = false;
    } else if (fieldType != SemanticType::Unknown &&
               !unionFieldTypeAllowed(fieldType)) {
      reportUnion(field.declaration->type().name.last(),
                  "Union field '" + field.declaration->name().lexeme +
                      "' has non-passive type '" + typeSpelling(fieldType) +
                      "'.");
      valid = false;
    }
  }
  return valid;
}

auto SemanticVisitor::Impl::computeUnionLayout(
    ClassId id, std::vector<std::uint8_t> &state,
    const std::vector<bool> &shapeValid) -> std::optional<UnionLayout> {
  if (id == 0 || id > classes.size() || !shapeValid[id - 1]) {
    return std::nullopt;
  }
  ClassInfo &owner = classInfo(id);
  if (state[id - 1] == 2) {
    return owner.unionLayout;
  }
  if (state[id - 1] == 3) {
    return std::nullopt;
  }
  state[id - 1] = 1;
  UnionLayout layout{.abiAlignmentBytes = 1};
  layout.fields.reserve(owner.fields.size());

  const auto fieldLayout = [&](const auto &self, const SemanticType &type,
                               const Token &location) -> LayoutEvaluation {
    if (type.kind == SemanticType::Array) {
      if (type.arguments.size() != 1 || type.arrayLength == 0 ||
          type.arrayLengthParameterId != 0) {
        return {.error = LayoutEvaluationError::UnsupportedType};
      }
      const LayoutEvaluation element =
          self(self, type.arguments.front(), location);
      if (!element) {
        return element;
      }
      if (element.sizeBytes >
          std::numeric_limits<std::uint64_t>::max() / type.arrayLength) {
        return {.error = LayoutEvaluationError::SizeOverflow};
      }
      return {.sizeBytes = element.sizeBytes * type.arrayLength,
              .abiAlignmentBytes = element.abiAlignmentBytes};
    }
    if (type.kind == SemanticType::Class && type.classId != 0 &&
        type.classId <= classes.size()) {
      ClassInfo &nestedOwner = classInfo(type.classId);
      if (nestedOwner.kind == ClassKind::Union) {
        if (state[type.classId - 1] == 1) {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2066", DiagnosticPhase::Semantics, location,
              "Unions cannot contain a recursive by-value field.");
          diagnostic.related.push_back(
              {tokenSpan(nestedOwner.name),
               "The recursive union is declared here."});
          diagnostic.hints.emplace_back(
              "Use a one-level raw pointer for the recursive edge, or a "
              "safe owning standard-library wrapper at a higher layer.");
          diagnostics.emplace_back(std::move(diagnostic));
          return {.error = LayoutEvaluationError::UnsupportedType};
        }
        const std::optional<UnionLayout> nested =
            computeUnionLayout(type.classId, state, shapeValid);
        return nested ? LayoutEvaluation{.sizeBytes = nested->sizeBytes,
                                         .abiAlignmentBytes =
                                             nested->abiAlignmentBytes}
                      : LayoutEvaluation{
                            .error = LayoutEvaluationError::UnsupportedType};
      }
      if (nestedOwner.cAbiRecord && nestedOwner.cAbiLayout) {
        return {.sizeBytes = nestedOwner.cAbiLayout->sizeBytes,
                .abiAlignmentBytes = nestedOwner.cAbiLayout->abiAlignmentBytes};
      }
    }
    return evaluateLayout(type);
  };

  for (const FieldInfo &field : owner.fields) {
    if (field.declaration == nullptr) {
      state[id - 1] = 3;
      return std::nullopt;
    }
    const auto member = owner.members.find(field.declaration->name().lexeme);
    if (member == owner.members.end()) {
      state[id - 1] = 3;
      return std::nullopt;
    }
    const LayoutEvaluation fieldResult =
        fieldLayout(fieldLayout, member->second.symbol.type,
                    field.declaration->type().name.last());
    if (!fieldResult) {
      reportUnion(field.declaration->type().name.last(),
                  "Target layout facts are unavailable for union field '" +
                      field.declaration->name().lexeme + "'.");
      state[id - 1] = 3;
      return std::nullopt;
    }
    layout.fields.push_back(
        {.declaration = field.declaration,
         .type = member->second.symbol.type,
         .sizeBytes = fieldResult.sizeBytes,
         .abiAlignmentBytes = fieldResult.abiAlignmentBytes});
    layout.sizeBytes = std::max(layout.sizeBytes, fieldResult.sizeBytes);
    layout.abiAlignmentBytes =
        std::max(layout.abiAlignmentBytes, fieldResult.abiAlignmentBytes);
  }
  std::uint64_t finalSize = 0;
  if (!alignCAbiOffset(layout.sizeBytes, layout.abiAlignmentBytes, finalSize)) {
    reportUnion(owner.name, "Layout of union '" + owner.name.lexeme +
                                "' exceeds uint64_t.");
    state[id - 1] = 3;
    return std::nullopt;
  }
  layout.sizeBytes = finalSize;
  owner.unionLayout = layout;
  state[id - 1] = 2;
  return layout;
}

auto SemanticVisitor::Impl::validateUnions() -> void {
  std::vector<bool> shapeValid(classes.size(), false);
  for (ClassInfo &owner : classes) {
    if (owner.kind == ClassKind::Union) {
      shapeValid[owner.id - 1] = validateUnionShape(owner);
    }
  }
  std::vector<std::uint8_t> state(classes.size(), 0);
  for (ClassInfo &owner : classes) {
    if (owner.kind == ClassKind::Union && shapeValid[owner.id - 1] &&
        !computeUnionLayout(owner.id, state, shapeValid)) {
      state[owner.id - 1] = 3;
    }
  }
}

auto SemanticVisitor::Impl::validateNativeRecords() -> void {
  validateCOpaqueHandles();
  std::vector<bool> shapeValid(classes.size(), false);
  for (ClassInfo &owner : classes) {
    if (owner.cAbiRecord) {
      shapeValid[owner.id - 1] = validateCAbiRecordShape(owner);
    }
  }
  std::vector<std::uint8_t> state(classes.size(), 0);
  for (ClassInfo &owner : classes) {
    if (owner.cAbiRecord && shapeValid[owner.id - 1] &&
        !computeCAbiRecordLayout(owner.id, state, shapeValid)) {
      state[owner.id - 1] = 3;
    }
  }
  validateUnions();
}

auto SemanticVisitor::Impl::validateStoredReferenceContracts() -> void {
  for (ClassInfo &owner : classes) {
    if (owner.kind == ClassKind::Union) {
      continue;
    }
    for (const FieldInfo &field : owner.fields) {
      if (field.declaration == nullptr) {
        continue;
      }
      const auto member = owner.members.find(field.declaration->name().lexeme);
      if (member == owner.members.end() ||
          member->second.symbol.type.kind != SemanticType::Reference) {
        continue;
      }

      const SemanticType &fieldType = member->second.symbol.type;
      if (field.declaration->initializer()) {
        report(field.declaration->name(),
               "A stored reference field must be bound explicitly by every "
               "constructor.",
               "GTI-S2045");
      }
      if (owner.storedReference) {
        continue;
      }
      owner.storedReference =
          StoredReferenceInfo{.field = field.declaration,
                              .type = fieldType,
                              .access = fieldType.referenceAccess};
    }
  }

  for (ClassInfo &owner : classes) {
    if (owner.kind == ClassKind::Union) {
      continue;
    }
    const auto dependencies =
        storedReferencePaths(SemanticType::classType(owner.id));
    if (dependencies.size() > 1) {
      if (owner.constructors.empty()) {
        report(owner.name,
               "A composed borrowed value requires a declared constructor "
               "that explicitly binds every borrowed field.",
               "GTI-S2045");
      }
      for (ConstructorInfo &constructor : owner.constructors) {
        const auto bindings = storedCarrierConstructorBindings(
            SemanticType::classType(owner.id), constructor.declaration,
            constructor.parameterTypes);
        for (std::size_t index = 0; index < bindings.size(); ++index) {
          for (std::size_t earlier = 0; earlier < index; ++earlier) {
            const auto &left = bindings[earlier];
            const auto &right = bindings[index];
            if (left.parameter == right.parameter &&
                left.sourceFields == right.sourceFields &&
                (left.destination.reference.referenceAccess ==
                     AccessMode::Mutable ||
                 right.destination.reference.referenceAccess ==
                     AccessMode::Mutable)) {
              report(right.destination.fields.front()->name(),
                     "Stored reference fields cannot duplicate an exclusive "
                     "dependency from the same constructor parameter.",
                     "GTI-S2045");
            }
          }
        }
        constructor.borrowParameters = storedCarrierConstructorParameters(
            SemanticType::classType(owner.id), constructor,
            constructor.parameterTypes);
        if (constructor.borrowParameters.empty()) {
          report(constructor.declaration->name(),
                 "Every constructor of a composed borrowed value must bind "
                 "each borrowed field directly from an exact reference or "
                 "carrier parameter.",
                 "GTI-S2045");
        } else {
          constructor.borrowParameter = constructor.borrowParameters.front();
          constructor.borrowAccess =
              dependencies.front().reference.referenceAccess;
        }
      }
      continue;
    }
    for (const ClassBaseTypeInfo &base : owner.bases) {
      if (!base.interface && typeTraits(base.type).containsBorrowedState) {
        report(base.syntax == nullptr ? owner.name
                                      : base.syntax->type.name.last(),
               "Borrowed state cannot be inherited in the current lifetime "
               "model; store one direct reference instead.",
               "GTI-S2045");
      }
    }
    for (const FieldInfo &field : owner.fields) {
      if (field.declaration == nullptr || field.declaration->type().reference) {
        continue;
      }
      const auto member = owner.members.find(field.declaration->name().lexeme);
      if (member != owner.members.end() &&
          typeTraits(member->second.symbol.type).containsBorrowedState &&
          !storedReferencePath(SemanticType::classType(owner.id))) {
        report(
            field.declaration->name(),
            "Borrowed field state must use checked stored-reference paths "
            "through ordinary nominal fields; fallible and other non-nominal "
            "carriers cannot be stored as fields.",
            "GTI-S2045");
      }
    }
    if (!owner.storedReference) {
      const SemanticType ownerType = SemanticType::classType(owner.id);
      const auto path = storedReferencePath(ownerType);
      if (path) {
        if (owner.constructors.empty()) {
          report(path->fields.front()->name(),
                 "A nested stored-reference field requires a declared "
                 "constructor that binds it from one exact carrier parameter.",
                 "GTI-S2045");
        }
        for (ConstructorInfo &constructor : owner.constructors) {
          if (constructor.declaration == nullptr) {
            continue;
          }
          constructor.borrowParameter = storedCarrierConstructorParameter(
              ownerType, constructor, constructor.parameterTypes);
          constructor.borrowAccess = path->reference.referenceAccess;
          if (!constructor.borrowParameter) {
            report(constructor.declaration->name(),
                   "Every constructor of a nested stored-reference class "
                   "must initialize its borrowed field directly from one "
                   "exact carrier parameter, optionally using std::move.",
                   "GTI-S2045");
          }
        }
      }
      continue;
    }
    const StoredReferenceInfo &stored = *owner.storedReference;
    if (owner.constructors.empty()) {
      report(stored.field->name(),
             "A stored reference field requires a declared constructor "
             "that binds it from a reference parameter.",
             "GTI-S2045");
      continue;
    }

    for (ConstructorInfo &constructor : owner.constructors) {
      if (constructor.declaration == nullptr) {
        continue;
      }
      const ConstructorInitializer *binding = nullptr;
      for (const ConstructorInitializer &initializer :
           constructor.declaration->initializers()) {
        if (initializer.target.name.segments.size() == 1 &&
            initializer.target.arguments.empty() &&
            initializer.target.name.last().lexeme ==
                stored.field->name().lexeme) {
          binding = &initializer;
          break;
        }
      }
      if (binding == nullptr || binding->arguments.size() != 1) {
        report(constructor.declaration->name(),
               "Every constructor of a stored-reference class must bind "
               "field '" +
                   stored.field->name().lexeme +
                   "' from exactly one reference parameter.",
               "GTI-S2045");
        continue;
      }
      const auto *source = exprAs<Variable>(binding->arguments.front().get());
      if (source == nullptr) {
        report(expressionToken(binding->arguments.front()),
               "Stored reference field '" + stored.field->name().lexeme +
                   "' must be bound directly from a constructor reference "
                   "parameter.",
               "GTI-S2045");
        continue;
      }
      const auto parameter =
          std::find_if(constructor.declaration->parameters().begin(),
                       constructor.declaration->parameters().end(),
                       [&](const Parameter &candidate) {
                         return candidate.name.lexeme == source->name().lexeme;
                       });
      if (parameter == constructor.declaration->parameters().end()) {
        report(source->name(),
               "Stored reference field '" + stored.field->name().lexeme +
                   "' must be bound from a constructor parameter.",
               "GTI-S2045");
        continue;
      }
      const std::size_t parameterIndex = static_cast<std::size_t>(std::distance(
          constructor.declaration->parameters().begin(), parameter));
      if (parameterIndex >= constructor.parameterTypes.size() ||
          constructor.parameterTypes[parameterIndex] != stored.type) {
        report(parameter->name,
               "Stored reference field '" + stored.field->name().lexeme +
                   "' requires a parameter of exact type '" +
                   typeSpelling(stored.type) + "'.",
               "GTI-S2045");
        continue;
      }
      constructor.borrowParameter = parameterIndex;
      constructor.borrowAccess = isDefaultLibraryMutexGuardCarrier(owner)
                                     ? AccessMode::Mutable
                                     : stored.access;
    }
  }
}

auto SemanticVisitor::Impl::indexFunctionCandidateBorrowSummaries(
    Symbol &symbol, BorrowSummaryCandidateIndex &index) -> void {
  for (CallableCandidate &candidate : symbol.overloads) {
    index[candidate.id].emplace_back(&candidate);
    if (workMetrics != nullptr) {
      ++workMetrics->semanticBorrowSummaryCandidateInspections;
    }
  }
}

auto SemanticVisitor::Impl::functionCandidateBorrowSummaryIndex()
    -> BorrowSummaryCandidateIndex {
  BorrowSummaryCandidateIndex index;
  for (auto &[_, symbol] : activeNamespaceSymbols()) {
    indexFunctionCandidateBorrowSummaries(symbol, index);
  }
  for (auto &[_, symbols] : activeVisibleNamespaceSymbols()) {
    for (auto &[__, symbol] : symbols) {
      indexFunctionCandidateBorrowSummaries(symbol, index);
    }
  }
  for (auto &[_, symbols] : activeInternalNamespaceSymbols()) {
    for (auto &[__, symbol] : symbols) {
      indexFunctionCandidateBorrowSummaries(symbol, index);
    }
  }
  for (ClassInfo &owner : classes) {
    for (auto &[_, member] : owner.members) {
      indexFunctionCandidateBorrowSummaries(member.symbol, index);
    }
  }
  return index;
}

auto SemanticVisitor::Impl::publishFunctionBorrowSummary(
    const FunctionInfo &function,
    const BorrowSummaryCandidateIndex &candidateIndex) -> void {
  if (function.declaration != nullptr) {
    if (CallableDeclaration *declaration =
            catalog.mutableCallable(*function.declaration)) {
      declaration->returnBorrowOrigin = function.returnBorrowOrigin;
      declaration->returnBorrowParameter = function.returnBorrowParameter;
      declaration->returnBorrowAccess = function.returnBorrowAccess;
      declaration->returnBorrowParameters = function.returnBorrowParameters;
      declaration->returnBorrowFields = function.returnBorrowFields;
      declaration->returnBorrowPlace = function.returnBorrowPlace;
    }
  }
  const auto found = candidateIndex.find(function.id);
  if (found == candidateIndex.end()) {
    return;
  }
  for (CallableCandidate *candidate : found->second) {
    if (workMetrics != nullptr) {
      ++workMetrics->semanticBorrowSummaryCandidateInspections;
    }
    candidate->returnBorrowOrigin = function.returnBorrowOrigin;
    candidate->returnBorrowParameter = function.returnBorrowParameter;
    candidate->returnBorrowAccess = function.returnBorrowAccess;
    candidate->returnBorrowParameters = function.returnBorrowParameters;
    candidate->returnBorrowFields = function.returnBorrowFields;
    candidate->returnBorrowPlace = function.returnBorrowPlace;
  }
}

auto SemanticVisitor::Impl::collectBorrowReturnExpressions(
    const Stmt *statement, std::vector<const ExprPtr *> &returns) const
    -> void {
  if (statement == nullptr) {
    return;
  }
  if (const auto *returned = stmtAs<ReturnStmt>(statement)) {
    returns.push_back(&returned->value());
    return;
  }
  if (const auto *block = stmtAs<BlockStmt>(statement)) {
    for (const StmtPtr &nested : block->statements()) {
      collectBorrowReturnExpressions(nested.get(), returns);
    }
    return;
  }
  if (const auto *conditional = stmtAs<ConditionalStmt>(statement)) {
    if (const StmtList *branch = conditional->activeBranch(target)) {
      for (const StmtPtr &nested : *branch) {
        collectBorrowReturnExpressions(nested.get(), returns);
      }
    }
    return;
  }
  if (const auto *branch = stmtAs<IfStmt>(statement)) {
    collectBorrowReturnExpressions(branch->thenBranch().get(), returns);
    collectBorrowReturnExpressions(branch->elseBranch().get(), returns);
    return;
  }
  if (const auto *selection = stmtAs<SwitchStmt>(statement)) {
    for (const SwitchArm &arm : selection->arms()) {
      for (const StmtPtr &nested : arm.statements) {
        collectBorrowReturnExpressions(nested.get(), returns);
      }
    }
    return;
  }
  if (const auto *loop = stmtAs<ForStmt>(statement)) {
    collectBorrowReturnExpressions(loop->body().get(), returns);
    return;
  }
  if (const auto *loop = stmtAs<RangeForStmt>(statement)) {
    collectBorrowReturnExpressions(loop->lowered().get(), returns);
    return;
  }
  if (const auto *loop = stmtAs<ConstexprForStmt>(statement)) {
    if (const ConstexprForExpansion *expansion =
            semanticModel.findConstexprForExpansion(*loop)) {
      for (const ConstexprForIteration &iteration : expansion->iterations) {
        collectBorrowReturnExpressions(iteration.block.get(), returns);
      }
    } else {
      collectBorrowReturnExpressions(loop->body().get(), returns);
    }
    return;
  }
  if (const auto *loop = stmtAs<DoWhileStmt>(statement)) {
    collectBorrowReturnExpressions(loop->body().get(), returns);
    return;
  }
  if (const auto *loop = stmtAs<WhileStmt>(statement)) {
    collectBorrowReturnExpressions(loop->body().get(), returns);
  }
}

auto SemanticVisitor::Impl::predeclaredTextBorrowSource(
    const ExprPtr &expression, const FunctionInfo &function)
    -> PredeclaredTextBorrowSource {
  if (!expression) {
    return {};
  }
  if (const auto *literal = exprAs<LiteralExpr>(expression.get())) {
    return {.known = literal->token().kind == TokenKind::STRING_LITERAL ||
                     literal->token().kind == TokenKind::NULLPTR};
  }
  if (exprAs<This>(expression.get()) != nullptr) {
    return {.known = true,
            .ownerDependent = true,
            .origin = BorrowOriginKind::Receiver};
  }
  if (const auto *variable = exprAs<Variable>(expression.get())) {
    if (function.declaration == nullptr) {
      return {};
    }
    for (std::size_t index = 0;
         index < function.declaration->parameters().size(); ++index) {
      if (function.declaration->parameters()[index].name.lexeme ==
          variable->name().lexeme) {
        return {.known = true,
                .ownerDependent = true,
                .origin = BorrowOriginKind::Argument,
                .parameter = index};
      }
    }
    if (function.ownerClass != 0 && !function.staticMember) {
      const MemberInfo *member = findMember(
          SemanticType::classType(function.ownerClass), variable->name());
      if (member != nullptr && member->symbol.variableDeclaration != nullptr &&
          !member->symbol.staticMember) {
        // Like explicit this.field, an implicit instance field can nominate
        // the receiver for the predeclared relay contract. Parameters take
        // precedence above. This is not a resolved body-local origin: body
        // analysis must still reject a shadowing local or an unrelated call
        // result through validateCurrentReturnBorrowSource.
        return {.known = true,
                .ownerDependent = true,
                .origin = BorrowOriginKind::Receiver};
      }
    }
    return {};
  }
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    return predeclaredTextBorrowSource(grouping->expression(), function);
  }
  if (const auto *binary = exprAs<Binary>(expression.get());
      binary != nullptr && binary->oper().kind == TokenKind::COMMA) {
    return predeclaredTextBorrowSource(binary->right(), function);
  }
  if (const auto *member = exprAs<Get>(expression.get())) {
    return predeclaredTextBorrowSource(member->object(), function);
  }
  if (const auto *index = exprAs<Index>(expression.get())) {
    // Like a field, an indexed receiver can nominate the enclosing source.
    // This is only the prospective relay contract: resolved body analysis
    // must still prove the selected access and returned view depend on it.
    return predeclaredTextBorrowSource(index->object(), function);
  }
  const auto *call = exprAs<Call>(expression.get());
  if (call == nullptr) {
    return {};
  }
  if (const auto *member = exprAs<Get>(call->callee().get())) {
    // Predeclare a receiver-rooted relay contract, not a proof that an
    // arbitrary member call borrows its receiver. Body analysis checks the
    // selected call's actual dependency against this contract before HIR.
    // This also handles nested owning fields without recognizing a wrapper.
    auto source = predeclaredTextBorrowSource(member->object(), function);
    if (source.origin == BorrowOriginKind::Argument) {
      if (source.parameter >= function.parameterTypes.size()) {
        return {};
      }
      const auto &parameter = function.parameterTypes[source.parameter];
      const bool externalOwner =
          parameter.kind == SemanticType::Reference &&
          parameter.referenceAccess == AccessMode::ReadOnly;
      if (!externalOwner && parameter.kind != SemanticType::StringView &&
          parameter.kind != SemanticType::CString) {
        return {};
      }
    }
    return source;
  }
  const std::optional<CallableCandidate> intrinsic =
      intrinsicCandidate(call->callee());
  if (intrinsic) {
    const auto kind = intrinsic->record().intrinsic;
    const std::size_t witness =
        kind == IntrinsicKind::TextViewFromAddress ? 2 : 1;
    if ((kind == IntrinsicKind::TextViewFromAddress ||
         kind == IntrinsicKind::CStringFromAddress) &&
        call->arguments().size() == witness + 1)
      return predeclaredTextBorrowSource(call->arguments()[witness], function);
  }
  if (!intrinsic) {
    // Follow an exact ordinary helper contract, never its public spelling.
    // The body-derived query is cycle guarded and works before declaration
    // iteration happens to publish the callee's summary. This is a prospective
    // origin only; resolved body analysis still validates the actual return.
    const auto symbol = resolveExpressionSymbol(call->callee());
    if (symbol && symbol->overloads.size() == 1) {
      const auto &candidate = symbol->overloads.front();
      const auto *target =
          candidate.declaration
              ? semanticModel.findFunction(*candidate.declaration)
              : nullptr;
      const auto summary =
          target ? dynamicTextReturnBorrowSummary(*target) : std::nullopt;
      if (summary && summary->origin == BorrowOriginKind::Argument &&
          summary->parameters.empty() &&
          summary->parameter < call->arguments().size())
        return predeclaredTextBorrowSource(
            call->arguments()[summary->parameter], function);
    }
  }
  if (!intrinsic || call->arguments().size() != 1 ||
      intrinsic->record().intrinsic != IntrinsicKind::Move) {
    return {};
  }
  return predeclaredTextBorrowSource(call->arguments().front(), function);
}

auto SemanticVisitor::Impl::dynamicTextReturnBorrowSummary(
    const FunctionInfo &function)
    -> std::optional<FunctionReturnBorrowSummary> {
  if ((function.returnType.kind != SemanticType::StringView &&
       function.returnType.kind != SemanticType::CString) ||
      function.intrinsic != IntrinsicKind::None ||
      function.declaration == nullptr ||
      function.declaration->body() == nullptr) {
    return std::nullopt;
  }
  if (!activeTextBorrowSummaries.insert(function.declaration).second)
    return std::nullopt;
  struct ActiveSummary {
    std::unordered_set<const FunctionDecl *> &active;
    const FunctionDecl *declaration;
    ~ActiveSummary() { active.erase(declaration); }
  } active{activeTextBorrowSummaries, function.declaration};

  std::vector<const ExprPtr *> returns;
  collectBorrowReturnExpressions(function.declaration->body().get(), returns);
  if (returns.empty()) {
    return std::nullopt;
  }

  const std::vector<std::string> enclosingNamespace = currentNamespace;
  const SourceUnitId enclosingSourceUnit = currentSourceUnit;
  const std::optional<ClassId> enclosingClass = currentClass;
  currentNamespace = function.namespaceScope;
  currentSourceUnit = function.sourceUnit;
  currentClass = function.ownerClass == 0
                     ? std::nullopt
                     : std::optional<ClassId>{function.ownerClass};

  std::optional<FunctionReturnBorrowSummary> result;
  bool valid = true;
  for (const ExprPtr *returned : returns) {
    const PredeclaredTextBorrowSource source =
        returned == nullptr ? PredeclaredTextBorrowSource{}
                            : predeclaredTextBorrowSource(*returned, function);
    if (!source.known) {
      valid = false;
      break;
    }
    if (!source.ownerDependent) {
      continue;
    }
    FunctionReturnBorrowSummary candidate{.origin = source.origin,
                                          .parameter = source.parameter,
                                          .access = AccessMode::ReadOnly};
    if (result && (result->origin != candidate.origin ||
                   result->parameter != candidate.parameter)) {
      valid = false;
      break;
    }
    result = candidate;
  }

  currentNamespace = enclosingNamespace;
  currentSourceUnit = enclosingSourceUnit;
  currentClass = enclosingClass;
  return valid ? result : std::nullopt;
}

auto SemanticVisitor::Impl::globalReturnBorrowPlace(
    const FunctionInfo &function) -> std::optional<BorrowOriginPlace> {
  if (function.declaration == nullptr ||
      function.declaration->body() == nullptr ||
      (function.ownerClass != 0 && !function.staticMember)) {
    return std::nullopt;
  }

  std::vector<const ExprPtr *> returns;
  collectBorrowReturnExpressions(function.declaration->body().get(), returns);
  if (returns.empty()) {
    return std::nullopt;
  }

  const std::vector<std::string> enclosingNamespace = currentNamespace;
  const SourceUnitId enclosingSourceUnit = currentSourceUnit;
  const std::optional<ClassId> enclosingClass = currentClass;
  currentNamespace = function.namespaceScope;
  currentSourceUnit = function.sourceUnit;
  currentClass = function.ownerClass == 0
                     ? std::nullopt
                     : std::optional<ClassId>{function.ownerClass};

  std::optional<BorrowOriginPlace> result;
  bool valid = true;
  for (const ExprPtr *returned : returns) {
    if (returned == nullptr || !*returned) {
      valid = false;
      break;
    }
    const auto unexpectedReturn = [&]() {
      const ExprPtr *candidate = returned;
      while (candidate != nullptr && *candidate) {
        const auto *grouping = exprAs<Grouping>(candidate->get());
        if (grouping == nullptr) {
          break;
        }
        candidate = &grouping->expression();
      }
      return candidate != nullptr && *candidate &&
             exprAs<Unexpected>(candidate->get()) != nullptr;
    }();
    if (isFallibleBorrowType(function.returnType) && unexpectedReturn) {
      continue;
    }
    SemanticPlace place = semanticPlace(*returned);
    std::optional<BorrowOriginPlace> candidate;
    if (place.root != nullptr && !place.receiver &&
        (place.root->bindingKind == SemanticBindingKind::GlobalVariable ||
         place.root->bindingKind == SemanticBindingKind::StaticField)) {
      candidate = BorrowOriginPlace{.root = place.rootIdentity == 0
                                                ? toolingSymbolFor(*place.root)
                                                : place.rootIdentity,
                                    .projections = place.projections};
    } else {
      candidate = predeclaredGlobalBorrowPlace(*returned);
    }
    if (!candidate) {
      valid = false;
      break;
    }
    if (!candidate->valid() || (result && *result != *candidate)) {
      valid = false;
      break;
    }
    result = std::move(candidate);
  }

  currentNamespace = enclosingNamespace;
  currentSourceUnit = enclosingSourceUnit;
  currentClass = enclosingClass;
  return valid && result ? result : std::nullopt;
}

auto SemanticVisitor::Impl::ownedPointeeReturnProjection(
    const FunctionInfo &function) -> std::optional<OwnedPointeeProjection> {
  // Deliberately bounded to a transparent accessor. Merely returning a
  // receiver-derived reference, or performing a borrow somewhere in a larger
  // body, does not establish the identity of uniquely owned storage.
  if (function.ownerClass == 0 || function.staticMember ||
      function.virtualMethod || function.pureVirtual ||
      function.overrideMethod || function.intrinsic != IntrinsicKind::None ||
      !function.parameterTypes.empty() || function.parameterPack ||
      function.declaration == nullptr || !function.declaration->body() ||
      function.declaration->body()->statements().size() != 1 ||
      function.returnType.kind != SemanticType::Reference ||
      function.returnType.arguments.size() != 1) {
    return std::nullopt;
  }
  const auto *returned = stmtAs<ReturnStmt>(
      function.declaration->body()->statements().front().get());
  const auto *call =
      returned == nullptr ? nullptr : exprAs<Call>(returned->value().get());
  if (call == nullptr || call->arguments().size() != 1 ||
      !call->typeArguments().empty()) {
    return std::nullopt;
  }
  const auto *field = exprAs<Get>(call->arguments().front().get());
  if (field == nullptr || field->access().kind != TokenKind::DOT ||
      exprAs<This>(field->object().get()) == nullptr) {
    return std::nullopt;
  }
  const auto &owner = classInfo(function.ownerClass);
  const auto member = owner.members.find(field->name().lexeme);
  if (member == owner.members.end() ||
      member->second.symbol.variableDeclaration == nullptr ||
      member->second.symbol.bindingKind != SemanticBindingKind::Field ||
      member->second.symbol.ownerClass != function.ownerClass) {
    return std::nullopt;
  }
  const Symbol &storage = member->second.symbol;
  const SemanticType storageType =
      substituteType(storage.type, instanceTypeSubstitution);
  if (storageType.kind != SemanticType::UniqueOwner ||
      storageType.arguments.size() != 1 ||
      storageType.arguments.front() != function.returnType.arguments.front()) {
    return std::nullopt;
  }
  const bool mutableBorrow =
      function.returnType.referenceAccess == AccessMode::Mutable;
  if (mutableBorrow &&
      (!storage.assignable || function.declaration->receiverMutability() !=
                                  ReceiverMutability::Mutable)) {
    return std::nullopt;
  }
  const auto enclosingNamespace = currentNamespace;
  const auto enclosingSourceUnit = currentSourceUnit;
  const auto enclosingClass = currentClass;
  currentNamespace = function.namespaceScope;
  currentSourceUnit = function.sourceUnit;
  currentClass = function.ownerClass;
  const auto intrinsic = intrinsicCandidate(call->callee());
  currentNamespace = enclosingNamespace;
  currentSourceUnit = enclosingSourceUnit;
  currentClass = enclosingClass;
  const auto expected = mutableBorrow ? IntrinsicKind::UniqueOwnerBorrowMut
                                      : IntrinsicKind::UniqueOwnerBorrow;
  if (!intrinsic || intrinsic->record().intrinsic != expected) {
    return std::nullopt;
  }
  return OwnedPointeeProjection{.fields = {toolingSymbolFor(storage)},
                                .borrowIntrinsic = expected};
}

auto SemanticVisitor::Impl::storedFieldReturnBorrowSummary(
    const FunctionInfo &function, SemanticType ownerType)
    -> std::optional<FunctionReturnBorrowSummary> {
  if (!isBorrowReturningType(function.returnType) ||
      isBorrowedCarrierReference(function.returnType) ||
      function.intrinsic != IntrinsicKind::None || function.virtualMethod ||
      function.overrideMethod || function.declaration == nullptr ||
      function.declaration->body() == nullptr)
    return std::nullopt;
  // Even a single stored reference needs an exact field origin: a carrier can
  // also own unrelated storage, and returning that storage must protect its
  // shell rather than being mistaken for the carrier's external dependency.
  if (function.ownerClass != 0 && (ownerType.kind != SemanticType::Class ||
                                   ownerType.classId != function.ownerClass))
    ownerType = substituteType(openClassType(function.ownerClass),
                               instanceTypeSubstitution);
  const auto enclosingNamespace = currentNamespace;
  const auto enclosingSourceUnit = currentSourceUnit;
  const auto enclosingClass = currentClass;
  currentNamespace = function.namespaceScope;
  currentSourceUnit = function.sourceUnit;
  currentClass = function.ownerClass == 0
                     ? std::nullopt
                     : std::optional<ClassId>{function.ownerClass};

  struct Origin {
    BorrowedFieldOrigin source;
    SemanticType type = SemanticType::Unknown;
  };
  const auto field = [&](Origin source,
                         const Token &name) -> std::optional<Origin> {
    SemanticType owner = source.type;
    if (owner.kind == SemanticType::Reference && owner.arguments.size() == 1)
      owner = SemanticType(owner.arguments.front());
    const auto *member = findMember(owner, name);
    if (member == nullptr || member->symbol.variableDeclaration == nullptr ||
        member->symbol.staticMember)
      return std::nullopt;
    source.source.sourceFields.push_back(toolingSymbolFor(member->symbol));
    source.type = substituteType(member->symbol.type, classSubstitution(owner));
    return source;
  };
  std::vector<FunctionId> activeOrigins{function.id};
  const auto origin =
      [&](const auto &self, const ExprPtr &expression,
          const FunctionInfo &context,
          const SemanticType &contextOwner) -> std::optional<Origin> {
    if (!expression)
      return std::nullopt;
    if (const auto *group = exprAs<Grouping>(expression.get()))
      return self(self, group->expression(), context, contextOwner);
    if (exprAs<This>(expression.get()) != nullptr && context.ownerClass != 0)
      return Origin{.source = {.origin = BorrowOriginKind::Receiver},
                    .type = contextOwner};
    if (const auto *variable = exprAs<Variable>(expression.get())) {
      for (std::size_t index = 0; index < context.parameterTypes.size();
           ++index)
        if (context.declaration->parameters()[index].name.lexeme ==
            variable->name().lexeme)
          return Origin{.source = {.origin = BorrowOriginKind::Argument,
                                   .parameter = index},
                        .type = context.parameterTypes[index]};
      if (context.ownerClass != 0 && !context.staticMember)
        return field({.source = {.origin = BorrowOriginKind::Receiver},
                      .type = contextOwner},
                     variable->name());
    }
    if (const auto *member = exprAs<Get>(expression.get()))
      if (auto object = self(self, member->object(), context, contextOwner))
        return field(std::move(*object), member->name());
    if (const auto *call = exprAs<Call>(expression.get())) {
      const auto contextIntrinsic = [&] {
        const auto savedNamespace = currentNamespace;
        const auto savedSourceUnit = currentSourceUnit;
        const auto savedClass = currentClass;
        currentNamespace = context.namespaceScope;
        currentSourceUnit = context.sourceUnit;
        currentClass = context.ownerClass == 0
                           ? std::nullopt
                           : std::optional<ClassId>{context.ownerClass};
        auto result = intrinsicCandidate(call->callee());
        currentNamespace = savedNamespace;
        currentSourceUnit = savedSourceUnit;
        currentClass = savedClass;
        return result;
      };
      if (const auto intrinsic = contextIntrinsic();
          intrinsic && !call->arguments().empty()) {
        // These trusted storage accessors borrow the first argument, not the
        // wrapper containing that argument. Preserve a stored-reference path
        // through the accessor so an element return is not confused with a
        // reference into the wrapper's own physical storage. The normal call
        // analysis and return validation still check types and permission.
        switch (intrinsic->record().intrinsic) {
        case IntrinsicKind::Move:
        case IntrinsicKind::UniqueOwnerBorrow:
        case IntrinsicKind::UniqueOwnerBorrowMut:
        case IntrinsicKind::OptionalStorageRead:
        case IntrinsicKind::OptionalStorageReadMut:
        case IntrinsicKind::PolymorphicStorageRead:
        case IntrinsicKind::PolymorphicStorageReadMut:
          return self(self, call->arguments().front(), context, contextOwner);
        default:
          break;
        }
      }
      // Compose an unambiguous source-defined accessor's checked field path.
      // This runs before expression overload resolution, so ambiguous calls,
      // dynamic dispatch, and recursive provenance remain conservative. The
      // selected function's normal body analysis independently validates the
      // returned origin; syntax here only proposes the declaration contract.
      const auto *member = exprAs<Get>(call->callee().get());
      auto receiver = member == nullptr
                          ? std::optional<Origin>{}
                          : self(self, member->object(), context, contextOwner);
      if (!receiver)
        return std::nullopt;
      SemanticType receiverType = receiver->type;
      if (receiverType.kind == SemanticType::Reference &&
          receiverType.arguments.size() == 1)
        receiverType = SemanticType(receiverType.arguments.front());
      const auto *selected = findMember(receiverType, member->name());
      if (selected == nullptr || selected->symbol.overloads.size() != 1)
        return std::nullopt;
      const auto &candidate = selected->symbol.overloads.front();
      const auto *target =
          candidate.declaration == nullptr
              ? nullptr
              : semanticModel.findFunction(*candidate.declaration);
      if (target == nullptr || target->staticMember || target->virtualMethod ||
          target->overrideMethod || target->intrinsic != IntrinsicKind::None ||
          target->returnType.kind != SemanticType::Reference ||
          target->declaration->body() == nullptr ||
          target->parameterTypes.size() != call->arguments().size() ||
          std::find(activeOrigins.begin(), activeOrigins.end(), target->id) !=
              activeOrigins.end())
        return std::nullopt;
      activeOrigins.push_back(target->id);
      std::vector<const ExprPtr *> returns;
      collectBorrowReturnExpressions(target->declaration->body().get(),
                                     returns);
      std::optional<Origin> result;
      for (const auto *returned : returns) {
        auto nested = self(self, *returned, *target, receiverType);
        std::optional<Origin> base;
        if (nested && nested->source.origin == BorrowOriginKind::Receiver) {
          base = receiver;
          // A call exposes its receiver as the owner of inline fields or
          // owned allocations, not the callee's private storage path. Only
          // a path crossing one of its stored references is an external
          // field-origin contract that callers may compose further.
          const auto paths = storedReferencePaths(receiverType);
          const bool external =
              std::any_of(paths.begin(), paths.end(), [&](const auto &path) {
                const auto fields =
                    storedFieldSymbols(receiverType, path.fields);
                return nested->source.sourceFields.size() >= fields.size() &&
                       std::equal(fields.begin(), fields.end(),
                                  nested->source.sourceFields.begin());
              });
          if (!external)
            nested->source.sourceFields.clear();
        } else if (nested &&
                   nested->source.origin == BorrowOriginKind::Argument &&
                   nested->source.parameter < call->arguments().size())
          base = self(self, call->arguments()[nested->source.parameter],
                      context, contextOwner);
        if (!base) {
          result.reset();
          break;
        }
        base->source.sourceFields.insert(base->source.sourceFields.end(),
                                         nested->source.sourceFields.begin(),
                                         nested->source.sourceFields.end());
        base->type =
            substituteType(target->returnType, classSubstitution(receiverType));
        if (result &&
            (result->source != base->source || result->type != base->type)) {
          result.reset();
          break;
        }
        result = std::move(base);
      }
      activeOrigins.pop_back();
      return result;
    }
    return std::nullopt;
  };
  const auto returnFields = [&](const auto &self, const ExprPtr &expression,
                                const FunctionInfo &context,
                                const SemanticType &contextOwner)
      -> std::optional<std::vector<BorrowedFieldOrigin>> {
    if (!expression)
      return std::nullopt;
    if (const auto *group = exprAs<Grouping>(expression.get()))
      return self(self, group->expression(), context, contextOwner);
    const bool singleValuePath =
        context.returnType.kind != SemanticType::Reference &&
        storedReferencePaths(context.returnType).size() <= 1;
    std::vector<BorrowedFieldOrigin> fields;
    if (singleValuePath && context.returnType.kind == SemanticType::Class) {
      const auto *call = exprAs<Call>(expression.get());
      const auto *member =
          call == nullptr ? nullptr : exprAs<Get>(call->callee().get());
      auto receiver = member == nullptr ? std::optional<Origin>{}
                                        : origin(origin, member->object(),
                                                 context, contextOwner);
      if (receiver) {
        auto receiverType = receiver->type;
        if (receiverType.kind == SemanticType::Reference &&
            receiverType.arguments.size() == 1)
          receiverType = SemanticType(receiverType.arguments.front());
        const auto *selected = findMember(receiverType, member->name());
        const auto *target =
            selected != nullptr && selected->symbol.overloads.size() == 1 &&
                    selected->symbol.overloads.front().declaration != nullptr
                ? semanticModel.findFunction(
                      *selected->symbol.overloads.front().declaration)
                : nullptr;
        const auto resultPaths = storedReferencePaths(context.returnType);
        auto sourceOwner =
            receiver->source.origin == BorrowOriginKind::Receiver
                ? contextOwner
                : context.parameterTypes[receiver->source.parameter];
        if (sourceOwner.kind == SemanticType::Reference &&
            sourceOwner.arguments.size() == 1)
          sourceOwner = SemanticType(sourceOwner.arguments.front());
        const auto sourcePaths = storedReferencePaths(sourceOwner);
        const bool externalReceiver = std::any_of(
            sourcePaths.begin(), sourcePaths.end(), [&](const auto &path) {
              const auto sourceFields =
                  storedFieldSymbols(sourceOwner, path.fields);
              return receiver->source.sourceFields.size() >=
                         sourceFields.size() &&
                     std::equal(sourceFields.begin(), sourceFields.end(),
                                receiver->source.sourceFields.begin());
            });
        if (target != nullptr && !target->staticMember &&
            !target->virtualMethod && !target->overrideMethod &&
            target->intrinsic == IntrinsicKind::None &&
            target->declaration != nullptr &&
            target->declaration->body() != nullptr &&
            target->returnBorrowOrigin == BorrowOriginKind::Receiver &&
            target->returnBorrowParameters.empty() &&
            target->returnBorrowFields.empty() &&
            target->parameterTypes.size() == call->arguments().size() &&
            substituteType(target->returnType,
                           classSubstitution(receiverType)) ==
                context.returnType &&
            resultPaths.size() == 1 && externalReceiver) {
          // Relaying an ordinary owner-tied adapter through a stored reference
          // keeps that external owner, not this wrapper's shell. The selected
          // call and the return body independently validate this proposed map.
          auto mapping = receiver->source;
          mapping.resultFields = storedFieldSymbols(context.returnType,
                                                    resultPaths.front().fields);
          mapping.access = resultPaths.front().reference.referenceAccess;
          return std::vector<BorrowedFieldOrigin>{std::move(mapping)};
        }
      }
    }
    if (auto source = origin(origin, expression, context, contextOwner)) {
      if (context.returnType.kind == SemanticType::Reference) {
        // Ordinary owner/field references already use the projected-place
        // return contract. Field mappings describe stored external referents,
        // not a second interpretation of owned storage or raw indirection.
        SemanticType owner =
            source->source.origin == BorrowOriginKind::Receiver
                ? contextOwner
                : context.parameterTypes[source->source.parameter];
        if (owner.kind == SemanticType::Reference &&
            owner.arguments.size() == 1)
          owner = SemanticType(owner.arguments.front());
        const auto paths = storedReferencePaths(owner);
        if (std::none_of(paths.begin(), paths.end(), [&](const auto &path) {
              const auto fields = storedFieldSymbols(owner, path.fields);
              return source->source.sourceFields.size() >= fields.size() &&
                     std::equal(fields.begin(), fields.end(),
                                source->source.sourceFields.begin());
            }))
          return std::nullopt;
        source->source.access = context.returnType.referenceAccess;
        fields.push_back(std::move(source->source));
        return fields;
      }
      SemanticType type = source->type;
      if (type.kind == SemanticType::Reference && type.arguments.size() == 1)
        type = SemanticType(type.arguments.front());
      if (type != context.returnType)
        return std::nullopt;
      // Whole single-path carriers retain the compact contract. A copied
      // nested carrier instead needs its field path so callers retain the
      // external referent rather than the enclosing physical object.
      if (singleValuePath && source->source.sourceFields.empty())
        return std::nullopt;
      for (const auto &path : storedReferencePaths(type)) {
        auto mapping = source->source;
        mapping.resultFields = storedFieldSymbols(type, path.fields);
        mapping.sourceFields.insert(mapping.sourceFields.end(),
                                    mapping.resultFields.begin(),
                                    mapping.resultFields.end());
        mapping.access = path.reference.referenceAccess;
        fields.push_back(std::move(mapping));
      }
      return fields.empty() ? std::nullopt : std::optional{fields};
    }
    const auto *call = exprAs<Call>(expression.get());
    const auto *initializer = exprAs<DirectInitializer>(expression.get());
    SemanticType constructed = SemanticType::Unknown;
    std::optional<ClassId> constructedClass;
    const Symbol *callee = nullptr;
    std::optional<Origin> receiver;
    if (call != nullptr) {
      const auto savedNamespace = currentNamespace;
      const auto savedSourceUnit = currentSourceUnit;
      const auto savedClass = currentClass;
      currentNamespace = context.namespaceScope;
      currentSourceUnit = context.sourceUnit;
      currentClass = context.ownerClass == 0
                         ? std::nullopt
                         : std::optional<ClassId>{context.ownerClass};
      if (const auto *name = exprAs<Variable>(call->callee().get())) {
        constructed = typeOf(TypeRef(name->name(), call->typeArguments()));
        constructedClass =
            resolveClassPath(NamePath(name->name()), currentNamespace);
        callee = resolve(name->name());
      } else if (const auto *name =
                     exprAs<QualifiedName>(call->callee().get())) {
        constructed = typeOf(TypeRef(name->name(), call->typeArguments()));
        constructedClass = resolveClassPath(name->name(), currentNamespace);
        callee = resolveQualified(name->name());
      } else if (const auto *member = exprAs<Get>(call->callee().get())) {
        receiver = origin(origin, member->object(), context, contextOwner);
        if (receiver) {
          auto receiverType = receiver->type;
          if (receiverType.kind == SemanticType::Reference &&
              receiverType.arguments.size() == 1)
            receiverType = SemanticType(receiverType.arguments.front());
          if (const auto *selected = findMember(receiverType, member->name()))
            callee = &selected->symbol;
        }
      }
      currentNamespace = savedNamespace;
      currentSourceUnit = savedSourceUnit;
      currentClass = savedClass;
    } else if (initializer != nullptr && initializer->designators().empty()) {
      constructed = context.returnType;
      constructedClass = context.returnType.classId;
    }

    // Before body resolution a declaration summary is only a proposal. Follow
    // an unambiguous exact-type factory's return paths and rebase every field
    // onto its actual argument/receiver origin; normal call/return validation
    // still checks selection, permission, and escape. Do not collapse a mixed
    // or multi-owner carrier to the first eligible parameter.
    if (context.returnType.kind == SemanticType::Class && call != nullptr &&
        callee != nullptr && callee->overloads.size() == 1 &&
        call->typeArguments().empty()) {
      const auto &candidate = callee->overloads.front();
      const auto *target =
          candidate.declaration == nullptr
              ? nullptr
              : semanticModel.findFunction(*candidate.declaration);
      if (target != nullptr && target->declaration->body() != nullptr &&
          target->intrinsic == IntrinsicKind::None && !target->virtualMethod &&
          !target->overrideMethod && !candidate.parameterPack &&
          target->parameterTypes.size() == call->arguments().size() &&
          std::find(activeOrigins.begin(), activeOrigins.end(), target->id) ==
              activeOrigins.end()) {
        FunctionInfo concrete = *target;
        SemanticType targetOwner = SemanticType::Unknown;
        if (receiver) {
          targetOwner = receiver->type;
          if (targetOwner.kind == SemanticType::Reference &&
              targetOwner.arguments.size() == 1)
            targetOwner = SemanticType(targetOwner.arguments.front());
          const auto substitution = classSubstitution(targetOwner);
          concrete.returnType =
              substituteType(concrete.returnType, substitution);
          for (auto &parameter : concrete.parameterTypes)
            parameter = substituteType(parameter, substitution);
        }
        if (concrete.returnType == context.returnType) {
          activeOrigins.push_back(target->id);
          std::vector<const ExprPtr *> returns;
          collectBorrowReturnExpressions(target->declaration->body().get(),
                                         returns);
          std::optional<std::vector<BorrowedFieldOrigin>> result;
          for (const auto *returned : returns) {
            auto nested = self(self, *returned, concrete, targetOwner);
            if (!nested) {
              result.reset();
              break;
            }
            fields.clear();
            for (const auto &mapping : *nested) {
              auto source =
                  mapping.origin == BorrowOriginKind::Receiver ? receiver
                  : mapping.origin == BorrowOriginKind::Argument &&
                          mapping.parameter < call->arguments().size()
                      ? origin(origin, call->arguments()[mapping.parameter],
                               context, contextOwner)
                      : std::optional<Origin>{};
              if (!source) {
                fields.clear();
                break;
              }
              auto rebased = std::move(source->source);
              rebased.resultFields = mapping.resultFields;
              rebased.sourceFields.insert(rebased.sourceFields.end(),
                                          mapping.sourceFields.begin(),
                                          mapping.sourceFields.end());
              rebased.access = mapping.access;
              fields.push_back(std::move(rebased));
            }
            if (fields.empty() || (result && *result != fields)) {
              result.reset();
              break;
            }
            result = fields;
          }
          activeOrigins.pop_back();
          if (result)
            return result;
        }
      }
    }

    if (!constructedClass || *constructedClass != context.returnType.classId)
      return std::nullopt;
    const auto &arguments =
        call != nullptr ? call->arguments() : initializer->arguments();
    if (constructed == context.returnType && arguments.size() == 1)
      if (auto source = self(self, arguments.front(), context, contextOwner))
        return source;
    // Ordinary constructor summaries already use the declared result's class
    // substitution. Preserve that path when a generic primary's written type
    // arguments are not yet in scope; only transparent wrapper inference above
    // requires a fully resolved exact type here. Body resolution validates the
    // selected constructor and all parameter types independently.
    const auto *owner = classInfo(context.returnType);
    if (owner == nullptr)
      return std::nullopt;
    std::optional<std::vector<BorrowedFieldOrigin>> result;
    for (const auto &constructor : owner->constructors) {
      if (constructor.parameterTypes.size() != arguments.size())
        continue;
      auto parameters = constructor.parameterTypes;
      for (auto &parameter : parameters)
        parameter =
            substituteType(parameter, classSubstitution(context.returnType));
      const auto bindings = storedCarrierConstructorBindings(
          context.returnType, constructor.declaration, parameters);
      if (bindings.empty())
        continue;
      fields.clear();
      for (const auto &binding : bindings) {
        auto source =
            origin(origin, arguments[binding.parameter], context, contextOwner);
        if (!source) {
          fields.clear();
          break;
        }
        auto mapping = std::move(source->source);
        mapping.resultFields =
            storedFieldSymbols(context.returnType, binding.destination.fields);
        auto nested = storedFieldSymbols(parameters[binding.parameter],
                                         binding.sourceFields);
        mapping.sourceFields.insert(mapping.sourceFields.end(), nested.begin(),
                                    nested.end());
        if (singleValuePath) {
          // A freshly constructed single-path adapter normally retains the
          // compact owner contract. When its argument crosses a stored
          // reference, however, its referent is external to that owner: the
          // source field map must survive construction just as it does a
          // direct reference return.
          auto sourceOwner = mapping.origin == BorrowOriginKind::Receiver
                                 ? contextOwner
                                 : context.parameterTypes[mapping.parameter];
          if (sourceOwner.kind == SemanticType::Reference &&
              sourceOwner.arguments.size() == 1)
            sourceOwner = SemanticType(sourceOwner.arguments.front());
          const auto paths = storedReferencePaths(sourceOwner);
          if (std::none_of(paths.begin(), paths.end(), [&](const auto &path) {
                const auto sourceFields =
                    storedFieldSymbols(sourceOwner, path.fields);
                return mapping.sourceFields.size() >= sourceFields.size() &&
                       std::equal(sourceFields.begin(), sourceFields.end(),
                                  mapping.sourceFields.begin());
              }))
            return std::nullopt;
        }
        mapping.access = binding.destination.reference.referenceAccess;
        fields.push_back(std::move(mapping));
      }
      if (fields.empty() || (result && *result != fields))
        return std::nullopt;
      result = fields;
    }
    return result;
  };
  std::vector<const ExprPtr *> returns;
  collectBorrowReturnExpressions(function.declaration->body().get(), returns);
  std::optional<std::vector<BorrowedFieldOrigin>> fields;
  for (const auto *expression : returns) {
    const auto candidate =
        returnFields(returnFields, *expression, function, ownerType);
    if (!candidate || (fields && *fields != *candidate)) {
      fields.reset();
      break;
    }
    fields = candidate;
  }
  currentNamespace = enclosingNamespace;
  currentSourceUnit = enclosingSourceUnit;
  currentClass = enclosingClass;
  if (!fields || fields->empty())
    return std::nullopt;
  FunctionReturnBorrowSummary result{.origin = fields->front().origin,
                                     .parameter = fields->front().parameter,
                                     .access = fields->front().access,
                                     .fields = *fields};
  for (const auto &field : *fields) {
    if (field.origin != result.origin)
      return std::nullopt;
    if (field.origin == BorrowOriginKind::Argument &&
        (field.parameter >= function.parameterTypes.size() ||
         (function.parameterTypes[field.parameter].kind !=
              SemanticType::Reference &&
          !isDirectStoredReferenceType(
              function.parameterTypes[field.parameter]))))
      return std::nullopt;
    if (field.origin == BorrowOriginKind::Argument &&
        std::find(result.parameters.begin(), result.parameters.end(),
                  field.parameter) == result.parameters.end())
      result.parameters.push_back(field.parameter);
  }
  if (result.parameters.size() == 1)
    result.parameters.clear();
  return result;
}

auto SemanticVisitor::Impl::resolveFunctionBorrowSummaries() -> void {
  BorrowSummaryCandidateIndex candidateIndex =
      functionCandidateBorrowSummaryIndex();
  for (const auto &[declaration, _] : functionGenericParameters) {
    if (declaration == nullptr) {
      continue;
    }
    const FunctionInfo *existing = semanticModel.findFunction(*declaration);
    if (existing == nullptr) {
      continue;
    }
    if (workMetrics != nullptr) {
      ++workMetrics->semanticBorrowSummaryFunctions;
    }
    FunctionInfo updated = *existing;
    if (updated.explicitReturnBorrowOrigin) {
      // A declaration contract is authoritative even when invalid (None).
      // Body-based inference must neither overwrite it nor rescue a typo.
      updated.ownedPointeeProjection = ownedPointeeReturnProjection(updated);
      semanticModel.record(*declaration, updated);
      publishFunctionBorrowSummary(updated, candidateIndex);
      continue;
    }
    FunctionReturnBorrowSummary summary = functionReturnBorrowSummary(
        updated.returnType, updated.parameterTypes, updated.ownerClass,
        updated.staticMember, updated.intrinsic);
    if (const std::optional<FunctionReturnBorrowSummary> dynamicText =
            dynamicTextReturnBorrowSummary(updated)) {
      summary = *dynamicText;
    }
    if (const auto fields = storedFieldReturnBorrowSummary(updated))
      summary = *fields;
    if (isBorrowReturningType(updated.returnType) &&
        updated.intrinsic == IntrinsicKind::None) {
      if (std::optional<BorrowOriginPlace> place =
              globalReturnBorrowPlace(updated)) {
        summary.origin = BorrowOriginKind::Global;
        summary.parameter = 0;
        summary.place = std::move(place);
      }
    }
    updated.returnBorrowOrigin = summary.origin;
    updated.returnBorrowParameter = summary.parameter;
    updated.returnBorrowAccess = summary.access;
    updated.returnBorrowParameters = summary.parameters;
    updated.returnBorrowFields = summary.fields;
    updated.returnBorrowPlace = summary.place;
    updated.ownedPointeeProjection = ownedPointeeReturnProjection(updated);
    semanticModel.record(*declaration, updated);
    publishFunctionBorrowSummary(updated, candidateIndex);
  }
}

auto SemanticVisitor::Impl::registerDesignatedConstructors() -> void {
  designatedMoves.clear();
  for (ClassInfo &owner : classes) {
    if (owner.kind != ClassKind::Struct || owner.compilerPrivate ||
        owner.cAbiRecord || owner.cOpaqueHandle || !owner.bases.empty() ||
        !owner.constructors.empty() || owner.copyConstructor ||
        owner.moveConstructor || owner.fields.empty()) {
      continue;
    }
    bool eligible = true;
    for (const FieldInfo &entry : owner.fields) {
      const auto member = owner.members.find(entry.declaration->name().lexeme);
      if (member == owner.members.end() ||
          member->second.access != AccessModifier::Public ||
          member->second.symbol.type.kind == SemanticType::Reference ||
          member->second.symbol.type.kind == SemanticType::Array ||
          typeTraits(member->second.symbol.type).containsBorrowedState) {
        eligible = false;
        break;
      }
    }
    if (!eligible) {
      continue;
    }
    // One generated constructor per parameter order: declaration order for
    // designated construction, reverse order for std::meta::construct. Field
    // initializers stay in declaration order; only the parameter list moves.
    const auto generate = [&](bool reversed) {
      std::vector<Parameter> parameters;
      std::vector<ConstructorInitializer> initializers;
      std::vector<SemanticType> types;
      std::vector<const Call *> transfers;
      const auto describe = [&](const FieldInfo &entry) {
        const VariableDecl &field = *entry.declaration;
        const auto member = owner.members.find(field.name().lexeme);
        Token parameter = field.name();
        parameter.lexeme =
            (reversed ? "__gti_reversed_" : "__gti_designated_") +
            parameter.lexeme;
        parameter.generated = true;
        parameters.emplace_back(field.type(), parameter);
        types.push_back(member->second.symbol.type);
      };
      if (reversed) {
        // A leading tag keeps the reversed signature distinct from the
        // designated one for every field list (a single field or a
        // palindromic type sequence would otherwise redeclare it).
        Token tag = owner.name;
        tag.kind = TokenKind::IDENTIFIER;
        tag.lexeme = "__gti_reversed_tag";
        tag.generated = true;
        Token boolKeyword = owner.name;
        boolKeyword.kind = TokenKind::BOOL;
        boolKeyword.lexeme = "bool";
        boolKeyword.generated = true;
        parameters.emplace_back(TypeRef(boolKeyword), tag);
        types.push_back(SemanticType::Bool);
        for (auto entry = owner.fields.rbegin(); entry != owner.fields.rend();
             ++entry) {
          describe(*entry);
        }
      } else {
        for (const FieldInfo &entry : owner.fields) {
          describe(entry);
        }
      }
      for (const FieldInfo &entry : owner.fields) {
        const VariableDecl &field = *entry.declaration;
        Token parameter = field.name();
        parameter.lexeme =
            (reversed ? "__gti_reversed_" : "__gti_designated_") +
            parameter.lexeme;
        parameter.generated = true;
        Token stdName = parameter;
        stdName.lexeme = "std";
        Token moveName = parameter;
        moveName.lexeme = "move";
        Token paren = parameter;
        paren.kind = TokenKind::RIGHT_PAREN;
        paren.lexeme = ")";
        ExprList arguments;
        arguments.push_back(std::make_unique<Variable>(parameter));
        ExprList fieldArguments;
        auto transfer = std::make_unique<Call>(
            std::make_unique<QualifiedName>(
                NamePath(std::vector<Token>{stdName, moveName})),
            std::vector<TypeRef>{}, paren, std::move(arguments));
        transfers.push_back(transfer.get());
        fieldArguments.push_back(std::move(transfer));
        initializers.push_back({.target = TypeRef(field.name()),
                                .arguments = std::move(fieldArguments)});
      }
      designatedMoves.insert(transfers.begin(), transfers.end());
      Token name = owner.name;
      name.generated = true;
      auto declaration = std::make_shared<ConstructorDecl>(
          std::nullopt, name, std::vector<GenericParameter>{},
          std::move(parameters), std::nullopt, std::move(initializers),
          std::nullopt, std::make_unique<BlockStmt>(StmtList{}));
      ConstructorInfo info{.id = nextConstructorId++,
                           .owner = owner.id,
                           .declaration = declaration.get(),
                           .parameterTypes = std::move(types),
                           .requiredParameterCount =
                               owner.fields.size() + (reversed ? 1 : 0)};
      constructorGenericParameters.emplace(declaration.get(),
                                           std::vector<GenericParameterInfo>{});
      ClassLifecycleInfo lifecycle =
          *semanticModel.findClassLifecycle(*owner.declaration);
      lifecycle.constructors.push_back(info);
      semanticModel.record(*owner.declaration, std::move(lifecycle));
      semanticModel.designatedConstructors.push_back(std::move(declaration));
      return info;
    };
    owner.designatedConstructor = generate(false);
    owner.reversedDesignatedConstructor = generate(true);
  }
}

auto SemanticVisitor::Impl::recordClassLifecycles() -> void {
  for (const ClassInfo &owner : classes) {
    if (owner.declaration == nullptr) {
      continue;
    }
    for (const FieldInfo &field : owner.fields) {
      if (field.declaration == nullptr || field.declaration->initializer())
        continue;
      const auto member = owner.members.find(field.declaration->name().lexeme);
      if (owner.kind != ClassKind::Union && !owner.cAbiRecord &&
          member != owner.members.end() &&
          member->second.symbol.type.kind == SemanticType::Class)
        semanticModel.retainImplicitFieldInitializer(*field.declaration);
    }
    SemanticTypeTraits traits = typeTraits(openClassType(owner.id));
    // The declaration-bound optional slot is a single ownership state. Copy
    // assignment is intentionally unavailable even though its source class
    // has no fields for structural analysis to reject: duplicating an engaged
    // backend slot would copy a live T outside the public optional policy.
    if (owner.compilerCapability ==
        CompilerCapabilityTypeKind::OptionalStorage) {
      traits.copyable = false;
      traits.copyAssignable = false;
      traits.moveAssignable = false;
    }
    // A class that owns the exact optional slot may transfer that slot only
    // through construction. GTI has no source-level custom special-member
    // assignment body with which the public policy class could uphold the
    // slot's engaged/empty transition and checked cleanup contract, so both
    // assignment shapes fail closed while reset/emplace remain available.
    const bool ownsOptionalStorage = std::any_of(
        owner.fields.begin(), owner.fields.end(), [&](const FieldInfo &field) {
          if (field.declaration == nullptr) {
            return false;
          }
          const auto member =
              owner.members.find(field.declaration->name().lexeme);
          if (member == owner.members.end()) {
            return false;
          }
          const SemanticType &fieldType = member->second.symbol.type;
          return fieldType.kind == SemanticType::Class &&
                 fieldType.classId != 0 &&
                 classInfo(fieldType.classId).compilerCapability ==
                     CompilerCapabilityTypeKind::OptionalStorage;
        });
    if (ownsOptionalStorage) {
      traits.copyAssignable = false;
      traits.moveAssignable = false;
    }
    const ConstructorInfo *declaredDefault = defaultConstructor(owner);
    SpecialMemberStatus defaultStatus = SpecialMemberStatus::Deleted;
    if (declaredDefault != nullptr &&
        requestsStructuralDefaultConstruction(*declaredDefault)) {
      if (classCanUseStructuralDefaultConstruction(owner)) {
        defaultStatus = SpecialMemberStatus::Generated;
      } else {
        const SpecialMemberSpecifier &specifier =
            *declaredDefault->declaration->specifier();
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2020", DiagnosticPhase::Semantics, specifier.keyword,
            "The defaulted default constructor of '" + owner.name.lexeme +
                "' is unavailable because its fields or state-bearing base "
                "cannot be structurally default-constructed.");
        const auto missing =
            std::find_if(owner.fields.begin(), owner.fields.end(),
                         [](const FieldInfo &field) {
                           return field.declaration != nullptr &&
                                  field.declaration->initializer() == nullptr;
                         });
        if (missing != owner.fields.end()) {
          diagnostic.related.push_back(
              {tokenSpan(missing->declaration->name()),
               "This field has no declaration initializer."});
        }
        diagnostic.hints.emplace_back(
            "Give every field a declaration initializer and ensure the "
            "state-bearing base has an accessible zero-argument "
            "constructor, or write an ordinary constructor body.");
        diagnostics.emplace_back(std::move(diagnostic));
      }
    } else if (declaredDefault != nullptr) {
      defaultStatus = SpecialMemberStatus::Declared;
    } else if (classCanGenerateDefaultConstructor(owner)) {
      defaultStatus = SpecialMemberStatus::Generated;
    }
    const auto specialStatus =
        [&](const std::optional<ConstructorInfo> &constructor, bool available,
            std::string_view operation) {
          if (!constructor || constructor->declaration == nullptr ||
              !constructor->declaration->specifier()) {
            return available ? SpecialMemberStatus::Generated
                             : SpecialMemberStatus::Deleted;
          }
          const SpecialMemberSpecifier &specifier =
              *constructor->declaration->specifier();
          if (specifier.kind == SpecialMemberSpecifierKind::Deleted) {
            return SpecialMemberStatus::Deleted;
          }
          if (!available) {
            Diagnostic diagnostic = makeDiagnostic(
                "GTI-S2020", DiagnosticPhase::Semantics, specifier.keyword,
                "The defaulted " + std::string(operation) + " of '" +
                    owner.name.lexeme +
                    "' is unavailable because its base, fields, or cleanup "
                    "policy does not support that operation.");
            diagnostic.hints.emplace_back(
                "Declare this constructor '= delete' or change the "
                "non-" +
                std::string(operation == "copy constructor" ? "copyable"
                                                            : "movable") +
                " component.");
            diagnostics.emplace_back(std::move(diagnostic));
            return SpecialMemberStatus::Deleted;
          }
          return SpecialMemberStatus::Generated;
        };
    const SpecialMemberStatus copyStatus = specialStatus(
        owner.copyConstructor, traits.copyable, "copy constructor");
    const SpecialMemberStatus moveStatus = specialStatus(
        owner.moveConstructor, traits.movable, "move constructor");
    const bool sourceDestructor = hasSourceDestructorBody(owner);
    const bool deletedDestructor =
        owner.destructor && owner.destructor->declaration != nullptr &&
        owner.destructor->declaration->specifier() &&
        owner.destructor->declaration->specifier()->kind ==
            SpecialMemberSpecifierKind::Deleted;
    ConstructorId generatedBaseConstructor = 0;
    if (defaultStatus == SpecialMemberStatus::Generated &&
        owner.fields.empty() && owner.genericParameters.empty()) {
      if (const auto *base = concreteBase(owner);
          base != nullptr && base->type.arguments.empty() &&
          base->type.valueArguments.empty()) {
        const auto *selected =
            defaultConstructor(classInfo(base->type.classId));
        if (selected != nullptr && selected->parameterTypes.empty() &&
            selected->declaration != nullptr &&
            selected->declaration->body() != nullptr)
          generatedBaseConstructor = selected->id;
      }
    }
    semanticModel.record(
        *owner.declaration,
        ClassLifecycleInfo{
            .id = owner.id,
            .declaration = owner.declaration,
            .constructors = owner.constructors,
            .declaredCopyConstructor = owner.copyConstructor,
            .declaredMoveConstructor = owner.moveConstructor,
            .declaredDestructor = owner.destructor,
            .defaultConstructor = defaultStatus,
            .generatedDefaultBaseConstructor = generatedBaseConstructor,
            .generatedDefaultFieldInitializers =
                defaultStatus == SpecialMemberStatus::Generated &&
                owner.kind != ClassKind::Union && !owner.fields.empty() &&
                std::all_of(owner.fields.begin(), owner.fields.end(),
                            [&](const FieldInfo &field) {
                              return field.declaration != nullptr &&
                                     (field.declaration->initializer() ||
                                      semanticModel.implicitFieldInitializer(
                                          *field.declaration));
                            }),
            .copyConstructor = copyStatus,
            .moveConstructor = moveStatus,
            .copyAssignment = traits.copyAssignable
                                  ? SpecialMemberStatus::Generated
                                  : SpecialMemberStatus::Deleted,
            .moveAssignment = traits.moveAssignable
                                  ? SpecialMemberStatus::Generated
                                  : SpecialMemberStatus::Deleted,
            .destructor =
                deletedDestructor
                    ? SpecialMemberStatus::Deleted
                    : (sourceDestructor ? SpecialMemberStatus::Declared
                                        : SpecialMemberStatus::Generated),
            .requiresActiveDropState = sourceDestructor,
            .traits = traits,
            .polymorphic = owner.polymorphic});
  }
}

auto SemanticVisitor::Impl::collectMembers(const StmtList &members,
                                           ClassInfo &owner,
                                           AccessModifier &access,
                                           const Token *mutableGroupOrigin)
    -> void {
  for (const StmtPtr &statement : members) {
    if (const auto *group = stmtAs<MutableFieldGroupDecl>(statement.get())) {
      collectMembers(group->members(), owner, access, &group->keyword());
      continue;
    }
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        collectMembers(*branch, owner, access, mutableGroupOrigin);
      }
      continue;
    }
    if (const auto *specifier = stmtAs<AccessSpecifierDecl>(statement.get())) {
      if (owner.kind == ClassKind::Interface) {
        report(specifier->keyword(),
               "Interface members are always public; access specifiers are "
               "not permitted.",
               "GTI-S2041");
      }
      if (owner.kind == ClassKind::Union &&
          specifier->modifier() == AccessModifier::Private) {
        report(specifier->keyword(),
               "Union fields are public; a union cannot declare a private "
               "section.",
               "GTI-S2066");
        access = AccessModifier::Public;
      } else {
        access = specifier->modifier();
      }
      continue;
    }

    if (stmtAs<TypeAliasDecl>(statement.get()) != nullptr) {
      // Type aliases are registered and resolved before value/function member
      // collection so declaration order cannot affect member signatures.
      continue;
    }

    if (const auto *constructor = stmtAs<ConstructorDecl>(statement.get())) {
      if (owner.kind == ClassKind::Union) {
        report(constructor->name(),
               "A passive union cannot declare constructors.", "GTI-S2066");
        continue;
      }
      if (owner.kind == ClassKind::Interface) {
        report(constructor->name(), "Interfaces cannot declare constructors.",
               "GTI-S2041");
        continue;
      }
      ConstructorInfo info{.id = nextConstructorId++,
                           .owner = owner.id,
                           .declaration = constructor,
                           .access = access,
                           .constexprConstructor = constructor->isConstexpr()};
      info.requiredParameterCount =
          requiredParameterCount(constructor->parameters());
      info.defaultArgumentEffectOwners =
          allocateDefaultArgumentEffectOwners(constructor->parameters());
      for (const GenericParameter &constructorParameter :
           constructor->genericParameters()) {
        if (constructorParameter.equal) {
          report(*constructorParameter.equal,
                 "Constructor-local generic parameters are inference-only "
                 "and cannot declare defaults.",
                 "GTI-S2026");
        }
        for (const GenericParameterInfo &classParameter :
             owner.genericParameters) {
          if (constructorParameter.name.lexeme == classParameter.name.lexeme) {
            report(constructorParameter.name,
                   "Constructor-local generic parameters cannot shadow "
                   "class or struct generic parameters.",
                   "GTI-S2026");
          }
        }
      }
      std::vector<std::string> memberScope = owner.namespaceScope;
      memberScope.emplace_back(owner.name.lexeme);
      info.genericParameters = makeGenericParameters(
          constructor->genericParameters(), constructor->name(), memberScope);
      constructorGenericParameters.insert_or_assign(constructor,
                                                    info.genericParameters);
      info.parameterPack = !constructor->parameters().empty() &&
                           constructor->parameters().back().pack.has_value();
      if (info.parameterPack) {
        info.requiredParameterCount = constructor->parameters().size() - 1;
      }
      beginTypeParameterScope(info.genericParameters);
      validateParameterPacks(constructor->parameters(), info.genericParameters);
      info.requirements = resolveRequirements(
          constructor->requiresClause() ? &*constructor->requiresClause()
                                        : nullptr,
          memberScope);
      info.parameterTypes.reserve(constructor->parameters().size());
      for (const Parameter &parameter : constructor->parameters()) {
        info.parameterTypes.emplace_back(typeOf(parameter, memberScope));
      }
      endTypeParameterScope();
      for (const GenericParameterInfo &parameter : info.genericParameters) {
        const bool inferable =
            std::any_of(info.parameterTypes.begin(), info.parameterTypes.end(),
                        [&](const SemanticType &type) {
                          return containsGenericParameter(type, parameter.id,
                                                          parameter.value);
                        });
        if (!inferable) {
          report(parameter.name,
                 "Constructor-local " +
                     std::string(parameter.value ? "value" : "type") +
                     " parameter '" + parameter.name.lexeme +
                     "' must occur in a constructor parameter type so calls "
                     "can infer it exactly.",
                 "GTI-S2026");
        }
      }
      info.compilerPrivate =
          owner.compilerPrivate ||
          (access == AccessModifier::Public &&
           std::any_of(info.parameterTypes.begin(), info.parameterTypes.end(),
                       [this](const SemanticType &type) {
                         return semanticModel.isCompilerPrivateType(type);
                       }));

      const SemanticType receiverType = openClassType(owner.id);
      if (info.genericParameters.empty() && info.parameterTypes.size() == 1) {
        const SemanticType &parameter = info.parameterTypes.front();
        const Parameter &syntax = constructor->parameters().front();
        const bool receiverReference =
            parameter.kind == SemanticType::Reference &&
            parameter.arguments.size() == 1 &&
            parameter.arguments.front() == receiverType;
        if (receiverReference && syntax.type.reference) {
          info.kind = syntax.type.reference->kind == TokenKind::AND
                          ? ConstructorKind::Move
                          : ConstructorKind::Copy;
        } else if (parameter == receiverType) {
          report(constructor->name(),
                 "A copy or move constructor must take exactly one '" +
                     owner.name.lexeme + "&' or '" + owner.name.lexeme +
                     "&&' parameter.",
                 "GTI-S2020");
          continue;
        }
      }

      if (info.kind != ConstructorKind::Ordinary) {
        const Parameter &parameter = constructor->parameters().front();
        if (parameter.mutability == Mutability::Mutable) {
          report(parameter.name.lexeme.empty() ? parameter.type.name.last()
                                               : parameter.name,
                 "Copy and move policy parameters do not take 'mut'.",
                 "GTI-S2020");
        }
        if (parameter.pack) {
          report(*parameter.pack,
                 "Copy and move constructors cannot use a parameter pack.",
                 "GTI-S2020");
        }
        if (access != AccessModifier::Public) {
          report(constructor->name(),
                 "Copy and move constructor policies must be public; use "
                 "'= delete' to disable an operation.",
                 "GTI-S2020");
        }
        if (!constructor->initializers().empty()) {
          report(constructor->name(),
                 "Copy and move constructor policies cannot have an "
                 "initializer list.",
                 "GTI-S2020");
        }
        if (!constructor->specifier()) {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2020", DiagnosticPhase::Semantics, constructor->name(),
              "Custom copy and move constructor bodies require "
              "constructor-wide definite initialization and active-drop "
              "tracking and are not supported yet.");
          diagnostic.hints.emplace_back(
              info.kind == ConstructorKind::Copy
                  ? "Declare the policy as '" + owner.name.lexeme + "(" +
                        owner.name.lexeme + "&) = default;' or '= delete;'."
                  : "Declare the policy as '" + owner.name.lexeme + "(" +
                        owner.name.lexeme + "&&) = default;' or '= delete;'.");
          diagnostics.emplace_back(std::move(diagnostic));
        }

        std::optional<ConstructorInfo> &declared =
            info.kind == ConstructorKind::Copy ? owner.copyConstructor
                                               : owner.moveConstructor;
        if (declared) {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2011", DiagnosticPhase::Semantics, constructor->name(),
              std::string("Duplicate ") +
                  (info.kind == ConstructorKind::Copy ? "copy" : "move") +
                  " constructor policy for '" + owner.name.lexeme + "'.");
          diagnostic.related.push_back(
              {tokenSpan(declared->declaration->name()),
               "Previous policy declaration is here."});
          diagnostics.emplace_back(std::move(diagnostic));
          continue;
        }
        declared = std::move(info);
        continue;
      }

      if (constructor->specifier()) {
        const SpecialMemberSpecifier &specifier = *constructor->specifier();
        if (specifier.kind == SpecialMemberSpecifierKind::Deleted) {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2020", DiagnosticPhase::Semantics, specifier.keyword,
              "'= delete' is not supported for ordinary constructors; "
              "only the exact default-constructor form '" +
                  owner.name.lexeme + "() = default;' is available.");
          diagnostic.hints.emplace_back(
              "Use constructor access and explicit overloads to control "
              "ordinary construction.");
          diagnostics.emplace_back(std::move(diagnostic));
        } else if (!constructor->genericParameters().empty() ||
                   !constructor->parameters().empty() ||
                   constructor->requiresClause() ||
                   !constructor->initializers().empty()) {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2020", DiagnosticPhase::Semantics, specifier.keyword,
              "A defaulted ordinary constructor must use the exact form '" +
                  owner.name.lexeme + "() = default;'.");
          diagnostic.related.push_back(
              {tokenSpan(constructor->name()),
               "This constructor has generic parameters, parameters, "
               "requirements, or an initializer list."});
          diagnostic.hints.emplace_back(
              "Remove the extra constructor surface or write an ordinary "
              "constructor body.");
          diagnostics.emplace_back(std::move(diagnostic));
        }
      }

      const auto duplicate =
          std::find_if(owner.constructors.begin(), owner.constructors.end(),
                       [&](const ConstructorInfo &previous) {
                         return sameConstructorSignature(previous, info);
                       });
      if (duplicate != owner.constructors.end()) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2011", DiagnosticPhase::Semantics, constructor->name(),
            "Duplicate constructor overload signature for '" +
                owner.name.lexeme + "'.");
        diagnostic.related.push_back(
            {tokenSpan(duplicate->declaration->name()),
             "Previous constructor overload is here."});
        diagnostics.emplace_back(std::move(diagnostic));
        continue;
      }

      owner.constructors.emplace_back(std::move(info));
      continue;
    }

    if (const auto *destructor = stmtAs<DestructorDecl>(statement.get())) {
      if (owner.kind == ClassKind::Union) {
        report(destructor->tilde(),
               "A passive union cannot declare a destructor.", "GTI-S2066");
        continue;
      }
      if (owner.kind == ClassKind::Interface) {
        report(destructor->tilde(),
               "Interfaces use compiler-generated polymorphic destruction "
               "and cannot declare destructors.",
               "GTI-S2041");
        continue;
      }
      if (destructor->name().lexeme != owner.name.lexeme) {
        report(destructor->name(),
               "Destructor name must match its class or struct name '" +
                   owner.name.lexeme + "'.",
               "GTI-S2021");
        continue;
      }
      if (owner.destructor) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2021", DiagnosticPhase::Semantics, destructor->tilde(),
            "A class or struct cannot declare more than one destructor.");
        diagnostic.related.push_back(
            {tokenSpan(owner.destructor->declaration->tilde()),
             "Previous destructor is here."});
        diagnostics.emplace_back(std::move(diagnostic));
        continue;
      }
      if (access == AccessModifier::Private) {
        report(destructor->tilde(),
               "Destructors must be public in the current ownership layer.",
               "GTI-S2021");
      }
      if (destructor->specifier() && destructor->specifier()->kind ==
                                         SpecialMemberSpecifierKind::Deleted) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2021", DiagnosticPhase::Semantics,
            destructor->specifier()->keyword,
            "'= delete' is not supported for destructors; use '~" +
                owner.name.lexeme +
                "() = default;' to select structural destruction.");
        diagnostic.hints.emplace_back(
            "GTI values must remain destructible in the current ownership "
            "model.");
        diagnostics.emplace_back(std::move(diagnostic));
      }
      owner.destructor = DestructorInfo{
          .owner = owner.id, .declaration = destructor, .access = access};
      continue;
    }

    const Token *name = nullptr;
    const VariableDecl *field = nullptr;
    const FunctionDecl *function = stmtAs<FunctionDecl>(statement.get());
    Symbol symbol;
    if (function != nullptr) {
      if (owner.kind == ClassKind::Union) {
        report(function->name(), "A passive union cannot declare methods.",
               "GTI-S2066");
        continue;
      }
      for (const GenericParameter &methodParameter :
           function->genericParameters()) {
        for (const GenericParameterInfo &classParameter :
             owner.genericParameters) {
          if (methodParameter.name.lexeme == classParameter.name.lexeme) {
            report(methodParameter.name,
                   "Method generic type parameters cannot shadow class or "
                   "struct type parameters.");
          }
        }
      }
      name = &function->name();
      std::vector<std::string> memberScope = owner.namespaceScope;
      memberScope.emplace_back(owner.name.lexeme);
      symbol = functionSymbol(*function, memberScope);
      symbol.staticMember = function->isStatic();
      const bool polymorphic = function->isVirtual() || function->isPure() ||
                               function->isOverride() ||
                               owner.kind == ClassKind::Interface;
      if (polymorphic && function->isStatic()) {
        report(function->name(),
               "Static methods do not have a receiver and cannot be "
               "virtual.",
               "GTI-S2042");
      }
      if (polymorphic && !function->genericParameters().empty()) {
        report(function->name(),
               "Virtual methods cannot declare method-level generic "
               "parameters.",
               "GTI-S2042");
      }
      if (function->isPure() && owner.kind != ClassKind::Interface &&
          !function->isVirtual() && !function->isOverride()) {
        report(function->pureSpecifier()->equal,
               "A pure class method must be declared 'virtual' or "
               "'override'.",
               "GTI-S2042");
      }
      if (function->isVirtual() && !function->isPure() && !function->body()) {
        report(function->name(), "A non-pure virtual method requires a body.",
               "GTI-S2042");
      }
      if (owner.kind == ClassKind::Interface) {
        if (function->body()) {
          report(function->name(),
                 "Interface methods are pure contracts and cannot have a "
                 "body.",
                 "GTI-S2041");
        }
        if (function->isPure()) {
          report(function->pureSpecifier()->equal,
                 "Interface methods are implicitly pure; end the "
                 "declaration with ';' instead of '= 0;'.",
                 "GTI-S2041");
        }
        if (access != AccessModifier::Public) {
          report(function->name(), "Interface methods must be public.",
                 "GTI-S2041");
        }
      }
    } else if (const auto *variable = stmtAs<VariableDecl>(statement.get())) {
      if (mutableGroupOrigin != nullptr) {
        mutableFieldGroupOrigins.insert_or_assign(variable, mutableGroupOrigin);
      }
      if (owner.kind == ClassKind::Union && variable->isStatic()) {
        report(variable->name(),
               "A passive union cannot declare static fields.", "GTI-S2066");
        continue;
      }
      name = &variable->name();
      field = variable;
      std::vector<std::string> memberScope = owner.namespaceScope;
      memberScope.emplace_back(owner.name.lexeme);
      symbol = Symbol{
          .type = typeOf(variable->type(), variable->mutability(), memberScope),
          .sourceUnit = owner.sourceUnit,
          .assignable = variable->isMutable(),
          .declaration = variable->name(),
          .variableDeclaration = variable,
          .bindingKind = variable->isStatic() ? SemanticBindingKind::StaticField
                                              : SemanticBindingKind::Field,
          .staticMember = variable->isStatic(),
          .compilerPrivate = owner.compilerPrivate};
      predeclaredVariables.insert(variable);
      if (owner.kind == ClassKind::Interface) {
        report(variable->name(),
               "Interfaces are behavior-only and cannot declare data "
               "members.",
               "GTI-S2041");
        continue;
      }
    }
    if (name == nullptr) {
      continue;
    }

    symbol.ownerClass = owner.id;
    symbol.access = access;
    symbol.compilerPrivate = symbol.compilerPrivate || owner.compilerPrivate ||
                             (access == AccessModifier::Public &&
                              semanticModel.isCompilerPrivateType(symbol.type));
    if ((function != nullptr && function->isStatic()) ||
        (field != nullptr && field->isStatic())) {
      // Protocol invocation retains an exact owner specialization and needs
      // no source-qualified generic member path. Ordinary static members
      // retain the existing source-path restriction.
      const bool collectionProtocol =
          function != nullptr && (name->lexeme == "collection_init" ||
                                  name->lexeme == "collection_insert");
      if (!owner.genericParameters.empty() && !collectionProtocol) {
        report(*name,
               "Static members of generic classes and structs require "
               "qualified generic member paths, which are not supported "
               "yet.",
               "GTI-S2039");
      }
    }
    for (CallableCandidate &overload : symbol.overloads) {
      overload.ownerClass = owner.id;
      overload.dispatchOwner = openClassType(owner.id);
      overload.compilerPrivate =
          overload.compilerPrivate || owner.compilerPrivate ||
          (access == AccessModifier::Public &&
           (semanticModel.isCompilerPrivateType(overload.returnType) ||
            std::any_of(overload.parameterTypes.begin(),
                        overload.parameterTypes.end(),
                        [this](const SemanticType &type) {
                          return semanticModel.isCompilerPrivateType(type);
                        })));
      overload.access = access;
      overload.staticMember = function != nullptr && function->isStatic();
      if (function != nullptr) {
        overload.virtualMethod = function->isVirtual() || function->isPure() ||
                                 function->isOverride() ||
                                 owner.kind == ClassKind::Interface;
        overload.pureVirtual =
            function->isPure() || owner.kind == ClassKind::Interface;
        overload.overrideMethod = function->isOverride();
        if (overload.virtualMethod && !overload.overrideMethod &&
            overload.id != 0) {
          overload.virtualRoots = {overload.id};
        }
      }
      applyFunctionReturnBorrowSummary(overload);
    }
    if (function != nullptr && !symbol.overloads.empty()) {
      publishCallableDeclaration(*function, symbol.overloads.front(),
                                 owner.namespaceScope, owner.id);
    }
    if (const auto alias = owner.typeAliases.find(name->lexeme);
        alias != owner.typeAliases.end()) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2006", DiagnosticPhase::Semantics, *name,
          "Duplicate member declaration of '" + name->lexeme +
              "'; the name already denotes a class-scoped type alias.");
      diagnostic.related.push_back(
          {tokenSpan(alias->second.declaration->name()),
           "Type alias is declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
      continue;
    }
    const auto existing = owner.members.find(name->lexeme);
    if (existing != owner.members.end()) {
      if (appendFunctionOverload(existing->second.symbol, std::move(symbol),
                                 *name)) {
        continue;
      }
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2006", DiagnosticPhase::Semantics, *name,
          "Duplicate member declaration of '" + name->lexeme + "'.");
      diagnostic.related.push_back(
          {tokenSpan(existing->second.symbol.declaration),
           "Previous member declaration is here."});
      diagnostics.emplace_back(std::move(diagnostic));
      continue;
    }

    owner.members.emplace(name->lexeme, MemberInfo{.symbol = std::move(symbol),
                                                   .access = access});
    if (field != nullptr) {
      (field->isStatic() ? owner.staticFields : owner.fields)
          .push_back(FieldInfo{.declaration = field});
    }
  }
}

auto SemanticVisitor::Impl::projectCallableDeclaration(
    const CallableDeclaration &declaration) -> FunctionInfo {
  return FunctionInfo{
      .id = declaration.id,
      .sourceUnit = declaration.sourceUnit,
      .declaration = declaration.syntax,
      .qualifiedName = declaration.qualifiedName,
      .namespaceScope = declaration.namespaceScope,
      .returnType = declaration.returnType,
      .parameterTypes = declaration.parameterTypes,
      .requiredParameterCount = declaration.requiredParameterCount,
      .defaultArgumentEffectOwners = declaration.defaultArgumentEffectOwners,
      .genericParameters = declaration.genericParameters,
      .requirements = declaration.requirements,
      .parameterPack = declaration.parameterPack,
      .ownerClass = declaration.ownerClass,
      .entryPoint = declaration.entryPoint,
      .entryKind = declaration.entryKind,
      .entryArgumentAppendFunction = declaration.entryArgumentAppendFunction,
      .staticMember = declaration.staticMember,
      .internalLinkage = declaration.internalLinkage,
      .constexprFunction = declaration.constexprFunction,
      .requiresUnsafe = declaration.requiresUnsafe,
      .neverReturns = declaration.neverReturns,
      .tracksCaller = declaration.tracksCaller,
      .discardableResult = declaration.discardableResult,
      .cThreadOperation = declaration.cThreadOperation,
      .processMemoryOperation = declaration.processMemoryOperation,
      .requiresCThreadRuntime =
          requiresCThreadConcurrency(declaration.cThreadOperation),
      .linkage = declaration.linkage,
      .externalSymbol = declaration.externalSymbol,
      .cArrayCountParameter = declaration.cArrayCountParameter,
      .virtualMethod = declaration.virtualMethod,
      .pureVirtual = declaration.pureVirtual,
      .overrideMethod = declaration.overrideMethod,
      .intrinsic = declaration.intrinsic,
      .returnBorrowOrigin = declaration.returnBorrowOrigin,
      .explicitReturnBorrowOrigin =
          declaration.syntax != nullptr &&
          declaration.syntax->hasFunctionAttribute("borrowed_from"),
      .returnBorrowParameter = declaration.returnBorrowParameter,
      .returnBorrowAccess = declaration.returnBorrowAccess,
      .returnBorrowParameters = declaration.returnBorrowParameters,
      .returnBorrowFields = declaration.returnBorrowFields,
      .returnBorrowPlace = declaration.returnBorrowPlace,
      .virtualRoots = declaration.virtualRoots,
      .compilerPrivate = declaration.compilerPrivate};
}

auto SemanticVisitor::Impl::publishCallableDeclaration(
    const FunctionDecl &function, const CallableCandidate &candidate,
    const std::vector<std::string> &namespaceScope, ClassId ownerClass)
    -> void {
  CallableDeclaration *declaration = catalog.mutableCallable(function);
  if (declaration == nullptr) {
    return;
  }
  std::vector<std::string> qualifiedScope = namespaceScope;
  if (ownerClass != 0) {
    qualifiedScope.emplace_back(classInfo(ownerClass).name.lexeme);
  }
  const FunctionReturnBorrowSummary concreteBorrow =
      functionReturnBorrowSummary(
          candidate.returnType, candidate.parameterTypes, ownerClass,
          candidate.staticMember, candidate.record().intrinsic);
  const bool candidateOwnsBorrow =
      candidate.returnBorrowOrigin != BorrowOriginKind::None ||
      function.hasFunctionAttribute("borrowed_from");
  declaration->qualifiedName =
      qualifiedName(qualifiedScope, function.name().lexeme);
  declaration->namespaceScope = namespaceScope;
  declaration->returnType = candidate.returnType;
  declaration->parameterTypes = candidate.parameterTypes;
  declaration->requirements = candidate.requirements;
  declaration->ownerClass = ownerClass;
  declaration->staticMember = candidate.staticMember;
  declaration->virtualMethod = candidate.virtualMethod;
  declaration->pureVirtual = candidate.pureVirtual;
  declaration->overrideMethod = candidate.overrideMethod;
  declaration->returnBorrowOrigin = candidateOwnsBorrow
                                        ? candidate.returnBorrowOrigin
                                        : concreteBorrow.origin;
  declaration->returnBorrowParameter = candidateOwnsBorrow
                                           ? candidate.returnBorrowParameter
                                           : concreteBorrow.parameter;
  declaration->returnBorrowAccess = candidateOwnsBorrow
                                        ? candidate.returnBorrowAccess
                                        : concreteBorrow.access;
  declaration->returnBorrowPlace =
      candidateOwnsBorrow ? candidate.returnBorrowPlace : concreteBorrow.place;
  declaration->returnBorrowParameters = candidateOwnsBorrow
                                            ? candidate.returnBorrowParameters
                                            : concreteBorrow.parameters;
  declaration->returnBorrowFields = candidateOwnsBorrow
                                        ? candidate.returnBorrowFields
                                        : concreteBorrow.fields;
  declaration->virtualRoots = candidate.virtualRoots;
  declaration->compilerPrivate = candidate.compilerPrivate;
  const SourceUnit *unit = sourceGraph == nullptr
                               ? nullptr
                               : sourceGraph->findUnit(declaration->sourceUnit);
  const SemanticType context = SemanticType::rawPointerTo(SemanticType::Void);
  if (unit != nullptr && unit->role == SourceUnitRole::StandardLibrary &&
      unit->standardLibraryName == "std/cthread" &&
      declaration->namespaceScope ==
          std::vector<std::string>{"gti_internal", "runtime"} &&
      declaration->compilerPrivate && function.hasCLinkage() &&
      function.body() == nullptr && !function.runtimeBinding() &&
      declaration->genericParameters.empty() && !declaration->parameterPack &&
      declaration->ownerClass == 0) {
    if (declaration->externalSymbol == "gti_rt_cthread_create_v1" &&
        declaration->returnType == SemanticType::Int32 &&
        declaration->parameterTypes ==
            std::vector<SemanticType>{
                SemanticType::rawPointerTo(SemanticType::UInt64),
                SemanticType::nativeFunctionType(SemanticType::Int32,
                                                 std::array{context}),
                context}) {
      declaration->cThreadOperation = CThreadRuntimeOperation::Create;
    } else if (declaration->externalSymbol == "gti_rt_cthread_join_v1" &&
               declaration->returnType == SemanticType::Int32 &&
               declaration->parameterTypes ==
                   std::vector<SemanticType>{
                       SemanticType::UInt64,
                       SemanticType::rawPointerTo(SemanticType::Int32)}) {
      declaration->cThreadOperation = CThreadRuntimeOperation::Join;
    } else if (declaration->externalSymbol == "gti_rt_once_invoke_v1" &&
               declaration->returnType == SemanticType::Void &&
               declaration->parameterTypes ==
                   std::vector<SemanticType>{SemanticType::nativeFunctionType(
                       SemanticType::Void, {})}) {
      declaration->cThreadOperation =
          CThreadRuntimeOperation::InvokeOnceInitializer;
    } else if (declaration->externalSymbol == "gti_rt_cnd_init_v1" &&
               declaration->returnType == SemanticType::Int32 &&
               declaration->parameterTypes ==
                   std::vector<SemanticType>{
                       SemanticType::rawPointerTo(SemanticType::UInt64)}) {
      declaration->cThreadOperation = CThreadRuntimeOperation::ConditionInit;
    } else if (declaration->externalSymbol == "gti_rt_cnd_destroy_v1" &&
               declaration->returnType == SemanticType::Void &&
               declaration->parameterTypes ==
                   std::vector<SemanticType>{SemanticType::UInt64}) {
      declaration->cThreadOperation = CThreadRuntimeOperation::ConditionDestroy;
    } else if (declaration->externalSymbol == "gti_rt_cnd_wait_v1" &&
               declaration->returnType == SemanticType::Int32 &&
               declaration->parameterTypes ==
                   std::vector<SemanticType>{SemanticType::UInt64,
                                             SemanticType::UInt64}) {
      declaration->cThreadOperation = CThreadRuntimeOperation::ConditionWait;
    } else if (declaration->externalSymbol == "gti_rt_cnd_timedwait_v1" &&
               declaration->returnType == SemanticType::Int32 &&
               declaration->parameterTypes ==
                   std::vector<SemanticType>{
                       SemanticType::UInt64, SemanticType::UInt64,
                       SemanticType::Int64, SemanticType::Int64}) {
      declaration->cThreadOperation =
          CThreadRuntimeOperation::ConditionTimedWait;
    } else if (declaration->externalSymbol == "gti_rt_mtx_init_v1" &&
               declaration->returnType == SemanticType::Int32 &&
               declaration->parameterTypes ==
                   std::vector<SemanticType>{
                       SemanticType::rawPointerTo(SemanticType::UInt64),
                       SemanticType::Int32}) {
      declaration->cThreadOperation = CThreadRuntimeOperation::MutexInit;
    } else if (declaration->externalSymbol == "gti_rt_mtx_destroy_v1" &&
               declaration->returnType == SemanticType::Void &&
               declaration->parameterTypes ==
                   std::vector<SemanticType>{SemanticType::UInt64}) {
      declaration->cThreadOperation = CThreadRuntimeOperation::MutexDestroy;
    } else if (declaration->externalSymbol == "gti_rt_mtx_timedlock_v1" &&
               declaration->returnType == SemanticType::Int32 &&
               declaration->parameterTypes ==
                   std::vector<SemanticType>{SemanticType::UInt64,
                                             SemanticType::Int64,
                                             SemanticType::Int64}) {
      declaration->cThreadOperation = CThreadRuntimeOperation::MutexTimedLock;
    } else if (declaration->returnType == SemanticType::Int32 &&
               declaration->parameterTypes ==
                   std::vector<SemanticType>{SemanticType::UInt64}) {
      if (declaration->externalSymbol == "gti_rt_mtx_lock_v1") {
        declaration->cThreadOperation = CThreadRuntimeOperation::MutexLock;
      } else if (declaration->externalSymbol == "gti_rt_mtx_trylock_v1") {
        declaration->cThreadOperation = CThreadRuntimeOperation::MutexTryLock;
      } else if (declaration->externalSymbol == "gti_rt_mtx_unlock_v1") {
        declaration->cThreadOperation = CThreadRuntimeOperation::MutexUnlock;
      } else if (declaration->externalSymbol == "gti_rt_cnd_signal_v1") {
        declaration->cThreadOperation =
            CThreadRuntimeOperation::ConditionSignal;
      } else if (declaration->externalSymbol == "gti_rt_cnd_broadcast_v1") {
        declaration->cThreadOperation =
            CThreadRuntimeOperation::ConditionBroadcast;
      }
    } else if (declaration->returnType == SemanticType::Void &&
               declaration->parameterTypes.empty()) {
      if (declaration->externalSymbol == "gti_rt_cthread_begin_v1") {
        declaration->cThreadOperation = CThreadRuntimeOperation::Begin;
      } else if (declaration->externalSymbol == "gti_rt_cthread_end_v1") {
        declaration->cThreadOperation = CThreadRuntimeOperation::End;
      }
    }
  }
  // The process allocator's runtime ABI is GTI's own. Its declared contract
  // binds only by trusted declaration identity, never by a public spelling.
  if (unit != nullptr && unit->role == SourceUnitRole::StandardLibrary &&
      unit->standardLibraryName == "std/memory" &&
      declaration->namespaceScope ==
          std::vector<std::string>{"gti_internal", "runtime"} &&
      declaration->compilerPrivate && function.hasCLinkage() &&
      function.body() == nullptr && !function.runtimeBinding() &&
      declaration->genericParameters.empty() && !declaration->parameterPack &&
      declaration->ownerClass == 0 && !declaration->requiresUnsafe) {
    declaration->processMemoryOperation = exactProcessMemoryRuntimeOperation(
        declaration->externalSymbol, declaration->returnType,
        declaration->parameterTypes);
  }
  requestSession.hasCThreadRequirements |=
      requiresCThreadConcurrency(declaration->cThreadOperation);
  FunctionInfo projected = projectCallableDeclaration(*declaration);
  projected.ownedPointeeProjection = ownedPointeeReturnProjection(projected);
  semanticModel.record(function, std::move(projected));
}

auto SemanticVisitor::Impl::capabilityRequirement(
    const ClassInfo &owner, bool transfer,
    std::unordered_set<ClassId> &visiting) const -> const ClassInfo * {
  if (!visiting.insert(owner.id).second) {
    return nullptr;
  }
  const ConcurrencyCapabilityPolicy policy =
      transfer ? owner.transferPolicy : owner.sharePolicy;
  if (owner.kind == ClassKind::Interface &&
      policy == ConcurrencyCapabilityPolicy::Required) {
    visiting.erase(owner.id);
    return &owner;
  }
  for (const ClassBaseTypeInfo &base : owner.bases) {
    if (base.type.kind != SemanticType::Class || base.type.classId == 0 ||
        base.type.classId > classes.size()) {
      continue;
    }
    if (const ClassInfo *requirement = capabilityRequirement(
            classInfo(base.type.classId), transfer, visiting)) {
      visiting.erase(owner.id);
      return requirement;
    }
  }
  visiting.erase(owner.id);
  return nullptr;
}

auto SemanticVisitor::Impl::validateInterfaceCapabilities() -> void {
  for (const ClassInfo &owner : classes) {
    if (owner.declaration == nullptr || owner.kind == ClassKind::Interface) {
      continue;
    }
    const SemanticTypeTraits traits = typeTraits(openClassType(owner.id));
    for (const bool transfer : {true, false}) {
      std::unordered_set<ClassId> visiting;
      const ClassInfo *requirement =
          capabilityRequirement(owner, transfer, visiting);
      const bool available =
          transfer ? traits.transferCapable : traits.shareCapable;
      if (requirement == nullptr || available) {
        continue;
      }
      const std::string capability = transfer ? "transfer" : "share";
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2059", DiagnosticPhase::Semantics, owner.name,
          "Type '" + owner.name.lexeme + "' does not satisfy the " +
              capability + " capability required by interface '" +
              requirement->name.lexeme + "'.");
      const std::optional<Token> &source =
          transfer ? requirement->transferPolicySource
                   : requirement->sharePolicySource;
      if (source) {
        diagnostic.related.push_back(
            {tokenSpan(*source), "Interface capability required here."});
      }
      diagnostic.hints.emplace_back(
          "Remove non-capable state or cleanup, constrain generic state "
          "with std::" +
          (transfer ? std::string("transferable") : std::string("shareable")) +
          ", or use [[unsafe_" + capability +
          "]] only after auditing the full nominal contract.");
      diagnostics.emplace_back(std::move(diagnostic));
    }
  }
}

auto SemanticVisitor::Impl::recordClassTypeIdentities() -> void {
  for (const ClassInfo &owner : classes) {
    if (owner.declaration == nullptr) {
      continue;
    }
    semanticModel.recordClassType(
        *owner.declaration,
        ClassTypeInfo{
            .id = owner.id,
            .sourceUnit = owner.sourceUnit,
            .declaration = owner.declaration,
            .qualifiedName =
                qualifiedName(owner.namespaceScope, owner.name.lexeme),
            .namespaceScope = owner.namespaceScope,
            .exactSpecializationPrimary = owner.exactSpecializationPrimary,
            .exactTypeArguments = owner.exactTypeArguments,
            .exactValueArguments = owner.exactValueArguments,
            .genericParameters = owner.genericParameters,
            .genericArgumentPatterns = owner.genericArgumentPatterns,
            .kind = owner.kind,
            .compilerPrivate = owner.compilerPrivate,
            .reflectPrivate = owner.reflectPrivate,
            .compilerCapability = owner.compilerCapability});
  }
}

auto SemanticVisitor::Impl::recordClassFieldSymbols() -> void {
  for (const ClassInfo &owner : classes) {
    if (owner.declaration == nullptr)
      continue;
    // The catalog owns active declaration order. Reserve physical field
    // identities before summaries/layout queries can first-reference them in
    // pointer-keyed lookup order; inherited lookup aliases reuse the same ID.
    const auto visit = [&](const auto &self, const StmtList &members) -> void {
      for (const StmtPtr &statement : members) {
        if (const auto *conditional =
                stmtAs<ConditionalStmt>(statement.get())) {
          if (const StmtList *active = conditional->activeBranch(target))
            self(self, *active);
        } else if (const auto *field = stmtAs<VariableDecl>(statement.get())) {
          const auto member = owner.members.find(field->name().lexeme);
          if (member != owner.members.end() &&
              (member->second.symbol.bindingKind ==
                   SemanticBindingKind::Field ||
               member->second.symbol.bindingKind ==
                   SemanticBindingKind::StaticField))
            toolingSymbolFor(member->second.symbol);
        }
      }
    };
    visit(visit, owner.declaration->members());
  }
}

auto SemanticVisitor::Impl::recordClassTypes() -> void {
  for (const ClassInfo &owner : classes) {
    if (owner.declaration == nullptr) {
      continue;
    }
    std::vector<ClassFieldTypeInfo> fields;
    std::vector<ClassFieldTypeInfo> staticFields;
    std::vector<const TypeAliasDecl *> aliases;
    for (const RegisteredTypeAlias &alias : typeAliases) {
      if (alias.ownerClass == owner.id &&
          alias.resolution == TypeAliasResolution::Resolved)
        aliases.push_back(alias.declaration);
    }
    fields.reserve(owner.fields.size());
    for (const FieldInfo &field : owner.fields) {
      if (field.declaration == nullptr) {
        continue;
      }
      const auto member = owner.members.find(field.declaration->name().lexeme);
      // Field projections cross concrete-analysis snapshots. Establish their
      // declaration identity before any body/query worker can allocate a
      // snapshot-local symbol for a not-yet-visited class declaration.
      if (member != owner.members.end())
        toolingSymbolFor(member->second.symbol);
      fields.push_back({.declaration = field.declaration,
                        .type = member == owner.members.end()
                                    ? SemanticType::Unknown
                                    : member->second.symbol.type});
    }
    staticFields.reserve(owner.staticFields.size());
    for (const FieldInfo &field : owner.staticFields) {
      if (field.declaration == nullptr) {
        continue;
      }
      const auto member = owner.members.find(field.declaration->name().lexeme);
      if (member != owner.members.end())
        toolingSymbolFor(member->second.symbol);
      staticFields.push_back({.declaration = field.declaration,
                              .type = member == owner.members.end()
                                          ? SemanticType::Unknown
                                          : member->second.symbol.type});
    }
    semanticModel.recordClassType(
        *owner.declaration,
        ClassTypeInfo{
            .id = owner.id,
            .sourceUnit = owner.sourceUnit,
            .declaration = owner.declaration,
            .qualifiedName =
                qualifiedName(owner.namespaceScope, owner.name.lexeme),
            .namespaceScope = owner.namespaceScope,
            .exactSpecializationPrimary = owner.exactSpecializationPrimary,
            .exactTypeArguments = owner.exactTypeArguments,
            .exactValueArguments = owner.exactValueArguments,
            .genericParameters = owner.genericParameters,
            .genericArgumentPatterns = owner.genericArgumentPatterns,
            .fields = std::move(fields),
            .staticFields = std::move(staticFields),
            .typeAliases = std::move(aliases),
            .storedReference = owner.storedReference,
            .lifecycleBoundBorrowCarrier =
                lifecycleBoundBorrowCarrierFor(openClassType(owner.id)),
            .mutexGuardBorrowCarrier = isDefaultLibraryMutexGuardCarrier(owner),
            .kind = owner.kind,
            .bases = owner.bases,
            .abstract = owner.abstract,
            .polymorphic = owner.polymorphic,
            .cAbiRecord = owner.cAbiRecord,
            .cOpaqueHandle = owner.cOpaqueHandle,
            .cAbiLayout = owner.cAbiLayout,
            .unionLayout = owner.unionLayout,
            .traits = typeTraits(openClassType(owner.id)),
            .transferPolicy = owner.transferPolicy,
            .sharePolicy = owner.sharePolicy,
            .transferCondition = owner.transferCondition,
            .shareCondition = owner.shareCondition,
            .compilerPrivate = owner.compilerPrivate,
            .reflectPrivate = owner.reflectPrivate,
            .compilerCapability = owner.compilerCapability});
  }
  for (const ExactClassSpecialization &specialization :
       exactClassSpecializations) {
    semanticModel.recordExactClassSpecialization(
        specialization.primary, specialization.specialization,
        specialization.typeArguments, specialization.valueArguments);
  }
}

auto SemanticVisitor::Impl::canMaterializeProgramConstant(
    const ConstantValue &value) -> bool {
  return !std::holds_alternative<ConstantCheckedIntegerResult>(value) &&
         !std::holds_alternative<ConstantAggregateValue>(value);
}

} // namespace lang
