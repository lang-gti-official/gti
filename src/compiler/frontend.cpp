#include "gti/frontend.h"
#include "gti/ast_clone.h"

#include "diagnostic_registry.h"
#include "gti/compiled_template_body.h"
#include "gti/diagnostic.h"
#include "gti/hir.h"
#include "gti/lexer.h"
#include "gti/mir_lowering.h"
#include "gti/mir_verifier.h"
#include "gti/parser.h"
#include "gti/project_template.h"
#include "gti/semantic_analyzer.h"
#include "gti/source_loader.h"
#include "gti/source_owner.h"
#include "gti/support.h"
#include "mir_borrow_audit_diagnostics.h"
#include "mir_verification_session.h"
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace lang {
namespace {

// Desugar named cases to ordinary zero-argument functions in the declaration's
// own source unit and namespace. Parsed/cache-owned AST stays immutable. All
// argument typing, borrowing, cleanup and execution use the normal pipeline.
StmtList expandNamedTestCases(const StmtList &written, const std::string &scope,
                              const SourceGraph &graph,
                              std::unordered_set<std::string> &occupied,
                              bool &collectedNames, std::size_t &serial) {
  StmtList result;
  for (const auto &statement : written) {
    if (const auto *space = stmtAs<NamespaceDecl>(statement.get())) {
      auto children = expandNamedTestCases(
          space->declarations(), scope + space->name().lexeme + "::", graph,
          occupied, collectedNames, serial);
      if (children == space->declarations()) {
        result.push_back(statement);
        continue;
      }
      auto replacement =
          std::make_shared<NamespaceDecl>(space->name(), std::move(children));
      if (space->extent())
        replacement->setExtent(*space->extent());
      if (space->exportKeyword())
        replacement->setExportKeyword(*space->exportKeyword());
      if (space->documentationMarkdown())
        replacement->setDocumentationMarkdown(*space->documentationMarkdown());
      result.push_back(std::move(replacement));
      continue;
    }
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      std::vector<ConditionalBranch> branches;
      bool changed = false;
      for (const auto &branch : conditional->branches()) {
        ConditionalBranch copy;
        if (branch.condition)
          copy.condition = cloneCompileCondition(*branch.condition);
        copy.statements = expandNamedTestCases(
            branch.statements, scope, graph, occupied, collectedNames, serial);
        changed = changed || copy.statements != branch.statements;
        branches.push_back(std::move(copy));
      }
      if (!changed) {
        result.push_back(statement);
        continue;
      }
      auto replacement = std::make_shared<ConditionalStmt>(
          conditional->directive(), std::move(branches));
      if (conditional->extent())
        replacement->setExtent(*conditional->extent());
      result.push_back(std::move(replacement));
      continue;
    }
    result.push_back(statement);
    const auto *function = stmtAs<FunctionDecl>(statement.get());
    if (!function)
      continue;
    for (const auto &attribute : function->functionAttributes()) {
      if (attribute.name.lexeme != "test" || !attribute.argument)
        continue;
      if (!collectedNames) {
        for (const auto &unit : graph.sourceUnits())
          if (unit.tokens)
            for (const Token &token : *unit.tokens)
              if (token.kind == TokenKind::IDENTIFIER)
                occupied.insert(token.lexeme);
        collectedNames = true;
      }
      Token name = attribute.name;
      name.generated = true;
      name.kind = TokenKind::IDENTIFIER;
      do {
        name.lexeme = "gti_generated_test_case_" + std::to_string(serial++);
      } while (!occupied.insert(name.lexeme).second);
      name.sourceLength =
          static_cast<std::uint32_t>(attribute.name.lexeme.size());
      Token voidToken = name;
      voidToken.kind = TokenKind::VOID;
      voidToken.lexeme = "void";
      Token close = *attribute.testClose;
      Token callee = function->name();
      callee.position = attribute.name.position;
      callee.line = attribute.name.line;
      callee.sourceLength =
          static_cast<std::uint32_t>(attribute.name.lexeme.size());
      callee.generated = true;
      ExprList arguments;
      for (const auto &argument : attribute.testArguments)
        arguments.push_back(cloneExpr(*argument));
      // Keep the written callee identity and argument spans for diagnostics and
      // editor references; only the wrapper declaration itself is synthetic.
      auto call = std::make_unique<Call>(std::make_unique<Variable>(callee),
                                         std::vector<TypeRef>{}, close,
                                         std::move(arguments));
      StmtList body;
      body.push_back(
          std::make_shared<ExpressionStmt>(attribute.name, std::move(call)));
      auto wrapper = std::make_shared<FunctionDecl>(
          TypeRef(voidToken), name, std::vector<GenericParameter>{},
          std::vector<Parameter>{},
          std::make_unique<BlockStmt>(std::move(body)));
      wrapper->markGeneratedTestCase(
          scope + function->name().lexeme + "/" +
              std::get<std::string>(attribute.argument->literal),
          function);
      result.push_back(std::move(wrapper));
    }
  }
  return result;
}

std::string testSelectorLiteral(std::string_view value) {
  std::string result = "\"";
  for (char c : value) {
    if (c == '\\' || c == '"')
      result += '\\';
    result += c;
  }
  return result + "\"";
}

const FunctionDecl *editedFunction(const StmtList &items, std::string_view path,
                                   std::size_t start, std::size_t end) {
  for (const auto &item : items) {
    if (!item)
      continue;
    if (const auto *fn = stmtAs<FunctionDecl>(item.get())) {
      if (!fn->body() || !fn->body()->extent())
        continue;
      const auto &span = *fn->body()->extent();
      if (span.source == path && span.start < start && end < span.end)
        return fn;
    } else if (const auto *space = stmtAs<NamespaceDecl>(item.get())) {
      if (const auto *fn =
              editedFunction(space->declarations(), path, start, end))
        return fn;
    } else if (const auto *owner = stmtAs<ClassDecl>(item.get())) {
      if (const auto *fn = editedFunction(owner->members(), path, start, end))
        return fn;
    }
  }
  return nullptr;
}

// A never-before-seen identifier is a cheap candidate for lookup in the
// registered declaration environment. A spelling elsewhere in the parsed
// inputs could introduce a lexical value during body checking. Leave those
// cases to the ordinary walk. Membership alone never produces a diagnostic.
const Token *editedLookupToken(const SourceGraph &graph, EditedLookupRange edit,
                               bool &uniqueSpelling) {
  const auto *entry = graph.findUnit(graph.entryUnit());
  if (!entry || !entry->tokens)
    return nullptr;
  const auto &tokens = *entry->tokens;
  for (auto it = tokens.begin(); it != tokens.end(); ++it) {
    if (it->position > edit.start || it->kind != TokenKind::IDENTIFIER ||
        it->generated || it->position + it->lexeme.size() < edit.end)
      continue;
    if (edit.recheck && (it->position != edit.start ||
                         it->position + it->lexeme.size() != edit.end))
      continue;
    uniqueSpelling = true;
    for (const auto id : graph.compilationOrder()) {
      const auto *unit = graph.findUnit(id);
      if (!unit || !unit->tokens || unit->compiledTemplate)
        return nullptr;
      if (std::ranges::any_of(*unit->tokens, [&](const Token &other) {
            return &other != &*it && other.lexeme == it->lexeme;
          })) {
        uniqueSpelling = false;
        break;
      }
    }
    return uniqueSpelling || edit.recheck ? &*it : nullptr;
  }
  return nullptr;
}

// The source diagnostics of MIR's borrow rejections. `internal` reports that
// some error was not a classified borrow rejection.
struct MirBorrowCheck {
  std::vector<Diagnostic> diagnostics;
  bool internal = false;
};

// Whether the rejected instruction passes move-only storage as a call's
// mutable receiver: the call may reallocate or replace that storage.
const MirBody *findMirBody(const MirProgram &program, MirBodyKind kind,
                           std::size_t owner) {
  const MirBody *body = nullptr;
  const auto search = [&](const auto &instances) {
    for (const auto &candidate : instances)
      if (candidate.id == owner)
        body = &candidate.body;
  };
  if (kind == MirBodyKind::Function)
    search(program.functionInstances());
  else if (kind == MirBodyKind::Constructor)
    search(program.constructorInstances());
  else if (kind == MirBodyKind::Destructor)
    search(program.destructorInstances());
  else if (kind == MirBodyKind::Lambda)
    search(program.lambdaInstances());
  return body;
}

bool moveOnlyMutableReceiver(const MirProgram &program, MirBodyKind kind,
                             std::size_t owner, MirBlockId blockId,
                             MirInstructionId instructionId,
                             const MirBorrowRejection &borrow) {
  if (!borrow.write || borrow.instruction != MirInstructionKind::CallInput)
    return false;
  const MirBody *body = findMirBody(program, kind, owner);
  const MirBlock *block = body ? body->findBlock(blockId) : nullptr;
  if (block == nullptr)
    return false;
  for (const MirInstruction &instruction : block->instructions) {
    if (instruction.id != instructionId ||
        instruction.callInputRole != MirCallInputRole::Receiver ||
        instruction.callInputKind != HirCallInputKind::MutableBorrow)
      continue;
    MirPlaceId place = 0;
    if (instruction.loan)
      if (const MirLoan *loan = body->findLoan(*instruction.loan))
        place = loan->source;
    if (place == 0 && !instruction.operands.empty())
      place = instruction.operands.front().place;
    const MirPlace *receiver = body->findPlace(place);
    return receiver != nullptr && receiver->access == AccessMode::Mutable &&
           !receiver->traits.copyable && receiver->traits.movable;
  }
  return false;
}

std::string conflictMessage(const MirBorrowRejection &borrow,
                            bool moveOnlyReceiver = false) {
  if (moveOnlyReceiver)
    return "Mutable method cannot use move-only storage while a reference "
           "borrowed from it may still be live.";
  if (borrow.kind == MirBorrowRejectionKind::ParentUse)
    return "Cannot use a parent loan while an overlapping child reborrow may "
           "still be live.";
  if (borrow.instruction == MirInstructionKind::Borrow)
    return borrow.write || borrow.loanAccess == AccessMode::ReadOnly
               ? "Cannot create a mutable borrow while an overlapping borrow "
                 "may still be live."
               : "Cannot create a read-only borrow while an overlapping "
                 "mutable borrow may still be live.";
  // A mutable call (its receiver or a mutable argument) uses the storage a
  // live loan still borrows.
  if (borrow.write && (borrow.instruction == MirInstructionKind::CallInput ||
                       borrow.instruction == MirInstructionKind::Call))
    return "A mutable call cannot use storage while a reference borrowed "
           "from it may still be live.";
  if (borrow.instruction == MirInstructionKind::Move)
    return "Cannot move storage while an overlapping borrow may still be "
           "live.";
  if (borrow.write)
    return "Cannot assign to storage while an overlapping borrow may still "
           "be live.";
  return "Cannot read storage while an overlapping mutable borrow may still "
         "be live.";
}

constexpr const char *escapeMessage =
    "A returned borrow does not outlive the function: it depends on storage "
    "the function owns.";

MirBorrowCheck mirBorrowDiagnostics(const MirLoweringResult &mir,
                                    const MirProgram &program,
                                    const HirProgram &hir) {
  MirBorrowCheck check;
  const auto add = [&](SourceSpan span, std::string message,
                       const char *code = "GTI-S2017",
                       const MirLoweringError *explained = nullptr) {
    Diagnostic diagnostic = makeDiagnostic(code, DiagnosticPhase::Semantics,
                                           span, std::move(message));
    if (explained != nullptr) {
      if (explained->loanOrigin)
        diagnostic.related.push_back(
            {*explained->loanOrigin,
             "Retained borrow originates here." +
                 (explained->borrowedBindingName.empty()
                      ? std::string{}
                      : " This borrows '" + explained->borrowedBindingName +
                            "'.")});
      if (explained->lifecycleBound)
        diagnostic.hints.emplace_back(
            "This borrow protects a resource until its carrier is destroyed, "
            "not merely until its last read. End the carrier's scope before "
            "accessing the owner again.");
      else if (explained->laterUse) {
        const bool loopUse =
            explained->laterUse->source == diagnostic.primary.source &&
            explained->laterUse->start < diagnostic.primary.start;
        diagnostic.related.push_back(
            {*explained->laterUse,
             loopUse ? "The borrowed value can be used again here on a later "
                       "loop iteration."
                     : "The borrowed value is used later here."});
        diagnostic.hints.emplace_back(
            explained->temporaryDestroyed
                ? "Store the temporary in a named local that outlives every "
                  "use of the borrowed value."
                : "Finish using the borrowed value before changing or moving "
                  "its owner, or perform the owner operation before creating "
                  "the borrow. Choose an order that preserves the intended "
                  "effects.");
      } else if (explained->loanOrigin && !explained->temporaryDestroyed)
        diagnostic.hints.emplace_back(
            "Place the borrowed value in a narrower block when an explicit "
            "lexical end is required across control flow.");
    }
    if (std::none_of(check.diagnostics.begin(), check.diagnostics.end(),
                     [&](const Diagnostic &existing) {
                       return existing.primary == diagnostic.primary &&
                              existing.message == diagnostic.message;
                     }))
      check.diagnostics.push_back(std::move(diagnostic));
  };
  // Program-level rejections (escapes through a return) anchor at the
  // returned expression of the function body that publishes them.
  const auto escapeSpan = [&](const auto &self, MirBodyKind kind,
                              std::size_t owner, MirBlockId blockId,
                              bool anyReturn) -> std::optional<SourceSpan> {
    if (kind != MirBodyKind::Function)
      return std::nullopt;
    const MirFunctionInstance *function = nullptr;
    for (const auto &candidate : program.functionInstances())
      if (candidate.id == owner)
        function = &candidate;
    const HirFunctionInstance *source = nullptr;
    for (const auto &candidate : hir.functionInstances())
      if (candidate.id == owner)
        source = &candidate;
    const MirBlock *block =
        function ? function->body.findBlock(blockId) : nullptr;
    // An exit drop runs in the return's cleanup chain: follow its
    // normal-successor edges to the return it precedes.
    for (std::size_t step = 0;
         block != nullptr && step < function->body.blocks.size() &&
         (block->terminator.kind == MirTerminatorKind::Goto ||
          block->terminator.kind == MirTerminatorKind::Invoke);
         ++step)
      block = function->body.findBlock(block->terminator.target);
    const HirValue *returned =
        block && source ? source->body.findValue(block->terminator.hirValue)
                        : nullptr;
    // A reference return names its value through the return statement.
    if (returned == nullptr && block && source)
      if (const HirStatement *statement =
              source->body.findStatement(block->terminator.hirStatement);
          statement != nullptr && statement->value)
        returned = source->body.findValue(*statement->value);
    // A returned carrier names its value through the MIR value it returns.
    if (returned == nullptr && block && source && block->terminator.value &&
        block->terminator.value->kind == MirOperandKind::Value)
      if (const MirValue *value =
              function->body.findValue(block->terminator.value->value))
        returned = source->body.findValue(value->sourceValue);
    // An implicit carrier construction has no expression of its own; it
    // names the converted value.
    while (returned != nullptr && returned->source == nullptr &&
           returned->operands.size() == 1 && source)
      returned = source->body.findValue(returned->operands.front());
    if (returned != nullptr && returned->source != nullptr)
      if (const Token token = expressionAnchorToken(*returned->source);
          !token.lexeme.empty())
        return tokenSpan(token);
    // A rejection at an exit drop names the function's returned value.
    if (anyReturn && function != nullptr)
      for (const MirBlock &candidate : function->body.blocks)
        if (candidate.terminator.kind == MirTerminatorKind::Return &&
            candidate.id != blockId)
          if (auto span = self(self, kind, owner, candidate.id, false))
            return span;
    return std::nullopt;
  };
  // A result declared `static` names its region: an escape from it is a
  // region relationship failure, not an elision one.
  const auto staticResult = [&](MirBodyKind kind, std::size_t owner) {
    if (kind != MirBodyKind::Function)
      return false;
    for (const auto &candidate : hir.functionInstances())
      if (candidate.id == owner && candidate.source != nullptr) {
        const TypeRef &result = candidate.source->returnType();
        return result.referenceLifetime &&
               result.referenceLifetime->name.lexeme == "static";
      }
    return false;
  };
  // RFC 0004: a carrier result no input supplies is implicitly `static`.
  const auto implicitStaticResult = [&](MirBodyKind kind, std::size_t owner) {
    if (kind != MirBodyKind::Function)
      return false;
    const auto *function = program.findFunctionInstance(owner);
    return function != nullptr &&
           function->returnBorrowOrigin == BorrowOriginKind::Global &&
           !function->returnBorrowPlace &&
           function->returnType.kind != SemanticType::Reference;
  };
  const auto addEscape = [&](SourceSpan span, MirBodyKind kind,
                             std::size_t owner) {
    if (staticResult(kind, owner))
      add(span,
          "The result is declared 'static', but a return borrows storage "
          "that does not live for 'static'.",
          "GTI-S2127");
    else if (implicitStaticResult(kind, owner))
      add(span,
          "No input supplies this borrowed result, so it is implicitly "
          "'static', but a return borrows storage that does not live for "
          "'static'.",
          "GTI-S2127");
    else
      add(span, escapeMessage);
  };
  // A return relays a formal the signature does not name as its supplier.
  const auto addSupplier = [&](SourceSpan span, MirBodyKind kind,
                               std::size_t owner) {
    std::string supplier;
    std::string description = "value";
    if (kind == MirBodyKind::Function)
      for (const auto &candidate : hir.functionInstances())
        if (candidate.id == owner && candidate.source != nullptr) {
          const auto &parameters = candidate.source->parameters();
          if (candidate.returnBorrowParameter < parameters.size())
            supplier = parameters[candidate.returnBorrowParameter].name.lexeme;
          if (candidate.source->returnType().reference)
            description = "reference";
        }
    add(span,
        "Returned " + description +
            " does not relay the exact external owner dependency carried by " +
            (supplier.empty() ? std::string("its declared supplier")
                              : "parameter '" + supplier + "'") +
            ".",
        "GTI-S2085");
  };
  // A write conflict anchored at the declaration rather than in its body is
  // the function's exit drop of storage it owns. A returned borrow stays live
  // there: it depends on storage that does not outlive the call, and under a
  // `static` result that storage does not live for 'static'.
  const auto escapingLoan = [&](const MirLoweringError &error) {
    const MirBody *body =
        findMirBody(program, error.body.kind, error.body.owner);
    const MirLoan *loan = body ? body->findLoan(error.borrow.loan) : nullptr;
    return loan != nullptr && loan->escapes ? loan : nullptr;
  };
  // The escaping loan borrows a by-value parameter that carries borrowed
  // state, or a projection of it: the parameter's shell, not the owner it
  // borrows. A whole-shell borrow counts only when the body returns a
  // reference; a returned carrier value transfers the shell instead. Yields
  // the parameter's declaration.
  const auto byValueCarrierShell =
      [&](const MirLoweringError &error) -> std::optional<SourceSpan> {
    const MirLoan *loan = escapingLoan(error);
    const MirBody *body =
        findMirBody(program, error.body.kind, error.body.owner);
    const MirPlace *source = loan ? body->findPlace(loan->source) : nullptr;
    if (source == nullptr || source->root != MirPlaceRootKind::Binding ||
        (source->projections.empty() &&
         body->returnType.kind != SemanticType::Reference))
      return std::nullopt;
    for (const auto &function : program.functionInstances())
      if (function.id == error.body.owner)
        for (std::size_t index = 0; index < function.parameterBindings.size();
             ++index)
          if (function.parameterBindings[index] == source->binding)
            for (const MirPlace &place : body->places)
              if (place.root == MirPlaceRootKind::Binding &&
                  place.binding == source->binding &&
                  place.projections.empty()) {
                if (place.type.kind == SemanticType::Reference ||
                    !place.traits.containsBorrowedState)
                  return std::nullopt;
                const auto *declared = hir.findFunctionInstance(function.id);
                if (declared != nullptr && declared->source != nullptr &&
                    index < declared->source->parameters().size())
                  return tokenSpan(declared->source->parameters()[index].name);
                return *error.source;
              }
    return std::nullopt;
  };
  const auto exitDropUnderReturnedBorrow = [&](const MirLoweringError &error) {
    if (!error.borrow.write || !error.source ||
        error.body.kind != MirBodyKind::Function ||
        (!staticResult(error.body.kind, error.body.owner) &&
         !implicitStaticResult(error.body.kind, error.body.owner) &&
         escapingLoan(error) == nullptr))
      return false;
    for (const auto &candidate : hir.functionInstances())
      if (candidate.id == error.body.owner && candidate.source != nullptr) {
        const SourceSpan name = tokenSpan(candidate.source->name());
        return error.source->source == name.source &&
               error.source->start <= name.start;
      }
    return false;
  };
  // The MIR and HIR bodies a borrow rejection belongs to.
  const auto bodiesOf =
      [&](MirBodyKind kind,
          std::size_t owner) -> std::pair<const MirBody *, const HirBody *> {
    const auto find =
        [&](const auto &mirs,
            const auto &hirs) -> std::pair<const MirBody *, const HirBody *> {
      const MirBody *lowered = nullptr;
      const HirBody *source = nullptr;
      for (const auto &candidate : mirs)
        if (candidate.id == owner)
          lowered = &candidate.body;
      for (const auto &candidate : hirs)
        if (candidate.id == owner)
          source = &candidate.body;
      return {lowered, source};
    };
    switch (kind) {
    case MirBodyKind::Function:
      return find(program.functionInstances(), hir.functionInstances());
    case MirBodyKind::Constructor:
      return find(program.constructorInstances(), hir.constructorInstances());
    case MirBodyKind::Destructor:
      return find(program.destructorInstances(), hir.destructorInstances());
    case MirBodyKind::Lambda:
      return find(program.lambdaInstances(), hir.lambdaInstances());
    default:
      return {nullptr, nullptr};
    }
  };
  const auto ownerlessRequirement = [&](MirBodyKind kind, std::size_t owner,
                                        MirBlockId blockId,
                                        MirInstructionId site) {
    const auto [lowered, source] = bodiesOf(kind, owner);
    const auto *block = lowered ? lowered->findBlock(blockId) : nullptr;
    if (!block || !source)
      return false;
    for (const auto &instruction : block->instructions) {
      if (instruction.id != site)
        continue;
      const auto *value = source->findValue(
          instruction.hirValue ? instruction.hirValue : instruction.callSite);
      if (!value || !value->source)
        return false;
      const Token token = expressionAnchorToken(*value->source);
      if (token.lexeme.empty())
        return false;
      add(tokenSpan(token),
          "This storage or call requires an ownerless value, but the supplied "
          "value retains an external owner.",
          "GTI-S2017");
      return true;
    }
    return false;
  };
  // Explain only concrete storage retained by the rejected return loan. A
  // reference binding is an alias, not evidence that its referent dies here.
  const auto addExplainedEscape = [&](SourceSpan span, MirBodyKind kind,
                                      std::size_t owner,
                                      const MirBorrowRejection &rejection) {
    const std::size_t before = check.diagnostics.size();
    addEscape(span, kind, owner);
    if (check.diagnostics.size() == before)
      return;
    const auto [lowered, source] = bodiesOf(kind, owner);
    const MirLoan *loan = lowered ? lowered->findLoan(rejection.loan) : nullptr;
    if (loan == nullptr || source == nullptr)
      return;
    std::vector<MirPlaceId> places{loan->source};
    for (const auto &dependency : loan->additionalDependencies)
      places.push_back(dependency.source);
    auto &diagnostic = check.diagnostics.back();
    std::vector<HirBindingId> explained;
    for (const MirPlaceId id : places) {
      const MirPlace *place = lowered->findPlace(id);
      if (place == nullptr || place->root != MirPlaceRootKind::Binding ||
          !place->projections.empty() ||
          std::find(explained.begin(), explained.end(), place->binding) !=
              explained.end())
        continue;
      for (const HirBinding &binding : source->bindings) {
        if (binding.id != place->binding || binding.variable == nullptr ||
            binding.info.staticStorage || binding.info.threadLocal)
          continue;
        const auto root =
            std::find_if(lowered->places.begin(), lowered->places.end(),
                         [&](const MirPlace &candidate) {
                           return candidate.root == MirPlaceRootKind::Binding &&
                                  candidate.binding == place->binding &&
                                  candidate.projections.empty();
                         });
        if (root == lowered->places.end() ||
            root->type.kind == SemanticType::Reference)
          continue;
        const Token &name = binding.variable->name();
        diagnostic.related.push_back(
            {tokenSpan(name),
             "Local '" + name.lexeme +
                 "' supplies storage retained by this result "
                 "and is destroyed when the function returns."});
        explained.push_back(binding.id);
      }
    }
    if (!explained.empty())
      diagnostic.hints.emplace_back(
          "Return an owned value, or borrow storage supplied by the caller. "
          "A lifetime annotation cannot extend local storage past the return.");
  };
  // A conflict names where the live borrow it overlaps originates.
  const auto relateLoan = [&](const MirBodyAddress &address,
                              const MirBorrowRejection &borrow) {
    if (borrow.kind == MirBorrowRejectionKind::None || borrow.loan == 0 ||
        check.diagnostics.empty())
      return;
    const auto [lowered, source] = bodiesOf(address.kind, address.owner);
    const MirLoan *loan = lowered ? lowered->findLoan(borrow.loan) : nullptr;
    const HirValue *produced = loan && source && loan->producedBy != 0
                                   ? source->findValue(loan->producedBy)
                                   : nullptr;
    if (produced == nullptr || produced->source == nullptr)
      return;
    const Token token = expressionAnchorToken(*produced->source);
    if (token.lexeme.empty())
      return;
    auto &related = check.diagnostics.back().related;
    if (related.empty())
      related.push_back(
          {tokenSpan(token),
           borrow.kind == MirBorrowRejectionKind::OwnerlessRequirement
               ? "The retained borrow originates here."
               : "The overlapping borrow originates here."});
  };
  // A body's user rejection explains why its construction stopped; any
  // structural failure it caused in the same body is a consequence of that
  // rejection, not an internal error.
  std::vector<MirBodyAddress> rejectedBodies;
  for (const MirLoweringError &error : mir.errors)
    if (error.borrow.kind != MirBorrowRejectionKind::None && error.source)
      rejectedBodies.push_back(error.body);
  for (const MirLoweringError &error : mir.errors) {
    if (error.borrow.kind == MirBorrowRejectionKind::OwnerlessRequirement &&
        ownerlessRequirement(error.body.kind, error.body.owner, error.block,
                             error.instruction)) {
      relateLoan(error.body, error.borrow);
      continue;
    }
    if (error.borrow.kind == MirBorrowRejectionKind::None &&
        std::any_of(rejectedBodies.begin(), rejectedBodies.end(),
                    [&](const MirBodyAddress &body) {
                      return body.kind == error.body.kind &&
                             body.owner == error.body.owner;
                    }))
      continue;
    if (error.borrow.kind == MirBorrowRejectionKind::UnrepresentedRetention ||
        error.borrow.kind == MirBorrowRejectionKind::ErasedRetention) {
      std::optional<SourceSpan> span = error.source;
      const auto [lowered, source] =
          bodiesOf(error.body.kind, error.body.owner);
      if (!span && lowered && source)
        if (const auto *block = lowered->findBlock(error.block))
          for (const auto &instruction : block->instructions)
            if (instruction.id == error.instruction) {
              const auto *value = source->findValue(instruction.hirValue
                                                        ? instruction.hirValue
                                                        : instruction.callSite);
              if (value && value->source) {
                const Token token = expressionAnchorToken(*value->source);
                if (!token.lexeme.empty())
                  span = tokenSpan(token);
              }
            }
      if (span) {
        add(*span,
            error.borrow.kind == MirBorrowRejectionKind::ErasedRetention
                ? "This callable type cannot represent the input lifetimes "
                  "retained by its target. Use a direct call that preserves "
                  "the target's retention contract."
                : "This call stores borrowed text, but its input has no "
                  "represented "
                  "owner lifetime or proof that it is ownerless.",
            "GTI-S2045");
        continue;
      }
    }
    // The provisional program proof reports an escape without a body source.
    if ((error.borrow.kind == MirBorrowRejectionKind::Escape ||
         error.borrow.kind == MirBorrowRejectionKind::Supplier) &&
        !error.source)
      if (const auto span = escapeSpan(escapeSpan, error.body.kind,
                                       error.body.owner, error.block, true)) {
        if (error.borrow.kind == MirBorrowRejectionKind::Supplier)
          addSupplier(*span, error.body.kind, error.body.owner);
        else
          addExplainedEscape(*span, error.body.kind, error.body.owner,
                             error.borrow);
        continue;
      }
    if (error.borrow.kind == MirBorrowRejectionKind::None || !error.source) {
      check.internal = true;
      continue;
    }
    if (error.borrow.kind == MirBorrowRejectionKind::NestedCapture) {
      const std::size_t before = check.diagnostics.size();
      add(*error.source,
          "A lambda cannot borrow an enclosing lambda's by-reference "
          "capture.",
          "GTI-S2045");
      if (check.diagnostics.size() > before)
        check.diagnostics.back().hints.emplace_back(
            "Capture the original owner in the enclosing lambda by value, or "
            "pass the borrowed value to the inner lambda as a parameter.");
    } else if (error.borrow.kind ==
               MirBorrowRejectionKind::SharedConsumedGroup) {
      add(*error.source,
          "A consuming borrowed payload requires one exact active carrier "
          "with independently transferable dependencies.",
          "GTI-S2067");
    } else if (error.borrow.kind == MirBorrowRejectionKind::ExpiredDependency) {
      add(*error.source,
          "Cannot use a borrowed value after its owner or retained dependency "
          "has become unavailable.",
          "GTI-S2017", &error);
    } else if (error.borrow.kind == MirBorrowRejectionKind::Escape)
      addExplainedEscape(*error.source, error.body.kind, error.body.owner,
                         error.borrow);
    else if (exitDropUnderReturnedBorrow(error)) {
      // The exit drop sits in a cleanup block; name the return that
      // publishes the escaping loan instead.
      std::optional<SourceSpan> returned = escapeSpan(
          escapeSpan, error.body.kind, error.body.owner, error.block, false);
      if (!returned)
        if (const MirBody *body =
                findMirBody(program, error.body.kind, error.body.owner))
          for (const MirBlock &block : body->blocks)
            if (!returned &&
                block.terminator.kind == MirTerminatorKind::Return &&
                block.terminator.returnLoan == error.borrow.loan)
              returned = escapeSpan(escapeSpan, error.body.kind,
                                    error.body.owner, block.id, false);
      if (!returned)
        returned = escapeSpan(escapeSpan, error.body.kind, error.body.owner,
                              error.block, true);
      const SourceSpan span = returned.value_or(*error.source);
      // An exclusive reference result retained by a cleanup-bound carrier
      // is rejected for the carrier's cleanup, not as a shell projection.
      const MirLoan *exclusive = escapingLoan(error);
      const auto [exitBody, exitSource] =
          bodiesOf(error.body.kind, error.body.owner);
      (void)exitSource;
      const bool cleanupBoundResult =
          exclusive != nullptr && exclusive->access == AccessMode::Mutable &&
          exitBody != nullptr &&
          exitBody->returnType.kind == SemanticType::Reference &&
          std::any_of(exitBody->loans.begin(), exitBody->loans.end(),
                      [](const MirLoan &loan) { return loan.lifecycleBound; });
      const auto shell =
          staticResult(error.body.kind, error.body.owner) || cleanupBoundResult
              ? std::nullopt
              : byValueCarrierShell(error);
      if (shell) {
        const MirBody *body =
            findMirBody(program, error.body.kind, error.body.owner);
        const MirLoan *loan = escapingLoan(error);
        const MirPlace *source =
            body && loan ? body->findPlace(loan->source) : nullptr;
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2085", DiagnosticPhase::Semantics, span,
            source != nullptr && source->projections.empty()
                ? "Returned reference refers to the by-value carrier shell "
                  "rather than its external owner dependency."
                : "Returned reference is a projection of the by-value "
                  "carrier shell rather than its external owner dependency.");
        diagnostic.related.push_back(
            {*shell, "Stored owner dependency enters here."});
        diagnostic.hints.emplace_back(
            "Return the carrier value itself, or return its one exact stored "
            "referent with the same type; ordinary carrier fields and shell "
            "projections cannot escape.");
        check.diagnostics.push_back(std::move(diagnostic));
      } else {
        // Name the function-owned storage whose exit drop the returned
        // borrow cannot survive.
        const auto [lowered, source] =
            bodiesOf(error.body.kind, error.body.owner);
        const MirBlock *block =
            lowered ? lowered->findBlock(error.block) : nullptr;
        const MirInstruction *drop = nullptr;
        if (block != nullptr)
          for (const MirInstruction &instruction : block->instructions)
            if (instruction.id == error.instruction)
              drop = &instruction;
        const MirPlace *dropped = drop && drop->destination
                                      ? lowered->findPlace(*drop->destination)
                                      : nullptr;
        const Token *name = nullptr;
        if (dropped != nullptr && source != nullptr &&
            dropped->root == MirPlaceRootKind::Binding)
          for (const HirBinding &binding : source->bindings)
            if (binding.id == dropped->binding)
              name = binding.variable    ? &binding.variable->name()
                     : binding.parameter ? &binding.parameter->name
                                         : nullptr;
        // An exclusive result of a cleanup-bound carrier: the carrier's own
        // cleanup still uses the referent the result would hand out.
        const MirLoan *escaping = escapingLoan(error);
        const bool cleanupBound =
            escaping != nullptr && escaping->access == AccessMode::Mutable &&
            lowered->returnType.kind == SemanticType::Reference &&
            dropped != nullptr && dropped->root == MirPlaceRootKind::Binding &&
            std::any_of(lowered->loans.begin(), lowered->loans.end(),
                        [&](const MirLoan &loan) {
                          return loan.lifecycleBound &&
                                 std::find(
                                     loan.carriers.begin(), loan.carriers.end(),
                                     dropped->binding) != loan.carriers.end();
                        });
        const std::size_t before = check.diagnostics.size();
        if (cleanupBound)
          add(span,
              "An exclusive borrow retained by a cleanup-bound carrier cannot "
              "escape through a reference return.");
        else
          addEscape(span, error.body.kind, error.body.owner);
        if (check.diagnostics.size() > before && name != nullptr &&
            !name->lexeme.empty()) {
          auto &diagnostic = check.diagnostics.back();
          diagnostic.related.push_back(
              {tokenSpan(*name),
               "'" + name->lexeme +
                   "' is destroyed when the function returns." +
                   (cleanupBound ? " Its cleanup still uses the borrow."
                                 : "")});
          diagnostic.hints.emplace_back(
              cleanupBound
                  ? "Keep the borrow within the carrier's lifetime, or return "
                    "an owned value."
                  : "Return an owned value, or return a borrow of storage the "
                    "caller provides.");
        }
      }
    } else if (error.temporaryDestroyed) {
      add(*error.source,
          "Borrowed value is derived from temporary storage that is destroyed "
          "while the borrow may still be live.",
          "GTI-S2045", &error);
    } else {
      add(*error.source,
          conflictMessage(error.borrow,
                          moveOnlyMutableReceiver(
                              program, error.body.kind, error.body.owner,
                              error.block, error.instruction, error.borrow)),
          "GTI-S2017", &error);
      if (!error.loanOrigin)
        relateLoan(error.body, error.borrow);
    }
  }
  if (!mir.errors.empty())
    return check;
  for (const MirVerificationError &error : verifyMirProgram(program).errors) {
    if (error.message == "MIR program is marked invalid")
      continue;
    if (error.borrow.kind == MirBorrowRejectionKind::OwnerlessRequirement &&
        ownerlessRequirement(error.bodyKind, error.owner, error.block,
                             error.instruction))
      continue;
    const auto span =
        error.borrow.kind == MirBorrowRejectionKind::Escape ||
                error.borrow.kind == MirBorrowRejectionKind::Supplier
            ? escapeSpan(escapeSpan, error.bodyKind, error.owner, error.block,
                         true)
            : std::nullopt;
    if (!span) {
      check.internal = true;
      continue;
    }
    if (error.borrow.kind == MirBorrowRejectionKind::Supplier)
      addSupplier(*span, error.bodyKind, error.owner);
    else
      addExplainedEscape(*span, error.bodyKind, error.owner, error.borrow);
  }
  return check;
}

// Adds the MIR stage's elapsed time to the caller's work metrics when the
// stage returns, whichever of its exits it takes.
class MirStageTimer {
public:
  explicit MirStageTimer(FrontendWorkMetrics *metrics)
      : metrics(metrics), start(std::chrono::steady_clock::now()) {}
  MirStageTimer(const MirStageTimer &) = delete;
  MirStageTimer &operator=(const MirStageTimer &) = delete;
  ~MirStageTimer() {
    if (metrics != nullptr)
      metrics->mirMicroseconds += static_cast<std::size_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - start)
              .count());
  }

private:
  FrontendWorkMetrics *metrics;
  std::chrono::steady_clock::time_point start;
};

bool isRawAuditDiagnostic(const Diagnostic &diagnostic) {
  return diagnostic.code == "GTI-S2101" || diagnostic.code == "GTI-S2102" ||
         diagnostic.code == "GTI-S2103";
}

// The audit analyses the whole program, because application provenance flows
// through standard-library bodies, but by default it reports only what the
// application can act on. A finding located inside a compiler-trusted unit
// (the prelude or the standard library) describes that library's own
// implementation, which the library's maintainers check with the library unit
// itself as the entry; it is dropped unless that unit is this compilation's
// entry unit or `reportLibraryAuditFindings` opts in. Findings in application
// code that calls into the library are unaffected.
void dropTrustedAuditFindings(std::vector<Diagnostic> &diagnostics,
                              const SourceGraph &sourceGraph) {
  const SourceUnitId entry = sourceGraph.entryUnit();
  std::erase_if(diagnostics, [&](const Diagnostic &diagnostic) {
    if (!isRawAuditDiagnostic(diagnostic))
      return false;
    const SourceUnitId unit =
        sourceGraph.sourceUnitForPath(diagnostic.primary.source);
    return unit != entry && sourceGraph.isCompilerTrusted(unit);
  });
}

bool promoteRawAuditDiagnostics(std::vector<Diagnostic> &diagnostics,
                                RawPointerAuditMode mode) {
  if (mode != RawPointerAuditMode::Strict)
    return false;
  bool promoted = false;
  for (auto &diagnostic : diagnostics) {
    if (diagnostic.severity == DiagnosticSeverity::Warning &&
        isRawAuditDiagnostic(diagnostic)) {
      diagnostic.severity = DiagnosticSeverity::Error;
      promoted = true;
    }
  }
  return promoted;
}

// These are syntax requests, not a second declaration catalog. Semantic
// publication validates export eligibility and uses exact declaration names.
bool collectExportRequests(const StmtList &statements, SourceUnit &unit,
                           const TargetInfo &target, bool inherited = false) {
  bool any = false;
  for (const StmtPtr &statement : statements) {
    if (!statement) {
      continue;
    }
    const bool exported = inherited || statement->exportKeyword().has_value();
    if (const auto *space = stmtAs<NamespaceDecl>(statement.get())) {
      if (collectExportRequests(space->declarations(), unit, target,
                                exported) ||
          exported) {
        unit.exportedNamespaces.push_back(tokenSpan(space->name()));
        any = true;
      }
    } else if (const auto *conditional =
                   stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        any = collectExportRequests(*branch, unit, target, exported) || any;
      }
    } else if (const auto *linkage = stmtAs<ExternCDecl>(statement.get())) {
      any = collectExportRequests(linkage->declarations(), unit, target,
                                  exported) ||
            any;
    } else if (exported && statement->extent()) {
      unit.exportedDeclarations.push_back(*statement->extent());
      any = true;
    }
  }
  return any;
}

std::string syntaxCacheContext(const FrontendOptions &options) {
  ConfigurationFlags flags = options.configurationFlags;
  normalizeConfigurationFlags(flags);
  std::string result =
      options.target.arch + '\n' + options.target.vendor + '\n' +
      options.target.os + '\n' +
      std::string(executionProfileName(options.target.executionProfile)) +
      '\n' +
      (options.target.runtimeCapabilities.threads ? "threads:1" : "threads:0") +
      '\n' +
      std::string(
          targetEndiannessName(options.target.dataLayout.endianness())) +
      '\n' + std::to_string(options.target.dataLayout.pointerWidthBits());
  result += "\nsource-debug:";
  result += sourceDebugProfileName(options.sourceDebugProfile);
  for (const std::string &flag : flags) {
    result += '\n';
    result += flag;
  }
  ConfigurationFlags declaredFlags = options.declaredConfigurationFlags;
  normalizeConfigurationFlags(declaredFlags);
  for (const auto &flag : declaredFlags) {
    result += "\ndeclared-flag:" + flag;
  }
  result += '\n';
  result += options.target.dataLayout.supported() ? "layout:supported"
                                                  : "layout:unsupported";
  for (std::size_t index = 0; index < targetScalarKindCount; ++index) {
    result += '\n';
    const auto layout = options.target.dataLayout.scalarLayout(
        static_cast<TargetScalarKind>(index));
    if (!layout) {
      result += '-';
      continue;
    }
    result += std::to_string(layout->sizeBytes) + ':' +
              std::to_string(layout->abiAlignmentBytes) + ':' +
              std::to_string(layout->preferredAlignmentBytes);
  }
  return result;
}

std::string semanticCacheContext(const FrontendOptions &options) {
  // These policies are applied after parsing. Keep them in semantic reuse
  // identities without making identical imported syntax depend on whether an
  // editor opened an executable root, library fragment or test target.
  std::string result = syntaxCacheContext(options);
  result += options.requireEntryPoint ? "\nentry:required" : "\nentry:optional";
  result += options.program.testSuite ? "\ntests:enabled" : "\ntests:disabled";
  result += options.synthesizeTestEntry ? "\ntest-entry:generated"
                                        : "\ntest-entry:source-only";
  result += options.program.kind == ProgramKind::Library
                ? "\nprogram:library"
                : "\nprogram:executable";
  std::vector<std::pair<std::string, std::string>> exports;
  exports.reserve(options.program.exports.size());
  for (const EmbeddingExportRequest &exportRequest : options.program.exports) {
    exports.emplace_back(exportRequest.symbol, exportRequest.function);
  }
  std::sort(exports.begin(), exports.end());
  for (const auto &[symbol, function] : exports) {
    result += "\nexport:" + symbol + '=' + function;
  }
  return result;
}

std::uint64_t sourceGeneration(const FrontendOptions &options,
                               std::string_view path) {
  const auto found = options.sourceGenerations.find(std::string(path));
  return found == options.sourceGenerations.end() ? 0 : found->second;
}

void digestInteger(std::uint64_t &digest, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    digest ^= static_cast<unsigned char>(value >> shift);
    digest *= 1099511628211ULL;
  }
}

void digestText(std::uint64_t &digest, std::string_view value) {
  digestInteger(digest, value.size());
  for (const unsigned char byte : value) {
    digest ^= byte;
    digest *= 1099511628211ULL;
  }
}

class DependencyDigestBuilder final {
public:
  DependencyDigestBuilder(const SourceGraph &graph,
                          const FrontendOptions &options)
      : graph(graph), options(options),
        dependenciesBySource(graph.sourceUnits().size() + 1) {
    for (const SourceDependency &dependency : graph.dependencyEdges()) {
      if (dependency.source < dependenciesBySource.size()) {
        dependenciesBySource[dependency.source].push_back(&dependency);
      }
    }
  }

  [[nodiscard]] std::unordered_map<SourceUnitId, std::uint64_t> build() {
    for (const SourceUnit &unit : graph.sourceUnits()) {
      static_cast<void>(digestFor(unit.id));
    }
    return std::move(memoized);
  }

private:
  [[nodiscard]] std::uint64_t digestFor(SourceUnitId source) {
    if (const auto found = memoized.find(source); found != memoized.end()) {
      return found->second;
    }

    std::uint64_t result = 14695981039346656037ULL;
    if (!visiting.insert(source).second) {
      // SourceLoader diagnoses import cycles. Retain a deterministic marker
      // so a recovered graph cannot recurse indefinitely while computing a
      // cache identity.
      digestText(result, "dependency-cycle");
      digestInteger(result, source);
      return result;
    }

    if (source >= dependenciesBySource.size()) {
      visiting.erase(source);
      memoized.emplace(source, result);
      return result;
    }
    for (const SourceDependency *dependency : dependenciesBySource[source]) {
      const SourceUnit *target = graph.findUnit(dependency->target);
      if (target == nullptr) {
        continue;
      }
      digestInteger(result, static_cast<std::uint64_t>(dependency->kind));
      digestText(result, dependency->includeSpelling);
      digestInteger(result, dependency->includeOccurrence);
      digestText(result, target->path.string());
      digestInteger(result, sourceGeneration(options, target->path.string()));
      digestInteger(result, target->sourceDigest);
      digestInteger(result, target->syntaxDigest);
      digestInteger(result, digestFor(target->id));
    }

    visiting.erase(source);
    // Zero is reserved as the identity of a raw lexical cache row.
    if (result == 0) {
      result = 1;
    }
    memoized.emplace(source, result);
    return result;
  }

  const SourceGraph &graph;
  const FrontendOptions &options;
  std::unordered_map<SourceUnitId, std::uint64_t> memoized;
  std::unordered_set<SourceUnitId> visiting;
  std::vector<std::vector<const SourceDependency *>> dependenciesBySource;
};

FrontendUnitCacheKey unitCacheKey(const FrontendOptions &options,
                                  const SourceUnit &unit,
                                  std::string_view context,
                                  std::uint64_t dependencyDigest) {
  return {.canonicalPath = unit.path.string(),
          .sourceGeneration = sourceGeneration(options, unit.path.string()),
          .sourceDigest = unit.sourceDigest,
          .syntaxDigest = unit.syntaxDigest,
          .dependencyDigest = dependencyDigest,
          .context = std::string(context),
          .completionOffset =
              unit.entry ? options.completionOffset : std::nullopt};
}

StmtPtr embeddedArrayDeclaration(const EmbeddedFile &file) {
  const auto token = [&](TokenKind kind, std::string spelling) {
    auto result = file.storageName();
    result.kind = kind;
    result.lexeme = std::move(spelling);
    return result;
  };
  TypeRef type(token(TokenKind::UINT8, "uint8_t"));
  auto extent =
      token(TokenKind::INT_LITERAL, std::to_string(file.bytes->size()));
  extent.literal = static_cast<std::uint64_t>(file.bytes->size());
  type.arrayExtents.emplace_back(
      std::make_shared<const ArrayExtentExpr>(std::move(extent)));
  auto initializer = std::make_unique<ArrayInitializer>(
      token(TokenKind::LEFT_BRACE, "{"), ExprList{},
      token(TokenKind::RIGHT_BRACE, "}"), file.bytes);
  return std::make_shared<VariableDecl>(
      Mutability::Immutable, std::move(type), file.storageName(),
      std::move(initializer), token(TokenKind::STATIC, "static"), false,
      token(TokenKind::CONSTEXPR, "constexpr"));
}

std::size_t estimatedTokenBytes(const std::vector<Token> &tokens) {
  std::size_t result = tokens.size() * sizeof(Token);
  for (const Token &token : tokens) {
    result += token.lexeme.size() + token.source.size();
    if (const auto *text = std::get_if<std::string>(&token.literal)) {
      result += text->size();
    }
    if (token.leadingDocumentationMarkdown) {
      result += token.leadingDocumentationMarkdown->size();
    }
  }
  return result;
}

} // namespace

std::string frontendCacheContext(const FrontendOptions &options) {
  // Lexical and parsed cache rows are keyed by the syntax identity alone.
  return syntaxCacheContext(options);
}

// An importer can be checked against its already checked imports when the
// entry is the only unit outside that prefix. Implementation units can
// complete declarations across files, even without an import edge.
static bool canReuseImportedModules(const SourceGraph &graph) {
  if (!graph.sourceOwners() || !graph.moduleImplementations().empty())
    return false;
  const auto order = graph.compilationOrder();
  if (order.empty() || order.back() != graph.entryUnit())
    return false;
  for (const SourceUnit &unit : graph.sourceUnits()) {
    if (unit.entry || graph.isCompilerTrusted(unit.id))
      continue;
    if (!unit.moduleDeclaration || !unit.moduleId ||
        graph.isVisible(unit.id, graph.entryUnit()))
      return false;
  }
  return true;
}

struct SemanticPreambleBuild::State {
  SemanticPreambleKey key;
  Program prefix;
  std::size_t trustedPrefixSize = 0;
  SourceGraph sourceGraph;
  SourceManager sources;
  TargetInfo target;
  bool toolingOccurrences = true;
  bool requireDefinitions = false;
  SourceDebugProfile sourceDebugProfile = SourceDebugProfile::None;
  CompileLimits limits;
  bool reuseConstantEvaluations = true;
  bool deferConstexprBodies = true;
  std::shared_ptr<SemanticPreambleCache> cache;
  // A deferred build first looks for a preamble another build stored since
  // the analysis that deferred it missed the cache.
  bool deferred = false;
  bool ran = false;
  bool cancelled = false;
};

SemanticPreambleBuild::SemanticPreambleBuild(std::unique_ptr<State> state)
    : state(std::move(state)) {}

SemanticPreambleBuild::~SemanticPreambleBuild() = default;

const SemanticPreambleKey &SemanticPreambleBuild::key() const {
  return state->key;
}

bool SemanticPreambleBuild::cancelled() const { return state->cancelled; }

std::shared_ptr<const SemanticPreamble>
SemanticPreambleBuild::run(const std::atomic<bool> *cancellation,
                           FrontendWorkMetrics *workMetrics) {
  if (state->deferred)
    if (std::shared_ptr<const SemanticPreamble> cached =
            state->cache->lookup(state->key);
        cached &&
        semanticPreambleSupportsSourceDebug(*cached, state->sourceDebugProfile))
      return cached;
  if (state->ran)
    return nullptr;
  state->ran = true;
  const auto buildStart = std::chrono::steady_clock::now();
  const auto build = [&](Program prefix) {
    SemanticVisitor builder(state->target, &state->sourceGraph,
                            state->toolingOccurrences, nullptr, &state->sources,
                            RawPointerAuditMode::Off, state->requireDefinitions,
                            TrustedBodyAnalysis::Complete,
                            state->sourceDebugProfile, state->limits);
    builder.setConstantEvaluationReuse(state->reuseConstantEvaluations);
    builder.setConstexprBodyDeferral(state->deferConstexprBodies);
    builder.setCancellation(cancellation);
    auto preamble = builder.buildPreamble(std::move(prefix));
    state->cancelled = builder.cancelled();
    return preamble;
  };
  const auto &declarations = state->prefix.declarations();
  StmtList fallback;
  if (state->trustedPrefixSize < declarations.size())
    fallback.assign(declarations.begin(),
                    declarations.begin() +
                        static_cast<std::ptrdiff_t>(state->trustedPrefixSize));
  auto preamble = build(std::move(state->prefix));
  // An invalid imported module still benefits from the normal standard
  // library cache. Adoption resumes at the actual checked prefix length.
  if (!preamble && !state->cancelled && !fallback.empty())
    preamble = build(Program(std::move(fallback)));
  if (state->cancelled)
    return nullptr;
  state->cache->store(state->key, preamble);
  if (workMetrics != nullptr) {
    ++workMetrics->semanticPreambleBuilds;
    workMetrics->semanticPreambleBuildMicroseconds += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - buildStart)
            .count());
  }
  return preamble;
}

Frontend::Frontend(FrontendOptions options) : options(std::move(options)) {}

FrontendResult Frontend::analyze(
    const std::filesystem::path &entryPath,
    std::optional<std::string> entrySource,
    const std::vector<std::filesystem::path> &preludePaths,
    const std::unordered_map<std::string, std::string> &sourceOverrides,
    const std::vector<std::filesystem::path> &standardLibraryRoots,
    const std::vector<PackageSourceRoot> &packageSourceRoots) const {
  FrontendResult result;
  if (options.workMetrics != nullptr) {
    options.workMetrics->clear();
  }
  const std::string frontendCacheContext = syntaxCacheContext(options);
  SourceLoader sourceLoader(options.workMetrics, options.cache,
                            &options.sourceGenerations, &options.sourceDigests,
                            frontendCacheContext);
  sourceLoader.setErrorLimit(options.limits.errorLimit);
  const auto sourceLoadStart = std::chrono::steady_clock::now();
  {
    const PhaseTimeScope timeScope("gti-source-load");
    const ProgressScope loadProgress(options.progress, ProgressPhase::Loading);
    result.sourceGraph = sourceLoader.load(
        entryPath, std::move(entrySource), preludePaths, sourceOverrides,
        standardLibraryRoots, options.completionOffset, packageSourceRoots,
        options.target, options.configurationFlags,
        options.declaredConfigurationFlags);
    loadProgress.update({}, result.sourceGraph.sourceUnits().size(),
                        result.sourceGraph.sourceUnits().size());
  }
  if (options.workMetrics != nullptr) {
    options.workMetrics->sourceLoadMicroseconds = static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - sourceLoadStart)
            .count());
  }
  result.sources = sourceLoader.sources();
  append(result.diagnostics, sourceLoader.errors());
  result.sourceValid = !sourceLoader.hadError();
  result = finishAnalysis(entryPath, std::move(result));
  applyErrorLimit(result.diagnostics, options.limits.errorLimit);
  if (!errorLimitReached(result.diagnostics, options.limits.errorLimit) &&
      options.suggestIncludes && !options.completionOffset &&
      options.stopAfter != FrontendPhase::Parsing) {
    addIncludeSuggestions(result, options, entryPath, preludePaths,
                          sourceOverrides, standardLibraryRoots,
                          packageSourceRoots);
  }
  return result;
}

// Continues the canonical frontend pipeline from a graph produced by the
// compiler's SourceLoader. Build orchestration uses this entry point after
// computing a content identity for the exact loaded inputs; it does not
// maintain a second include parser or semantic representation.
FrontendResult
Frontend::analyzeLoaded(const std::filesystem::path &entryPath,
                        SourceGraph sourceGraph, SourceManager sources,
                        std::vector<Diagnostic> sourceDiagnostics,
                        bool sourceValid) const {
  FrontendResult result;
  result.sourceGraph = std::move(sourceGraph);
  result.sources = std::move(sources);
  result.diagnostics = std::move(sourceDiagnostics);
  result.sourceValid = sourceValid;
  result = finishAnalysis(entryPath, std::move(result));
  applyErrorLimit(result.diagnostics, options.limits.errorLimit);
  return result;
}

FrontendResult Frontend::finishAnalysis(const std::filesystem::path &entryPath,
                                        FrontendResult result) const {
  if (applyErrorLimit(result.diagnostics, options.limits.errorLimit))
    return result;
  if (!result.sourceValid && !options.analyzeRecoveredSourceGraph) {
    return result;
  }

  if (!options.target.supported()) {
    const SourceUnit *entry =
        result.sourceGraph.findUnit(result.sourceGraph.entryUnit());
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2062", DiagnosticPhase::Semantics,
        SourceSpan{entry == nullptr ? entryPath.string() : entry->path.string(),
                   0, 0, 1},
        "Selected target '" + options.target.arch + "-" +
            options.target.vendor + "-" + options.target.os +
            "' has no supported GTI data layout.");
    diagnostic.hints.emplace_back(
        "Select a 64-bit little-endian arm64 or x86_64 target for macOS, "
        "Linux, or Windows.");
    result.diagnostics.emplace_back(std::move(diagnostic));
    return result;
  }

  StmtList declarations;
  bool syntaxValid = true;
  const std::string syntaxContext = syntaxCacheContext(options);
  const std::unordered_map<SourceUnitId, std::uint64_t> dependencyDigests =
      DependencyDigestBuilder(result.sourceGraph, options).build();
  // The parser consumes unshared unit tokens. Retain only the candidate
  // and the entry tokens until its enclosing declaration is known.
  std::shared_ptr<const std::vector<Token>> lookupTokens;
  if (options.editedRange) {
    bool uniqueSpelling = false;
    if (const auto *token = editedLookupToken(
            result.sourceGraph, *options.editedRange, uniqueSpelling)) {
      result.editedLookup.emplace(*token, uniqueSpelling,
                                  options.editedRange->recheck);
      lookupTokens =
          result.sourceGraph.findUnit(result.sourceGraph.entryUnit())->tokens;
    }
  }
  const auto parseStart = std::chrono::steady_clock::now();
  {
    const PhaseTimeScope timeScope("gti-parse");
    const ProgressScope progress(options.progress, ProgressPhase::Parsing, {},
                                 result.sourceGraph.compilationOrder().size());
    std::size_t completed = 0;
    for (const SourceUnitId unitId : result.sourceGraph.compilationOrder()) {
      SourceUnit *unit = result.sourceGraph.findUnit(unitId);
      if (unit == nullptr) {
        continue;
      }
      if (options.progress != nullptr) {
        progress.update(unit->path.string(), completed);
      }
      unit->declarationStart = declarations.size();
      StmtList unitDeclarations;
      const auto dependencyDigest = dependencyDigests.find(unitId);
      const FrontendUnitCacheKey key =
          unitCacheKey(options, *unit, syntaxContext,
                       dependencyDigest == dependencyDigests.end()
                           ? 0
                           : dependencyDigest->second);
      std::optional<CachedParsedUnit> cached =
          options.cache && !unit->compiledTemplate
              ? options.cache->lookupParsed(key)
              : std::nullopt;
      if (cached) {
        unitDeclarations = std::move(cached->declarations);
        unit->lifetimeNames = cached->lifetimeNames;
        if (cached->completionContext) {
          result.syntaxCompletion = cached->completionContext;
        }
        appendParserDiagnostics(result.diagnostics, cached->diagnostics,
                                result.sourceGraph, unitId);
        syntaxValid = syntaxValid && !cached->hadError;
        if (options.workMetrics != nullptr) {
          options.workMetrics->parseCacheHits.push_back(unit->path.string());
        }
      } else {
        if (options.workMetrics != nullptr) {
          options.workMetrics->parsedUnits.push_back(unit->path.string());
        }
        static const std::vector<Token> noTokens;
        const std::vector<Token> &unitTokens =
            unit->tokens ? *unit->tokens : noTokens;
        std::size_t estimatedInputBytes = estimatedTokenBytes(unitTokens);
        const std::string *source = result.sources.find(unit->path.string());
        if (source != nullptr) {
          estimatedInputBytes += source->size();
        }
        // Token vectors are created as mutable objects and published as
        // const; when no cache row or other unit shares this one, the parser
        // takes it instead of copying the unit's whole token stream.
        std::vector<Token> parserTokens =
            unit->tokens.use_count() == 1
                ? std::move(const_cast<std::vector<Token> &>(*unit->tokens))
                : unitTokens;
        unit->tokens.reset();
        const auto &recipe = unit->compiledTemplate;
        if (recipe && recipe->projectSyntax) {
          parserTokens =
              projectTemplateTokens(*recipe, parserTokens, unit->path.string());
          source = &*recipe->projectSyntax;
        }
        Parser parser(std::move(parserTokens),
                      source ? std::string_view(*source) : std::string_view{});
        const std::size_t parseErrorLimit =
            options.limits.errorLimit == 0
                ? 0
                : options.limits.errorLimit -
                      errorDiagnosticCount(result.diagnostics);
        parser.setErrorLimit(parseErrorLimit);
        Program parsed = parser.parse();
        if (parser.completionContext()) {
          result.syntaxCompletion = parser.completionContext();
        }
        appendParserDiagnostics(result.diagnostics, parser.errors(),
                                result.sourceGraph, unitId);
        syntaxValid = syntaxValid && !parser.hadError();
        unitDeclarations = parsed.takeDeclarations();
        if (recipe && !parser.hadError()) {
          if (recipe->projectSyntax) {
            verifyProjectTemplate(*recipe, unitDeclarations,
                                  parser.lifetimeNames());
            result.sources.set(unit->path.string(), *recipe->projectSyntax);
          } else if (auto spelling = restoreTemplateBodies(
                         unitDeclarations, *recipe, unit->path.string())) {
            result.sources.set(unit->path.string(), std::move(*spelling));
          }
        }
        if (recipe && !recipe->projectSyntax && !parser.hadError()) {
          auto names =
              compiledTemplateLifetimeNames(*recipe, unit->path.string());
          unit->lifetimeNames =
              names.empty()
                  ? nullptr
                  : std::make_shared<const LifetimeNames>(std::move(names));
        } else {
          unit->lifetimeNames = parser.lifetimeNames().empty()
                                    ? nullptr
                                    : std::make_shared<const LifetimeNames>(
                                          parser.lifetimeNames());
        }
        StmtList embeddedDeclarations;
        for (const auto &file : unit->embeddedFiles) {
          if (!file.bytes || file.storageOffset != file.keyword.position)
            continue;
          embeddedDeclarations.push_back(embeddedArrayDeclaration(file));
          estimatedInputBytes += file.bytes->size();
        }
        unitDeclarations.insert(
            unitDeclarations.begin(),
            std::make_move_iterator(embeddedDeclarations.begin()),
            std::make_move_iterator(embeddedDeclarations.end()));
        if (options.cache && !unit->compiledTemplate &&
            !errorLimitReached(parser.errors(), parseErrorLimit)) {
          options.cache->storeParsed(
              key, estimatedInputBytes,
              {.declarations = unitDeclarations,
               .diagnostics = parser.errors(),
               .hadError = parser.hadError(),
               .completionContext = parser.completionContext(),
               .lifetimeNames = unit->lifetimeNames});
        }
      }
      unit->declarationCount = unitDeclarations.size();
      unit->exportedDeclarations.clear();
      unit->exportedNamespaces.clear();
      collectExportRequests(unitDeclarations, *unit, options.target);
      declarations.insert(declarations.end(), unitDeclarations.begin(),
                          unitDeclarations.end());
      if (applyErrorLimit(result.diagnostics, options.limits.errorLimit)) {
        // Source-unit declaration ranges still describe the completed prefix.
        // Keep that AST alive for syntax-only editor queries.
        result.program = Program(std::move(declarations));
        return result;
      }
      ++completed;
      if (options.progress != nullptr) {
        progress.update(unit->path.string(), completed);
      }
    }
  }
  if (options.workMetrics != nullptr) {
    options.workMetrics->parseMicroseconds = static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - parseStart)
            .count());
  }
  result.program = Program(std::move(declarations));
  if (result.editedLookup) {
    // Until the lexical walk reaches this declaration, a discarded branch or
    // source macro may depend on body facts that are still unavailable.
    const Token &token = result.editedLookup->token;
    result.editedLookup->function =
        editedFunction(result.program.declarations(), token.source,
                       token.position, token.position + token.lexeme.size());
    if (const auto *fn = result.editedLookup->function) {
      const auto &extent = *fn->body()->extent();
      for (auto it = lookupTokens->begin(); it != lookupTokens->end(); ++it) {
        if (it->position < extent.start || it->position >= extent.end)
          continue;
        const bool macroCall = it->kind == TokenKind::BANG &&
                               it != lookupTokens->begin() &&
                               (std::prev(it)->kind == TokenKind::IDENTIFIER ||
                                std::prev(it)->kind == TokenKind::GREATER);
        if (it->kind == TokenKind::CONSTEXPR ||
            it->kind == TokenKind::HASH_MACRO || macroCall) {
          result.editedLookup.reset();
          break;
        }
      }
    }
  }

  result.syntaxValid = syntaxValid;
  if (options.stopAfter == FrontendPhase::Parsing) {
    result.parsingComplete = true;
    return result;
  }
  return checkParsed(entryPath, std::move(result));
}

FrontendResult Frontend::analyzeParsed(const std::filesystem::path &entryPath,
                                       FrontendResult result) const {
  return analyzeParsed(entryPath, std::move(result), {});
}

FrontendResult
Frontend::analyzeParsed(const std::filesystem::path &entryPath,
                        FrontendResult result,
                        const DiagnosticCheckpoint &observer) const {
  if (!result.parsingComplete)
    return result;
  result.parsingComplete = false;
  result = checkParsed(entryPath, std::move(result), observer);
  applyErrorLimit(result.diagnostics, options.limits.errorLimit);
  return result;
}

FrontendResult
Frontend::checkParsed(const std::filesystem::path &entryPath,
                      FrontendResult result,
                      const DiagnosticCheckpoint &observer) const {
  if (!result.syntaxValid && !options.analyzeRecoveredProgram)
    return result;
  if (options.cancellation &&
      options.cancellation->load(std::memory_order_relaxed)) {
    result.cancelled = true;
    return result;
  }
  {
    StmtList expanded;
    std::unordered_set<std::string> occupied;
    bool collectedNames = false;
    std::size_t serial = 0;
    for (SourceUnitId id : result.sourceGraph.compilationOrder()) {
      SourceUnit *unit = result.sourceGraph.findUnit(id);
      if (!unit)
        continue;
      const auto first = result.program.declarations().begin() +
                         static_cast<std::ptrdiff_t>(unit->declarationStart);
      StmtList written(
          first, first + static_cast<std::ptrdiff_t>(unit->declarationCount));
      auto local = expandNamedTestCases(written, "", result.sourceGraph,
                                        occupied, collectedNames, serial);
      unit->declarationStart = expanded.size();
      unit->declarationCount = local.size();
      expanded.insert(expanded.end(), local.begin(), local.end());
    }
    result.program = Program(std::move(expanded));
  }

  // Compile limits decide which constant, macro and expansion requests
  // succeed, so every semantic reuse (preamble, instance analysis) keys on
  // them; lexing and parsing do not depend on them.
  std::string frontendCacheContext =
      semanticCacheContext(options) +
      "\nlimits:" + compileLimitsIdentity(options.limits) +
      (options.reuseConstantEvaluations ? "" : "\nconstant-reuse:off") +
      (options.deferConstexprBodies ? "" : "\nconstexpr-bodies:eager") +
      "\nsource-owners:" +
      (result.sourceGraph.sourceOwners()
           ? result.sourceGraph.sourceOwners()->resolutionIdentity()
           : std::string{});
  // Logical maps are relocatable, but a loaded file's containment, module
  // membership and compiler trust also affect semantic meaning. Never reuse
  // a preamble merely because its source bytes and public map are unchanged.
  const auto appendIdentity = [&](std::string_view value) {
    frontendCacheContext += std::to_string(value.size()) + ":";
    frontendCacheContext += value;
  };
  for (const auto &unit : result.sourceGraph.sourceUnits()) {
    appendIdentity(unit.path.generic_string());
    const auto owners = result.sourceGraph.sourceOwners();
    const auto *owner = owners ? owners->ownerForPath(unit.path) : nullptr;
    appendIdentity(owner ? owner->identity : std::string{});
    const auto *module = result.sourceGraph.findModule(unit.moduleId);
    appendIdentity(module ? module->ownerIdentity : std::string{});
    appendIdentity(module ? module->name : std::string{});
    appendIdentity(std::to_string(static_cast<int>(unit.role)));
    appendIdentity(unit.prelude ? "prelude" : "source");
  }
  // Concrete generic discovery can select bodies which were not visible to
  // declaration-time analysis. With bounded trusted-body analysis the
  // semantic visitor completes such a body while HIR runs (late demand): it
  // only appends records for that body and fills its own declaration facts,
  // and HIR re-runs any concrete instance that consulted it. When late demand
  // cannot stay equivalent to analysing the body in the pass, discovery
  // requests a fresh, more complete analysis instead.
  for (const auto &unit : result.sourceGraph.sourceUnits())
    if (unit.compiledScalarInterface) {
      frontendCacheContext += "\ncompiled-scalar:" + std::to_string(unit.id);
      for (const auto offset : unit.noAmbientStorageDeclarations)
        frontendCacheContext += ":" + std::to_string(offset);
    }
  const std::size_t preSemanticDiagnostics = result.diagnostics.size();
  // Trusted prefix preamble: the leading compiler-trusted units of the
  // compilation order, analysed once per configuration and shared through
  // the caller's SemanticPreambleCache; see SemanticPreamble.
  std::shared_ptr<const SemanticPreamble> preamble;
  std::size_t preambleDeclarations = 0;
  const bool requireDefinitions = options.requireFunctionDefinitions ||
                                  options.requireEntryPoint ||
                                  options.program.kind == ProgramKind::Library;
  if (result.sourceValid && options.preambleCache != nullptr &&
      options.rawPointerAudit == RawPointerAuditMode::Off) {
    SemanticPreambleKey key;
    key.context = frontendCacheContext + "\npreamble:" +
                  (options.toolingOccurrences ? "tooling" : "plain") +
                  (requireDefinitions ? ":definitions" : ":fragments");
    std::size_t trustedPrefixSize = 0;
    bool trustedPrefix = true;
    const bool reuseImports =
        options.lowerEntryUnitOnly &&
        options.trustedBodies == TrustedBodyAnalysis::Reachable &&
        canReuseImportedModules(result.sourceGraph);
    const StmtList &declarations = result.program.declarations();
    for (const SourceUnitId unitId : result.sourceGraph.compilationOrder()) {
      const SourceUnit *unit = result.sourceGraph.findUnit(unitId);
      if (unit == nullptr || unit->entry ||
          (!reuseImports && !result.sourceGraph.isCompilerTrusted(unitId)) ||
          unit->declarationStart != preambleDeclarations ||
          unit->declarationStart + unit->declarationCount > declarations.size())
        break;
      SemanticPreambleKey::Unit identity{.id = unitId,
                                         .path = unit->path.string(),
                                         .sourceDigest = unit->sourceDigest,
                                         .syntaxDigest = unit->syntaxDigest};
      for (std::size_t index = unit->declarationStart;
           index < unit->declarationStart + unit->declarationCount; ++index)
        identity.declarations.push_back(declarations[index]);
      key.units.push_back(std::move(identity));
      preambleDeclarations = unit->declarationStart + unit->declarationCount;
      trustedPrefix =
          trustedPrefix && result.sourceGraph.isCompilerTrusted(unitId);
      if (trustedPrefix)
        trustedPrefixSize = preambleDeclarations;
    }
    for (const SourceUnitId unitId : result.sourceGraph.compilationOrder())
      if (const SourceUnit *unit = result.sourceGraph.findUnit(unitId))
        key.graphUnits.emplace_back(unitId, unit->path.string());
    for (const SourceDependency &edge : result.sourceGraph.dependencyEdges())
      key.graphEdges.emplace_back(edge.source, edge.target);
    if (!key.units.empty()) {
      preamble = options.preambleCache->lookup(key);
      if (preamble && !semanticPreambleSupportsSourceDebug(
                          *preamble, options.sourceDebugProfile))
        preamble.reset();
      if (preamble == nullptr) {
        auto build = std::make_shared<SemanticPreambleBuild>(
            std::make_unique<SemanticPreambleBuild::State>(
                SemanticPreambleBuild::State{
                    .key = std::move(key),
                    .prefix = Program(StmtList(
                        declarations.begin(),
                        declarations.begin() +
                            static_cast<std::ptrdiff_t>(preambleDeclarations))),
                    .trustedPrefixSize = trustedPrefixSize,
                    .sourceGraph = result.sourceGraph,
                    .sources = result.sources,
                    .target = options.target,
                    .toolingOccurrences = options.toolingOccurrences,
                    .requireDefinitions = requireDefinitions,
                    .sourceDebugProfile = options.sourceDebugProfile,
                    .limits = options.limits,
                    .reuseConstantEvaluations =
                        options.reuseConstantEvaluations,
                    .deferConstexprBodies = options.deferConstexprBodies,
                    .cache = options.preambleCache,
                    .deferred = options.deferPreambleBuild}));
        if (options.deferPreambleBuild) {
          // Bounded analysis of this program reports what an adopted
          // preamble would; the caller builds it for later analyses.
          result.deferredPreambleBuild = std::move(build);
          if (options.workMetrics != nullptr)
            ++options.workMetrics->semanticPreambleDeferrals;
        } else {
          preamble = build->run(options.cancellation, options.workMetrics);
          if (build->cancelled()) {
            result.cancelled = true;
            return result;
          }
        }
      }
    }
  }
  bool reportedTypeChecking = false;
  std::unordered_set<const FunctionDecl *> requiredBodies;
  bool testDiscoveryComplete = false;
  std::unique_ptr<SemanticVisitor> semanticOwner;
  HirLoweringResult hir;
  bool includeLibraryBodies = options.lowerLibraryOnlyInstanceBodies;
  TrustedBodyAnalysis trustedBodies = options.trustedBodies;
  const auto applyAuditPolicy = [&] {
    if (!options.reportLibraryAuditFindings)
      dropTrustedAuditFindings(result.diagnostics, result.sourceGraph);
    if (promoteRawAuditDiagnostics(result.diagnostics, options.rawPointerAudit))
      result.semanticValid = false;
  };
  for (std::size_t attempt = 0;; ++attempt) {
    result.diagnostics.resize(preSemanticDiagnostics);
    CompileLimits semanticLimits = options.limits;
    if (semanticLimits.errorLimit != 0)
      semanticLimits.errorLimit -= errorDiagnosticCount(result.diagnostics);
    semanticOwner = std::make_unique<SemanticVisitor>(
        options.target, &result.sourceGraph, options.toolingOccurrences,
        options.workMetrics, &result.sources, options.rawPointerAudit,
        requireDefinitions, trustedBodies, options.sourceDebugProfile,
        semanticLimits);
    SemanticVisitor &semantic = *semanticOwner;
    semantic.requireFunctionBodies(requiredBodies);
    semantic.setEnumSwitchCaseSuggestions(options.suggestEnumSwitchCases);
    semantic.setCancellation(options.cancellation);
    semantic.setConstantEvaluationReuse(options.reuseConstantEvaluations);
    semantic.setConstexprBodyDeferral(options.deferConstexprBodies);
    if (preamble != nullptr) {
      semantic.adoptPreamble(preamble,
                             semanticPreambleDeclarationCount(*preamble));
      if (options.workMetrics != nullptr)
        ++options.workMetrics->semanticPreambleAdoptions;
    }
    if (result.sourceValid && options.instanceCache != nullptr &&
        trustedBodies == TrustedBodyAnalysis::Reachable)
      semantic.useInstanceAnalysisCache(
          options.instanceCache,
          frontendCacheContext +
              (preamble == nullptr
                   ? std::string{}
                   : "\npreamble:" +
                         std::to_string(semanticPreambleIdentity(*preamble))));
    if (options.workMetrics != nullptr && attempt == 0) {
      for (const SourceUnitId unitId : result.sourceGraph.compilationOrder()) {
        if (const SourceUnit *unit = result.sourceGraph.findUnit(unitId)) {
          options.workMetrics->semanticallyAnalyzedUnits.push_back(
              unit->path.string());
        }
      }
    }
    const auto semanticStart = std::chrono::steady_clock::now();
    {
      const PhaseTimeScope timeScope("gti-semantics");
      const ProgressScope progress(options.progress, ProgressPhase::Checking);
      if (observer && attempt == 0 && result.sourceValid &&
          result.syntaxValid) {
        result.semanticValid = semantic.check(
            result.program,
            [&](std::span<const Diagnostic> diagnostics) {
              observer(DiagnosticStage::Declaration, result, semantic.model(),
                       diagnostics);
            },
            [&] {
              if (hasErrorDiagnostics(semantic.errors()))
                observer(DiagnosticStage::Declaration, result, semantic.model(),
                         semantic.errors());
              else {
                observer(DiagnosticStage::Registered, result, semantic.model(),
                         {});
                if (const auto &candidate = result.editedLookup) {
                  const auto &token = candidate->token;
                  const auto lookup = semantic.diagnoseRegisteredLookup(
                      result.program, result.sources, token,
                      candidate->function, options.cancellation);
                  if (lookup && (lookup->empty() ? candidate->recheck
                                                 : candidate->uniqueSpelling))
                    observer(DiagnosticStage::Lookup, result, semantic.model(),
                             *lookup);
                }
              }
            });
      } else {
        result.semanticValid = semantic.check(result.program);
      }
      if (semantic.cancelled()) {
        // A superseded editor snapshot: no later phase runs and no instance
        // analysis is published.
        result.cancelled = true;
        return result;
      }
      if (result.semanticValid) {
        result.semanticValid =
            semantic.applyProgramRequest(result.program, options.program);
      }
    }
    if (options.workMetrics != nullptr) {
      options.workMetrics->semanticMicroseconds += static_cast<std::size_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - semanticStart)
              .count());
    }
    if (result.sourceValid && result.semanticValid &&
        options.program.testSuite && !testDiscoveryComplete &&
        !options.completionOffset) {
      testDiscoveryComplete = true;
      SourceUnit *entry =
          result.sourceGraph.findUnit(result.sourceGraph.entryUnit());
      std::unordered_map<std::string, std::string> caseFunctions;
      for (const FunctionInfo *function : semantic.model().testFunctions()) {
        const SourceUnit *unit =
            result.sourceGraph.findUnit(function->sourceUnit);
        if (entry != nullptr && unit != nullptr &&
            !result.sourceGraph.isCompilerTrusted(unit->id) &&
            unit->packageIdentity == entry->packageIdentity) {
          const std::string name =
              function->declaration->generatedTestCaseName().empty()
                  ? function->qualifiedName
                  : function->declaration->generatedTestCaseName();
          if (!caseFunctions.emplace(name, function->qualifiedName).second) {
            result.diagnostics.push_back(
                makeDiagnostic(DiagnosticId::InvalidFunctionContract,
                               function->declaration->name(),
                               {"Duplicate test identity '" + name + "'."}));
            result.semanticValid = false;
          }
          result.testCases.push_back(name);
        }
      }
      std::sort(result.testCases.begin(), result.testCases.end());
      if (result.semanticValid && !result.testCases.empty() &&
          entry != nullptr) {
        if (!semantic.model().executableEntryDiagnostic(entry->id,
                                                        entry->path.string())) {
          result.diagnostics.push_back(makeDiagnostic(
              DiagnosticId::InvalidFunctionContract,
              SourceSpan{entry->path.string(), 0, 0, 1},
              {"An annotated test suite supplies its own entry point; remove "
               "the suite's main or use a separate legacy test target."}));
          result.semanticValid = false;
        } else if (options.synthesizeTestEntry) {
          // Generate ordinary source only after semantic discovery validates
          // declaration identities and signatures. The dispatcher goes back
          // through semantics, HIR and verified MIR, including hosted argv
          // ownership and failure cleanup; the backend has no test policy.
          const std::string *original =
              result.sources.find(entry->path.string());
          const std::string prefix =
              original == nullptr ? std::string{} : *original;
          // Generated locals must not collide with an annotated global
          // function: generated-call lookup may cross source-unit boundaries.
          const auto freshLocal = [&](std::string candidate) {
            while (std::binary_search(result.testCases.begin(),
                                      result.testCases.end(), candidate))
              candidate += '_';
            return candidate;
          };
          const std::string argcName = freshLocal("gti_test_argc");
          const std::string argvName = freshLocal("gti_test_argv");
          std::string dispatcher =
              "\nint main(int " + argcName + ", std::vector<std::string> " +
              argvName + ") {\n  if (" + argcName + " != 2) { return 2; }\n";
          for (const std::string &name : result.testCases) {
            dispatcher += "  if (" + argvName +
                          "[std::size_t(1)] == " + testSelectorLiteral(name) +
                          ") { " + caseFunctions.at(name) + "(); return 0; }\n";
          }
          dispatcher += "  return 2;\n}\n";
          const std::string expanded = prefix + dispatcher;
          Lexer lexer;
          std::vector<Token> tokens =
              lexer.scan(dispatcher, entry->path.string());
          const int lineOffset =
              static_cast<int>(std::count(prefix.begin(), prefix.end(), '\n'));
          for (Token &token : tokens) {
            token.position += prefix.size();
            token.line += lineOffset;
            token.generated = true;
          }
          Parser parser(std::move(tokens), expanded);
          Program generated = parser.parse();
          for (const StmtPtr &statement : generated.declarations()) {
            if (stmtAs<FunctionDecl>(statement.get()) != nullptr)
              static_cast<FunctionDecl *>(statement.get())
                  ->markGeneratedTestEntry();
          }
          if (lexer.hadError() || parser.hadError()) {
            result.diagnostics.push_back(
                makeDiagnostic(DiagnosticId::InvalidFunctionContract,
                               SourceSpan{entry->path.string(), 0, 0, 1},
                               {"The compiler could not construct the "
                                "annotated test dispatcher: " +
                                (parser.errors().empty()
                                     ? std::string("invalid generated tokens")
                                     : parser.errors().front().message)}));
            result.semanticValid = false;
          } else {
            StmtList statements = result.program.declarations();
            const std::size_t insertion =
                entry->declarationStart + entry->declarationCount;
            const std::size_t count = generated.declarations().size();
            statements.insert(statements.begin() +
                                  static_cast<std::ptrdiff_t>(insertion),
                              generated.declarations().begin(),
                              generated.declarations().end());
            for (SourceUnit &unit : result.sourceGraph.units) {
              if (unit.id != entry->id && unit.declarationStart >= insertion)
                unit.declarationStart += count;
            }
            entry->declarationCount += count;
            entry->sourceDigest = sourceTextDigest(expanded);
            result.sources.set(entry->path.string(), expanded);
            result.program = Program(std::move(statements));
            continue;
          }
        }
      }
    }
    append(result.diagnostics, semantic.errors());
    if (applyErrorLimit(result.diagnostics, options.limits.errorLimit))
      return result;
    std::size_t semanticDiagnosticCount = semantic.errors().size();
    // Complete the requested inference phases before applying CI policy, so
    // Strict and Warn discover the same concrete bodies and audit findings.
    const bool discoveredTestEntry = options.program.testSuite &&
                                     !options.synthesizeTestEntry &&
                                     !result.testCases.empty();
    if (options.requireEntryPoint && !discoveredTestEntry &&
        result.sourceValid && result.syntaxValid && result.semanticValid &&
        !options.completionOffset) {
      const SourceUnit *entry =
          result.sourceGraph.findUnit(result.sourceGraph.entryUnit());
      if (auto diagnostic = semantic.model().executableEntryDiagnostic(
              result.sourceGraph.entryUnit(),
              entry == nullptr ? entryPath.string() : entry->path.string())) {
        if (options.program.testSuite && result.testCases.empty()) {
          diagnostic->message = "Test target requires an annotated test "
                                "function or a global main definition.";
          diagnostic->hints.emplace_back(
              "Define [[test]] void name() {} for an automatically reported, "
              "isolated test suite.");
        }
        result.diagnostics.push_back(std::move(*diagnostic));
        result.semanticValid = false;
      }
    }
    // Request-level net (#398): a compile-time path that needed a deferred
    // body where it could not be completed leaves a need behind. Whatever
    // the request's validity, it is repeated with those bodies required,
    // under the same bounded rounds as late demand, so a missed demand site
    // costs time rather than correctness. The corpus asserts it never fires.
    if (result.sourceValid && !options.completionOffset &&
        trustedBodies == TrustedBodyAnalysis::Reachable) {
      const std::size_t previousCount = requiredBodies.size();
      for (const FunctionDecl *body : semantic.unmetBodyDemands())
        requiredBodies.insert(body);
      if (requiredBodies.size() != previousCount) {
        if (options.workMetrics != nullptr)
          ++options.workMetrics->semanticDemandRounds;
        if (attempt >= 2) {
          trustedBodies = TrustedBodyAnalysis::Complete;
          if (options.workMetrics != nullptr)
            ++options.workMetrics->semanticDemandFallbacks;
        }
        continue;
      }
    }
    if (observer && !reportedTypeChecking && result.sourceValid &&
        result.syntaxValid && !options.completionOffset) {
      observer(DiagnosticStage::TypeChecking, result, semantic.model(),
               result.diagnostics);
      reportedTypeChecking = true;
    }
    if (!result.sourceValid || !result.semanticValid ||
        options.completionOffset ||
        options.stopAfter == FrontendPhase::Semantics) {
      result.semantics = semantic.takeModel();
      applyAuditPolicy();
      return result;
    }

    for (;;) {
      hir = lowerHir(result, semantic, includeLibraryBodies);
      // Late demand may report diagnostics in the bodies it analysed.
      const auto &semanticErrors = semantic.errors();
      for (std::size_t index = semanticDiagnosticCount;
           index < semanticErrors.size(); ++index)
        result.diagnostics.push_back(semanticErrors[index]);
      semanticDiagnosticCount = semanticErrors.size();
      if (errorLimitReached(hir.diagnostics, options.limits.errorLimit) ||
          errorLimitReached(result.diagnostics, options.limits.errorLimit)) {
        append(result.diagnostics, hir.diagnostics);
        applyErrorLimit(result.diagnostics, options.limits.errorLimit);
        result.semanticValid = false;
        return result;
      }
      if (semantic.lateDemandFailed()) {
        for (const FunctionDecl *body : semantic.lateDemandedBodies())
          hir.requiredSemanticBodies.push_back(body);
      }
      // The net again: concrete discovery can leave a need that late demand
      // could not meet.
      if (trustedBodies == TrustedBodyAnalysis::Reachable)
        for (const FunctionDecl *body : semantic.unmetBodyDemands())
          hir.requiredSemanticBodies.push_back(body);

      if (!hir.requiredSemanticBodies.empty() &&
          trustedBodies != TrustedBodyAnalysis::Complete)
        break;
      if (hir.requiredSemanticBodies.empty())
        semantic.publishInstanceAnalyses();

      const bool canConfirm = options.confirmSignatureOnlyMir &&
                              !includeLibraryBodies &&
                              hir.omittedInstanceBodies != 0;
      // A speculative MIR diagnostic belongs only to this lowering. Keep
      // diagnostics from the primary semantic check, including late demands.
      std::vector<Diagnostic> primaryDiagnostics;
      if (canConfirm)
        primaryDiagnostics = result.diagnostics;
      verifyLowering(entryPath, result, hir, semantic.model());
      applyAuditPolicy();
      if (canConfirm &&
          result.needsLibraryBodyConfirmation(hir.omittedInstanceBodies) &&
          !(options.cancellation &&
            options.cancellation->load(std::memory_order_relaxed))) {
        result.confirmedWithLibraryBodies = true;
        includeLibraryBodies = true;
        result.diagnostics = std::move(primaryDiagnostics);
        result.semanticValid = true;
        result.mirBorrowRejected = false;
        result.hirValid = result.failureMetadataValid = result.mirValid = false;
        result.mir = {};
        result.failureMetadata = {};
        result.hir = {};
        hir = {};
        continue;
      }
      result.semantics = semantic.takeModel();
      if (options.retainBodyQueryContext) {
        semantic.detachBodyQueryContext();
        result.bodyQueryContext = std::move(semanticOwner);
      }
      return result;
    }
    // Re-enter the existing bounded demand loop if lowering discovered a
    // primary body it could not check with this semantic environment.
    const std::size_t previousCount = requiredBodies.size();
    requiredBodies.insert(hir.requiredSemanticBodies.begin(),
                          hir.requiredSemanticBodies.end());
    if (options.workMetrics != nullptr)
      ++options.workMetrics->semanticDemandRounds;
    if (attempt >= 2 || previousCount == requiredBodies.size()) {
      trustedBodies = TrustedBodyAnalysis::Complete;
      if (options.workMetrics != nullptr)
        ++options.workMetrics->semanticDemandFallbacks;
    }
    // Destroy partial instance models before replacing their base snapshot.
    hir = {};
  }
}

HirLoweringResult Frontend::lowerHir(const FrontendResult &result,
                                     SemanticVisitor &semantic,
                                     bool includeLibraryBodies) const {
  if (options.workMetrics != nullptr)
    ++options.workMetrics->hirPasses;
  const auto hirStart = std::chrono::steady_clock::now();
  const std::size_t lateDemandStart =
      options.workMetrics == nullptr
          ? 0
          : options.workMetrics->semanticLateDemandMicroseconds;
  auto hir = [&] {
    const PhaseTimeScope timeScope("gti-hir-lowering");
    const ProgressScope progress(options.progress, ProgressPhase::HirLowering);
    HirLoweringScope scope{
        .errorLimit = options.limits.errorLimit == 0
                          ? 0
                          : options.limits.errorLimit -
                                errorDiagnosticCount(result.diagnostics),
        .reachablePrefix = options.lowerReachableOnly};
    if (options.lowerReachableOnly && !options.lowerEntryUnitOnly) {
      std::vector<std::pair<std::size_t, std::size_t>> complete;
      for (const SourceUnit &unit : result.sourceGraph.sourceUnits()) {
        if ((unit.entry || !result.sourceGraph.isCompilerTrusted(unit.id)) &&
            unit.declarationCount != 0) {
          complete.emplace_back(unit.declarationStart, unit.declarationCount);
        }
      }
      // Every application body must reach the MIR borrow checker. When
      // library declarations are lowered completely too, the application
      // units are lowered first so the check need not see the rest.
      if (options.lowerLibraryReachableOnly)
        scope.completeDeclarations = std::move(complete);
      else
        scope.borrowCheckedDeclarations = std::move(complete);
    }
    if (options.lowerEntryUnitOnly) {
      if (const SourceUnit *entry =
              result.sourceGraph.findUnit(result.sourceGraph.entryUnit())) {
        scope.entryDeclarations = {entry->declarationStart,
                                   entry->declarationCount};
      }
    }
    for (const SourceUnit &unit : result.sourceGraph.sourceUnits())
      if (result.sourceGraph.isCompilerTrusted(unit.id))
        scope.trustedSources.push_back(unit.path.string());
    // MIR lowers an omitted instance as a signature-only declaration, so
    // omission applies to analysis through HIR and through MIR alike; see
    // FrontendOptions::lowerLibraryOnlyInstanceBodies.
    if (!includeLibraryBodies) {
      scope.omitLibraryOnlyBodies = true;
      for (const SourceUnit &unit : result.sourceGraph.sourceUnits())
        if (result.sourceGraph.isCompilerTrusted(unit.id))
          scope.trustedUnits.push_back(unit.id);
    }
    return HirLowerer(options.target, scope)
        .lower(result.program, semantic.model(), semantic);
  }();
  if (options.workMetrics != nullptr) {
    // Late demand runs inside HIR discovery but is semantic work.
    const std::size_t lateDemand =
        options.workMetrics->semanticLateDemandMicroseconds - lateDemandStart;
    const auto elapsed = static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - hirStart)
            .count());
    options.workMetrics->semanticMicroseconds += lateDemand;
    options.workMetrics->hirMicroseconds +=
        elapsed > lateDemand ? elapsed - lateDemand : 0;
    options.workMetrics->hirFunctionInstances =
        hir.program.functionInstances().size();
    options.workMetrics->hirOmittedInstanceBodies = hir.omittedInstanceBodies;
  }
  return hir;
}

void Frontend::verifyLowering(const std::filesystem::path &entryPath,
                              FrontendResult &result, HirLoweringResult &hir,
                              const SemanticModel &model) const {
  result.hirValid = hir.valid();
  result.hir = std::move(hir.program);
  append(result.diagnostics, hir.diagnostics);
  if (applyErrorLimit(result.diagnostics, options.limits.errorLimit)) {
    result.semanticValid = false;
    return;
  }
  // Concrete instance discovery can demand semantic analysis while lowering
  // HIR. Its source errors are still semantic failures, not valid semantics
  // followed by an unexplained HIR failure.
  result.semanticValid =
      result.semanticValid &&
      std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                   [](const Diagnostic &diagnostic) {
                     return diagnostic.phase == DiagnosticPhase::Semantics &&
                            diagnostic.severity == DiagnosticSeverity::Error;
                   });
  if (!result.hirValid && !hasErrorDiagnostics(result.diagnostics)) {
    const SourceUnit *entry =
        result.sourceGraph.findUnit(result.sourceGraph.entryUnit());
    std::string message =
        "Internal compiler error: failed to construct valid HIR.";
    const auto verification = verifyHirProgramPlans(model, result.hir);
    constexpr std::size_t maximumReportedDetails = 5;
    for (std::size_t index = 0;
         index < verification.errors.size() && index < maximumReportedDetails;
         ++index) {
      message += " " + verification.errors[index] + ".";
    }
    result.diagnostics.push_back(makeDiagnostic(
        "GTI-B0001", DiagnosticPhase::Backend,
        SourceSpan{entry == nullptr ? entryPath.string() : entry->path.string(),
                   0, 0, 1},
        std::move(message)));
  }
  // A program the parser recovered (analyzeRecoveredProgram) stops here.
  // Semantics stays quiet about recovered nodes to avoid cascades, so their
  // bodies can be incomplete; MIR checks only programs that parsed, and the
  // parse error is the program's diagnostic.
  if (!result.syntaxValid || !result.semanticValid || !result.hirValid ||
      options.stopAfter == FrontendPhase::Hir) {
    return;
  }
  // A superseded editor snapshot stops before MIR instead of lowering bodies
  // whose diagnostics would be discarded. HIR's completed instance analyses
  // were published above and remain valid for later snapshots.
  if (options.cancellation != nullptr &&
      options.cancellation->load(std::memory_order_relaxed)) {
    result.cancelled = true;
    return;
  }
  // Executable lowering needs only what the program roots reach. Lowering the
  // complete program above has already reported every diagnostic.
  // Every application body is borrow-checked, reachable or not: MIR checks
  // the complete HIR before code generation narrows it to what roots reach.
  // MIR is also the authority for the raw-pointer audit's live-borrow finding
  // and for cross-function persistent-storage borrows (GTI-S2045); both are
  // reported from the program that covers every application body.
  const bool rawAudit = options.rawPointerAudit != RawPointerAuditMode::Off;
  bool rawAudited = false;
  // A persistent-storage rejection fails MIR verification. It is the
  // program's borrow error, anchored at the rejected invocation, not an ICE.
  const auto persistentStorageRejection = [&](MirBorrowCheck &check,
                                              const MirProgram &program,
                                              const HirProgram &source) {
    if (!check.diagnostics.empty())
      return;
    auto storage =
        mirPersistentStorageDiagnostics(program, source, entryPath.string());
    if (storage.empty())
      return;
    check.internal = false;
    check.diagnostics = std::move(storage);
  };
  if (hir.reachable && !options.retainApplicationBodies) {
    const HirProgram &checked =
        hir.borrowChecked ? *hir.borrowChecked : result.hir;
    FailureMetadataBuildResult completeMetadata =
        FailureMetadataBuilder().build(result.sourceGraph, result.sources,
                                       checked, entryPath);
    if (completeMetadata.valid()) {
      MirLoweringResult complete =
          MirLowerer().lower(checked, completeMetadata.metadata);
      if (rawAudit) {
        for (Diagnostic &diagnostic : mirRawAuditDiagnostics(
                 complete.program, checked, entryPath.string()))
          result.diagnostics.push_back(std::move(diagnostic));
        rawAudited = true;
      }
      if (!complete.valid()) {
        MirBorrowCheck check =
            mirBorrowDiagnostics(complete, complete.program, checked);
        persistentStorageRejection(check, complete.program, checked);
        const bool borrowRejection = !check.diagnostics.empty();
        // A body only this check lowers must not hide an internal failure.
        if (check.internal && check.diagnostics.empty())
          check.diagnostics.push_back(makeDiagnostic(
              "GTI-B0001", DiagnosticPhase::Backend,
              complete.errors.empty() || !complete.errors.front().source
                  ? SourceSpan{entryPath.string(), 0, 0, 1}
                  : *complete.errors.front().source,
              "Internal compiler error: failed to construct valid MIR for "
              "borrow checking. " +
                  (complete.errors.empty() ? std::string()
                                           : complete.errors.front().message) +
                  "."));
        if (!check.diagnostics.empty()) {
          if (borrowRejection) {
            result.semanticValid = false;
            result.mirBorrowRejected = true;
          }
          for (Diagnostic &diagnostic : check.diagnostics)
            result.diagnostics.push_back(std::move(diagnostic));
          // A borrow rejection is a semantic error of the program, as it is
          // when semantics checks body-local borrows.
          if (!check.internal)
            result.semanticValid = false;
          result.mirValid = false;
          return;
        }
      }
    }
  }
  if (hir.reachable && !options.retainApplicationBodies) {
    result.hir = std::move(*hir.reachable);
  }

  const MirStageTimer mirStageTimer(options.workMetrics);
  FailureMetadataBuildResult failureMetadata = [&] {
    const PhaseTimeScope timeScope("gti-failure-metadata");
    return FailureMetadataBuilder().build(result.sourceGraph, result.sources,
                                          result.hir, entryPath);
  }();
  result.failureMetadataValid = failureMetadata.valid();
  result.failureMetadata = std::move(failureMetadata.metadata);
  if (!result.failureMetadataValid) {
    const SourceUnit *entry =
        result.sourceGraph.findUnit(result.sourceGraph.entryUnit());
    std::string message =
        "Internal compiler error: failed to construct failure metadata.";
    if (!failureMetadata.errors.empty()) {
      message += " " + failureMetadata.errors.front() + ".";
    }
    result.diagnostics.push_back(makeDiagnostic(
        "GTI-B0002", DiagnosticPhase::Backend,
        SourceSpan{entry == nullptr ? entryPath.string() : entry->path.string(),
                   0, 0, 1},
        std::move(message)));
    return;
  }

  mir_detail::VerificationSession verification(
      options.mirVerificationCache.get(), options.workMetrics);
  const PhaseTimeScope mirTimeScope("gti-mir-lowering");
  ProgressScope mirProgress(options.progress, ProgressPhase::MirLowering);
  MirLoweringResult mir =
      MirLowerer(verification.enabled() ? &verification : nullptr)
          .lower(result.hir, result.failureMetadata);
  mirProgress.finish();
  if (rawAudit && !rawAudited)
    for (Diagnostic &diagnostic :
         mirRawAuditDiagnostics(mir.program, result.hir, entryPath.string()))
      result.diagnostics.push_back(std::move(diagnostic));
  result.mirValid = mir.valid();
  result.mir = std::move(mir.program);
  if (!mir.diagnostics.empty()) {
    // A borrow-checker rejection explains why the program was refused; any
    // construction failure it caused is a consequence, not an ICE.
    for (Diagnostic &diagnostic : mir.diagnostics)
      result.diagnostics.push_back(std::move(diagnostic));
    result.mirValid = false;
    return;
  }
  if (!result.mirValid) {
    // MIR is the single authority for body-local borrow validity: its borrow
    // rejections are the program's diagnostics, not internal errors.
    MirBorrowCheck check = mirBorrowDiagnostics(mir, result.mir, result.hir);
    persistentStorageRejection(check, result.mir, result.hir);
    if (!check.diagnostics.empty() && !check.internal) {
      result.semanticValid = false;
      result.mirBorrowRejected = true;
      for (Diagnostic &diagnostic : check.diagnostics)
        result.diagnostics.push_back(std::move(diagnostic));
      result.semanticValid = false;
      return;
    }
    const SourceUnit *entry =
        result.sourceGraph.findUnit(result.sourceGraph.entryUnit());
    const MirVerificationResult verification = verifyMirProgram(result.mir);
    std::string message =
        "Internal compiler error: failed to construct valid MIR.";
    // One internal failure often reports several verifier errors, and the
    // first is not always the owning cause. Report every distinct error so an
    // internal failure names its real origin instead of a later symptom.
    constexpr std::size_t maximumReportedDetails = 5;
    std::vector<std::string> details;
    SourceSpan primary{
        entry == nullptr ? entryPath.string() : entry->path.string(), 0, 0, 1};
    std::vector<RelatedDiagnostic> related;
    for (const MirLoweringError &error : mir.errors) {
      std::string detail = error.message + " (body kind " +
                           std::to_string(static_cast<int>(error.body.kind)) +
                           ", owner " + std::to_string(error.body.owner) +
                           ", block " + std::to_string(error.block) +
                           ", instruction " +
                           std::to_string(error.instruction) + ")";
      if (std::find(details.begin(), details.end(), detail) != details.end())
        continue;
      if (error.source) {
        if (details.empty())
          primary = *error.source;
        else if (*error.source != primary)
          related.push_back({*error.source, error.message});
      }
      details.push_back(std::move(detail));
      if (details.size() == maximumReportedDetails)
        break;
    }
    // Construction may stop before hosted-startup lowering. Its absence is
    // then a consequence, not the body-local rejection that caused the stop.
    for (const MirVerificationError &error : verification.errors) {
      if (!mir.errors.empty())
        break;
      if (error.message == "MIR program is marked invalid") {
        continue;
      }
      std::string detail = error.message + " (body owner " +
                           std::to_string(error.owner) + ", block " +
                           std::to_string(error.block) + ", instruction " +
                           std::to_string(error.instruction) + ")";
      if (std::find(details.begin(), details.end(), detail) != details.end()) {
        continue;
      }
      details.push_back(std::move(detail));
      if (details.size() == maximumReportedDetails) {
        break;
      }
    }
    for (const std::string &detail : details) {
      message += " " + detail + ".";
    }
    auto diagnostic = makeDiagnostic("GTI-B0001", DiagnosticPhase::Backend,
                                     std::move(primary), std::move(message));
    diagnostic.related = std::move(related);
    result.diagnostics.push_back(std::move(diagnostic));
  }
}

void Frontend::appendParserDiagnostics(
    std::vector<Diagnostic> &destination,
    const std::vector<ParseDiagnostic> &diagnostics,
    const SourceGraph &sourceGraph, SourceUnitId unit) {
  for (const ParseDiagnostic &source : diagnostics) {
    Diagnostic diagnostic = source;
    for (const SourceDependency &dependency : sourceGraph.dependencyEdges()) {
      if (dependency.target == unit && dependency.directive) {
        diagnostic.related.push_back(
            {*dependency.directive, "Imported from here."});
      }
    }
    destination.push_back(std::move(diagnostic));
  }
}

template <typename DiagnosticType>
void Frontend::append(std::vector<Diagnostic> &destination,
                      const std::vector<DiagnosticType> &source) {
  // A check that runs twice over the same syntax reports once. Any difference,
  // such as another concrete substitution, keeps both reports.
  std::unordered_multimap<std::size_t, std::size_t> byStart;
  for (std::size_t index = 0; index < destination.size(); ++index)
    byStart.emplace(destination[index].primary.start, index);
  for (const DiagnosticType &item : source) {
    Diagnostic diagnostic = item;
    const auto [first, last] = byStart.equal_range(diagnostic.primary.start);
    if (std::any_of(first, last, [&](const auto &entry) {
          return destination[entry.second] == diagnostic;
        }))
      continue;
    byStart.emplace(diagnostic.primary.start, destination.size());
    destination.push_back(std::move(diagnostic));
  }
}

EditorBodyDiagnostics::EditorBodyDiagnostics(
    const std::shared_ptr<FrontendResult> &snapshot)
    : baseline(snapshot), context(std::move(snapshot->bodyQueryContext)) {}

std::shared_ptr<const EditorBodyDiagnostics> EditorBodyDiagnostics::takeFrom(
    const std::shared_ptr<FrontendResult> &snapshot) {
  if (!snapshot)
    return {};
  if (!snapshot->bodyQueryContext || !snapshot->canGenerateCode() ||
      hasErrorDiagnostics(snapshot->diagnostics) || snapshot->cancelled) {
    snapshot->bodyQueryContext.reset();
    return {};
  }
  return std::shared_ptr<const EditorBodyDiagnostics>(
      new EditorBodyDiagnostics(snapshot));
}

namespace {
// Only the edited declaration has current statement locations. Check its
// concrete call closure, then publish solely diagnostics owned by this body.
// The background pass owns callers, program storage and cross-body findings.
std::optional<std::vector<Diagnostic>>
verifyEditedBody(const SemanticModel &model, SemanticInstanceProvider &provider,
                 FunctionId root, const BlockStmt &body,
                 const SourceGraph &graph, const SourceManager &sources,
                 const std::filesystem::path &entry,
                 const std::atomic<bool> *cancel) {
  const auto cancelled = [&] {
    return cancel && cancel->load(std::memory_order_relaxed);
  };
  if (cancelled())
    return std::nullopt;
  auto hir = HirLowerer(model.analysisSeal().target)
                 .lowerEditedFunction(model, provider, root, body);
  if (!hir.valid() || !hir.diagnostics.empty() || cancelled())
    return std::nullopt;
  auto metadata =
      FailureMetadataBuilder().build(graph, sources, hir.program, entry);
  if (!metadata.valid() || cancelled())
    return std::nullopt;
  auto mir = MirLowerer().lower(hir.program, metadata.metadata);
  if (!mir.diagnostics.empty() || cancelled())
    return std::nullopt;
  if (mir.valid())
    return std::vector<Diagnostic>{};
  if (mir.errors.empty())
    return std::nullopt;
  for (const auto &error : mir.errors) {
    const auto *instance = hir.program.findFunctionInstance(error.body.owner);
    if (error.body.kind != MirBodyKind::Function || !instance ||
        instance->declaration != root)
      return std::nullopt;
  }
  auto checked = mirBorrowDiagnostics(mir, mir.program, hir.program);
  if (checked.internal || checked.diagnostics.empty())
    return std::nullopt;
  const auto &span = *body.extent();
  const auto local = [&](const SourceSpan &site) {
    return site.source == span.source && site.start >= span.start &&
           site.end <= span.end;
  };
  for (const auto &diagnostic : checked.diagnostics) {
    if (!local(diagnostic.primary) || !diagnostic.fixes.empty())
      return std::nullopt;
    for (const auto &related : diagnostic.related)
      if (!local(related.span))
        return std::nullopt;
  }
  return checked.diagnostics;
}
} // namespace

bool EditorBodyDiagnostics::matchesSource(std::string_view source) const {
  const auto *unit =
      baseline->sourceGraph.findUnit(baseline->sourceGraph.entryUnit());
  const auto *text =
      unit ? baseline->sources.find(unit->path.string()) : nullptr;
  return text && *text == source;
}

std::optional<std::vector<Diagnostic>>
EditorBodyDiagnostics::check(const std::string &source,
                             const std::atomic<bool> *cancel,
                             bool *borrowProof) const {
  return checkEdit(source, cancel, std::nullopt, borrowProof);
}

bool EditorBodyDiagnostics::reproducesRecovery(
    const std::string &source, const std::vector<Diagnostic> &observed,
    const std::atomic<bool> *cancel) const {
  if (!std::ranges::any_of(observed, [](const Diagnostic &d) {
        return d.phase == DiagnosticPhase::Parsing;
      }))
    return false;
  const auto replay = checkEdit(source, cancel, observed.size() + 1);
  return replay && *replay == observed;
}

std::optional<std::vector<Diagnostic>> EditorBodyDiagnostics::checkEdit(
    const std::string &source, const std::atomic<bool> *cancel,
    std::optional<std::size_t> recoveryLimit, bool *borrowProof) const {
  if (borrowProof)
    *borrowProof = false;
  const std::lock_guard lock(queryMutex);
  if (cancel && cancel->load(std::memory_order_relaxed))
    return std::nullopt;
  const auto *unit =
      baseline->sourceGraph.findUnit(baseline->sourceGraph.entryUnit());
  if (!unit)
    return std::nullopt;
  const auto path = unit->path.string();
  const auto *original = baseline->sources.find(path);
  if (!original)
    return std::nullopt;
  std::size_t first = 0;
  while (first < source.size() && first < original->size() &&
         source[first] == (*original)[first])
    ++first;
  if (first == source.size() && first == original->size())
    return std::vector<Diagnostic>{};
  std::size_t past = original->size(), newPast = source.size();
  while (past > first && newPast > first &&
         (*original)[past - 1] == source[newPast - 1]) {
    --past;
    --newPast;
  }
  const auto *prior =
      editedFunction(baseline->program.declarations(), path, first, past);
  if (!prior || prior->isConstexpr() || prior->isProceduralMacro())
    return std::nullopt;
  const auto &span = *prior->body()->extent();
  if (span.end > original->size())
    return std::nullopt;
  const auto bodyEnd = source.size() - (original->size() - span.end);
  Lexer lexer;
  lexer.setErrorLimit(1);
  const auto scanned = lexer.scan(source, path);
  if (lexer.hadError() || scanned.empty())
    return std::nullopt;
  std::vector<Token> tokens;
  for (const auto &token : scanned) {
    if (token.position < span.start || token.position >= bodyEnd)
      continue;
    if (isDirectiveToken(token.kind) || token.kind == TokenKind::HASH_MACRO ||
        token.kind == TokenKind::EMBED_BYTES)
      return std::nullopt;
    tokens.push_back(token);
  }
  tokens.push_back(scanned.back());
  Parser parser(std::move(tokens), source);
  parser.setErrorLimit(recoveryLimit.value_or(1));
  auto changed = parser.parseFunctionBody();
  if (!changed || (parser.hadError() && !recoveryLimit) ||
      !parser.lifetimeNames().empty() ||
      changed->extent()->start != span.start ||
      changed->extent()->end != bodyEnd)
    return std::nullopt;

  auto sources = baseline->sources;
  sources.set(path, source);
  EditedFunctionBody input{baseline->program,
                           baseline->semantics,
                           baseline->sourceGraph,
                           sources,
                           *prior,
                           *changed,
                           {}};
  if (borrowProof && !parser.hadError()) {
    input.verify =
        [&](const SemanticModel &model, SemanticInstanceProvider &provider,
            FunctionId root) -> std::optional<std::vector<Diagnostic>> {
      auto result = verifyEditedBody(model, provider, root, *changed,
                                     input.graph, sources, unit->path, cancel);
      *borrowProof = result.has_value();
      return result;
    };
  }
  auto answer = context->diagnoseEditedBody(input, cancel);
  if (!answer || !parser.hadError())
    return answer;
  answer->insert(answer->begin(), parser.errors().begin(),
                 parser.errors().end());
  return answer;
}

} // namespace lang
