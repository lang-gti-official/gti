#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace lang {

enum class ReferenceAlignment {
  Left,
  Right,
  Middle,
};

enum class BraceBreakingStyle {
  Attach,
  Allman,
};

// Where a trailing `requires` clause is placed relative to the declaration
// it constrains.
enum class RequiresClausePosition {
  // Start the clause on its own line, indented one level past the
  // declaration. This is the style the shipped standard library and examples
  // are written in.
  OwnLine,
  // Keep the clause on the declaration line.
  SingleLine,
};

enum class SpaceBeforeParensStyle {
  Never,
  ControlStatements,
  Always,
};

struct FormatOptions {
  std::size_t indentWidth = 2;
  bool insertSpaces = true;
  ReferenceAlignment referenceAlignment = ReferenceAlignment::Middle;
  BraceBreakingStyle breakBeforeBraces = BraceBreakingStyle::Attach;
  RequiresClausePosition requiresClausePosition =
      RequiresClausePosition::OwnLine;
  SpaceBeforeParensStyle spaceBeforeParens =
      SpaceBeforeParensStyle::ControlStatements;
  bool indentCaseLabels = false;
  std::optional<int> accessModifierOffset;
  std::size_t maxEmptyLinesToKeep = 1;
  std::size_t spacesBeforeTrailingComments = 1;
  bool spaceBeforeAssignmentOperators = true;
  bool disableFormat = false;
};

class Formatter {
public:
  explicit Formatter(FormatOptions options = {}) : options(options) {
    if (this->options.indentWidth == 0) {
      this->options.indentWidth = 2;
    }
  }

  std::string format(std::string_view source) const {
    if (options.disableFormat) {
      return std::string(source);
    }

    const std::vector<Lexeme> lexemes = scan(source);
    const std::unordered_set<std::string> declaredTypes =
        collectDeclaredTypes(lexemes);
    State state(options);

    for (std::size_t index = 0; index < lexemes.size(); ++index) {
      const Lexeme &lexeme = lexemes[index];
      if (lexeme.kind == Kind::Newline) {
        ++state.sourceNewlines;
        if ((state.directiveLine || state.includeLine) &&
            state.lineHasContent) {
          state.newline();
        }
        if (state.sourceNewlines >= 2) {
          state.emptyLines(
              std::min(options.maxEmptyLinesToKeep, state.sourceNewlines - 1));
        }
        continue;
      }

      state.sourceNewlines = 0;
      const Lexeme *previous = previousSignificant(lexemes, index);
      const Lexeme *next = nextSignificant(lexemes, index);

      switch (lexeme.kind) {
      case Kind::Directive:
        if (state.lineHasContent) {
          state.newline();
        }
        state.appendUnindented(lexeme.text);
        state.directiveLine = true;
        state.includeLine = lexeme.text == "#include";
        break;
      case Kind::Comment:
        if (state.lineHasContent) {
          state.trailingCommentSpace();
        }
        state.append(lexeme.text);
        state.newline();
        break;
      case Kind::Word:
      case Kind::Number:
      case Kind::String:
        if (lexeme.kind == Kind::Word && next != nullptr &&
            next->kind == Kind::Colon &&
            (lexeme.text == "public" || lexeme.text == "private")) {
          state.appendAccessModifier(lexeme.text);
          break;
        }
        if (options.requiresClausePosition == RequiresClausePosition::OwnLine &&
            isTrailingRequiresClause(lexemes, index)) {
          state.beginRequiresClause();
          state.append(lexeme.text);
          break;
        }
        if (lexeme.kind == Kind::Word &&
            (lexeme.text == "case" ||
             (lexeme.text == "default" && next != nullptr &&
              next->kind == Kind::Colon))) {
          state.beginCaseLabel();
          if (options.indentCaseLabels) {
            state.append(lexeme.text);
          } else {
            state.appendOutdented(lexeme.text);
          }
          break;
        }
        if (needsSpaceBeforeValue(previous)) {
          state.space();
        }
        state.append(lexeme.kind == Kind::Word && !state.includeLine
                         ? canonicalIntegerType(lexeme.text)
                         : lexeme.text);
        break;
      case Kind::LeftBrace: {
        state.endRequiresClause();
        const bool directInitializer = isDirectInitializerBrace(lexemes, index);
        const bool doBody = previous != nullptr &&
                            previous->kind == Kind::Word &&
                            previous->text == "do";
        if (state.initializerBraceDepth > 0 ||
            (previous != nullptr && previous->kind == Kind::Operator &&
             previous->text == "=") ||
            (previous != nullptr && previous->kind == Kind::LeftParen) ||
            (previous != nullptr && previous->kind == Kind::Word &&
             previous->text == "return") ||
            directInitializer) {
          if (directInitializer) {
            state.trimSpaces();
          }
          state.append("{");
          ++state.initializerBraceDepth;
          break;
        }
        if (next != nullptr && next->kind == Kind::RightBrace) {
          state.beforeBlockBrace();
          state.append("{}");
          while (index + 1 < lexemes.size() &&
                 lexemes[index + 1].kind != Kind::RightBrace) {
            ++index;
          }
          if (index + 1 < lexemes.size()) {
            ++index;
          }
          const Lexeme *afterBrace = nextSignificant(lexemes, index);
          if (afterBrace == nullptr ||
              (afterBrace->kind != Kind::Semicolon &&
               afterBrace->kind != Kind::Comma &&
               !(afterBrace->kind == Kind::Word &&
                 (afterBrace->text == "else" ||
                  (doBody && afterBrace->text == "while"))) &&
               afterBrace->kind != Kind::Comment)) {
            state.newline();
          }
          break;
        }
        state.beforeBlockBrace();
        state.append("{");
        state.newline();
        ++state.indentLevel;
        state.pushBlock(isSwitchBodyStart(lexemes, index), doBody);
        if (isEnumBodyStart(lexemes, index)) {
          ++state.enumBodyDepth;
        }
        break;
      }
      case Kind::RightBrace: {
        if (state.initializerBraceDepth > 0) {
          state.trimSpaces();
          state.append("}");
          --state.initializerBraceDepth;
          break;
        }
        const bool doBody = state.currentBlockIsDoBody();
        state.endBlock();
        if (state.indentLevel > 0) {
          --state.indentLevel;
        }
        if (state.enumBodyDepth > 0) {
          --state.enumBodyDepth;
        }
        if (state.lineHasContent) {
          state.newline();
        }
        state.append("}");
        if (next == nullptr ||
            (next->kind != Kind::Semicolon && next->kind != Kind::Comma &&
             !(next->kind == Kind::Word &&
               (next->text == "else" || (doBody && next->text == "while"))) &&
             next->kind != Kind::Comment) ||
            (options.breakBeforeBraces == BraceBreakingStyle::Allman &&
             next->kind == Kind::Word && next->text == "else")) {
          state.newline();
        }
        break;
      }
      case Kind::LeftParen:
        if (spaceBeforeParenthesis(lexemes, index)) {
          state.space();
        }
        state.append("(");
        ++state.parenthesisDepth;
        break;
      case Kind::RightParen:
        state.trimSpaces();
        state.append(")");
        if (state.parenthesisDepth > 0) {
          --state.parenthesisDepth;
        }
        break;
      case Kind::LeftBracket:
        if (isLambdaCaptureStart(lexemes, index) ||
            isStructuredBindingStart(lexemes, index)) {
          state.space();
        } else {
          state.trimSpaces();
        }
        state.append("[");
        break;
      case Kind::RightBracket:
        state.trimSpaces();
        state.append("]");
        break;
      case Kind::Comma:
        state.trimSpaces();
        state.append(",");
        if (state.enumBodyDepth > 0 && state.parenthesisDepth == 0) {
          state.newline();
        } else {
          state.space();
        }
        break;
      case Kind::Dot:
        if (lexeme.text == "->" && isLambdaReturnArrow(lexemes, index)) {
          state.binaryOperator(lexeme.text);
        } else {
          state.trimSpaces();
          state.append(lexeme.text);
        }
        break;
      case Kind::Ellipsis:
        state.trimSpaces();
        state.append("...");
        if (next != nullptr && next->kind == Kind::Word) {
          state.space();
        }
        break;
      case Kind::Scope:
        state.trimSpaces();
        state.append("::");
        break;
      case Kind::Colon:
        state.trimSpaces();
        if ((previous != nullptr && previous->kind == Kind::Word &&
             (previous->text == "public" || previous->text == "private")) ||
            isSwitchLabelColon(lexemes, index)) {
          state.append(":");
          state.newline();
          if (isSwitchLabelColon(lexemes, index)) {
            state.beginCaseBody();
          }
        } else {
          state.space();
          state.append(":");
          state.space();
        }
        break;
      case Kind::Semicolon:
        state.endRequiresClause();
        state.trimSpaces();
        state.append(";");
        if (state.parenthesisDepth == 0 &&
            (next == nullptr || next->kind != Kind::Comment)) {
          state.newline();
        } else {
          state.space();
        }
        break;
      case Kind::At:
        state.trimSpaces();
        state.append("@");
        break;
      case Kind::Less:
        if (state.includeLine) {
          state.space();
          state.append("<");
        } else if (previous != nullptr && previous->kind == Kind::Word &&
                   previous->text == "operator") {
          state.trimSpaces();
          state.append("<");
        } else if (state.templateDepth > 0 ||
                   isGenericAngleStart(lexemes, index, declaredTypes)) {
          state.trimSpaces();
          state.append("<");
          ++state.templateDepth;
        } else {
          state.binaryOperator("<");
        }
        break;
      case Kind::Greater:
        if (state.includeLine) {
          state.trimSpaces();
          state.append(">");
        } else if (previous != nullptr && previous->kind == Kind::Word &&
                   previous->text == "operator") {
          state.trimSpaces();
          state.append(">");
        } else if (state.templateDepth > 0) {
          state.trimSpaces();
          state.append(">");
          --state.templateDepth;
        } else {
          state.binaryOperator(">");
        }
        break;
      case Kind::ShiftLeft:
        state.binaryOperator("<<");
        break;
      case Kind::ShiftRight:
        if (state.templateDepth >= 2) {
          state.trimSpaces();
          state.append(">>");
          state.templateDepth -= 2;
        } else if (state.templateDepth == 1) {
          state.trimSpaces();
          state.append(">");
          --state.templateDepth;
          state.binaryOperator(">");
        } else {
          state.binaryOperator(">>");
        }
        break;
      case Kind::Operator:
        if (state.includeLine && lexeme.text == "/") {
          state.trimSpaces();
          state.append("/");
        } else if (lexeme.text == ">=" && state.templateDepth > 0) {
          state.trimSpaces();
          state.append(">");
          --state.templateDepth;
          state.binaryOperator("=");
        } else if ((lexeme.text == "&" || lexeme.text == "&&") &&
                   !isConceptGenericAngle(lexemes, index) &&
                   isReferenceDeclarator(lexemes, index, declaredTypes)) {
          state.referenceMarker(lexeme.text);
        } else if (lexeme.text == "*" &&
                   isPointerDeclarator(lexemes, index, declaredTypes)) {
          state.pointerMarker();
        } else if (previous != nullptr && previous->kind == Kind::Word &&
                   previous->text == "operator") {
          state.trimSpaces();
          state.append(lexeme.text);
        } else if (lexeme.text == "!" || lexeme.text == "~" ||
                   ((lexeme.text == "*" || lexeme.text == "&") &&
                    isUnaryContext(previous)) ||
                   ((lexeme.text == "+" || lexeme.text == "-") &&
                    isUnaryContext(previous))) {
          if (previous != nullptr && previous->kind == Kind::Word &&
              previous->text == "return") {
            state.space();
          }
          state.append(lexeme.text);
        } else if (lexeme.text == "++" || lexeme.text == "--") {
          if (canEndExpression(previous)) {
            state.trimSpaces();
          }
          state.append(lexeme.text);
        } else if (isAssignmentOperator(lexeme.text)) {
          state.assignmentOperator(lexeme.text);
        } else {
          state.binaryOperator(lexeme.text);
        }
        break;
      case Kind::Newline:
        break;
      }
    }

    state.trimSpaces();
    while (state.output.ends_with("\n\n")) {
      state.output.pop_back();
    }
    if (!state.output.empty() && state.output.back() != '\n') {
      state.output.push_back('\n');
    }
    return state.output;
  }

private:
  enum class Kind {
    Word,
    Number,
    String,
    Comment,
    Directive,
    Newline,
    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    Comma,
    Dot,
    Ellipsis,
    Semicolon,
    Scope,
    Colon,
    At,
    Less,
    Greater,
    ShiftLeft,
    ShiftRight,
    Operator,
  };

  struct Lexeme {
    Kind kind;
    std::string text;
  };

  struct State {
    struct Block {
      bool switchBody = false;
      bool doBody = false;
      bool caseBodyIndented = false;
    };

    explicit State(const FormatOptions &options) : options(options) {}

    void writeIndentColumns(std::size_t columns) {
      if (options.insertSpaces) {
        output.append(columns, ' ');
        return;
      }

      output.append(columns / options.indentWidth, '\t');
      output.append(columns % options.indentWidth, ' ');
    }

    void writeIndent() {
      if (!atLineStart) {
        return;
      }
      writeIndentColumns(indentLevel * options.indentWidth);
      atLineStart = false;
    }

    void append(std::string_view text) {
      writeIndent();
      output.append(text);
      lineHasContent = true;
    }

    void appendUnindented(std::string_view text) {
      atLineStart = false;
      output.append(text);
      lineHasContent = true;
    }

    void appendOutdented(std::string_view text) {
      if (atLineStart) {
        const std::size_t level = indentLevel == 0 ? 0 : indentLevel - 1;
        writeIndentColumns(level * options.indentWidth);
        atLineStart = false;
      }
      output.append(text);
      lineHasContent = true;
    }

    void appendAccessModifier(std::string_view text) {
      if (atLineStart) {
        const std::size_t baseColumns = indentLevel * options.indentWidth;
        std::size_t columns =
            indentLevel == 0 ? 0 : baseColumns - options.indentWidth;
        if (options.accessModifierOffset) {
          const long long adjusted =
              static_cast<long long>(baseColumns) +
              static_cast<long long>(*options.accessModifierOffset);
          columns = adjusted <= 0 ? 0 : static_cast<std::size_t>(adjusted);
        }
        writeIndentColumns(columns);
        atLineStart = false;
      }
      output.append(text);
      lineHasContent = true;
    }

    void trimSpaces() {
      while (!output.empty() &&
             (output.back() == ' ' || output.back() == '\t')) {
        output.pop_back();
      }
    }

    void space() {
      if (lineHasContent && !output.empty() && output.back() != ' ' &&
          output.back() != '\t' && output.back() != '\n') {
        output.push_back(' ');
      }
    }

    void newline() {
      trimSpaces();
      if (output.empty() || output.back() != '\n') {
        output.push_back('\n');
      }
      atLineStart = true;
      lineHasContent = false;
      directiveLine = false;
      includeLine = false;
    }

    void emptyLines(std::size_t count) {
      newline();
      std::size_t existingNewlines = 0;
      for (std::size_t index = output.size();
           index > 0 && output[index - 1] == '\n'; --index) {
        ++existingNewlines;
      }
      const std::size_t requestedNewlines = count + 1;
      while (existingNewlines < requestedNewlines) {
        output.push_back('\n');
        ++existingNewlines;
      }
    }

    // A trailing requires clause is emitted on its own line one level past
    // the declaration. endRequiresClause releases that level before the
    // declaration's brace or semicolon is written, so the body indents from
    // the declaration rather than from the clause.
    void beginRequiresClause() {
      newline();
      ++indentLevel;
      requiresClauseOpen = true;
    }

    void endRequiresClause() {
      if (!requiresClauseOpen) {
        return;
      }
      requiresClauseOpen = false;
      if (indentLevel > 0) {
        --indentLevel;
      }
    }

    void beforeBlockBrace() {
      if (options.breakBeforeBraces == BraceBreakingStyle::Allman) {
        newline();
      } else {
        space();
      }
    }

    void trailingCommentSpace() {
      trimSpaces();
      output.append(options.spacesBeforeTrailingComments, ' ');
    }

    void binaryOperator(std::string_view text) {
      trimSpaces();
      space();
      append(text);
      space();
    }

    void assignmentOperator(std::string_view text) {
      trimSpaces();
      if (options.spaceBeforeAssignmentOperators) {
        space();
      }
      append(text);
      space();
    }

    void referenceMarker(std::string_view text) {
      trimSpaces();
      if (options.referenceAlignment != ReferenceAlignment::Left) {
        space();
      }
      append(text);
      if (options.referenceAlignment != ReferenceAlignment::Right) {
        space();
      }
    }

    void pointerMarker() {
      trimSpaces();
      append("*");
      space();
    }

    void pushBlock(bool switchBody, bool doBody) {
      blocks.push_back({.switchBody = switchBody, .doBody = doBody});
    }

    [[nodiscard]] bool currentBlockIsDoBody() const {
      return !blocks.empty() && blocks.back().doBody;
    }

    void beginCaseLabel() {
      if (!blocks.empty() && blocks.back().switchBody &&
          blocks.back().caseBodyIndented) {
        if (indentLevel > 0) {
          --indentLevel;
        }
        blocks.back().caseBodyIndented = false;
      }
    }

    void beginCaseBody() {
      if (!options.indentCaseLabels || blocks.empty() ||
          !blocks.back().switchBody || blocks.back().caseBodyIndented) {
        return;
      }
      ++indentLevel;
      blocks.back().caseBodyIndented = true;
    }

    void endBlock() {
      if (blocks.empty()) {
        return;
      }
      if (blocks.back().caseBodyIndented && indentLevel > 0) {
        --indentLevel;
      }
      blocks.pop_back();
    }

    FormatOptions options;
    std::string output;
    std::size_t indentLevel = 0;
    std::size_t parenthesisDepth = 0;
    std::size_t initializerBraceDepth = 0;
    std::size_t enumBodyDepth = 0;
    std::size_t templateDepth = 0;
    std::size_t sourceNewlines = 0;
    std::vector<Block> blocks;
    bool atLineStart = true;
    bool lineHasContent = false;
    bool directiveLine = false;
    bool includeLine = false;
    bool requiresClauseOpen = false;
  };

  static bool isIdentifierStart(char value) {
    const unsigned char character = static_cast<unsigned char>(value);
    return std::isalpha(character) != 0 || value == '_';
  }

  static bool isIdentifierPart(char value) {
    const unsigned char character = static_cast<unsigned char>(value);
    return std::isalnum(character) != 0 || value == '_';
  }

  static bool isBuiltinType(std::string_view word) {
    return word == "auto" || word == "bool" || word == "expected" ||
           word == "float" || word == "double" || word == "int" ||
           word == "int8_t" || word == "int16_t" || word == "int32_t" ||
           word == "int64_t" || word == "int8" || word == "int16" ||
           word == "int32" || word == "int64" || word == "nullptr_t" ||
           word == "char" || word == "uint" || word == "uint8_t" ||
           word == "uint16_t" || word == "uint32_t" || word == "uint64_t" ||
           word == "uint8" || word == "uint16" || word == "uint32" ||
           word == "uint64" || word == "void";
  }

  static std::string_view canonicalIntegerType(std::string_view word) {
    if (word == "int8") {
      return "int8_t";
    }
    if (word == "int16") {
      return "int16_t";
    }
    if (word == "int32") {
      return "int32_t";
    }
    if (word == "int64") {
      return "int64_t";
    }
    if (word == "uint8") {
      return "uint8_t";
    }
    if (word == "uint16") {
      return "uint16_t";
    }
    if (word == "uint32") {
      return "uint32_t";
    }
    if (word == "uint64") {
      return "uint64_t";
    }
    return word;
  }

  static std::unordered_set<std::string>
  collectDeclaredTypes(const std::vector<Lexeme> &lexemes) {
    std::unordered_set<std::string> result;
    for (std::size_t index = 0; index < lexemes.size(); ++index) {
      if (lexemes[index].kind != Kind::Word ||
          (lexemes[index].text != "class" && lexemes[index].text != "concept" &&
           lexemes[index].text != "interface" &&
           lexemes[index].text != "struct" && lexemes[index].text != "enum" &&
           lexemes[index].text != "using")) {
        continue;
      }
      const Lexeme *name = nextSignificant(lexemes, index);
      if (lexemes[index].text == "enum" && name != nullptr &&
          name->kind == Kind::Word && name->text == "class") {
        name = nextSignificant(lexemes,
                               static_cast<std::size_t>(name - lexemes.data()));
      }
      if (name != nullptr && name->kind == Kind::Word) {
        result.insert(name->text);
        const std::size_t nameIndex =
            static_cast<std::size_t>(name - lexemes.data());
        const Lexeme *generic = nextSyntaxLexeme(lexemes, nameIndex);
        if (generic == nullptr || generic->kind != Kind::Less) {
          continue;
        }

        std::size_t depth = 1;
        std::size_t segmentStart =
            static_cast<std::size_t>(generic - lexemes.data()) + 1;
        for (std::size_t cursor = segmentStart; cursor < lexemes.size();
             ++cursor) {
          if (lexemes[cursor].kind == Kind::Less) {
            ++depth;
            continue;
          }
          const bool segmentEnd =
              depth == 1 && lexemes[cursor].kind == Kind::Comma;
          const bool genericEnd =
              depth == 1 && lexemes[cursor].kind == Kind::Greater;
          if (!segmentEnd && !genericEnd) {
            if (lexemes[cursor].kind == Kind::Greater && depth > 1) {
              --depth;
            }
            continue;
          }

          const Lexeme *firstWord = nullptr;
          const Lexeme *parameterName = nullptr;
          std::size_t nestedDepth = 0;
          for (std::size_t candidate = segmentStart; candidate < cursor;
               ++candidate) {
            if (lexemes[candidate].kind == Kind::Less) {
              ++nestedDepth;
            } else if (lexemes[candidate].kind == Kind::Greater &&
                       nestedDepth > 0) {
              --nestedDepth;
            } else if (nestedDepth == 0 &&
                       lexemes[candidate].kind == Kind::Word) {
              if (firstWord == nullptr) {
                firstWord = &lexemes[candidate];
              }
              parameterName = &lexemes[candidate];
            }
          }
          if (firstWord != nullptr && parameterName != nullptr &&
              firstWord->text != "uint64_t" && firstWord->text != "uint64") {
            result.insert(parameterName->text);
          }
          if (genericEnd) {
            break;
          }
          segmentStart = cursor + 1;
        }
      }
    }
    return result;
  }

  static bool isEnumBodyStart(const std::vector<Lexeme> &lexemes,
                              std::size_t brace) {
    while (brace > 0) {
      --brace;
      const Lexeme &candidate = lexemes[brace];
      if (candidate.kind == Kind::LeftBrace ||
          candidate.kind == Kind::RightBrace ||
          candidate.kind == Kind::Semicolon) {
        return false;
      }
      if (candidate.kind == Kind::Word && candidate.text == "enum") {
        const Lexeme *next = nextSignificant(lexemes, brace);
        return next != nullptr && next->kind == Kind::Word &&
               next->text == "class";
      }
    }
    return false;
  }

  static bool isDirectInitializerBrace(const std::vector<Lexeme> &lexemes,
                                       std::size_t brace) {
    for (std::size_t cursor = brace; cursor > 0;) {
      --cursor;
      const Lexeme &candidate = lexemes[cursor];
      if (candidate.kind == Kind::LeftBrace ||
          candidate.kind == Kind::RightBrace ||
          candidate.kind == Kind::Semicolon) {
        break;
      }
      if (candidate.kind == Kind::Word &&
          (candidate.text == "class" || candidate.text == "interface" ||
           candidate.text == "struct")) {
        return false;
      }
    }
    const Lexeme *name = previousSignificant(lexemes, brace);
    if (name == nullptr || name->kind != Kind::Word) {
      return false;
    }

    const std::size_t nameIndex =
        static_cast<std::size_t>(name - lexemes.data());
    const Lexeme *typeEnd = previousSignificant(lexemes, nameIndex);
    if (typeEnd == nullptr) {
      return false;
    }
    if (typeEnd->kind == Kind::Word &&
        (typeEnd->text == "class" || typeEnd->text == "interface" ||
         typeEnd->text == "struct" || typeEnd->text == "enum" ||
         typeEnd->text == "namespace" || typeEnd->text == "return" ||
         typeEnd->text == "else" || typeEnd->text == "case" ||
         typeEnd->text == "default")) {
      return false;
    }
    const bool plausibleTypeEnd =
        typeEnd->kind == Kind::Word || typeEnd->kind == Kind::Greater ||
        typeEnd->kind == Kind::ShiftRight ||
        typeEnd->kind == Kind::RightBracket ||
        (typeEnd->kind == Kind::Operator &&
         (typeEnd->text == "*" || typeEnd->text == "&" ||
          typeEnd->text == "&&"));
    if (!plausibleTypeEnd) {
      return false;
    }

    std::size_t depth = 0;
    for (std::size_t index = brace; index < lexemes.size(); ++index) {
      if (lexemes[index].kind == Kind::LeftBrace) {
        ++depth;
      } else if (lexemes[index].kind == Kind::RightBrace && --depth == 0) {
        const Lexeme *after = nextSignificant(lexemes, index);
        return after != nullptr && after->kind == Kind::Semicolon;
      }
    }
    return false;
  }

  static bool
  isKnownTypeWord(const std::vector<Lexeme> &lexemes, std::size_t index,
                  const std::unordered_set<std::string> &declaredTypes) {
    const Lexeme &word = lexemes[index];
    if (word.kind != Kind::Word) {
      return false;
    }
    if (isBuiltinType(word.text) || declaredTypes.contains(word.text)) {
      return true;
    }
    return index > 0 && lexemes[index - 1].kind == Kind::Scope;
  }

  static bool typeEndsAt(const std::vector<Lexeme> &lexemes, std::size_t index,
                         const std::unordered_set<std::string> &declaredTypes) {
    if (lexemes[index].kind == Kind::Word) {
      return isKnownTypeWord(lexemes, index, declaredTypes);
    }
    if (lexemes[index].kind == Kind::RightBracket) {
      std::size_t depth = 1;
      while (index > 0) {
        --index;
        if (lexemes[index].kind == Kind::RightBracket) {
          ++depth;
        } else if (lexemes[index].kind == Kind::LeftBracket && --depth == 0) {
          const Lexeme *elementType = previousSignificant(lexemes, index);
          return elementType != nullptr &&
                 typeEndsAt(
                     lexemes,
                     static_cast<std::size_t>(elementType - lexemes.data()),
                     declaredTypes);
        }
      }
      return false;
    }
    if (lexemes[index].kind == Kind::Operator && lexemes[index].text == "*") {
      const Lexeme *pointee = previousSignificant(lexemes, index);
      return pointee != nullptr &&
             typeEndsAt(lexemes,
                        static_cast<std::size_t>(pointee - lexemes.data()),
                        declaredTypes);
    }
    if (lexemes[index].kind != Kind::Greater &&
        lexemes[index].kind != Kind::ShiftRight) {
      return false;
    }

    std::size_t depth =
        lexemes[index].kind == Kind::ShiftRight ? std::size_t{2} : 1;
    while (index > 0) {
      --index;
      if (lexemes[index].kind == Kind::Greater) {
        ++depth;
      } else if (lexemes[index].kind == Kind::ShiftRight) {
        depth += 2;
      } else if (lexemes[index].kind == Kind::Less) {
        if (--depth == 0) {
          const Lexeme *baseType = previousSignificant(lexemes, index);
          return baseType != nullptr &&
                 isKnownTypeWord(
                     lexemes,
                     static_cast<std::size_t>(baseType - lexemes.data()),
                     declaredTypes);
        }
      }
    }
    return false;
  }

  static bool
  isPointerDeclarator(const std::vector<Lexeme> &lexemes, std::size_t index,
                      const std::unordered_set<std::string> &declaredTypes) {
    const Lexeme *previous = previousSignificant(lexemes, index);
    const Lexeme *next = nextSignificant(lexemes, index);
    if (previous == nullptr || next == nullptr) {
      return false;
    }

    if (!typeEndsAt(lexemes,
                    static_cast<std::size_t>(previous - lexemes.data()),
                    declaredTypes)) {
      return false;
    }

    if (next->kind == Kind::Operator && next->text == "*") {
      return true;
    }
    if (next->kind != Kind::Word) {
      return next->kind == Kind::Comma || next->kind == Kind::Greater ||
             next->kind == Kind::ShiftRight || next->kind == Kind::RightParen ||
             next->kind == Kind::Semicolon;
    }

    const std::size_t nextIndex =
        static_cast<std::size_t>(next - lexemes.data());
    const Lexeme *afterName = nextSignificant(lexemes, nextIndex);
    if (afterName == nullptr) {
      return true;
    }
    return afterName->kind == Kind::Comma ||
           afterName->kind == Kind::LeftBracket ||
           afterName->kind == Kind::LeftParen ||
           afterName->kind == Kind::LeftBrace ||
           afterName->kind == Kind::RightParen ||
           afterName->kind == Kind::Semicolon ||
           (afterName->kind == Kind::Operator && afterName->text == "=");
  }

  static bool
  isReferenceDeclarator(const std::vector<Lexeme> &lexemes, std::size_t index,
                        const std::unordered_set<std::string> &declaredTypes) {
    const Lexeme *previous = previousSignificant(lexemes, index);
    const Lexeme *next = nextSignificant(lexemes, index);
    if (previous == nullptr || next == nullptr) {
      return false;
    }

    const std::size_t previousIndex =
        static_cast<std::size_t>(previous - lexemes.data());
    const bool typeBefore = typeEndsAt(lexemes, previousIndex, declaredTypes);
    if (!typeBefore) {
      return false;
    }

    if (next->kind != Kind::Word) {
      return next->kind == Kind::Comma || next->kind == Kind::Greater ||
             next->kind == Kind::ShiftRight || next->kind == Kind::RightParen ||
             next->kind == Kind::Semicolon;
    }

    if (next->text == "operator") {
      return true;
    }

    const std::size_t nextIndex =
        static_cast<std::size_t>(next - lexemes.data());
    const Lexeme *afterName = nextSignificant(lexemes, nextIndex);
    if (afterName == nullptr) {
      return true;
    }
    return afterName->kind == Kind::Comma ||
           afterName->kind == Kind::LeftBracket ||
           afterName->kind == Kind::LeftParen ||
           afterName->kind == Kind::RightParen ||
           afterName->kind == Kind::Semicolon ||
           (afterName->kind == Kind::Operator && afterName->text == "=");
  }

  static std::vector<Lexeme> scan(std::string_view source) {
    std::vector<Lexeme> result;
    std::size_t current = 0;

    const auto add = [&result](Kind kind, std::string_view text) {
      result.push_back({kind, std::string(text)});
    };

    while (current < source.size()) {
      const std::size_t start = current;
      const char character = source[current++];

      if (character == ' ' || character == '\t' || character == '\r') {
        continue;
      }
      if (character == '\n') {
        add(Kind::Newline, "\n");
        continue;
      }
      if (character == '/' && current < source.size() &&
          source[current] == '/') {
        ++current;
        while (current < source.size() && source[current] != '\n') {
          ++current;
        }
        add(Kind::Comment, source.substr(start, current - start));
        continue;
      }
      if (character == '"' || character == '\'') {
        const char delimiter = character;
        while (current < source.size()) {
          if (source[current] == '\\' && current + 1 < source.size()) {
            current += 2;
            continue;
          }
          if (source[current++] == delimiter) {
            break;
          }
        }
        add(Kind::String, source.substr(start, current - start));
        continue;
      }
      if (isIdentifierStart(character)) {
        while (current < source.size() && isIdentifierPart(source[current])) {
          ++current;
        }
        add(Kind::Word, source.substr(start, current - start));
        continue;
      }
      if (std::isdigit(static_cast<unsigned char>(character)) != 0) {
        if (character == '0' && current < source.size() &&
            (source[current] == 'x' || source[current] == 'X' ||
             source[current] == 'b' || source[current] == 'B')) {
          ++current;
          while (current < source.size() && isIdentifierPart(source[current])) {
            ++current;
          }
          add(Kind::Number, source.substr(start, current - start));
          continue;
        }
        while (current < source.size() &&
               std::isdigit(static_cast<unsigned char>(source[current])) != 0) {
          ++current;
        }
        if (current + 1 < source.size() && source[current] == '.' &&
            std::isdigit(static_cast<unsigned char>(source[current + 1])) !=
                0) {
          ++current;
          while (current < source.size() &&
                 std::isdigit(static_cast<unsigned char>(source[current])) !=
                     0) {
            ++current;
          }
          if (current < source.size() &&
              (source[current] == 'd' || source[current] == 'D')) {
            ++current;
          }
        }
        add(Kind::Number, source.substr(start, current - start));
        continue;
      }
      if (character == '#') {
        while (current < source.size() && isIdentifierPart(source[current])) {
          ++current;
        }
        add(Kind::Directive, source.substr(start, current - start));
        continue;
      }

      if (current + 1 < source.size() && source.substr(start, 3) == "...") {
        current += 2;
        add(Kind::Ellipsis, "...");
        continue;
      }

      if (current + 1 < source.size()) {
        const std::string_view triple = source.substr(start, 3);
        if (triple == "<<=" || triple == ">>=") {
          current += 2;
          add(Kind::Operator, triple);
          continue;
        }
      }

      if (current < source.size()) {
        const std::string_view pair = source.substr(start, 2);
        if (pair == "::") {
          ++current;
          add(Kind::Scope, pair);
          continue;
        }
        if (pair == "->") {
          ++current;
          add(Kind::Dot, pair);
          continue;
        }
        if (pair == "<<") {
          ++current;
          add(Kind::ShiftLeft, pair);
          continue;
        }
        if (pair == ">>") {
          ++current;
          add(Kind::ShiftRight, pair);
          continue;
        }
        if (pair == "==" || pair == "!=" || pair == "<=" || pair == ">=" ||
            pair == "++" || pair == "--" || pair == "+=" || pair == "-=" ||
            pair == "*=" || pair == "/=" || pair == "%=" || pair == "&=" ||
            pair == "|=" || pair == "^=" || pair == "&&" || pair == "||") {
          ++current;
          add(Kind::Operator, pair);
          continue;
        }
      }

      switch (character) {
      case '(':
        add(Kind::LeftParen, "(");
        break;
      case ')':
        add(Kind::RightParen, ")");
        break;
      case '{':
        add(Kind::LeftBrace, "{");
        break;
      case '}':
        add(Kind::RightBrace, "}");
        break;
      case '[':
        add(Kind::LeftBracket, "[");
        break;
      case ']':
        add(Kind::RightBracket, "]");
        break;
      case ',':
        add(Kind::Comma, ",");
        break;
      case '.':
        add(Kind::Dot, ".");
        break;
      case ';':
        add(Kind::Semicolon, ";");
        break;
      case ':':
        add(Kind::Colon, ":");
        break;
      case '@':
        add(Kind::At, "@");
        break;
      case '<':
        add(Kind::Less, "<");
        break;
      case '>':
        add(Kind::Greater, ">");
        break;
      default:
        add(Kind::Operator, source.substr(start, 1));
        break;
      }
    }
    return result;
  }

  static const Lexeme *previousSignificant(const std::vector<Lexeme> &lexemes,
                                           std::size_t index) {
    while (index > 0) {
      --index;
      if (lexemes[index].kind != Kind::Newline) {
        return &lexemes[index];
      }
    }
    return nullptr;
  }

  static const Lexeme *nextSignificant(const std::vector<Lexeme> &lexemes,
                                       std::size_t index) {
    for (++index; index < lexemes.size(); ++index) {
      if (lexemes[index].kind != Kind::Newline) {
        return &lexemes[index];
      }
    }
    return nullptr;
  }

  static const Lexeme *previousSyntaxLexeme(const std::vector<Lexeme> &lexemes,
                                            std::size_t index) {
    while (index > 0) {
      --index;
      if (lexemes[index].kind != Kind::Newline &&
          lexemes[index].kind != Kind::Comment) {
        return &lexemes[index];
      }
    }
    return nullptr;
  }

  static const Lexeme *nextSyntaxLexeme(const std::vector<Lexeme> &lexemes,
                                        std::size_t index) {
    for (++index; index < lexemes.size(); ++index) {
      if (lexemes[index].kind != Kind::Newline &&
          lexemes[index].kind != Kind::Comment) {
        return &lexemes[index];
      }
    }
    return nullptr;
  }

  static std::optional<std::size_t>
  matchingGenericClose(const std::vector<Lexeme> &lexemes, std::size_t left) {
    std::size_t depth = 0;
    for (std::size_t index = left; index < lexemes.size(); ++index) {
      if (lexemes[index].kind == Kind::Less) {
        ++depth;
      } else if (lexemes[index].kind == Kind::Greater) {
        if (--depth == 0) {
          return index;
        }
      } else if (lexemes[index].kind == Kind::ShiftRight) {
        for (std::size_t close = 0; close < 2; ++close) {
          if (--depth == 0) {
            return index;
          }
        }
      }
    }
    return std::nullopt;
  }

  static bool isConceptGenericAngle(const std::vector<Lexeme> &lexemes,
                                    std::size_t index) {
    for (std::size_t cursor = index; cursor > 0;) {
      --cursor;
      const Lexeme &candidate = lexemes[cursor];
      if (candidate.kind == Kind::Semicolon ||
          candidate.kind == Kind::LeftBrace ||
          candidate.kind == Kind::RightBrace) {
        return false;
      }
      if (candidate.kind == Kind::Word && candidate.text == "concept") {
        return true;
      }
    }
    return false;
  }

  // True when this `requires` introduces a trailing requirement on a
  // declaration, as opposed to an ordinary identifier that happens to be
  // spelled `requires` (the word is not a GTI keyword). The grammar places a
  // requires-clause directly after the parameter clause, optionally separated
  // by the receiver-mutability `mut`, and it is always followed by a concept
  // application.
  static bool isTrailingRequiresClause(const std::vector<Lexeme> &lexemes,
                                       std::size_t index) {
    const Lexeme &lexeme = lexemes[index];
    if (lexeme.kind != Kind::Word || lexeme.text != "requires") {
      return false;
    }
    const Lexeme *next = nextSyntaxLexeme(lexemes, index);
    if (next == nullptr || next->kind != Kind::Word) {
      return false;
    }
    const Lexeme *previous = previousSyntaxLexeme(lexemes, index);
    if (previous == nullptr) {
      return false;
    }
    if (previous->kind == Kind::Word && previous->text == "mut") {
      previous = previousSyntaxLexeme(
          lexemes, static_cast<std::size_t>(previous - lexemes.data()));
      if (previous == nullptr) {
        return false;
      }
    }
    return previous->kind == Kind::RightParen;
  }

  static bool isRequiresGenericAngle(const std::vector<Lexeme> &lexemes,
                                     std::size_t index) {
    for (std::size_t cursor = index; cursor > 0;) {
      --cursor;
      const Lexeme &candidate = lexemes[cursor];
      if (candidate.kind == Kind::Semicolon ||
          candidate.kind == Kind::LeftBrace ||
          candidate.kind == Kind::RightBrace) {
        return false;
      }
      if (candidate.kind == Kind::Word && candidate.text == "requires") {
        return true;
      }
    }
    return false;
  }

  static bool
  isGenericAngleStart(const std::vector<Lexeme> &lexemes, std::size_t index,
                      const std::unordered_set<std::string> &declaredTypes) {
    const Lexeme *previous = previousSyntaxLexeme(lexemes, index);
    if (previous == nullptr || previous->kind != Kind::Word) {
      return false;
    }
    if (previous->text == "expected") {
      return true;
    }
    if (isConceptGenericAngle(lexemes, index) ||
        isRequiresGenericAngle(lexemes, index)) {
      return true;
    }

    const std::optional<std::size_t> close =
        matchingGenericClose(lexemes, index);
    if (!close) {
      return false;
    }
    const Lexeme *next = nextSyntaxLexeme(lexemes, *close);
    if (next == nullptr || next->kind == Kind::LeftParen ||
        next->kind == Kind::LeftBrace || next->kind == Kind::Comma ||
        next->kind == Kind::RightParen || next->kind == Kind::Greater) {
      return true;
    }
    if (next->kind == Kind::Operator &&
        (next->text == "*" || next->text == "&" || next->text == "&&")) {
      return isKnownTypeWord(
          lexemes, static_cast<std::size_t>(previous - lexemes.data()),
          declaredTypes);
    }
    if (next->kind != Kind::Word) {
      return false;
    }

    std::size_t typeStart = static_cast<std::size_t>(previous - lexemes.data());
    while (typeStart >= 2 && lexemes[typeStart - 1].kind == Kind::Scope &&
           lexemes[typeStart - 2].kind == Kind::Word) {
      typeStart -= 2;
    }
    const Lexeme *beforeType = previousSyntaxLexeme(lexemes, typeStart);
    if (beforeType != nullptr && beforeType->kind != Kind::LeftBrace &&
        beforeType->kind != Kind::RightBrace &&
        beforeType->kind != Kind::LeftParen &&
        beforeType->kind != Kind::Comma &&
        beforeType->kind != Kind::Semicolon &&
        beforeType->kind != Kind::Colon &&
        beforeType->kind != Kind::Directive &&
        !(beforeType->kind == Kind::Word &&
          (beforeType->text == "mut" || beforeType->text == "static"))) {
      return false;
    }

    const std::size_t nextIndex =
        static_cast<std::size_t>(next - lexemes.data());
    const Lexeme *afterName = nextSyntaxLexeme(lexemes, nextIndex);
    return afterName != nullptr &&
           (afterName->kind == Kind::LeftParen ||
            afterName->kind == Kind::LeftBrace ||
            afterName->kind == Kind::Less ||
            afterName->kind == Kind::Semicolon ||
            afterName->kind == Kind::Comma ||
            afterName->kind == Kind::RightParen ||
            (afterName->kind == Kind::Operator && afterName->text == "="));
  }

  static bool isControlKeyword(std::string_view word) {
    return word == "if" || word == "for" || word == "switch" || word == "while";
  }

  bool spaceBeforeParenthesis(const std::vector<Lexeme> &lexemes,
                              std::size_t parenthesis) const {
    const Lexeme *previous = previousSignificant(lexemes, parenthesis);
    if (previous == nullptr || previous->text == "operator") {
      return false;
    }
    if (previous->kind == Kind::Word &&
        (previous->text == "sizeof" || previous->text == "alignof")) {
      return false;
    }
    if (previous->kind == Kind::Word && previous->text == "return") {
      return true;
    }
    switch (options.spaceBeforeParens) {
    case SpaceBeforeParensStyle::Never:
      return false;
    case SpaceBeforeParensStyle::ControlStatements: {
      const Lexeme *control = previous;
      if (previous->kind == Kind::Word && previous->text == "constexpr") {
        control = previousSignificant(
            lexemes, static_cast<std::size_t>(previous - lexemes.data()));
      }
      return control != nullptr && control->kind == Kind::Word &&
             isControlKeyword(control->text);
    }
    case SpaceBeforeParensStyle::Always:
      return previous->kind == Kind::Word ||
             previous->kind == Kind::RightParen ||
             previous->kind == Kind::RightBracket ||
             previous->kind == Kind::Greater ||
             previous->kind == Kind::ShiftRight;
    }
    return false;
  }

  static bool isAssignmentOperator(std::string_view text) {
    return text == "=" || text == "+=" || text == "-=" || text == "*=" ||
           text == "/=" || text == "%=" || text == "&=" || text == "|=" ||
           text == "^=" || text == "<<=" || text == ">>=";
  }

  static bool isSwitchBodyStart(const std::vector<Lexeme> &lexemes,
                                std::size_t brace) {
    const Lexeme *close = previousSignificant(lexemes, brace);
    if (close == nullptr || close->kind != Kind::RightParen) {
      return false;
    }

    std::size_t depth = 0;
    for (std::size_t current =
             static_cast<std::size_t>(close - lexemes.data()) + 1;
         current-- > 0;) {
      if (lexemes[current].kind == Kind::RightParen) {
        ++depth;
      } else if (lexemes[current].kind == Kind::LeftParen && --depth == 0) {
        const Lexeme *owner = previousSignificant(lexemes, current);
        return owner != nullptr && owner->kind == Kind::Word &&
               owner->text == "switch";
      }
    }
    return false;
  }

  static bool isSwitchLabelColon(const std::vector<Lexeme> &lexemes,
                                 std::size_t index) {
    for (std::size_t current = index; current-- > 0;) {
      const Lexeme &candidate = lexemes[current];
      if (candidate.kind == Kind::Newline || candidate.kind == Kind::Comment) {
        continue;
      }
      if (candidate.kind == Kind::Word &&
          (candidate.text == "case" || candidate.text == "default")) {
        return true;
      }
      if (candidate.kind == Kind::Colon || candidate.kind == Kind::Semicolon ||
          candidate.kind == Kind::LeftBrace ||
          candidate.kind == Kind::RightBrace) {
        return false;
      }
    }
    return false;
  }

  static bool isLambdaCaptureStart(const std::vector<Lexeme> &lexemes,
                                   std::size_t index) {
    const Lexeme *previous = previousSignificant(lexemes, index);
    if (canEndExpression(previous)) {
      return false;
    }
    std::size_t depth = 0;
    for (std::size_t current = index; current < lexemes.size(); ++current) {
      if (lexemes[current].kind == Kind::LeftBracket) {
        ++depth;
      } else if (lexemes[current].kind == Kind::RightBracket && --depth == 0) {
        const Lexeme *next = nextSignificant(lexemes, current);
        return next != nullptr && next->kind == Kind::LeftParen;
      }
    }
    return false;
  }

  static bool isStructuredBindingStart(const std::vector<Lexeme> &lexemes,
                                       std::size_t index) {
    const Lexeme *previous = previousSignificant(lexemes, index);
    if (previous == nullptr || previous->kind != Kind::Word ||
        previous->text != "auto") {
      return false;
    }
    std::size_t depth = 0;
    for (std::size_t current = index; current < lexemes.size(); ++current) {
      if (lexemes[current].kind == Kind::LeftBracket) {
        ++depth;
      } else if (lexemes[current].kind == Kind::RightBracket && --depth == 0) {
        const Lexeme *next = nextSignificant(lexemes, current);
        return next != nullptr && next->kind == Kind::Operator &&
               next->text == "=";
      }
    }
    return false;
  }

  static bool isLambdaReturnArrow(const std::vector<Lexeme> &lexemes,
                                  std::size_t index) {
    const Lexeme *previous = previousSignificant(lexemes, index);
    if (previous == nullptr || previous->kind != Kind::RightParen) {
      return false;
    }
    std::size_t depth = 0;
    for (std::size_t current =
             static_cast<std::size_t>(previous - lexemes.data()) + 1;
         current-- > 0;) {
      if (lexemes[current].kind == Kind::RightParen) {
        ++depth;
      } else if (lexemes[current].kind == Kind::LeftParen && --depth == 0) {
        const Lexeme *before = previousSignificant(lexemes, current);
        return before != nullptr && before->kind == Kind::RightBracket;
      }
    }
    return false;
  }

  static bool canEndExpression(const Lexeme *lexeme) {
    if (lexeme == nullptr) {
      return false;
    }
    return lexeme->kind == Kind::Word || lexeme->kind == Kind::Number ||
           lexeme->kind == Kind::String || lexeme->kind == Kind::RightParen ||
           lexeme->kind == Kind::RightBracket ||
           (lexeme->kind == Kind::Operator &&
            (lexeme->text == "++" || lexeme->text == "--"));
  }

  static bool isUnaryContext(const Lexeme *previous) {
    return previous == nullptr ||
           (previous->kind == Kind::Word && previous->text == "return") ||
           !canEndExpression(previous);
  }

  static bool needsSpaceBeforeValue(const Lexeme *previous) {
    if (previous == nullptr) {
      return false;
    }
    switch (previous->kind) {
    case Kind::Word:
    case Kind::Number:
    case Kind::String:
    case Kind::RightParen:
    case Kind::RightBracket:
    case Kind::RightBrace:
    case Kind::Greater:
    case Kind::ShiftRight:
    case Kind::Directive:
      return true;
    default:
      return false;
    }
  }

  FormatOptions options;
};

} // namespace lang
