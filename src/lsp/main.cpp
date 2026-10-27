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
#include <cstring>
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
#include <system_error>
#include <thread>
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
// the compiler's structured 64-level parser diagnostic. Keep the frontend
// workers long-lived, but give every compiler-facing worker the same practical
// stack budget as an ordinary compiler process.
constexpr unsigned frontendWorkerStackSize = 8U * 1024U * 1024U;

// Every open document is its own whole-program root, so one shared-dependency
// edit queues an independent analysis per open dependant. A small pool runs
// them concurrently. Each in-flight analysis holds a complete frontend result,
// which is why the default stays well below the host's core count.
constexpr unsigned maximumDefaultAnalysisWorkers = 4;
constexpr unsigned maximumAnalysisWorkers = 16;

unsigned analysisWorkerCountFromEnvironment() {
  if (const char *configured = std::getenv("GTI_LSP_ANALYSIS_WORKERS");
      configured != nullptr && *configured != '\0') {
    const char *end = configured + std::strlen(configured);
    unsigned value = 0;
    const auto [last, error] = std::from_chars(configured, end, value);
    if (error == std::errc{} && last == end && value >= 1 &&
        value <= maximumAnalysisWorkers) {
      return value;
    }
  }
  return std::clamp(std::thread::hardware_concurrency() / 2, 1U,
                    maximumDefaultAnalysisWorkers);
}

// An edit does not start its analysis at once. The scheduler waits for a
// short, adaptive period so a burst of keystrokes collapses into one
// whole-program analysis of the final text, as clangd's update debounce does.
// The wait is the document's median recent analysis time, bounded below so a
// trivially cheap file still coalesces and above so a slow file is never
// deferred for long: a further edit cancels a running analysis of the same
// document (`AnalysisRequest::cancellation`), so starting early wastes no
// waiting. A semantic request waiting on the document ends the wait
// immediately. `GTI_LSP_UPDATE_DEBOUNCE_MS` pins a fixed wait (0 disables it)
// for benchmarks and tests.
constexpr std::chrono::milliseconds minimumUpdateDebounce{50};
constexpr std::chrono::milliseconds maximumUpdateDebounce{150};
constexpr std::size_t analysisDurationHistory = 8;

// A raw-pointer audit of a published generation starts only after the document
// has been quiet for this long with nothing interactive pending, or at once on
// save. `GTI_LSP_AUDIT_QUIET_MS` pins it for tests.
constexpr std::chrono::milliseconds defaultAuditQuietPeriod{2000};

std::chrono::milliseconds auditQuietPeriodFromEnvironment() {
  if (const char *configured = std::getenv("GTI_LSP_AUDIT_QUIET_MS");
      configured != nullptr && *configured != '\0') {
    const char *end = configured + std::strlen(configured);
    unsigned value = 0;
    const auto [last, error] = std::from_chars(configured, end, value);
    if (error == std::errc{} && last == end && value <= 600000) {
      return std::chrono::milliseconds(value);
    }
  }
  return defaultAuditQuietPeriod;
}

std::optional<std::chrono::microseconds> fixedUpdateDebounceFromEnvironment() {
  if (const char *configured = std::getenv("GTI_LSP_UPDATE_DEBOUNCE_MS");
      configured != nullptr && *configured != '\0') {
    const char *end = configured + std::strlen(configured);
    unsigned value = 0;
    const auto [last, error] = std::from_chars(configured, end, value);
    if (error == std::errc{} && last == end && value <= 10000) {
      return std::chrono::milliseconds(value);
    }
  }
  return std::nullopt;
}

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

  bool operator==(const SemanticToken &other) const {
    return position.line == other.position.line &&
           position.character == other.position.character &&
           length == other.length && type == other.type &&
           modifiers == other.modifiers;
  }
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

  // `implicit` is a modifier only before a constructor's name or `constexpr`,
  // or before a conversion operator: `operator` and its target type, possibly
  // after `virtual`.
  const auto conversionOperatorAt = [&](std::size_t at) {
    return at + 1 < tokens.size() && tokens[at].kind == OPERATOR &&
           (tokens[at + 1].kind == IDENTIFIER ||
            lang::isTypeKeywordToken(tokens[at + 1].kind));
  };
  if (token.lexeme == "implicit" && index + 2 < tokens.size() &&
      (tokens[index + 1].kind == CONSTEXPR || conversionOperatorAt(index + 1) ||
       (tokens[index + 1].kind == VIRTUAL && conversionOperatorAt(index + 2)) ||
       (tokens[index + 1].kind == IDENTIFIER &&
        (tokens[index + 2].kind == LEFT_PAREN ||
         tokens[index + 2].kind == LESS)))) {
    return SemanticClassification{Keyword, 0};
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
  case lang::SymbolKind::Macro:
    type = Macro;
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
            classifications[index]->type != Decorator &&
            classifications[index]->type != Keyword))) {
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
    if (inactive[index]) {
      if (!classifications[index]) {
        if (configurationRoles[index]) {
          // Preserve defined(...)'s dedicated syntax capture in conditions.
          continue;
        }
        // Inactive punctuation needs an overlay too; leave active punctuation
        // to the syntax highlighter rather than inventing semantic identities.
        classifications[index] = SemanticClassification{Operator, 0};
      }
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
  case lang::CompletionCandidateKind::Snippet:
    return 15;
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
    if (candidate.kind == lang::CompletionCandidateKind::Snippet &&
        !snippetSupport)
      continue;
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

// Why a document's analysis was scheduled. Edits are debounced; opening a
// document and project-wide invalidation run at once. Only an analysis the
// document did not ask for itself (a dependency or project change) can change
// the semantic tokens a client already holds without the client knowing, so
// only those publications may request a token refresh.
enum class AnalysisTrigger {
  Opened,
  DocumentChanged,
  DependencyChanged,
  ProjectChanged,
};

struct AnalysisRequest {
  std::string uri;
  std::string source;
  std::optional<std::int64_t> version;
  std::uint64_t generation = 0;
  AnalysisTrigger trigger = AnalysisTrigger::Opened;
  // Earliest start time; see `minimumUpdateDebounce`.
  SteadyClock::time_point readyAt{};
  // A raw-pointer audit of an already published generation; see
  // `runAuditWorker`. Ordinary analyses never run the audit.
  bool audit = false;
  SteadyClock::time_point scheduledAt{};
  std::uint64_t includeDiscoveryGeneration = 0;
  std::optional<std::string> projectProfile;
  std::unordered_map<std::string, std::string> sourceOverrides;
  // Content generations of every open document captured with the request.
  // Publication compares them with `contentGenerations`, never with another
  // document's analysis generation: re-analysing an unchanged dependency must
  // not invalidate the dependants that already used its current text.
  std::unordered_map<std::string, std::uint64_t> documentGenerations;
  lang::lsp::FrontendSourceSnapshot frontendSources;
  // Set when a newer edit supersedes this analysis while it runs; the
  // frontend then stops at its next safe point and nothing is published.
  std::shared_ptr<std::atomic<bool>> cancellation =
      std::make_shared<std::atomic<bool>>(false);
};

struct CompletionRequest {
  JsonValue id{nullptr};
  std::string idKey;
  std::string uri;
  std::filesystem::path entryPath;
  std::string source;
  std::uint64_t generation = 0;
  std::size_t byteOffset = 0;
  std::optional<std::string> projectProfile;
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
  MacroExpansion,
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
  // Every loaded dependency's URI and the digest of the exact text read.
  std::unordered_map<std::string, std::uint64_t> dependencies;
  std::string rootPath;
  // The selected profile (or process option) enables the raw-pointer audit,
  // so a background audit of this generation should follow.
  bool auditAvailable = false;
  std::shared_ptr<const lang::FrontendResult> frontend;
  lang::FrontendWorkMetrics workMetrics;
  std::uint64_t frontendElapsedMicroseconds = 0;
  std::unordered_map<std::string, lang::lsp::FrontendSourceIdentity>
      sourceIdentities;
};

struct AnalysisSnapshot {
  std::uint64_t generation = 0;
  std::string rootPath;
  // Released as soon as the root's generation advances; see
  // `advanceAnalysisGenerationLocked`. Every reader requires a current
  // generation and a non-null frontend.
  std::shared_ptr<const lang::FrontendResult> frontend;
  // Copied from the frontend so dependency invalidation can still read it
  // after a stale frontend has been released.
  bool includeSuggestionsRequested = false;
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
  bool configurationValid = true;
  lang::RawPointerAuditMode rawPointerAudit = lang::RawPointerAuditMode::Off;
  bool rawPointerAuditLibraryFindings = false;
  lang::driver::AnalysisConfiguration configuration;
  std::vector<lang::PackageSourceRoot> packageSourceRoots;
  lang::WarningPolicy warnings;
  lang::driver::PackageWarningPolicy packageWarnings;
  lang::SourceManager sources;
  std::vector<lang::Diagnostic> diagnostics;
  bool requireEntryPoint = false;
  lang::ProgramRequest program;
  std::vector<lang::IncludeSearchRoot> includeSearchRoots;
};

ProjectAnalysisInputs resolveProjectAnalysisInputs(
    const std::filesystem::path &entryPath,
    const std::optional<std::string> &requestedProfile,
    std::optional<lang::ExecutionProfile> executionProfileOverride,
    std::optional<lang::RawPointerAuditMode> rawPointerAuditOverride) {
  const lang::TargetInfo host = lang::TargetInfo::host();
  ProjectAnalysisInputs result{
      .rawPointerAudit =
          rawPointerAuditOverride.value_or(lang::RawPointerAuditMode::Off),
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
  const std::string &profileName =
      requestedProfile ? *requestedProfile : facts.defaultProfileName();
  const auto *profile = facts.findProfile(profileName);
  if (profile == nullptr) {
    result.configurationValid = false;
    lang::Diagnostic diagnostic = lang::makeDiagnostic(
        "GTI-B1202", lang::DiagnosticPhase::Driver,
        lang::SourceSpan{canonicalPath(entryPath).string(), 0, 1, 1},
        "Unknown build profile '" + profileName + "'.");
    std::string available;
    for (const auto &candidate : facts.profiles()) {
      if (!available.empty()) {
        available += ", ";
      }
      available += "'" + candidate.name + "'";
    }
    diagnostic.hints.push_back(
        "Select an available editor profile: " + available + ".");
    result.diagnostics.push_back(std::move(diagnostic));
    return result;
  }
  const auto *projectTarget = facts.targetForSource(entryPath);
  const std::optional<lang::driver::AnalysisConfiguration>
      projectConfiguration = facts.analysisConfiguration(
          profileName, host, lang::driver::RuntimeProfile::Hosted,
          projectTarget ? std::optional<std::string_view>(projectTarget->name)
                        : std::nullopt);
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
        result.configuration.configurationFlags(),
        result.configuration.declaredConfigurationFlags());
  }
  result.packageSourceRoots = facts.packageSourceRoots();
  result.rawPointerAudit =
      rawPointerAuditOverride.value_or(profile->rawPointerAudit);
  result.rawPointerAuditLibraryFindings =
      profile->rawPointerAuditLibraryFindings;
  result.warnings = profile->warningPolicy;
  result.packageWarnings = facts.packageWarnings(profile->name);
  result.includeSearchRoots = lang::driver::projectIncludeSearchRoots(facts);
  if (projectTarget != nullptr &&
      canonicalPath(projectTarget->root) == canonicalPath(entryPath)) {
    result.requireEntryPoint = true;
    result.program.testSuite =
        projectTarget->kind == lang::driver::ProjectTargetKind::Test;
  }
  return result;
}

class LanguageServer {
public:
  explicit LanguageServer(
      lang::StandardLibraryLayout standardLibrary,
      std::optional<lang::ExecutionProfile> executionProfileOverride =
          std::nullopt,
      std::optional<lang::RawPointerAuditMode> rawPointerAuditOverride =
          std::nullopt)
      : standardLibrary(std::move(standardLibrary)),
        executionProfileOverride(executionProfileOverride),
        rawPointerAuditOverride(rawPointerAuditOverride),
        frontendTracePath(frontendTracePathFromEnvironment()),
        completionWorker(std::optional<unsigned>{frontendWorkerStackSize},
                         [this] { runCompletionWorker(); }) {
    const unsigned workers = analysisWorkerCountFromEnvironment();
    analysisWorkers.reserve(workers);
    for (unsigned index = 0; index < workers; ++index) {
      analysisWorkers.emplace_back(
          std::optional<unsigned>{frontendWorkerStackSize},
          [this] { runAnalysisWorker(); });
    }
    // Frontend destruction recurses through deep syntax trees, so it gets
    // the same stack budget as the workers that built them.
    reclamationWorker =
        llvm::thread(std::optional<unsigned>{frontendWorkerStackSize},
                     [this] { runReclamationWorker(); });
    auditWorker = llvm::thread(std::optional<unsigned>{frontendWorkerStackSize},
                               [this] { runAuditWorker(); });
  }

  ~LanguageServer() {
    stopCompletionWorker();
    stopAnalysisWorker();
    stopReclamationWorker();
    (void)stopAuditWorker();
  }

  // Stops the workers for process exit. An audit the compiler is still running
  // cannot be interrupted and may take tens of seconds; the protocol's `exit`
  // means terminate now, so in that case the process ends without running
  // destructors rather than making the editor wait for the audit.
  [[noreturn]] void exitProcess(int code) {
    stopCompletionWorker();
    stopAnalysisWorker();
    stopReclamationWorker();
    if (stopAuditWorker()) {
      std::exit(code);
    }
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(code);
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
          exitProcess(shutdownRequested ? 0 : 1);
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
    stopReclamationWorker();
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
    } else if (method == "workspace/didChangeConfiguration") {
      didChangeConfiguration(params);
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
    } else if (method == "gti/inspectMacroExpansion") {
      semanticFeature(id, params, SemanticRequestKind::MacroExpansion);
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
    if (const JsonValue *options = member(params, "initializationOptions")) {
      updateProjectProfile(member(options, "profile"));
    }
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
        {"experimental",
         JsonObject{{"macroExpansionProvider",
                     JsonObject{{"method", "gti/inspectMacroExpansion"},
                                {"readOnly", true}}}}},
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
    {
      const std::lock_guard lock(stateMutex);
      clientUris[uri] = clientUri;
      openClientUris[uri].insert(clientUri);
      const bool alreadyOpen = documents.contains(uri);
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
      ++contentGenerations[uri];
      requests.push_back(makeAnalysisRequestLocked(
          uri, version, advanceAnalysisGenerationLocked(uri),
          AnalysisTrigger::Opened));
      // Dependants read this file from disk while it was closed. Only those
      // whose text differs from the opened overlay need analysing again.
      invalidateDependentsLocked(
          uri, requests,
          alreadyOpen ? std::nullopt
                      : std::optional<std::uint64_t>(
                            lang::sourceTextDigest(documents[uri])));
    }
    rejectPendingSemanticRequests(requests, -32801, "Content modified");
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
        ++contentGenerations[uri];
        requests.push_back(makeAnalysisRequestLocked(
            uri, version, advanceAnalysisGenerationLocked(uri),
            AnalysisTrigger::DocumentChanged));
        invalidateDependentsLocked(uri, requests);
      }
      // Nothing is published here. The client keeps the diagnostics it has
      // until a newer result replaces them, exactly as it keeps its semantic
      // tokens; publishing an edit-time subset made every diagnostic in the
      // file vanish and reappear on each keystroke.
      rejectPendingSemanticRequests(requests, -32801, "Content modified");
      scheduleAnalyses(std::move(requests));
    }
  }

  void didSave(const JsonValue *params) {
    // Full synchronization already schedules analysis for every changed
    // version. Repeating it here blocks later requests and republishes the
    // same diagnostics during format-on-save. Saving is a natural settling
    // point, though, so a waiting raw-pointer audit of the saved document
    // skips the rest of its quiet period.
    const std::string uri =
        documentKeyFromUri(stringMember(member(params, "textDocument"), "uri"));
    bool woken = false;
    {
      const std::lock_guard lock(stateMutex);
      if (const auto pending = pendingAudits.find(uri);
          pending != pendingAudits.end()) {
        pending->second.readyAt = SteadyClock::now();
        woken = true;
      }
    }
    if (woken) {
      auditCondition.notify_all();
    }
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
      if (const auto found = auditDiagnosticsByRoot.find(uri);
          found != auditDiagnosticsByRoot.end()) {
        for (const LspDiagnostic &diagnostic : found->second.diagnostics) {
          affected.insert(diagnostic.uri);
        }
        auditDiagnosticsByRoot.erase(found);
      }
      pendingAudits.erase(uri);
      documents.erase(uri);
      if (const std::optional<std::filesystem::path> path =
              filePathFromUri(uri)) {
        const std::string canonical = canonicalPath(*path).string();
        invalidateFrontendSourceLocked(canonical);
      }
      documentVersions.erase(uri);
      semanticTokenCache.erase(uri);
      advanceAnalysisGenerationLocked(uri);
      analysisSnapshots.erase(uri);
      diagnosticsByRoot.erase(uri);
      dependenciesByRoot.erase(uri);
      pendingAnalyses.erase(uri);
      ++contentGenerations[uri];
      invalidateDependentsLocked(uri, requests);
      // The closed root's diagnostics leave every file they touched now;
      // dependants keep theirs until their own re-analysis replaces them.
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
    {
      const std::lock_guard lock(stateMutex);
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
        invalidateDependentsLocked(uri, requests);
      }
      if (projectModelChanged) {
        invalidateProjectSnapshotsLocked(requests);
      }
    }
    rejectPendingSemanticRequests(requests, -32801, "Content modified");
    scheduleAnalyses(std::move(requests));
  }

  void
  invalidateProjectSnapshotsLocked(std::vector<AnalysisRequest> &requests) {
    requests.clear();
    ++includeDiscoveryGeneration;
    for (const auto &[uri, source] : documents) {
      (void)source;
      // The token cache stays: publication compares the new tokens with it
      // and asks for a refresh only when the project change altered them.
      const auto version = documentVersions.find(uri);
      requests.push_back(makeAnalysisRequestLocked(
          uri,
          version == documentVersions.end()
              ? std::nullopt
              : std::optional<std::int64_t>(version->second),
          advanceAnalysisGenerationLocked(uri),
          AnalysisTrigger::ProjectChanged));
    }
  }

  void didChangeConfiguration(const JsonValue *params) {
    const JsonValue *settings = member(params, "settings");
    const JsonValue *gti = member(settings, "gti");
    if (gti == nullptr) {
      return;
    }
    if (gti->getAsObject() == nullptr) {
      reportConfigurationError("GTI settings must be an object.");
      return;
    }
    updateProjectProfile(member(gti, "profile"));
  }

  void reportConfigurationError(std::string message) {
    sendJson(JsonObject{
        {"jsonrpc", "2.0"},
        {"method", "window/showMessage"},
        {"params", JsonObject{{"type", 1}, {"message", std::move(message)}}}});
  }

  void updateProjectProfile(const JsonValue *value) {
    std::optional<std::string> selected;
    if (value != nullptr && value->kind() != JsonValue::Null) {
      const auto name = value->getAsString();
      if (!name || !lang::driver::isPortableProjectName(*name)) {
        reportConfigurationError(
            "gti.profile must be null or a portable build profile name "
            "(for example, 'dev' or 'release'). The previous selection is "
            "unchanged.");
        return;
      }
      selected = name->str();
    }
    std::vector<AnalysisRequest> requests;
    {
      const std::lock_guard lock(stateMutex);
      if (selected == projectProfile) {
        return;
      }
      projectProfile = std::move(selected);
      invalidateProjectSnapshotsLocked(requests);
    }
    rejectPendingSemanticRequests(requests, -32801, "Content modified");
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
      // A waiting request ends the document's update debounce.
      analysisCondition.notify_all();
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
    {
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
    }
    if (access.queued) {
      // A waiting request ends the document's update debounce.
      analysisCondition.notify_all();
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
    case SemanticRequestKind::MacroExpansion:
      respondMacroExpansion(id, params, uri, *position, source, snapshot);
      break;
    default:
      sendJson(response(id, nullptr));
      break;
    }
  }

  void respondMacroExpansion(const JsonValue *id, const JsonValue *params,
                             const std::string &uri, Position position,
                             const std::string &source,
                             const AnalysisSnapshot &snapshot) {
    std::optional<std::int64_t> version;
    {
      const std::lock_guard lock(stateMutex);
      const auto current = analysisGenerations.find(uri);
      const auto foundVersion = documentVersions.find(uri);
      const auto requestedVersion =
          integerMember(member(params, "textDocument"), "version");
      if (foundVersion != documentVersions.end())
        version = foundVersion->second;
      if (current == analysisGenerations.end() ||
          current->second != snapshot.generation || !documents.contains(uri) ||
          (requestedVersion && requestedVersion != version)) {
        sendJson(errorResponse(id, -32801, "Content modified"));
        return;
      }
    }
    const auto offset = SourcePositionIndex(source).byteOffset(position);
    if (!offset) {
      sendJson(response(id, nullptr));
      return;
    }
    const auto sourceUnit =
        snapshot.frontend->sourceGraph.sourceUnitForPath(snapshot.rootPath);
    const auto info = lang::LanguageQueries().macroExpansion(
        *snapshot.frontend, sourceUnit, *offset);
    if (!info) {
      sendJson(response(id, nullptr));
      return;
    }
    const std::string declarationSource =
        sourceForSpan(info->declaration, snapshot.frontend->sources,
                      snapshot.rootPath, source);
    sendJson(response(
        id,
        JsonObject{
            {"uri", uri},
            {"version", version ? JsonValue(*version) : JsonValue(nullptr)},
            {"range", rangeJson(source, info->invocation.start,
                                info->invocation.end - info->invocation.start)},
            {"declaration",
             JsonObject{
                 {"uri", uriForSource(info->declaration.source,
                                      snapshot.rootPath, uri)},
                 {"range",
                  rangeJson(declarationSource, info->declaration.start,
                            info->declaration.end - info->declaration.start)}}},
            {"languageId", "gti"},
            {"readOnly", true},
            {"text", info->text},
            {"truncated", info->truncated}}));
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
        request.projectProfile = projectProfile;
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
    // The raw-pointer audit multiplies analysis time by well over an order of
    // magnitude (41 s against 0.6 s for an eight-include file), so the
    // edit-path analysis never runs it. A separate background audit of the
    // published generation carries the profile's mode; see `runAuditWorker`.
    result.auditAvailable =
        project.rawPointerAudit != lang::RawPointerAuditMode::Off;
    frontendOptions.rawPointerAudit = request.audit
                                          ? project.rawPointerAudit
                                          : lang::RawPointerAuditMode::Off;
    frontendOptions.reportLibraryAuditFindings =
        project.rawPointerAuditLibraryFindings;
    frontendOptions.analyzeRecoveredProgram = true;
    frontendOptions.requireEntryPoint = project.requireEntryPoint;
    frontendOptions.program = project.program;
    frontendOptions.synthesizeTestEntry = false;
    // HIR performs the compiler's concrete generic-instance reanalysis. The
    // LSP retains it only so editor diagnostics agree with `gti check` for
    // the open file's own code; MIR remains code-generation work and is not
    // built for editor snapshots. Lowering is scoped to the program roots and
    // the open file's declarations: library instances nothing in the file
    // reaches are the library's own `gti check` concern.
    frontendOptions.stopAfter = lang::FrontendPhase::Hir;
    frontendOptions.cancellation = request.cancellation.get();
    frontendOptions.lowerEntryUnitOnly = true;
    // The LSP never reads HIR bodies: library-only instances (trusted
    // declarations with only trusted arguments) are analysed for their
    // diagnostics but not lowered, so the library code only they reach is
    // not discovered; see HirLoweringScope::omitLibraryOnlyBodies.
    frontendOptions.lowerLibraryOnlyInstanceBodies = false;
    // Likewise, standard-library bodies nothing in the open file reaches are
    // not analysed; their diagnostics are that library's `gti check` concern.
    frontendOptions.trustedBodies = lang::TrustedBodyAnalysis::Reachable;
    frontendOptions.workMetrics = &result.workMetrics;
    frontendOptions.cache = request.frontendSources.cache;
    // Library-only concrete instance analyses are reused across snapshots;
    // see InstanceAnalysisCache.
    frontendOptions.instanceCache = request.frontendSources.instanceCache;
    // The standard-library prefix is analysed once per include graph and
    // adopted by every later snapshot; see SemanticPreambleCache.
    frontendOptions.preambleCache = request.frontendSources.preambleCache;
    frontendOptions.sourceGenerations = request.frontendSources.generations;
    frontendOptions.sourceDigests = request.frontendSources.digests;
    frontendOptions.target = project.configuration.target();
    frontendOptions.configurationFlags =
        project.configuration.configurationFlags();
    frontendOptions.declaredConfigurationFlags =
        project.configuration.declaredConfigurationFlags();
    frontendOptions.includeSearchRoots = project.includeSearchRoots;
    for (const lang::Diagnostic &diagnostic : project.diagnostics) {
      result.diagnostics.push_back(convertDiagnostic(
          diagnostic, project.sources, rootPath, uri, source));
    }
    if (!project.configurationValid) {
      // An invalid editor selection must not fabricate default-profile
      // semantics or inactive spans. Keep a current, empty semantic snapshot
      // so queries can return lexical/empty recovery instead of staying queued.
      result.frontend = std::make_shared<const lang::FrontendResult>();
      return result;
    }
    const SteadyClock::time_point frontendStart = SteadyClock::now();
    lang::FrontendResult frontend =
        lang::Frontend(frontendOptions)
            .analyze(*filePath, source, {standardLibrary.prelude},
                     request.sourceOverrides, {standardLibrary.root},
                     project.packageSourceRoots);
    result.frontendElapsedMicroseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            SteadyClock::now() - frontendStart)
            .count());
    // HIR runs so that instantiated generic bodies report the same
    // diagnostics as `gti check`; those diagnostics are already in
    // `frontend.diagnostics`. No editor query reads HIR, so it is released
    // here instead of being retained with every open document's snapshot.
    frontend.hir = lang::HirProgram{};
    result.frontend =
        std::make_shared<const lang::FrontendResult>(std::move(frontend));
    if (result.frontend->cancelled)
      return result;
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
        result.dependencies.emplace(
            fileUriFromPath(std::filesystem::path(loadedSource)),
            unit.sourceDigest);
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
                                            std::uint64_t generation,
                                            AnalysisTrigger trigger) const {
    AnalysisRequest request{.uri = uri,
                            .source = documents.at(uri),
                            .version = version,
                            .generation = generation,
                            .trigger = trigger};
    request.includeDiscoveryGeneration = includeDiscoveryGeneration;
    request.projectProfile = projectProfile;
    for (const auto &[documentUri, documentSource] : documents) {
      const std::optional<std::filesystem::path> path =
          filePathFromUri(documentUri);
      if (!path) {
        continue;
      }
      request.sourceOverrides[canonicalPath(*path).string()] = documentSource;
      if (const auto current = contentGenerations.find(documentUri);
          current != contentGenerations.end()) {
        request.documentGenerations[documentUri] = current->second;
      }
    }
    request.frontendSources = frontendSnapshots.capture();
    return request;
  }

  // `openedDigest` is the digest of a newly opened overlay. A root whose
  // published analysis read exactly that text sees no change and keeps its
  // result; opening a file the editor merely navigated to is then free.
  //
  // A dependant keeps its published diagnostics until its own re-analysis
  // replaces them. `publicationsForLocked` still drops any diagnostic whose
  // file text is no longer the one it was computed for, so the editor never
  // sees a range from a superseded version of an open file.
  void invalidateDependentsLocked(
      const std::string &changedUri, std::vector<AnalysisRequest> &requests,
      std::optional<std::uint64_t> openedDigest = std::nullopt) {
    for (const auto &[rootUri, dependencies] : dependenciesByRoot) {
      const auto snapshot = analysisSnapshots.find(rootUri);
      const bool discovering = snapshot != analysisSnapshots.end() &&
                               snapshot->second.includeSuggestionsRequested;
      const auto dependency = dependencies.find(changedUri);
      if (rootUri == changedUri ||
          (dependency == dependencies.end() && !discovering)) {
        continue;
      }
      if (openedDigest && dependency != dependencies.end() &&
          dependency->second != 0 && dependency->second == *openedDigest) {
        continue;
      }
      pendingAnalyses.erase(rootUri);
      const std::uint64_t generation = advanceAnalysisGenerationLocked(rootUri);
      if (documents.contains(rootUri)) {
        const auto version = documentVersions.find(rootUri);
        requests.push_back(makeAnalysisRequestLocked(
            rootUri,
            version == documentVersions.end()
                ? std::nullopt
                : std::optional<std::int64_t>(version->second),
            generation, AnalysisTrigger::DependencyChanged));
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
              request.entryPath, request.projectProfile,
              executionProfileOverride, rawPointerAuditOverride);
          if (project.configurationValid) {
            completion = lang::LanguageQueries().complete(
                {.entryPath = request.entryPath,
                 .source = request.source,
                 .byteOffset = request.byteOffset,
                 .includeSnippets = request.snippetSupport,
                 .target = project.configuration.target(),
                 .configurationFlags =
                     project.configuration.configurationFlags(),
                 .declaredConfigurationFlags =
                     project.configuration.declaredConfigurationFlags(),
                 // Completion is latency-critical and offers no diagnostics;
                 // the audit is background work (see `runAuditWorker`).
                 .rawPointerAudit = lang::RawPointerAuditMode::Off,
                 .trustedBodies = lang::TrustedBodyAnalysis::Reachable,
                 .preludePaths = {standardLibrary.prelude},
                 .sourceOverrides = request.sourceOverrides,
                 .standardLibraryRoots = {standardLibrary.root},
                 .packageSourceRoots = project.packageSourceRoots,
                 .workMetrics = &workMetrics,
                 .cache = request.frontendSources.cache,
                 .sourceGenerations = request.frontendSources.generations,
                 .sourceDigests = request.frontendSources.digests});
          }
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
      // A waiting audit may start once nothing interactive is pending.
      auditCondition.notify_all();
    }
  }

  // The wait before an edit-triggered analysis may start: the document's
  // median recent analysis time within the fixed bounds, or the pinned value.
  [[nodiscard]] std::chrono::microseconds
  updateDebounceLocked(const std::string &uri) const {
    if (fixedUpdateDebounce) {
      return *fixedUpdateDebounce;
    }
    const auto history = analysisDurations.find(uri);
    if (history == analysisDurations.end() || history->second.empty()) {
      return minimumUpdateDebounce;
    }
    std::vector<std::chrono::microseconds> sorted(history->second.begin(),
                                                  history->second.end());
    std::sort(sorted.begin(), sorted.end());
    return std::clamp<std::chrono::microseconds>(sorted[sorted.size() / 2],
                                                 minimumUpdateDebounce,
                                                 maximumUpdateDebounce);
  }

  void scheduleAnalysis(AnalysisRequest request) {
    {
      const std::lock_guard lock(stateMutex);
      if (stoppingAnalysis) {
        return;
      }
      const SteadyClock::time_point now = SteadyClock::now();
      switch (request.trigger) {
      case AnalysisTrigger::DocumentChanged:
      case AnalysisTrigger::DependencyChanged:
        // Each further edit restarts the wait, so a typing burst is analysed
        // once, from its final text, and stops a running analysis of the
        // document, whose result would only be discarded.
        request.readyAt = now + updateDebounceLocked(request.uri);
        if (const auto running = activeAnalysisCancellations.find(request.uri);
            running != activeAnalysisCancellations.end())
          running->second->store(true, std::memory_order_relaxed);
        break;
      case AnalysisTrigger::Opened:
      case AnalysisTrigger::ProjectChanged:
        request.readyAt = now;
        break;
      }
      if (!pendingAnalyses.contains(request.uri)) {
        analysisOrder.push_back(request.uri);
      }
      pendingAnalyses[request.uri] = std::move(request);
    }
    analysisCondition.notify_one();
  }

  // Takes the next runnable analysis, or nothing. A document already being
  // analysed is not started again on another worker: its newer request stays
  // pending until the running analysis finishes, so rapid edits to one file
  // cannot occupy the whole pool with results that will be discarded. A
  // debounced request is not runnable before `readyAt` unless a semantic
  // request is waiting on its document or the server is draining; `wake`
  // receives the earliest deferred start so the caller can sleep until it.
  [[nodiscard]] std::optional<AnalysisRequest>
  takeNextAnalysisLocked(SteadyClock::time_point now,
                         std::optional<SteadyClock::time_point> &wake) {
    const auto take =
        [this](std::unordered_map<std::string, AnalysisRequest>::iterator
                   pending) {
          AnalysisRequest request = std::move(pending->second);
          pendingAnalyses.erase(pending);
          activeAnalysisUris.insert(request.uri);
          activeAnalysisCancellations[request.uri] = request.cancellation;
          return request;
        };
    // A queued hover, definition, or similar request waits for its
    // document's analysis. Run that analysis before dependants queued
    // ahead of it; the order entry it leaves behind is skipped later.
    for (const PendingSemanticRequest &waiting : pendingSemanticRequests) {
      const auto pending = pendingAnalyses.find(waiting.uri);
      if (pending != pendingAnalyses.end() &&
          !activeAnalysisUris.contains(waiting.uri)) {
        return take(pending);
      }
    }
    for (auto entry = analysisOrder.begin(); entry != analysisOrder.end();) {
      const auto pending = pendingAnalyses.find(*entry);
      if (pending == pendingAnalyses.end()) {
        entry = analysisOrder.erase(entry);
        continue;
      }
      if (activeAnalysisUris.contains(*entry)) {
        ++entry;
        continue;
      }
      if (!drainingAnalyses && pending->second.readyAt > now) {
        if (!wake || pending->second.readyAt < *wake) {
          wake = pending->second.readyAt;
        }
        ++entry;
        continue;
      }
      analysisOrder.erase(entry);
      return take(pending);
    }
    return std::nullopt;
  }

  void runAnalysisWorker() {
    while (true) {
      AnalysisRequest request;
      {
        std::unique_lock lock(stateMutex);
        std::optional<AnalysisRequest> next;
        while (!stoppingAnalysis) {
          std::optional<SteadyClock::time_point> wake;
          next = takeNextAnalysisLocked(SteadyClock::now(), wake);
          if (next) {
            break;
          }
          if (wake) {
            analysisCondition.wait_until(lock, *wake);
          } else {
            analysisCondition.wait(lock);
          }
        }
        if (stoppingAnalysis) {
          if (next) {
            activeAnalysisUris.erase(next->uri);
            activeAnalysisCancellations.erase(next->uri);
          }
          return;
        }
        request = std::move(*next);
      }
      const SteadyClock::time_point analysisStart = SteadyClock::now();

      // Driver resolution runs on the worker but outside the crash guard. The
      // guard remains the narrow compiler-only boundary documented in the LSP
      // architecture, and neither step holds shared state.
      GuardedAnalysis guarded;
      try {
        ProjectAnalysisInputs project;
        if (const std::optional<std::filesystem::path> path =
                filePathFromUri(request.uri)) {
          project = resolveProjectAnalysisInputs(*path, request.projectProfile,
                                                 executionProfileOverride,
                                                 rawPointerAuditOverride);
        }
        guarded = runIsolatedAnalysis(request, project);
      } catch (const std::exception &error) {
        guarded.status = GuardedAnalysisStatus::Failed;
        guarded.failure = error.what();
      } catch (...) {
        guarded.status = GuardedAnalysisStatus::Failed;
        guarded.failure = "unknown exception during project resolution";
      }
      // A newer edit stopped this analysis: its partial result is dropped and
      // the pending request for the newer text runs next.
      const bool superseded =
          guarded.status == GuardedAnalysisStatus::Completed &&
          guarded.analysis->frontend != nullptr &&
          guarded.analysis->frontend->cancelled;
      if (superseded && frontendTracePath)
        recordFrontendWork("cancelled", request.uri, request.generation,
                           guarded.analysis->frontendElapsedMicroseconds,
                           guarded.analysis->workMetrics);
      switch (superseded ? GuardedAnalysisStatus::Failed : guarded.status) {
      case GuardedAnalysisStatus::Completed:
        if (frontendTracePath) {
          std::size_t retained = 0;
          {
            const std::lock_guard lock(stateMutex);
            retained = retainedFrontendCountLocked();
          }
          recordFrontendWork(
              "analysis", request.uri, request.generation,
              guarded.analysis->frontendElapsedMicroseconds,
              guarded.analysis->workMetrics, retained,
              guarded.analysis->frontend != nullptr &&
                  guarded.analysis->frontend->includeSuggestionsRequested);
        }
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
        if (superseded)
          break;
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

      bool sendRefresh = false;
      {
        const std::lock_guard lock(stateMutex);
        activeAnalysisUris.erase(request.uri);
        activeAnalysisCancellations.erase(request.uri);
        // A stopped analysis's partial duration says nothing about the
        // document's cost.
        if (!superseded) {
          std::deque<std::chrono::microseconds> &history =
              analysisDurations[request.uri];
          history.push_back(
              std::chrono::duration_cast<std::chrono::microseconds>(
                  SteadyClock::now() - analysisStart));
          if (history.size() > analysisDurationHistory) {
            history.pop_front();
          }
        }
        // One refresh per wave of dependency re-analyses, once the pool is
        // idle. Clients may answer a refresh by discarding and re-requesting
        // the tokens of every open document, so it is never sent per result.
        if (semanticTokenRefreshPending && pendingAnalyses.empty() &&
            activeAnalysisUris.empty()) {
          semanticTokenRefreshPending = false;
          sendRefresh = true;
        }
      }
      if (sendRefresh) {
        requestSemanticTokenRefresh();
      }
      // Wakes idle workers for a newer request of this document and lets
      // `flushAnalyses` observe the pool becoming idle.
      analysisCondition.notify_all();
      // A waiting audit may start once nothing interactive is pending.
      auditCondition.notify_all();
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
    std::optional<std::vector<SemanticToken>> heldTokens;
    AnalysisSnapshot published;
    bool auditScheduled = false;
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
      for (const auto &[dependency, digest] : analysis.dependencies) {
        const auto overlay = documents.find(dependency);
        if (overlay == documents.end()) {
          continue;
        }
        const auto snapshot = request.documentGenerations.find(dependency);
        // The dependency opened after this request was made, so the analysis
        // read it from disk. That is still its text if the overlay that
        // opened is exactly what was read.
        if (snapshot == request.documentGenerations.end() && digest != 0 &&
            digest == lang::sourceTextDigest(overlay->second)) {
          continue;
        }
        const auto current = contentGenerations.find(dependency);
        if (snapshot == request.documentGenerations.end() ||
            current == contentGenerations.end() ||
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
            advanceAnalysisGenerationLocked(request.uri),
            AnalysisTrigger::DependencyChanged);
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
        if (const auto audit = auditDiagnosticsByRoot.find(request.uri);
            audit != auditDiagnosticsByRoot.end() &&
            audit->second.generation != request.generation) {
          for (const LspDiagnostic &diagnostic : audit->second.diagnostics) {
            affected.insert(diagnostic.uri);
          }
          auditDiagnosticsByRoot.erase(audit);
        }
        if (const auto previous = analysisSnapshots.find(request.uri);
            previous != analysisSnapshots.end() && previous->second.frontend) {
          retireFrontendLocked(std::move(previous->second.frontend));
        }
        const bool includeSuggestionsRequested =
            analysis.frontend && analysis.frontend->includeSuggestionsRequested;
        analysisSnapshots[request.uri] = {
            .generation = request.generation,
            .rootPath = std::move(analysis.rootPath),
            .frontend = std::move(analysis.frontend),
            .includeSuggestionsRequested = includeSuggestionsRequested};
        switch (request.trigger) {
        case AnalysisTrigger::Opened:
        case AnalysisTrigger::DocumentChanged:
          // The client re-requests tokens after its own edit; the entry was
          // already dropped when the text changed.
          semanticTokenCache.erase(request.uri);
          break;
        case AnalysisTrigger::DependencyChanged:
        case AnalysisTrigger::ProjectChanged:
          // The client holds tokens it has no reason to re-request. Keep
          // them so the comparison below can tell whether they changed.
          if (const auto held = semanticTokenCache.find(request.uri);
              held != semanticTokenCache.end()) {
            heldTokens = held->second.tokens;
            published = analysisSnapshots.at(request.uri);
          }
          break;
        }
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
        if (analysis.auditAvailable && !stoppingAudit) {
          // The audit of this generation waits for the quiet period and an
          // idle pool; a newer generation replaces it before it starts.
          AnalysisRequest audit = request;
          audit.audit = true;
          audit.scheduledAt = SteadyClock::now();
          audit.readyAt = audit.scheduledAt + auditQuietPeriod;
          pendingAudits[request.uri] = std::move(audit);
          auditScheduled = true;
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
    if (heldTokens) {
      reconcileHeldSemanticTokens(request, published, std::move(*heldTokens));
    }
    if (auditScheduled) {
      auditCondition.notify_all();
    }
  }

  // Takes the audit that may start now: the pool and completion worker are
  // idle, its quiet period has passed, and its generation is still the
  // document's current one. Stale audits are dropped here. Among several
  // ready audits the most recently scheduled runs first, so the document the
  // user is working in is audited before documents left behind.
  [[nodiscard]] std::optional<AnalysisRequest>
  takeNextAuditLocked(SteadyClock::time_point now,
                      std::optional<SteadyClock::time_point> &wake) {
    for (auto pending = pendingAudits.begin();
         pending != pendingAudits.end();) {
      const auto generation = analysisGenerations.find(pending->first);
      if (generation == analysisGenerations.end() ||
          generation->second != pending->second.generation ||
          !documents.contains(pending->first)) {
        pending = pendingAudits.erase(pending);
        continue;
      }
      ++pending;
    }
    const bool idle = pendingAnalyses.empty() && activeAnalysisUris.empty() &&
                      completionRequests.empty() && activeCompletions == 0;
    auto selected = pendingAudits.end();
    for (auto pending = pendingAudits.begin(); pending != pendingAudits.end();
         ++pending) {
      if (pending->second.readyAt > now) {
        if (!wake || pending->second.readyAt < *wake) {
          wake = pending->second.readyAt;
        }
        continue;
      }
      if (!idle) {
        // Woken again when the pool drains.
        continue;
      }
      if (selected == pendingAudits.end() ||
          pending->second.scheduledAt > selected->second.scheduledAt) {
        selected = pending;
      }
    }
    if (selected == pendingAudits.end()) {
      return std::nullopt;
    }
    AnalysisRequest request = std::move(selected->second);
    pendingAudits.erase(selected);
    return request;
  }

  // The raw-pointer audit is whole-program provenance analysis that the
  // compiler cannot interrupt, so it runs on its own worker, only when nothing
  // interactive is pending, and only on a generation the editor has settled
  // on. Its findings are published as an addition to that generation's
  // ordinary diagnostics and discarded if the document moved on meanwhile.
  void runAuditWorker() {
    while (true) {
      AnalysisRequest request;
      {
        std::unique_lock lock(stateMutex);
        std::optional<AnalysisRequest> next;
        while (!stoppingAudit) {
          std::optional<SteadyClock::time_point> wake;
          next = takeNextAuditLocked(SteadyClock::now(), wake);
          if (next) {
            break;
          }
          if (wake) {
            auditCondition.wait_until(lock, *wake);
          } else {
            auditCondition.wait(lock);
          }
        }
        if (stoppingAudit) {
          return;
        }
        request = std::move(*next);
        auditRunning = true;
      }

      GuardedAnalysis guarded;
      try {
        ProjectAnalysisInputs project;
        if (const std::optional<std::filesystem::path> path =
                filePathFromUri(request.uri)) {
          project = resolveProjectAnalysisInputs(*path, request.projectProfile,
                                                 executionProfileOverride,
                                                 rawPointerAuditOverride);
        }
        guarded = runIsolatedAnalysis(request, project);
      } catch (const std::exception &error) {
        guarded.status = GuardedAnalysisStatus::Failed;
        guarded.failure = error.what();
      } catch (...) {
        guarded.status = GuardedAnalysisStatus::Failed;
        guarded.failure = "unknown exception during project resolution";
      }
      if (guarded.status == GuardedAnalysisStatus::Completed) {
        recordFrontendWork("audit", request.uri, request.generation,
                           guarded.analysis->frontendElapsedMicroseconds,
                           guarded.analysis->workMetrics);
        try {
          publishAudit(request, std::move(*guarded.analysis));
        } catch (const std::exception &error) {
          std::cerr << "LSP audit publication failed: " << error.what() << '\n';
        } catch (...) {
          std::cerr << "LSP audit publication failed with an unknown "
                       "exception\n";
        }
      } else if (guarded.status == GuardedAnalysisStatus::Failed) {
        std::cerr << "LSP audit failed: " << guarded.failure << '\n';
      } else {
        std::cerr << "LSP audit crashed; the document was skipped\n";
      }
      {
        const std::lock_guard lock(stateMutex);
        auditRunning = false;
      }
      auditCondition.notify_all();
    }
  }

  static std::string diagnosticIdentity(const LspDiagnostic &diagnostic) {
    const lang::SourceSpan &span = diagnostic.diagnostic.primary;
    return diagnostic.diagnostic.code + '\n' + diagnostic.uri + '\n' +
           std::to_string(span.start) + ':' + std::to_string(span.end) + '\n' +
           diagnosticMessage(diagnostic.diagnostic);
  }

  // Publishes the audit findings of a generation that is still current. The
  // audit analysis reports every diagnostic the ordinary one did; only the
  // additions are retained, so nothing is published twice and no list of
  // audit codes lives in the protocol layer.
  void publishAudit(const AnalysisRequest &request, DocumentAnalysis analysis) {
    std::vector<DiagnosticPublication> publications;
    {
      const std::lock_guard lock(stateMutex);
      // The whole-program result is not retained; the ordinary snapshot
      // already serves every query.
      if (analysis.frontend) {
        retireFrontendLocked(std::move(analysis.frontend));
      }
      const auto generation = analysisGenerations.find(request.uri);
      const auto ordinary = diagnosticsByRoot.find(request.uri);
      if (generation == analysisGenerations.end() ||
          generation->second != request.generation ||
          !documents.contains(request.uri) ||
          ordinary == diagnosticsByRoot.end()) {
        return;
      }
      std::unordered_set<std::string> known;
      for (const LspDiagnostic &diagnostic : ordinary->second) {
        known.insert(diagnosticIdentity(diagnostic));
      }
      std::unordered_set<std::string> affected{request.uri};
      if (const auto previous = auditDiagnosticsByRoot.find(request.uri);
          previous != auditDiagnosticsByRoot.end()) {
        for (const LspDiagnostic &diagnostic : previous->second.diagnostics) {
          affected.insert(diagnostic.uri);
        }
      }
      AuditDiagnostics findings{.generation = request.generation};
      for (LspDiagnostic &diagnostic : analysis.diagnostics) {
        if (known.contains(diagnosticIdentity(diagnostic))) {
          continue;
        }
        affected.insert(diagnostic.uri);
        findings.diagnostics.push_back(std::move(diagnostic));
      }
      auditDiagnosticsByRoot[request.uri] = std::move(findings);
      publications = publicationsForLocked(affected);
    }
    publish(std::move(publications));
  }

  // A dependency or project change re-analysed a document whose client
  // already holds semantic tokens. Compute the new tokens off the protocol
  // thread and compare: unchanged tokens become the cache for the new
  // generation and need no client round trip; changed tokens request one
  // refresh once the pool is idle.
  void reconcileHeldSemanticTokens(const AnalysisRequest &request,
                                   const AnalysisSnapshot &snapshot,
                                   std::vector<SemanticToken> heldTokens) {
    if (snapshot.frontend == nullptr) {
      return;
    }
    const lang::SourceUnitId sourceUnit =
        snapshot.frontend->sourceGraph.sourceUnitForPath(snapshot.rootPath);
    std::vector<SemanticToken> tokens = collectSemanticTokens(
        request.source, snapshot.frontend.get(), sourceUnit);
    const std::lock_guard lock(stateMutex);
    const auto generation = analysisGenerations.find(request.uri);
    if (generation == analysisGenerations.end() ||
        generation->second != request.generation ||
        !documents.contains(request.uri)) {
      return;
    }
    if (tokens != heldTokens) {
      semanticTokenRefreshPending = true;
    }
    semanticTokenCache[request.uri] = {.generation = request.generation,
                                       .tokens = std::move(tokens)};
  }

  // Advances a root's analysis generation. Its retained snapshot can then no
  // longer serve any request, so the frontend result is released now rather
  // than when the replacement publishes. A shared-dependency edit otherwise
  // holds a stale and a new whole-program result for every dependant at once.
  std::uint64_t advanceAnalysisGenerationLocked(const std::string &uri) {
    if (const auto snapshot = analysisSnapshots.find(uri);
        snapshot != analysisSnapshots.end() && snapshot->second.frontend) {
      retireFrontendLocked(std::move(snapshot->second.frontend));
    }
    return ++analysisGenerations[uri];
  }

  // Destroying a frontend graph is slow, so it happens on the reclamation
  // worker rather than on the protocol thread or under `stateMutex`. A
  // request still holding the result keeps it alive until it finishes.
  void
  retireFrontendLocked(std::shared_ptr<const lang::FrontendResult> frontend) {
    retiredFrontends.push_back(std::move(frontend));
    reclamationCondition.notify_one();
  }

  void runReclamationWorker() {
    while (true) {
      std::vector<std::shared_ptr<const lang::FrontendResult>> retired;
      {
        std::unique_lock lock(stateMutex);
        reclamationCondition.wait(lock, [this] {
          return stoppingReclamation || !retiredFrontends.empty();
        });
        retired.swap(retiredFrontends);
        if (retired.empty()) {
          return;
        }
      }
      retired.clear();
    }
  }

  void stopReclamationWorker() {
    {
      const std::lock_guard lock(stateMutex);
      stoppingReclamation = true;
    }
    reclamationCondition.notify_all();
    if (reclamationWorker.joinable()) {
      reclamationWorker.join();
    }
  }

  [[nodiscard]] std::size_t retainedFrontendCountLocked() const {
    return static_cast<std::size_t>(std::count_if(
        analysisSnapshots.begin(), analysisSnapshots.end(),
        [](const auto &entry) { return entry.second.frontend != nullptr; }));
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
      // An open file's publication carries its current version, so it may
      // include only diagnostics computed from exactly that text. A root
      // awaiting re-analysis still contributes the diagnostics it placed in
      // files whose text has not changed since it read them.
      const auto openText = documents.find(uri);
      // Audit findings follow their root's ordinary diagnostics; they are
      // additions to the same generation and obey the same text rule.
      std::vector<const std::vector<LspDiagnostic> *> sets;
      for (const std::string &root : roots) {
        sets.push_back(&diagnosticsByRoot.at(root));
        // Findings belong to one analysis generation: a dependency or
        // project change may alter them even when this file's text did not
        // change, so they wait for the next audit rather than carrying over.
        const auto audit = auditDiagnosticsByRoot.find(root);
        const auto generation = analysisGenerations.find(root);
        if (audit != auditDiagnosticsByRoot.end() &&
            generation != analysisGenerations.end() &&
            audit->second.generation == generation->second) {
          sets.push_back(&audit->second.diagnostics);
        }
      }
      for (const std::vector<LspDiagnostic> *diagnostics : sets) {
        for (const LspDiagnostic &diagnostic : *diagnostics) {
          if (diagnostic.uri != uri) {
            continue;
          }
          if (openText != documents.end() &&
              diagnostic.source != openText->second) {
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
    // Shutdown runs every pending analysis now rather than after its wait.
    drainingAnalyses = true;
    analysisCondition.notify_all();
    analysisCondition.wait(lock, [this] {
      return pendingAnalyses.empty() && activeAnalysisUris.empty();
    });
  }

  // `retainedFrontends` is the number of published snapshots still holding a
  // whole-program result when an analysis finished, before it publishes.
  void recordFrontendWork(
      std::string_view kind, std::string_view uri, std::uint64_t generation,
      std::uint64_t elapsedMicroseconds,
      const lang::FrontendWorkMetrics &metrics,
      std::optional<std::size_t> retainedFrontends = std::nullopt,
      bool includeSuggestionsRequested = false) const {
    if (!frontendTracePath) {
      return;
    }
    JsonObject record{
        {"schema", 1},
        {"kind", std::string(kind)},
        {"uri", std::string(uri)},
        {"generation", static_cast<std::int64_t>(generation)},
        {"elapsedMicroseconds", static_cast<std::int64_t>(elapsedMicroseconds)},
        // Monotonic time since server start, so concurrent work can be
        // ordered: the work ran over [finished - elapsed, finished].
        {"finishedMicroseconds",
         static_cast<std::int64_t>(
             std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::steady_clock::now() - serverStart)
                 .count())},
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
        {"semanticDeferredBodies",
         static_cast<std::int64_t>(metrics.semanticDeferredBodies)},
        {"semanticAnalyzedDeferredBodies",
         static_cast<std::int64_t>(metrics.semanticAnalyzedDeferredBodies)},
        {"semanticDemandRounds",
         static_cast<std::int64_t>(metrics.semanticDemandRounds)},
        {"semanticDemandFallbacks",
         static_cast<std::int64_t>(metrics.semanticDemandFallbacks)},
        {"semanticLateDemandBodies",
         static_cast<std::int64_t>(metrics.semanticLateDemandBodies)},
        {"hirOmittedInstanceBodies",
         static_cast<std::int64_t>(metrics.hirOmittedInstanceBodies)},
        {"semanticLateDemandMicroseconds",
         static_cast<std::int64_t>(metrics.semanticLateDemandMicroseconds)},
        {"hirMicroseconds", static_cast<std::int64_t>(metrics.hirMicroseconds)},
        {"hirFunctionInstances",
         static_cast<std::int64_t>(metrics.hirFunctionInstances)},
        {"includeSuggestionsRequested", includeSuggestionsRequested},
        {"preambleBuilds",
         static_cast<std::int64_t>(metrics.semanticPreambleBuilds)},
        {"preambleBuildMicroseconds",
         static_cast<std::int64_t>(metrics.semanticPreambleBuildMicroseconds)},
        {"preambleAdoptions",
         static_cast<std::int64_t>(metrics.semanticPreambleAdoptions)},
        {"preambleAdoptMicroseconds",
         static_cast<std::int64_t>(metrics.semanticPreambleAdoptMicroseconds)},
        {"preambleBodies",
         static_cast<std::int64_t>(metrics.semanticPreambleBodies)},
        {"preambleReachedBodies",
         static_cast<std::int64_t>(metrics.semanticPreambleReachedBodies)},
        {"instanceReuseHits",
         static_cast<std::int64_t>(metrics.semanticInstanceReuseHits)},
        {"instanceReuseMisses",
         static_cast<std::int64_t>(metrics.semanticInstanceReuseMisses)},
        {"instanceReuseRejected",
         static_cast<std::int64_t>(metrics.semanticInstanceReuseRejected)},
        {"instanceReuseIneligible",
         static_cast<std::int64_t>(metrics.semanticInstanceReuseIneligible)},
        {"instanceReuseStores",
         static_cast<std::int64_t>(metrics.semanticInstanceReuseStores)},
        {"instanceReuseMicroseconds",
         static_cast<std::int64_t>(metrics.semanticInstanceReuseMicroseconds)},
        {"conceptProofReuses",
         static_cast<std::int64_t>(metrics.semanticConceptProofReuses)},
        {"conceptProofRecords",
         static_cast<std::int64_t>(metrics.semanticConceptProofRecords)},
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
    const lang::SemanticPreambleCacheStats preambleStats =
        frontendSnapshots.preambleCache()->stats();
    record["preambleCache"] = JsonObject{
        {"entries", static_cast<std::int64_t>(preambleStats.entries)},
        {"hits", static_cast<std::int64_t>(preambleStats.hits)},
        {"misses", static_cast<std::int64_t>(preambleStats.misses)},
        {"stores", static_cast<std::int64_t>(preambleStats.stores)},
        {"maximumEntries",
         static_cast<std::int64_t>(preambleStats.limits.maximumEntries)}};
    const lang::InstanceAnalysisCacheStats instanceStats =
        frontendSnapshots.instanceCache()->stats();
    record["instanceCache"] = JsonObject{
        {"entries", static_cast<std::int64_t>(instanceStats.entries)},
        {"estimatedBytes",
         static_cast<std::int64_t>(instanceStats.estimatedBytes)},
        {"generations", static_cast<std::int64_t>(instanceStats.generations)},
        {"maximumEntries",
         static_cast<std::int64_t>(instanceStats.limits.maximumEntries)},
        {"maximumEstimatedBytes",
         static_cast<std::int64_t>(
             instanceStats.limits.maximumEstimatedBytes)}};
    if (retainedFrontends) {
      record["retainedFrontends"] =
          static_cast<std::int64_t>(*retainedFrontends);
    }
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
    for (llvm::thread &worker : analysisWorkers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  // Returns false when an audit is still running inside the compiler; the
  // caller then must not wait for it (see `exitProcess`).
  [[nodiscard]] bool stopAuditWorker() {
    bool running = false;
    {
      const std::lock_guard lock(stateMutex);
      stoppingAudit = true;
      pendingAudits.clear();
      running = auditRunning;
    }
    auditCondition.notify_all();
    if (running) {
      return false;
    }
    if (auditWorker.joinable()) {
      auditWorker.join();
    }
    return true;
  }

  lang::StandardLibraryLayout standardLibrary;
  std::optional<lang::ExecutionProfile> executionProfileOverride;
  std::optional<lang::RawPointerAuditMode> rawPointerAuditOverride;
  // Protected by stateMutex; workers consume only their captured request value.
  std::optional<std::string> projectProfile;
  std::optional<std::filesystem::path> frontendTracePath;
  const std::chrono::steady_clock::time_point serverStart =
      std::chrono::steady_clock::now();
  mutable std::mutex frontendTraceMutex;
  lang::lsp::FrontendSnapshotState frontendSnapshots;
  mutable std::mutex stateMutex;
  std::condition_variable analysisCondition;
  std::condition_variable reclamationCondition;
  std::condition_variable completionCondition;
  std::unordered_map<std::string, std::string> clientUris;
  std::unordered_map<std::string, std::unordered_set<std::string>>
      openClientUris;
  std::unordered_map<std::string, std::string> documents;
  std::unordered_map<std::string, std::int64_t> documentVersions;
  std::unordered_map<std::string, std::vector<LspDiagnostic>> diagnosticsByRoot;
  std::unordered_map<std::string,
                     std::unordered_map<std::string, std::uint64_t>>
      dependenciesByRoot;
  std::unordered_map<std::string, std::uint64_t> analysisGenerations;
  // Advances only when an open document's own text or overlay presence
  // changes (open, change, close). Analysis generations also advance when a
  // document is merely re-analysed because a dependency changed; using those
  // for dependency staleness made each publication along an include chain
  // invalidate every dependant above it.
  std::unordered_map<std::string, std::uint64_t> contentGenerations;
  std::uint64_t includeDiscoveryGeneration = 0;
  std::unordered_map<std::string, AnalysisSnapshot> analysisSnapshots;
  std::unordered_map<std::string, CachedSemanticTokens> semanticTokenCache;
  std::unordered_map<std::string, AnalysisRequest> pendingAnalyses;
  std::deque<std::string> analysisOrder;
  // Recent wall-clock analysis durations per document, newest last; they size
  // that document's update debounce.
  std::unordered_map<std::string, std::deque<std::chrono::microseconds>>
      analysisDurations;
  const std::optional<std::chrono::microseconds> fixedUpdateDebounce =
      fixedUpdateDebounceFromEnvironment();
  bool drainingAnalyses = false;
  // Set when a dependency or project re-analysis changed tokens a client
  // holds; cleared when the one refresh for that wave is sent.
  bool semanticTokenRefreshPending = false;
  // Background raw-pointer audits; see `runAuditWorker`.
  struct AuditDiagnostics {
    std::uint64_t generation = 0;
    std::vector<LspDiagnostic> diagnostics;
  };
  std::condition_variable auditCondition;
  std::unordered_map<std::string, AnalysisRequest> pendingAudits;
  std::unordered_map<std::string, AuditDiagnostics> auditDiagnosticsByRoot;
  const std::chrono::milliseconds auditQuietPeriod =
      auditQuietPeriodFromEnvironment();
  bool stoppingAudit = false;
  bool auditRunning = false;
  std::deque<CompletionRequest> completionRequests;
  std::deque<PendingSemanticRequest> pendingSemanticRequests;
  std::unordered_set<std::string> activeCompletionRequests;
  std::unordered_set<std::string> canceledRequests;
  // Documents whose analysis is running on a pool worker.
  std::unordered_set<std::string> activeAnalysisUris;
  // The cancellation flag of each document's running analysis.
  std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>>
      activeAnalysisCancellations;
  std::size_t activeCompletions = 0;
  bool stoppingAnalysis = false;
  bool stoppingReclamation = false;
  std::vector<std::shared_ptr<const lang::FrontendResult>> retiredFrontends;
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
  llvm::thread completionWorker;
  std::vector<llvm::thread> analysisWorkers;
  llvm::thread reclamationWorker;
  llvm::thread auditWorker;
};

} // namespace

int main(int argc, char *argv[]) {
  lang::installCrashHandlers(argc > 0 ? argv[0] : "gti_lsp");
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    std::cout << "gti_lsp " << GTI_VERSION << '\n';
    return 0;
  }
  std::optional<lang::ExecutionProfile> executionProfile;
  std::optional<lang::RawPointerAuditMode> rawPointerAudit;
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
    if (argument == "--raw-pointer-audit" ||
        argument.starts_with("--raw-pointer-audit=")) {
      std::string_view value;
      if (argument == "--raw-pointer-audit") {
        if (++index >= argc) {
          std::cerr
              << "gti_lsp: --raw-pointer-audit requires off, warn, or strict\n";
          return 64;
        }
        value = argv[index];
      } else {
        value =
            argument.substr(std::string_view("--raw-pointer-audit=").size());
      }
      if (rawPointerAudit ||
          !(rawPointerAudit = lang::parseRawPointerAuditMode(value))) {
        std::cerr << "gti_lsp: --raw-pointer-audit must be off, warn, or "
                     "strict and specified only once\n";
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
             executionProfile, rawPointerAudit)
      .run();
}
