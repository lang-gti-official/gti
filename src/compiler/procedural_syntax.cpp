#include "gti/procedural_syntax.h"

#include "gti/lexer.h"
#include "gti/parser.h"

#include <atomic>
#include <limits>

namespace lang {
struct SyntaxArena::Node {
  SyntaxCategory category = SyntaxCategory::Tokens;
  std::shared_ptr<const SyntaxCapture> syntax;
  std::shared_ptr<const Expr> expression;
  SourceSpan sourceSpan;
};
namespace {
std::atomic<std::uint64_t> nextSession{1};
bool literalKind(TokenKind kind) {
  return kind == TokenKind::INT_LITERAL || kind == TokenKind::FLOAT_LITERAL ||
         kind == TokenKind::STRING_LITERAL ||
         kind == TokenKind::CHARACTER_LITERAL || kind == TokenKind::TRUE ||
         kind == TokenKind::FALSE || kind == TokenKind::NULLPTR;
}
} // namespace

SyntaxArena::SyntaxArena(std::uint32_t definitionContext,
                         std::uint32_t callerContext,
                         std::function<std::uint32_t()> freshContext,
                         SyntaxLimits limits, std::function<bool()> cancelled,
                         std::vector<RelatedDiagnostic> expansionTrace,
                         std::shared_ptr<SyntaxBudget> budget,
                         std::function<void(std::size_t)> workCharge)
    : session_(nextSession.fetch_add(1)), definitionContext_(definitionContext),
      callerContext_(callerContext), freshContext_(std::move(freshContext)),
      limits_(limits), cancelled_(std::move(cancelled)),
      budget_(budget ? std::move(budget) : std::make_shared<SyntaxBudget>()),
      charge_(std::move(workCharge)),
      expansionTrace_(std::move(expansionTrace)) {
  if (!session_ || !definitionContext_ || !callerContext_ || !freshContext_)
    throw SyntaxError("Syntax requires valid frontend context identities.");
  if (expansionTrace_.size() > 128)
    throw SyntaxError("Syntax expansion trace limit exceeded.");
}
SyntaxArena::~SyntaxArena() = default;
void SyntaxArena::charge(std::size_t work) const {
  check();
  if (charge_)
    charge_(work);
}
std::function<void(std::size_t)>
SyntaxArena::exchangeWorkCharge(std::function<void(std::size_t)> next) {
  return std::exchange(charge_, std::move(next));
}
void SyntaxArena::check() const {
  if (cancelled_ && cancelled_())
    throw SyntaxError("Syntax request cancelled.");
}
const SyntaxArena::Node &SyntaxArena::get(std::uint64_t session,
                                          std::size_t generation,
                                          std::size_t node,
                                          SyntaxCategory category) const {
  check();
  if (session != session_ || generation != generation_ ||
      node >= nodes_.size() || nodes_[node].category != category)
    throw SyntaxError("Stale, foreign or invalid syntax handle.");
  return nodes_[node];
}
std::size_t SyntaxArena::put(Node node) {
  check();
  const auto bytes = node.syntax ? node.syntax->text.size() : 0;
  const auto tokens = node.syntax ? node.syntax->tokens.size() : 0;
  if (budget_->nodes >= limits_.nodes)
    throw SyntaxLimitError(&CompileLimits::syntaxNodes,
                           "Procedural syntax-nodes limit exceeded.");
  if (bytes > limits_.bytes - budget_->bytes)
    throw SyntaxLimitError(&CompileLimits::syntaxBytes,
                           "Procedural syntax-bytes limit exceeded.");
  if (tokens > limits_.tokens - budget_->tokens)
    throw SyntaxLimitError(&CompileLimits::syntaxTokens,
                           "Procedural syntax-tokens limit exceeded.");
  // Every construction path, including joins of individually shallow pieces,
  // must enforce the output nesting bound before publishing a handle.
  std::size_t depth = 0;
  if (node.syntax)
    for (const auto &token : node.syntax->tokens) {
      check();
      if (token.kind == TokenKind::LEFT_BRACE ||
          token.kind == TokenKind::LEFT_PAREN ||
          token.kind == TokenKind::LEFT_BRACKET) {
        if (++depth > limits_.depth)
          throw SyntaxLimitError(&CompileLimits::syntaxDepth,
                                 "Procedural syntax-depth limit exceeded.");
      } else if (token.kind == TokenKind::RIGHT_BRACE ||
                 token.kind == TokenKind::RIGHT_PAREN ||
                 token.kind == TokenKind::RIGHT_BRACKET) {
        if (depth)
          --depth;
      }
    }
  charge(1 + bytes + tokens);
  ++budget_->nodes;
  budget_->bytes += bytes;
  budget_->tokens += tokens;
  nodes_.push_back(std::move(node));
  return nodes_.size() - 1;
}
SyntaxTokens SyntaxArena::capture(SyntaxCapture input) {
  if (input.origins.empty())
    for (const auto &token : input.tokens)
      input.origins.push_back(tokenSpan(token));
  if (input.origins.size() != input.tokens.size() ||
      input.textRanges.size() != input.tokens.size())
    throw SyntaxError("Invalid syntax origin table.");
  for (auto &token : input.tokens) {
    check();
    if (syntaxDirective(token.kind))
      throw SyntaxError("Directives are forbidden in syntax input.");
    if (token.kind == TokenKind::END_OF_FILE)
      throw SyntaxError("Unexpected end token in syntax input.");
    if (!token.expansion)
      token.expansion = callerContext_;
  }
  return {session_, generation_,
          put({.syntax = std::make_shared<SyntaxCapture>(std::move(input))})};
}
std::size_t SyntaxArena::size(SyntaxTokens input) const {
  return get(input).syntax->tokens.size();
}
Token SyntaxArena::token(SyntaxTokens input, std::size_t index) const {
  const auto &tokens = get(input).syntax->tokens;
  if (index >= tokens.size())
    throw SyntaxError("Syntax token index is out of range.");
  return tokens[index];
}
std::string SyntaxArena::text(SyntaxTokens input) const {
  return get(input).syntax->text;
}
SyntaxTokens SyntaxArena::slice(SyntaxTokens input, std::size_t begin,
                                std::size_t end) {
  const auto syntax = get(input).syntax;
  if (begin > end || end > syntax->tokens.size())
    throw SyntaxError("Syntax slice is out of range.");
  SyntaxCapture result;
  result.tokens.assign(syntax->tokens.begin() + begin,
                       syntax->tokens.begin() + end);
  result.origins.assign(syntax->origins.begin() + begin,
                        syntax->origins.begin() + end);
  if (!result.tokens.empty()) {
    result.start = result.tokens.front().position;
    const auto first = syntax->textRanges[begin].first;
    const auto last = syntax->textRanges[end - 1].second;
    result.text = syntax->text.substr(first, last - first);
    for (std::size_t i = begin; i < end; ++i)
      result.textRanges.emplace_back(syntax->textRanges[i].first - first,
                                     syntax->textRanges[i].second - first);
  }
  return {session_, generation_,
          put({.syntax = std::make_shared<SyntaxCapture>(std::move(result))})};
}
SyntaxTokens SyntaxArena::group(SyntaxTokens input, std::size_t opening) {
  const auto &syntax = *get(input).syntax;
  const auto found = balancedSyntax(syntax.tokens, opening);
  if (!found.complete)
    throw SyntaxError("Expected a balanced syntax group.");
  return slice(input, opening, found.end + 1);
}
bool SyntaxArena::adjacent(SyntaxTokens input, std::size_t left) const {
  const auto &syntax = *get(input).syntax;
  if (left >= syntax.tokens.size() || syntax.tokens.size() - left < 2)
    throw SyntaxError("Syntax adjacency index is out of range.");
  return syntax.textRanges[left].second == syntax.textRanges[left + 1].first;
}
SyntaxSpan SyntaxArena::span(SyntaxTokens input, std::size_t begin,
                             std::size_t end) {
  const auto &tokens = get(input).syntax->origins;
  if (begin >= tokens.size() || end <= begin || end > tokens.size())
    throw SyntaxError("Syntax span is out of range.");
  const auto &first = tokens[begin];
  const auto &last = tokens[end - 1];
  if (first.source != last.source || first.start > last.start)
    throw SyntaxError("A diagnostic span must belong to one source interval.");
  return {
      session_, generation_,
      put({.category = SyntaxCategory::Span,
           .sourceSpan = {first.source, first.start, last.end, first.line}})};
}
SyntaxTokens SyntaxArena::constructed(std::string spelling, SyntaxSpan origin,
                                      SyntaxCategory category,
                                      std::uint32_t context) {
  const SourceSpan where = get(origin).sourceSpan;
  if (spelling.size() > limits_.bytes - budget_->bytes)
    throw SyntaxLimitError(&CompileLimits::syntaxBytes,
                           "Procedural syntax-bytes limit exceeded.");
  Lexer lexer;
  auto tokens = lexer.scan(spelling, where.source);
  if (tokens.size() != 2 || lexer.hadError())
    throw SyntaxError("Expected exactly one lexical token.");
  Token value = tokens.front();
  bool valid = false;
  if (category == SyntaxCategory::Binding)
    valid = value.kind == TokenKind::IDENTIFIER;
  else if (category == SyntaxCategory::Expression)
    valid = literalKind(value.kind);
  else if (category == SyntaxCategory::Span)
    valid = (isKeywordToken(value.kind) || isTypeKeywordToken(value.kind)) &&
            value.kind != TokenKind::UNSAFE && !syntaxDirective(value.kind) &&
            value.kind != TokenKind::HASH_MACRO;
  else
    valid = isOperatorToken(value.kind) ||
            value.kind == TokenKind::LEFT_PAREN ||
            value.kind == TokenKind::RIGHT_PAREN ||
            value.kind == TokenKind::LEFT_BRACE ||
            value.kind == TokenKind::RIGHT_BRACE ||
            value.kind == TokenKind::LEFT_BRACKET ||
            value.kind == TokenKind::RIGHT_BRACKET ||
            value.kind == TokenKind::SCOPE || value.kind == TokenKind::COMMA ||
            value.kind == TokenKind::SEMICOLON || value.kind == TokenKind::DOT;
  if (!valid)
    throw SyntaxError("Token does not match the requested syntax category.");
  if (where.end < where.start ||
      where.end - where.start > std::numeric_limits<std::uint32_t>::max())
    throw SyntaxError("Syntax diagnostic origin is out of range.");
  value.sourceLength = static_cast<std::uint32_t>(where.end - where.start);
  value.position = where.start;
  value.line = where.line;
  value.generated = true;
  value.expansion = context; // Span attribution NEVER changes lookup authority.
  auto syntax = std::make_shared<SyntaxCapture>();
  syntax->tokens.push_back(std::move(value));
  syntax->origins.push_back(where);
  syntax->text = std::move(spelling);
  syntax->textRanges.emplace_back(0, syntax->text.size());
  syntax->start = where.start;
  return {session_, generation_, put({.syntax = std::move(syntax)})};
}
SyntaxTokens SyntaxArena::literal(std::string spelling, SyntaxSpan origin) {
  return constructed(std::move(spelling), origin, SyntaxCategory::Expression,
                     definitionContext_);
}
SyntaxTokens SyntaxArena::stringLiteral(std::string_view value,
                                        SyntaxSpan origin) {
  // Bound escaped storage before allocating, and poll during large operations.
  // Spelling and diagnostic attribution are separate from lookup authority.
  std::size_t length = 2;
  for (char ch : value) {
    check();
    const bool escaped = ch == '\n' || ch == '\r' || ch == '\t' || ch == '\0' ||
                         ch == '\\' || ch == '"';
    const std::size_t width = escaped ? 2 : 1;
    if (length > limits_.bytes - budget_->bytes ||
        width > limits_.bytes - budget_->bytes - length)
      throw SyntaxLimitError(&CompileLimits::syntaxBytes,
                             "Procedural syntax-bytes limit exceeded.");
    length += width;
  }
  std::string spelling;
  spelling.reserve(length);
  spelling += '"';
  for (char ch : value) {
    check();
    switch (ch) {
    case '\n':
      spelling += "\\n";
      break;
    case '\r':
      spelling += "\\r";
      break;
    case '\t':
      spelling += "\\t";
      break;
    case '\0':
      spelling += "\\0";
      break;
    case '\\':
      spelling += "\\\\";
      break;
    case '"':
      spelling += "\\\"";
      break;
    default:
      spelling += ch;
      break;
    }
  }
  spelling += '"';
  return literal(std::move(spelling), origin);
}
SyntaxTokens SyntaxArena::punctuation(std::string spelling, SyntaxSpan origin) {
  return constructed(std::move(spelling), origin, SyntaxCategory::Tokens,
                     definitionContext_);
}
SyntaxTokens SyntaxArena::identifier(std::string spelling, SyntaxSpan origin) {
  return constructed(std::move(spelling), origin, SyntaxCategory::Binding,
                     definitionContext_);
}
SyntaxTokens SyntaxArena::keyword(std::string spelling, SyntaxSpan origin) {
  return constructed(std::move(spelling), origin, SyntaxCategory::Span,
                     definitionContext_);
}
SyntaxBinding SyntaxArena::freshBinding(std::string spelling,
                                        SyntaxSpan origin) {
  const auto identity = freshContext_();
  if (!identity || identity == definitionContext_ ||
      identity == callerContext_ || !freshIdentities_.insert(identity).second)
    throw SyntaxError("Fresh binding requires a new frontend identity.");
  const auto value = constructed(std::move(spelling), origin,
                                 SyntaxCategory::Binding, identity);
  const auto syntax = get(value).syntax;
  return {session_, generation_,
          put({.category = SyntaxCategory::Binding, .syntax = syntax})};
}
SyntaxTokens SyntaxArena::reference(SyntaxBinding binding) {
  const auto syntax = get(binding).syntax;
  return {session_, generation_, put({.syntax = syntax})};
}
SyntaxTokens SyntaxArena::join(std::span<const SyntaxTokens> parts) {
  auto result = std::make_shared<SyntaxCapture>();
  for (auto part : parts) {
    const auto &syntax = *get(part).syntax;
    // Check before allocation, including discarded construction in the total.
    if (syntax.tokens.size() >
        limits_.tokens - budget_->tokens - result->tokens.size())
      throw SyntaxLimitError(&CompileLimits::syntaxTokens,
                             "Procedural syntax-tokens limit exceeded.");
    if (syntax.text.size() + 1 >
        limits_.bytes - budget_->bytes - result->text.size())
      throw SyntaxLimitError(&CompileLimits::syntaxBytes,
                             "Procedural syntax-bytes limit exceeded.");
    result->tokens.insert(result->tokens.end(), syntax.tokens.begin(),
                          syntax.tokens.end());
    result->origins.insert(result->origins.end(), syntax.origins.begin(),
                           syntax.origins.end());
    if (!result->text.empty())
      result->text += ' ';
    const auto offset = result->text.size();
    for (const auto &[begin, end] : syntax.textRanges)
      result->textRanges.emplace_back(offset + begin, offset + end);
    result->text += syntax.text;
  }
  return {session_, generation_, put({.syntax = std::move(result)})};
}
SyntaxExpression SyntaxArena::parse(SyntaxTokens input) {
  const auto syntax = get(input).syntax;
  if (syntax->tokens.empty() ||
      syntax->tokens.back().kind == TokenKind::SEMICOLON)
    throw SyntaxError(
        "Expected exactly one expression without trailing tokens.");
  charge(1 + syntax->text.size() + syntax->tokens.size());
  Parser parser(syntax);
  std::shared_ptr<const Expr> expression = parser.parseExpression();
  check();
  if (!expression || parser.hadError())
    throw SyntaxError("Syntax does not parse as one complete expression.");
  return {session_, generation_,
          put({.category = SyntaxCategory::Expression,
               .syntax = syntax,
               .expression = std::move(expression)})};
}
SyntaxTokens SyntaxArena::splice(SyntaxExpression expression) {
  const auto syntax = get(expression).syntax;
  return {session_, generation_, put({.syntax = syntax})};
}
SealedSyntaxExpression SyntaxArena::seal(SyntaxExpression expression) const {
  const auto &node = get(expression);
  return {node.expression, node.syntax, expansionTrace_};
}
Diagnostic SyntaxArena::failure(SyntaxSpan primary, std::string message,
                                std::span<const SyntaxSpan> related) const {
  if (message.size() > 4096 || related.size() > 16)
    throw SyntaxError("Syntax diagnostic limit exceeded.");
  Diagnostic result =
      makeDiagnostic("GTI-S2124", DiagnosticPhase::Semantics,
                     get(primary).sourceSpan, std::move(message));
  for (auto location : related)
    result.related.push_back(
        {get(location).sourceSpan, "Related macro input."});
  result.related.insert(result.related.end(), expansionTrace_.begin(),
                        expansionTrace_.end());
  return result;
}
void SyntaxArena::reset() {
  nodes_.clear();
  // Reset invalidates handles, but never refunds the outer request budget.
  freshIdentities_.clear();
  if (generation_ == std::numeric_limits<std::size_t>::max())
    throw SyntaxError("Syntax session generation exhausted.");
  ++generation_;
}
} // namespace lang
