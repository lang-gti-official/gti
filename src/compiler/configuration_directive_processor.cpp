#include "configuration_directive_processor.h"

#include <optional>
#include <string_view>

namespace lang {
namespace {

class ConditionEvaluator final {
public:
  ConditionEvaluator(std::vector<Token> &tokens, std::size_t begin,
                     std::size_t end, const TargetInfo &target,
                     const std::unordered_set<std::string> &flags,
                     std::vector<ConfigurationToken> &configurationTokens)
      : tokens(tokens), current(begin), end(end), target(target), flags(flags),
        configurationTokens(configurationTokens) {}

  [[nodiscard]] std::optional<bool> evaluate() { return compileOr(); }

private:
  [[nodiscard]] std::optional<bool> compileOr() {
    std::optional<bool> value = compileAnd();
    if (!value) {
      return std::nullopt;
    }
    while (matches(TokenKind::OR, "||")) {
      const std::optional<bool> right = compileAnd();
      if (!right) {
        return std::nullopt;
      }
      *value = *value || *right;
    }
    return value;
  }

  [[nodiscard]] std::optional<bool> compileAnd() {
    std::optional<bool> value = compileUnary();
    if (!value) {
      return std::nullopt;
    }
    while (matches(TokenKind::AND, "&&")) {
      const std::optional<bool> right = compileUnary();
      if (!right) {
        return std::nullopt;
      }
      *value = *value && *right;
    }
    return value;
  }

  [[nodiscard]] std::optional<bool> compileUnary() {
    if (matches(TokenKind::BANG)) {
      const std::optional<bool> operand = compileUnary();
      return operand ? std::optional<bool>(!*operand) : std::nullopt;
    }
    return compilePrimary();
  }

  [[nodiscard]] std::optional<bool> compilePrimary() {
    if (matches(TokenKind::LEFT_PAREN)) {
      const std::optional<bool> value = compileOr();
      return value && matches(TokenKind::RIGHT_PAREN) ? value : std::nullopt;
    }

    if (current < end && tokens[current].kind == TokenKind::IDENTIFIER &&
        tokens[current].lexeme == "defined") {
      configurationTokens.push_back({.kind = ConfigurationTokenKind::Operator,
                                     .span = tokenSpan(tokens[current])});
      ++current;
      if (!matches(TokenKind::LEFT_PAREN) || current >= end ||
          tokens[current].kind != TokenKind::IDENTIFIER) {
        return std::nullopt;
      }
      Token &flag = tokens[current++];
      configurationTokens.push_back(
          {.kind = ConfigurationTokenKind::Flag, .span = tokenSpan(flag)});
      flag.configurationFlagReference = true;
      const bool isDefined = flags.contains(flag.lexeme);
      flag.configurationFlagDefined = isDefined;
      return matches(TokenKind::RIGHT_PAREN) ? std::optional<bool>(isDefined)
                                             : std::nullopt;
    }

    if (current + 4 >= end || tokens[current].kind != TokenKind::IDENTIFIER ||
        tokens[current].lexeme != "target" ||
        tokens[current + 1].kind != TokenKind::DOT ||
        tokens[current + 2].kind != TokenKind::IDENTIFIER ||
        (tokens[current + 3].kind != TokenKind::EQUAL_EQUAL &&
         tokens[current + 3].kind != TokenKind::BANG_EQUAL) ||
        tokens[current + 4].kind != TokenKind::STRING_LITERAL) {
      return std::nullopt;
    }
    const std::optional<TargetProperty> property =
        parseTargetProperty(tokens[current + 2].lexeme);
    const auto *expected =
        std::get_if<std::string>(&tokens[current + 4].literal);
    if (!property || expected == nullptr) {
      return std::nullopt;
    }
    const bool equal = target.value(*property) == *expected;
    const bool result =
        tokens[current + 3].kind == TokenKind::BANG_EQUAL ? !equal : equal;
    current += 5;
    return result;
  }

  [[nodiscard]] bool matches(TokenKind kind, std::string_view spelling = {}) {
    if (current >= end || tokens[current].kind != kind ||
        (!spelling.empty() && tokens[current].lexeme != spelling)) {
      return false;
    }
    ++current;
    return true;
  }

  std::vector<Token> &tokens;
  std::size_t current;
  std::size_t end;
  const TargetInfo &target;
  const std::unordered_set<std::string> &flags;
  std::vector<ConfigurationToken> &configurationTokens;
};

std::size_t directiveLineEnd(const std::vector<Token> &tokens,
                             std::size_t directive) {
  const int line = tokens[directive].line;
  std::size_t end = directive + 1;
  while (end < tokens.size() && tokens[end].kind != TokenKind::END_OF_FILE &&
         tokens[end].line == line) {
    ++end;
  }
  return end;
}

} // namespace

ConfigurationDirectiveProcessor::ConfigurationDirectiveProcessor(
    std::vector<Token> &tokens, const TargetInfo &target,
    std::unordered_set<std::string> &activeFlags,
    std::unordered_set<std::string> &declaredFlags,
    std::vector<Diagnostic> &diagnostics)
    : tokens(tokens), target(target), activeFlags(activeFlags),
      declaredFlags(declaredFlags), diagnostics(diagnostics) {}

ConfigurationDirectiveStep
ConfigurationDirectiveProcessor::process(std::size_t index) {
  Token &token = tokens[index];
  ConfigurationDirectiveStep result{.lastConsumedIndex = index};
  if (token.kind == TokenKind::END_OF_FILE) {
    return result;
  }

  if (token.kind == TokenKind::HASH_IF || token.kind == TokenKind::HASH_IFDEF ||
      token.kind == TokenKind::HASH_IFNDEF) {
    result.conditionalBoundary = true;
    const bool parentActive = currentPathActive();
    bool selected = false;
    if (token.kind == TokenKind::HASH_IF) {
      selected = ConditionEvaluator(tokens, index + 1, tokens.size(), target,
                                    activeFlags, configurationTokens)
                     .evaluate()
                     .value_or(false);
    } else {
      const std::size_t end = directiveLineEnd(tokens, index);
      if (index + 1 < end && tokens[index + 1].kind == TokenKind::IDENTIFIER) {
        configurationTokens.push_back({.kind = ConfigurationTokenKind::Flag,
                                       .span = tokenSpan(tokens[index + 1])});
      }
      if (index + 1 >= end || tokens[index + 1].kind != TokenKind::IDENTIFIER ||
          index + 2 != end) {
        reportFlagName(token);
      } else {
        Token &flag = tokens[index + 1];
        flag.configurationFlagReference = true;
        selected = activeFlags.contains(flag.lexeme);
        flag.configurationFlagDefined = selected;
        if (token.kind == TokenKind::HASH_IFNDEF) {
          selected = !selected;
        }
      }
    }
    conditionals.push_back({.opener = token,
                            .parentActive = parentActive,
                            .branchTaken = selected,
                            .active = parentActive && selected});
  } else if (token.kind == TokenKind::HASH_ENDIF) {
    result.conditionalBoundary = true;
    if (conditionals.empty()) {
      report(token, "Unexpected '#endif' without a matching '#if'.",
             "GTI-I0002");
    } else {
      conditionals.pop_back();
    }
  } else if (token.kind == TokenKind::HASH_ELIF) {
    result.conditionalBoundary = true;
    const bool selected =
        ConditionEvaluator(tokens, index + 1, tokens.size(), target,
                           activeFlags, configurationTokens)
            .evaluate()
            .value_or(false);
    if (conditionals.empty()) {
      report(token, "Unexpected '#elif' without a matching '#if'.",
             "GTI-I0002");
    } else if (conditionals.back().sawElse) {
      report(token, "Unexpected '#elif' after '#else'.", "GTI-I0002");
    } else {
      ConditionalFrame &frame = conditionals.back();
      frame.active = frame.parentActive && !frame.branchTaken && selected;
      frame.branchTaken = frame.branchTaken || selected;
    }
  } else if (token.kind == TokenKind::HASH_ELSE) {
    result.conditionalBoundary = true;
    if (conditionals.empty()) {
      report(token, "Unexpected '#else' without a matching '#if'.",
             "GTI-I0002");
    } else if (conditionals.back().sawElse) {
      report(token, "Duplicate '#else' in compile-time conditional.",
             "GTI-I0002");
    } else {
      ConditionalFrame &frame = conditionals.back();
      frame.active = frame.parentActive && !frame.branchTaken;
      frame.branchTaken = true;
      frame.sawElse = true;
    }
  }

  if (token.kind != TokenKind::HASH_DEFINE &&
      token.kind != TokenKind::HASH_UNDEF) {
    return result;
  }

  result.consumed = true;
  const bool active = currentPathActive();
  const std::size_t end = directiveLineEnd(tokens, index);
  result.lastConsumedIndex = end - 1;
  if (!active) {
    for (std::size_t inactive = index; inactive < end; ++inactive) {
      retainInactive(tokens[inactive]);
    }
  }
  const bool hasName =
      index + 1 < end && tokens[index + 1].kind == TokenKind::IDENTIFIER;
  const bool hasExtra = hasName && index + 2 < end;
  if (hasName) {
    configurationTokens.push_back({.kind = ConfigurationTokenKind::Flag,
                                   .span = tokenSpan(tokens[index + 1])});
  }
  if (!hasName) {
    reportFlagName(token);
  } else if (hasExtra) {
    if (token.kind == TokenKind::HASH_DEFINE) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-I0011", DiagnosticPhase::SourceLoading, tokens[index + 2],
          "Configuration flags carry no value; use a typed 'constexpr' "
          "selected by '#ifdef'.");
      diagnostic.hints.emplace_back(
          "'#define' accepts exactly one flag name and no replacement list.");
      diagnostics.emplace_back(std::move(diagnostic));
    } else {
      reportFlagName(token);
    }
  } else {
    const std::string flag = tokens[index + 1].lexeme;
    if (token.kind == TokenKind::HASH_DEFINE) {
      declaredFlags.insert(flag);
    }
    if (active) {
      if (token.kind == TokenKind::HASH_DEFINE) {
        activeFlags.insert(flag);
      } else {
        activeFlags.erase(flag);
      }
    }
  }
  return result;
}

bool ConfigurationDirectiveProcessor::currentPathActive() const {
  return conditionals.empty() || conditionals.back().active;
}

int ConfigurationDirectiveProcessor::conditionalDepth() const {
  return static_cast<int>(conditionals.size());
}

void ConfigurationDirectiveProcessor::retainInactive(const Token &token) {
  if (token.kind != TokenKind::END_OF_FILE && !token.lexeme.empty()) {
    inactiveSpans.push_back(tokenSpan(token));
  }
}

void ConfigurationDirectiveProcessor::finish() {
  if (finished) {
    return;
  }
  finished = true;
  if (!conditionals.empty()) {
    report(conditionals.front().opener,
           "Unterminated compile-time conditional. Expect '#endif'.",
           "GTI-I0003");
  }
}

std::vector<SourceSpan> ConfigurationDirectiveProcessor::takeInactiveSpans() {
  return std::move(inactiveSpans);
}

std::vector<ConfigurationToken>
ConfigurationDirectiveProcessor::takeConfigurationTokens() {
  return std::move(configurationTokens);
}

void ConfigurationDirectiveProcessor::report(const Token &token,
                                             std::string message,
                                             std::string code) {
  diagnostics.emplace_back(makeDiagnostic(std::move(code),
                                          DiagnosticPhase::SourceLoading, token,
                                          std::move(message)));
}

void ConfigurationDirectiveProcessor::reportFlagName(const Token &directive) {
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-L0012", DiagnosticPhase::Lexing, directive,
      "'" + directive.lexeme +
          "' must be followed by exactly one configuration flag name.");
  diagnostic.hints.emplace_back(
      "Configuration flag names use ordinary GTI identifier syntax.");
  diagnostics.emplace_back(std::move(diagnostic));
}

} // namespace lang
