#pragma once

#include "gti/token.h"

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

  friend bool operator==(const SourceSpan &, const SourceSpan &) = default;
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

SourceSpan tokenSpan(const Token &token);
Diagnostic makeDiagnostic(std::string code, DiagnosticPhase phase,
                          SourceSpan primary, std::string message);
Diagnostic makeDiagnostic(std::string code, DiagnosticPhase phase,
                          const Token &token, std::string message);

struct SourceLocation {
  std::size_t line = 1;
  std::size_t column = 1;
  std::size_t lineStart = 0;
  std::size_t lineEnd = 0;
};

class SourceManager {
public:
  void clear();
  void set(std::string name, std::string source);
  [[nodiscard]] const std::string *find(std::string_view name) const;
  [[nodiscard]] std::vector<std::string> names() const;
  [[nodiscard]] SourceLocation locate(const SourceSpan &span) const;
  [[nodiscard]] std::string_view line(const SourceSpan &span) const;

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

  [[nodiscard]] const Entry *findEntry(std::string_view name) const;

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
