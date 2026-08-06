#pragma once

#include "gti/token.h"

#include <algorithm>
#include <cstddef>
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
  return {token.source, token.position, token.position + token.lexeme.size(),
          token.line};
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
    sources.insert_or_assign(std::move(name), std::move(source));
  }

  [[nodiscard]] const std::string *find(std::string_view name) const {
    const auto found = sources.find(std::string(name));
    return found == sources.end() ? nullptr : &found->second;
  }

  [[nodiscard]] SourceLocation locate(const SourceSpan &span) const {
    const std::string *source = find(span.source);
    if (source == nullptr) {
      return {.line = static_cast<std::size_t>(std::max(span.line, 1))};
    }

    const std::size_t offset = std::min(span.start, source->size());
    SourceLocation location;
    for (std::size_t index = 0; index < offset; ++index) {
      if ((*source)[index] == '\n') {
        ++location.line;
        location.lineStart = index + 1;
      }
    }
    location.lineEnd = source->find('\n', offset);
    if (location.lineEnd == std::string::npos) {
      location.lineEnd = source->size();
    }

    location.column = 1;
    for (std::size_t index = location.lineStart; index < offset;) {
      const unsigned char byte = static_cast<unsigned char>((*source)[index]);
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
    const std::string *source = find(span.source);
    if (source == nullptr) {
      return {};
    }
    const SourceLocation location = locate(span);
    return std::string_view(*source).substr(
        location.lineStart, location.lineEnd - location.lineStart);
  }

private:
  std::unordered_map<std::string, std::string> sources;
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
