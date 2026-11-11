// RFC 0004 lifetime names.
//
// Lifetimes are erased at the syntax boundary, so their names are resolved
// lexically by the parser (gti/lifetime_names.h). This pass publishes that
// resolution: a region name no binder declares is an error, and for editor
// tooling each binder is a symbol whose written uses are its occurrences. No
// semantic type, borrow fact or lowering input is derived from it.

#include "semantic_analyzer_impl.h"

namespace lang {

auto SemanticVisitor::Impl::recordLifetimeNames(const Program &program)
    -> void {
  if (sourceGraph == nullptr)
    return;
  // A resumed analysis owns only the units after its adopted prefix; the
  // prefix's own analysis published those.
  const std::size_t first = adoptedPreamble ? adoptedPrefixDeclarations : 0;
  const std::size_t last = program.declarations().size();
  for (const SourceUnit &unit : sourceGraph->sourceUnits()) {
    if (!unit.lifetimeNames || unit.declarationStart < first ||
        unit.declarationStart + unit.declarationCount > last)
      continue;
    const LifetimeNames &names = *unit.lifetimeNames;
    for (const Token &name : names.unknown)
      report(name,
             "Unknown lifetime '" + name.lexeme +
                 "'; declare it with 'lifetime " + name.lexeme + "'.",
             "GTI-S2128");
    if (!toolingOccurrences)
      continue;
    for (const Token &binder : names.binders) {
      const SymbolId symbol =
          recordToolingSymbol(binder, SymbolKind::LifetimeParameter,
                              binder.lexeme, SemanticType::Unknown);
      semanticModel.recordOccurrence(
          {.sourceUnit = sourceUnitFor(binder),
           .span = tokenSpan(binder),
           .kind = SemanticOccurrenceKind::Symbol,
           .symbol = symbol,
           .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
           .name = binder.lexeme});
    }
    for (const LifetimeNameUse &use : names.uses) {
      const SymbolId symbol = symbolForDeclaration(use.binder);
      if (symbol == 0)
        continue;
      semanticModel.recordOccurrence(
          {.sourceUnit = sourceUnitFor(use.name),
           .span = tokenSpan(use.name),
           .kind = SemanticOccurrenceKind::Symbol,
           .symbol = symbol,
           .roles = OccurrenceRole::Reference | OccurrenceRole::TypeUse,
           .name = use.name.lexeme});
    }
    if (!names.completion)
      continue;
    const LifetimeNameCompletion &completion = *names.completion;
    SemanticCompletionContext context{
        .kind = SemanticCompletionKind::Unqualified,
        .sourceUnit = sourceUnitFor(completion.token),
        .replacementRange = tokenSpan(completion.token),
        .prefix = completion.token.lexeme};
    std::size_t distance = 0;
    for (const Token &binder : completion.visible)
      context.candidates.push_back(
          {.kind = SemanticCompletionCandidateKind::LifetimeParameter,
           .name = binder.lexeme,
           .qualifiedName = binder.lexeme,
           .detail = "lifetime " + binder.lexeme,
           .scopeDistance = distance++,
           .symbol = symbolForDeclaration(binder)});
    for (const char *builtin : {"_", "static"})
      context.candidates.push_back(
          {.kind = SemanticCompletionCandidateKind::LifetimeParameter,
           .name = builtin,
           .qualifiedName = builtin,
           .detail = std::string("lifetime ") + builtin,
           .scopeDistance = distance});
    semanticModel.recordCompletion(std::move(context));
  }
}

} // namespace lang
