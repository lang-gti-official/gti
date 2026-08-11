#include "gti/format_config.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/lexer.h"
#include "gti/standard_library.h"
#include "gti/token.h"

#if defined(GTI_BUNDLED_JSON_C)
#include <json.h>
#else
#include <json-c/json.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

#if !defined(GTI_BUILD_STDLIB_ROOT)
#define GTI_BUILD_STDLIB_ROOT ""
#endif

#if !defined(GTI_VERSION)
#define GTI_VERSION "development"
#endif

struct Position {
  std::uint32_t line = 0;
  std::uint32_t character = 0;
};

struct LspRange {
  Position start;
  Position end;
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
  TypeParameter,
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
  EnumMember,
  Struct,
  Enum,
};

enum SemanticTokenModifier : std::uint32_t {
  Declaration = 1U << 0U,
  Definition = 1U << 1U,
  Readonly = 1U << 2U,
  DefaultLibrary = 1U << 3U,
  FunctionScope = 1U << 4U,
  Static = 1U << 5U,
};

struct SemanticClassification {
  std::uint32_t type = Variable;
  std::uint32_t modifiers = 0;
};

struct LspRelatedDiagnostic {
  lang::RelatedDiagnostic related;
  std::string uri;
  std::string source;
};

struct LspFixIt {
  lang::FixIt fix;
  std::string uri;
  std::string source;
};

struct LspDiagnostic {
  lang::Diagnostic diagnostic;
  std::string uri;
  std::string source;
  std::vector<LspRelatedDiagnostic> related;
  std::vector<LspFixIt> fixes;
};

struct CodeActionCandidate {
  LspDiagnostic diagnostic;
  LspFixIt fix;
  std::optional<std::int64_t> version;
  bool preferred = false;
};

constexpr std::string_view diagnosticSource = "gti";
std::mutex outputMutex;

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

class SourcePositionIndex {
public:
  explicit SourcePositionIndex(std::string_view source) : source(source) {
    lineStarts.push_back(0);
    for (std::size_t index = 0; index < source.size(); ++index) {
      if (source[index] == '\n') {
        lineStarts.push_back(index + 1);
      }
    }
  }

  [[nodiscard]] Position at(std::size_t byteOffset) const {
    const std::size_t limit = std::min(byteOffset, source.size());
    const auto next =
        std::upper_bound(lineStarts.begin(), lineStarts.end(), limit);
    const std::size_t line =
        static_cast<std::size_t>(std::distance(lineStarts.begin(), next) - 1);
    return Position{.line = static_cast<std::uint32_t>(line),
                    .character = utf16Length(source.substr(
                        lineStarts[line], limit - lineStarts[line]))};
  }

  [[nodiscard]] std::optional<std::size_t> byteOffset(Position position) const {
    if (position.line >= lineStarts.size()) {
      return std::nullopt;
    }
    const std::size_t lineStart = lineStarts[position.line];
    std::size_t lineEnd = position.line + 1 < lineStarts.size()
                              ? lineStarts[position.line + 1] - 1
                              : source.size();
    if (lineEnd > lineStart && source[lineEnd - 1] == '\r') {
      --lineEnd;
    }

    std::uint32_t character = 0;
    for (std::size_t index = lineStart; index < lineEnd;) {
      if (character == position.character) {
        return index;
      }
      const std::size_t sequenceLength = std::min(
          utf8SequenceLength(static_cast<unsigned char>(source[index])),
          lineEnd - index);
      const std::uint32_t codeUnits = sequenceLength == 4 ? 2 : 1;
      if (position.character < character + codeUnits) {
        return std::nullopt;
      }
      character += codeUnits;
      index += sequenceLength;
    }
    return character == position.character ? std::optional<std::size_t>(lineEnd)
                                           : std::nullopt;
  }

private:
  std::string_view source;
  std::vector<std::size_t> lineStarts;
};

Position positionAt(std::string_view source, std::size_t byteOffset) {
  return SourcePositionIndex(source).at(byteOffset);
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
  json_object_object_add(range, "start",
                         positionJson(positionAt(source, byteOffset)));
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

std::optional<std::int64_t> integerMember(json_object *object,
                                          const char *name) {
  json_object *value = member(object, name);
  if (value == nullptr || !json_object_is_type(value, json_type_int)) {
    return std::nullopt;
  }
  return json_object_get_int64(value);
}

std::optional<Position> positionMember(json_object *object) {
  const std::optional<std::int64_t> line = integerMember(object, "line");
  const std::optional<std::int64_t> character =
      integerMember(object, "character");
  if (!line || !character || *line < 0 || *character < 0 ||
      *line > std::numeric_limits<std::uint32_t>::max() ||
      *character > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return Position{.line = static_cast<std::uint32_t>(*line),
                  .character = static_cast<std::uint32_t>(*character)};
}

std::optional<LspRange> rangeMember(json_object *object) {
  const std::optional<Position> start = positionMember(member(object, "start"));
  const std::optional<Position> end = positionMember(member(object, "end"));
  if (!start || !end) {
    return std::nullopt;
  }
  return LspRange{.start = *start, .end = *end};
}

bool samePosition(Position left, Position right) {
  return left.line == right.line && left.character == right.character;
}

bool positionBefore(Position left, Position right) {
  return left.line < right.line ||
         (left.line == right.line && left.character < right.character);
}

bool validRange(const LspRange &range) {
  return !positionBefore(range.end, range.start);
}

bool rangesIntersect(const LspRange &left, const LspRange &right) {
  if (!validRange(left) || !validRange(right)) {
    return false;
  }
  if (samePosition(left.start, left.end)) {
    return !positionBefore(left.start, right.start) &&
           !positionBefore(right.end, left.start);
  }
  if (samePosition(right.start, right.end)) {
    return !positionBefore(right.start, left.start) &&
           !positionBefore(left.end, right.start);
  }
  return positionBefore(left.start, right.end) &&
         positionBefore(right.start, left.end);
}

bool boolMember(json_object *object, const char *name, bool fallback) {
  json_object *value = member(object, name);
  return value != nullptr && json_object_is_type(value, json_type_boolean)
             ? json_object_get_boolean(value) != 0
             : fallback;
}

std::string requestIdKey(json_object *id) {
  if (id == nullptr || (!json_object_is_type(id, json_type_int) &&
                        !json_object_is_type(id, json_type_string))) {
    return {};
  }
  return json_object_to_json_string_ext(id, JSON_C_TO_STRING_PLAIN);
}

bool supportsHoverFormat(json_object *params, std::string_view format) {
  json_object *contentFormats = member(
      member(member(member(params, "capabilities"), "textDocument"), "hover"),
      "contentFormat");
  if (contentFormats == nullptr ||
      !json_object_is_type(contentFormats, json_type_array)) {
    return false;
  }
  const std::size_t count = json_object_array_length(contentFormats);
  for (std::size_t index = 0; index < count; ++index) {
    json_object *candidate = json_object_array_get_idx(contentFormats, index);
    if (candidate != nullptr &&
        json_object_is_type(candidate, json_type_string) &&
        format == json_object_get_string(candidate)) {
      return true;
    }
  }
  return false;
}

bool supportsCompletionSnippets(json_object *params) {
  json_object *completionItem =
      member(member(member(member(params, "capabilities"), "textDocument"),
                    "completion"),
             "completionItem");
  return boolMember(completionItem, "snippetSupport", false);
}

bool supportsSemanticTokenRefresh(json_object *params) {
  json_object *semanticTokens = member(
      member(member(params, "capabilities"), "workspace"), "semanticTokens");
  return boolMember(semanticTokens, "refreshSupport", false);
}

bool supportsWorkspaceDocumentChanges(json_object *params) {
  json_object *workspaceEdit = member(
      member(member(params, "capabilities"), "workspace"), "workspaceEdit");
  return boolMember(workspaceEdit, "documentChanges", false);
}

bool supportsWatchedFilesDynamicRegistration(json_object *params) {
  json_object *watchedFiles =
      member(member(member(params, "capabilities"), "workspace"),
             "didChangeWatchedFiles");
  return boolMember(watchedFiles, "dynamicRegistration", false);
}

json_object *publishDiagnosticsCapabilities(json_object *params) {
  return member(member(member(params, "capabilities"), "textDocument"),
                "publishDiagnostics");
}

bool supportsDiagnosticRelatedInformation(json_object *params) {
  return boolMember(publishDiagnosticsCapabilities(params),
                    "relatedInformation", false);
}

bool supportsDiagnosticData(json_object *params) {
  return boolMember(publishDiagnosticsCapabilities(params), "dataSupport",
                    false);
}

json_object *codeActionCapabilities(json_object *params) {
  return member(member(member(params, "capabilities"), "textDocument"),
                "codeAction");
}

bool supportsCodeActionLiterals(json_object *params) {
  json_object *literalSupport =
      member(codeActionCapabilities(params), "codeActionLiteralSupport");
  json_object *valueSet =
      member(member(literalSupport, "codeActionKind"), "valueSet");
  return literalSupport != nullptr &&
         json_object_is_type(literalSupport, json_type_object) &&
         valueSet != nullptr && json_object_is_type(valueSet, json_type_array);
}

bool supportsPreferredCodeActions(json_object *params) {
  return boolMember(codeActionCapabilities(params), "isPreferredSupport",
                    false);
}

bool contextAllowsQuickFix(json_object *context) {
  json_object *only = member(context, "only");
  if (only == nullptr || !json_object_is_type(only, json_type_array)) {
    return true;
  }
  const std::size_t count = json_object_array_length(only);
  for (std::size_t index = 0; index < count; ++index) {
    json_object *kind = json_object_array_get_idx(only, index);
    if (kind != nullptr && json_object_is_type(kind, json_type_string) &&
        std::string_view(json_object_get_string(kind)) == "quickfix") {
      return true;
    }
  }
  return false;
}

void sendJson(json_object *message) {
  const std::lock_guard lock(outputMutex);
  const char *json =
      json_object_to_json_string_ext(message, JSON_C_TO_STRING_PLAIN);
  const std::size_t length = std::char_traits<char>::length(json);
  std::cout << "Content-Length: " << length << "\r\n\r\n";
  std::cout.write(json, static_cast<std::streamsize>(length));
  std::cout.flush();
  json_object_put(message);
}

std::optional<std::string> readMessage() {
  constexpr std::size_t maxContentLength = 16U * 1024U * 1024U;
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
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char value) {
                     return static_cast<char>(std::tolower(value));
                   });
    if (name == "content-length") {
      std::string_view value(header);
      value.remove_prefix(colon + 1);
      while (!value.empty() &&
             std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
      }
      while (!value.empty() &&
             std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
      }
      std::size_t parsed = 0;
      const auto [end, error] =
          std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (hasContentLength || value.empty() || error != std::errc{} ||
          end != value.data() + value.size() || parsed > maxContentLength) {
        return std::nullopt;
      }
      contentLength = parsed;
      hasContentLength = true;
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

json_object *errorResponse(json_object *id, int code,
                           std::string_view message) {
  json_object *error = json_object_new_object();
  json_object_object_add(error, "code", json_object_new_int(code));
  json_object_object_add(error, "message",
                         json_object_new_string_len(
                             message.data(), static_cast<int>(message.size())));

  json_object *result = json_object_new_object();
  json_object_object_add(result, "jsonrpc", json_object_new_string("2.0"));
  json_object_object_add(result, "id", json_object_get(id));
  json_object_object_add(result, "error", error);
  return result;
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

std::string fileUriFromPath(const std::filesystem::path &path) {
  const std::string text = canonicalPath(path).generic_string();
  constexpr char hex[] = "0123456789ABCDEF";
  std::string uri = "file://";
  for (unsigned char character : text) {
    const bool unreserved = std::isalnum(character) != 0 || character == '-' ||
                            character == '_' || character == '.' ||
                            character == '~' || character == '/' ||
                            character == ':';
    if (unreserved) {
      uri += static_cast<char>(character);
    } else {
      uri += '%';
      uri += hex[character >> 4U];
      uri += hex[character & 0x0FU];
    }
  }
  return uri;
}

std::string documentKeyFromUri(std::string_view uri) {
  const std::optional<std::filesystem::path> path = filePathFromUri(uri);
  return path ? fileUriFromPath(*path) : std::string(uri);
}

std::string uriForSource(std::string_view source, std::string_view rootPath,
                         std::string_view rootUri) {
  return source.empty() || source == rootPath
             ? std::string(rootUri)
             : fileUriFromPath(std::filesystem::path(source));
}

std::string sourceForSpan(const lang::SourceSpan &span,
                          const lang::SourceManager &sources,
                          std::string_view rootPath,
                          std::string_view rootSource) {
  if (const std::string *source = sources.find(span.source)) {
    return *source;
  }
  return span.source.empty() || span.source == rootPath
             ? std::string(rootSource)
             : std::string{};
}

LspDiagnostic convertDiagnostic(const lang::Diagnostic &diagnostic,
                                const lang::SourceManager &sources,
                                std::string_view rootPath,
                                std::string_view rootUri,
                                std::string_view rootSource) {
  LspDiagnostic result{
      .diagnostic = diagnostic,
      .uri = uriForSource(diagnostic.primary.source, rootPath, rootUri),
      .source =
          sourceForSpan(diagnostic.primary, sources, rootPath, rootSource)};
  result.related.reserve(diagnostic.related.size());
  for (const lang::RelatedDiagnostic &related : diagnostic.related) {
    result.related.push_back(
        {.related = related,
         .uri = uriForSource(related.span.source, rootPath, rootUri),
         .source = sourceForSpan(related.span, sources, rootPath, rootSource)});
  }
  result.fixes.reserve(diagnostic.fixes.size());
  for (const lang::FixIt &fix : diagnostic.fixes) {
    result.fixes.push_back(
        {.fix = fix,
         .uri = uriForSource(fix.span.source, rootPath, rootUri),
         .source = sourceForSpan(fix.span, sources, rootPath, rootSource)});
  }
  return result;
}

std::string diagnosticMessage(const lang::Diagnostic &diagnostic) {
  std::string message = diagnostic.message;
  for (const std::string &hint : diagnostic.hints) {
    message += "\nhelp: " + hint;
  }
  return message;
}

json_object *diagnosticJson(const LspDiagnostic &published,
                            bool includeRelatedInformation, bool includeData) {
  const lang::Diagnostic &diagnostic = published.diagnostic;
  json_object *result = json_object_new_object();
  json_object_object_add(
      result, "range",
      rangeJson(published.source, diagnostic.primary.start,
                diagnostic.primary.end >= diagnostic.primary.start
                    ? diagnostic.primary.end - diagnostic.primary.start
                    : 0));
  json_object_object_add(
      result, "severity",
      json_object_new_int(static_cast<int>(diagnostic.severity)));
  if (!diagnostic.code.empty()) {
    json_object_object_add(result, "code",
                           json_object_new_string(diagnostic.code.c_str()));
  }
  json_object_object_add(
      result, "source",
      json_object_new_string_len(diagnosticSource.data(),
                                 static_cast<int>(diagnosticSource.size())));

  const std::string message = diagnosticMessage(diagnostic);
  json_object_object_add(result, "message",
                         json_object_new_string(message.c_str()));

  if (includeRelatedInformation && !published.related.empty()) {
    json_object *relatedInformation = json_object_new_array();
    for (const LspRelatedDiagnostic &related : published.related) {
      json_object *location = json_object_new_object();
      json_object_object_add(location, "uri",
                             json_object_new_string(related.uri.c_str()));
      json_object_object_add(
          location, "range",
          rangeJson(related.source, related.related.span.start,
                    related.related.span.end >= related.related.span.start
                        ? related.related.span.end - related.related.span.start
                        : 0));
      json_object *information = json_object_new_object();
      json_object_object_add(information, "location", location);
      json_object_object_add(
          information, "message",
          json_object_new_string(related.related.message.c_str()));
      json_object_array_add(relatedInformation, information);
    }
    json_object_object_add(result, "relatedInformation", relatedInformation);
  }
  if (includeData) {
    json_object *data = json_object_new_object();
    const std::string_view phase = lang::phaseName(diagnostic.phase);
    json_object_object_add(data, "phase",
                           json_object_new_string_len(
                               phase.data(), static_cast<int>(phase.size())));
    if (!diagnostic.hints.empty()) {
      json_object *hints = json_object_new_array();
      for (const std::string &hint : diagnostic.hints) {
        json_object_array_add(hints, json_object_new_string(hint.c_str()));
      }
      json_object_object_add(data, "hints", hints);
    }
    if (!published.fixes.empty()) {
      json_object *fixes = json_object_new_array();
      for (const LspFixIt &fix : published.fixes) {
        json_object *edit = json_object_new_object();
        json_object_object_add(edit, "uri",
                               json_object_new_string(fix.uri.c_str()));
        json_object_object_add(
            edit, "range",
            rangeJson(fix.source, fix.fix.span.start,
                      fix.fix.span.end >= fix.fix.span.start
                          ? fix.fix.span.end - fix.fix.span.start
                          : 0));
        json_object_object_add(
            edit, "replacement",
            json_object_new_string(fix.fix.replacement.c_str()));
        json_object_object_add(edit, "message",
                               json_object_new_string(fix.fix.message.c_str()));
        json_object_array_add(fixes, edit);
      }
      json_object_object_add(data, "fixes", fixes);
    }
    json_object_object_add(result, "data", data);
  }
  return result;
}

bool diagnosticIntersects(const LspDiagnostic &diagnostic,
                          const LspRange &requestedRange) {
  const lang::SourceSpan &span = diagnostic.diagnostic.primary;
  const LspRange diagnosticRange{
      .start = positionAt(diagnostic.source, span.start),
      .end = positionAt(diagnostic.source, span.end)};
  return rangesIntersect(diagnosticRange, requestedRange);
}

bool diagnosticRequested(const LspDiagnostic &diagnostic,
                         json_object *requestedDiagnostics) {
  if (requestedDiagnostics == nullptr ||
      !json_object_is_type(requestedDiagnostics, json_type_array)) {
    return false;
  }
  const Position expectedStart =
      positionAt(diagnostic.source, diagnostic.diagnostic.primary.start);
  const Position expectedEnd =
      positionAt(diagnostic.source, diagnostic.diagnostic.primary.end);
  const std::size_t count = json_object_array_length(requestedDiagnostics);
  for (std::size_t index = 0; index < count; ++index) {
    json_object *requested =
        json_object_array_get_idx(requestedDiagnostics, index);
    const std::optional<LspRange> range =
        rangeMember(member(requested, "range"));
    if (!range || !samePosition(range->start, expectedStart) ||
        !samePosition(range->end, expectedEnd)) {
      continue;
    }
    if (stringMember(requested, "code") != diagnostic.diagnostic.code) {
      continue;
    }
    if (stringMember(requested, "source") != diagnosticSource) {
      continue;
    }
    if (stringMember(requested, "message") !=
        diagnosticMessage(diagnostic.diagnostic)) {
      continue;
    }
    return true;
  }
  return false;
}

json_object *codeActionJson(const CodeActionCandidate &candidate,
                            bool documentChanges,
                            bool includeRelatedInformation,
                            bool includeDiagnosticData, bool includePreferred) {
  const std::string title =
      candidate.fix.fix.message.empty()
          ? (candidate.diagnostic.diagnostic.code.empty()
                 ? "Apply suggested fix"
                 : "Apply fix for " + candidate.diagnostic.diagnostic.code)
          : candidate.fix.fix.message;
  json_object *action = json_object_new_object();
  json_object_object_add(action, "title",
                         json_object_new_string(title.c_str()));
  json_object_object_add(action, "kind", json_object_new_string("quickfix"));
  json_object *diagnostics = json_object_new_array();
  json_object_array_add(diagnostics, diagnosticJson(candidate.diagnostic,
                                                    includeRelatedInformation,
                                                    includeDiagnosticData));
  json_object_object_add(action, "diagnostics", diagnostics);
  if (includePreferred) {
    json_object_object_add(action, "isPreferred",
                           json_object_new_boolean(candidate.preferred));
  }

  json_object *textEdit = json_object_new_object();
  json_object_object_add(
      textEdit, "range",
      rangeJson(candidate.fix.source, candidate.fix.fix.span.start,
                candidate.fix.fix.span.end >= candidate.fix.fix.span.start
                    ? candidate.fix.fix.span.end - candidate.fix.fix.span.start
                    : 0));
  json_object_object_add(
      textEdit, "newText",
      json_object_new_string(candidate.fix.fix.replacement.c_str()));
  json_object *edits = json_object_new_array();
  json_object_array_add(edits, textEdit);

  json_object *workspaceEdit = json_object_new_object();
  if (documentChanges) {
    json_object *textDocument = json_object_new_object();
    json_object_object_add(textDocument, "uri",
                           json_object_new_string(candidate.fix.uri.c_str()));
    json_object_object_add(textDocument, "version",
                           candidate.version
                               ? json_object_new_int64(*candidate.version)
                               : json_object_new_null());
    json_object *documentEdit = json_object_new_object();
    json_object_object_add(documentEdit, "textDocument", textDocument);
    json_object_object_add(documentEdit, "edits", edits);
    json_object *changes = json_object_new_array();
    json_object_array_add(changes, documentEdit);
    json_object_object_add(workspaceEdit, "documentChanges", changes);
  } else {
    json_object *changes = json_object_new_object();
    json_object_object_add(changes, candidate.fix.uri.c_str(), edits);
    json_object_object_add(workspaceEdit, "changes", changes);
  }
  json_object_object_add(action, "edit", workspaceEdit);
  return action;
}

std::optional<SemanticClassification>
basicSemanticType(const std::vector<lang::Token> &tokens, std::size_t index) {
  using enum lang::TokenKind;
  const lang::Token &token = tokens[index];

  // Tree-sitter has more precise captures for these syntax-owned tokens than
  // the LSP semantic-token vocabulary. Emitting them as keywords would
  // override @boolean, @constant.builtin, and @variable.builtin.
  if (token.kind == TRUE || token.kind == FALSE || token.kind == NULLPTR ||
      token.kind == THIS) {
    return std::nullopt;
  }
  if (lang::isDirectiveToken(token.kind)) {
    return SemanticClassification{Macro, 0};
  }
  if (lang::isKeywordToken(token.kind)) {
    return SemanticClassification{Keyword, 0};
  }
  if (lang::isTypeKeywordToken(token.kind)) {
    return SemanticClassification{Type, 0};
  }
  if (token.kind == STRING_LITERAL || token.kind == CHARACTER_LITERAL) {
    return SemanticClassification{String, 0};
  }
  if (token.kind == INT_LITERAL || token.kind == FLOAT_LITERAL) {
    return SemanticClassification{Number, 0};
  }
  if (lang::isOperatorToken(token.kind)) {
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

  if (token.lexeme == "target") {
    return SemanticClassification{Variable, Readonly};
  }
  return std::nullopt;
}

void classifyStandardLibraryIncludes(
    const std::vector<lang::Token> &tokens,
    std::vector<std::optional<SemanticClassification>> &types) {
  using enum lang::TokenKind;
  for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
    if (tokens[index].kind != HASH_INCLUDE || tokens[index + 1].kind != LESS) {
      continue;
    }
    const int line = tokens[index].line;
    for (std::size_t path = index + 1;
         path < tokens.size() && tokens[path].line == line; ++path) {
      types[path] = SemanticClassification{String, DefaultLibrary};
      if (tokens[path].kind == GREATER) {
        break;
      }
    }
  }
}

void collectCommentTokens(std::string_view source,
                          const SourcePositionIndex &positions,
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
    result.push_back({positions.at(start),
                      utf16Length(source.substr(start, index - start)), Comment,
                      0});
  }
}

SemanticClassification classificationForSymbol(const lang::SymbolRecord &symbol,
                                               lang::OccurrenceRole roles) {
  std::uint32_t type = Variable;
  switch (symbol.kind) {
  case lang::SymbolKind::Namespace:
  case lang::SymbolKind::NamespaceAlias:
    type = Namespace;
    break;
  case lang::SymbolKind::TypeAlias:
  case lang::SymbolKind::Concept:
    type = Type;
    break;
  case lang::SymbolKind::Class:
    type = Class;
    break;
  case lang::SymbolKind::Struct:
    type = Struct;
    break;
  case lang::SymbolKind::Enum:
    type = Enum;
    break;
  case lang::SymbolKind::Enumerator:
    type = EnumMember;
    break;
  case lang::SymbolKind::Constructor:
  case lang::SymbolKind::Destructor:
  case lang::SymbolKind::Method:
  case lang::SymbolKind::Operator:
    type = Method;
    break;
  case lang::SymbolKind::Function:
    type = Function;
    break;
  case lang::SymbolKind::Field:
    type = Property;
    break;
  case lang::SymbolKind::GlobalVariable:
  case lang::SymbolKind::LocalVariable:
  case lang::SymbolKind::LambdaCapture:
    type = Variable;
    break;
  case lang::SymbolKind::Parameter:
  case lang::SymbolKind::ValueParameter:
    type = Parameter;
    break;
  case lang::SymbolKind::TypeParameter:
    type = TypeParameter;
    break;
  }

  std::uint32_t modifiers = 0;
  if (lang::hasRole(roles, lang::OccurrenceRole::Declaration)) {
    modifiers |= Declaration;
  }
  if (lang::hasRole(roles, lang::OccurrenceRole::Definition)) {
    modifiers |= Definition;
  }
  if (symbol.defaultLibrary) {
    modifiers |= DefaultLibrary;
  }
  if (symbol.staticMember || symbol.internalLinkage) {
    modifiers |= Static;
  }
  switch (symbol.kind) {
  case lang::SymbolKind::Enumerator:
  case lang::SymbolKind::ValueParameter:
    modifiers |= Readonly;
    break;
  case lang::SymbolKind::Field:
  case lang::SymbolKind::GlobalVariable:
  case lang::SymbolKind::LocalVariable:
  case lang::SymbolKind::Parameter:
  case lang::SymbolKind::LambdaCapture:
    if (!symbol.mutableBinding) {
      modifiers |= Readonly;
    }
    break;
  default:
    break;
  }
  if (symbol.kind == lang::SymbolKind::LocalVariable ||
      symbol.kind == lang::SymbolKind::Parameter ||
      symbol.kind == lang::SymbolKind::LambdaCapture) {
    modifiers |= FunctionScope;
  }
  return SemanticClassification{type, modifiers};
}

void applyResolvedSymbolClassifications(
    const std::vector<lang::Token> &tokens,
    std::vector<std::optional<SemanticClassification>> &classifications,
    const lang::SemanticDatabase &database, lang::SourceUnitId sourceUnit) {
  std::unordered_map<std::size_t, std::size_t> tokenAt;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    if (tokens[index].kind == lang::TokenKind::IDENTIFIER) {
      tokenAt.emplace(tokens[index].position, index);
    }
  }
  for (const lang::SemanticOccurrence &occurrence :
       database.occurrences(sourceUnit)) {
    if (occurrence.symbol == 0) {
      continue;
    }
    const auto token = tokenAt.find(occurrence.span.start);
    if (token == tokenAt.end() ||
        tokens[token->second].position + tokens[token->second].lexeme.size() !=
            occurrence.span.end) {
      continue;
    }
    const lang::SymbolRecord *symbol = database.findSymbol(occurrence.symbol);
    if (symbol == nullptr) {
      continue;
    }
    classifications[token->second] =
        classificationForSymbol(*symbol, occurrence.roles);
  }
}

std::vector<SemanticToken>
collectSemanticTokens(std::string_view source,
                      const lang::SemanticDatabase *database = nullptr,
                      lang::SourceUnitId sourceUnit = 0) {
  const SourcePositionIndex positions(source);
  lang::Lexer lexer;
  const std::vector<lang::Token> tokens = lexer.scan(std::string(source));
  std::vector<std::optional<SemanticClassification>> classifications;
  classifications.reserve(tokens.size());
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    classifications.push_back(basicSemanticType(tokens, index));
  }
  classifyStandardLibraryIncludes(tokens, classifications);
  if (database != nullptr && sourceUnit != 0) {
    for (std::size_t index = 0; index < tokens.size(); ++index) {
      if (tokens[index].kind == lang::TokenKind::IDENTIFIER &&
          tokens[index].lexeme != "discard" &&
          tokens[index].lexeme != "target" &&
          (!classifications[index] || classifications[index]->type != String)) {
        classifications[index].reset();
      }
    }
    applyResolvedSymbolClassifications(tokens, classifications, *database,
                                       sourceUnit);
  }

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
        result.push_back({positions.at(segmentStart),
                          utf16Length(source.substr(segmentStart,
                                                    segmentEnd - segmentStart)),
                          classifications[index]->type,
                          classifications[index]->modifiers});
      }
      segmentStart = segmentEnd < tokenEnd ? segmentEnd + 1 : tokenEnd;
    }
  }

  collectCommentTokens(source, positions, result);
  std::sort(result.begin(), result.end(),
            [](const SemanticToken &left, const SemanticToken &right) {
              return left.position.line < right.position.line ||
                     (left.position.line == right.position.line &&
                      left.position.character < right.position.character);
            });
  return result;
}

json_object *semanticTokensJson(const std::vector<SemanticToken> &tokens) {
  json_object *data = json_object_new_array();
  Position previous;

  for (const SemanticToken &token : tokens) {
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

int completionItemKind(lang::CompletionCandidateKind kind) {
  switch (kind) {
  case lang::CompletionCandidateKind::Method:
    return 2;
  case lang::CompletionCandidateKind::Function:
    return 3;
  case lang::CompletionCandidateKind::Field:
    return 5;
  case lang::CompletionCandidateKind::Variable:
  case lang::CompletionCandidateKind::Parameter:
    return 6;
  case lang::CompletionCandidateKind::Class:
    return 7;
  case lang::CompletionCandidateKind::Namespace:
    return 9;
  case lang::CompletionCandidateKind::Enum:
    return 13;
  case lang::CompletionCandidateKind::TypeAlias:
    return 18;
  case lang::CompletionCandidateKind::Enumerator:
    return 20;
  case lang::CompletionCandidateKind::Struct:
    return 22;
  case lang::CompletionCandidateKind::TypeParameter:
    return 25;
  }
  return 6;
}

json_object *completionListJson(const lang::CompletionResult &completion,
                                std::string_view source, bool snippetSupport) {
  json_object *items = json_object_new_array();
  for (const lang::CompletionCandidate &candidate : completion.candidates) {
    json_object *item = json_object_new_object();
    json_object_object_add(item, "label",
                           json_object_new_string(candidate.label.c_str()));
    json_object_object_add(
        item, "kind", json_object_new_int(completionItemKind(candidate.kind)));
    if (!candidate.detail.empty()) {
      json_object_object_add(item, "detail",
                             json_object_new_string(candidate.detail.c_str()));
    }
    json_object_object_add(item, "filterText",
                           json_object_new_string(candidate.label.c_str()));
    json_object_object_add(item, "sortText",
                           json_object_new_string(candidate.sortText.c_str()));

    const bool useSnippet = snippetSupport && candidate.snippet.has_value();
    const std::string &insertion =
        useSnippet ? *candidate.snippet : candidate.insertion;
    json_object *textEdit = json_object_new_object();
    json_object_object_add(textEdit, "range",
                           rangeJson(source, candidate.replacementRange.start,
                                     candidate.replacementRange.end -
                                         candidate.replacementRange.start));
    json_object_object_add(textEdit, "newText",
                           json_object_new_string(insertion.c_str()));
    json_object_object_add(item, "textEdit", textEdit);
    if (useSnippet) {
      json_object_object_add(item, "insertTextFormat", json_object_new_int(2));
    }
    json_object_array_add(items, item);
  }

  json_object *result = json_object_new_object();
  json_object_object_add(result, "isIncomplete",
                         json_object_new_boolean(completion.isIncomplete));
  json_object_object_add(result, "items", items);
  return result;
}

struct AnalysisRequest {
  std::string uri;
  std::string source;
  std::optional<std::int64_t> version;
  std::uint64_t generation = 0;
  std::unordered_map<std::string, std::string> sourceOverrides;
  std::unordered_map<std::string, std::uint64_t> documentGenerations;
};

struct CompletionRequest {
  json_object *id = nullptr;
  std::string idKey;
  std::string uri;
  std::filesystem::path entryPath;
  std::string source;
  std::uint64_t generation = 0;
  std::size_t byteOffset = 0;
  std::unordered_map<std::string, std::string> sourceOverrides;
  bool snippetSupport = false;
};

enum class SemanticRequestKind { Tokens, Hover, Definition };

struct PendingSemanticRequest {
  json_object *id = nullptr;
  json_object *params = nullptr;
  std::string idKey;
  std::string uri;
  std::uint64_t generation = 0;
  SemanticRequestKind kind = SemanticRequestKind::Hover;
};

struct DocumentAnalysis {
  std::vector<LspDiagnostic> diagnostics;
  std::unordered_set<std::string> dependencies;
  std::string rootPath;
  std::shared_ptr<const lang::FrontendResult> frontend;
};

struct AnalysisSnapshot {
  std::uint64_t generation = 0;
  std::string rootPath;
  std::shared_ptr<const lang::FrontendResult> frontend;
};

struct CachedSemanticTokens {
  std::uint64_t generation = 0;
  std::vector<SemanticToken> tokens;
};

struct DiagnosticPublication {
  std::string uri;
  std::optional<std::int64_t> version;
  std::vector<LspDiagnostic> diagnostics;
};

class LanguageServer {
public:
  explicit LanguageServer(lang::StandardLibraryLayout standardLibrary)
      : standardLibrary(std::move(standardLibrary)),
        analysisWorker(&LanguageServer::runAnalysisWorker, this),
        completionWorker(&LanguageServer::runCompletionWorker, this) {}

  ~LanguageServer() {
    stopCompletionWorker();
    stopAnalysisWorker();
  }

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
        sendJson(errorResponse(nullptr, -32700, "Parse error"));
        continue;
      }

      if (!json_object_is_type(message, json_type_object)) {
        sendJson(errorResponse(nullptr, -32600, "Invalid Request"));
        json_object_put(message);
        continue;
      }

      try {
        if (handle(message)) {
          json_object_put(message);
          return shutdownRequested ? 0 : 1;
        }
      } catch (const std::exception &error) {
        std::cerr << "LSP request failed: " << error.what() << '\n';
        sendJson(
            errorResponse(member(message, "id"), -32603, "Internal error"));
      } catch (...) {
        std::cerr << "LSP request failed with an unknown exception\n";
        sendJson(
            errorResponse(member(message, "id"), -32603, "Internal error"));
      }
      json_object_put(message);
    }
    stopCompletionWorker();
    stopAnalysisWorker();
    return 0;
  }

private:
  bool handle(json_object *message) {
    const std::string method = stringMember(message, "method");
    json_object *id = member(message, "id");
    json_object *params = member(message, "params");

    if (method == "initialize") {
      sendJson(response(id, initializeResult(params)));
    } else if (method == "initialized") {
      registerWatchedFiles();
    } else if (method == "shutdown") {
      flushAnalyses();
      flushCompletions();
      shutdownRequested = true;
      sendJson(response(id, nullptr));
    } else if (method == "exit") {
      return true;
    } else if (method == "textDocument/didOpen") {
      didOpen(params);
    } else if (method == "textDocument/didChange") {
      didChange(params);
    } else if (method == "textDocument/didSave") {
      didSave(params);
    } else if (method == "textDocument/didClose") {
      didClose(params);
    } else if (method == "workspace/didChangeWatchedFiles") {
      didChangeWatchedFiles(params);
    } else if (method == "$/cancelRequest") {
      cancelRequest(params);
    } else if (method == "textDocument/semanticTokens/full") {
      semanticTokens(id, params);
    } else if (method == "textDocument/hover") {
      hover(id, params);
    } else if (method == "textDocument/definition") {
      definition(id, params);
    } else if (method == "textDocument/completion") {
      completion(id, params);
    } else if (method == "textDocument/codeAction") {
      codeActions(id, params);
    } else if (method == "textDocument/formatting") {
      documentFormatting(id, params);
    } else if (id != nullptr && !method.empty()) {
      sendJson(errorResponse(id, -32601, "Method not found"));
    }
    return false;
  }

  json_object *initializeResult(json_object *params) {
    markdownHover = supportsHoverFormat(params, "markdown");
    completionSnippets = supportsCompletionSnippets(params);
    semanticTokenRefreshSupport = supportsSemanticTokenRefresh(params);
    workspaceDocumentChanges = supportsWorkspaceDocumentChanges(params);
    watchedFilesDynamicRegistration =
        supportsWatchedFilesDynamicRegistration(params);
    diagnosticRelatedInformation = supportsDiagnosticRelatedInformation(params);
    diagnosticData = supportsDiagnosticData(params);
    codeActionLiterals = supportsCodeActionLiterals(params);
    preferredCodeActions = supportsPreferredCodeActions(params);
    json_object *sync = json_object_new_object();
    json_object_object_add(sync, "openClose", json_object_new_boolean(true));
    json_object_object_add(sync, "change", json_object_new_int(1));
    json_object_object_add(sync, "save", json_object_new_boolean(true));

    json_object *tokenTypes = json_object_new_array();
    for (const char *type :
         {"keyword", "type", "typeParameter", "namespace", "class", "function",
          "method", "variable", "parameter", "property", "string", "number",
          "operator", "macro", "decorator", "comment", "enumMember", "struct",
          "enum"}) {
      json_object_array_add(tokenTypes, json_object_new_string(type));
    }
    json_object *tokenModifiers = json_object_new_array();
    for (const char *modifier : {"declaration", "definition", "readonly",
                                 "defaultLibrary", "functionScope", "static"}) {
      json_object_array_add(tokenModifiers, json_object_new_string(modifier));
    }
    json_object *legend = json_object_new_object();
    json_object_object_add(legend, "tokenTypes", tokenTypes);
    json_object_object_add(legend, "tokenModifiers", tokenModifiers);

    json_object *semanticTokens = json_object_new_object();
    json_object_object_add(semanticTokens, "legend", legend);
    json_object_object_add(semanticTokens, "full",
                           json_object_new_boolean(true));

    json_object *capabilities = json_object_new_object();
    json_object_object_add(capabilities, "positionEncoding",
                           json_object_new_string("utf-16"));
    json_object_object_add(capabilities, "textDocumentSync", sync);
    json_object_object_add(capabilities, "semanticTokensProvider",
                           semanticTokens);
    json_object_object_add(capabilities, "documentFormattingProvider",
                           json_object_new_boolean(true));
    json_object_object_add(capabilities, "hoverProvider",
                           json_object_new_boolean(true));
    json_object_object_add(capabilities, "definitionProvider",
                           json_object_new_boolean(true));
    if (codeActionLiterals) {
      json_object *codeActions = json_object_new_object();
      json_object *codeActionKinds = json_object_new_array();
      json_object_array_add(codeActionKinds,
                            json_object_new_string("quickfix"));
      json_object_object_add(codeActions, "codeActionKinds", codeActionKinds);
      json_object_object_add(codeActions, "resolveProvider",
                             json_object_new_boolean(false));
      json_object_object_add(capabilities, "codeActionProvider", codeActions);
    }
    json_object *completion = json_object_new_object();
    json_object *triggers = json_object_new_array();
    for (const char *trigger : {".", ">", ":"}) {
      json_object_array_add(triggers, json_object_new_string(trigger));
    }
    json_object_object_add(completion, "triggerCharacters", triggers);
    json_object_object_add(completion, "resolveProvider",
                           json_object_new_boolean(false));
    json_object_object_add(capabilities, "completionProvider", completion);

    json_object *serverInfo = json_object_new_object();
    json_object_object_add(serverInfo, "name",
                           json_object_new_string("gti_lsp"));
    json_object_object_add(serverInfo, "version",
                           json_object_new_string(GTI_VERSION));

    json_object *result = json_object_new_object();
    json_object_object_add(result, "capabilities", capabilities);
    json_object_object_add(result, "serverInfo", serverInfo);
    return result;
  }

  void didOpen(json_object *params) {
    json_object *document = member(params, "textDocument");
    const std::string clientUri = stringMember(document, "uri");
    if (clientUri.empty()) {
      return;
    }
    const std::string uri = documentKeyFromUri(clientUri);
    std::vector<AnalysisRequest> requests;
    std::vector<DiagnosticPublication> publications;
    {
      const std::lock_guard lock(stateMutex);
      clientUris[uri] = clientUri;
      openClientUris[uri].insert(clientUri);
      documents[uri] = stringMember(document, "text");
      const std::optional<std::int64_t> version =
          integerMember(document, "version");
      if (version) {
        documentVersions[uri] = *version;
      }
      semanticTokenCache.erase(uri);
      analysisSnapshots.erase(uri);
      requests.push_back(
          makeAnalysisRequestLocked(uri, version, ++analysisGenerations[uri]));
      std::unordered_set<std::string> affected;
      invalidateDependentsLocked(uri, affected, requests);
      publications = publicationsForLocked(affected);
    }
    rejectPendingSemanticRequests(requests, -32801, "Content modified");
    publish(std::move(publications));
    scheduleAnalyses(std::move(requests));
  }

  void didChange(json_object *params) {
    const std::string clientUri =
        stringMember(member(params, "textDocument"), "uri");
    json_object *changes = member(params, "contentChanges");
    if (clientUri.empty() || changes == nullptr ||
        !json_object_is_type(changes, json_type_array)) {
      return;
    }
    const std::string uri = documentKeyFromUri(clientUri);

    const std::size_t count = json_object_array_length(changes);
    if (count > 0) {
      const std::string source =
          stringMember(json_object_array_get_idx(changes, count - 1), "text");
      const std::optional<std::int64_t> version =
          integerMember(member(params, "textDocument"), "version");
      std::vector<AnalysisRequest> requests;
      std::vector<DiagnosticPublication> publications;
      {
        const std::lock_guard lock(stateMutex);
        clientUris[uri] = clientUri;
        openClientUris[uri].insert(clientUri);
        documents[uri] = source;
        if (version) {
          documentVersions[uri] = *version;
        }
        semanticTokenCache.erase(uri);
        analysisSnapshots.erase(uri);
        requests.push_back(makeAnalysisRequestLocked(
            uri, version, ++analysisGenerations[uri]));
        std::unordered_set<std::string> affected;
        invalidateDependentsLocked(uri, affected, requests);
        publications = publicationsForLocked(affected);
      }
      rejectPendingSemanticRequests(requests, -32801, "Content modified");
      publish(std::move(publications));
      scheduleAnalyses(std::move(requests));
    }
  }

  void didSave(json_object *) {
    // Full synchronization already schedules analysis for every changed
    // version. Repeating it here blocks later requests and republishes the
    // same diagnostics during format-on-save.
  }

  void didClose(json_object *params) {
    const std::string clientUri =
        stringMember(member(params, "textDocument"), "uri");
    if (clientUri.empty()) {
      return;
    }
    const std::string uri = documentKeyFromUri(clientUri);
    {
      const std::lock_guard lock(stateMutex);
      const auto aliases = openClientUris.find(uri);
      if (aliases != openClientUris.end()) {
        aliases->second.erase(clientUri);
        if (!aliases->second.empty()) {
          if (clientUris[uri] == clientUri) {
            clientUris[uri] = *aliases->second.begin();
          }
          return;
        }
        openClientUris.erase(aliases);
      }
    }
    std::vector<DiagnosticPublication> publications;
    std::vector<AnalysisRequest> requests;
    {
      const std::lock_guard lock(stateMutex);
      std::unordered_set<std::string> affected{uri};
      if (const auto found = diagnosticsByRoot.find(uri);
          found != diagnosticsByRoot.end()) {
        for (const LspDiagnostic &diagnostic : found->second) {
          affected.insert(diagnostic.uri);
        }
      }
      documents.erase(uri);
      documentVersions.erase(uri);
      semanticTokenCache.erase(uri);
      analysisSnapshots.erase(uri);
      diagnosticsByRoot.erase(uri);
      dependenciesByRoot.erase(uri);
      pendingAnalyses.erase(uri);
      ++analysisGenerations[uri];
      invalidateDependentsLocked(uri, affected, requests);
      publications = publicationsForLocked(affected);
      clientUris.erase(uri);
    }
    rejectPendingSemanticRequests(uri, -32801, "Content modified");
    rejectPendingSemanticRequests(requests, -32801, "Content modified");
    publish(std::move(publications));
    scheduleAnalyses(std::move(requests));
    analysisCondition.notify_all();
  }

  void didChangeWatchedFiles(json_object *params) {
    json_object *changes = member(params, "changes");
    if (changes == nullptr || !json_object_is_type(changes, json_type_array)) {
      return;
    }

    std::vector<AnalysisRequest> requests;
    std::vector<DiagnosticPublication> publications;
    {
      const std::lock_guard lock(stateMutex);
      std::unordered_set<std::string> affected;
      const std::size_t count = json_object_array_length(changes);
      for (std::size_t index = 0; index < count; ++index) {
        const std::string clientUri =
            stringMember(json_object_array_get_idx(changes, index), "uri");
        if (clientUri.empty()) {
          continue;
        }
        const std::string uri = documentKeyFromUri(clientUri);
        // Dirty open buffers remain authoritative over filesystem events.
        if (documents.contains(uri)) {
          continue;
        }
        affected.insert(uri);
        invalidateDependentsLocked(uri, affected, requests);
      }
      publications = publicationsForLocked(affected);
    }
    rejectPendingSemanticRequests(requests, -32801, "Content modified");
    publish(std::move(publications));
    scheduleAnalyses(std::move(requests));
  }

  void registerWatchedFiles() {
    if (!watchedFilesDynamicRegistration) {
      return;
    }

    json_object *watcher = json_object_new_object();
    json_object_object_add(watcher, "globPattern",
                           json_object_new_string("**/*.gti"));
    json_object *watchers = json_object_new_array();
    json_object_array_add(watchers, watcher);
    json_object *options = json_object_new_object();
    json_object_object_add(options, "watchers", watchers);

    json_object *registration = json_object_new_object();
    json_object_object_add(registration, "id",
                           json_object_new_string("gti-source-watcher"));
    json_object_object_add(
        registration, "method",
        json_object_new_string("workspace/didChangeWatchedFiles"));
    json_object_object_add(registration, "registerOptions", options);
    json_object *registrations = json_object_new_array();
    json_object_array_add(registrations, registration);
    json_object *params = json_object_new_object();
    json_object_object_add(params, "registrations", registrations);

    json_object *request = json_object_new_object();
    json_object_object_add(request, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(
        request, "id", json_object_new_int64(nextServerRequestId.fetch_add(1)));
    json_object_object_add(request, "method",
                           json_object_new_string("client/registerCapability"));
    json_object_object_add(request, "params", params);
    sendJson(request);
  }

  void semanticTokens(json_object *id, json_object *params) {
    const std::string uri =
        documentKeyFromUri(stringMember(member(params, "textDocument"), "uri"));
    std::string source;
    std::uint64_t generation = 0;
    std::optional<std::vector<SemanticToken>> cached;
    AnalysisSnapshot snapshot;
    bool hasCurrentSnapshot = false;
    bool queued = false;
    {
      const std::lock_guard lock(stateMutex);
      if (const auto document = documents.find(uri);
          document != documents.end()) {
        source = document->second;
      }
      if (const auto current = analysisGenerations.find(uri);
          current != analysisGenerations.end()) {
        generation = current->second;
      }
      if (const auto found = semanticTokenCache.find(uri);
          found != semanticTokenCache.end() &&
          found->second.generation == generation) {
        cached = found->second.tokens;
      }
      const auto currentSnapshot = analysisSnapshots.find(uri);
      if (currentSnapshot != analysisSnapshots.end() &&
          currentSnapshot->second.generation == generation &&
          currentSnapshot->second.frontend != nullptr) {
        snapshot = currentSnapshot->second;
        hasCurrentSnapshot = true;
      } else if (id != nullptr && documents.contains(uri)) {
        pendingSemanticRequests.push_back(
            {.id = json_object_get(id),
             .params = json_object_get(params),
             .idKey = requestIdKey(id),
             .uri = uri,
             .generation = generation,
             .kind = SemanticRequestKind::Tokens});
        queued = true;
      }
    }
    if (queued) {
      return;
    }

    lang::SourceUnitId sourceUnit = 0;
    const lang::SemanticDatabase *database = nullptr;
    if (hasCurrentSnapshot) {
      sourceUnit =
          snapshot.frontend->sourceGraph.sourceUnitForPath(snapshot.rootPath);
      database = &snapshot.frontend->semantics.database();
    }
    std::vector<SemanticToken> tokens =
        cached ? std::move(*cached)
               : collectSemanticTokens(source, database, sourceUnit);
    if (!cached && !uri.empty()) {
      const std::lock_guard lock(stateMutex);
      const auto current = analysisGenerations.find(uri);
      if (current != analysisGenerations.end() &&
          current->second == generation && documents.contains(uri)) {
        semanticTokenCache[uri] = {
            .generation = generation,
            .tokens = tokens,
        };
      }
    }
    sendJson(response(id, semanticTokensJson(tokens)));
  }

  void respondHover(json_object *id, Position position, std::string source,
                    const AnalysisSnapshot &snapshot) {
    const std::optional<std::size_t> byteOffset =
        SourcePositionIndex(source).byteOffset(position);
    if (!byteOffset) {
      sendJson(response(id, nullptr));
      return;
    }
    const lang::SourceUnitId sourceUnit =
        snapshot.frontend->sourceGraph.sourceUnitForPath(snapshot.rootPath);
    const std::optional<lang::HoverInfo> info = lang::LanguageQueries().hover(
        *snapshot.frontend, sourceUnit, *byteOffset);
    if (!info) {
      sendJson(response(id, nullptr));
      return;
    }

    std::string value = markdownHover ? "```gti\n" + info->signature + "\n```"
                                      : info->signature;
    if (info->documentationMarkdown) {
      value += "\n\n" + *info->documentationMarkdown;
    }
    for (const std::string &note : info->notes) {
      value += markdownHover ? "\n\n*" + note + "*" : "\n\n" + note;
    }

    json_object *contents = json_object_new_object();
    json_object_object_add(
        contents, "kind",
        json_object_new_string(markdownHover ? "markdown" : "plaintext"));
    json_object_object_add(contents, "value",
                           json_object_new_string_len(
                               value.data(), static_cast<int>(value.size())));
    json_object *result = json_object_new_object();
    json_object_object_add(result, "contents", contents);
    json_object_object_add(result, "range",
                           rangeJson(source, info->range.start,
                                     info->range.end - info->range.start));
    sendJson(response(id, result));
  }

  void hover(json_object *id, json_object *params) {
    const std::string uri =
        documentKeyFromUri(stringMember(member(params, "textDocument"), "uri"));
    const std::optional<Position> position =
        positionMember(member(params, "position"));
    if (!position) {
      sendJson(response(id, nullptr));
      return;
    }
    std::string source;
    AnalysisSnapshot snapshot;
    bool hasCurrentSnapshot = false;
    bool queued = false;
    {
      const std::lock_guard lock(stateMutex);
      const auto document = documents.find(uri);
      const auto currentGeneration = analysisGenerations.find(uri);
      const auto currentSnapshot = analysisSnapshots.find(uri);
      if (document != documents.end() &&
          currentGeneration != analysisGenerations.end() &&
          currentSnapshot != analysisSnapshots.end() &&
          currentSnapshot->second.generation == currentGeneration->second &&
          currentSnapshot->second.frontend != nullptr) {
        source = document->second;
        snapshot = currentSnapshot->second;
        hasCurrentSnapshot = true;
      } else if (id != nullptr && document != documents.end() &&
                 currentGeneration != analysisGenerations.end()) {
        pendingSemanticRequests.push_back(
            {.id = json_object_get(id),
             .params = json_object_get(params),
             .idKey = requestIdKey(id),
             .uri = uri,
             .generation = currentGeneration->second,
             .kind = SemanticRequestKind::Hover});
        queued = true;
      }
    }
    if (!hasCurrentSnapshot) {
      if (!queued) {
        sendJson(response(id, nullptr));
      }
      return;
    }

    respondHover(id, *position, std::move(source), snapshot);
  }

  void respondDefinition(json_object *id, std::string_view uri,
                         Position position, std::string source,
                         const AnalysisSnapshot &snapshot) {
    const std::optional<std::size_t> byteOffset =
        SourcePositionIndex(source).byteOffset(position);
    if (!byteOffset) {
      sendJson(response(id, nullptr));
      return;
    }
    const lang::SourceUnitId sourceUnit =
        snapshot.frontend->sourceGraph.sourceUnitForPath(snapshot.rootPath);
    const std::optional<lang::DefinitionInfo> info =
        lang::LanguageQueries().definition(*snapshot.frontend, sourceUnit,
                                           *byteOffset);
    if (!info) {
      sendJson(response(id, nullptr));
      return;
    }

    const std::string targetSource = sourceForSpan(
        info->target, snapshot.frontend->sources, snapshot.rootPath, source);
    if (targetSource.empty()) {
      sendJson(response(id, nullptr));
      return;
    }
    json_object *location = json_object_new_object();
    std::string targetUri =
        uriForSource(info->target.source, snapshot.rootPath, uri);
    {
      const std::lock_guard lock(stateMutex);
      if (const auto preferred = clientUris.find(targetUri);
          preferred != clientUris.end()) {
        targetUri = preferred->second;
      }
    }
    json_object_object_add(location, "uri",
                           json_object_new_string(targetUri.c_str()));
    json_object_object_add(location, "range",
                           rangeJson(targetSource, info->target.start,
                                     info->target.end - info->target.start));
    sendJson(response(id, location));
  }

  void definition(json_object *id, json_object *params) {
    const std::string uri =
        documentKeyFromUri(stringMember(member(params, "textDocument"), "uri"));
    const std::optional<Position> position =
        positionMember(member(params, "position"));
    if (!position) {
      sendJson(response(id, nullptr));
      return;
    }

    std::string source;
    AnalysisSnapshot snapshot;
    bool hasCurrentSnapshot = false;
    bool queued = false;
    {
      const std::lock_guard lock(stateMutex);
      const auto document = documents.find(uri);
      const auto generation = analysisGenerations.find(uri);
      const auto current = analysisSnapshots.find(uri);
      if (document != documents.end() &&
          generation != analysisGenerations.end() &&
          current != analysisSnapshots.end() &&
          current->second.generation == generation->second &&
          current->second.frontend != nullptr) {
        source = document->second;
        snapshot = current->second;
        hasCurrentSnapshot = true;
      } else if (id != nullptr && document != documents.end() &&
                 generation != analysisGenerations.end()) {
        pendingSemanticRequests.push_back(
            {.id = json_object_get(id),
             .params = json_object_get(params),
             .idKey = requestIdKey(id),
             .uri = uri,
             .generation = generation->second,
             .kind = SemanticRequestKind::Definition});
        queued = true;
      }
    }
    if (!hasCurrentSnapshot) {
      if (!queued) {
        sendJson(response(id, nullptr));
      }
      return;
    }

    respondDefinition(id, uri, *position, std::move(source), snapshot);
  }

  void completion(json_object *id, json_object *params) {
    if (id == nullptr) {
      return;
    }
    const std::string uri =
        documentKeyFromUri(stringMember(member(params, "textDocument"), "uri"));
    const std::optional<Position> position =
        positionMember(member(params, "position"));
    std::string source;
    CompletionRequest request;
    bool validRequest = false;
    {
      const std::lock_guard lock(stateMutex);
      const auto document = documents.find(uri);
      const auto generation = analysisGenerations.find(uri);
      if (position && document != documents.end() &&
          generation != analysisGenerations.end()) {
        source = document->second;
        request.uri = uri;
        request.source = source;
        request.generation = generation->second;
        request.snippetSupport = completionSnippets;
        for (const auto &[documentUri, documentSource] : documents) {
          const std::optional<std::filesystem::path> path =
              filePathFromUri(documentUri);
          if (path) {
            request.sourceOverrides[canonicalPath(*path).string()] =
                documentSource;
          }
        }
        validRequest = true;
      }
    }
    if (!validRequest) {
      sendJson(
          response(id, completionListJson({}, source, completionSnippets)));
      return;
    }

    const std::optional<std::filesystem::path> filePath = filePathFromUri(uri);
    const std::optional<std::size_t> byteOffset =
        SourcePositionIndex(source).byteOffset(*position);
    if (!filePath || !byteOffset) {
      sendJson(
          response(id, completionListJson({}, source, completionSnippets)));
      return;
    }
    request.id = json_object_get(id);
    request.idKey = requestIdKey(id);
    request.entryPath = *filePath;
    request.byteOffset = *byteOffset;
    scheduleCompletion(std::move(request));
  }

  void codeActions(json_object *id, json_object *params) {
    if (id == nullptr) {
      return;
    }
    json_object *actions = json_object_new_array();
    const std::string clientUri =
        stringMember(member(params, "textDocument"), "uri");
    const std::string uri = documentKeyFromUri(clientUri);
    const std::optional<LspRange> requestedRange =
        rangeMember(member(params, "range"));
    json_object *context = member(params, "context");
    json_object *requestedDiagnostics = member(context, "diagnostics");
    if (uri.empty() || !codeActionLiterals || !requestedRange ||
        !validRange(*requestedRange) || !contextAllowsQuickFix(context)) {
      sendJson(response(id, actions));
      return;
    }

    std::vector<CodeActionCandidate> candidates;
    {
      const std::lock_guard lock(stateMutex);
      const auto document = documents.find(uri);
      const auto generation = analysisGenerations.find(uri);
      const auto snapshot = analysisSnapshots.find(uri);
      const bool current = document != documents.end() &&
                           generation != analysisGenerations.end() &&
                           snapshot != analysisSnapshots.end() &&
                           snapshot->second.generation == generation->second &&
                           snapshot->second.frontend != nullptr;
      if (current) {
        std::unordered_set<std::string> seen;
        for (const auto &[_, diagnostics] : diagnosticsByRoot) {
          for (const LspDiagnostic &diagnostic : diagnostics) {
            if (diagnostic.uri != uri ||
                diagnostic.source != document->second ||
                !diagnosticIntersects(diagnostic, *requestedRange) ||
                !diagnosticRequested(diagnostic, requestedDiagnostics)) {
              continue;
            }
            for (std::size_t index = 0; index < diagnostic.fixes.size();
                 ++index) {
              const LspFixIt &fix = diagnostic.fixes[index];
              const auto target = documents.find(fix.uri);
              if (target == documents.end() || target->second != fix.source) {
                continue;
              }
              const std::string key =
                  diagnostic.diagnostic.code + '\n' +
                  diagnostic.diagnostic.message + '\n' + fix.uri + '\n' +
                  std::to_string(fix.fix.span.start) + ':' +
                  std::to_string(fix.fix.span.end) + '\n' + fix.fix.replacement;
              if (!seen.insert(key).second) {
                continue;
              }
              const auto targetVersion = documentVersions.find(fix.uri);
              LspDiagnostic publishedDiagnostic = diagnostic;
              useClientUrisLocked(publishedDiagnostic);
              LspFixIt publishedFix = fix;
              publishedFix.uri = clientUriForKeyLocked(fix.uri);
              candidates.push_back(
                  {.diagnostic = std::move(publishedDiagnostic),
                   .fix = std::move(publishedFix),
                   .version =
                       targetVersion == documentVersions.end()
                           ? std::nullopt
                           : std::optional<std::int64_t>(targetVersion->second),
                   .preferred = index == 0});
            }
          }
        }
      }
    }

    for (const CodeActionCandidate &candidate : candidates) {
      json_object_array_add(
          actions, codeActionJson(candidate, workspaceDocumentChanges,
                                  diagnosticRelatedInformation, diagnosticData,
                                  preferredCodeActions));
    }
    sendJson(response(id, actions));
  }

  void documentFormatting(json_object *id, json_object *params) {
    json_object *edits = json_object_new_array();
    const std::string clientUri =
        stringMember(member(params, "textDocument"), "uri");
    const std::string uri = documentKeyFromUri(clientUri);
    std::string source;
    {
      const std::lock_guard lock(stateMutex);
      if (const auto document = documents.find(uri);
          document != documents.end()) {
        source = document->second;
      }
    }
    if (source.empty()) {
      sendJson(response(id, edits));
      return;
    }

    json_object *formatOptions = member(params, "options");
    lang::FormatOptions options{
        .indentWidth =
            std::min<std::size_t>(sizeMember(formatOptions, "tabSize", 2), 16),
        .insertSpaces = boolMember(formatOptions, "insertSpaces", true),
    };
    if (const std::optional<std::filesystem::path> filePath =
            filePathFromUri(clientUri)) {
      lang::FormatConfigResult config =
          lang::loadFormatConfig(*filePath, std::move(options));
      options = std::move(config.options);
      for (const lang::FormatConfigIssue &issue : config.issues) {
        std::cerr << (config.configPath ? config.configPath->string()
                                        : std::string(".gti-format"))
                  << ':' << issue.line << ": " << issue.message << '\n';
      }
    }
    const std::string formatted = lang::Formatter(options).format(source);
    if (formatted == source) {
      sendJson(response(id, edits));
      return;
    }

    json_object *edit = json_object_new_object();
    json_object_object_add(edit, "range", rangeJson(source, 0, source.size()));
    json_object_object_add(
        edit, "newText",
        json_object_new_string_len(formatted.data(),
                                   static_cast<int>(formatted.size())));
    json_object_array_add(edits, edit);
    sendJson(response(id, edits));
  }

  DocumentAnalysis analyzeDocument(const AnalysisRequest &request) const {
    const std::string &uri = request.uri;
    const std::string &source = request.source;
    DocumentAnalysis result;
    const std::optional<std::filesystem::path> filePath = filePathFromUri(uri);
    if (!filePath) {
      lang::SourceManager sources;
      sources.set("", source);
      const lang::Diagnostic diagnostic = lang::makeDiagnostic(
          "GTI-D0001", lang::DiagnosticPhase::Driver,
          lang::SourceSpan{"", 0, std::min<std::size_t>(source.size(), 1), 1},
          "GTI source dependencies require a file URI.");
      result.diagnostics.push_back(
          convertDiagnostic(diagnostic, sources, "", uri, source));
      return result;
    }

    const std::string rootPath = canonicalPath(*filePath).string();
    result.rootPath = rootPath;
    lang::FrontendOptions frontendOptions;
    frontendOptions.analyzeRecoveredProgram = true;
    result.frontend = std::make_shared<const lang::FrontendResult>(
        lang::Frontend(frontendOptions)
            .analyze(*filePath, source, {standardLibrary.prelude},
                     request.sourceOverrides, {standardLibrary.root}));
    const lang::FrontendResult &analysis = *result.frontend;
    for (const lang::SourceUnit &unit : analysis.sourceGraph.sourceUnits()) {
      const std::string loadedSource = unit.path.string();
      if (unit.id != analysis.sourceGraph.entryUnit() &&
          loadedSource != rootPath) {
        result.dependencies.insert(
            fileUriFromPath(std::filesystem::path(loadedSource)));
      }
    }
    for (const lang::Diagnostic &diagnostic : analysis.diagnostics) {
      result.diagnostics.push_back(convertDiagnostic(
          diagnostic, analysis.sources, rootPath, uri, source));
    }
    return result;
  }

  AnalysisRequest makeAnalysisRequestLocked(const std::string &uri,
                                            std::optional<std::int64_t> version,
                                            std::uint64_t generation) const {
    AnalysisRequest request{.uri = uri,
                            .source = documents.at(uri),
                            .version = version,
                            .generation = generation};
    for (const auto &[documentUri, documentSource] : documents) {
      const std::optional<std::filesystem::path> path =
          filePathFromUri(documentUri);
      if (!path) {
        continue;
      }
      request.sourceOverrides[canonicalPath(*path).string()] = documentSource;
      if (const auto current = analysisGenerations.find(documentUri);
          current != analysisGenerations.end()) {
        request.documentGenerations[documentUri] = current->second;
      }
    }
    return request;
  }

  void invalidateDependentsLocked(const std::string &changedUri,
                                  std::unordered_set<std::string> &affected,
                                  std::vector<AnalysisRequest> &requests) {
    for (const auto &[rootUri, dependencies] : dependenciesByRoot) {
      if (rootUri == changedUri || !dependencies.contains(changedUri)) {
        continue;
      }
      if (const auto previous = diagnosticsByRoot.find(rootUri);
          previous != diagnosticsByRoot.end()) {
        for (const LspDiagnostic &diagnostic : previous->second) {
          affected.insert(diagnostic.uri);
        }
        diagnosticsByRoot.erase(previous);
      }
      pendingAnalyses.erase(rootUri);
      analysisSnapshots.erase(rootUri);
      const std::uint64_t generation = ++analysisGenerations[rootUri];
      if (documents.contains(rootUri)) {
        const auto version = documentVersions.find(rootUri);
        requests.push_back(makeAnalysisRequestLocked(
            rootUri,
            version == documentVersions.end()
                ? std::nullopt
                : std::optional<std::int64_t>(version->second),
            generation));
      }
    }
  }

  void scheduleAnalyses(std::vector<AnalysisRequest> requests) {
    for (AnalysisRequest &request : requests) {
      scheduleAnalysis(std::move(request));
    }
  }

  void rejectPendingSemanticRequests(const std::string &uri, int code,
                                     std::string_view message) {
    std::deque<PendingSemanticRequest> rejected;
    {
      const std::lock_guard lock(stateMutex);
      for (auto request = pendingSemanticRequests.begin();
           request != pendingSemanticRequests.end();) {
        if (request->uri != uri) {
          ++request;
          continue;
        }
        rejected.push_back(std::move(*request));
        request = pendingSemanticRequests.erase(request);
      }
    }
    for (PendingSemanticRequest &request : rejected) {
      sendJson(errorResponse(request.id, code, message));
      json_object_put(request.params);
      json_object_put(request.id);
    }
  }

  void rejectPendingSemanticGeneration(const std::string &uri,
                                       std::uint64_t generation, int code,
                                       std::string_view message) {
    std::deque<PendingSemanticRequest> rejected;
    {
      const std::lock_guard lock(stateMutex);
      for (auto request = pendingSemanticRequests.begin();
           request != pendingSemanticRequests.end();) {
        if (request->uri != uri || request->generation != generation) {
          ++request;
          continue;
        }
        rejected.push_back(std::move(*request));
        request = pendingSemanticRequests.erase(request);
      }
    }
    for (PendingSemanticRequest &request : rejected) {
      sendJson(errorResponse(request.id, code, message));
      json_object_put(request.params);
      json_object_put(request.id);
    }
  }

  void
  rejectPendingSemanticRequests(const std::vector<AnalysisRequest> &requests,
                                int code, std::string_view message) {
    std::unordered_set<std::string> rejected;
    for (const AnalysisRequest &request : requests) {
      if (rejected.insert(request.uri).second) {
        rejectPendingSemanticRequests(request.uri, code, message);
      }
    }
  }

  void dispatchSemanticRequests(std::deque<PendingSemanticRequest> requests) {
    for (PendingSemanticRequest &request : requests) {
      std::string source;
      AnalysisSnapshot snapshot;
      bool current = false;
      {
        const std::lock_guard lock(stateMutex);
        const auto document = documents.find(request.uri);
        const auto generation = analysisGenerations.find(request.uri);
        const auto found = analysisSnapshots.find(request.uri);
        if (document != documents.end() &&
            generation != analysisGenerations.end() &&
            generation->second == request.generation &&
            found != analysisSnapshots.end() &&
            found->second.generation == request.generation &&
            found->second.frontend != nullptr) {
          source = document->second;
          snapshot = found->second;
          current = true;
        }
      }
      const std::optional<Position> position =
          positionMember(member(request.params, "position"));
      if (!current) {
        sendJson(errorResponse(request.id, -32801, "Content modified"));
      } else if (request.kind == SemanticRequestKind::Tokens) {
        semanticTokens(request.id, request.params);
      } else if (!position) {
        sendJson(response(request.id, nullptr));
      } else if (request.kind == SemanticRequestKind::Hover) {
        respondHover(request.id, *position, std::move(source), snapshot);
      } else {
        respondDefinition(request.id, request.uri, *position, std::move(source),
                          snapshot);
      }
      json_object_put(request.params);
      json_object_put(request.id);
    }
  }

  void cancelRequest(json_object *params) {
    const std::string idKey = requestIdKey(member(params, "id"));
    if (idKey.empty()) {
      return;
    }

    std::deque<CompletionRequest> canceled;
    std::deque<PendingSemanticRequest> canceledSemantic;
    {
      const std::lock_guard lock(stateMutex);
      for (auto request = completionRequests.begin();
           request != completionRequests.end();) {
        if (request->idKey != idKey) {
          ++request;
          continue;
        }
        canceled.push_back(std::move(*request));
        request = completionRequests.erase(request);
      }
      if (activeCompletionRequests.contains(idKey)) {
        canceledRequests.insert(idKey);
      }
      for (auto request = pendingSemanticRequests.begin();
           request != pendingSemanticRequests.end();) {
        if (request->idKey != idKey) {
          ++request;
          continue;
        }
        canceledSemantic.push_back(std::move(*request));
        request = pendingSemanticRequests.erase(request);
      }
    }
    for (CompletionRequest &request : canceled) {
      sendJson(errorResponse(request.id, -32800, "Request cancelled"));
      json_object_put(request.id);
    }
    for (PendingSemanticRequest &request : canceledSemantic) {
      sendJson(errorResponse(request.id, -32800, "Request cancelled"));
      json_object_put(request.params);
      json_object_put(request.id);
    }
    completionCondition.notify_all();
  }

  void scheduleCompletion(CompletionRequest request) {
    std::deque<CompletionRequest> dropped;
    {
      const std::lock_guard lock(stateMutex);
      if (stoppingCompletion) {
        json_object_put(request.id);
        return;
      }
      for (auto queued = completionRequests.begin();
           queued != completionRequests.end();) {
        if (queued->uri != request.uri) {
          ++queued;
          continue;
        }
        dropped.push_back(std::move(*queued));
        queued = completionRequests.erase(queued);
      }
      constexpr std::size_t queueLimit = 32;
      if (completionRequests.size() >= queueLimit) {
        dropped.push_back(std::move(completionRequests.front()));
        completionRequests.pop_front();
      }
      completionRequests.push_back(std::move(request));
    }
    for (CompletionRequest &obsolete : dropped) {
      sendJson(errorResponse(obsolete.id, -32802,
                             "Superseded by a newer completion request"));
      json_object_put(obsolete.id);
    }
    completionCondition.notify_one();
  }

  void runCompletionWorker() {
    while (true) {
      CompletionRequest request;
      {
        std::unique_lock lock(stateMutex);
        completionCondition.wait(lock, [this] {
          return stoppingCompletion || !completionRequests.empty();
        });
        if (stoppingCompletion && completionRequests.empty()) {
          return;
        }
        request = std::move(completionRequests.front());
        completionRequests.pop_front();
        ++activeCompletions;
        activeCompletionRequests.insert(request.idKey);
      }

      lang::CompletionResult completion;
      bool current = false;
      bool canceled = false;
      {
        const std::lock_guard lock(stateMutex);
        const auto generation = analysisGenerations.find(request.uri);
        current = documents.contains(request.uri) &&
                  generation != analysisGenerations.end() &&
                  generation->second == request.generation;
        canceled = canceledRequests.contains(request.idKey);
      }
      if (current && !canceled) {
        try {
          completion = lang::LanguageQueries().complete(
              {.entryPath = request.entryPath,
               .source = request.source,
               .byteOffset = request.byteOffset,
               .preludePaths = {standardLibrary.prelude},
               .sourceOverrides = request.sourceOverrides,
               .standardLibraryRoots = {standardLibrary.root}});
        } catch (const std::exception &) {
          completion = {};
        }
      }
      {
        const std::lock_guard lock(stateMutex);
        const auto generation = analysisGenerations.find(request.uri);
        current = current && documents.contains(request.uri) &&
                  generation != analysisGenerations.end() &&
                  generation->second == request.generation;
        canceled = canceled || canceledRequests.erase(request.idKey) > 0;
        activeCompletionRequests.erase(request.idKey);
      }
      if (canceled) {
        sendJson(errorResponse(request.id, -32800, "Request cancelled"));
      } else if (!current) {
        sendJson(errorResponse(request.id, -32801, "Content modified"));
      } else {
        sendJson(
            response(request.id, completionListJson(completion, request.source,
                                                    request.snippetSupport)));
      }
      json_object_put(request.id);
      {
        const std::lock_guard lock(stateMutex);
        --activeCompletions;
      }
      completionCondition.notify_all();
    }
  }

  void scheduleAnalysis(AnalysisRequest request) {
    {
      const std::lock_guard lock(stateMutex);
      if (stoppingAnalysis) {
        return;
      }
      if (!pendingAnalyses.contains(request.uri)) {
        analysisOrder.push_back(request.uri);
      }
      pendingAnalyses[request.uri] = std::move(request);
    }
    analysisCondition.notify_one();
  }

  void runAnalysisWorker() {
    while (true) {
      AnalysisRequest request;
      {
        std::unique_lock lock(stateMutex);
        analysisCondition.wait(lock, [this] {
          return stoppingAnalysis || !analysisOrder.empty();
        });
        if (stoppingAnalysis) {
          return;
        }

        while (!analysisOrder.empty()) {
          const std::string uri = std::move(analysisOrder.front());
          analysisOrder.pop_front();
          const auto pending = pendingAnalyses.find(uri);
          if (pending == pendingAnalyses.end()) {
            continue;
          }
          request = std::move(pending->second);
          pendingAnalyses.erase(pending);
          ++activeAnalyses;
          break;
        }
        if (request.uri.empty()) {
          analysisCondition.notify_all();
          continue;
        }
      }

      try {
        analyzeAndPublish(request);
      } catch (const std::exception &error) {
        std::cerr << "LSP analysis failed: " << error.what() << '\n';
        rejectPendingSemanticGeneration(request.uri, request.generation, -32603,
                                        "Internal error");
      } catch (...) {
        std::cerr << "LSP analysis failed with an unknown exception\n";
        rejectPendingSemanticGeneration(request.uri, request.generation, -32603,
                                        "Internal error");
      }

      {
        const std::lock_guard lock(stateMutex);
        --activeAnalyses;
      }
      analysisCondition.notify_all();
    }
  }

  void analyzeAndPublish(const AnalysisRequest &request) {
    DocumentAnalysis analysis = analyzeDocument(request);
    std::vector<DiagnosticPublication> publications;
    std::optional<AnalysisRequest> retry;
    std::deque<PendingSemanticRequest> semanticRequests;
    {
      const std::lock_guard lock(stateMutex);
      const auto generation = analysisGenerations.find(request.uri);
      if (generation == analysisGenerations.end() ||
          generation->second != request.generation ||
          !documents.contains(request.uri)) {
        return;
      }

      bool dependencyChanged = false;
      for (const std::string &dependency : analysis.dependencies) {
        if (!documents.contains(dependency)) {
          continue;
        }
        const auto snapshot = request.documentGenerations.find(dependency);
        const auto current = analysisGenerations.find(dependency);
        if (snapshot == request.documentGenerations.end() ||
            current == analysisGenerations.end() ||
            snapshot->second != current->second) {
          dependencyChanged = true;
          break;
        }
      }
      if (dependencyChanged) {
        const auto version = documentVersions.find(request.uri);
        retry = makeAnalysisRequestLocked(
            request.uri,
            version == documentVersions.end()
                ? std::nullopt
                : std::optional<std::int64_t>(version->second),
            ++analysisGenerations[request.uri]);
      } else {
        std::unordered_set<std::string> affected{request.uri};
        if (const auto previous = diagnosticsByRoot.find(request.uri);
            previous != diagnosticsByRoot.end()) {
          for (const LspDiagnostic &diagnostic : previous->second) {
            affected.insert(diagnostic.uri);
          }
        }
        for (const LspDiagnostic &diagnostic : analysis.diagnostics) {
          affected.insert(diagnostic.uri);
        }
        diagnosticsByRoot[request.uri] = std::move(analysis.diagnostics);
        dependenciesByRoot[request.uri] = std::move(analysis.dependencies);
        analysisSnapshots[request.uri] = {
            .generation = request.generation,
            .rootPath = std::move(analysis.rootPath),
            .frontend = std::move(analysis.frontend)};
        semanticTokenCache.erase(request.uri);
        publications = publicationsForLocked(affected);
        for (auto pending = pendingSemanticRequests.begin();
             pending != pendingSemanticRequests.end();) {
          if (pending->uri != request.uri ||
              pending->generation != request.generation) {
            ++pending;
            continue;
          }
          semanticRequests.push_back(std::move(*pending));
          pending = pendingSemanticRequests.erase(pending);
        }
      }
    }
    if (retry) {
      rejectPendingSemanticGeneration(request.uri, request.generation, -32801,
                                      "Content modified");
      scheduleAnalysis(std::move(*retry));
      return;
    }
    publish(std::move(publications));
    dispatchSemanticRequests(std::move(semanticRequests));
    requestSemanticTokenRefresh();
  }

  void requestSemanticTokenRefresh() {
    if (!semanticTokenRefreshSupport) {
      return;
    }
    json_object *request = json_object_new_object();
    json_object_object_add(request, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(
        request, "id", json_object_new_int64(nextServerRequestId.fetch_add(1)));
    json_object_object_add(
        request, "method",
        json_object_new_string("workspace/semanticTokens/refresh"));
    sendJson(request);
  }

  std::string clientUriForKeyLocked(std::string_view key) const {
    const auto preferred = clientUris.find(std::string(key));
    return preferred == clientUris.end() ? std::string(key) : preferred->second;
  }

  void useClientUrisLocked(LspDiagnostic &diagnostic) const {
    diagnostic.uri = clientUriForKeyLocked(diagnostic.uri);
    for (LspRelatedDiagnostic &related : diagnostic.related) {
      related.uri = clientUriForKeyLocked(related.uri);
    }
    for (LspFixIt &fix : diagnostic.fixes) {
      fix.uri = clientUriForKeyLocked(fix.uri);
    }
  }

  std::vector<DiagnosticPublication>
  publicationsForLocked(const std::unordered_set<std::string> &uris) const {
    std::vector<DiagnosticPublication> publications;
    publications.reserve(uris.size());
    for (const std::string &uri : uris) {
      DiagnosticPublication publication{.uri = clientUriForKeyLocked(uri)};
      if (const auto version = documentVersions.find(uri);
          version != documentVersions.end()) {
        publication.version = version->second;
      }
      std::unordered_set<std::string> seen;
      for (const auto &[_, diagnostics] : diagnosticsByRoot) {
        for (const LspDiagnostic &diagnostic : diagnostics) {
          if (diagnostic.uri != uri) {
            continue;
          }
          const lang::SourceSpan &span = diagnostic.diagnostic.primary;
          const std::string key = diagnostic.diagnostic.code + '\n' +
                                  std::to_string(span.start) + ':' +
                                  std::to_string(span.end) + '\n' +
                                  diagnostic.diagnostic.message;
          if (seen.insert(key).second) {
            LspDiagnostic published = diagnostic;
            useClientUrisLocked(published);
            publication.diagnostics.push_back(std::move(published));
          }
        }
      }
      publications.push_back(std::move(publication));
    }
    return publications;
  }

  void publish(std::vector<DiagnosticPublication> publications) const {
    for (const DiagnosticPublication &publication : publications) {
      json_object *diagnostics = json_object_new_array();
      for (const LspDiagnostic &diagnostic : publication.diagnostics) {
        json_object_array_add(diagnostics,
                              diagnosticJson(diagnostic,
                                             diagnosticRelatedInformation,
                                             diagnosticData));
      }
      publishDiagnostics(publication.uri, diagnostics, publication.version);
    }
  }

  static void
  publishDiagnostics(const std::string &uri, json_object *diagnostics,
                     std::optional<std::int64_t> version = std::nullopt) {
    json_object *params = json_object_new_object();
    json_object_object_add(params, "uri", json_object_new_string(uri.c_str()));
    if (version) {
      json_object_object_add(params, "version",
                             json_object_new_int64(*version));
    }
    json_object_object_add(params, "diagnostics", diagnostics);

    json_object *notification = json_object_new_object();
    json_object_object_add(notification, "jsonrpc",
                           json_object_new_string("2.0"));
    json_object_object_add(
        notification, "method",
        json_object_new_string("textDocument/publishDiagnostics"));
    json_object_object_add(notification, "params", params);
    sendJson(notification);
  }

  void flushAnalyses() {
    std::unique_lock lock(stateMutex);
    analysisCondition.wait(lock, [this] {
      return pendingAnalyses.empty() && analysisOrder.empty() &&
             activeAnalyses == 0;
    });
  }

  void flushCompletions() {
    std::unique_lock lock(stateMutex);
    completionCondition.wait(lock, [this] {
      return completionRequests.empty() && activeCompletions == 0;
    });
  }

  void stopCompletionWorker() {
    std::deque<CompletionRequest> abandoned;
    {
      const std::lock_guard lock(stateMutex);
      if (stoppingCompletion) {
        return;
      }
      stoppingCompletion = true;
      abandoned.swap(completionRequests);
    }
    for (CompletionRequest &request : abandoned) {
      json_object_put(request.id);
    }
    completionCondition.notify_all();
    if (completionWorker.joinable()) {
      completionWorker.join();
    }
  }

  void stopAnalysisWorker() {
    std::deque<PendingSemanticRequest> abandoned;
    {
      const std::lock_guard lock(stateMutex);
      if (stoppingAnalysis) {
        return;
      }
      stoppingAnalysis = true;
      pendingAnalyses.clear();
      analysisOrder.clear();
      abandoned.swap(pendingSemanticRequests);
    }
    for (PendingSemanticRequest &request : abandoned) {
      json_object_put(request.params);
      json_object_put(request.id);
    }
    analysisCondition.notify_all();
    if (analysisWorker.joinable()) {
      analysisWorker.join();
    }
  }

  lang::StandardLibraryLayout standardLibrary;
  mutable std::mutex stateMutex;
  std::condition_variable analysisCondition;
  std::condition_variable completionCondition;
  std::unordered_map<std::string, std::string> clientUris;
  std::unordered_map<std::string, std::unordered_set<std::string>>
      openClientUris;
  std::unordered_map<std::string, std::string> documents;
  std::unordered_map<std::string, std::int64_t> documentVersions;
  std::unordered_map<std::string, std::vector<LspDiagnostic>> diagnosticsByRoot;
  std::unordered_map<std::string, std::unordered_set<std::string>>
      dependenciesByRoot;
  std::unordered_map<std::string, std::uint64_t> analysisGenerations;
  std::unordered_map<std::string, AnalysisSnapshot> analysisSnapshots;
  std::unordered_map<std::string, CachedSemanticTokens> semanticTokenCache;
  std::unordered_map<std::string, AnalysisRequest> pendingAnalyses;
  std::deque<std::string> analysisOrder;
  std::deque<CompletionRequest> completionRequests;
  std::deque<PendingSemanticRequest> pendingSemanticRequests;
  std::unordered_set<std::string> activeCompletionRequests;
  std::unordered_set<std::string> canceledRequests;
  std::size_t activeAnalyses = 0;
  std::size_t activeCompletions = 0;
  bool stoppingAnalysis = false;
  bool stoppingCompletion = false;
  bool shutdownRequested = false;
  bool markdownHover = false;
  bool completionSnippets = false;
  bool semanticTokenRefreshSupport = false;
  bool workspaceDocumentChanges = false;
  bool watchedFilesDynamicRegistration = false;
  bool diagnosticRelatedInformation = false;
  bool diagnosticData = false;
  bool codeActionLiterals = false;
  bool preferredCodeActions = false;
  std::atomic<std::uint64_t> nextServerRequestId{1};
  std::thread analysisWorker;
  std::thread completionWorker;
};

} // namespace

int main(int argc, char *argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::cout << "gti_lsp " << GTI_VERSION << '\n';
    return 0;
  }
  return LanguageServer(
             lang::discoverStandardLibrary(argc > 0 ? argv[0] : "gti_lsp",
                                           GTI_BUILD_STDLIB_ROOT))
      .run();
}
