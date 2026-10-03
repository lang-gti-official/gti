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
      parameterSymbols.reserve(declaration->typeParameters().size());
      for (const Token &parameter : declaration->typeParameters()) {
        parameterSymbols.emplace_back(
            recordToolingSymbol(parameter, SymbolKind::TypeParameter,
                                qualified + "::" + parameter.lexeme,
                                SemanticType::Unknown, false, false));
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
        isAtomicLibraryUnit(conceptInfo.sourceUnit) &&
        binding.name == "atomic_scalar";
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
         !trustedIteratorConstraint && !trustedFunctionalConstraint) ||
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
              return argument.arguments.empty() && !argument.pointer &&
                     !argument.outerPointer && !argument.reference &&
                     argument.arrayExtents.empty() &&
                     candidate.lexeme == argumentToken.lexeme;
            });
        if (parameter == declaration.typeParameters().end()) {
          validateType(argument);
          const SemanticType concrete = typeOf(argument);
          if (concrete == SemanticType::Unknown ||
              containsOpenTypeParameter(concrete)) {
            report(argumentToken,
                   declaration.typeParameters().size() == 1
                       ? "Concept requirements must apply to the "
                         "declaration's type parameter '" +
                             declaration.typeParameters().front().lexeme +
                             "' or a closed exact type; dependent compound "
                             "types are not supported."
                       : "Concept requirements must apply to one of the "
                         "declaration's type parameters or a closed exact "
                         "type; dependent compound types are not "
                         "supported.",
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
        for (const RegisteredConcept::StructuralPattern::Argument &argument :
             pattern.arguments) {
          if (!argument.parameter) {
            mapped.arguments.emplace_back(argument);
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
  if (type.name.segments.size() > 1) {
    TypeRef qualifier = type;
    qualifier.name.segments.pop_back();
    qualifier.arguments.clear();
    resolveTypeAliasDependencies(qualifier, scope);
  }
  if (type.name.last().kind == TokenKind::IDENTIFIER &&
      !resolveTypeParameter(type.name)) {
    if (const std::optional<ResolvedTypeAlias> dependency =
            resolveTypeAliasPath(type.name, scope)) {
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
    validateType(target, false, target.isNativeFunction());
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
        TypeAliasInfo{.sourceUnit = alias.sourceUnit,
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

auto SemanticVisitor::Impl::resolveEnumPayloads() -> void {
  const std::vector<std::string> enclosingNamespace = currentNamespace;
  for (EnumInfo &info : enums) {
    if (!info.payload || info.declaration == nullptr) {
      continue;
    }
    currentSourceUnit = info.sourceUnit;
    currentNamespace = info.namespaceScope;
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
                     .underlyingType = info.underlyingType,
                     .enumerators = std::move(modelEnumerators),
                     .payload = true,
                     .compilerPrivate = info.compilerPrivate});
  }
  currentNamespace = enclosingNamespace;
}

auto SemanticVisitor::Impl::registerClassAttributes(
    const ClassDecl &declaration) -> CapabilityPolicyRegistration {
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
                    "[[unsafe_transfer]], and [[unsafe_share]]; a passive "
                    "struct may instead opt into [[c_abi]], while an "
                    "incomplete native handle uses [[c_opaque]].");
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

  for (const Token &attribute : declaration.attributes()) {
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
                                      classDecl->name(), scope)
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
      if (trustedMemoryCapabilityScope &&
          classDecl->genericParameters().size() == 1 &&
          !classDecl->genericParameters().front().valueType &&
          !classDecl->genericParameters().front().pack) {
        if (classDecl->name().lexeme == "unique_owner") {
          compilerCapability = CompilerCapabilityTypeKind::UniqueOwner;
        } else if (classDecl->name().lexeme == "shared_owner") {
          compilerCapability = CompilerCapabilityTypeKind::SharedOwner;
        } else if (classDecl->name().lexeme == "storage") {
          compilerCapability = CompilerCapabilityTypeKind::Storage;
        }
      } else if (trustedPreludeCapabilityScope &&
                 classDecl->genericParameters().size() == 1 &&
                 !classDecl->genericParameters().front().valueType &&
                 !classDecl->genericParameters().front().pack &&
                 classDecl->name().lexeme == "prefix_storage") {
        compilerCapability = CompilerCapabilityTypeKind::PrefixStorage;
      } else if (trustedOptionalCapabilityScope &&
                 classDecl->genericParameters().size() == 1 &&
                 !classDecl->genericParameters().front().valueType &&
                 !classDecl->genericParameters().front().pack &&
                 classDecl->name().lexeme == "optional_storage") {
        compilerCapability = CompilerCapabilityTypeKind::OptionalStorage;
      } else if (trustedPreludeCapabilityScope &&
                 classDecl->genericParameters().empty() &&
                 classDecl->name().lexeme == "text_view") {
        compilerCapability = CompilerCapabilityTypeKind::TextView;
      } else if (trustedPreludeCapabilityScope &&
                 classDecl->genericParameters().empty() &&
                 classDecl->name().lexeme == "c_string") {
        compilerCapability = CompilerCapabilityTypeKind::CString;
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
        for (const Token &attribute : classDecl->attributes()) {
          report(attribute,
                 "Unions do not accept class capability or native-record "
                 "attributes.",
                 "GTI-S2066");
        }
      } else {
        capabilityPolicies = registerClassAttributes(*classDecl);
      }
      classes.push_back(
          ClassInfo{.id = id,
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
                    .transferPolicy = capabilityPolicies.transfer,
                    .sharePolicy = capabilityPolicies.share,
                    .transferPolicySource = capabilityPolicies.transferSource,
                    .sharePolicySource = capabilityPolicies.shareSource,
                    .compilerPrivate =
                        compilerPrivateDeclaration(currentSourceUnit, scope),
                    .compilerCapability = compilerCapability});
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
  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size() ||
      classInfo(type.classId).exactSpecializationPrimary != 0) {
    return type;
  }
  type.classId = selectSignatureClassDefinition(type.classId, type.arguments);
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
    classes.push_back(
        ClassInfo{.id = specializationId,
                  .sourceUnit = currentSourceUnit,
                  .declaration = declaration,
                  .name = declaration->name(),
                  .kind = declaration->kind(),
                  .namespaceScope = primary.namespaceScope,
                  .exactSpecializationPrimary = *primaryId,
                  .exactTypeArguments = typeArguments,
                  .exactValueArguments = valueArguments,
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
          .requiresUnsafe = function->unsafeAttribute().has_value(),
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
    const SemanticType &type, ClassId target,
    std::unordered_set<ClassId> &visiting,
    std::unordered_set<EnumId> &visitingEnums) const -> bool {
  if (type.kind == SemanticType::Array && type.arguments.size() == 1) {
    return byValueTypeReachesClass(type.arguments.front(), target, visiting,
                                   visitingEnums);
  }
  if (type.kind == SemanticType::Enum && type.enumId != 0 &&
      type.enumId <= enums.size()) {
    if (!visitingEnums.insert(type.enumId).second) {
      return false;
    }
    bool reaches = false;
    for (const auto &[_, enumerator] : enums[type.enumId - 1].enumerators) {
      for (const SemanticType &payload : enumerator.payloadTypes) {
        reaches = reaches || byValueTypeReachesClass(payload, target, visiting,
                                                     visitingEnums);
      }
    }
    visitingEnums.erase(type.enumId);
    return reaches;
  }
  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size()) {
    return false;
  }
  if (type.classId == target) {
    return true;
  }
  if (!visiting.insert(type.classId).second) {
    return false;
  }

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
    if (byValueTypeReachesClass(fieldType, target, visiting, visitingEnums)) {
      visiting.erase(type.classId);
      return true;
    }
  }

  visiting.erase(type.classId);
  return false;
}

auto SemanticVisitor::Impl::validateOrdinaryClassLayouts() -> void {
  for (const ClassInfo &owner : classes) {
    if (owner.declaration == nullptr ||
        owner.declaration->isForwardDeclaration() ||
        owner.kind == ClassKind::Interface || owner.kind == ClassKind::Union ||
        owner.cAbiRecord || owner.cOpaqueHandle) {
      continue;
    }

    const SemanticType ownerType = openClassType(owner.id);
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
      std::unordered_set<ClassId> visiting;
      std::unordered_set<EnumId> visitingEnums;
      if (!byValueTypeReachesClass(fieldType, owner.id, visiting,
                                   visitingEnums)) {
        continue;
      }

      recursiveLayoutFields.insert(field.declaration);
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2082", DiagnosticPhase::Semantics,
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

auto SemanticVisitor::Impl::fieldsHaveDeclarationInitializers(
    const ClassInfo &owner) -> bool {
  return std::all_of(owner.fields.begin(), owner.fields.end(),
                     [](const FieldInfo &field) {
                       return field.declaration != nullptr &&
                              field.declaration->initializer() != nullptr;
                     });
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
    const ClassInfo &owner, std::unordered_set<ClassId> &visiting) const
    -> bool {
  if (owner.kind == ClassKind::Union) {
    return owner.unionLayout.has_value();
  }
  if (!fieldsHaveDeclarationInitializers(owner)) {
    return false;
  }
  if (!visiting.insert(owner.id).second) {
    return false;
  }
  const ClassBaseTypeInfo *base = concreteBase(owner);
  if (base == nullptr || base->type.classId == 0 ||
      base->type.classId > classes.size()) {
    visiting.erase(owner.id);
    return true;
  }
  const ClassInfo &baseOwner = classInfo(base->type.classId);
  const ConstructorInfo *declared = defaultConstructor(baseOwner);
  const bool available =
      declared != nullptr
          ? declared->access == AccessModifier::Public &&
                (!requestsStructuralDefaultConstruction(*declared) ||
                 classCanUseStructuralDefaultConstruction(baseOwner, visiting))
          : classCanGenerateDefaultConstructor(baseOwner, visiting);
  visiting.erase(owner.id);
  return available;
}

auto SemanticVisitor::Impl::classCanUseStructuralDefaultConstruction(
    const ClassInfo &owner) const -> bool {
  std::unordered_set<ClassId> visiting;
  return classCanUseStructuralDefaultConstruction(owner, visiting);
}

auto SemanticVisitor::Impl::classCanGenerateDefaultConstructor(
    const ClassInfo &owner, std::unordered_set<ClassId> &visiting) const
    -> bool {
  if (defaultConstructorCandidateCount(owner) != 0) {
    return false;
  }
  return classCanUseStructuralDefaultConstruction(owner, visiting);
}

auto SemanticVisitor::Impl::classCanGenerateDefaultConstructor(
    const ClassInfo &owner) const -> bool {
  std::unordered_set<ClassId> visiting;
  return classCanGenerateDefaultConstructor(owner, visiting);
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
      "containing only public instance fields of fixed-width integer or "
      "floating types, one-level raw pointers, or other [[c_abi]] "
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
  if (type == SemanticType::Void || isCAbiScalar(type)) {
    return true;
  }
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
      if (std::any_of(dependencies.begin(), dependencies.end(),
                      [](const StoredReferencePath &path) {
                        return path.reference.referenceAccess !=
                               AccessMode::ReadOnly;
                      })) {
        report(owner.name,
               "A composed borrowed value requires read-only dependencies; "
               "multiple or mixed exclusive dependencies are not supported.",
               "GTI-S2045");
        continue;
      }
      if (owner.constructors.empty()) {
        report(owner.name,
               "A composed borrowed value requires a declared constructor "
               "that explicitly binds every borrowed field.",
               "GTI-S2045");
      }
      for (ConstructorInfo &constructor : owner.constructors) {
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
          constructor.borrowAccess = AccessMode::ReadOnly;
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
        report(field.declaration->name(),
               "A class or struct may contain only one stored-reference "
               "dependency, through direct or nested nominal fields.",
               "GTI-S2045");
      }
    }
    if (!owner.storedReference) {
      const SemanticType ownerType = SemanticType::classType(owner.id);
      const auto path = storedReferencePath(ownerType);
      if (path) {
        if (hasSourceDestructorBody(owner)) {
          report(owner.destructor->declaration->tilde(),
                 "A class carrying a stored reference cannot declare a "
                 "destructor in the current lifetime model.",
                 "GTI-S2045");
        }
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
    if (hasSourceDestructorBody(owner) &&
        !isDefaultLibraryMutexGuardCarrier(owner)) {
      report(owner.destructor->declaration->tilde(),
             "A class carrying a stored reference cannot declare a "
             "destructor in the current lifetime model.",
             "GTI-S2045");
    }
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
  if (const auto *loop = stmtAs<DoWhileStmt>(statement)) {
    collectBorrowReturnExpressions(loop->body().get(), returns);
    return;
  }
  if (const auto *loop = stmtAs<WhileStmt>(statement)) {
    collectBorrowReturnExpressions(loop->body().get(), returns);
  }
}

auto SemanticVisitor::Impl::predeclaredTextBorrowSource(
    const ExprPtr &expression, const FunctionInfo &function) const
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
  if (!intrinsic || call->arguments().size() != 1 ||
      (intrinsic->record().intrinsic != IntrinsicKind::PrefixStorageTextView &&
       intrinsic->record().intrinsic != IntrinsicKind::PrefixStorageCString &&
       intrinsic->record().intrinsic !=
           IntrinsicKind::PrefixStorageTryCString &&
       intrinsic->record().intrinsic != IntrinsicKind::Move)) {
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
    FunctionReturnBorrowSummary summary = functionReturnBorrowSummary(
        updated.returnType, updated.parameterTypes, updated.ownerClass,
        updated.staticMember, updated.intrinsic);
    if (const std::optional<FunctionReturnBorrowSummary> dynamicText =
            dynamicTextReturnBorrowSummary(updated)) {
      summary = *dynamicText;
    }
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
    std::vector<Parameter> parameters;
    std::vector<ConstructorInitializer> initializers;
    std::vector<SemanticType> types;
    std::vector<const Call *> transfers;
    bool eligible = true;
    for (const FieldInfo &entry : owner.fields) {
      const VariableDecl &field = *entry.declaration;
      const auto member = owner.members.find(field.name().lexeme);
      if (member == owner.members.end() ||
          member->second.access != AccessModifier::Public ||
          member->second.symbol.type.kind == SemanticType::Reference ||
          member->second.symbol.type.kind == SemanticType::Array ||
          typeTraits(member->second.symbol.type).containsBorrowedState) {
        eligible = false;
        break;
      }
      Token parameter = field.name();
      parameter.lexeme = "__gti_designated_" + parameter.lexeme;
      parameter.generated = true;
      parameters.emplace_back(field.type(), parameter);
      types.push_back(member->second.symbol.type);
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
    if (!eligible) {
      continue;
    }
    designatedMoves.insert(transfers.begin(), transfers.end());
    Token name = owner.name;
    name.generated = true;
    auto declaration = std::make_shared<ConstructorDecl>(
        std::nullopt, name, std::vector<GenericParameter>{},
        std::move(parameters), std::nullopt, std::move(initializers),
        std::nullopt, std::make_unique<BlockStmt>(StmtList{}));
    owner.designatedConstructor =
        ConstructorInfo{.id = nextConstructorId++,
                        .owner = owner.id,
                        .declaration = declaration.get(),
                        .parameterTypes = std::move(types),
                        .requiredParameterCount = owner.fields.size()};
    constructorGenericParameters.emplace(declaration.get(),
                                         std::vector<GenericParameterInfo>{});
    ClassLifecycleInfo lifecycle =
        *semanticModel.findClassLifecycle(*owner.declaration);
    lifecycle.constructors.push_back(*owner.designatedConstructor);
    semanticModel.record(*owner.declaration, std::move(lifecycle));
    semanticModel.designatedConstructors.push_back(std::move(declaration));
  }
}

auto SemanticVisitor::Impl::recordClassLifecycles() -> void {
  for (const ClassInfo &owner : classes) {
    if (owner.declaration == nullptr) {
      continue;
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
      .cThreadOperation = declaration.cThreadOperation,
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
      .returnBorrowParameter = declaration.returnBorrowParameter,
      .returnBorrowAccess = declaration.returnBorrowAccess,
      .returnBorrowParameters = declaration.returnBorrowParameters,
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
      candidate.returnBorrowOrigin != BorrowOriginKind::None;
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
            .compilerCapability = owner.compilerCapability});
  }
}

auto SemanticVisitor::Impl::recordClassTypes() -> void {
  for (const ClassInfo &owner : classes) {
    if (owner.declaration == nullptr) {
      continue;
    }
    std::vector<ClassFieldTypeInfo> fields;
    std::vector<ClassFieldTypeInfo> staticFields;
    fields.reserve(owner.fields.size());
    for (const FieldInfo &field : owner.fields) {
      if (field.declaration == nullptr) {
        continue;
      }
      const auto member = owner.members.find(field.declaration->name().lexeme);
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
            .storedReference = owner.storedReference,
            .lifecycleBoundBorrowCarrier =
                isDefaultLibraryMutexGuardCarrier(owner),
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
            .compilerPrivate = owner.compilerPrivate,
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
