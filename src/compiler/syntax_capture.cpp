#include "gti/syntax_capture.h"

namespace lang {
namespace {
TokenKind closing(TokenKind kind) {
  switch (kind) {
  case TokenKind::LEFT_PAREN:
    return TokenKind::RIGHT_PAREN;
  case TokenKind::LEFT_BRACKET:
    return TokenKind::RIGHT_BRACKET;
  case TokenKind::LEFT_BRACE:
    return TokenKind::RIGHT_BRACE;
  default:
    return TokenKind::END_OF_FILE;
  }
}
} // namespace

BalancedSyntax balancedSyntax(std::span<const Token> tokens,
                              std::size_t start) {
  if (start >= tokens.size() ||
      closing(tokens[start].kind) == TokenKind::END_OF_FILE)
    return {start, false};
  std::vector<TokenKind> stack;
  for (std::size_t i = start; i < tokens.size(); ++i) {
    const TokenKind kind = tokens[i].kind;
    const TokenKind end = closing(kind);
    if (end != TokenKind::END_OF_FILE) {
      stack.push_back(end);
    } else if (kind == TokenKind::RIGHT_PAREN ||
               kind == TokenKind::RIGHT_BRACKET ||
               kind == TokenKind::RIGHT_BRACE) {
      if (stack.empty() || stack.back() != kind)
        return {i, false};
      stack.pop_back();
      if (stack.empty())
        return {i, true};
    } else if (kind == TokenKind::END_OF_FILE) {
      return {i, false};
    }
  }
  return {tokens.size(), false};
}

SyntaxCapture captureSyntax(std::span<const Token> tokens,
                            std::string_view source) {
  SyntaxCapture result;
  result.tokens.assign(tokens.begin(), tokens.end());
  for (const auto &token : tokens)
    result.origins.push_back(tokenSpan(token));
  if (tokens.empty())
    return result;
  result.start = tokens.front().position;
  const std::size_t end = tokens.back().position + tokens.back().lexeme.size();
  if (end <= source.size() && result.start <= end) {
    result.text = source.substr(result.start, end - result.start);
    for (const auto &token : tokens)
      result.textRanges.emplace_back(token.position - result.start,
                                     token.position - result.start +
                                         token.lexeme.size());
  } else {
    // Token-only clients still retain token identities. Do not pretend this
    // synthesized rendering is the original trivia.
    for (const Token &token : tokens) {
      if (!result.text.empty())
        result.text += ' ';
      const auto begin = result.text.size();
      result.text += token.lexeme;
      result.textRanges.emplace_back(begin, result.text.size());
    }
  }
  return result;
}

bool syntaxDirective(TokenKind kind) {
  return isDirectiveToken(kind) || kind == TokenKind::HASH_MACRO ||
         kind == TokenKind::IMPORT || kind == TokenKind::MODULE ||
         kind == TokenKind::EXPORT;
}
} // namespace lang
