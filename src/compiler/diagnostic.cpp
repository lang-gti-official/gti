#include "gti/diagnostic.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace lang {

SourceSpan tokenSpan(const Token &token) {
  const std::size_t length = token.generated ? 1 : token.lexeme.size();
  return {token.source, token.position, token.position + length, token.line};
}

Diagnostic makeDiagnostic(std::string code, DiagnosticPhase phase,
                          SourceSpan primary, std::string message) {
  return {.code = std::move(code),
          .phase = phase,
          .primary = std::move(primary),
          .message = std::move(message)};
}

Diagnostic makeDiagnostic(std::string code, DiagnosticPhase phase,
                          const Token &token, std::string message) {
  return makeDiagnostic(std::move(code), phase, tokenSpan(token),
                        std::move(message));
}

void SourceManager::clear() { sources.clear(); }

void SourceManager::set(std::string name, std::string source) {
  Entry entry{std::move(source), {}};
  entry.lineStarts.push_back(0);
  for (std::size_t index = 0; index < entry.text.size(); ++index) {
    if (entry.text[index] == '\n') {
      entry.lineStarts.push_back(index + 1);
    }
  }
  sources.insert_or_assign(std::move(name), std::move(entry));
}

const std::string *SourceManager::find(std::string_view name) const {
  const Entry *entry = findEntry(name);
  return entry == nullptr ? nullptr : &entry->text;
}

std::vector<std::string> SourceManager::names() const {
  std::vector<std::string> result;
  result.reserve(sources.size());
  for (const auto &[name, _] : sources) {
    result.push_back(name);
  }
  return result;
}

SourceLocation SourceManager::locate(const SourceSpan &span) const {
  const Entry *entry = findEntry(span.source);
  if (entry == nullptr) {
    return {.line = static_cast<std::size_t>(std::max(span.line, 1))};
  }

  const std::string &source = entry->text;
  const std::size_t offset = std::min(span.start, source.size());
  const auto next = std::upper_bound(entry->lineStarts.begin(),
                                     entry->lineStarts.end(), offset);
  const std::size_t lineIndex =
      static_cast<std::size_t>(std::distance(entry->lineStarts.begin(), next)) -
      1;

  SourceLocation location;
  location.line = lineIndex + 1;
  location.lineStart = entry->lineStarts[lineIndex];
  location.lineEnd = lineIndex + 1 < entry->lineStarts.size()
                         ? entry->lineStarts[lineIndex + 1] - 1
                         : source.size();

  location.column = 1;
  for (std::size_t index = location.lineStart; index < offset;) {
    const unsigned char byte = static_cast<unsigned char>(source[index]);
    std::size_t length = 1;
    if ((byte & 0xE0U) == 0xC0U) {
      length = 2;
    } else if ((byte & 0xF0U) == 0xE0U) {
      length = 3;
    } else if ((byte & 0xF8U) == 0xF0U) {
      length = 4;
    }
    index += std::min(length, offset - index);
    ++location.column;
  }
  return location;
}

std::string_view SourceManager::line(const SourceSpan &span) const {
  const Entry *entry = findEntry(span.source);
  if (entry == nullptr) {
    return {};
  }
  const SourceLocation location = locate(span);
  return std::string_view(entry->text)
      .substr(location.lineStart, location.lineEnd - location.lineStart);
}

const SourceManager::Entry *
SourceManager::findEntry(std::string_view name) const {
  const auto found = sources.find(name);
  return found == sources.end() ? nullptr : &found->second;
}

} // namespace lang
