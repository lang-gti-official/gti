#include "gti/formatter.h"
#include "gti/lexer.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"
#include "gti/source_loader.h"
#include "gti/token.h"

#if defined(GTI_BUNDLED_JSON_C)
#include <json.h>
#else
#include <json-c/json.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

#if !defined(GTI_BUILD_STDLIB_PATH)
#define GTI_BUILD_STDLIB_PATH ""
#endif

#if !defined(GTI_VERSION)
#define GTI_VERSION "development"
#endif

struct Position {
  std::uint32_t line = 0;
  std::uint32_t character = 0;
};

struct SemanticToken {
  Position position;
  std::uint32_t length = 0;
  std::uint32_t type = 0;
  std::uint32_t modifiers = 0;
};

enum SemanticTokenType : std::uint32_t {
  Keyword,
  Type,
  Namespace,
  Class,
  Function,
  Method,
  Variable,
  Parameter,
  Property,
  String,
  Number,
  Operator,
  Macro,
  Decorator,
  Comment,
};

enum SemanticTokenModifier : std::uint32_t {
  Declaration = 1U << 0U,
  Definition = 1U << 1U,
  Readonly = 1U << 2U,
  DefaultLibrary = 1U << 3U,
};

struct SemanticClassification {
  std::uint32_t type = Variable;
  std::uint32_t modifiers = 0;
};

constexpr std::string_view diagnosticSource = "gti";

std::filesystem::path standardLibraryPath(const char *driver) {
  if (const char *configured = std::getenv("GTI_STDLIB_PATH");
      configured != nullptr && *configured != '\0') {
    return configured;
  }

  std::error_code error;
  std::filesystem::path executable = std::filesystem::absolute(driver, error);
  if (!error) {
    executable = std::filesystem::weakly_canonical(executable, error);
  }
  const std::filesystem::path installed =
      executable.parent_path().parent_path() /
      "share/gti/stdlib/prelude.gti";
  if (std::filesystem::exists(installed, error)) {
    return installed;
  }
  return GTI_BUILD_STDLIB_PATH;
}

std::size_t utf8SequenceLength(unsigned char byte) {
  if ((byte & 0x80U) == 0) {
    return 1;
  }
  if ((byte & 0xE0U) == 0xC0U) {
    return 2;
  }
  if ((byte & 0xF0U) == 0xE0U) {
    return 3;
  }
  if ((byte & 0xF8U) == 0xF0U) {
    return 4;
  }
  return 1;
}

std::uint32_t utf16Length(std::string_view text) {
  std::uint32_t length = 0;
  for (std::size_t index = 0; index < text.size();) {
    const std::size_t sequenceLength =
        std::min(utf8SequenceLength(static_cast<unsigned char>(text[index])),
                 text.size() - index);
    length += sequenceLength == 4 ? 2 : 1;
    index += sequenceLength;
  }
  return length;
}

Position positionAt(std::string_view source, std::size_t byteOffset) {
  Position position;
  const std::size_t limit = std::min(byteOffset, source.size());

  for (std::size_t index = 0; index < limit;) {
    if (source[index] == '\n') {
      ++position.line;
      position.character = 0;
      ++index;
      continue;
    }

    const std::size_t sequenceLength =
        std::min(utf8SequenceLength(static_cast<unsigned char>(source[index])),
                 limit - index);
    position.character += sequenceLength == 4 ? 2 : 1;
    index += sequenceLength;
  }
  return position;
}

json_object *positionJson(Position position) {
  json_object *result = json_object_new_object();
  json_object_object_add(result, "line", json_object_new_int64(position.line));
  json_object_object_add(result, "character",
                         json_object_new_int64(position.character));
  return result;
}

json_object *rangeJson(std::string_view source, std::size_t byteOffset,
                       std::size_t byteLength) {
  const std::size_t end = std::min(source.size(), byteOffset + byteLength);
  json_object *range = json_object_new_object();
  json_object_object_add(range, "start", positionJson(positionAt(source, byteOffset)));
  json_object_object_add(range, "end", positionJson(positionAt(source, end)));
  return range;
}

json_object *member(json_object *object, const char *name) {
  json_object *value = nullptr;
  return object != nullptr && json_object_object_get_ex(object, name, &value)
             ? value
             : nullptr;
}

std::string stringMember(json_object *object, const char *name) {
  json_object *value = member(object, name);
  return value != nullptr && json_object_is_type(value, json_type_string)
             ? json_object_get_string(value)
             : std::string{};
}

std::size_t sizeMember(json_object *object, const char *name,
                       std::size_t fallback) {
  json_object *value = member(object, name);
  if (value == nullptr || !json_object_is_type(value, json_type_int)) {
    return fallback;
  }
  const std::int64_t number = json_object_get_int64(value);
  return number > 0 ? static_cast<std::size_t>(number) : fallback;
}

bool boolMember(json_object *object, const char *name, bool fallback) {
  json_object *value = member(object, name);
  return value != nullptr && json_object_is_type(value, json_type_boolean)
             ? json_object_get_boolean(value) != 0
             : fallback;
}

void sendJson(json_object *message) {
  const char *json =
      json_object_to_json_string_ext(message, JSON_C_TO_STRING_PLAIN);
  const std::size_t length = std::char_traits<char>::length(json);
  std::cout << "Content-Length: " << length << "\r\n\r\n";
  std::cout.write(json, static_cast<std::streamsize>(length));
  std::cout.flush();
  json_object_put(message);
}

std::optional<std::string> readMessage() {
  std::string header;
  std::size_t contentLength = 0;
  bool hasContentLength = false;

  while (std::getline(std::cin, header)) {
    if (!header.empty() && header.back() == '\r') {
      header.pop_back();
    }
    if (header.empty()) {
      break;
    }

    const std::size_t colon = header.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string name = header.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char value) {
      return static_cast<char>(std::tolower(value));
    });
    if (name == "content-length") {
      try {
        contentLength = std::stoull(header.substr(colon + 1));
        hasContentLength = true;
      } catch (const std::exception &) {
        return std::nullopt;
      }
    }
  }

  if (!hasContentLength) {
    return std::nullopt;
  }

  std::string payload(contentLength, '\0');
  std::cin.read(payload.data(), static_cast<std::streamsize>(contentLength));
  if (static_cast<std::size_t>(std::cin.gcount()) != contentLength) {
    return std::nullopt;
  }
  return payload;
}

json_object *response(json_object *id, json_object *result) {
  json_object *message = json_object_new_object();
  json_object_object_add(message, "jsonrpc", json_object_new_string("2.0"));
  json_object_object_add(message, "id", json_object_get(id));
  json_object_object_add(message, "result", result);
  return message;
}

json_object *errorResponse(json_object *id, int code, std::string_view message) {
  json_object *error = json_object_new_object();
  json_object_object_add(error, "code", json_object_new_int(code));
  json_object_object_add(
      error, "message",
      json_object_new_string_len(message.data(), static_cast<int>(message.size())));

  json_object *result = json_object_new_object();
  json_object_object_add(result, "jsonrpc", json_object_new_string("2.0"));
  json_object_object_add(result, "id", json_object_get(id));
  json_object_object_add(result, "error", error);
  return result;
}

void appendDiagnostic(json_object *diagnostics, std::string_view source,
                      std::size_t position, std::size_t length,
                      std::string_view message) {
  json_object *diagnostic = json_object_new_object();
  json_object_object_add(diagnostic, "range",
                         rangeJson(source, position, length));
  json_object_object_add(diagnostic, "severity", json_object_new_int(1));
  json_object_object_add(
      diagnostic, "source",
      json_object_new_string_len(diagnosticSource.data(),
                                 static_cast<int>(diagnosticSource.size())));
  json_object_object_add(
      diagnostic, "message",
      json_object_new_string_len(message.data(), static_cast<int>(message.size())));
  json_object_array_add(diagnostics, diagnostic);
}

bool isOperator(lang::TokenKind kind) {
  using enum lang::TokenKind;
  switch (kind) {
  case AT:
  case LEFT_BRACKET:
  case RIGHT_BRACKET:
  case AND:
  case OR:
  case MINUS:
  case PLUS:
  case SLASH:
  case STAR:
  case BANG:
  case BANG_EQUAL:
  case EQUAL:
  case EQUAL_EQUAL:
  case GREATER:
  case GREATER_EQUAL:
  case LESS:
  case LESS_EQUAL:
  case MINUS_MINUS:
  case MINUS_EQUAL:
  case PLUS_PLUS:
  case PLUS_EQUAL:
  case SCOPE:
  case COLON:
    return true;
  default:
    return false;
  }
}

bool isKeyword(lang::TokenKind kind) {
  using enum lang::TokenKind;
  switch (kind) {
  case CLASS:
  case ELSE:
  case FALSE:
  case FOR:
  case IF:
  case INCLUDE:
  case MUT:
  case NAMESPACE:
  case PRIVATE:
  case PUBLIC:
  case RETURN:
  case STRUCT:
  case TRUE:
  case WHILE:
  case SELF:
  case NULLPTR:
  case UNEXPECTED:
    return true;
  default:
    return false;
  }
}

bool isTypeToken(lang::TokenKind kind) {
  using enum lang::TokenKind;
  return kind == INT || kind == INT8 || kind == INT16 || kind == INT32 ||
         kind == INT64 || kind == UINT || kind == UINT8 || kind == UINT16 ||
         kind == UINT32 || kind == UINT64 || kind == FLOAT || kind == BOOL ||
         kind == STRING_TYPE || kind == EXPECTED || kind == VOID;
}

bool isDirective(lang::TokenKind kind) {
  using enum lang::TokenKind;
  return kind == HASH_IF || kind == HASH_ELIF || kind == HASH_ELSE ||
         kind == HASH_ENDIF;
}

int hexDigit(char character) {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

std::optional<std::filesystem::path> filePathFromUri(std::string_view uri) {
  constexpr std::string_view prefix = "file://";
  if (!uri.starts_with(prefix)) {
    return std::nullopt;
  }

  std::string decoded;
  uri.remove_prefix(prefix.size());
  decoded.reserve(uri.size());
  for (std::size_t index = 0; index < uri.size(); ++index) {
    if (uri[index] == '%' && index + 2 < uri.size()) {
      const int high = hexDigit(uri[index + 1]);
      const int low = hexDigit(uri[index + 2]);
      if (high >= 0 && low >= 0) {
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2;
        continue;
      }
    }
    decoded.push_back(uri[index]);
  }

  return std::filesystem::path(decoded);
}

std::filesystem::path canonicalPath(const std::filesystem::path &path) {
  std::error_code error;
  std::filesystem::path absolute = std::filesystem::absolute(path, error);
  if (error) {
    absolute = path;
    error.clear();
  }
  std::filesystem::path canonical =
      std::filesystem::weakly_canonical(absolute, error);
  return error ? absolute.lexically_normal() : canonical;
}

void appendTokenDiagnostic(json_object *diagnostics, std::string_view rootSource,
                           std::string_view rootPath, const lang::Token &token,
                           std::string_view message) {
  if (token.source.empty() || token.source == rootPath) {
    appendDiagnostic(diagnostics, rootSource, token.position,
                     std::max<std::size_t>(token.lexeme.size(), 1), message);
    return;
  }

  const std::string dependencyMessage =
      token.source + ':' + std::to_string(token.line) + ": " +
      std::string(message);
  appendDiagnostic(diagnostics, rootSource, 0, 1, dependencyMessage);
}

std::optional<std::size_t>
typeEnd(const std::vector<lang::Token> &tokens, std::size_t start) {
  using enum lang::TokenKind;
  if (start >= tokens.size()) {
    return std::nullopt;
  }

  if (tokens[start].kind == EXPECTED) {
    if (start + 1 >= tokens.size() || tokens[start + 1].kind != LESS) {
      return std::nullopt;
    }
    const std::optional<std::size_t> valueEnd = typeEnd(tokens, start + 2);
    if (!valueEnd || *valueEnd >= tokens.size() ||
        tokens[*valueEnd].kind != COMMA) {
      return std::nullopt;
    }
    const std::optional<std::size_t> errorEnd = typeEnd(tokens, *valueEnd + 1);
    if (!errorEnd || *errorEnd >= tokens.size() ||
        tokens[*errorEnd].kind != GREATER) {
      return std::nullopt;
    }
    return *errorEnd + 1;
  }

  if (isTypeToken(tokens[start].kind)) {
    return start + 1;
  }
  if (tokens[start].kind != IDENTIFIER) {
    return std::nullopt;
  }

  std::size_t end = start + 1;
  while (end + 1 < tokens.size() && tokens[end].kind == SCOPE &&
         tokens[end + 1].kind == IDENTIFIER) {
    end += 2;
  }
  if (end >= tokens.size() || tokens[end].kind != LESS) {
    return end;
  }

  do {
    const std::optional<std::size_t> argumentEnd = typeEnd(tokens, end + 1);
    if (!argumentEnd) {
      return std::nullopt;
    }
    end = *argumentEnd;
  } while (end < tokens.size() && tokens[end].kind == COMMA);
  if (end >= tokens.size() || tokens[end].kind != GREATER) {
    return std::nullopt;
  }
  return end + 1;
}

std::optional<std::size_t>
matchingLeftAngle(const std::vector<lang::Token> &tokens, std::size_t right) {
  using enum lang::TokenKind;
  std::size_t depth = 0;
  for (std::size_t index = right + 1; index > 0; --index) {
    const std::size_t current = index - 1;
    if (tokens[current].kind == GREATER) {
      ++depth;
    } else if (tokens[current].kind == LESS && --depth == 0) {
      return current;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t>
declarationNameBefore(const std::vector<lang::Token> &tokens,
                      std::size_t delimiter) {
  using enum lang::TokenKind;
  if (delimiter == 0) {
    return std::nullopt;
  }
  std::size_t candidate = delimiter - 1;
  if (tokens[candidate].kind == GREATER) {
    const std::optional<std::size_t> left =
        matchingLeftAngle(tokens, candidate);
    if (!left || *left == 0) {
      return std::nullopt;
    }
    candidate = *left - 1;
  }
  return tokens[candidate].kind == IDENTIFIER
             ? std::optional<std::size_t>(candidate)
             : std::nullopt;
}

std::optional<std::size_t>
matchingRightParenthesis(const std::vector<lang::Token> &tokens,
                         std::size_t left) {
  using enum lang::TokenKind;
  std::size_t depth = 0;
  for (std::size_t index = left; index < tokens.size(); ++index) {
    if (tokens[index].kind == LEFT_PAREN) {
      ++depth;
    } else if (tokens[index].kind == RIGHT_PAREN && --depth == 0) {
      return index;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t>
matchingLeftParenthesis(const std::vector<lang::Token> &tokens,
                        std::size_t right) {
  using enum lang::TokenKind;
  std::size_t depth = 0;
  for (std::size_t index = right + 1; index > 0; --index) {
    const std::size_t current = index - 1;
    if (tokens[current].kind == RIGHT_PAREN) {
      ++depth;
    } else if (tokens[current].kind == LEFT_PAREN && --depth == 0) {
      return current;
    }
  }
  return std::nullopt;
}

struct ScopeDepth {
  std::size_t classes = 0;
  std::size_t functions = 0;
  std::string className;
};

enum class BraceKind { Block, Class, Function };

std::vector<ScopeDepth>
scopeDepths(const std::vector<lang::Token> &tokens) {
  using enum lang::TokenKind;
  std::vector<ScopeDepth> result(tokens.size());
  std::vector<BraceKind> stack;
  std::vector<std::string> classNames;
  ScopeDepth depth;

  for (std::size_t index = 0; index < tokens.size(); ++index) {
    if (tokens[index].kind == RIGHT_BRACE && !stack.empty()) {
      if (stack.back() == BraceKind::Class) {
        --depth.classes;
        classNames.pop_back();
      } else if (stack.back() == BraceKind::Function) {
        --depth.functions;
      }
      stack.pop_back();
    }

    depth.className = classNames.empty() ? std::string{} : classNames.back();
    result[index] = depth;
    if (tokens[index].kind != LEFT_BRACE) {
      continue;
    }

    BraceKind kind = BraceKind::Block;
    const std::optional<std::size_t> declarationName =
        declarationNameBefore(tokens, index);
    if (declarationName && *declarationName > 0 &&
        (tokens[*declarationName - 1].kind == CLASS ||
         tokens[*declarationName - 1].kind == STRUCT)) {
      kind = BraceKind::Class;
      ++depth.classes;
      classNames.emplace_back(tokens[*declarationName].lexeme);
    } else if (index > 0) {
      std::size_t signatureEnd = index - 1;
      if (tokens[signatureEnd].kind == MUT && signatureEnd > 0) {
        --signatureEnd;
      }
      if (tokens[signatureEnd].kind != RIGHT_PAREN) {
        stack.push_back(kind);
        continue;
      }
      const std::optional<std::size_t> left =
          matchingLeftParenthesis(tokens, signatureEnd);
      if (left && declarationNameBefore(tokens, *left)) {
        kind = BraceKind::Function;
        ++depth.functions;
      }
    }
    stack.push_back(kind);
  }
  return result;
}

bool isDefaultLibraryReference(const std::vector<lang::Token> &tokens,
                               std::size_t index) {
  using enum lang::TokenKind;
  std::size_t root = index;
  while (root >= 2 && tokens[root - 1].kind == SCOPE &&
         tokens[root - 2].kind == IDENTIFIER) {
    root -= 2;
  }
  return tokens[root].kind == IDENTIFIER && tokens[root].lexeme == "std";
}

std::optional<SemanticClassification>
basicSemanticType(const std::vector<lang::Token> &tokens, std::size_t index) {
  using enum lang::TokenKind;
  const lang::Token &token = tokens[index];

  if (isDirective(token.kind)) {
    return SemanticClassification{Macro, 0};
  }
  if (isKeyword(token.kind)) {
    return SemanticClassification{Keyword, 0};
  }
  if (isTypeToken(token.kind)) {
    return SemanticClassification{Type, 0};
  }
  if (token.kind == STRING_LITERAL) {
    return SemanticClassification{String, 0};
  }
  if (token.kind == INT_LITERAL || token.kind == FLOAT_LITERAL) {
    return SemanticClassification{Number, 0};
  }
  if (isOperator(token.kind)) {
    return SemanticClassification{Operator, 0};
  }
  if (token.kind != IDENTIFIER) {
    return std::nullopt;
  }

  if (token.lexeme == "discard" && index >= 2 && index + 2 < tokens.size() &&
      tokens[index - 2].kind == LEFT_BRACKET &&
      tokens[index - 1].kind == LEFT_BRACKET &&
      tokens[index + 1].kind == RIGHT_BRACKET &&
      tokens[index + 2].kind == RIGHT_BRACKET) {
    return SemanticClassification{Decorator, 0};
  }

  const lang::TokenKind previous =
      index > 0 ? tokens[index - 1].kind : END_OF_FILE;
  const lang::TokenKind next =
      index + 1 < tokens.size() ? tokens[index + 1].kind : END_OF_FILE;
  std::uint32_t modifiers =
      isDefaultLibraryReference(tokens, index) ? DefaultLibrary : 0;

  if (previous == CLASS || previous == STRUCT) {
    return SemanticClassification{Class, Declaration | Definition};
  }
  if (previous == AT) {
    return SemanticClassification{Decorator, 0};
  }
  if (previous == NAMESPACE) {
    return SemanticClassification{Namespace, Declaration | Definition};
  }
  if (previous == DOT) {
    return SemanticClassification{next == LEFT_PAREN ? Method : Property,
                                  modifiers};
  }
  if (token.lexeme == "target") {
    return SemanticClassification{Variable, Readonly};
  }
  if (previous == SCOPE && next == LEFT_PAREN) {
    return SemanticClassification{Function, modifiers};
  }
  if (next == LEFT_PAREN) {
    return SemanticClassification{Function, modifiers};
  }
  if (next == SCOPE) {
    return SemanticClassification{Namespace, modifiers};
  }
  if (previous == SCOPE) {
    return SemanticClassification{Variable, modifiers};
  }
  return SemanticClassification{Variable, modifiers};
}

void classifyType(const std::vector<lang::Token> &tokens,
                  std::vector<std::optional<SemanticClassification>> &types,
                  std::size_t start, std::size_t end) {
  using enum lang::TokenKind;
  for (std::size_t index = start; index < end; ++index) {
    if (tokens[index].kind == IDENTIFIER) {
      const std::uint32_t modifiers =
          isDefaultLibraryReference(tokens, index) ? DefaultLibrary : 0;
      types[index] = SemanticClassification{Type, modifiers};
    }
  }
}

std::optional<std::size_t>
genericParameterListEnd(const std::vector<lang::Token> &tokens,
                        std::size_t left) {
  using enum lang::TokenKind;
  if (left >= tokens.size() || tokens[left].kind != LESS) {
    return std::nullopt;
  }
  bool expectParameter = true;
  for (std::size_t index = left + 1; index < tokens.size(); ++index) {
    if (expectParameter && tokens[index].kind == IDENTIFIER) {
      expectParameter = false;
    } else if (!expectParameter && tokens[index].kind == COMMA) {
      expectParameter = true;
    } else if (!expectParameter && tokens[index].kind == GREATER) {
      return index + 1;
    } else {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t>
explicitTypeArgumentListEnd(const std::vector<lang::Token> &tokens,
                            std::size_t left) {
  using enum lang::TokenKind;
  if (left >= tokens.size() || tokens[left].kind != LESS) {
    return std::nullopt;
  }
  std::size_t next = left + 1;
  while (true) {
    const std::optional<std::size_t> argumentEnd = typeEnd(tokens, next);
    if (!argumentEnd || *argumentEnd >= tokens.size()) {
      return std::nullopt;
    }
    next = *argumentEnd;
    if (tokens[next].kind != COMMA) {
      break;
    }
    ++next;
  }
  return tokens[next].kind == GREATER && next + 1 < tokens.size() &&
                 tokens[next + 1].kind == LEFT_PAREN
             ? std::optional<std::size_t>(next + 1)
             : std::nullopt;
}

void classifyDeclarations(
    const std::vector<lang::Token> &tokens,
    std::vector<std::optional<SemanticClassification>> &types) {
  using enum lang::TokenKind;
  const std::vector<ScopeDepth> depths = scopeDepths(tokens);

  for (std::size_t index = 0; index < tokens.size(); ++index) {
    if (tokens[index].kind == LESS && index > 0 &&
        tokens[index - 1].kind == IDENTIFIER) {
      if (const std::optional<std::size_t> end =
              explicitTypeArgumentListEnd(tokens, index)) {
        classifyType(tokens, types, index + 1, *end);
      }
    }

    if (tokens[index].kind == IDENTIFIER && index > 0 &&
        (tokens[index - 1].kind == CLASS || tokens[index - 1].kind == STRUCT) &&
        index + 1 < tokens.size() && tokens[index + 1].kind == LESS) {
      const std::optional<std::size_t> end =
          genericParameterListEnd(tokens, index + 1);
      if (end) {
        for (std::size_t parameter = index + 2; parameter + 1 < *end;
             ++parameter) {
          if (tokens[parameter].kind == IDENTIFIER) {
            types[parameter] =
                SemanticClassification{Type, Declaration | Definition};
          }
        }
      }
    }

    if (tokens[index].kind == IDENTIFIER && depths[index].functions == 0 &&
        !depths[index].className.empty() &&
        tokens[index].lexeme == depths[index].className &&
        index + 1 < tokens.size() && tokens[index + 1].kind == LEFT_PAREN) {
      types[index] = SemanticClassification{Method, Declaration | Definition};
      continue;
    }

    const bool mutableBinding = tokens[index].kind == MUT;
    if (!mutableBinding && index > 0 && tokens[index - 1].kind == MUT) {
      continue;
    }
    const std::size_t typeStart = index + (mutableBinding ? 1 : 0);
    const std::optional<std::size_t> end = typeEnd(tokens, typeStart);
    if (!end || *end >= tokens.size() ||
        tokens[*end].kind != IDENTIFIER) {
      continue;
    }

    const std::size_t name = *end;
    std::size_t afterName = name + 1;
    if (afterName >= tokens.size()) {
      continue;
    }

    if (tokens[afterName].kind == LESS) {
      const std::optional<std::size_t> genericEnd =
          genericParameterListEnd(tokens, afterName);
      if (!genericEnd) {
        continue;
      }
      for (std::size_t parameter = afterName + 1; parameter + 1 < *genericEnd;
           ++parameter) {
        if (tokens[parameter].kind == IDENTIFIER) {
          types[parameter] =
              SemanticClassification{Type, Declaration | Definition};
        }
      }
      afterName = *genericEnd;
      if (afterName >= tokens.size()) {
        continue;
      }
    }

    if (tokens[afterName].kind == LEFT_PAREN) {
      const std::optional<std::size_t> right =
          matchingRightParenthesis(tokens, afterName);
      if (!right) {
        continue;
      }
      std::uint32_t modifiers = Declaration;
      const std::size_t bodyStart =
          *right + 1 < tokens.size() && tokens[*right + 1].kind == MUT
              ? *right + 2
              : *right + 1;
      if (bodyStart < tokens.size() && tokens[bodyStart].kind == LEFT_BRACE) {
        modifiers |= Definition;
      }
      const bool classMethod = depths[name].classes > 0 &&
                               depths[name].functions == 0;
      types[name] =
          SemanticClassification{classMethod ? Method : Function, modifiers};
      classifyType(tokens, types, typeStart, *end);
      continue;
    }

    const lang::TokenKind following = tokens[afterName].kind;
    const bool parameter = following == COMMA || following == RIGHT_PAREN;
    const bool variable = following == EQUAL || following == SEMICOLON;
    if (!parameter && !variable) {
      continue;
    }

    std::uint32_t modifiers = Declaration;
    if (!mutableBinding) {
      modifiers |= Readonly;
    }
    const bool classProperty = variable && depths[name].classes > 0 &&
                               depths[name].functions == 0;
    types[name] = SemanticClassification{
        parameter ? Parameter : (classProperty ? Property : Variable), modifiers};
    classifyType(tokens, types, typeStart, *end);
  }
}

void collectCommentTokens(std::string_view source,
                          std::vector<SemanticToken> &result) {
  for (std::size_t index = 0; index < source.size();) {
    if (source[index] == '"') {
      for (++index; index < source.size();) {
        if (source[index] == '\\' && index + 1 < source.size()) {
          index += 2;
        } else if (source[index++] == '"') {
          break;
        }
      }
      continue;
    }
    if (source[index] != '/' || index + 1 >= source.size() ||
        source[index + 1] != '/') {
      ++index;
      continue;
    }

    const std::size_t start = index;
    const std::size_t end = source.find('\n', start);
    index = end == std::string_view::npos ? source.size() : end;
    result.push_back({positionAt(source, start),
                      utf16Length(source.substr(start, index - start)), Comment,
                      0});
  }
}

std::vector<SemanticToken> collectSemanticTokens(std::string_view source) {
  lang::Lexer lexer;
  const std::vector<lang::Token> tokens = lexer.scan(std::string(source));
  std::vector<std::optional<SemanticClassification>> classifications;
  classifications.reserve(tokens.size());
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    classifications.push_back(basicSemanticType(tokens, index));
  }
  classifyDeclarations(tokens, classifications);

  std::vector<SemanticToken> result;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    if (!classifications[index] || tokens[index].lexeme.empty()) {
      continue;
    }

    const std::size_t tokenStart = tokens[index].position;
    const std::size_t tokenEnd = tokenStart + tokens[index].lexeme.size();
    for (std::size_t segmentStart = tokenStart; segmentStart < tokenEnd;) {
      const std::size_t newline = source.find('\n', segmentStart);
      const std::size_t segmentEnd =
          newline == std::string_view::npos || newline > tokenEnd ? tokenEnd
                                                                  : newline;
      if (segmentEnd > segmentStart) {
        result.push_back(
            {positionAt(source, segmentStart),
             utf16Length(
                 source.substr(segmentStart, segmentEnd - segmentStart)),
             classifications[index]->type, classifications[index]->modifiers});
      }
      segmentStart = segmentEnd < tokenEnd ? segmentEnd + 1 : tokenEnd;
    }
  }

  collectCommentTokens(source, result);
  std::sort(result.begin(), result.end(),
            [](const SemanticToken &left, const SemanticToken &right) {
              return left.position.line < right.position.line ||
                     (left.position.line == right.position.line &&
                      left.position.character < right.position.character);
            });
  return result;
}

json_object *semanticTokensJson(std::string_view source) {
  json_object *data = json_object_new_array();
  Position previous;

  for (const SemanticToken &token : collectSemanticTokens(source)) {
    const std::uint32_t deltaLine = token.position.line - previous.line;
    const std::uint32_t deltaCharacter =
        deltaLine == 0 ? token.position.character - previous.character
                       : token.position.character;
    json_object_array_add(data, json_object_new_int64(deltaLine));
    json_object_array_add(data, json_object_new_int64(deltaCharacter));
    json_object_array_add(data, json_object_new_int64(token.length));
    json_object_array_add(data, json_object_new_int64(token.type));
    json_object_array_add(data, json_object_new_int64(token.modifiers));
    previous = token.position;
  }

  json_object *result = json_object_new_object();
  json_object_object_add(result, "data", data);
  return result;
}

class LanguageServer {
public:
  explicit LanguageServer(std::filesystem::path standardLibrary)
      : standardLibrary(std::move(standardLibrary)) {}

  int run() {
    while (const std::optional<std::string> payload = readMessage()) {
      json_tokener *tokener = json_tokener_new();
      json_object *message = json_tokener_parse_ex(
          tokener, payload->data(), static_cast<int>(payload->size()));
      const json_tokener_error parseError = json_tokener_get_error(tokener);
      json_tokener_free(tokener);

      if (parseError != json_tokener_success || message == nullptr) {
        if (message != nullptr) {
          json_object_put(message);
        }
        continue;
      }

      if (handle(message)) {
        json_object_put(message);
        return shutdownRequested ? 0 : 1;
      }
      json_object_put(message);
    }
    return 0;
  }

private:
  bool handle(json_object *message) {
    const std::string method = stringMember(message, "method");
    json_object *id = member(message, "id");
    json_object *params = member(message, "params");

    if (method == "initialize") {
      sendJson(response(id, initializeResult()));
    } else if (method == "initialized") {
      return false;
    } else if (method == "shutdown") {
      shutdownRequested = true;
      sendJson(response(id, nullptr));
    } else if (method == "exit") {
      return true;
    } else if (method == "textDocument/didOpen") {
      didOpen(params);
    } else if (method == "textDocument/didChange") {
      didChange(params);
    } else if (method == "textDocument/didSave") {
      publishForDocument(stringMember(member(params, "textDocument"), "uri"));
    } else if (method == "textDocument/didClose") {
      didClose(params);
    } else if (method == "textDocument/semanticTokens/full") {
      semanticTokens(id, params);
    } else if (method == "textDocument/formatting") {
      documentFormatting(id, params);
    } else if (id != nullptr && !method.empty()) {
      sendJson(errorResponse(id, -32601, "Method not found"));
    }
    return false;
  }

  json_object *initializeResult() {
    json_object *sync = json_object_new_object();
    json_object_object_add(sync, "openClose", json_object_new_boolean(true));
    json_object_object_add(sync, "change", json_object_new_int(1));
    json_object_object_add(sync, "save", json_object_new_boolean(true));

    json_object *tokenTypes = json_object_new_array();
    for (const char *type : {"keyword", "type", "namespace", "class",
                             "function", "method", "variable", "parameter",
                             "property", "string", "number", "operator",
                             "macro", "decorator", "comment"}) {
      json_object_array_add(tokenTypes, json_object_new_string(type));
    }
    json_object *tokenModifiers = json_object_new_array();
    for (const char *modifier : {"declaration", "definition", "readonly",
                                 "defaultLibrary"}) {
      json_object_array_add(tokenModifiers, json_object_new_string(modifier));
    }
    json_object *legend = json_object_new_object();
    json_object_object_add(legend, "tokenTypes", tokenTypes);
    json_object_object_add(legend, "tokenModifiers", tokenModifiers);

    json_object *semanticTokens = json_object_new_object();
    json_object_object_add(semanticTokens, "legend", legend);
    json_object_object_add(semanticTokens, "full", json_object_new_boolean(true));

    json_object *capabilities = json_object_new_object();
    json_object_object_add(capabilities, "positionEncoding",
                           json_object_new_string("utf-16"));
    json_object_object_add(capabilities, "textDocumentSync", sync);
    json_object_object_add(capabilities, "semanticTokensProvider",
                           semanticTokens);
    json_object_object_add(capabilities, "documentFormattingProvider",
                           json_object_new_boolean(true));

    json_object *serverInfo = json_object_new_object();
    json_object_object_add(serverInfo, "name", json_object_new_string("gti_lsp"));
    json_object_object_add(serverInfo, "version",
                           json_object_new_string(GTI_VERSION));

    json_object *result = json_object_new_object();
    json_object_object_add(result, "capabilities", capabilities);
    json_object_object_add(result, "serverInfo", serverInfo);
    return result;
  }

  void didOpen(json_object *params) {
    json_object *document = member(params, "textDocument");
    const std::string uri = stringMember(document, "uri");
    if (uri.empty()) {
      return;
    }
    documents[uri] = stringMember(document, "text");
    publishForDocument(uri);
  }

  void didChange(json_object *params) {
    const std::string uri =
        stringMember(member(params, "textDocument"), "uri");
    json_object *changes = member(params, "contentChanges");
    if (uri.empty() || changes == nullptr ||
        !json_object_is_type(changes, json_type_array)) {
      return;
    }

    const std::size_t count = json_object_array_length(changes);
    if (count > 0) {
      documents[uri] =
          stringMember(json_object_array_get_idx(changes, count - 1), "text");
      publishForDocument(uri);
    }
  }

  void didClose(json_object *params) {
    const std::string uri =
        stringMember(member(params, "textDocument"), "uri");
    documents.erase(uri);
    publishDiagnostics(uri, json_object_new_array());
  }

  void semanticTokens(json_object *id, json_object *params) {
    const std::string uri =
        stringMember(member(params, "textDocument"), "uri");
    const auto document = documents.find(uri);
    sendJson(response(id, semanticTokensJson(
                              document == documents.end() ? std::string_view{}
                                                          : document->second)));
  }

  void documentFormatting(json_object *id, json_object *params) {
    json_object *edits = json_object_new_array();
    const std::string uri =
        stringMember(member(params, "textDocument"), "uri");
    const auto document = documents.find(uri);
    if (document == documents.end()) {
      sendJson(response(id, edits));
      return;
    }

    json_object *formatOptions = member(params, "options");
    const lang::FormatOptions options{
        .indentWidth = std::min<std::size_t>(
            sizeMember(formatOptions, "tabSize", 2), 16),
        .insertSpaces = boolMember(formatOptions, "insertSpaces", true),
    };
    const std::string formatted = lang::Formatter(options).format(document->second);
    if (formatted == document->second) {
      sendJson(response(id, edits));
      return;
    }

    json_object *edit = json_object_new_object();
    json_object_object_add(
        edit, "range",
        rangeJson(document->second, 0, document->second.size()));
    json_object_object_add(
        edit, "newText",
        json_object_new_string_len(formatted.data(),
                                   static_cast<int>(formatted.size())));
    json_object_array_add(edits, edit);
    sendJson(response(id, edits));
  }

  void publishForDocument(const std::string &uri) {
    const auto document = documents.find(uri);
    if (document == documents.end()) {
      return;
    }

    const std::string &source = document->second;
    json_object *diagnostics = json_object_new_array();
    const std::optional<std::filesystem::path> filePath = filePathFromUri(uri);
    if (!filePath) {
      appendDiagnostic(diagnostics, source, 0, 1,
                       "GTI source dependencies require a file URI.");
      publishDiagnostics(uri, diagnostics);
      return;
    }

    const std::string rootPath = canonicalPath(*filePath).string();
    lang::SourceLoader sourceLoader;
    std::vector<lang::Token> tokens =
        sourceLoader.load(*filePath, source, {standardLibrary});
    for (const lang::SourceDiagnostic &diagnostic : sourceLoader.errors()) {
      appendTokenDiagnostic(diagnostics, source, rootPath, diagnostic.token,
                            diagnostic.message);
    }

    if (!sourceLoader.hadError()) {
      lang::Parser parser(std::move(tokens));
      lang::Program program = parser.parse();
      for (const lang::ParseDiagnostic &diagnostic : parser.errors()) {
        appendTokenDiagnostic(diagnostics, source, rootPath, diagnostic.token,
                              diagnostic.message);
      }

      if (!parser.hadError()) {
        lang::SemanticVisitor semantic;
        semantic.check(program);
        for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
          appendTokenDiagnostic(diagnostics, source, rootPath, diagnostic.token,
                                diagnostic.message);
        }
      }
    }

    publishDiagnostics(uri, diagnostics);
  }

  void publishDiagnostics(const std::string &uri, json_object *diagnostics) {
    json_object *params = json_object_new_object();
    json_object_object_add(params, "uri", json_object_new_string(uri.c_str()));
    json_object_object_add(params, "diagnostics", diagnostics);

    json_object *notification = json_object_new_object();
    json_object_object_add(notification, "jsonrpc",
                           json_object_new_string("2.0"));
    json_object_object_add(notification, "method",
                           json_object_new_string("textDocument/publishDiagnostics"));
    json_object_object_add(notification, "params", params);
    sendJson(notification);
  }

  std::unordered_map<std::string, std::string> documents;
  std::filesystem::path standardLibrary;
  bool shutdownRequested = false;
};

} // namespace

int main(int argc, char *argv[]) {
  return LanguageServer(standardLibraryPath(argc > 0 ? argv[0] : "gti_lsp"))
      .run();
}
