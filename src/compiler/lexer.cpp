#include "gti/lexer.h"

#include "source_text.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

namespace lang {

RawStringScan scanRawStringLiteral(std::string_view source, std::size_t start) {
  using Problem = RawStringScan::Problem;
  RawStringScan scan;
  const std::size_t delimiterStart = start + 2;
  std::size_t index = delimiterStart;
  const auto delimiterCharacter = [](char value) {
    const auto byte = static_cast<unsigned char>(value);
    return byte > 0x20 && byte < 0x7F && value != '(' && value != ')' &&
           value != '\\' && value != '"';
  };
  while (index < source.size() && delimiterCharacter(source[index]) &&
         index - delimiterStart < rawStringDelimiterLimit)
    ++index;
  scan.delimiterEnd = index;
  if (index < source.size() && source[index] == '(') {
    std::string closing = ")";
    closing.append(source.substr(delimiterStart, index - delimiterStart));
    closing += '"';
    const std::size_t close = source.find(closing, index + 1);
    if (close == std::string_view::npos) {
      scan.problem = Problem::Unterminated;
      scan.end = source.size();
    } else {
      scan.end = close + closing.size();
    }
    return scan;
  }
  scan.problem = index < source.size() && delimiterCharacter(source[index])
                     ? Problem::DelimiterTooLong
                 : index >= source.size() || source[index] == '"' ||
                         source[index] == '\n' || source[index] == '\r'
                     ? Problem::MissingOpenParenthesis
                     : Problem::InvalidDelimiterCharacter;
  // Recover through the closing sequence the writer evidently intended, so
  // its quotes and contents do not cascade into further diagnostics.
  std::size_t lineEnd = source.find('\n', start);
  if (lineEnd == std::string_view::npos)
    lineEnd = source.size();
  if (lineEnd > start && source[lineEnd - 1] == '\r')
    --lineEnd;
  scan.end = lineEnd;
  const std::size_t paren = source.find('(', delimiterStart);
  if (paren != std::string_view::npos && paren < lineEnd) {
    std::string closing = ")";
    closing.append(source.substr(delimiterStart, paren - delimiterStart));
    closing += '"';
    const std::size_t close = source.find(closing, paren + 1);
    if (close != std::string_view::npos)
      scan.end = close + closing.size();
  }
  return scan;
}

std::vector<Token> Lexer::consume(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);

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

  if (const auto invalid = source_text::firstInvalidUtf8(source)) {
    report("GTI-L0013", "Source text is not valid UTF-8.", *invalid,
           *invalid + 1);
    start = current = source.size();
    addToken(TokenKind::END_OF_FILE);
    return std::move(tokens);
  }
  if (const auto loneCarriageReturn =
          source_text::firstLoneCarriageReturn(source)) {
    report("GTI-L0014",
           "A carriage return must be followed by a line feed; use LF or "
           "CRLF line endings.",
           *loneCarriageReturn, *loneCarriageReturn + 1);
    start = current = source.size();
    addToken(TokenKind::END_OF_FILE);
    return std::move(tokens);
  }
  if (source_text::hasUtf8Bom(source)) {
    current = source_text::utf8Bom.size();
  }

  while (!isAtEnd() && !errorLimitReached(diagnostics, errorLimit)) {
    start = current;
    scanToken();
  }

  addToken(TokenKind::END_OF_FILE);
  return std::move(tokens);
}

std::vector<Token> Lexer::scanModuleHeader(std::istream &input,
                                           std::string sourceName) {
  std::string text;
  std::size_t chunk = 4096;
  for (;;) {
    const auto offset = text.size();
    text.resize(offset + chunk);
    input.read(text.data() + offset, static_cast<std::streamsize>(chunk));
    text.resize(offset + static_cast<std::size_t>(input.gcount()));
    auto result = scanModuleHeader(std::move(text), sourceName);
    // A token/comment/UTF-8 sequence may straddle a read boundary. Retry an
    // exhausted prefix with more input before interpreting its diagnostics.
    // Geometric growth keeps arbitrarily long leading trivia linear in size.
    if (!input || current < source.size() ||
        (result.size() > 1 &&
         result[result.size() - 2].kind == TokenKind::SEMICOLON))
      return result;
    text = std::move(source);
    chunk = text.size();
  }
}

std::vector<Token> Lexer::scanModuleHeader(std::string sourceText,
                                           std::string sourceName) {
  reset(std::move(sourceName));
  source = std::move(sourceText);
  if (source_text::hasUtf8Bom(source))
    current = source_text::utf8Bom.size();
  while (!isAtEnd()) {
    start = current;
    const auto before = tokens.size();
    scanToken();
    if (hadError())
      break;
    if (tokens.size() == before)
      continue;
    const auto count = tokens.size();
    const bool exported = tokens.front().kind == TokenKind::EXPORT;
    const auto prefix = exported ? 2U : 1U;
    if (count == 1) {
      if (!exported && tokens.front().kind != TokenKind::MODULE)
        break;
    } else if (exported && count == 2) {
      if (tokens.back().kind != TokenKind::MODULE)
        break;
    } else {
      const bool identifier = (count - prefix) % 2 == 1;
      if (tokens.back().kind !=
          (identifier ? TokenKind::IDENTIFIER : TokenKind::DOT))
        break;
    }
  }
  // Validate only the consumed prefix. Unused module bodies are not admitted
  // or diagnosed merely because their declarations were discovered.
  const auto prefix = std::string_view(source).substr(0, current);
  if (const auto invalid = source_text::firstInvalidUtf8(prefix))
    report("GTI-L0013", "Source text is not valid UTF-8.", *invalid,
           *invalid + 1);
  if (const auto carriage = source_text::firstLoneCarriageReturn(prefix))
    report("GTI-L0014",
           "A carriage return must be followed by a line feed; use LF or "
           "CRLF line endings.",
           *carriage, *carriage + 1);
  start = current;
  addToken(TokenKind::END_OF_FILE);
  return std::move(tokens);
}

std::vector<Token> Lexer::scanForCompletion(std::string sourceText,
                                            std::size_t byteOffset,
                                            std::string sourceName) {
  collectCompletionExclusions = true;
  std::vector<Token> result =
      scan(std::move(sourceText), std::move(sourceName));
  collectCompletionExclusions = false;
  if (byteOffset > source.size()) {
    return result;
  }
  for (const auto &[begin, end] : completionExclusions) {
    if (byteOffset >= begin && byteOffset < end) {
      return result;
    }
  }

  for (Token &token : result) {
    const std::size_t end = token.position + token.lexeme.size();
    if (token.lexeme.empty() || token.kind == TokenKind::STRING_LITERAL ||
        !isAlpha(token.lexeme.front()) || byteOffset < token.position ||
        byteOffset > end) {
      continue;
    }
    token.lexeme = source.substr(token.position, byteOffset - token.position);
    // Keyword-prefix completion is editor syntax, not an identifier admitted
    // to ordinary parsing. This also makes a complete `class`/`if` trigger
    // work.
    token.kind = TokenKind::IDENTIFIER;
    token.completion = true;
    return result;
  }

  const int completionLine =
      1 + static_cast<int>(
              std::count(source.begin(), source.begin() + byteOffset, '\n'));
  Token completion(TokenKind::IDENTIFIER, "", std::monostate{}, byteOffset,
                   completionLine, this->sourceName, true);
  // The cursor marker must precede EOF even when the last real token ends
  // before trailing whitespace. Ordinary EOF tokens need not start at the
  // cursor, and inserting after EOF makes the marker unreachable to loading
  // and parsing (notably for an empty `import ` prefix).
  const auto insertion =
      std::find_if(result.begin(), result.end(), [&](const Token &token) {
        return token.kind == TokenKind::END_OF_FILE ||
               token.position >= byteOffset;
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
  completionExclusions.clear();
  start = 0;
  current = 0;
  line = 1;
  pendingDocumentationMarkdown.reset();
  pendingDocumentationLastLine = 0;
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
      lineComment();
    } else if (match('*')) {
      pendingDocumentationMarkdown.reset();
      pendingDocumentationLastLine = 0;
      blockComment();
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
    } else if (currentCharacter == 'R' && peek() == '"') {
      rawString();
    } else if (isAlpha(currentCharacter)) {
      identifier();
    } else if (static_cast<unsigned char>(currentCharacter) >= 0x80U) {
      const source_text::Utf8Scalar scalar =
          source_text::decodeUtf8(source, start);
      current = start + scalar.length;
      report("GTI-L0015",
             "Non-ASCII source text is permitted only inside comments and "
             "string literals; GTI identifiers and tokens use ASCII.");
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
  if (kind != TokenKind::END_OF_FILE && pendingDocumentationMarkdown) {
    if (line <= pendingDocumentationLastLine + 1) {
      tokens.back().leadingDocumentationMarkdown =
          std::make_shared<const std::string>(
              std::move(*pendingDocumentationMarkdown));
    }
    pendingDocumentationMarkdown.reset();
    pendingDocumentationLastLine = 0;
  }
}

void Lexer::lineComment() {
  const bool documentation = peek() == '/';
  if (documentation) {
    advance();
  }
  const std::size_t contentStart = current;
  while (peek() != '\n' && !isAtEnd()) {
    advance();
  }
  if (collectCompletionExclusions)
    completionExclusions.emplace_back(start, current + 1);

  if (!documentation) {
    pendingDocumentationMarkdown.reset();
    pendingDocumentationLastLine = 0;
    return;
  }

  std::size_t contentEnd = current;
  if (contentEnd > contentStart && source[contentEnd - 1] == '\r') {
    --contentEnd;
  }
  std::string content = source.substr(contentStart, contentEnd - contentStart);
  if (!content.empty() && content.front() == ' ') {
    content.erase(content.begin());
  }

  if (!pendingDocumentationMarkdown ||
      line != pendingDocumentationLastLine + 1) {
    pendingDocumentationMarkdown = std::move(content);
  } else {
    *pendingDocumentationMarkdown += '\n';
    *pendingDocumentationMarkdown += content;
  }
  pendingDocumentationLastLine = line;
}

void Lexer::blockComment() {
  while (!isAtEnd()) {
    if (peek() == '*' && peekNext() == '/') {
      advance();
      advance();
      if (collectCompletionExclusions)
        completionExclusions.emplace_back(start, current);
      return;
    }
    if (advance() == '\n') {
      ++line;
    }
  }

  report("GTI-L0011", "Unterminated block comment.", start,
         std::min(start + 2, source.size()));
  if (collectCompletionExclusions)
    completionExclusions.emplace_back(start, current + 1);
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
  } else if (text == "#define") {
    addToken(TokenKind::HASH_DEFINE);
  } else if (text == "#macro") {
    addToken(TokenKind::HASH_MACRO);
  } else if (text == "#undef") {
    addToken(TokenKind::HASH_UNDEF);
  } else if (text == "#ifdef") {
    addToken(TokenKind::HASH_IFDEF);
  } else if (text == "#ifndef") {
    addToken(TokenKind::HASH_IFNDEF);
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
    if (collectCompletionExclusions)
      completionExclusions.emplace_back(start, current + 1);
    return;
  }

  advance();

  const std::string_view encoded(source.data() + start + 1,
                                 current - start - 2);
  if (collectCompletionExclusions)
    completionExclusions.emplace_back(start + 1, current);
  std::string value;
  value.reserve(encoded.size());
  decodeLiteralBody(encoded, start + 1, value);
  addToken(TokenKind::STRING_LITERAL, value);
}

void Lexer::character() {
  // Find the closing quote first; an escape always covers its next byte, so
  // `'\''` and `'\\'` stay one literal. Decoding happens on the whole body.
  while (peek() != '\'' && peek() != '\n' && !isAtEnd()) {
    if (advance() == '\\' && !isAtEnd() && peek() != '\n') {
      advance();
    }
  }

  if (isAtEnd() || peek() == '\n') {
    report("GTI-L0009", "Unterminated character literal.", start,
           std::max(current, std::min(start + 1, source.size())));
    addToken(TokenKind::CHARACTER_LITERAL, CharacterLiteral{});
    if (collectCompletionExclusions)
      completionExclusions.emplace_back(start, current + 1);
    return;
  }

  advance();
  if (collectCompletionExclusions)
    completionExclusions.emplace_back(start + 1, current);
  std::string value;
  decodeLiteralBody(
      std::string_view(source.data() + start + 1, current - start - 2),
      start + 1, value);
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

void Lexer::decodeLiteralBody(std::string_view body, std::size_t bodyStart,
                              std::string &value) {
  value.reserve(value.size() + body.size());
  for (std::size_t index = 0;
       index < body.size() && !errorLimitReached(diagnostics, errorLimit);
       ++index) {
    if (body[index] != '\\' || index + 1 >= body.size()) {
      value += body[index];
      continue;
    }
    decodeEscape(body, index, bodyStart, value);
  }
}

namespace {

[[nodiscard]] int hexDigitValue(char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}

void appendUtf8(std::uint32_t scalar, std::string &value) {
  if (scalar < 0x80) {
    value += static_cast<char>(scalar);
  } else if (scalar < 0x800) {
    value += static_cast<char>(0xC0 | (scalar >> 6));
    value += static_cast<char>(0x80 | (scalar & 0x3F));
  } else if (scalar < 0x10000) {
    value += static_cast<char>(0xE0 | (scalar >> 12));
    value += static_cast<char>(0x80 | ((scalar >> 6) & 0x3F));
    value += static_cast<char>(0x80 | (scalar & 0x3F));
  } else {
    value += static_cast<char>(0xF0 | (scalar >> 18));
    value += static_cast<char>(0x80 | ((scalar >> 12) & 0x3F));
    value += static_cast<char>(0x80 | ((scalar >> 6) & 0x3F));
    value += static_cast<char>(0x80 | (scalar & 0x3F));
  }
}

} // namespace

// `body[index]` is a backslash with at least one byte after it. Appends the
// decoded code units and leaves `index` on the escape's final byte.
void Lexer::decodeEscape(std::string_view body, std::size_t &index,
                         std::size_t bodyStart, std::string &value) {
  const std::size_t escapeStart = bodyStart + index;
  const char escaped = body[++index];
  // Fixed-width hexadecimal escapes. Unlike C++, `\x` takes exactly two
  // digits, so a following hexadecimal letter is never absorbed.
  const auto hexadecimal =
      [&](std::size_t width) -> std::optional<std::uint32_t> {
    std::uint32_t result = 0;
    std::size_t taken = 0;
    while (taken < width && index + 1 < body.size() &&
           hexDigitValue(body[index + 1]) >= 0) {
      result = result * 16 +
               static_cast<std::uint32_t>(hexDigitValue(body[++index]));
      ++taken;
    }
    if (taken == width)
      return result;
    report("GTI-L0005",
           std::string("Escape '\\") + escaped + "' requires exactly " +
               std::to_string(width) + " hexadecimal digits.",
           escapeStart, std::min(bodyStart + index + 1, source.size()));
    return std::nullopt;
  };
  switch (escaped) {
  case '\\':
    value += '\\';
    return;
  case '"':
    value += '"';
    return;
  case '\'':
    value += '\'';
    return;
  case 'n':
    value += '\n';
    return;
  case 'r':
    value += '\r';
    return;
  case 't':
    value += '\t';
    return;
  case '0':
    value += '\0';
    return;
  case 'a':
    value += '\a';
    return;
  case 'b':
    value += '\b';
    return;
  case 'e':
    value += static_cast<char>(0x1B);
    return;
  case 'f':
    value += '\f';
    return;
  case 'v':
    value += '\v';
    return;
  case 'x':
    if (const auto byte = hexadecimal(2))
      value += static_cast<char>(*byte);
    return;
  case 'u':
  case 'U':
    if (const auto scalar = hexadecimal(escaped == 'u' ? 4 : 8)) {
      if (*scalar > 0x10FFFF || (*scalar >= 0xD800 && *scalar <= 0xDFFF)) {
        report("GTI-L0005",
               "Escape '\\" + std::string(1, escaped) +
                   "' does not name a Unicode scalar value.",
               escapeStart, std::min(bodyStart + index + 1, source.size()));
        return;
      }
      appendUtf8(*scalar, value);
    }
    return;
  default:
    break;
  }
  report("GTI-L0005",
         std::string("Unknown escape sequence '\\") + escaped + "'.",
         escapeStart, std::min(escapeStart + 2, source.size()));
  value += escaped;
}

bool Lexer::isNumber(char value) { return value >= '0' && value <= '9'; }

bool Lexer::isAlpha(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         value == '_';
}

bool Lexer::isAlphaNumeric(char value) {
  return isAlpha(value) || isNumber(value);
}

namespace {

[[nodiscard]] bool isDigitOfBase(char value, int base) {
  if (value >= '0' && value <= '9')
    return value - '0' < base;
  if (base == 16)
    return (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
  return false;
}

} // namespace

// Validates the `_` digit separators of one digit run `[runStart, runEnd)`.
// A separator must sit between two digits of the run. The single exception
// is one trailing separator of the final run immediately before a suffix
// (`10_u8`). Reports the first misplaced separator and returns false.
bool Lexer::validDigitSeparators(std::size_t runStart, std::size_t runEnd,
                                 std::string_view before,
                                 bool trailingBeforeSuffix,
                                 std::string_view code) {
  if (runStart < runEnd && source[runStart] == '_') {
    report(std::string(code),
           before == "."
               ? "A digit separator '_' cannot appear next to '.'."
               : "A digit separator '_' cannot follow the radix prefix '" +
                     std::string(before) + "'.",
           runStart, runStart + 1);
    return false;
  }
  for (std::size_t index = runStart; index < runEnd; ++index) {
    if (source[index] != '_')
      continue;
    if (index + 1 < runEnd && source[index + 1] == '_') {
      std::size_t last = index + 1;
      while (last < runEnd && source[last] == '_')
        ++last;
      report(std::string(code),
             "Consecutive digit separators are not allowed; a '_' must sit "
             "between two digits.",
             index, last);
      return false;
    }
    if (index + 1 == runEnd && !trailingBeforeSuffix) {
      const char next = runEnd < source.size() ? source[runEnd] : '\0';
      report(std::string(code),
             next == '.' ? "A digit separator '_' cannot appear next to '.'."
                         : "A digit separator '_' must be followed by a digit.",
             index, index + 1);
      return false;
    }
  }
  return true;
}

void Lexer::number() {
  int base = 10;
  std::string_view prefix;
  if (source[start] == '0' && (peek() == 'x' || peek() == 'X')) {
    base = 16;
    prefix = peek() == 'x' ? "0x" : "0X";
    advance();
  } else if (source[start] == '0' && (peek() == 'b' || peek() == 'B')) {
    base = 2;
    prefix = peek() == 'b' ? "0b" : "0B";
    advance();
  }

  // Binary scans decimal digits too, so `0b102` reports an invalid digit
  // instead of starting a suffix. In a hexadecimal literal `f` is a digit:
  // `0x1f32` is an integer, never a floating suffix.
  const int scanBase = base == 2 ? 10 : base;
  const auto scanDigits = [&] {
    while (isDigitOfBase(peek(), scanBase) || peek() == '_')
      advance();
  };
  // A decimal literal's first digit is already consumed; a prefixed one's
  // digits start after its radix prefix.
  const std::size_t integerStart = base == 10 ? start : current;
  scanDigits();
  const std::size_t integerEnd = current;

  bool floating = false;
  std::size_t fractionStart = current;
  std::size_t fractionEnd = current;
  if (base == 10 && peek() == '.' &&
      (isNumber(peekNext()) || peekNext() == '_')) {
    advance();
    floating = true;
    fractionStart = current;
    scanDigits();
    fractionEnd = current;
  }

  // A suffix is the identifier-like tail written directly after the digits.
  const std::size_t suffixStart = current;
  while (isAlphaNumeric(peek()))
    advance();
  const std::string_view suffix(source.data() + suffixStart,
                                current - suffixStart);

  const auto stripped = [&](std::size_t from, std::size_t to) {
    std::string digits;
    digits.reserve(to - from);
    for (std::size_t index = from; index < to; ++index)
      if (source[index] != '_')
        digits += source[index];
    return digits;
  };

  NumericSuffix written = NumericSuffix::None;
  bool legacyDouble = false;
  bool valid = true;
  const auto integerSuffixError = [&](std::string message) {
    report(floating ? "GTI-L0006" : "GTI-L0007", std::move(message),
           suffixStart, current);
    valid = false;
  };
  if (!suffix.empty()) {
    const std::optional<NumericSuffix> known =
        numericSuffixFromSpelling(suffix);
    if (!known && floating && (suffix == "d" || suffix == "D")) {
      legacyDouble = true;
    } else if (!known && (suffix.front() == 'e' || suffix.front() == 'E') &&
               suffix.size() > 1 && isNumber(suffix[1])) {
      integerSuffixError("Numeric literals have no exponent form; '" +
                         std::string(suffix) +
                         "' is not a valid suffix. Write the value in full.");
    } else if (!known && (suffix == "d" || suffix == "D")) {
      integerSuffixError("Suffix '" + std::string(suffix) +
                         "' applies only to a literal with a fraction, such "
                         "as '1.0d'; write 'f64' for a double.");
    } else if (!known) {
      integerSuffixError(
          "Unknown numeric literal suffix '" + std::string(suffix) +
          "'. Integer literals accept u8, u16, u32, u64, i8, i16, i32 or i64; "
          "decimal literals also accept f32 or f64.");
    } else if (floating && isIntegerSuffix(*known)) {
      integerSuffixError("Integer suffix '" + std::string(suffix) +
                         "' cannot be applied to a floating-point literal.");
    } else if (base != 10 && isFloatingSuffix(*known)) {
      integerSuffixError("Floating-point suffix '" + std::string(suffix) +
                         "' requires a decimal literal.");
    } else {
      written = *known;
    }
  }

  const bool floatingResult = floating || isFloatingSuffix(written);
  const std::string_view code = floatingResult ? "GTI-L0006" : "GTI-L0007";
  const bool hasSuffix = !suffix.empty();
  if (valid && base != 10 && integerStart == integerEnd) {
    report("GTI-L0007", std::string("Invalid ") +
                            (base == 16 ? "hexadecimal" : "binary") +
                            " integer literal.");
    valid = false;
  }
  if (valid)
    valid = validDigitSeparators(integerStart, integerEnd,
                                 base == 10 ? std::string_view{} : prefix,
                                 !floating && hasSuffix, code);
  if (valid && floating)
    valid =
        validDigitSeparators(fractionStart, fractionEnd, ".", hasSuffix, code);

  if (floatingResult) {
    const BinaryFloatFormat format =
        legacyDouble || written == NumericSuffix::F64
            ? BinaryFloatFormat::Binary64
            : BinaryFloatFormat::Binary32;
    if (valid) {
      // `1f32` has no written fraction; it denotes the same value as `1.0`.
      const std::string text =
          stripped(integerStart, integerEnd) + "." +
          (floating ? stripped(fractionStart, fractionEnd) : std::string("0"));
      const BinaryFloatParseResult parsed = parseBinaryFloat(text, format);
      if (parsed) {
        addToken(TokenKind::FLOAT_LITERAL, *parsed.value);
        tokens.back().numericSuffix = written;
        return;
      }
      report("GTI-L0006",
             parsed.failure == BinaryFloatParseFailure::OutOfRange
                 ? "Floating-point literal is outside the finite binary" +
                       std::to_string(*binaryFloatWidth(format)) + " range."
                 : "Invalid floating-point literal.");
    }
    addToken(TokenKind::FLOAT_LITERAL, BinaryFloat{.format = format});
    tokens.back().numericSuffix = written;
    return;
  }

  if (valid) {
    const std::string digits = stripped(integerStart, integerEnd);
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(
        digits.data(), digits.data() + digits.size(), value, base);
    if (error == std::errc{} && end == digits.data() + digits.size()) {
      addToken(TokenKind::INT_LITERAL, value);
      tokens.back().numericSuffix = written;
      return;
    }
    report("GTI-L0007", base == 16  ? "Invalid hexadecimal integer literal."
                        : base == 2 ? "Invalid binary integer literal."
                                    : "Invalid integer literal.");
  }
  addToken(TokenKind::INT_LITERAL, std::uint64_t{0});
  tokens.back().numericSuffix = written;
}

// `R"delim( ... )delim"`: the delimiter is at most 16 characters of the basic
// source set other than space, parentheses, backslash and control characters.
// The body is taken verbatim, with no escape processing, until the first
// `)delim"`, and may span lines.
void Lexer::rawString() {
  using Problem = RawStringScan::Problem;
  const RawStringScan scan = scanRawStringLiteral(source, start);
  const std::size_t end = scan.end;
  const int newlines = static_cast<int>(
      std::count(source.begin() + static_cast<std::ptrdiff_t>(start),
                 source.begin() + static_cast<std::ptrdiff_t>(end), '\n'));
  if (scan.problem == Problem::Unterminated) {
    current = end;
    line += newlines;
    report("GTI-L0004", "Unterminated raw string literal.", start, start + 2);
    if (collectCompletionExclusions)
      completionExclusions.emplace_back(start, current + 1);
    return;
  }
  if (scan.problem != Problem::None) {
    std::string message;
    std::size_t errorEnd = scan.delimiterEnd;
    switch (scan.problem) {
    case Problem::DelimiterTooLong:
      message = "A raw string delimiter can have at most " +
                std::to_string(rawStringDelimiterLimit) + " characters.";
      errorEnd = scan.delimiterEnd + 1;
      break;
    case Problem::InvalidDelimiterCharacter:
      message = "Invalid character in raw string delimiter; a delimiter "
                "cannot contain spaces, parentheses, backslashes or control "
                "characters.";
      errorEnd = scan.delimiterEnd + 1;
      break;
    default:
      message = "Raw string literal is missing '(' after its delimiter.";
      break;
    }
    report("GTI-L0016", std::move(message), start, errorEnd);
    // Recover as one empty string over the malformed literal.
    current = end;
    if (collectCompletionExclusions)
      completionExclusions.emplace_back(start + 1, current);
    addToken(TokenKind::STRING_LITERAL, std::string{});
    line += newlines;
    return;
  }
  const std::size_t bodyStart = scan.delimiterEnd + 1;
  const std::size_t delimiterLength = scan.delimiterEnd - (start + 2);
  std::string value =
      source.substr(bodyStart, end - bodyStart - delimiterLength - 2);
  current = end;
  if (collectCompletionExclusions)
    completionExclusions.emplace_back(start + 1, current);
  addToken(TokenKind::STRING_LITERAL, std::move(value));
  line += newlines;
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
