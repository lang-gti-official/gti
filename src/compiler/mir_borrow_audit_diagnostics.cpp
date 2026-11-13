#include "mir_borrow_audit_diagnostics.h"

#include "diagnostic_registry.h"
#include "gti/ast.h"
#include "mir_borrow_audits.h"

#include <algorithm>
#include <optional>

namespace lang {
namespace {

struct BodySource {
  const HirBody *body = nullptr;
  std::optional<SourceSpan> declaration;
};

BodySource hirBody(const HirProgram &hir, MirBodyAddress address) {
  const auto statement = [](const Stmt *source) -> std::optional<SourceSpan> {
    if (source != nullptr && source->extent())
      return *source->extent();
    return std::nullopt;
  };
  switch (address.kind) {
  case MirBodyKind::Function:
    for (const auto &instance : hir.functionInstances())
      if (instance.id == address.owner)
        return {&instance.body, statement(instance.source)};
    break;
  case MirBodyKind::Constructor:
    for (const auto &instance : hir.constructorInstances())
      if (instance.id == address.owner)
        return {&instance.body, statement(instance.source)};
    break;
  case MirBodyKind::Destructor:
    for (const auto &instance : hir.destructorInstances())
      if (instance.id == address.owner)
        return {&instance.body, statement(instance.source)};
    break;
  case MirBodyKind::Lambda:
    for (const auto &instance : hir.lambdaInstances())
      if (instance.id == address.owner) {
        std::optional<SourceSpan> span;
        if (instance.source != nullptr)
          if (const Token token = expressionAnchorToken(*instance.source);
              !token.lexeme.empty())
            span = tokenSpan(token);
        return {&instance.body, span};
      }
    break;
  default:
    break;
  }
  return {};
}

std::optional<SourceSpan> valueSpan(const HirBody &body, HirValueId id) {
  const HirValue *value = id == 0 ? nullptr : body.findValue(id);
  while (value != nullptr && value->source == nullptr &&
         value->operands.size() == 1)
    value = body.findValue(value->operands.front());
  if (value != nullptr && value->source != nullptr)
    if (const Token token = expressionAnchorToken(*value->source);
        !token.lexeme.empty())
      return tokenSpan(token);
  return std::nullopt;
}

std::optional<SourceSpan> statementSpan(const HirBody &body,
                                        HirStatementId id) {
  const HirStatement *statement = id == 0 ? nullptr : body.findStatement(id);
  if (statement != nullptr && statement->source != nullptr &&
      statement->source->extent())
    return *statement->source->extent();
  return std::nullopt;
}

// The instruction's own expression, its call site, the local a scope-exit
// drop destroys, its statement, then (for an expanded pack element with no
// expression) the next source-bearing instruction, then the body itself.
std::optional<SourceSpan> findingSpan(const HirProgram &hir,
                                      const MirBorrowAuditFinding &finding) {
  const BodySource source = hirBody(hir, finding.body);
  if (source.body == nullptr)
    return std::nullopt;
  const HirBody &body = *source.body;
  for (const HirValueId id : {finding.hirValue, finding.callSite})
    if (auto span = valueSpan(body, id))
      return span;
  for (const HirBinding &binding : body.bindings)
    if (finding.binding != 0 && binding.id == finding.binding &&
        binding.variable != nullptr)
      return tokenSpan(binding.variable->name());
  if (auto span = statementSpan(body, finding.hirStatement))
    return span;
  for (const HirValueId id : finding.following)
    if (auto span = valueSpan(body, id))
      return span;
  for (const HirStatementId id : finding.followingStatements)
    if (auto span = statementSpan(body, id))
      return span;
  return source.declaration;
}

std::optional<SourceSpan> loanOrigin(const MirProgram &program,
                                     const HirProgram &hir,
                                     MirBodyAddress address, MirLoanId id) {
  const MirBody *lowered = findMirBody(program, address);
  const MirLoan *loan = lowered ? lowered->findLoan(id) : nullptr;
  const BodySource source = hirBody(hir, address);
  if (loan == nullptr || source.body == nullptr)
    return std::nullopt;
  return valueSpan(*source.body, loan->producedBy);
}

void addRelated(Diagnostic &diagnostic, std::optional<SourceSpan> span,
                const char *message) {
  if (!span)
    return;
  const bool duplicate = std::any_of(
      diagnostic.related.begin(), diagnostic.related.end(),
      [&](const RelatedDiagnostic &existing) {
        return existing.message == message && existing.span == *span;
      });
  if (!duplicate)
    diagnostic.related.push_back({*span, message});
}

void addUnique(std::vector<Diagnostic> &diagnostics, Diagnostic diagnostic) {
  if (std::none_of(diagnostics.begin(), diagnostics.end(),
                   [&](const Diagnostic &existing) {
                     return existing.primary == diagnostic.primary &&
                            existing.message == diagnostic.message;
                   }))
    diagnostics.push_back(std::move(diagnostic));
}

} // namespace

std::vector<Diagnostic>
mirRawAuditDiagnostics(const MirProgram &program, const HirProgram &hir,
                       const std::string &fallbackSource) {
  std::vector<Diagnostic> diagnostics;
  for (const auto &finding : auditMirRawAccesses(program)) {
    Diagnostic diagnostic = makeDiagnostic(
        DiagnosticId::RawAuditPotential,
        findingSpan(hir, finding).value_or(SourceSpan{fallbackSource, 0, 0, 1}),
        {"Raw access may overlap a live incompatible checked borrow."});
    for (const auto loan : finding.loans)
      addRelated(diagnostic, loanOrigin(program, hir, finding.body, loan),
                 "Retained borrow originates here.");
    diagnostic.hints.emplace_back(
        "Finish using the borrowed value before accessing its storage through "
        "a raw pointer, or form the borrow after the raw access.");
    addUnique(diagnostics, std::move(diagnostic));
  }
  return diagnostics;
}

std::vector<Diagnostic>
mirPersistentStorageDiagnostics(const MirProgram &program,
                                const HirProgram &hir,
                                const std::string &fallbackSource) {
  std::vector<Diagnostic> diagnostics;
  for (const auto &finding : auditMirPersistentStorageBorrows(program)) {
    const bool unknown = finding.unknownEffects && !finding.accessBody;
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2045", DiagnosticPhase::Semantics,
        findingSpan(hir, finding).value_or(SourceSpan{fallbackSource, 0, 0, 1}),
        unknown ? "This call has unresolved GTI storage effects while a "
                  "borrow of persistent storage is live."
                : "This call accesses persistent storage through an "
                  "overlapping path while a conflicting borrow is live.");
    if (!finding.context.empty())
      diagnostic.related.push_back({diagnostic.primary, finding.context});
    if (finding.accessBody)
      if (const MirBody *body = findMirBody(program, *finding.accessBody)) {
        MirBorrowAuditFinding access{.body = *finding.accessBody,
                                     .block = finding.accessBlock,
                                     .instruction = finding.accessInstruction};
        attachMirFindingSource(*body, access);
        addRelated(diagnostic, findingSpan(hir, access),
                   "The conflicting transitive storage access occurs here.");
      }
    for (const auto loan : finding.loans)
      addRelated(diagnostic, loanOrigin(program, hir, finding.body, loan),
                 "The protected borrow originates here.");
    diagnostic.hints.emplace_back(
        "End the borrow of persistent storage before this call, or avoid "
        "accessing the same storage inside the call.");
    addUnique(diagnostics, std::move(diagnostic));
  }
  return diagnostics;
}

} // namespace lang
