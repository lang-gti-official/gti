#include "gti/driver/compilation.h"
#include "gti/driver/project_facts.h"
#include "gti/format_config.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/lexer.h"
#include "gti/standard_library.h"
#include "gti/support.h"
#include "gti/token.h"

#include "frontend_snapshot_state.h"

#include <llvm/Support/JSON.h>
#include <llvm/Support/thread.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using JsonArray = llvm::json::Array;
using JsonObject = llvm::json::Object;
using JsonValue = llvm::json::Value;

using SteadyClock = std::chrono::steady_clock;

// Darwin's default secondary-thread stack is too small for the recursive
// frontend in an unoptimized build: native exhaustion can otherwise precede
// the compiler's structured 64-level parser diagnostic. Keep the two workers
// long-lived, but give both compiler-facing queues the same practical stack
// budget as an ordinary compiler process.
constexpr unsigned frontendWorkerStackSize = 8U * 1024U * 1024U;

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

std::optional<std::filesystem::path> frontendTracePathFromEnvironment() {
  const char *configured = std::getenv("GTI_LSP_FRONTEND_TRACE");
  if (configured == nullptr || *configured == '\0') {
    return std::nullopt;
  }
  return std::filesystem::path(configured);
}

JsonArray stringArray(const std::vector<std::string> &values) {
  JsonArray result;
  result.reserve(values.size());
  for (const std::string &value : values) {
    result.emplace_back(value);
  }
  return result;
}

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
  InactiveCode = 1U << 6U,
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

JsonValue positionJson(Position position) {
  return JsonObject{{"line", position.line}, {"character", position.character}};
}

JsonValue rangeJson(std::string_view source, std::size_t byteOffset,
                    std::size_t byteLength) {
  const std::size_t end = std::min(source.size(), byteOffset + byteLength);
  return JsonObject{{"start", positionJson(positionAt(source, byteOffset))},
                    {"end", positionJson(positionAt(source, end))}};
}

const JsonValue *member(const JsonValue *value, llvm::StringRef name) {
  const JsonObject *object = value != nullptr ? value->getAsObject() : nullptr;
  return object != nullptr ? object->get(name) : nullptr;
}

std::string stringMember(const JsonValue *object, llvm::StringRef name) {
  const JsonValue *value = member(object, name);
  const std::optional<llvm::StringRef> string =
      value != nullptr ? value->getAsString() : std::nullopt;
  return string ? string->str() : std::string{};
}

std::size_t sizeMember(const JsonValue *object, llvm::StringRef name,
                       std::size_t fallback) {
  const JsonValue *value = member(object, name);
  const std::optional<std::int64_t> number =
      value != nullptr ? value->getAsInteger() : std::nullopt;
  if (!number) {
    return fallback;
  }
  return *number > 0 ? static_cast<std::size_t>(*number) : fallback;
}

std::optional<std::int64_t> integerMember(const JsonValue *object,
                                          llvm::StringRef name) {
  const JsonValue *value = member(object, name);
  return value != nullptr ? value->getAsInteger() : std::nullopt;
}

std::optional<Position> positionMember(const JsonValue *object) {
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

std::optional<LspRange> rangeMember(const JsonValue *object) {
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

bool boolMember(const JsonValue *object, llvm::StringRef name, bool fallback) {
  const JsonValue *value = member(object, name);
  const std::optional<bool> boolean =
      value != nullptr ? value->getAsBoolean() : std::nullopt;
  return boolean.value_or(fallback);
}

std::string requestIdKey(const JsonValue *id) {
  if (id == nullptr ||
      (!id->getAsInteger().has_value() && !id->getAsString().has_value())) {
    return {};
  }
  return llvm::formatv("{0}", *id).str();
}

bool formatListContains(const JsonValue *formatList, std::string_view format) {
  const JsonArray *formats =
      formatList != nullptr ? formatList->getAsArray() : nullptr;
  if (formats == nullptr) {
    return false;
  }
  for (const JsonValue &candidate : *formats) {
    const std::optional<llvm::StringRef> value = candidate.getAsString();
    if (value && llvm::StringRef(format.data(), format.size()) == *value) {
      return true;
    }
  }
  return false;
}

bool supportsHoverFormat(const JsonValue *params, std::string_view format) {
  return formatListContains(
      member(member(member(member(params, "capabilities"), "textDocument"),
                    "hover"),
             "contentFormat"),
      format);
}

bool supportsCompletionDocumentationMarkdown(const JsonValue *params) {
  return formatListContains(
      member(
          member(member(member(member(params, "capabilities"), "textDocument"),
                        "completion"),
                 "completionItem"),
          "documentationFormat"),
      "markdown");
}

bool supportsSignatureDocumentationMarkdown(const JsonValue *params) {
  return formatListContains(
      member(
          member(member(member(member(params, "capabilities"), "textDocument"),
                        "signatureHelp"),
                 "signatureInformation"),
          "documentationFormat"),
      "markdown");
}

bool supportsCompletionSnippets(const JsonValue *params) {
  const JsonValue *completionItem =
      member(member(member(member(params, "capabilities"), "textDocument"),
                    "completion"),
             "completionItem");
  return boolMember(completionItem, "snippetSupport", false);
}

bool supportsSemanticTokenRefresh(const JsonValue *params) {
  const JsonValue *semanticTokens = member(
      member(member(params, "capabilities"), "workspace"), "semanticTokens");
  return boolMember(semanticTokens, "refreshSupport", false);
}

bool supportsWorkspaceDocumentChanges(const JsonValue *params) {
  const JsonValue *workspaceEdit = member(
      member(member(params, "capabilities"), "workspace"), "workspaceEdit");
  return boolMember(workspaceEdit, "documentChanges", false);
}

bool supportsWatchedFilesDynamicRegistration(const JsonValue *params) {
  const JsonValue *watchedFiles =
      member(member(member(params, "capabilities"), "workspace"),
             "didChangeWatchedFiles");
  return boolMember(watchedFiles, "dynamicRegistration", false);
}

const JsonValue *publishDiagnosticsCapabilities(const JsonValue *params) {
  return member(member(member(params, "capabilities"), "textDocument"),
                "publishDiagnostics");
}

bool supportsDiagnosticRelatedInformation(const JsonValue *params) {
  return boolMember(publishDiagnosticsCapabilities(params),
                    "relatedInformation", false);
}

bool supportsDiagnosticData(const JsonValue *params) {
  return boolMember(publishDiagnosticsCapabilities(params), "dataSupport",
                    false);
}

bool supportsDiagnosticTag(const JsonValue *params, lang::DiagnosticTag tag) {
  const JsonValue *valueSet = member(
      member(publishDiagnosticsCapabilities(params), "tagSupport"), "valueSet");
  const JsonArray *values =
      valueSet == nullptr ? nullptr : valueSet->getAsArray();
  if (values == nullptr) {
    return false;
  }
  const std::int64_t expected = static_cast<std::int64_t>(tag);
  return std::any_of(values->begin(), values->end(),
                     [expected](const JsonValue &value) {
                       return value.getAsInteger() == expected;
                     });
}

const JsonValue *codeActionCapabilities(const JsonValue *params) {
  return member(member(member(params, "capabilities"), "textDocument"),
                "codeAction");
}

bool supportsCodeActionLiterals(const JsonValue *params) {
  const JsonValue *literalSupport =
      member(codeActionCapabilities(params), "codeActionLiteralSupport");
  const JsonValue *valueSet =
      member(member(literalSupport, "codeActionKind"), "valueSet");
  return literalSupport != nullptr &&
         literalSupport->getAsObject() != nullptr && valueSet != nullptr &&
         valueSet->getAsArray() != nullptr;
}

bool supportsPreferredCodeActions(const JsonValue *params) {
  return boolMember(codeActionCapabilities(params), "isPreferredSupport",
                    false);
}

bool supportsHierarchicalDocumentSymbols(const JsonValue *params) {
  const JsonValue *documentSymbol = member(
      member(member(params, "capabilities"), "textDocument"), "documentSymbol");
  return boolMember(documentSymbol, "hierarchicalDocumentSymbolSupport", false);
}

bool contextAllowsQuickFix(const JsonValue *context) {
  const JsonValue *only = member(context, "only");
  const JsonArray *kinds = only != nullptr ? only->getAsArray() : nullptr;
  if (kinds == nullptr) {
    return true;
  }
  for (const JsonValue &kind : *kinds) {
    const std::optional<llvm::StringRef> name = kind.getAsString();
    if (name && *name == "quickfix") {
      return true;
    }
  }
  return false;
}

void sendJson(JsonValue message) {
  const std::lock_guard lock(outputMutex);
  const std::string json = llvm::formatv("{0}", message).str();
  std::cout << "Content-Length: " << json.size() << "\r\n\r\n";
  std::cout.write(json.data(), static_cast<std::streamsize>(json.size()));
  std::cout.flush();
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

JsonValue response(const JsonValue *id, JsonValue result) {
  return JsonObject{{"jsonrpc", "2.0"},
                    {"id", id != nullptr ? JsonValue(*id) : JsonValue(nullptr)},
                    {"result", std::move(result)}};
}

JsonValue response(const JsonValue &id, JsonValue result) {
  return response(&id, std::move(result));
}

JsonValue errorResponse(const JsonValue *id, int code,
                        std::string_view message) {
  return JsonObject{
      {"jsonrpc", "2.0"},
      {"id", id != nullptr ? JsonValue(*id) : JsonValue(nullptr)},
      {"error", JsonObject{{"code", code}, {"message", std::string(message)}}}};
}

JsonValue errorResponse(const JsonValue &id, int code,
                        std::string_view message) {
  return errorResponse(&id, code, message);
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

std::string diagnosticMessage(const lang::Diagnostic &diagnostic,
                              bool fixAvailable = false) {
  std::string message = diagnostic.message;
  if (fixAvailable) {
    message += " (fix available)";
  }
  for (const std::string &hint : diagnostic.hints) {
    message += "\nhelp: " + hint;
  }
  return message;
}

void mergeDiagnosticRelatedInformation(LspDiagnostic &target,
                                       const LspDiagnostic &additional) {
  for (const LspRelatedDiagnostic &related : additional.related) {
    const bool duplicate = std::any_of(
        target.related.begin(), target.related.end(),
        [&](const LspRelatedDiagnostic &existing) {
          return existing.uri == related.uri &&
                 existing.related.span == related.related.span &&
                 existing.related.message == related.related.message;
        });
    if (!duplicate) {
      target.related.push_back(related);
      target.diagnostic.related.push_back(related.related);
    }
  }
}

JsonValue diagnosticJson(const LspDiagnostic &published,
                         bool includeRelatedInformation, bool includeData,
                         bool includeFixAvailability,
                         bool includeUnnecessaryTag,
                         bool includeDeprecatedTag) {
  const lang::Diagnostic &diagnostic = published.diagnostic;
  JsonObject result{
      {"range",
       rangeJson(published.source, diagnostic.primary.start,
                 diagnostic.primary.end >= diagnostic.primary.start
                     ? diagnostic.primary.end - diagnostic.primary.start
                     : 0)},
      {"severity", static_cast<int>(diagnostic.severity)},
      {"source", std::string(diagnosticSource)}};
  if (!diagnostic.code.empty()) {
    result["code"] = diagnostic.code;
  }

  const std::string message = diagnosticMessage(
      diagnostic, includeFixAvailability && !published.fixes.empty());
  result["message"] = message;

  JsonArray tags;
  for (const lang::DiagnosticTag tag : diagnostic.tags) {
    if ((tag == lang::DiagnosticTag::Unnecessary && includeUnnecessaryTag) ||
        (tag == lang::DiagnosticTag::Deprecated && includeDeprecatedTag)) {
      tags.push_back(static_cast<int>(tag));
    }
  }
  if (!tags.empty()) {
    result["tags"] = std::move(tags);
  }

  if (includeRelatedInformation && !published.related.empty()) {
    JsonArray relatedInformation;
    for (const LspRelatedDiagnostic &related : published.related) {
      JsonObject location{
          {"uri", related.uri},
          {"range",
           rangeJson(related.source, related.related.span.start,
                     related.related.span.end >= related.related.span.start
                         ? related.related.span.end - related.related.span.start
                         : 0)}};
      relatedInformation.push_back(
          JsonObject{{"location", std::move(location)},
                     {"message", related.related.message}});
    }
    result["relatedInformation"] = std::move(relatedInformation);
  }
  if (includeData) {
    JsonObject data;
    const std::string_view phase = lang::phaseName(diagnostic.phase);
    data["phase"] = std::string(phase);
    if (!diagnostic.group.empty()) {
      data["group"] = diagnostic.group;
    }
    if (!diagnostic.hints.empty()) {
      JsonArray hints;
      for (const std::string &hint : diagnostic.hints) {
        hints.push_back(hint);
      }
      data["hints"] = std::move(hints);
    }
    if (!published.fixes.empty()) {
      JsonArray fixes;
      for (const LspFixIt &fix : published.fixes) {
        fixes.push_back(JsonObject{
            {"uri", fix.uri},
            {"range", rangeJson(fix.source, fix.fix.span.start,
                                fix.fix.span.end >= fix.fix.span.start
                                    ? fix.fix.span.end - fix.fix.span.start
                                    : 0)},
            {"replacement", fix.fix.replacement},
            {"message", fix.fix.message}});
      }
      data["fixes"] = std::move(fixes);
    }
    result["data"] = std::move(data);
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
                         const JsonValue *requestedDiagnostics,
                         bool includeFixAvailability) {
  const JsonArray *requestedValues = requestedDiagnostics != nullptr
                                         ? requestedDiagnostics->getAsArray()
                                         : nullptr;
  if (requestedValues == nullptr) {
    return false;
  }
  const Position expectedStart =
      positionAt(diagnostic.source, diagnostic.diagnostic.primary.start);
  const Position expectedEnd =
      positionAt(diagnostic.source, diagnostic.diagnostic.primary.end);
  for (const JsonValue &requested : *requestedValues) {
    const std::optional<LspRange> range =
        rangeMember(member(&requested, "range"));
    if (!range || !samePosition(range->start, expectedStart) ||
        !samePosition(range->end, expectedEnd)) {
      continue;
    }
    if (stringMember(&requested, "code") != diagnostic.diagnostic.code) {
      continue;
    }
    if (stringMember(&requested, "source") != diagnosticSource) {
      continue;
    }
    if (stringMember(&requested, "message") !=
        diagnosticMessage(diagnostic.diagnostic,
                          includeFixAvailability &&
                              !diagnostic.fixes.empty())) {
      continue;
    }
    return true;
  }
  return false;
}

JsonValue codeActionJson(const CodeActionCandidate &candidate,
                         bool documentChanges, bool includeRelatedInformation,
                         bool includeDiagnosticData, bool includePreferred,
                         bool includeUnnecessaryTag,
                         bool includeDeprecatedTag) {
  const std::string title =
      candidate.fix.fix.message.empty()
          ? (candidate.diagnostic.diagnostic.code.empty()
                 ? "Apply suggested fix"
                 : "Apply fix for " + candidate.diagnostic.diagnostic.code)
          : candidate.fix.fix.message;
  JsonArray diagnostics;
  diagnostics.push_back(diagnosticJson(
      candidate.diagnostic, includeRelatedInformation, includeDiagnosticData,
      true, includeUnnecessaryTag, includeDeprecatedTag));
  JsonObject action{{"title", title},
                    {"kind", "quickfix"},
                    {"diagnostics", std::move(diagnostics)}};
  if (includePreferred) {
    action["isPreferred"] = candidate.preferred;
  }

  JsonArray edits;
  edits.push_back(JsonObject{
      {"range",
       rangeJson(candidate.fix.source, candidate.fix.fix.span.start,
                 candidate.fix.fix.span.end >= candidate.fix.fix.span.start
                     ? candidate.fix.fix.span.end - candidate.fix.fix.span.start
                     : 0)},
      {"newText", candidate.fix.fix.replacement}});

  JsonObject workspaceEdit;
  if (documentChanges) {
    JsonObject textDocument{{"uri", candidate.fix.uri},
                            {"version", candidate.version
                                            ? JsonValue(*candidate.version)
                                            : JsonValue(nullptr)}};
    JsonArray changes;
    changes.push_back(JsonObject{{"textDocument", std::move(textDocument)},
                                 {"edits", std::move(edits)}});
    workspaceEdit["documentChanges"] = std::move(changes);
  } else {
    JsonObject changes;
    changes[candidate.fix.uri] = std::move(edits);
    workspaceEdit["changes"] = std::move(changes);
  }
  action["edit"] = std::move(workspaceEdit);
  return action;
}

std::optional<SemanticClassification>
basicSemanticType(const std::vector<lang::Token> &tokens, std::size_t index) {
  using enum lang::TokenKind;
  const lang::Token &token = tokens[index];

  // Tree-sitter has more precise captures for these syntax-owned tokens than
  // the LSP semantic-token vocabulary. Emitting them would override captures
  // such as @keyword.directive.define, @function.macro, @boolean,
  // @constant.builtin, and @variable.builtin.
  if (token.kind == TRUE || token.kind == FALSE || token.kind == NULLPTR ||
      token.kind == THIS || token.kind == HASH_DEFINE) {
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

  if (token.lexeme == "c_array" && index >= 2 && index + 5 < tokens.size() &&
      tokens[index - 2].kind == LEFT_BRACKET &&
      tokens[index - 1].kind == LEFT_BRACKET &&
      tokens[index + 1].kind == LEFT_PAREN &&
      tokens[index + 2].kind == IDENTIFIER &&
      tokens[index + 3].kind == RIGHT_PAREN &&
      tokens[index + 4].kind == RIGHT_BRACKET &&
      tokens[index + 5].kind == RIGHT_BRACKET) {
    return SemanticClassification{Decorator, 0};
  }

  // Attribute spelling is lexical. The owner argument is deliberately left to
  // compiler occurrences so definition/rename and parameter tokens share its
  // resolved symbol identity, including on recovered declarations.
  if (token.lexeme == "borrowed_from" && index + 1 < tokens.size() &&
      tokens[index + 1].kind == LEFT_PAREN) {
    std::size_t begin = index;
    while (begin > 0 &&
           (tokens[begin - 1].kind == IDENTIFIER ||
            tokens[begin - 1].kind == THIS || tokens[begin - 1].kind == COMMA ||
            tokens[begin - 1].kind == LEFT_PAREN ||
            tokens[begin - 1].kind == RIGHT_PAREN)) {
      --begin;
    }
    if (begin >= 2 && tokens[begin - 2].kind == LEFT_BRACKET &&
        tokens[begin - 1].kind == LEFT_BRACKET) {
      return SemanticClassification{Decorator, 0};
    }
  }

  if (token.lexeme == "link_name" && index >= 2 && index + 5 < tokens.size() &&
      tokens[index - 2].kind == LEFT_BRACKET &&
      tokens[index - 1].kind == LEFT_BRACKET &&
      tokens[index + 1].kind == LEFT_PAREN &&
      tokens[index + 2].kind == STRING_LITERAL &&
      tokens[index + 3].kind == RIGHT_PAREN &&
      tokens[index + 4].kind == RIGHT_BRACKET &&
      tokens[index + 5].kind == RIGHT_BRACKET) {
    return SemanticClassification{Decorator, 0};
  }

  std::size_t attributeBegin = index;
  while (attributeBegin > 0 && (tokens[attributeBegin - 1].kind == IDENTIFIER ||
                                tokens[attributeBegin - 1].kind == COMMA)) {
    --attributeBegin;
  }
  std::size_t attributeEnd = index + 1;
  while (attributeEnd < tokens.size() &&
         (tokens[attributeEnd].kind == IDENTIFIER ||
          tokens[attributeEnd].kind == COMMA)) {
    ++attributeEnd;
  }
  if (attributeBegin >= 2 && attributeEnd + 2 < tokens.size() &&
      tokens[attributeBegin - 2].kind == LEFT_BRACKET &&
      tokens[attributeBegin - 1].kind == LEFT_BRACKET &&
      tokens[attributeEnd].kind == RIGHT_BRACKET &&
      tokens[attributeEnd + 1].kind == RIGHT_BRACKET &&
      (tokens[attributeEnd + 2].kind == CLASS ||
       tokens[attributeEnd + 2].kind == STRUCT ||
       tokens[attributeEnd + 2].kind == INTERFACE ||
       tokens[attributeEnd + 2].kind == UNION)) {
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
  const auto appendComment = [&](std::size_t start, std::size_t end) {
    for (std::size_t segmentStart = start; segmentStart < end;) {
      const std::size_t newline = source.find('\n', segmentStart);
      const std::size_t segmentEnd =
          newline == std::string_view::npos || newline > end ? end : newline;
      if (segmentEnd > segmentStart) {
        result.push_back({positions.at(segmentStart),
                          utf16Length(source.substr(segmentStart,
                                                    segmentEnd - segmentStart)),
                          Comment, 0});
      }
      segmentStart = segmentEnd < end ? segmentEnd + 1 : end;
    }
  };

  for (std::size_t index = 0; index < source.size();) {
    if (source[index] == '"' || source[index] == '\'') {
      const char delimiter = source[index];
      for (++index; index < source.size();) {
        if (source[index] == '\\' && index + 1 < source.size()) {
          index += 2;
        } else if (source[index++] == delimiter) {
          break;
        }
      }
      continue;
    }
    if (source[index] != '/' || index + 1 >= source.size()) {
      ++index;
      continue;
    }

    if (source[index + 1] == '/') {
      const std::size_t start = index;
      const std::size_t newline = source.find('\n', start);
      index = newline == std::string_view::npos ? source.size() : newline;
      appendComment(start, index);
      continue;
    }
    if (source[index + 1] == '*') {
      const std::size_t start = index;
      const std::size_t close = source.find("*/", start + 2);
      index = close == std::string_view::npos ? source.size() : close + 2;
      appendComment(start, index);
      continue;
    }
    ++index;
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
  case lang::SymbolKind::RequirementParameter:
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
  case lang::SymbolKind::RequirementParameter:
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
    const lang::FrontendResult &snapshot, lang::SourceUnitId sourceUnit) {
  const lang::SemanticDatabase &database = snapshot.semantics.database();
  std::unordered_map<std::size_t, std::size_t> tokenAt;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    if (tokens[index].kind == lang::TokenKind::IDENTIFIER) {
      tokenAt.emplace(tokens[index].position, index);
    }
  }
  for (const lang::SemanticOccurrence &occurrence :
       database.occurrences(sourceUnit)) {
    if (occurrence.symbol == 0 ||
        !snapshot.semantics.canPresent(sourceUnit, occurrence,
                                       snapshot.sourceGraph)) {
      continue;
    }
    const auto token = tokenAt.find(occurrence.span.start);
    if (token == tokenAt.end() ||
        tokens[token->second].position + tokens[token->second].lexeme.size() !=
            occurrence.span.end) {
      continue;
    }
    const lang::SymbolRecord *symbol = database.findSymbol(occurrence.symbol);
    if (symbol == nullptr || !snapshot.semantics.canPresent(
                                 sourceUnit, *symbol, snapshot.sourceGraph)) {
      continue;
    }
    classifications[token->second] =
        classificationForSymbol(*symbol, occurrence.roles);
  }
}

std::vector<std::optional<lang::ConfigurationTokenKind>>
configurationTokenRoles(const std::vector<lang::Token> &tokens,
                        const lang::SourceUnit *unit) {
  std::vector<std::optional<lang::ConfigurationTokenKind>> roles(tokens.size());
  if (unit == nullptr) {
    return roles;
  }

  std::unordered_map<std::size_t, std::size_t> tokenAt;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    tokenAt.emplace(tokens[index].position, index);
  }
  for (const lang::ConfigurationToken &configuration :
       unit->configurationTokens) {
    const auto token = tokenAt.find(configuration.span.start);
    if (token == tokenAt.end() ||
        tokens[token->second].position + tokens[token->second].lexeme.size() !=
            configuration.span.end) {
      continue;
    }
    roles[token->second] = configuration.kind;
  }
  return roles;
}

std::vector<SemanticToken>
collectSemanticTokens(std::string_view source,
                      const lang::FrontendResult *snapshot = nullptr,
                      lang::SourceUnitId sourceUnit = 0) {
  const SourcePositionIndex positions(source);
  lang::Lexer lexer;
  const std::vector<lang::Token> tokens = lexer.scan(std::string(source));
  std::vector<std::optional<SemanticClassification>> classifications;
  std::vector<bool> inactive(tokens.size(), false);
  const lang::SourceUnit *unit = nullptr;
  if (snapshot != nullptr && sourceUnit != 0) {
    unit = snapshot->sourceGraph.findUnit(sourceUnit);
    if (unit != nullptr) {
      std::size_t inactiveIndex = 0;
      for (std::size_t index = 0; index < tokens.size(); ++index) {
        const std::size_t start = tokens[index].position;
        const std::size_t end = start + tokens[index].lexeme.size();
        while (inactiveIndex < unit->inactiveSpans.size() &&
               unit->inactiveSpans[inactiveIndex].end <= start) {
          ++inactiveIndex;
        }
        inactive[index] = inactiveIndex < unit->inactiveSpans.size() &&
                          start < unit->inactiveSpans[inactiveIndex].end &&
                          end > unit->inactiveSpans[inactiveIndex].start;
      }
    }
  }
  const std::vector<std::optional<lang::ConfigurationTokenKind>>
      configurationRoles = configurationTokenRoles(tokens, unit);
  classifications.reserve(tokens.size());
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    classifications.push_back(basicSemanticType(tokens, index));
  }
  classifyStandardLibraryIncludes(tokens, classifications);
  if (snapshot != nullptr && sourceUnit != 0) {
    for (std::size_t index = 0; index < tokens.size(); ++index) {
      if (tokens[index].kind == lang::TokenKind::IDENTIFIER &&
          !configurationRoles[index] && tokens[index].lexeme != "discard" &&
          tokens[index].lexeme != "target" &&
          (!classifications[index] ||
           (classifications[index]->type != String &&
            classifications[index]->type != Decorator))) {
        if (inactive[index]) {
          classifications[index] = SemanticClassification{Variable, 0};
        } else {
          classifications[index].reset();
        }
      }
    }
    applyResolvedSymbolClassifications(tokens, classifications, *snapshot,
                                       sourceUnit);
  }
  for (std::size_t index = 0; index < configurationRoles.size(); ++index) {
    if (!configurationRoles[index]) {
      continue;
    }
    if (*configurationRoles[index] == lang::ConfigurationTokenKind::Flag) {
      classifications[index] = SemanticClassification{Macro, 0};
    } else {
      // The syntax query distinguishes defined(...) as @function.macro; an
      // LSP token would be less precise and take precedence in Neovim.
      classifications[index].reset();
    }
  }
  for (std::size_t index = 0; index < classifications.size(); ++index) {
    if (inactive[index] && classifications[index]) {
      classifications[index]->modifiers |= InactiveCode;
    }
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

JsonValue semanticTokensJson(const std::vector<SemanticToken> &tokens) {
  JsonArray data;
  data.reserve(tokens.size() * 5);
  Position previous;

  for (const SemanticToken &token : tokens) {
    const std::uint32_t deltaLine = token.position.line - previous.line;
    const std::uint32_t deltaCharacter =
        deltaLine == 0 ? token.position.character - previous.character
                       : token.position.character;
    data.push_back(deltaLine);
    data.push_back(deltaCharacter);
    data.push_back(token.length);
    data.push_back(token.type);
    data.push_back(token.modifiers);
    previous = token.position;
  }

  return JsonObject{{"data", std::move(data)}};
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

// Retained `///` documentation as LSP MarkupContent. The value is the
// compiler-owned Markdown either way; the kind follows the client's declared
// documentationFormat support, mirroring hover's contentFormat handling.
JsonValue documentationJson(const std::string &markdown, bool markdownSupport) {
  return JsonObject{{"kind", markdownSupport ? "markdown" : "plaintext"},
                    {"value", markdown}};
}

JsonValue completionListJson(const lang::CompletionResult &completion,
                             std::string_view source, bool snippetSupport,
                             bool markdownDocumentation) {
  JsonArray items;
  items.reserve(completion.candidates.size());
  for (const lang::CompletionCandidate &candidate : completion.candidates) {
    JsonObject item{{"label", candidate.label},
                    {"kind", completionItemKind(candidate.kind)},
                    {"filterText", candidate.label},
                    {"sortText", candidate.sortText}};
    if (!candidate.detail.empty()) {
      item["detail"] = candidate.detail;
    }
    if (candidate.documentationMarkdown) {
      item["documentation"] = documentationJson(
          *candidate.documentationMarkdown, markdownDocumentation);
    }

    const bool useSnippet = snippetSupport && candidate.snippet.has_value();
    const std::string &insertion =
        useSnippet ? *candidate.snippet : candidate.insertion;
    item["textEdit"] =
        JsonObject{{"range", rangeJson(source, candidate.replacementRange.start,
                                       candidate.replacementRange.end -
                                           candidate.replacementRange.start)},
                   {"newText", insertion}};
    if (useSnippet) {
      item["insertTextFormat"] = 2;
    }
    items.push_back(std::move(item));
  }

  return JsonObject{{"isIncomplete", completion.isIncomplete},
                    {"items", std::move(items)}};
}

// LSP SymbolKind numbers for compiler-classified document symbols.
int lspSymbolKind(lang::DocumentSymbolKind kind) {
  switch (kind) {
  case lang::DocumentSymbolKind::Namespace:
    return 3;
  case lang::DocumentSymbolKind::Class:
    return 5;
  case lang::DocumentSymbolKind::Struct:
  case lang::DocumentSymbolKind::Union:
    return 23;
  case lang::DocumentSymbolKind::Interface:
  case lang::DocumentSymbolKind::Concept:
    return 11;
  case lang::DocumentSymbolKind::Enum:
    return 10;
  case lang::DocumentSymbolKind::Enumerator:
    return 22;
  case lang::DocumentSymbolKind::Function:
    return 12;
  case lang::DocumentSymbolKind::Method:
  case lang::DocumentSymbolKind::Destructor:
    return 6;
  case lang::DocumentSymbolKind::Constructor:
    return 9;
  case lang::DocumentSymbolKind::Operator:
    return 25;
  case lang::DocumentSymbolKind::Field:
    return 8;
  case lang::DocumentSymbolKind::Variable:
    return 13;
  case lang::DocumentSymbolKind::TypeAlias:
    return 5;
  }
  return 13;
}

JsonArray
documentSymbolArray(const std::vector<lang::DocumentSymbolInfo> &symbols,
                    const std::string &source) {
  JsonArray result;
  for (const lang::DocumentSymbolInfo &symbol : symbols) {
    JsonObject json{
        {"name", symbol.name},
        {"kind", lspSymbolKind(symbol.kind)},
        {"range", rangeJson(source, symbol.range.start,
                            symbol.range.end - symbol.range.start)},
        {"selectionRange",
         rangeJson(source, symbol.selectionRange.start,
                   symbol.selectionRange.end - symbol.selectionRange.start)}};
    if (!symbol.detail.empty()) {
      json["detail"] = symbol.detail;
    }
    if (!symbol.children.empty()) {
      json["children"] = documentSymbolArray(symbol.children, source);
    }
    result.push_back(std::move(json));
  }
  return result;
}

void flattenDocumentSymbols(
    const std::vector<lang::DocumentSymbolInfo> &symbols,
    const std::string &container, const std::string &clientUri,
    const std::string &source, JsonArray &result) {
  for (const lang::DocumentSymbolInfo &symbol : symbols) {
    JsonObject json{
        {"name", symbol.name},
        {"kind", lspSymbolKind(symbol.kind)},
        {"location", JsonObject{{"uri", clientUri},
                                {"range", rangeJson(source, symbol.range.start,
                                                    symbol.range.end -
                                                        symbol.range.start)}}}};
    if (!container.empty()) {
      json["containerName"] = container;
    }
    result.push_back(std::move(json));
    flattenDocumentSymbols(symbol.children, symbol.name, clientUri, source,
                           result);
  }
}

struct AnalysisRequest {
  std::string uri;
  std::string source;
  std::optional<std::int64_t> version;
  std::uint64_t generation = 0;
  std::uint64_t includeDiscoveryGeneration = 0;
  std::unordered_map<std::string, std::string> sourceOverrides;
  std::unordered_map<std::string, std::uint64_t> documentGenerations;
  lang::lsp::FrontendSourceSnapshot frontendSources;
};

struct CompletionRequest {
  JsonValue id{nullptr};
  std::string idKey;
  std::string uri;
  std::filesystem::path entryPath;
  std::string source;
  std::uint64_t generation = 0;
  std::size_t byteOffset = 0;
  std::unordered_map<std::string, std::string> sourceOverrides;
  lang::lsp::FrontendSourceSnapshot frontendSources;
  bool snippetSupport = false;
  bool markdownDocumentation = false;
};

enum class SemanticRequestKind {
  Tokens,
  Hover,
  Definition,
  References,
  Highlights,
  DocumentSymbols,
  PrepareRename,
  Rename,
  SignatureHelp,
};

struct PendingSemanticRequest {
  JsonValue id{nullptr};
  JsonValue params{nullptr};
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
  lang::FrontendWorkMetrics workMetrics;
  std::uint64_t frontendElapsedMicroseconds = 0;
  std::unordered_map<std::string, lang::lsp::FrontendSourceIdentity>
      sourceIdentities;
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

// Immutable driver-owned inputs for one editor analysis. Project resolution is
// deliberately read-only: it never fetches, builds, or mutates the workspace.
// A discovery miss preserves standalone-file behavior; every other project
// failure is retained as a structured diagnostic while frontend recovery
// continues without inventing a partial package graph.
struct ProjectAnalysisInputs {
  lang::driver::AnalysisConfiguration configuration;
  std::vector<lang::PackageSourceRoot> packageSourceRoots;
  lang::WarningPolicy warnings;
  lang::driver::PackageWarningPolicy packageWarnings;
  lang::SourceManager sources;
  std::vector<lang::Diagnostic> diagnostics;
  bool requireEntryPoint = false;
  std::vector<lang::IncludeSearchRoot> includeSearchRoots;
};

ProjectAnalysisInputs resolveProjectAnalysisInputs(
    const std::filesystem::path &entryPath,
    std::optional<lang::ExecutionProfile> executionProfileOverride) {
  const lang::TargetInfo host = lang::TargetInfo::host();
  ProjectAnalysisInputs result{
      .configuration = lang::driver::AnalysisConfiguration(
          host,
          executionProfileOverride.value_or(
              lang::ExecutionProfile::SingleThreaded),
          lang::driver::RuntimeProfile::Hosted)};

  lang::driver::ProjectFactsResult project =
      lang::driver::resolveProjectFacts(entryPath.parent_path());
  if (!project.succeeded()) {
    if (project.status != lang::driver::ProjectFactsStatus::DiscoveryFailure) {
      result.sources = std::move(project.sources);
      result.diagnostics = std::move(project.diagnostics);
    } else {
      result.includeSearchRoots.push_back(
          {entryPath.parent_path(), std::nullopt, {}});
    }
    return result;
  }

  const lang::driver::ProjectFacts &facts = *project.facts;
  const std::optional<lang::driver::AnalysisConfiguration>
      projectConfiguration =
          facts.analysisConfiguration(facts.defaultProfileName(), host,
                                      lang::driver::RuntimeProfile::Hosted);
  if (projectConfiguration) {
    result.configuration = *projectConfiguration;
  } else {
    result.configuration = lang::driver::AnalysisConfiguration(
        host, lang::ExecutionProfile::SingleThreaded,
        lang::driver::RuntimeProfile::Hosted, facts.configurationFlags());
  }
  if (executionProfileOverride) {
    result.configuration = lang::driver::AnalysisConfiguration(
        result.configuration.target(), *executionProfileOverride,
        lang::driver::RuntimeProfile::Hosted,
        result.configuration.configurationFlags());
  }
  result.packageSourceRoots = facts.packageSourceRoots();
  if (const auto *profile = facts.findProfile(facts.defaultProfileName())) {
    result.warnings = profile->warningPolicy;
    result.packageWarnings = facts.packageWarnings(profile->name);
  }
  result.includeSearchRoots = lang::driver::projectIncludeSearchRoots(facts);
  for (const auto &target : facts.targets()) {
    if (canonicalPath(target.root) == canonicalPath(entryPath)) {
      result.requireEntryPoint = true;
      break;
    }
  }
  return result;
}

class LanguageServer {
public:
  explicit LanguageServer(lang::StandardLibraryLayout standardLibrary,
                          std::optional<lang::ExecutionProfile>
                              executionProfileOverride = std::nullopt)
      : standardLibrary(std::move(standardLibrary)),
        executionProfileOverride(executionProfileOverride),
        frontendTracePath(frontendTracePathFromEnvironment()),
        analysisWorker(std::optional<unsigned>{frontendWorkerStackSize},
                       [this] { runAnalysisWorker(); }),
        completionWorker(std::optional<unsigned>{frontendWorkerStackSize},
                         [this] { runCompletionWorker(); }) {}

  ~LanguageServer() {
    stopCompletionWorker();
    stopAnalysisWorker();
  }

  int run() {
    while (const std::optional<std::string> payload = readMessage()) {
      llvm::Expected<JsonValue> message = llvm::json::parse(*payload);
      if (!message) {
        llvm::consumeError(message.takeError());
        sendJson(errorResponse(nullptr, -32700, "Parse error"));
        continue;
      }

      if (message->getAsObject() == nullptr) {
        sendJson(errorResponse(nullptr, -32600, "Invalid Request"));
        continue;
      }

      try {
        if (handle(&*message)) {
          return shutdownRequested ? 0 : 1;
        }
      } catch (const std::exception &error) {
        std::cerr << "LSP request failed: " << error.what() << '\n';
        sendJson(
            errorResponse(member(&*message, "id"), -32603, "Internal error"));
      } catch (...) {
        std::cerr << "LSP request failed with an unknown exception\n";
        sendJson(
            errorResponse(member(&*message, "id"), -32603, "Internal error"));
      }
    }
    stopCompletionWorker();
    stopAnalysisWorker();
    return 0;
  }

private:
  bool handle(const JsonValue *message) {
    const std::string method = stringMember(message, "method");
    const JsonValue *id = member(message, "id");
    const JsonValue *params = member(message, "params");

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
    } else if (method == "textDocument/references") {
      semanticFeature(id, params, SemanticRequestKind::References);
    } else if (method == "textDocument/documentHighlight") {
      semanticFeature(id, params, SemanticRequestKind::Highlights);
    } else if (method == "textDocument/documentSymbol") {
      semanticFeature(id, params, SemanticRequestKind::DocumentSymbols);
    } else if (method == "textDocument/prepareRename") {
      semanticFeature(id, params, SemanticRequestKind::PrepareRename);
    } else if (method == "textDocument/rename") {
      semanticFeature(id, params, SemanticRequestKind::Rename);
    } else if (method == "textDocument/signatureHelp") {
      semanticFeature(id, params, SemanticRequestKind::SignatureHelp);
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

  JsonValue initializeResult(const JsonValue *params) {
    markdownHover = supportsHoverFormat(params, "markdown");
    markdownCompletionDocumentation =
        supportsCompletionDocumentationMarkdown(params);
    markdownSignatureDocumentation =
        supportsSignatureDocumentationMarkdown(params);
    completionSnippets = supportsCompletionSnippets(params);
    semanticTokenRefreshSupport = supportsSemanticTokenRefresh(params);
    workspaceDocumentChanges = supportsWorkspaceDocumentChanges(params);
    watchedFilesDynamicRegistration =
        supportsWatchedFilesDynamicRegistration(params);
    diagnosticRelatedInformation = supportsDiagnosticRelatedInformation(params);
    diagnosticData = supportsDiagnosticData(params);
    diagnosticUnnecessaryTag =
        supportsDiagnosticTag(params, lang::DiagnosticTag::Unnecessary);
    diagnosticDeprecatedTag =
        supportsDiagnosticTag(params, lang::DiagnosticTag::Deprecated);
    codeActionLiterals = supportsCodeActionLiterals(params);
    preferredCodeActions = supportsPreferredCodeActions(params);
    hierarchicalDocumentSymbols = supportsHierarchicalDocumentSymbols(params);
    JsonObject sync{{"openClose", true}, {"change", 1}, {"save", true}};

    JsonArray tokenTypes;
    for (const char *type :
         {"keyword", "type", "typeParameter", "namespace", "class", "function",
          "method", "variable", "parameter", "property", "string", "number",
          "operator", "macro", "decorator", "comment", "enumMember", "struct",
          "enum"}) {
      tokenTypes.push_back(type);
    }
    JsonArray tokenModifiers;
    for (const char *modifier :
         {"declaration", "definition", "readonly", "defaultLibrary",
          "functionScope", "static", "inactiveCode"}) {
      tokenModifiers.push_back(modifier);
    }
    JsonObject legend{{"tokenTypes", std::move(tokenTypes)},
                      {"tokenModifiers", std::move(tokenModifiers)}};

    JsonObject semanticTokens{{"legend", std::move(legend)}, {"full", true}};

    JsonObject capabilities{
        {"positionEncoding", "utf-16"},
        {"textDocumentSync", std::move(sync)},
        {"semanticTokensProvider", std::move(semanticTokens)},
        {"documentFormattingProvider", true},
        {"hoverProvider", true},
        {"definitionProvider", true},
        {"referencesProvider", true},
        {"documentHighlightProvider", true},
        {"documentSymbolProvider", true},
        {"signatureHelpProvider",
         JsonObject{{"triggerCharacters", JsonArray{"(", ","}},
                    {"retriggerCharacters", JsonArray{","}}}},
        {"renameProvider", JsonObject{{"prepareProvider", true}}}};
    if (codeActionLiterals) {
      capabilities["codeActionProvider"] =
          JsonObject{{"codeActionKinds", JsonArray{"quickfix"}},
                     {"resolveProvider", false}};
    }
    JsonArray triggers;
    for (const char *trigger : {".", ">", ":"}) {
      triggers.push_back(trigger);
    }
    capabilities["completionProvider"] = JsonObject{
        {"triggerCharacters", std::move(triggers)}, {"resolveProvider", false}};

    return JsonObject{{"capabilities", std::move(capabilities)},
                      {"serverInfo", JsonObject{{"name", "gti_lsp"},
                                                {"version", GTI_VERSION}}}};
  }

  // Must run under stateMutex. A generation change makes every older row
  // unreachable and erases the learned digest. Obsolete rows remain subject
  // to the cache's global LRU instead of destroying ASTs on the protocol
  // thread while the editor is sending a change.
  void invalidateFrontendSourceLocked(std::string_view canonicalPath) {
    ++includeDiscoveryGeneration;
    frontendSnapshots.invalidate(canonicalPath);
  }

  void didOpen(const JsonValue *params) {
    const JsonValue *document = member(params, "textDocument");
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
      if (const std::optional<std::filesystem::path> path =
              filePathFromUri(uri)) {
        const std::string canonical = canonicalPath(*path).string();
        invalidateFrontendSourceLocked(canonical);
      }
      const std::optional<std::int64_t> version =
          integerMember(document, "version");
      if (version) {
        documentVersions[uri] = *version;
      }
      semanticTokenCache.erase(uri);
      // Generation checks make the prior snapshot inaccessible to requests.
      // Retaining it until replacement keeps its large frontend graph from
      // being destroyed synchronously on the protocol thread.
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

  void didChange(const JsonValue *params) {
    const std::string clientUri =
        stringMember(member(params, "textDocument"), "uri");
    const JsonValue *changes = member(params, "contentChanges");
    const JsonArray *contentChanges =
        changes != nullptr ? changes->getAsArray() : nullptr;
    if (clientUri.empty() || contentChanges == nullptr) {
      return;
    }
    const std::string uri = documentKeyFromUri(clientUri);

    if (!contentChanges->empty()) {
      const std::string source = stringMember(&contentChanges->back(), "text");
      const std::optional<std::int64_t> version =
          integerMember(member(params, "textDocument"), "version");
      std::vector<AnalysisRequest> requests;
      std::vector<DiagnosticPublication> publications;
      {
        const std::lock_guard lock(stateMutex);
        clientUris[uri] = clientUri;
        openClientUris[uri].insert(clientUri);
        documents[uri] = source;
        if (const std::optional<std::filesystem::path> path =
                filePathFromUri(uri)) {
          const std::string canonical = canonicalPath(*path).string();
          invalidateFrontendSourceLocked(canonical);
        }
        if (version) {
          documentVersions[uri] = *version;
        }
        semanticTokenCache.erase(uri);
        // Keep the stale snapshot alive until the worker publishes its
        // replacement; all readers require an exact generation match.
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

  void didSave(const JsonValue *) {
    // Full synchronization already schedules analysis for every changed
    // version. Repeating it here blocks later requests and republishes the
    // same diagnostics during format-on-save.
  }

  void didClose(const JsonValue *params) {
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
      if (const std::optional<std::filesystem::path> path =
              filePathFromUri(uri)) {
        const std::string canonical = canonicalPath(*path).string();
        invalidateFrontendSourceLocked(canonical);
      }
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

  void didChangeWatchedFiles(const JsonValue *params) {
    const JsonValue *changes = member(params, "changes");
    const JsonArray *changedFiles =
        changes != nullptr ? changes->getAsArray() : nullptr;
    if (changedFiles == nullptr) {
      return;
    }

    std::vector<AnalysisRequest> requests;
    std::vector<DiagnosticPublication> publications;
    {
      const std::lock_guard lock(stateMutex);
      std::unordered_set<std::string> affected;
      bool projectModelChanged = false;
      for (const JsonValue &change : *changedFiles) {
        const std::string clientUri = stringMember(&change, "uri");
        if (clientUri.empty()) {
          continue;
        }
        const std::string uri = documentKeyFromUri(clientUri);
        const std::optional<std::filesystem::path> changedPath =
            filePathFromUri(clientUri);
        if (changedPath && (changedPath->filename() == "gti.toml" ||
                            changedPath->filename() == "gti.lock")) {
          projectModelChanged = true;
          continue;
        }
        // Dirty open buffers remain authoritative over filesystem events.
        if (documents.contains(uri)) {
          continue;
        }
        if (changedPath) {
          const std::string canonical = canonicalPath(*changedPath).string();
          invalidateFrontendSourceLocked(canonical);
        }
        affected.insert(uri);
        invalidateDependentsLocked(uri, affected, requests);
      }
      if (projectModelChanged) {
        requests.clear();
        for (const auto &[documentUri, documentSource] : documents) {
          (void)documentSource;
          affected.insert(documentUri);
          semanticTokenCache.erase(documentUri);
          const auto version = documentVersions.find(documentUri);
          requests.push_back(makeAnalysisRequestLocked(
              documentUri,
              version == documentVersions.end()
                  ? std::nullopt
                  : std::optional<std::int64_t>(version->second),
              ++analysisGenerations[documentUri]));
        }
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

    JsonArray watchers;
    watchers.push_back(JsonObject{{"globPattern", "**/*.gti"}});
    watchers.push_back(JsonObject{{"globPattern", "**/gti.toml"}});
    watchers.push_back(JsonObject{{"globPattern", "**/gti.lock"}});
    JsonArray registrations;
    registrations.push_back(JsonObject{
        {"id", "gti-source-watcher"},
        {"method", "workspace/didChangeWatchedFiles"},
        {"registerOptions", JsonObject{{"watchers", std::move(watchers)}}}});
    sendJson(JsonObject{
        {"jsonrpc", "2.0"},
        {"id", nextServerRequestId.fetch_add(1)},
        {"method", "client/registerCapability"},
        {"params", JsonObject{{"registrations", std::move(registrations)}}}});
  }

  void semanticTokens(const JsonValue *id, const JsonValue *params) {
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
            {.id = *id,
             .params =
                 params != nullptr ? JsonValue(*params) : JsonValue(nullptr),
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
    const lang::FrontendResult *frontend = nullptr;
    if (hasCurrentSnapshot) {
      sourceUnit =
          snapshot.frontend->sourceGraph.sourceUnitForPath(snapshot.rootPath);
      frontend = snapshot.frontend.get();
    }
    std::vector<SemanticToken> tokens =
        cached ? std::move(*cached)
               : collectSemanticTokens(source, frontend, sourceUnit);
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

  void respondHover(const JsonValue *id, Position position, std::string source,
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

    sendJson(response(
        id, JsonObject{
                {"contents",
                 JsonObject{{"kind", markdownHover ? "markdown" : "plaintext"},
                            {"value", value}}},
                {"range", rangeJson(source, info->range.start,
                                    info->range.end - info->range.start)}}));
  }

  // Shared snapshot acquisition for semantic requests: answer from the
  // current-generation snapshot, or queue the request until analysis
  // publishes one for the document's current generation.
  struct SnapshotAccess {
    std::string source;
    AnalysisSnapshot snapshot;
    bool current = false;
    bool queued = false;
  };

  SnapshotAccess acquireSnapshotOrQueue(const JsonValue *id,
                                        const JsonValue *params,
                                        const std::string &uri,
                                        SemanticRequestKind kind) {
    SnapshotAccess access;
    const std::lock_guard lock(stateMutex);
    const auto document = documents.find(uri);
    const auto generation = analysisGenerations.find(uri);
    const auto snapshot = analysisSnapshots.find(uri);
    if (document != documents.end() &&
        generation != analysisGenerations.end() &&
        snapshot != analysisSnapshots.end() &&
        snapshot->second.generation == generation->second &&
        snapshot->second.frontend != nullptr) {
      access.source = document->second;
      access.snapshot = snapshot->second;
      access.current = true;
    } else if (id != nullptr && document != documents.end() &&
               generation != analysisGenerations.end()) {
      pendingSemanticRequests.push_back({.id = *id,
                                         .params = params != nullptr
                                                       ? JsonValue(*params)
                                                       : JsonValue(nullptr),
                                         .idKey = requestIdKey(id),
                                         .uri = uri,
                                         .generation = generation->second,
                                         .kind = kind});
      access.queued = true;
    }
    return access;
  }

  void hover(const JsonValue *id, const JsonValue *params) {
    const std::string uri =
        documentKeyFromUri(stringMember(member(params, "textDocument"), "uri"));
    const std::optional<Position> position =
        positionMember(member(params, "position"));
    if (!position) {
      sendJson(response(id, nullptr));
      return;
    }
    const SnapshotAccess access =
        acquireSnapshotOrQueue(id, params, uri, SemanticRequestKind::Hover);
    if (!access.current) {
      if (!access.queued) {
        sendJson(response(id, nullptr));
      }
      return;
    }

    respondHover(id, *position, access.source, access.snapshot);
  }

  void respondDefinition(const JsonValue *id, std::string_view uri,
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
    std::string targetUri =
        uriForSource(info->target.source, snapshot.rootPath, uri);
    {
      const std::lock_guard lock(stateMutex);
      if (const auto preferred = clientUris.find(targetUri);
          preferred != clientUris.end()) {
        targetUri = preferred->second;
      }
    }
    sendJson(response(
        id, JsonObject{
                {"uri", targetUri},
                {"range", rangeJson(targetSource, info->target.start,
                                    info->target.end - info->target.start)}}));
  }

  void definition(const JsonValue *id, const JsonValue *params) {
    const std::string uri =
        documentKeyFromUri(stringMember(member(params, "textDocument"), "uri"));
    const std::optional<Position> position =
        positionMember(member(params, "position"));
    if (!position) {
      sendJson(response(id, nullptr));
      return;
    }

    const SnapshotAccess access = acquireSnapshotOrQueue(
        id, params, uri, SemanticRequestKind::Definition);
    if (!access.current) {
      if (!access.queued) {
        sendJson(response(id, nullptr));
      }
      return;
    }

    respondDefinition(id, uri, *position, access.source, access.snapshot);
  }

  void semanticFeature(const JsonValue *id, const JsonValue *params,
                       SemanticRequestKind kind) {
    const std::string uri =
        documentKeyFromUri(stringMember(member(params, "textDocument"), "uri"));
    const SnapshotAccess access = acquireSnapshotOrQueue(id, params, uri, kind);
    if (!access.current) {
      if (!access.queued) {
        sendJson(response(id, nullptr));
      }
      return;
    }
    respondSemanticFeature(kind, id, params, uri, access.source,
                           access.snapshot);
  }

  void respondSemanticFeature(SemanticRequestKind kind, const JsonValue *id,
                              const JsonValue *params, const std::string &uri,
                              const std::string &source,
                              const AnalysisSnapshot &snapshot) {
    if (kind == SemanticRequestKind::DocumentSymbols) {
      respondDocumentSymbols(id, uri, source, snapshot);
      return;
    }
    const std::optional<Position> position =
        positionMember(member(params, "position"));
    if (!position) {
      sendJson(response(id, nullptr));
      return;
    }
    switch (kind) {
    case SemanticRequestKind::References:
      respondReferences(
          id, uri, *position, source, snapshot,
          boolMember(member(params, "context"), "includeDeclaration", false));
      break;
    case SemanticRequestKind::Highlights:
      respondHighlights(id, *position, source, snapshot);
      break;
    case SemanticRequestKind::PrepareRename:
      respondPrepareRename(id, *position, source, snapshot);
      break;
    case SemanticRequestKind::Rename:
      respondRename(id, uri, *position, source, snapshot,
                    stringMember(params, "newName"));
      break;
    case SemanticRequestKind::SignatureHelp:
      respondSignatureHelp(id, *position, source, snapshot);
      break;
    default:
      sendJson(response(id, nullptr));
      break;
    }
  }

  void respondSignatureHelp(const JsonValue *id, Position position,
                            const std::string &source,
                            const AnalysisSnapshot &snapshot) {
    const std::optional<std::size_t> byteOffset =
        SourcePositionIndex(source).byteOffset(position);
    if (!byteOffset) {
      sendJson(response(id, nullptr));
      return;
    }
    const lang::SourceUnitId sourceUnit =
        snapshot.frontend->sourceGraph.sourceUnitForPath(snapshot.rootPath);
    const std::optional<lang::SignatureHelpInfo> info =
        lang::LanguageQueries().signatureHelp(*snapshot.frontend, sourceUnit,
                                              *byteOffset);
    if (!info) {
      sendJson(response(id, nullptr));
      return;
    }
    JsonArray parameters;
    parameters.reserve(info->parameterLabels.size());
    for (const std::string &label : info->parameterLabels) {
      parameters.push_back(JsonObject{{"label", label}});
    }
    JsonObject signature{{"label", info->label},
                         {"parameters", std::move(parameters)}};
    if (info->documentationMarkdown) {
      signature["documentation"] = documentationJson(
          *info->documentationMarkdown, markdownSignatureDocumentation);
    }
    JsonArray signatures;
    signatures.push_back(std::move(signature));
    sendJson(response(
        id, JsonObject{{"signatures", std::move(signatures)},
                       {"activeSignature", 0},
                       {"activeParameter",
                        static_cast<std::int64_t>(info->activeParameter)}}));
  }

  // Converts a compiler span into an LSP location, preferring the client's
  // own spelling of the URI for open documents.
  std::optional<JsonValue> locationJson(const lang::SourceSpan &span,
                                        const AnalysisSnapshot &snapshot,
                                        std::string_view rootUri,
                                        const std::string &rootSource) {
    const std::string targetSource = sourceForSpan(
        span, snapshot.frontend->sources, snapshot.rootPath, rootSource);
    if (targetSource.empty()) {
      return std::nullopt;
    }
    std::string targetUri =
        uriForSource(span.source, snapshot.rootPath, rootUri);
    {
      const std::lock_guard lock(stateMutex);
      if (const auto preferred = clientUris.find(targetUri);
          preferred != clientUris.end()) {
        targetUri = preferred->second;
      }
    }
    return JsonValue(JsonObject{
        {"uri", targetUri},
        {"range", rangeJson(targetSource, span.start, span.end - span.start)}});
  }

  void respondReferences(const JsonValue *id, std::string_view uri,
                         Position position, const std::string &source,
                         const AnalysisSnapshot &snapshot,
                         bool includeDeclaration) {
    const std::optional<std::size_t> byteOffset =
        SourcePositionIndex(source).byteOffset(position);
    if (!byteOffset) {
      sendJson(response(id, nullptr));
      return;
    }
    const lang::SourceUnitId sourceUnit =
        snapshot.frontend->sourceGraph.sourceUnitForPath(snapshot.rootPath);
    const std::optional<lang::ReferencesInfo> info =
        lang::LanguageQueries().references(*snapshot.frontend, sourceUnit,
                                           *byteOffset, includeDeclaration);
    if (!info) {
      sendJson(response(id, nullptr));
      return;
    }
    JsonArray locations;
    for (const lang::ReferenceSite &site : info->sites) {
      if (std::optional<JsonValue> location =
              locationJson(site.span, snapshot, uri, source)) {
        locations.push_back(std::move(*location));
      }
    }
    sendJson(response(id, JsonValue(std::move(locations))));
  }

  void respondHighlights(const JsonValue *id, Position position,
                         const std::string &source,
                         const AnalysisSnapshot &snapshot) {
    const std::optional<std::size_t> byteOffset =
        SourcePositionIndex(source).byteOffset(position);
    if (!byteOffset) {
      sendJson(response(id, nullptr));
      return;
    }
    const lang::SourceUnitId sourceUnit =
        snapshot.frontend->sourceGraph.sourceUnitForPath(snapshot.rootPath);
    const std::optional<lang::ReferencesInfo> info =
        lang::LanguageQueries().references(*snapshot.frontend, sourceUnit,
                                           *byteOffset, true);
    if (!info) {
      sendJson(response(id, nullptr));
      return;
    }
    JsonArray highlights;
    for (const lang::ReferenceSite &site : info->sites) {
      if (!site.span.source.empty() && site.span.source != snapshot.rootPath) {
        continue;
      }
      highlights.push_back(JsonObject{
          {"range",
           rangeJson(source, site.span.start, site.span.end - site.span.start)},
          {"kind",
           lang::hasRole(site.roles, lang::OccurrenceRole::Write) ? 3 : 2}});
    }
    sendJson(response(id, JsonValue(std::move(highlights))));
  }

  void respondPrepareRename(const JsonValue *id, Position position,
                            const std::string &source,
                            const AnalysisSnapshot &snapshot) {
    const std::optional<std::size_t> byteOffset =
        SourcePositionIndex(source).byteOffset(position);
    if (!byteOffset) {
      sendJson(response(id, nullptr));
      return;
    }
    const lang::SourceUnitId sourceUnit =
        snapshot.frontend->sourceGraph.sourceUnitForPath(snapshot.rootPath);
    const std::optional<lang::RenamePreparation> preparation =
        lang::LanguageQueries().prepareRename(*snapshot.frontend, sourceUnit,
                                              *byteOffset);
    if (!preparation) {
      sendJson(response(id, nullptr));
      return;
    }
    sendJson(response(
        id, JsonObject{{"range", rangeJson(source, preparation->origin.start,
                                           preparation->origin.end -
                                               preparation->origin.start)},
                       {"placeholder", preparation->placeholder}}));
  }

  void respondRename(const JsonValue *id, std::string_view uri,
                     Position position, const std::string &source,
                     const AnalysisSnapshot &snapshot,
                     const std::string &newName) {
    const std::optional<std::size_t> byteOffset =
        SourcePositionIndex(source).byteOffset(position);
    if (!byteOffset) {
      sendJson(errorResponse(id, -32602, "Invalid position"));
      return;
    }
    const lang::SourceUnitId sourceUnit =
        snapshot.frontend->sourceGraph.sourceUnitForPath(snapshot.rootPath);
    const lang::RenameOutcome outcome = lang::LanguageQueries().rename(
        *snapshot.frontend, sourceUnit, *byteOffset, newName);
    if (!outcome.edits) {
      sendJson(errorResponse(id, -32803,
                             outcome.failure.empty() ? "Rename failed."
                                                     : outcome.failure));
      return;
    }

    struct FileEdits {
      std::string clientUri;
      std::optional<std::int64_t> version;
      JsonArray edits;
    };
    std::vector<std::string> order;
    std::unordered_map<std::string, FileEdits> byUri;
    for (const lang::SourceSpan &span : outcome.edits->spans) {
      const std::string targetSource = sourceForSpan(
          span, snapshot.frontend->sources, snapshot.rootPath, source);
      if (targetSource.empty()) {
        sendJson(errorResponse(id, -32803,
                               "Rename coverage is incomplete in this "
                               "snapshot."));
        return;
      }
      const std::string targetKey =
          uriForSource(span.source, snapshot.rootPath, uri);
      auto entry = byUri.find(targetKey);
      if (entry == byUri.end()) {
        entry = byUri.emplace(targetKey, FileEdits{}).first;
        order.push_back(targetKey);
      }
      entry->second.edits.push_back(JsonObject{
          {"range", rangeJson(targetSource, span.start, span.end - span.start)},
          {"newText", newName}});
    }
    {
      const std::lock_guard lock(stateMutex);
      for (const std::string &key : order) {
        FileEdits &entry = byUri.at(key);
        entry.clientUri = clientUriForKeyLocked(key);
        if (const auto version = documentVersions.find(key);
            version != documentVersions.end()) {
          entry.version = version->second;
        }
      }
    }

    if (workspaceDocumentChanges) {
      JsonArray documentChanges;
      for (const std::string &key : order) {
        FileEdits &entry = byUri.at(key);
        JsonObject textDocument{{"uri", entry.clientUri}};
        textDocument["version"] =
            entry.version ? JsonValue(*entry.version) : JsonValue(nullptr);
        documentChanges.push_back(
            JsonObject{{"textDocument", std::move(textDocument)},
                       {"edits", std::move(entry.edits)}});
      }
      sendJson(response(
          id, JsonObject{{"documentChanges", std::move(documentChanges)}}));
      return;
    }
    JsonObject changes;
    for (const std::string &key : order) {
      changes[byUri.at(key).clientUri] = std::move(byUri.at(key).edits);
    }
    sendJson(response(id, JsonObject{{"changes", std::move(changes)}}));
  }

  void respondDocumentSymbols(const JsonValue *id, const std::string &uri,
                              const std::string &source,
                              const AnalysisSnapshot &snapshot) {
    const lang::SourceUnitId sourceUnit =
        snapshot.frontend->sourceGraph.sourceUnitForPath(snapshot.rootPath);
    const std::vector<lang::DocumentSymbolInfo> symbols =
        lang::LanguageQueries().documentSymbols(*snapshot.frontend, sourceUnit);
    if (hierarchicalDocumentSymbols) {
      sendJson(response(id, JsonValue(documentSymbolArray(symbols, source))));
      return;
    }
    std::string clientUri;
    {
      const std::lock_guard lock(stateMutex);
      clientUri = clientUriForKeyLocked(uri);
    }
    JsonArray flat;
    flattenDocumentSymbols(symbols, {}, clientUri, source, flat);
    sendJson(response(id, JsonValue(std::move(flat))));
  }

  void completion(const JsonValue *id, const JsonValue *params) {
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
        request.markdownDocumentation = markdownCompletionDocumentation;
        request.frontendSources = frontendSnapshots.capture();
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
          response(id, completionListJson({}, source, completionSnippets,
                                          markdownCompletionDocumentation)));
      return;
    }

    const std::optional<std::filesystem::path> filePath = filePathFromUri(uri);
    const std::optional<std::size_t> byteOffset =
        SourcePositionIndex(source).byteOffset(*position);
    if (!filePath || !byteOffset) {
      sendJson(
          response(id, completionListJson({}, source, completionSnippets,
                                          markdownCompletionDocumentation)));
      return;
    }
    request.id = *id;
    request.idKey = requestIdKey(id);
    request.entryPath = *filePath;
    request.byteOffset = *byteOffset;
    scheduleCompletion(std::move(request));
  }

  void codeActions(const JsonValue *id, const JsonValue *params) {
    if (id == nullptr) {
      return;
    }
    JsonArray actions;
    const std::string clientUri =
        stringMember(member(params, "textDocument"), "uri");
    const std::string uri = documentKeyFromUri(clientUri);
    const std::optional<LspRange> requestedRange =
        rangeMember(member(params, "range"));
    const JsonValue *context = member(params, "context");
    const JsonValue *requestedDiagnostics = member(context, "diagnostics");
    if (uri.empty() || !codeActionLiterals || !requestedRange ||
        !validRange(*requestedRange) || !contextAllowsQuickFix(context)) {
      sendJson(response(id, JsonValue(std::move(actions))));
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
                !diagnosticRequested(diagnostic, requestedDiagnostics,
                                     codeActionLiterals)) {
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
                   .preferred = fix.fix.preferred});
            }
          }
        }
      }
    }

    for (const CodeActionCandidate &candidate : candidates) {
      actions.push_back(codeActionJson(
          candidate, workspaceDocumentChanges, diagnosticRelatedInformation,
          diagnosticData, preferredCodeActions, diagnosticUnnecessaryTag,
          diagnosticDeprecatedTag));
    }
    sendJson(response(id, JsonValue(std::move(actions))));
  }

  void documentFormatting(const JsonValue *id, const JsonValue *params) {
    JsonArray edits;
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
      sendJson(response(id, JsonValue(std::move(edits))));
      return;
    }

    const JsonValue *formatOptions = member(params, "options");
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
      sendJson(response(id, JsonValue(std::move(edits))));
      return;
    }

    edits.push_back(JsonObject{{"range", rangeJson(source, 0, source.size())},
                               {"newText", formatted}});
    sendJson(response(id, JsonValue(std::move(edits))));
  }

  DocumentAnalysis analyzeDocument(const AnalysisRequest &request,
                                   const ProjectAnalysisInputs &project) const {
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
    frontendOptions.requireEntryPoint = project.requireEntryPoint;
    // HIR performs the compiler's concrete generic-instance reanalysis. The
    // LSP retains it only so editor diagnostics agree with `gti check`; MIR
    // remains code-generation work and is not built for editor snapshots.
    frontendOptions.stopAfter = lang::FrontendPhase::Hir;
    frontendOptions.workMetrics = &result.workMetrics;
    frontendOptions.cache = request.frontendSources.cache;
    frontendOptions.sourceGenerations = request.frontendSources.generations;
    frontendOptions.sourceDigests = request.frontendSources.digests;
    frontendOptions.target = project.configuration.target();
    frontendOptions.configurationFlags =
        project.configuration.configurationFlags();
    frontendOptions.includeSearchRoots = project.includeSearchRoots;
    for (const lang::Diagnostic &diagnostic : project.diagnostics) {
      result.diagnostics.push_back(convertDiagnostic(
          diagnostic, project.sources, rootPath, uri, source));
    }
    const SteadyClock::time_point frontendStart = SteadyClock::now();
    result.frontend = std::make_shared<const lang::FrontendResult>(
        lang::Frontend(frontendOptions)
            .analyze(*filePath, source, {standardLibrary.prelude},
                     request.sourceOverrides, {standardLibrary.root},
                     project.packageSourceRoots));
    result.frontendElapsedMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            SteadyClock::now() - frontendStart)
            .count());
    const lang::FrontendResult &analysis = *result.frontend;
    for (const lang::SourceUnit &unit : analysis.sourceGraph.sourceUnits()) {
      const std::string loadedSource = unit.path.string();
      const auto generation =
          request.frontendSources.generations.find(loadedSource);
      result.sourceIdentities.emplace(
          loadedSource,
          lang::lsp::FrontendSourceIdentity{
              .generation =
                  generation == request.frontendSources.generations.end()
                      ? 0
                      : generation->second,
              .digest = unit.sourceDigest});
      if (unit.id != analysis.sourceGraph.entryUnit() &&
          loadedSource != rootPath) {
        result.dependencies.insert(
            fileUriFromPath(std::filesystem::path(loadedSource)));
      }
    }
    for (const lang::Diagnostic &diagnostic : analysis.diagnostics) {
      if (project.warnings.suppresses(diagnostic) ||
          project.packageWarnings.suppresses(diagnostic,
                                             analysis.sourceGraph)) {
        continue;
      }
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
    request.includeDiscoveryGeneration = includeDiscoveryGeneration;
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
    request.frontendSources = frontendSnapshots.capture();
    return request;
  }

  void invalidateDependentsLocked(const std::string &changedUri,
                                  std::unordered_set<std::string> &affected,
                                  std::vector<AnalysisRequest> &requests) {
    for (const auto &[rootUri, dependencies] : dependenciesByRoot) {
      const auto snapshot = analysisSnapshots.find(rootUri);
      const bool discovering =
          snapshot != analysisSnapshots.end() &&
          snapshot->second.frontend->includeSuggestionsRequested;
      if (rootUri == changedUri ||
          (!dependencies.contains(changedUri) && !discovering)) {
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
          positionMember(member(&request.params, "position"));
      if (!current) {
        sendJson(errorResponse(request.id, -32801, "Content modified"));
      } else if (request.kind == SemanticRequestKind::Tokens) {
        semanticTokens(&request.id, &request.params);
      } else if (request.kind == SemanticRequestKind::Hover) {
        if (!position) {
          sendJson(response(request.id, nullptr));
        } else {
          respondHover(&request.id, *position, source, snapshot);
        }
      } else if (request.kind == SemanticRequestKind::Definition) {
        if (!position) {
          sendJson(response(request.id, nullptr));
        } else {
          respondDefinition(&request.id, request.uri, *position, source,
                            snapshot);
        }
      } else {
        respondSemanticFeature(request.kind, &request.id, &request.params,
                               request.uri, source, snapshot);
      }
    }
  }

  void cancelRequest(const JsonValue *params) {
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
    }
    for (PendingSemanticRequest &request : canceledSemantic) {
      sendJson(errorResponse(request.id, -32800, "Request cancelled"));
    }
    completionCondition.notify_all();
  }

  void scheduleCompletion(CompletionRequest request) {
    std::deque<CompletionRequest> dropped;
    {
      const std::lock_guard lock(stateMutex);
      if (stoppingCompletion) {
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
        lang::FrontendWorkMetrics workMetrics;
        const SteadyClock::time_point frontendStart = SteadyClock::now();
        try {
          ProjectAnalysisInputs project = resolveProjectAnalysisInputs(
              request.entryPath, executionProfileOverride);
          completion = lang::LanguageQueries().complete(
              {.entryPath = request.entryPath,
               .source = request.source,
               .byteOffset = request.byteOffset,
               .target = project.configuration.target(),
               .configurationFlags = project.configuration.configurationFlags(),
               .preludePaths = {standardLibrary.prelude},
               .sourceOverrides = request.sourceOverrides,
               .standardLibraryRoots = {standardLibrary.root},
               .packageSourceRoots = project.packageSourceRoots,
               .workMetrics = &workMetrics,
               .cache = request.frontendSources.cache,
               .sourceGenerations = request.frontendSources.generations,
               .sourceDigests = request.frontendSources.digests});
        } catch (const std::exception &) {
          completion = {};
        }
        const std::uint64_t elapsedMicroseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                SteadyClock::now() - frontendStart)
                .count());
        recordFrontendWork("completion", request.uri, request.generation,
                           elapsedMicroseconds, workMetrics);
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
        sendJson(response(request.id,
                          completionListJson(completion, request.source,
                                             request.snippetSupport,
                                             request.markdownDocumentation)));
      }
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

      // Driver resolution runs on the worker but outside the crash guard. The
      // guard remains the narrow compiler-only boundary documented in the LSP
      // architecture, and neither step holds shared state.
      GuardedAnalysis guarded;
      try {
        ProjectAnalysisInputs project;
        if (const std::optional<std::filesystem::path> path =
                filePathFromUri(request.uri)) {
          project =
              resolveProjectAnalysisInputs(*path, executionProfileOverride);
        }
        guarded = runIsolatedAnalysis(request, project);
      } catch (const std::exception &error) {
        guarded.status = GuardedAnalysisStatus::Failed;
        guarded.failure = error.what();
      } catch (...) {
        guarded.status = GuardedAnalysisStatus::Failed;
        guarded.failure = "unknown exception during project resolution";
      }
      switch (guarded.status) {
      case GuardedAnalysisStatus::Completed:
        recordFrontendWork("analysis", request.uri, request.generation,
                           guarded.analysis->frontendElapsedMicroseconds,
                           guarded.analysis->workMetrics);
        try {
          publishAnalysis(request, std::move(*guarded.analysis));
        } catch (const std::exception &error) {
          std::cerr << "LSP publication failed: " << error.what() << '\n';
          rejectPendingSemanticGeneration(request.uri, request.generation,
                                          -32603, "Internal error");
        } catch (...) {
          std::cerr << "LSP publication failed with an unknown exception\n";
          rejectPendingSemanticGeneration(request.uri, request.generation,
                                          -32603, "Internal error");
        }
        break;
      case GuardedAnalysisStatus::Failed:
        std::cerr << "LSP analysis failed: " << guarded.failure << '\n';
        rejectPendingSemanticGeneration(request.uri, request.generation, -32603,
                                        "Internal error");
        break;
      case GuardedAnalysisStatus::Crashed:
        std::cerr << "LSP analysis crashed; the document was skipped\n";
        rejectPendingSemanticGeneration(request.uri, request.generation, -32603,
                                        "Internal error");
        break;
      }

      {
        const std::lock_guard lock(stateMutex);
        --activeAnalyses;
      }
      analysisCondition.notify_all();
    }
  }

  enum class GuardedAnalysisStatus {
    Completed,
    Failed,
    Crashed,
  };

  // Outcome of the isolated analysis step. Failures are returned as data so
  // that no exception has to travel out through LLVM's crash-recovery frame.
  struct GuardedAnalysis {
    std::unique_ptr<DocumentAnalysis> analysis;
    GuardedAnalysisStatus status = GuardedAnalysisStatus::Crashed;
    std::string failure;
  };

  // Runs only the compiler work under the crash guard. This is the narrow
  // boundary docs/architecture/lsp.md requires: the callback touches no
  // shared LSP state and holds no lock, so a signal-recovery stack restore
  // cannot leave `stateMutex` owned, and it catches its own exceptions so
  // none crosses LLVM's no-exception frame. Publication happens afterwards
  // on the normal path, where unwinding and locking behave normally.
  [[nodiscard]] GuardedAnalysis
  runIsolatedAnalysis(const AnalysisRequest &request,
                      const ProjectAnalysisInputs &project) const {
    GuardedAnalysis result;
    const bool completed = lang::runGuarded([&] {
      try {
        result.analysis = std::make_unique<DocumentAnalysis>(
            analyzeDocument(request, project));
        result.status = GuardedAnalysisStatus::Completed;
      } catch (const std::exception &error) {
        result.status = GuardedAnalysisStatus::Failed;
        result.failure = error.what();
      } catch (...) {
        result.status = GuardedAnalysisStatus::Failed;
        result.failure = "unknown exception";
      }
    });
    if (!completed) {
      result.status = GuardedAnalysisStatus::Crashed;
      // The guarded frame was abandoned, possibly mid-construction. Running
      // destructors over torn state risks a second fault, so the partially
      // built analysis is deliberately leaked instead of freed.
      (void)result.analysis.release();
    }
    return result;
  }

  void publishAnalysis(const AnalysisRequest &request,
                       DocumentAnalysis analysis) {
    std::vector<DiagnosticPublication> publications;
    std::optional<AnalysisRequest> retry;
    std::deque<PendingSemanticRequest> semanticRequests;
    std::shared_ptr<const lang::FrontendResult> retiredFrontend;
    {
      const std::lock_guard lock(stateMutex);
      const auto generation = analysisGenerations.find(request.uri);
      if (generation == analysisGenerations.end() ||
          generation->second != request.generation ||
          !documents.contains(request.uri)) {
        return;
      }

      bool dependencyChanged =
          analysis.frontend && analysis.frontend->includeSuggestionsRequested &&
          request.includeDiscoveryGeneration != includeDiscoveryGeneration;
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
        for (const auto &[path, identity] : analysis.sourceIdentities) {
          frontendSnapshots.publish(path, identity);
        }
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
        if (const auto previous = analysisSnapshots.find(request.uri);
            previous != analysisSnapshots.end()) {
          retiredFrontend = previous->second.frontend;
        }
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
    // `retiredFrontend` is intentionally released after diagnostics and
    // queued semantic replies have been sent, outside `stateMutex`.
  }

  void requestSemanticTokenRefresh() {
    if (!semanticTokenRefreshSupport) {
      return;
    }
    sendJson(JsonObject{{"jsonrpc", "2.0"},
                        {"id", nextServerRequestId.fetch_add(1)},
                        {"method", "workspace/semanticTokens/refresh"}});
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
      std::unordered_map<std::string, std::size_t> diagnosticIndexes;
      std::vector<std::string> roots;
      roots.reserve(diagnosticsByRoot.size());
      for (const auto &[root, _] : diagnosticsByRoot) {
        roots.push_back(root);
      }
      std::sort(roots.begin(), roots.end());
      for (const std::string &root : roots) {
        const std::vector<LspDiagnostic> &diagnostics =
            diagnosticsByRoot.at(root);
        for (const LspDiagnostic &diagnostic : diagnostics) {
          if (diagnostic.uri != uri) {
            continue;
          }
          const lang::SourceSpan &span = diagnostic.diagnostic.primary;
          std::string key =
              diagnostic.diagnostic.code + '\n' +
              std::to_string(static_cast<int>(diagnostic.diagnostic.phase)) +
              ':' +
              std::to_string(static_cast<int>(diagnostic.diagnostic.severity)) +
              ':' + diagnostic.diagnostic.group + '\n' +
              std::to_string(span.start) + ':' + std::to_string(span.end) +
              '\n' + diagnosticMessage(diagnostic.diagnostic);
          for (const lang::DiagnosticTag tag : diagnostic.diagnostic.tags) {
            key += "\ntag:" + std::to_string(static_cast<int>(tag));
          }
          for (const LspFixIt &fix : diagnostic.fixes) {
            key += '\n' + fix.uri + ':' + std::to_string(fix.fix.span.start) +
                   ':' + std::to_string(fix.fix.span.end) + ':' +
                   fix.fix.replacement + ':' + fix.fix.message + ':' +
                   (fix.fix.preferred ? "preferred" : "nonpreferred");
          }
          const auto [found, inserted] =
              diagnosticIndexes.emplace(key, publication.diagnostics.size());
          if (inserted) {
            LspDiagnostic published = diagnostic;
            publication.diagnostics.push_back(std::move(published));
          } else {
            mergeDiagnosticRelatedInformation(
                publication.diagnostics[found->second], diagnostic);
          }
        }
      }
      for (LspDiagnostic &diagnostic : publication.diagnostics) {
        useClientUrisLocked(diagnostic);
      }
      publications.push_back(std::move(publication));
    }
    return publications;
  }

  void publish(std::vector<DiagnosticPublication> publications) const {
    for (const DiagnosticPublication &publication : publications) {
      JsonArray diagnostics;
      diagnostics.reserve(publication.diagnostics.size());
      for (const LspDiagnostic &diagnostic : publication.diagnostics) {
        diagnostics.push_back(
            diagnosticJson(diagnostic, diagnosticRelatedInformation,
                           diagnosticData, codeActionLiterals,
                           diagnosticUnnecessaryTag, diagnosticDeprecatedTag));
      }
      publishDiagnostics(publication.uri, std::move(diagnostics),
                         publication.version);
    }
  }

  static void
  publishDiagnostics(const std::string &uri, JsonArray diagnostics,
                     std::optional<std::int64_t> version = std::nullopt) {
    JsonObject params{{"uri", uri}, {"diagnostics", std::move(diagnostics)}};
    if (version) {
      params["version"] = *version;
    }
    sendJson(JsonObject{{"jsonrpc", "2.0"},
                        {"method", "textDocument/publishDiagnostics"},
                        {"params", std::move(params)}});
  }

  void flushAnalyses() {
    std::unique_lock lock(stateMutex);
    analysisCondition.wait(lock, [this] {
      return pendingAnalyses.empty() && analysisOrder.empty() &&
             activeAnalyses == 0;
    });
  }

  void recordFrontendWork(std::string_view kind, std::string_view uri,
                          std::uint64_t generation,
                          std::uint64_t elapsedMicroseconds,
                          const lang::FrontendWorkMetrics &metrics) const {
    if (!frontendTracePath) {
      return;
    }
    JsonObject record{
        {"schema", 1},
        {"kind", std::string(kind)},
        {"uri", std::string(uri)},
        {"generation", static_cast<std::int64_t>(generation)},
        {"elapsedMicroseconds", static_cast<std::int64_t>(elapsedMicroseconds)},
        {"sourceBytes", static_cast<std::int64_t>(metrics.sourceBytes)},
        {"sourceLoadMicroseconds",
         static_cast<std::int64_t>(metrics.sourceLoadMicroseconds)},
        {"lexMicroseconds", static_cast<std::int64_t>(metrics.lexMicroseconds)},
        {"parseMicroseconds",
         static_cast<std::int64_t>(metrics.parseMicroseconds)},
        {"semanticMicroseconds",
         static_cast<std::int64_t>(metrics.semanticMicroseconds)},
        {"semanticRegistrationMicroseconds",
         static_cast<std::int64_t>(metrics.semanticRegistrationMicroseconds)},
        {"semanticBodyMicroseconds",
         static_cast<std::int64_t>(metrics.semanticBodyMicroseconds)},
        {"semanticTrustedBodyMicroseconds",
         static_cast<std::int64_t>(metrics.semanticTrustedBodyMicroseconds)},
        {"semanticApplicationBodyMicroseconds",
         static_cast<std::int64_t>(
             metrics.semanticApplicationBodyMicroseconds)},
        {"semanticFinalizationMicroseconds",
         static_cast<std::int64_t>(metrics.semanticFinalizationMicroseconds)},
        {"hirMicroseconds", static_cast<std::int64_t>(metrics.hirMicroseconds)},
        {"semanticBorrowSummaryCandidateInspections",
         static_cast<std::int64_t>(
             metrics.semanticBorrowSummaryCandidateInspections)},
        {"semanticBorrowSummaryFunctions",
         static_cast<std::int64_t>(metrics.semanticBorrowSummaryFunctions)},
        {"loadedUnits", stringArray(metrics.loadedUnits)},
        {"lexedUnits", stringArray(metrics.lexedUnits)},
        {"parsedUnits", stringArray(metrics.parsedUnits)},
        {"semanticallyAnalyzedUnits",
         stringArray(metrics.semanticallyAnalyzedUnits)},
        {"lexCacheHits", stringArray(metrics.lexCacheHits)},
        {"parseCacheHits", stringArray(metrics.parseCacheHits)},
    };
    const lang::FrontendCacheStats cacheStats =
        frontendSnapshots.cache()->stats();
    record["cache"] = JsonObject{
        {"entries", static_cast<std::int64_t>(cacheStats.entries)},
        {"estimatedBytes",
         static_cast<std::int64_t>(cacheStats.estimatedBytes)},
        {"maximumEntries",
         static_cast<std::int64_t>(cacheStats.limits.maximumEntries)},
        {"maximumEstimatedBytes",
         static_cast<std::int64_t>(cacheStats.limits.maximumEstimatedBytes)}};
    const std::string encoded =
        llvm::formatv("{0}", JsonValue(std::move(record))).str();
    const std::lock_guard lock(frontendTraceMutex);
    std::ofstream output(*frontendTracePath, std::ios::app);
    if (output) {
      output << encoded << '\n';
    }
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
    analysisCondition.notify_all();
    if (analysisWorker.joinable()) {
      analysisWorker.join();
    }
  }

  lang::StandardLibraryLayout standardLibrary;
  std::optional<lang::ExecutionProfile> executionProfileOverride;
  std::optional<std::filesystem::path> frontendTracePath;
  mutable std::mutex frontendTraceMutex;
  lang::lsp::FrontendSnapshotState frontendSnapshots;
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
  std::uint64_t includeDiscoveryGeneration = 0;
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
  bool markdownCompletionDocumentation = false;
  bool markdownSignatureDocumentation = false;
  bool completionSnippets = false;
  bool semanticTokenRefreshSupport = false;
  bool workspaceDocumentChanges = false;
  bool watchedFilesDynamicRegistration = false;
  bool diagnosticRelatedInformation = false;
  bool diagnosticData = false;
  bool diagnosticUnnecessaryTag = false;
  bool diagnosticDeprecatedTag = false;
  bool codeActionLiterals = false;
  bool preferredCodeActions = false;
  bool hierarchicalDocumentSymbols = false;
  std::atomic<std::uint64_t> nextServerRequestId{1};
  llvm::thread analysisWorker;
  llvm::thread completionWorker;
};

} // namespace

int main(int argc, char *argv[]) {
  lang::installCrashHandlers(argc > 0 ? argv[0] : "gti_lsp");
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::cout << "gti_lsp " << GTI_VERSION << '\n';
    return 0;
  }
  std::optional<lang::ExecutionProfile> executionProfile;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--execution-profile") {
      if (++index >= argc) {
        std::cerr << "gti_lsp: missing name after --execution-profile\n";
        return 64;
      }
      if (executionProfile) {
        std::cerr
            << "gti_lsp: --execution-profile may be specified only once\n";
        return 64;
      }
      executionProfile = lang::parseExecutionProfile(argv[index]);
      if (!executionProfile) {
        std::cerr << "gti_lsp: --execution-profile must be single-threaded or "
                     "concurrent\n";
        return 64;
      }
      continue;
    }
    std::cerr << "gti_lsp: unknown option '" << argument << "'\n";
    return 64;
  }
  return LanguageServer(
             lang::discoverStandardLibrary(argc > 0 ? argv[0] : "gti_lsp",
                                           GTI_BUILD_STDLIB_ROOT),
             executionProfile)
      .run();
}
