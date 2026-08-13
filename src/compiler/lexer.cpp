#include "gti/lexer.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace lang {

std::vector<Token> Lexer::consume(const std::filesystem::path &path) {
  std::ifstream file(path);

  if (!file) {
    reset(path.string());
    report("GTI-L0001", "Failed to open file: " + path.string(), 0, 0);
    addToken(TokenKind::END_OF_FILE);
    return std::move(tokens);
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  return scan(buffer.str(), path.string());
}

std::vector<Token> Lexer::scan(std::string sourceText, std::string sourceName) {
  reset(std::move(sourceName));
  source = std::move(sourceText);

  while (!isAtEnd()) {
    start = current;
    scanToken();
  }

  addToken(TokenKind::END_OF_FILE);
  return std::move(tokens);
}

std::vector<Token> Lexer::scanForCompletion(std::string sourceText,
                                            std::size_t byteOffset,
                                            std::string sourceName) {
  std::vector<Token> result =
      scan(std::move(sourceText), std::move(sourceName));
  if (byteOffset > source.size()) {
    return result;
  }

  for (Token &token : result) {
    const std::size_t end = token.position + token.lexeme.size();
    if (token.kind != TokenKind::IDENTIFIER || byteOffset < token.position ||
        byteOffset > end) {
      continue;
    }
    token.lexeme = source.substr(token.position, byteOffset - token.position);
    token.completion = true;
    return result;
  }

  const int completionLine =
      1 + static_cast<int>(
              std::count(source.begin(), source.begin() + byteOffset, '\n'));
  Token completion(TokenKind::IDENTIFIER, "", std::monostate{}, byteOffset,
                   completionLine, this->sourceName, true);
  const auto insertion =
      std::lower_bound(result.begin(), result.end(), byteOffset,
                       [](const Token &token, std::size_t offset) {
                         return token.position < offset;
                       });
  result.insert(insertion, std::move(completion));
  return result;
}

bool Lexer::hadError() const { return !diagnostics.empty(); }

const std::vector<LexDiagnostic> &Lexer::errors() const { return diagnostics; }

const std::string &Lexer::sourceText() const { return source; }

void Lexer::reset(std::string sourceName) {
  source.clear();
  this->sourceName = std::move(sourceName);
  tokens.clear();
  diagnostics.clear();
  start = 0;
  current = 0;
  line = 1;
}

bool Lexer::isAtEnd() const { return current >= source.length(); }

bool Lexer::match(char expected) {
  if (isAtEnd() || source.at(current) != expected) {
    return false;
  }

  ++current;
  return true;
}

void Lexer::scanToken() {
  const char currentCharacter = advance();

  switch (currentCharacter) {
  case ' ':
  case '\r':
  case '\t':
    break;
  case '\n':
    ++line;
    break;
  case '@':
    addToken(TokenKind::AT);
    break;
  case '&':
    addToken(match('&')   ? TokenKind::AND
             : match('=') ? TokenKind::AMPERSAND_EQUAL
                          : TokenKind::AMPERSAND);
    break;
  case '^':
    addToken(match('=') ? TokenKind::CARET_EQUAL : TokenKind::CARET);
    break;
  case '#':
    directive();
    break;
  case '(':
    addToken(TokenKind::LEFT_PAREN);
    break;
  case ')':
    addToken(TokenKind::RIGHT_PAREN);
    break;
  case '{':
    addToken(TokenKind::LEFT_BRACE);
    break;
  case '}':
    addToken(TokenKind::RIGHT_BRACE);
    break;
  case '[':
    addToken(TokenKind::LEFT_BRACKET);
    break;
  case ']':
    addToken(TokenKind::RIGHT_BRACKET);
    break;
  case ',':
    addToken(TokenKind::COMMA);
    break;
  case '.':
    if (peek() == '.' && peekNext() == '.') {
      advance();
      advance();
      addToken(TokenKind::ELLIPSIS);
    } else {
      addToken(TokenKind::DOT);
    }
    break;
  case '-':
    addToken(match('>')   ? TokenKind::ARROW
             : match('-') ? TokenKind::MINUS_MINUS
             : match('=') ? TokenKind::MINUS_EQUAL
                          : TokenKind::MINUS);
    break;
  case '%':
    addToken(match('=') ? TokenKind::PERCENT_EQUAL : TokenKind::PERCENT);
    break;
  case '|':
    addToken(match('|')   ? TokenKind::OR
             : match('=') ? TokenKind::PIPE_EQUAL
                          : TokenKind::PIPE);
    break;
  case '+':
    addToken(match('+')   ? TokenKind::PLUS_PLUS
             : match('=') ? TokenKind::PLUS_EQUAL
                          : TokenKind::PLUS);
    break;
  case '?':
    addToken(TokenKind::QUESTION);
    break;
  case ';':
    addToken(TokenKind::SEMICOLON);
    break;
  case ':':
    addToken(match(':') ? TokenKind::SCOPE : TokenKind::COLON);
    break;
  case '*':
    addToken(match('=') ? TokenKind::STAR_EQUAL : TokenKind::STAR);
    break;
  case '~':
    addToken(TokenKind::TILDE);
    break;
  case '/':
    if (match('/')) {
      while (peek() != '\n' && !isAtEnd()) {
        advance();
      }
    } else {
      addToken(match('=') ? TokenKind::SLASH_EQUAL : TokenKind::SLASH);
    }
    break;
  case '!':
    addToken(match('=') ? TokenKind::BANG_EQUAL : TokenKind::BANG);
    break;
  case '=':
    addToken(match('=') ? TokenKind::EQUAL_EQUAL : TokenKind::EQUAL);
    break;
  case '<':
    if (peek() == '<' && peekNext() == '=') {
      advance();
      advance();
      addToken(TokenKind::SHIFT_LEFT_EQUAL);
    } else {
      addToken(match('=') ? TokenKind::LESS_EQUAL : TokenKind::LESS);
    }
    break;
  case '>':
    if (peek() == '>' && peekNext() == '=') {
      advance();
      advance();
      addToken(TokenKind::SHIFT_RIGHT_EQUAL);
    } else {
      addToken(match('=') ? TokenKind::GREATER_EQUAL : TokenKind::GREATER);
    }
    break;
  case '"':
    string();
    break;
  case '\'':
    character();
    break;
  default:
    if (isNumber(currentCharacter)) {
      number();
    } else if (isAlpha(currentCharacter)) {
      identifier();
    } else {
      report("GTI-L0002",
             std::string("Unexpected character '") + currentCharacter + "'.");
    }
  }
}

char Lexer::advance() { return source.at(current++); }

char Lexer::peek() const {
  if (isAtEnd()) {
    return '\0';
  }
  return source.at(current);
}

char Lexer::peekNext() const {
  if (current + 1 >= source.length()) {
    return '\0';
  }
  return source.at(current + 1);
}

void Lexer::addToken(TokenKind token) { addToken(token, std::monostate{}); }

void Lexer::addToken(TokenKind kind, Literal literal) {
  std::string text = source.substr(start, current - start);
  tokens.emplace_back(kind, text, std::move(literal), start, line, sourceName);
}

void Lexer::directive() {
  while (isAlphaNumeric(peek())) {
    advance();
  }

  const std::string text = source.substr(start, current - start);
  if (text == "#if") {
    addToken(TokenKind::HASH_IF);
  } else if (text == "#elif") {
    addToken(TokenKind::HASH_ELIF);
  } else if (text == "#else") {
    addToken(TokenKind::HASH_ELSE);
  } else if (text == "#endif") {
    addToken(TokenKind::HASH_ENDIF);
  } else if (text == "#error") {
    addToken(TokenKind::HASH_ERROR);
  } else if (text == "#include") {
    addToken(TokenKind::HASH_INCLUDE);
  } else {
    report("GTI-L0003", "Unknown compile-time directive '" + text + "'.");
  }
}

void Lexer::string() {
  while (peek() != '"' && !isAtEnd()) {
    if (peek() == '\\') {
      advance();
      if (!isAtEnd()) {
        if (peek() == '\n') {
          ++line;
        }
        advance();
      }
      continue;
    }
    if (peek() == '\n') {
      ++line;
    }
    advance();
  }

  if (isAtEnd()) {
    report("GTI-L0004", "Unterminated string.", start,
           std::min(start + 1, source.size()));
    return;
  }

  advance();

  const std::string_view encoded(source.data() + start + 1,
                                 current - start - 2);
  std::string value;
  value.reserve(encoded.size());
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    if (encoded[index] != '\\' || index + 1 >= encoded.size()) {
      value += encoded[index];
      continue;
    }

    const char escaped = encoded[++index];
    const std::size_t escapeStart = start + index;
    value += decodeEscape(escaped, escapeStart, false);
  }
  addToken(TokenKind::STRING_LITERAL, value);
}

void Lexer::character() {
  std::string value;
  while (peek() != '\'' && peek() != '\n' && !isAtEnd()) {
    const std::size_t characterStart = current;
    const char currentCharacter = advance();
    if (currentCharacter != '\\') {
      value += currentCharacter;
      continue;
    }
    if (isAtEnd() || peek() == '\n') {
      break;
    }
    value += decodeEscape(advance(), characterStart, true);
  }

  if (isAtEnd() || peek() == '\n') {
    report("GTI-L0009", "Unterminated character literal.", start,
           std::max(current, std::min(start + 1, source.size())));
    addToken(TokenKind::CHARACTER_LITERAL, CharacterLiteral{});
    return;
  }

  advance();
  if (value.size() != 1) {
    report("GTI-L0010",
           "A character literal must contain exactly one 8-bit code unit.",
           start, current);
  }
  const std::uint8_t decoded =
      value.empty() ? 0
                    : static_cast<std::uint8_t>(
                          static_cast<unsigned char>(value.front()));
  addToken(TokenKind::CHARACTER_LITERAL, CharacterLiteral{decoded});
}

char Lexer::decodeEscape(char escaped, std::size_t escapeStart,
                         bool characterLiteral) {
  switch (escaped) {
  case '\\':
    return '\\';
  case '"':
    return '"';
  case 'n':
    return '\n';
  case 'r':
    return '\r';
  case 't':
    return '\t';
  case '0':
    return '\0';
  case '\'':
    if (characterLiteral) {
      return '\'';
    }
    break;
  default:
    break;
  }
  report("GTI-L0005",
         std::string("Unknown escape sequence '\\") + escaped + "'.",
         escapeStart, std::min(escapeStart + 2, source.size()));
  return escaped;
}

bool Lexer::isNumber(char value) { return value >= '0' && value <= '9'; }

bool Lexer::isAlpha(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         value == '_';
}

bool Lexer::isAlphaNumeric(char value) {
  return isAlpha(value) || isNumber(value);
}

void Lexer::prefixedInteger(int base, std::string_view description) {
  advance();
  const std::size_t digitsStart = current;
  while (isAlphaNumeric(peek())) {
    advance();
  }

  const std::string_view digits(source.data() + digitsStart,
                                current - digitsStart);
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(
      digits.data(), digits.data() + digits.size(), value, base);
  if (digits.empty() || error != std::errc{} ||
      end != digits.data() + digits.size()) {
    report("GTI-L0007",
           "Invalid " + std::string(description) + " integer literal.");
    addToken(TokenKind::INT_LITERAL, std::uint64_t{0});
    return;
  }
  addToken(TokenKind::INT_LITERAL, value);
}

void Lexer::number() {
  if (source[start] == '0' && (peek() == 'x' || peek() == 'X')) {
    prefixedInteger(16, "hexadecimal");
    return;
  }
  if (source[start] == '0' && (peek() == 'b' || peek() == 'B')) {
    prefixedInteger(2, "binary");
    return;
  }

  while (isNumber(peek())) {
    advance();
  }

  if (peek() == '.' && isNumber(peekNext())) {
    advance();
    while (isNumber(peek())) {
      advance();
    }

    BinaryFloatFormat format = BinaryFloatFormat::Binary32;
    if (peek() == 'd' || peek() == 'D') {
      format = BinaryFloatFormat::Binary64;
      advance();
    }
    const std::size_t suffixLength =
        format == BinaryFloatFormat::Binary64 ? 1 : 0;
    const std::string_view text(source.data() + start,
                                current - start - suffixLength);
    const BinaryFloatParseResult parsed = parseBinaryFloat(text, format);
    if (parsed) {
      addToken(TokenKind::FLOAT_LITERAL, *parsed.value);
      return;
    }
    report("GTI-L0006",
           parsed.failure == BinaryFloatParseFailure::OutOfRange
               ? "Floating-point literal is outside the finite binary" +
                     std::to_string(*binaryFloatWidth(format)) + " range."
               : "Invalid floating-point literal.");
    addToken(TokenKind::FLOAT_LITERAL, BinaryFloat{.format = format});
  } else {
    const std::string text = source.substr(start, current - start);
    std::uint64_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
      report("GTI-L0007", "Invalid integer literal.");
      addToken(TokenKind::INT_LITERAL, std::uint64_t{0});
    } else {
      addToken(TokenKind::INT_LITERAL, value);
    }
  }
}

void Lexer::identifier() {
  while (isAlphaNumeric(peek())) {
    advance();
  }
  const std::string text = source.substr(start, current - start);
  if (text.rfind("__gti_", 0) == 0) {
    report("GTI-L0008", "Identifiers beginning with '__gti_' are reserved for "
                        "compiler-generated names.");
  }
  if (const auto type = keywords.find(text); type != keywords.end()) {
    addToken(type->second);
  } else if (isCppReservedIdentifier(text)) {
    addToken(TokenKind::CPP_RESERVED);
  } else {
    addToken(TokenKind::IDENTIFIER);
  }
}

int Lexer::lineAt(std::size_t position) const {
  int result = 1;
  const std::size_t limit = std::min(position, source.size());
  for (std::size_t index = 0; index < limit; ++index) {
    if (source[index] == '\n') {
      ++result;
    }
  }
  return result;
}

void Lexer::report(std::string code, std::string message) {
  report(std::move(code), std::move(message), start, current);
}

void Lexer::report(std::string code, std::string message,
                   std::size_t errorStart, std::size_t errorEnd) {
  diagnostics.push_back(makeDiagnostic(
      std::move(code), DiagnosticPhase::Lexing,
      SourceSpan{sourceName, errorStart, errorEnd, lineAt(errorStart)},
      std::move(message)));
}

} // namespace lang
