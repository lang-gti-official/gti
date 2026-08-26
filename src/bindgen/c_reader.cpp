#include "gti/bindgen/header_reader.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// BoundedCReader: a hand-written lexer and declaration parser for a bounded
// C17 subset. Reading is fail-soft per declaration: anything outside the
// bounded surface becomes a warning diagnostic plus a skip, never an abort,
// so one exotic declaration cannot cost the rest of the header. The
// preprocessing policy is deliberately minimal and honest — see
// docs/plans/native-bindings-import.md.

namespace lang::bindgen {
namespace {

// ---------------------------------------------------------------------------
// Preprocessing: strip comments, recognize `#pragma once` and a whole-file
// include guard, report every other directive, and blank the lines it drops
// so token origins keep their source line numbers.
// ---------------------------------------------------------------------------

struct PreprocessResult {
  std::string text;
  std::vector<ReaderDiagnostic> diagnostics;
};

void stripComments(std::string &text) {
  std::string out;
  out.reserve(text.size());
  std::size_t index = 0;
  const std::size_t size = text.size();
  while (index < size) {
    const char c = text[index];
    if (c == '/' && index + 1 < size && text[index + 1] == '/') {
      while (index < size && text[index] != '\n') {
        ++index;
      }
      continue;
    }
    if (c == '/' && index + 1 < size && text[index + 1] == '*') {
      index += 2;
      while (index + 1 < size &&
             !(text[index] == '*' && text[index + 1] == '/')) {
        if (text[index] == '\n') {
          out.push_back('\n');
        }
        ++index;
      }
      index = index + 2 <= size ? index + 2 : size;
      out.push_back(' ');
      continue;
    }
    if (c == '"' || c == '\'') {
      const char quote = c;
      out.push_back(c);
      ++index;
      while (index < size && text[index] != quote) {
        if (text[index] == '\\' && index + 1 < size) {
          out.push_back(text[index]);
          ++index;
        }
        out.push_back(text[index]);
        ++index;
      }
      if (index < size) {
        out.push_back(quote);
        ++index;
      }
      continue;
    }
    out.push_back(c);
    ++index;
  }
  text = std::move(out);
}

[[nodiscard]] std::string_view trimmed(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return value;
}

[[nodiscard]] std::string_view directiveName(std::string_view line) {
  line = trimmed(line);
  if (line.empty() || line.front() != '#') {
    return {};
  }
  line = trimmed(line.substr(1));
  std::size_t end = 0;
  while (end < line.size() &&
         std::isalpha(static_cast<unsigned char>(line[end])) != 0) {
    ++end;
  }
  return line.substr(0, end);
}

[[nodiscard]] std::string_view directiveRest(std::string_view line) {
  line = trimmed(line);
  line = trimmed(line.substr(1));
  std::size_t end = 0;
  while (end < line.size() &&
         std::isalpha(static_cast<unsigned char>(line[end])) != 0) {
    ++end;
  }
  return trimmed(line.substr(end));
}

PreprocessResult preprocess(std::string_view headerText) {
  PreprocessResult result;
  std::string text(headerText);
  stripComments(text);

  std::vector<std::string_view> lines;
  {
    const std::string_view view(text);
    std::size_t start = 0;
    while (start <= view.size()) {
      const std::size_t end = view.find('\n', start);
      if (end == std::string_view::npos) {
        lines.push_back(view.substr(start));
        break;
      }
      lines.push_back(view.substr(start, end - start));
      start = end + 1;
    }
  }

  // Whole-file include-guard recognition: the first directive is `#ifndef X`
  // immediately followed by `#define X`, and the final directive in the file
  // is the matching `#endif`.
  std::size_t guardIfLine = lines.size();
  std::size_t guardDefineLine = lines.size();
  std::size_t guardEndLine = lines.size();
  {
    std::vector<std::size_t> directives;
    for (std::size_t index = 0; index < lines.size(); ++index) {
      if (!directiveName(lines[index]).empty()) {
        directives.push_back(index);
      }
    }
    if (directives.size() >= 3 &&
        directiveName(lines[directives[0]]) == "ifndef" &&
        directiveName(lines[directives[1]]) == "define" &&
        directiveName(lines[directives.back()]) == "endif") {
      const std::string_view guard = directiveRest(lines[directives[0]]);
      if (!guard.empty() && guard == directiveRest(lines[directives[1]])) {
        guardIfLine = directives[0];
        guardDefineLine = directives[1];
        guardEndLine = directives.back();
      }
    }
  }

  std::ostringstream out;
  std::size_t conditionalDepth = 0;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::string_view line = lines[index];
    const std::string_view directive = directiveName(line);
    const SourceOrigin origin{static_cast<std::uint32_t>(index + 1), 1};
    const bool guardLine = index == guardIfLine || index == guardDefineLine ||
                           index == guardEndLine;
    if (directive.empty() && conditionalDepth == 0) {
      out << line << '\n';
      continue;
    }
    out << '\n';
    if (directive.empty() || guardLine) {
      continue;
    }
    if (conditionalDepth != 0) {
      if (directive == "if" || directive == "ifdef" || directive == "ifndef") {
        ++conditionalDepth;
      } else if (directive == "endif") {
        --conditionalDepth;
      }
      continue;
    }
    if (directive == "pragma" && directiveRest(line) == "once") {
      continue;
    }
    if (directive == "include") {
      result.diagnostics.push_back(
          {ReaderSeverity::Warning, origin,
           "#include is not resolved; declarations from the included header "
           "are unavailable to this import"});
      continue;
    }
    if (directive == "define" || directive == "undef") {
      result.diagnostics.push_back({ReaderSeverity::Warning, origin,
                                    "macro definitions are not imported"});
      continue;
    }
    if (directive == "if" || directive == "ifdef" || directive == "ifndef") {
      result.diagnostics.push_back(
          {ReaderSeverity::Warning, origin,
           "conditional block is outside the bounded preprocessing policy; "
           "its contents were skipped"});
      conditionalDepth = 1;
      continue;
    }
    result.diagnostics.push_back(
        {ReaderSeverity::Warning, origin,
         "directive '#" + std::string(directive) + "' was skipped"});
  }
  result.text = out.str();
  return result;
}

// ---------------------------------------------------------------------------
// Lexer.
// ---------------------------------------------------------------------------

enum class TokenKind { Identifier, Number, Punct, String, Character, End };

struct Token {
  TokenKind kind = TokenKind::End;
  std::string text;
  SourceOrigin origin;
};

std::vector<Token> lex(std::string_view text) {
  std::vector<Token> tokens;
  std::uint32_t line = 1;
  std::uint32_t column = 1;
  std::size_t index = 0;
  const auto advance = [&](std::size_t count) {
    for (std::size_t step = 0; step < count && index < text.size(); ++step) {
      if (text[index] == '\n') {
        ++line;
        column = 1;
      } else {
        ++column;
      }
      ++index;
    }
  };
  while (index < text.size()) {
    const char c = text[index];
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      advance(1);
      continue;
    }
    const SourceOrigin origin{line, column};
    if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_') {
      std::size_t end = index;
      while (end < text.size() &&
             (std::isalnum(static_cast<unsigned char>(text[end])) != 0 ||
              text[end] == '_')) {
        ++end;
      }
      tokens.push_back({TokenKind::Identifier,
                        std::string(text.substr(index, end - index)), origin});
      advance(end - index);
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
      std::size_t end = index;
      while (end < text.size() &&
             (std::isalnum(static_cast<unsigned char>(text[end])) != 0 ||
              text[end] == '.')) {
        ++end;
      }
      tokens.push_back({TokenKind::Number,
                        std::string(text.substr(index, end - index)), origin});
      advance(end - index);
      continue;
    }
    if (c == '"' || c == '\'') {
      std::size_t end = index + 1;
      while (end < text.size() && text[end] != c) {
        end += text[end] == '\\' && end + 1 < text.size() ? 2 : 1;
      }
      end = end < text.size() ? end + 1 : end;
      tokens.push_back({c == '"' ? TokenKind::String : TokenKind::Character,
                        std::string(text.substr(index, end - index)), origin});
      advance(end - index);
      continue;
    }
    if (c == '.' && index + 2 < text.size() && text[index + 1] == '.' &&
        text[index + 2] == '.') {
      tokens.push_back({TokenKind::Punct, "...", origin});
      advance(3);
      continue;
    }
    tokens.push_back({TokenKind::Punct, std::string(1, c), origin});
    advance(1);
  }
  tokens.push_back({TokenKind::End, "", {line, column}});
  return tokens;
}

// ---------------------------------------------------------------------------
// Parser.
// ---------------------------------------------------------------------------

class Parser {
public:
  Parser(std::vector<Token> tokens, CTargetProfile profile, ReadResult &result)
      : tokens_(std::move(tokens)), profile_(profile), result_(result) {
    rootScope_ = result_.model.addScope(
        {NativeScopeKind::TranslationUnit, std::string(), 0});
  }

  void run() {
    while (!atEnd()) {
      parseTopLevel();
    }
  }

private:
  std::vector<Token> tokens_;
  std::size_t position_ = 0;
  CTargetProfile profile_;
  ReadResult &result_;
  NativeScopeId rootScope_ = 0;
  // One name table resolves `struct X`/`enum X` tags and bare typedef names;
  // parseBaseType builds the reference kind from the declaration's own kind.
  std::vector<std::pair<std::string, NativeDeclarationId>> tags_;
  std::vector<std::pair<std::string, NativeDeclarationId>> names_;

  [[nodiscard]] const Token &peek(std::size_t offset = 0) const {
    const std::size_t index = position_ + offset;
    return index < tokens_.size() ? tokens_[index] : tokens_.back();
  }
  [[nodiscard]] bool atEnd() const { return peek().kind == TokenKind::End; }
  const Token &take() {
    const Token &token = peek();
    position_ = std::min(position_ + 1, tokens_.size() - 1);
    return token;
  }
  [[nodiscard]] bool check(std::string_view text) const {
    return peek().text == text;
  }
  bool match(std::string_view text) {
    if (!check(text)) {
      return false;
    }
    take();
    return true;
  }

  void warn(SourceOrigin origin, std::string message) {
    result_.diagnostics.push_back(
        {ReaderSeverity::Warning, origin, std::move(message)});
  }

  // Skip to the next declaration boundary: `;` at brace depth zero, or the
  // close of a top-level braced body.
  void resync() {
    std::size_t depth = 0;
    while (!atEnd()) {
      const Token &token = take();
      if (token.text == "{") {
        ++depth;
      } else if (token.text == "}") {
        if (depth <= 1) {
          match(";");
          return;
        }
        --depth;
      } else if (token.text == ";" && depth == 0) {
        return;
      }
    }
  }

  [[nodiscard]] static NativeDeclarationId
  lookup(const std::vector<std::pair<std::string, NativeDeclarationId>> &table,
         std::string_view name) {
    for (const auto &[known, id] : table) {
      if (known == name) {
        return id;
      }
    }
    return 0;
  }

  NativeDeclarationId ensureRecordTag(const std::string &tag, bool isUnion,
                                      SourceOrigin origin) {
    if (const NativeDeclarationId existing = lookup(tags_, tag);
        existing != 0) {
      return existing;
    }
    NativeDeclaration record;
    record.kind = NativeDeclarationKind::Record;
    record.name = tag;
    record.scope = rootScope_;
    record.origin = origin;
    record.recordIsUnion = isUnion;
    const NativeDeclarationId id =
        result_.model.addDeclaration(std::move(record));
    result_.model.order.push_back(id);
    tags_.emplace_back(tag, id);
    return id;
  }

  // -- Types ----------------------------------------------------------------

  [[nodiscard]] NativeTypeId makeInteger(std::uint32_t bits, bool isSigned) {
    NativeType type;
    type.kind = NativeTypeKind::Integer;
    type.bitWidth = bits;
    type.isSigned = isSigned;
    return result_.model.addType(std::move(type));
  }

  [[nodiscard]] NativeTypeId makeReference(NativeDeclarationId declaration) {
    const NativeDeclaration *target =
        result_.model.findDeclaration(declaration);
    NativeType type;
    type.kind =
        target->kind == NativeDeclarationKind::Record ? NativeTypeKind::Record
        : target->kind == NativeDeclarationKind::Enum ? NativeTypeKind::Enum
                                                      : NativeTypeKind::Alias;
    type.declaration = declaration;
    return result_.model.addType(std::move(type));
  }

  [[nodiscard]] NativeTypeId makeUnsupported(std::string spelling,
                                             std::string reason) {
    NativeType type;
    type.kind = NativeTypeKind::Unsupported;
    type.spelling = std::move(spelling);
    type.reason = std::move(reason);
    return result_.model.addType(std::move(type));
  }

  // Known <stdint.h>/<stddef.h> spellings resolve through the target profile
  // so ubiquitous fixed-width headers read without their includes.
  [[nodiscard]] std::optional<NativeTypeId>
  builtinTypedef(std::string_view name) {
    struct Fixed {
      std::string_view name;
      std::uint32_t bits;
      bool isSigned;
    };
    static constexpr Fixed fixed[] = {
        {"int8_t", 8, true},     {"int16_t", 16, true},
        {"int32_t", 32, true},   {"int64_t", 64, true},
        {"uint8_t", 8, false},   {"uint16_t", 16, false},
        {"uint32_t", 32, false}, {"uint64_t", 64, false},
    };
    for (const Fixed &entry : fixed) {
      if (entry.name == name) {
        return makeInteger(entry.bits, entry.isSigned);
      }
    }
    if (name == "size_t" || name == "uintptr_t") {
      return makeInteger(profile_.sizeBits(), false);
    }
    if (name == "ptrdiff_t" || name == "ssize_t" || name == "intptr_t") {
      return makeInteger(profile_.sizeBits(), true);
    }
    return std::nullopt;
  }

  // `restrict` never changes what a caller passes, so the reader consumes it
  // like `volatile`; the common compiler spellings survive preprocessing.
  [[nodiscard]] static bool restrictSpelling(std::string_view word) {
    return word == "restrict" || word == "__restrict" || word == "__restrict__";
  }

  [[nodiscard]] bool ignorableQualifier() const {
    return check("volatile") || restrictSpelling(peek().text);
  }

  // Parses a base type. Returns 0 when the tokens do not begin a type.
  [[nodiscard]] NativeTypeId parseBaseType(bool &sawConst) {
    while (check("const") || ignorableQualifier()) {
      sawConst = sawConst || take().text == "const";
    }
    const Token first = peek();
    if (first.kind != TokenKind::Identifier) {
      return 0;
    }
    if (first.text == "struct" || first.text == "union") {
      const bool isUnion = first.text == "union";
      take();
      if (peek().kind != TokenKind::Identifier) {
        return makeUnsupported(first.text,
                               "an anonymous record type in this position is "
                               "outside the bounded surface");
      }
      const std::string tag = take().text;
      return makeReference(ensureRecordTag(tag, isUnion, first.origin));
    }
    if (first.text == "enum") {
      take();
      if (peek().kind != TokenKind::Identifier) {
        return makeUnsupported("enum",
                               "an anonymous enum type in this position is "
                               "outside the bounded surface");
      }
      const std::string tag = take().text;
      const NativeDeclarationId id = lookup(tags_, tag);
      if (id == 0 || result_.model.findDeclaration(id)->kind !=
                         NativeDeclarationKind::Enum) {
        return makeUnsupported("enum " + tag,
                               "enum '" + tag + "' is not declared");
      }
      return makeReference(id);
    }
    if (first.text == "void" || first.text == "char" || first.text == "int" ||
        first.text == "short" || first.text == "long" ||
        first.text == "signed" || first.text == "unsigned" ||
        first.text == "float" || first.text == "double" ||
        first.text == "_Bool" || first.text == "bool") {
      bool isSigned = true;
      bool sawSign = false;
      bool sawChar = false;
      bool sawShort = false;
      int longCount = 0;
      bool sawVoid = false;
      bool sawFloat = false;
      bool sawDouble = false;
      bool sawBool = false;
      while (peek().kind == TokenKind::Identifier) {
        const std::string &word = peek().text;
        if (word == "signed") {
          sawSign = true;
          isSigned = true;
        } else if (word == "unsigned") {
          sawSign = true;
          isSigned = false;
        } else if (word == "char") {
          sawChar = true;
        } else if (word == "short") {
          sawShort = true;
        } else if (word == "long") {
          ++longCount;
        } else if (word == "int") {
          // Width comes from the accompanying modifiers.
        } else if (word == "void") {
          sawVoid = true;
        } else if (word == "float") {
          sawFloat = true;
        } else if (word == "double") {
          sawDouble = true;
        } else if (word == "_Bool" || word == "bool") {
          sawBool = true;
        } else if (word == "const" || word == "volatile" ||
                   restrictSpelling(word)) {
          sawConst = sawConst || word == "const";
        } else {
          break;
        }
        take();
      }
      NativeType type;
      if (sawVoid) {
        type.kind = NativeTypeKind::Void;
        return result_.model.addType(std::move(type));
      }
      if (sawBool) {
        type.kind = NativeTypeKind::Bool;
        return result_.model.addType(std::move(type));
      }
      if (sawDouble || sawFloat) {
        type.kind = NativeTypeKind::Floating;
        type.bitWidth = sawDouble ? 64 : 32;
        return result_.model.addType(std::move(type));
      }
      if (sawChar) {
        if (sawSign) {
          return makeInteger(8, isSigned);
        }
        type.kind = NativeTypeKind::Char;
        return result_.model.addType(std::move(type));
      }
      if (sawShort) {
        return makeInteger(16, isSigned);
      }
      if (longCount == 1) {
        return makeInteger(profile_.longBits(), isSigned);
      }
      if (longCount >= 2) {
        return makeInteger(profile_.longLongBits(), isSigned);
      }
      return makeInteger(profile_.intBits(), isSigned);
    }
    if (const std::optional<NativeTypeId> builtin =
            builtinTypedef(first.text)) {
      take();
      return *builtin;
    }
    if (const NativeDeclarationId named = lookup(names_, first.text);
        named != 0) {
      take();
      return makeReference(named);
    }
    return 0;
  }

  // Parses `base ('*' const?)*`.
  [[nodiscard]] NativeTypeId parseType() {
    bool sawConst = false;
    NativeTypeId base = parseBaseType(sawConst);
    if (base == 0) {
      return 0;
    }
    while (check("*")) {
      take();
      bool pointerConst = false;
      while (check("const") || ignorableQualifier()) {
        pointerConst = pointerConst || take().text == "const";
      }
      NativeType pointer;
      pointer.kind = NativeTypeKind::Pointer;
      pointer.pointee = base;
      pointer.constPointee = sawConst;
      base = result_.model.addType(std::move(pointer));
      sawConst = pointerConst;
    }
    return base;
  }

  [[nodiscard]] std::optional<std::int64_t> parseIntegerLiteral() {
    const bool negative = match("-");
    if (peek().kind != TokenKind::Number) {
      return std::nullopt;
    }
    std::string text = take().text;
    while (!text.empty()) {
      const char suffix = static_cast<char>(
          std::tolower(static_cast<unsigned char>(text.back())));
      if (suffix == 'u' || suffix == 'l') {
        text.pop_back();
      } else {
        break;
      }
    }
    try {
      std::size_t used = 0;
      const std::int64_t value = std::stoll(text, &used, 0);
      if (used != text.size()) {
        return std::nullopt;
      }
      return negative ? -value : value;
    } catch (...) {
      return std::nullopt;
    }
  }

  // -- Declarations ---------------------------------------------------------

  void parseTopLevel() {
    const Token first = peek();
    if (first.text == ";") {
      take();
      return;
    }
    if (first.text == "typedef") {
      parseTypedef();
      return;
    }
    if (first.text == "static" || first.text == "inline") {
      warn(first.origin,
           "'" + first.text +
               "' declaration is not part of the linkable C ABI surface and "
               "was skipped");
      resync();
      return;
    }
    if ((first.text == "struct" || first.text == "union") &&
        peek(1).kind == TokenKind::Identifier &&
        (peek(2).text == "{" || peek(2).text == ";")) {
      parseRecordDeclaration(first.text == "union");
      return;
    }
    if (first.text == "enum" && peek(1).kind == TokenKind::Identifier &&
        peek(2).text == "{") {
      take(); // enum
      const std::string tag = take().text;
      const NativeDeclarationId id = parseEnumBody(tag, first.origin);
      if (id != 0) {
        tags_.emplace_back(tag, id);
      }
      expectSemicolon(first.origin, "enum");
      return;
    }
    if (first.text == "enum" && peek(1).text == "{") {
      // `enum { ... };` — a bare anonymous enum is a pure constant list in C
      // too: it cannot be named in a later declaration.
      take(); // enum
      [[maybe_unused]] const NativeDeclarationId id =
          parseEnumBody(std::string(), first.origin);
      expectSemicolon(first.origin, "enum");
      return;
    }
    match("extern");
    parsePrototypeOrVariable(first.origin);
  }

  void parseRecordDeclaration(bool isUnion) {
    const SourceOrigin origin = take().origin; // struct / union
    const std::string tag = take().text;
    const NativeDeclarationId id = ensureRecordTag(tag, isUnion, origin);
    if (match(";")) {
      return; // Forward declaration.
    }
    parseRecordBody(id);
    expectSemicolon(origin, "record '" + tag + "'");
  }

  void parseRecordBody(NativeDeclarationId id) {
    take(); // {
    std::vector<NativeRecordField> fields;
    std::string failureReason;
    while (!atEnd() && !check("}")) {
      const SourceOrigin fieldOrigin = peek().origin;
      const auto fail = [&](std::string reason) {
        if (failureReason.empty()) {
          failureReason = std::move(reason);
        }
        skipPastFieldSemicolon();
      };
      const NativeTypeId type = parseType();
      if (type == 0) {
        fail("field type at line " + std::to_string(fieldOrigin.line) +
             " is outside the bounded surface");
        continue;
      }
      if (peek().kind != TokenKind::Identifier) {
        fail("field declarator at line " + std::to_string(fieldOrigin.line) +
             " is outside the bounded surface");
        continue;
      }
      std::string name = take().text;
      NativeTypeId fieldType = type;
      if (check("[")) {
        std::vector<std::uint64_t> extents;
        bool extentsValid = true;
        while (match("[")) {
          const std::optional<std::int64_t> count = parseIntegerLiteral();
          if (!count || *count <= 0 || !match("]")) {
            extentsValid = false;
            break;
          }
          extents.push_back(static_cast<std::uint64_t>(*count));
        }
        if (!extentsValid || extents.empty()) {
          fail("array field '" + name +
               "' does not use a positive integer literal extent");
          continue;
        }
        // `int a[2][3]` is array-of-2 of array-of-3 of int; build the
        // innermost array first so nesting mirrors the declarator.
        for (std::size_t index = extents.size(); index > 0; --index) {
          NativeType array;
          array.kind = NativeTypeKind::Array;
          array.element = fieldType;
          array.count = extents[index - 1];
          fieldType = result_.model.addType(std::move(array));
        }
      }
      if (check(":")) {
        fail("bit-field '" + name + "' has no GTI representation");
        continue;
      }
      if (check(",")) {
        fail("multiple declarators per field are outside the bounded "
             "surface");
        continue;
      }
      if (!match(";")) {
        fail("field '" + name + "' is missing ';'");
        continue;
      }
      fields.push_back({std::move(name), fieldType, fieldOrigin});
    }
    match("}");
    NativeDeclaration *record = result_.model.findDeclaration(id);
    record->complete = true;
    if (failureReason.empty()) {
      record->fields = std::move(fields);
    } else {
      // The planner degrades a complete-but-inexpressible record to an
      // opaque handle carrying this reason.
      record->fields.clear();
      record->layoutReason = std::move(failureReason);
    }
  }

  void skipPastFieldSemicolon() {
    while (!atEnd() && !check(";") && !check("}")) {
      take();
    }
    match(";");
  }

  // Returns the created declaration id, or 0 when the enum was skipped.
  NativeDeclarationId parseEnumBody(const std::string &name,
                                    SourceOrigin origin) {
    take(); // {
    NativeDeclaration declaration;
    declaration.kind = NativeDeclarationKind::Enum;
    declaration.name = name;
    declaration.scope = rootScope_;
    declaration.origin = origin;
    std::int64_t next = 0;
    bool expressible = true;
    while (!atEnd() && !check("}")) {
      if (peek().kind != TokenKind::Identifier) {
        expressible = false;
        break;
      }
      NativeEnumerator enumerator;
      enumerator.name = take().text;
      if (match("=")) {
        const std::optional<std::int64_t> value = parseIntegerLiteral();
        if (!value) {
          expressible = false;
          break;
        }
        enumerator.value = *value;
      } else {
        enumerator.value = next;
      }
      next = enumerator.value + 1;
      declaration.enumerators.push_back(std::move(enumerator));
      if (!match(",")) {
        break;
      }
    }
    if (!expressible || !check("}")) {
      warn(origin,
           (name.empty() ? std::string("an anonymous enum")
                         : "enum '" + name + "'") +
               " uses enumerator expressions outside the bounded surface "
               "and was skipped");
      while (!atEnd() && !check("}")) {
        take();
      }
      match("}");
      return 0;
    }
    take(); // }
    const NativeDeclarationId id =
        result_.model.addDeclaration(std::move(declaration));
    result_.model.order.push_back(id);
    return id;
  }

  void parseTypedef() {
    const SourceOrigin origin = take().origin; // typedef
    if (check("struct") || check("union")) {
      parseRecordTypedef(origin, check("union"));
      return;
    }
    if (check("enum")) {
      parseEnumTypedef(origin);
      return;
    }
    const std::size_t rollback = position_;
    const NativeTypeId aliased = parseType();
    // typedef ret (*Name)(params);
    if (aliased != 0 && check("(") && peek(1).text == "*" &&
        peek(2).kind == TokenKind::Identifier && peek(3).text == ")") {
      take(); // (
      take(); // *
      const std::string name = take().text;
      take(); // )
      NativeType function;
      function.kind = NativeTypeKind::Function;
      function.result = aliased;
      std::vector<NativeParameter> names;
      if (!parseParameterList(function, names)) {
        warn(origin, "callback typedef '" + name +
                         "' has a parameter list outside the bounded "
                         "surface and was skipped");
        resync();
        return;
      }
      addTypedef(name, result_.model.addType(std::move(function)), origin);
      expectSemicolon(origin, "typedef");
      return;
    }
    // typedef <type> Name;
    if (aliased != 0 && peek().kind == TokenKind::Identifier &&
        peek(1).text == ";") {
      const std::string name = take().text;
      const NativeDeclaration *target = aliasedRecordDeclaration(aliased);
      if (target != nullptr && target->name == name) {
        // `typedef struct Foo Foo;` — the bare name answers for the record.
        names_.emplace_back(name, aliasTargetDeclaration(aliased));
      } else {
        addTypedef(name, aliased, origin);
      }
      take(); // ;
      return;
    }
    position_ = rollback;
    warn(origin, "typedef form is outside the bounded surface and was "
                 "skipped");
    resync();
  }

  [[nodiscard]] NativeDeclarationId
  aliasTargetDeclaration(NativeTypeId type) const {
    const NativeType *node = result_.model.findType(type);
    return node != nullptr && (node->kind == NativeTypeKind::Record ||
                               node->kind == NativeTypeKind::Enum)
               ? node->declaration
               : 0;
  }

  [[nodiscard]] const NativeDeclaration *
  aliasedRecordDeclaration(NativeTypeId type) const {
    return result_.model.findDeclaration(aliasTargetDeclaration(type));
  }

  void addTypedef(const std::string &name, NativeTypeId aliased,
                  SourceOrigin origin) {
    NativeDeclaration alias;
    alias.kind = NativeDeclarationKind::Typedef;
    alias.name = name;
    alias.scope = rootScope_;
    alias.origin = origin;
    alias.aliased = aliased;
    const NativeDeclarationId id =
        result_.model.addDeclaration(std::move(alias));
    result_.model.order.push_back(id);
    names_.emplace_back(name, id);
  }

  void parseRecordTypedef(SourceOrigin origin, bool isUnion) {
    take(); // struct / union
    std::string tag;
    if (peek().kind == TokenKind::Identifier) {
      tag = take().text;
    }
    if (check("{")) {
      NativeDeclarationId id = 0;
      if (tag.empty()) {
        // `typedef struct { ... } Name;` — the record takes the typedef
        // name once it is known.
        NativeDeclaration record;
        record.kind = NativeDeclarationKind::Record;
        record.scope = rootScope_;
        record.origin = origin;
        record.recordIsUnion = isUnion;
        id = result_.model.addDeclaration(std::move(record));
        result_.model.order.push_back(id);
      } else {
        id = ensureRecordTag(tag, isUnion, origin);
      }
      parseRecordBody(id);
      if (peek().kind != TokenKind::Identifier) {
        warn(origin, "typedef record is missing its name and was skipped");
        resync();
        return;
      }
      const std::string name = take().text;
      NativeDeclaration *record = result_.model.findDeclaration(id);
      if (record->name.empty()) {
        record->name = name;
        tags_.emplace_back(name, id);
        names_.emplace_back(name, id);
      } else if (record->name == name) {
        names_.emplace_back(name, id);
      } else {
        addTypedef(name, makeReference(id), origin);
      }
      expectSemicolon(origin, "typedef");
      return;
    }
    if (tag.empty() || peek().kind != TokenKind::Identifier) {
      warn(origin, "typedef record reference is outside the bounded surface "
                   "and was skipped");
      resync();
      return;
    }
    const NativeDeclarationId id = ensureRecordTag(tag, isUnion, origin);
    const std::string name = take().text;
    if (name == tag) {
      names_.emplace_back(name, id);
    } else {
      addTypedef(name, makeReference(id), origin);
    }
    expectSemicolon(origin, "typedef");
  }

  void parseEnumTypedef(SourceOrigin origin) {
    take(); // enum
    std::string tag;
    if (peek().kind == TokenKind::Identifier) {
      tag = take().text;
    }
    if (!check("{")) {
      warn(origin, "typedef of an enum reference is outside the bounded "
                   "surface and was skipped");
      resync();
      return;
    }
    const NativeDeclarationId id = parseEnumBody(tag, origin);
    if (peek().kind != TokenKind::Identifier) {
      warn(origin, "typedef enum is missing its name and was skipped");
      resync();
      return;
    }
    const std::string name = take().text;
    if (id != 0) {
      NativeDeclaration *declaration = result_.model.findDeclaration(id);
      declaration->name = name;
      if (!tag.empty()) {
        tags_.emplace_back(tag, id);
      }
      names_.emplace_back(name, id);
    }
    expectSemicolon(origin, "typedef");
  }

  void expectSemicolon(SourceOrigin origin, std::string_view what) {
    if (!match(";")) {
      warn(origin, "expected ';' after " + std::string(what));
      resync();
    }
  }

  // Parses `(params)` into `function`; false when structurally unreadable.
  bool parseParameterList(NativeType &function,
                          std::vector<NativeParameter> &names) {
    if (!match("(")) {
      return false;
    }
    if (check("void") && peek(1).text == ")") {
      take();
      take();
      return true;
    }
    if (match(")")) {
      return true;
    }
    while (true) {
      if (check("...")) {
        take();
        function.variadic = true;
        break;
      }
      const SourceOrigin parameterOrigin = peek().origin;
      NativeTypeId type = parseType();
      if (type == 0) {
        type = makeUnsupported(peek().text,
                               "parameter type at line " +
                                   std::to_string(parameterOrigin.line) +
                                   " is outside the bounded surface");
        while (!atEnd() && !check(",") && !check(")")) {
          take();
        }
        function.parameters.push_back(type);
        names.push_back({});
        if (match(",")) {
          continue;
        }
        break;
      }
      std::string name;
      if (peek().kind == TokenKind::Identifier) {
        name = take().text;
      }
      // C array parameters decay to pointers.
      if (check("[")) {
        while (!atEnd() && !check("]")) {
          take();
        }
        match("]");
        NativeType pointer;
        pointer.kind = NativeTypeKind::Pointer;
        pointer.pointee = type;
        type = result_.model.addType(std::move(pointer));
      }
      function.parameters.push_back(type);
      names.push_back({std::move(name), type});
      if (!match(",")) {
        break;
      }
    }
    return match(")");
  }

  void parsePrototypeOrVariable(SourceOrigin origin) {
    const NativeTypeId resultType = parseType();
    if (resultType == 0) {
      warn(peek().origin,
           "declaration is outside the bounded surface and was skipped");
      resync();
      return;
    }
    if (peek().kind != TokenKind::Identifier) {
      warn(peek().origin, "expected a declarator name");
      resync();
      return;
    }
    const std::string name = take().text;
    if (!check("(")) {
      warn(origin, "native variable '" + name +
                       "' is not part of the GTI C ABI and was skipped");
      resync();
      return;
    }
    NativeType function;
    function.kind = NativeTypeKind::Function;
    function.result = resultType;
    std::vector<NativeParameter> names;
    if (!parseParameterList(function, names)) {
      warn(origin, "function '" + name +
                       "' has a parameter list outside the bounded surface "
                       "and was skipped");
      resync();
      return;
    }
    if (check("{")) {
      warn(origin, "function definition '" + name +
                       "' is not importable and was skipped");
      resync();
      return;
    }
    NativeDeclaration declaration;
    declaration.kind = NativeDeclarationKind::Function;
    declaration.name = name;
    declaration.scope = rootScope_;
    declaration.origin = origin;
    declaration.type = result_.model.addType(std::move(function));
    declaration.parameterNames = std::move(names);
    const NativeDeclarationId id =
        result_.model.addDeclaration(std::move(declaration));
    result_.model.order.push_back(id);
    expectSemicolon(origin, "function declaration");
  }
};

} // namespace

BoundedCReader::BoundedCReader(CTargetProfile profile) : profile_(profile) {}

ReadResult BoundedCReader::read(std::string_view headerText) {
  ReadResult result;
  PreprocessResult preprocessed = preprocess(headerText);
  result.diagnostics = std::move(preprocessed.diagnostics);
  Parser parser(lex(preprocessed.text), profile_, result);
  parser.run();
  return result;
}

} // namespace lang::bindgen
