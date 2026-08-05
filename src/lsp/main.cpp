#include "gti/lexer.h"
#include "gti/parser.h"
#include "gti/semantic_analyzer.h"
#include "gti/source_loader.h"
#include "gti/token.h"

#include <json-c/json.h>

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

struct Position {
  std::uint32_t line = 0;
  std::uint32_t character = 0;
};

struct SemanticToken {
  Position position;
  std::uint32_t length = 0;
  std::uint32_t type = 0;
};

enum SemanticTokenType : std::uint32_t {
  Keyword,
  Type,
  Namespace,
  Class,
  Function,
  Variable,
  Property,
  String,
  Number,
  Operator,
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
  case RETURN:
  case TRUE:
  case WHILE:
  case SELF:
  case NULLPTR:
    return true;
  default:
    return false;
  }
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

std::optional<std::uint32_t>
semanticType(const std::vector<lang::Token> &tokens, std::size_t index) {
  using enum lang::TokenKind;
  const lang::Token &token = tokens[index];

  if (isKeyword(token.kind)) {
    return Keyword;
  }
  if (token.kind == INT || token.kind == FLOAT || token.kind == BOOL ||
      token.kind == VOID) {
    return Type;
  }
  if (token.kind == STRING_LITERAL) {
    return String;
  }
  if (token.kind == INT_LITERAL || token.kind == FLOAT_LITERAL) {
    return Number;
  }
  if (isOperator(token.kind)) {
    return Operator;
  }
  if (token.kind != IDENTIFIER) {
    return std::nullopt;
  }

  const lang::TokenKind previous =
      index > 0 ? tokens[index - 1].kind : END_OF_FILE;
  const lang::TokenKind next =
      index + 1 < tokens.size() ? tokens[index + 1].kind : END_OF_FILE;

  if (previous == CLASS) {
    return Class;
  }
  if (previous == NAMESPACE) {
    return Namespace;
  }
  if (previous == DOT) {
    return Property;
  }
  if (next == LEFT_PAREN) {
    return Function;
  }
  if (previous == SCOPE || next == SCOPE) {
    return Namespace;
  }
  if (next == IDENTIFIER) {
    return Type;
  }
  return Variable;
}

std::vector<SemanticToken> collectSemanticTokens(std::string_view source) {
  lang::Lexer lexer;
  const std::vector<lang::Token> tokens = lexer.scan(std::string(source));
  std::vector<SemanticToken> result;

  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const std::optional<std::uint32_t> type = semanticType(tokens, index);
    if (!type || tokens[index].lexeme.empty()) {
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
        result.push_back({positionAt(source, segmentStart),
                          utf16Length(source.substr(
                              segmentStart, segmentEnd - segmentStart)),
                          *type});
      }
      segmentStart = segmentEnd < tokenEnd ? segmentEnd + 1 : tokenEnd;
    }
  }
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
    json_object_array_add(data, json_object_new_int(0));
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
                             "function", "variable", "property", "string",
                             "number", "operator"}) {
      json_object_array_add(tokenTypes, json_object_new_string(type));
    }
    json_object *legend = json_object_new_object();
    json_object_object_add(legend, "tokenTypes", tokenTypes);
    json_object_object_add(legend, "tokenModifiers", json_object_new_array());

    json_object *semanticTokens = json_object_new_object();
    json_object_object_add(semanticTokens, "legend", legend);
    json_object_object_add(semanticTokens, "full", json_object_new_boolean(true));

    json_object *capabilities = json_object_new_object();
    json_object_object_add(capabilities, "positionEncoding",
                           json_object_new_string("utf-16"));
    json_object_object_add(capabilities, "textDocumentSync", sync);
    json_object_object_add(capabilities, "semanticTokensProvider",
                           semanticTokens);

    json_object *serverInfo = json_object_new_object();
    json_object_object_add(serverInfo, "name", json_object_new_string("gti_lsp"));
    json_object_object_add(serverInfo, "version", json_object_new_string("0.1.0"));

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
