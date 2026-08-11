#pragma once

#include "gti/token.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lang {

enum class DiagnosticSeverity {
  Error = 1,
  Warning = 2,
  Information = 3,
  Hint = 4,
};

enum class DiagnosticPhase {
  Lexing,
  SourceLoading,
  Parsing,
  Semantics,
  Driver,
  Backend,
};

struct SourceSpan {
  std::string source;
  std::size_t start = 0;
  std::size_t end = 0;
  int line = 1;
};

struct RelatedDiagnostic {
  SourceSpan span;
  std::string message;
};

struct FixIt {
  SourceSpan span;
  std::string replacement;
  std::string message;
};

struct Diagnostic {
  std::string code;
  DiagnosticPhase phase = DiagnosticPhase::Semantics;
  DiagnosticSeverity severity = DiagnosticSeverity::Error;
  SourceSpan primary;
  std::string message;
  std::vector<RelatedDiagnostic> related;
  std::vector<FixIt> fixes;
  std::vector<std::string> hints;
};

inline SourceSpan tokenSpan(const Token &token) {
  const std::size_t length = token.generated ? 1 : token.lexeme.size();
  return {token.source, token.position, token.position + length, token.line};
}

inline Diagnostic makeDiagnostic(std::string code, DiagnosticPhase phase,
                                 SourceSpan primary, std::string message) {
  return {.code = std::move(code),
          .phase = phase,
          .primary = std::move(primary),
          .message = std::move(message)};
}

inline Diagnostic makeDiagnostic(std::string code, DiagnosticPhase phase,
                                 const Token &token, std::string message) {
  return makeDiagnostic(std::move(code), phase, tokenSpan(token),
                        std::move(message));
}

struct SourceLocation {
  std::size_t line = 1;
  std::size_t column = 1;
  std::size_t lineStart = 0;
  std::size_t lineEnd = 0;
};

class SourceManager {
public:
  void clear() { sources.clear(); }

  void set(std::string name, std::string source) {
    Entry entry{std::move(source), {}};
    entry.lineStarts.push_back(0);
    for (std::size_t index = 0; index < entry.text.size(); ++index) {
      if (entry.text[index] == '\n') {
        entry.lineStarts.push_back(index + 1);
      }
    }
    sources.insert_or_assign(std::move(name), std::move(entry));
  }

  [[nodiscard]] const std::string *find(std::string_view name) const {
    const Entry *entry = findEntry(name);
    return entry == nullptr ? nullptr : &entry->text;
  }

  [[nodiscard]] std::vector<std::string> names() const {
    std::vector<std::string> result;
    result.reserve(sources.size());
    for (const auto &[name, _] : sources) {
      result.push_back(name);
    }
    return result;
  }

  [[nodiscard]] SourceLocation locate(const SourceSpan &span) const {
    const Entry *entry = findEntry(span.source);
    if (entry == nullptr) {
      return {.line = static_cast<std::size_t>(std::max(span.line, 1))};
    }

    const std::string &source = entry->text;
    const std::size_t offset = std::min(span.start, source.size());
    const auto next = std::upper_bound(entry->lineStarts.begin(),
                                       entry->lineStarts.end(), offset);
    const std::size_t lineIndex = static_cast<std::size_t>(std::distance(
                                      entry->lineStarts.begin(), next)) -
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

  [[nodiscard]] std::string_view line(const SourceSpan &span) const {
    const Entry *entry = findEntry(span.source);
    if (entry == nullptr) {
      return {};
    }
    const SourceLocation location = locate(span);
    return std::string_view(entry->text)
        .substr(location.lineStart, location.lineEnd - location.lineStart);
  }

private:
  // Text plus the byte offset of every line start. The index makes locate()
  // a binary search instead of a scan from the first byte, and it is built
  // once when the source is registered.
  struct Entry {
    std::string text;
    std::vector<std::size_t> lineStarts;
  };

  struct TransparentStringHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const {
      return std::hash<std::string_view>{}(value);
    }
  };

  [[nodiscard]] const Entry *findEntry(std::string_view name) const {
    const auto found = sources.find(name);
    return found == sources.end() ? nullptr : &found->second;
  }

  std::unordered_map<std::string, Entry, TransparentStringHash, std::equal_to<>>
      sources;
};

inline constexpr std::string_view phaseName(DiagnosticPhase phase) {
  switch (phase) {
  case DiagnosticPhase::Lexing:
    return "lexing";
  case DiagnosticPhase::SourceLoading:
    return "source";
  case DiagnosticPhase::Parsing:
    return "parsing";
  case DiagnosticPhase::Semantics:
    return "semantics";
  case DiagnosticPhase::Driver:
    return "driver";
  case DiagnosticPhase::Backend:
    return "backend";
  }
  return "compiler";
}

} // namespace lang
