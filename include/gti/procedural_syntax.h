#pragma once

#include "gti/ast.h"
#include "gti/diagnostic.h"
#include "gti/syntax_capture.h"

#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_set>

namespace lang {

// Compiler-side capability boundary for the future verified-MIR procedural
// profile. These are NOT ordinary ConstantValue payloads or runtime objects.
class SyntaxArena;
enum class SyntaxCategory { Tokens, Expression, Span, Binding };
template <SyntaxCategory Category> class SyntaxHandle {
public:
  SyntaxHandle() = default;

private:
  friend class SyntaxArena;
  SyntaxHandle(std::uint64_t session, std::size_t generation, std::size_t node)
      : session_(session), generation_(generation), node_(node) {}
  std::uint64_t session_ = 0;
  std::size_t generation_ = 0;
  std::size_t node_ = 0;
};
using SyntaxTokens = SyntaxHandle<SyntaxCategory::Tokens>;
using SyntaxExpression = SyntaxHandle<SyntaxCategory::Expression>;
using SyntaxSpan = SyntaxHandle<SyntaxCategory::Span>;
using SyntaxBinding = SyntaxHandle<SyntaxCategory::Binding>;

struct SyntaxLimits {
  std::size_t nodes = 65536;
  std::size_t bytes = 1024 * 1024;
  std::size_t tokens = 65536;
  std::size_t depth = 256;
};

struct SyntaxBudget {
  std::size_t nodes = 0, bytes = 0, tokens = 0;
};

class SyntaxError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Owns both syntax and source provenance after its arena is gone. A sealed
// expression is syntactically valid; ordinary semantic checking is still
// mandatory. No reference into an interpreter or temporary analysis worker.
struct SealedSyntaxExpression {
  std::shared_ptr<const Expr> expression;
  std::shared_ptr<const SyntaxCapture> syntax;
  std::vector<RelatedDiagnostic> expansionTrace;
};

class SyntaxArena {
public:
  // Context IDs come from the frontend's existing MacroLookupContext registry.
  // The allocator must allocate from that SAME registry, never from spelling.
  SyntaxArena(std::uint32_t definitionContext, std::uint32_t callerContext,
              std::function<std::uint32_t()> freshContext,
              SyntaxLimits limits = {}, std::function<bool()> cancelled = {},
              std::vector<RelatedDiagnostic> expansionTrace = {},
              std::shared_ptr<SyntaxBudget> budget = {},
              std::function<void(std::size_t)> charge = {});
  ~SyntaxArena();
  SyntaxArena(const SyntaxArena &) = delete;
  SyntaxArena &operator=(const SyntaxArena &) = delete;
  SyntaxTokens capture(SyntaxCapture input);
  [[nodiscard]] std::size_t size(SyntaxTokens input) const;
  [[nodiscard]] Token token(SyntaxTokens input, std::size_t index) const;
  [[nodiscard]] std::string text(SyntaxTokens input) const;
  SyntaxTokens slice(SyntaxTokens input, std::size_t begin, std::size_t end);
  SyntaxTokens group(SyntaxTokens input, std::size_t opening);
  [[nodiscard]] bool adjacent(SyntaxTokens input, std::size_t left) const;
  SyntaxSpan span(SyntaxTokens input, std::size_t begin, std::size_t end);
  SyntaxTokens literal(std::string spelling, SyntaxSpan origin);
  SyntaxTokens punctuation(std::string spelling, SyntaxSpan origin);
  SyntaxTokens identifier(std::string spelling, SyntaxSpan origin);
  SyntaxTokens keyword(std::string spelling, SyntaxSpan origin);
  SyntaxBinding freshBinding(std::string spelling, SyntaxSpan origin);
  SyntaxTokens reference(SyntaxBinding binding);
  SyntaxTokens join(std::span<const SyntaxTokens> parts);
  SyntaxExpression parse(SyntaxTokens input);
  SyntaxTokens splice(SyntaxExpression expression);
  [[nodiscard]] SealedSyntaxExpression seal(SyntaxExpression expression) const;
  [[nodiscard]] Diagnostic
  failure(SyntaxSpan primary, std::string message,
          std::span<const SyntaxSpan> related = {}) const;
  // Cancellation/error teardown invalidates every outstanding handle. Sealed
  // output has independent ownership; aborted, unsealed nodes publish nothing.
  void reset();
  // The MIR executor temporarily installs its own shared logical-work account.
  std::function<void(std::size_t)>
  exchangeWorkCharge(std::function<void(std::size_t)> charge);

private:
  struct Node;
  const Node &get(std::uint64_t session, std::size_t generation,
                  std::size_t node, SyntaxCategory category) const;
  template <SyntaxCategory C> const Node &get(SyntaxHandle<C> handle) const {
    return get(handle.session_, handle.generation_, handle.node_, C);
  }
  std::size_t put(Node node);
  void check() const;
  SyntaxTokens constructed(std::string spelling, SyntaxSpan origin,
                           SyntaxCategory category, std::uint32_t context);
  std::uint64_t session_;
  std::size_t generation_ = 1;
  std::uint32_t definitionContext_, callerContext_;
  std::function<std::uint32_t()> freshContext_;
  SyntaxLimits limits_;
  std::function<bool()> cancelled_;
  std::shared_ptr<SyntaxBudget> budget_;
  std::function<void(std::size_t)> charge_;
  void charge(std::size_t work) const;

  std::vector<Node> nodes_;
  std::unordered_set<std::uint32_t> freshIdentities_;
  std::vector<RelatedDiagnostic> expansionTrace_;
};
} // namespace lang
