#include "gti/formatter.h"

#include "gti/lexer.h"

#include "source_text.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lang {

class FormatterImpl {
public:
  explicit FormatterImpl(FormatOptions options) : options(options) {
    if (this->options.indentWidth == 0) {
      this->options.indentWidth = 2;
    }
  }

  std::string format(std::string_view source) const {
    if (options.disableFormat) {
      return std::string(source);
    }

    if (source_text::firstInvalidUtf8(source) ||
        source_text::firstLoneCarriageReturn(source)) {
      return std::string(source);
    }
    Lexer validationLexer;
    static_cast<void>(validationLexer.scan(std::string(source)));
    if (std::any_of(validationLexer.errors().begin(),
                    validationLexer.errors().end(),
                    [](const LexDiagnostic &diagnostic) {
                      return diagnostic.code == "GTI-L0015";
                    })) {
      return std::string(source);
    }

    const bool hasBom = source_text::hasUtf8Bom(source);
    std::string normalizedSource;
    normalizedSource.reserve(source.size());
    for (const char character : source) {
      if (character != '\r') {
        normalizedSource.push_back(character);
      }
    }

    const std::vector<Lexeme> lexemes = scan(normalizedSource);
    const std::vector<bool> multilineBraces =
        multilineInitializerBraces(lexemes);
    const std::vector<ConstructorLayout> constructorLayout =
        constructorInitializerLayout(lexemes);
    const std::unordered_set<std::string> declaredTypes =
        collectDeclaredTypes(lexemes);
    State state(options);

    for (std::size_t index = 0; index < lexemes.size(); ++index) {
      const Lexeme &lexeme = lexemes[index];
      if (constructorLayout[index] == ConstructorLayout::Body) {
        state.endConstructorInitializers();
      }
      if (state.initializerLineBreakPending && !isComment(lexeme.kind)) {
        state.newline();
        state.initializerLineBreakPending = false;
      }
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
      if (needsLexicalSeparator(lexemes, index)) {
        state.requireLexicalSeparator();
      }

      switch (lexeme.kind) {
      case Kind::Directive:
        // An interface republishes one legacy unit with `export #include`;
        // the prefix belongs to that directive's line.
        if (lexeme.text == "#include" && previous != nullptr &&
            previous->kind == Kind::Word && previous->text == "export" &&
            state.lineHasContent && !state.directiveLine) {
          state.space();
          state.append(lexeme.text);
        } else {
          if (state.lineHasContent) {
            state.newline();
          }
          state.appendUnindented(lexeme.text);
        }
        state.directiveLine = true;
        state.includeLine = lexeme.text == "#include";
        break;
      // A comment alone on its source line describes what follows it, so it
      // keeps its own line rather than trailing the preceding code.
      case Kind::LineComment:
        if (state.lineHasContent) {
          if (startsSourceLine(lexemes, index)) {
            state.newline();
          } else {
            state.trailingCommentSpace();
          }
        }
        state.append(lexeme.text);
        state.newline();
        break;
      case Kind::BlockComment: {
        const bool endsLine = endsSourceLine(lexemes, index);
        if (state.lineHasContent) {
          if (endsLine && startsSourceLine(lexemes, index)) {
            state.newline();
          } else {
            state.space();
          }
        }
        state.appendBlockComment(lexeme.text);
        if (endsLine) {
          state.newline();
        } else {
          state.space();
        }
        break;
      }
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
        if (!state.initializers.empty() ||
            (previous != nullptr && previous->kind == Kind::Operator &&
             previous->text == "=") ||
            (previous != nullptr && previous->kind == Kind::LeftParen) ||
            (previous != nullptr && previous->kind == Kind::Comma) ||
            (previous != nullptr && previous->kind == Kind::Word &&
             previous->text == "return") ||
            directInitializer) {
          if (directInitializer) {
            state.trimSpaces();
          } else if (previous != nullptr &&
                     previous->kind == Kind::RightParen) {
            // A lambda body can occur inside a collection element.
            state.space();
          }
          state.append("{");
          const bool multiline = multilineBraces[index];
          state.initializers.push_back({multiline, state.parenthesisDepth,
                                        state.bracketDepth,
                                        state.templateDepth});
          if (multiline) {
            ++state.indentLevel;
            state.initializerLineBreakPending = true;
          }
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
               !commentFollowsOnLine(lexemes, index))) {
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
        if (!state.initializers.empty()) {
          if (state.initializers.back().multiline) {
            state.newline();
            --state.indentLevel;
          }
          state.trimSpaces();
          state.append("}");
          state.initializers.pop_back();
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
             !commentFollowsOnLine(lexemes, index)) ||
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
        // Consecutive attribute lists stay adjacent: `[[a]][[b]]`.
        if (isLambdaCaptureStart(lexemes, index) ||
            isStructuredBindingStart(lexemes, index) ||
            (isAttributeListStart(lexemes, index) &&
             (previous == nullptr || previous->kind != Kind::RightBracket))) {
          state.space();
        } else {
          state.trimSpaces();
        }
        state.append("[");
        ++state.bracketDepth;
        break;
      case Kind::RightBracket:
        state.trimSpaces();
        state.append("]");
        if (state.bracketDepth > 0) {
          --state.bracketDepth;
        }
        break;
      case Kind::Comma:
        state.trimSpaces();
        state.append(",");
        if (state.atMultilineInitializerElement() ||
            constructorLayout[index] == ConstructorLayout::Separator) {
          // Preserve a trailing comment beside its element; break before the
          // next token or source newline, rather than moving that comment.
          state.initializerLineBreakPending = true;
        } else if (state.enumBodyDepth > 0 && state.parenthesisDepth == 0) {
          state.newline();
        } else {
          state.space();
        }
        break;
      case Kind::Dot:
        if (lexeme.text == "->" &&
            (isLambdaReturnArrow(lexemes, index) ||
             isNativeFunctionTypeArrow(lexemes, index))) {
          state.binaryOperator(lexeme.text);
        } else {
          state.trimSpaces();
          state.append(lexeme.text);
        }
        break;
      case Kind::Ellipsis:
        if (previous != nullptr && previous->kind == Kind::Comma) {
          state.space();
        } else {
          state.trimSpaces();
        }
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
        state.endRequiresClause();
        state.trimSpaces();
        if (constructorLayout[index] == ConstructorLayout::Colon) {
          state.beginConstructorInitializers();
          break;
        }
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
        state.templateDepth = 0;
        state.trimSpaces();
        state.append(";");
        if ((state.parenthesisDepth == 0 || state.atBlockStatementLevel()) &&
            !commentFollowsOnLine(lexemes, index)) {
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
        } else if (lexeme.text == ">=" && state.templateDepth > 0 &&
                   isConceptGenericCloseAssignment(lexemes, index)) {
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
          state.pointerMarker(next == nullptr || next->kind != Kind::Operator ||
                              next->text != "*");
        } else if (previous != nullptr && previous->kind == Kind::Word &&
                   previous->text == "operator") {
          state.trimSpaces();
          state.append(lexeme.text);
        } else if (lexeme.text == "!" || lexeme.text == "~" ||
                   (lexeme.text == "?" && isUnaryContext(previous)) ||
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
    if (hasBom) {
      state.output.insert(0, source_text::utf8Bom);
    }
    return state.output;
  }

private:
  enum class Kind {
    Word,
    Number,
    String,
    LineComment,
    BlockComment,
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

  [[nodiscard]] static constexpr bool isComment(Kind kind) {
    return kind == Kind::LineComment || kind == Kind::BlockComment;
  }

  struct Lexeme {
    Kind kind;
    std::string text;
  };

  enum class ConstructorLayout { None, Colon, Separator, Body };

  struct State {
    struct Initializer {
      bool multiline;
      std::size_t parenthesisDepth;
      std::size_t bracketDepth;
      std::size_t templateDepth;
    };
    struct Block {
      bool switchBody = false;
      bool doBody = false;
      bool caseBodyIndented = false;
      std::size_t parenthesisDepth = 0;
    };

    explicit State(const FormatOptions &options) : options(options) {}

    [[nodiscard]] bool atMultilineInitializerElement() const {
      if (initializers.empty()) {
        return false;
      }
      const Initializer &initializer = initializers.back();
      return initializer.multiline &&
             parenthesisDepth == initializer.parenthesisDepth &&
             bracketDepth == initializer.bracketDepth &&
             templateDepth == initializer.templateDepth;
    }

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
      writeIndentColumns(indentLevel * options.indentWidth +
                         constructorIndentColumns);
      atLineStart = false;
    }

    void append(std::string_view text) {
      writeIndent();
      beforeAppend();
      output.append(text);
      lineHasContent = true;
    }

    void appendUnindented(std::string_view text) {
      atLineStart = false;
      beforeAppend();
      output.append(text);
      lineHasContent = true;
    }

    void appendOutdented(std::string_view text) {
      if (atLineStart) {
        const std::size_t level = indentLevel == 0 ? 0 : indentLevel - 1;
        writeIndentColumns(level * options.indentWidth +
                           constructorIndentColumns);
        atLineStart = false;
      }
      beforeAppend();
      output.append(text);
      lineHasContent = true;
    }

    void appendAccessModifier(std::string_view text) {
      if (atLineStart) {
        const std::size_t baseColumns =
            indentLevel * options.indentWidth + constructorIndentColumns;
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
      beforeAppend();
      output.append(text);
      lineHasContent = true;
    }

    void requireLexicalSeparator() { lexicalSeparatorRequired = true; }

    void beforeAppend() {
      if (!lexicalSeparatorRequired) {
        return;
      }
      if (!output.empty() && output.back() != ' ' && output.back() != '\t' &&
          output.back() != '\n') {
        output.push_back(' ');
      }
      lexicalSeparatorRequired = false;
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
      lexicalSeparatorRequired = false;
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

    void beginConstructorInitializers() {
      const bool afterColon = options.breakConstructorInitializers ==
                              BreakConstructorInitializersStyle::AfterColon;
      const std::size_t width = options.constructorInitializerIndentWidth;
      if (afterColon) {
        space();
        append(":");
        constructorIndentColumns += width;
        constructorIndents.push_back(width);
        initializerLineBreakPending = true;
      } else {
        newline();
        constructorIndentColumns += width;
        append(":");
        space();
        // Subsequent members align with the first target after ": ".
        constructorIndentColumns += 2;
        constructorIndents.push_back(width + 2);
      }
    }

    void endConstructorInitializers() {
      if (!constructorIndents.empty()) {
        constructorIndentColumns -= constructorIndents.back();
        constructorIndents.pop_back();
      }
    }

    void trailingCommentSpace() {
      trimSpaces();
      output.append(options.spacesBeforeTrailingComments, ' ');
    }

    void appendBlockComment(std::string_view text) {
      std::size_t lineStart = 0;
      while (lineStart < text.size()) {
        const std::size_t newline = text.find('\n', lineStart);
        const std::size_t lineEnd =
            newline == std::string_view::npos ? text.size() : newline;
        std::string_view segment = text.substr(lineStart, lineEnd - lineStart);
        if (segment.ends_with('\r')) {
          segment.remove_suffix(1);
        }
        if (lineStart == 0) {
          append(segment);
        } else {
          while (!segment.empty() &&
                 (segment.front() == ' ' || segment.front() == '\t')) {
            segment.remove_prefix(1);
          }
          if (!segment.empty()) {
            writeIndent();
            output.push_back(' ');
            output.append(segment);
            lineHasContent = true;
          }
        }
        if (newline == std::string_view::npos) {
          return;
        }

        if (output.empty() || output.back() != '\n') {
          output.push_back('\n');
        }
        atLineStart = true;
        lineHasContent = false;
        directiveLine = false;
        includeLine = false;
        lineStart = newline + 1;
      }
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

    void pointerMarker(bool trailingSpace) {
      trimSpaces();
      append("*");
      if (trailingSpace) {
        space();
      }
    }

    void pushBlock(bool switchBody, bool doBody) {
      blocks.push_back({.switchBody = switchBody,
                        .doBody = doBody,
                        .parenthesisDepth = parenthesisDepth});
    }

    [[nodiscard]] bool currentBlockIsDoBody() const {
      return !blocks.empty() && blocks.back().doBody;
    }

    [[nodiscard]] bool atBlockStatementLevel() const {
      return !blocks.empty() &&
             parenthesisDepth == blocks.back().parenthesisDepth;
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
    std::size_t bracketDepth = 0;
    std::vector<Initializer> initializers;
    bool initializerLineBreakPending = false;
    std::size_t enumBodyDepth = 0;
    std::size_t templateDepth = 0;
    std::size_t sourceNewlines = 0;
    std::vector<Block> blocks;
    bool atLineStart = true;
    bool lineHasContent = false;
    bool directiveLine = false;
    bool includeLine = false;
    bool requiresClauseOpen = false;
    bool lexicalSeparatorRequired = false;
    std::size_t constructorIndentColumns = 0;
    std::vector<std::size_t> constructorIndents;
  };

  // Annotate legal constructor-list boundaries before emitting whitespace.
  // No width heuristic: Never requests mandatory breaks; BinPack preserves
  // the existing layout. Balanced argument subtrees are opaque here, so their
  // commas, colons, lambdas and collection braces cannot become list breaks.
  std::vector<ConstructorLayout>
  constructorInitializerLayout(const std::vector<Lexeme> &lexemes) const {
    std::vector<ConstructorLayout> layout(lexemes.size(),
                                          ConstructorLayout::None);
    if (options.packConstructorInitializers !=
        PackConstructorInitializersStyle::Never) {
      return layout;
    }
    std::vector<std::size_t> significant;
    std::vector<std::size_t> closes(lexemes.size(), lexemes.size());
    std::vector<std::size_t> openings;
    bool directive = false;
    for (std::size_t i = 0; i < lexemes.size(); ++i) {
      const Kind kind = lexemes[i].kind;
      if (kind == Kind::Directive) {
        directive = true;
      }
      if (kind == Kind::Newline) {
        directive = false;
      }
      if (directive) {
        continue;
      }
      if (kind != Kind::Newline && !isComment(kind)) {
        significant.push_back(i);
      }
      if (kind == Kind::LeftParen || kind == Kind::LeftBracket ||
          kind == Kind::LeftBrace) {
        openings.push_back(i);
      } else if (kind == Kind::RightParen || kind == Kind::RightBracket ||
                 kind == Kind::RightBrace) {
        if (openings.empty()) {
          continue;
        }
        const Kind open = lexemes[openings.back()].kind;
        if ((open == Kind::LeftParen && kind == Kind::RightParen) ||
            (open == Kind::LeftBracket && kind == Kind::RightBracket) ||
            (open == Kind::LeftBrace && kind == Kind::RightBrace)) {
          closes[openings.back()] = i;
          openings.pop_back();
        } else {
          openings.clear();
        }
      }
    }
    const auto positionAfter = [&](std::size_t token) {
      return static_cast<std::size_t>(
          std::upper_bound(significant.begin(), significant.end(), token) -
          significant.begin());
    };
    const auto kindAt = [&](std::size_t position, Kind kind) {
      return position < significant.size() &&
             lexemes[significant[position]].kind == kind;
    };
    const auto skipGeneric = [&](std::size_t &position) {
      if (!kindAt(position, Kind::Less)) {
        return true;
      }
      const auto close =
          matchingGenericClose(lexemes, significant[position], true);
      if (!close) {
        return false;
      }
      position = positionAfter(*close);
      return true;
    };
    for (std::size_t start = 0; start < significant.size(); ++start) {
      if (start != 0) {
        const Lexeme &previous = lexemes[significant[start - 1]];
        const bool access = previous.kind == Kind::Colon && start >= 2 &&
                            (lexemes[significant[start - 2]].text == "public" ||
                             lexemes[significant[start - 2]].text == "private");
        if (previous.kind != Kind::LeftBrace &&
            previous.kind != Kind::RightBrace &&
            previous.kind != Kind::Semicolon && !access) {
          continue;
        }
      }
      std::size_t position = start;
      if (lexemes[significant[position]].text == "constexpr") {
        ++position;
      }
      if (!kindAt(position, Kind::Word)) {
        continue;
      }
      const std::string &name = lexemes[significant[position++]].text;
      if (name == "if" || name == "while" || name == "for" ||
          name == "switch" || name == "return") {
        continue;
      }
      if (!skipGeneric(position) || !kindAt(position, Kind::LeftParen)) {
        continue;
      }
      const std::size_t parametersEnd = closes[significant[position]];
      if (parametersEnd == lexemes.size()) {
        continue;
      }
      position = positionAfter(parametersEnd);
      if (kindAt(position, Kind::Word) &&
          lexemes[significant[position]].text == "requires") {
        ++position;
        while (position < significant.size() &&
               !kindAt(position, Kind::Colon) &&
               !kindAt(position, Kind::LeftBrace) &&
               !kindAt(position, Kind::Semicolon) &&
               !kindAt(position, Kind::RightBrace)) {
          ++position;
        }
      }
      if (!kindAt(position, Kind::Colon)) {
        continue;
      }
      const std::size_t colon = significant[position++];
      std::vector<std::size_t> separators;
      bool complete = false;
      while (position < significant.size()) {
        if (!kindAt(position, Kind::Word)) {
          break;
        }
        ++position;
        if (!skipGeneric(position)) {
          break;
        }
        while (kindAt(position, Kind::Scope) &&
               kindAt(position + 1, Kind::Word)) {
          position += 2;
          if (!skipGeneric(position)) {
            break;
          }
        }
        if (!kindAt(position, Kind::LeftParen)) {
          break;
        }
        const std::size_t argumentsEnd = closes[significant[position]];
        if (argumentsEnd == lexemes.size()) {
          break;
        }
        position = positionAfter(argumentsEnd);
        if (kindAt(position, Kind::LeftBrace)) {
          complete = true;
          break;
        }
        if (!kindAt(position, Kind::Comma)) {
          break;
        }
        separators.push_back(significant[position++]);
      }
      if (complete) {
        layout[colon] = ConstructorLayout::Colon;
        layout[significant[position]] = ConstructorLayout::Body;
        for (const std::size_t separator : separators) {
          layout[separator] = ConstructorLayout::Separator;
        }
      }
    }
    return layout;
  }

  // Determine layout once, including on incomplete editor input. A designated
  // list is structural, not a packed argument list. Its enclosing collections
  // expand too, while single-line scalar lists and constructor arguments stay
  // compact. Never synthesize braces, commas, or semicolons here.
  static std::vector<bool>
  multilineInitializerBraces(const std::vector<Lexeme> &lexemes) {
    std::vector<bool> multiline(lexemes.size(), false);
    std::vector<std::size_t> openings;
    std::vector<bool> hasContent(lexemes.size(), false);
    const auto close = [&] {
      const std::size_t opening = openings.back();
      openings.pop_back();
      multiline[opening] = multiline[opening] && hasContent[opening];
      if (!openings.empty() && multiline[opening]) {
        multiline[openings.back()] = true;
      }
    };
    for (std::size_t index = 0; index < lexemes.size(); ++index) {
      const Lexeme &lexeme = lexemes[index];
      if (lexeme.kind == Kind::RightBrace) {
        if (!openings.empty()) {
          close();
        }
        continue;
      }
      if (!openings.empty()) {
        if (lexeme.kind == Kind::Newline || lexeme.kind == Kind::Semicolon ||
            lexeme.kind == Kind::LineComment ||
            (lexeme.kind == Kind::BlockComment &&
             lexeme.text.find('\n') != std::string::npos)) {
          multiline[openings.back()] = true;
        }
        if (lexeme.kind != Kind::Newline) {
          hasContent[openings.back()] = true;
        }
      }
      if (lexeme.kind == Kind::LeftBrace) {
        openings.push_back(index);
        const Lexeme *first = nextSignificant(lexemes, index);
        multiline[index] =
            first != nullptr && first->kind == Kind::Dot && first->text == ".";
      }
    }
    while (!openings.empty()) {
      close();
    }
    return multiline;
  }

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
    const auto collectGenericParameters = [&](std::size_t genericStart) {
      std::size_t depth = 1;
      std::size_t segmentStart = genericStart + 1;
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
    };

    for (std::size_t index = 0; index < lexemes.size(); ++index) {
      if (lexemes[index].kind != Kind::Word ||
          (lexemes[index].text != "class" && lexemes[index].text != "concept" &&
           lexemes[index].text != "interface" &&
           lexemes[index].text != "struct" && lexemes[index].text != "union" &&
           lexemes[index].text != "enum" && lexemes[index].text != "using")) {
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
        collectGenericParameters(
            static_cast<std::size_t>(generic - lexemes.data()));
      }
    }

    for (std::size_t index = 0; index < lexemes.size(); ++index) {
      if (lexemes[index].kind == Kind::Less &&
          isFunctionGenericParameterClause(lexemes, index, result)) {
        collectGenericParameters(index);
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
        return next != nullptr && next->kind == Kind::Word;
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
           candidate.text == "struct" || candidate.text == "union")) {
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
         typeEnd->text == "struct" || typeEnd->text == "union" ||
         typeEnd->text == "enum" || typeEnd->text == "namespace" ||
         typeEnd->text == "return" || typeEnd->text == "else" ||
         typeEnd->text == "case" || typeEnd->text == "default")) {
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
    const Lexeme *previous = previousSyntaxLexeme(lexemes, index);
    const Lexeme *next = nextSyntaxLexeme(lexemes, index);
    if (previous == nullptr || next == nullptr) {
      return false;
    }

    const std::size_t previousIndex =
        static_cast<std::size_t>(previous - lexemes.data());
    if (!typeEndsAt(lexemes, previousIndex, declaredTypes) &&
        !isUndeclaredFunctionReturnDeclarator(lexemes, previousIndex, index) &&
        !isUndeclaredParameterDeclarator(lexemes, index, declaredTypes) &&
        !isInitializedUndeclaredDeclarator(lexemes, index, declaredTypes)) {
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
           (afterName->kind == Kind::Less &&
            isFunctionGenericParameterClause(
                lexemes, static_cast<std::size_t>(afterName - lexemes.data()),
                declaredTypes)) ||
           (afterName->kind == Kind::Operator && afterName->text == "=");
  }

  static bool
  isReferenceDeclarator(const std::vector<Lexeme> &lexemes, std::size_t index,
                        const std::unordered_set<std::string> &declaredTypes) {
    const Lexeme *previous = previousSyntaxLexeme(lexemes, index);
    const Lexeme *next = nextSyntaxLexeme(lexemes, index);
    if (previous == nullptr || next == nullptr) {
      return false;
    }

    const std::size_t previousIndex =
        static_cast<std::size_t>(previous - lexemes.data());
    const bool typeBefore = typeEndsAt(lexemes, previousIndex, declaredTypes);
    if (!typeBefore &&
        !isUndeclaredFunctionReturnDeclarator(lexemes, previousIndex, index) &&
        !isUndeclaredParameterDeclarator(lexemes, index, declaredTypes) &&
        !isInitializedUndeclaredDeclarator(lexemes, index, declaredTypes)) {
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
    return afterName->kind == Kind::Comma || afterName->kind == Kind::Colon ||
           afterName->kind == Kind::LeftBracket ||
           afterName->kind == Kind::LeftParen ||
           afterName->kind == Kind::LeftBrace ||
           afterName->kind == Kind::RightParen ||
           afterName->kind == Kind::Semicolon ||
           (afterName->kind == Kind::Less &&
            isFunctionGenericParameterClause(
                lexemes, static_cast<std::size_t>(afterName - lexemes.data()),
                declaredTypes)) ||
           (afterName->kind == Kind::Operator && afterName->text == "=");
  }

  static bool isDeclarationModifier(const Lexeme &lexeme) {
    return lexeme.kind == Kind::Word &&
           (lexeme.text == "export" || lexeme.text == "virtual" ||
            lexeme.text == "static" || lexeme.text == "constexpr" ||
            lexeme.text == "mut" || lexeme.text == "unsafe" ||
            lexeme.text == "implicit");
  }

  static bool isDeclarationBoundary(const Lexeme *lexeme) {
    return lexeme == nullptr || lexeme->kind == Kind::LeftBrace ||
           lexeme->kind == Kind::RightBrace ||
           lexeme->kind == Kind::Semicolon || lexeme->kind == Kind::Colon ||
           lexeme->kind == Kind::Directive;
  }

  static bool isDeclarationBodyStart(const std::vector<Lexeme> &lexemes,
                                     std::size_t brace) {
    while (brace > 0) {
      --brace;
      const Lexeme &candidate = lexemes[brace];
      if (candidate.kind == Kind::LeftBrace ||
          candidate.kind == Kind::RightBrace ||
          candidate.kind == Kind::Semicolon) {
        return false;
      }
      if (candidate.kind == Kind::Word &&
          (candidate.text == "class" || candidate.text == "interface" ||
           candidate.text == "struct" || candidate.text == "union" ||
           candidate.text == "enum" || candidate.text == "namespace")) {
        return true;
      }
    }
    return false;
  }

  static bool isDeclarationScope(const std::vector<Lexeme> &lexemes,
                                 std::size_t before) {
    std::size_t closedBraces = 0;
    while (before > 0) {
      --before;
      if (lexemes[before].kind == Kind::RightBrace) {
        ++closedBraces;
      } else if (lexemes[before].kind == Kind::LeftBrace) {
        if (closedBraces > 0) {
          --closedBraces;
        } else {
          return isDeclarationBodyStart(lexemes, before);
        }
      }
    }
    return true;
  }

  static bool isNonTypeDeclarationWord(std::string_view word) {
    return word == "break" || word == "case" || word == "class" ||
           word == "concept" || word == "continue" || word == "default" ||
           word == "do" || word == "else" || word == "enum" || word == "for" ||
           word == "if" || word == "interface" || word == "namespace" ||
           word == "return" || word == "struct" || word == "switch" ||
           word == "this" || word == "union" || word == "unsafe" ||
           word == "using" || word == "while";
  }

  static bool isDeclarationTypeLexeme(const Lexeme &lexeme) {
    if (lexeme.kind == Kind::Word) {
      return !isNonTypeDeclarationWord(lexeme.text);
    }
    if (lexeme.kind == Kind::Scope || lexeme.kind == Kind::Less ||
        lexeme.kind == Kind::Greater || lexeme.kind == Kind::ShiftRight ||
        lexeme.kind == Kind::Comma || lexeme.kind == Kind::Number ||
        lexeme.kind == Kind::LeftBracket || lexeme.kind == Kind::RightBracket) {
      return true;
    }
    return lexeme.kind == Kind::Operator &&
           (lexeme.text == "*" || lexeme.text == "&" || lexeme.text == "&&");
  }

  static std::optional<std::size_t>
  declarationTypeStartEndingAt(const std::vector<Lexeme> &lexemes,
                               std::size_t typeEnd) {
    std::size_t cursor = typeEnd;
    std::size_t typeStart = typeEnd;
    bool hasTypeWord = false;
    while (true) {
      const Lexeme &candidate = lexemes[cursor];
      if (!isDeclarationTypeLexeme(candidate)) {
        return std::nullopt;
      }
      hasTypeWord = hasTypeWord || (candidate.kind == Kind::Word &&
                                    !isDeclarationModifier(candidate));
      typeStart = cursor;

      const Lexeme *previous = previousSyntaxLexeme(lexemes, cursor);
      if (isDeclarationBoundary(previous)) {
        break;
      }
      cursor = static_cast<std::size_t>(previous - lexemes.data());
    }
    return hasTypeWord ? std::optional<std::size_t>(typeStart) : std::nullopt;
  }

  static bool isFunctionDeclarationName(
      const std::vector<Lexeme> &lexemes, std::size_t name,
      const std::unordered_set<std::string> &declaredTypes) {
    const Lexeme *typeEnd = previousSyntaxLexeme(lexemes, name);
    if (typeEnd != nullptr) {
      const std::optional<std::size_t> typeStart = declarationTypeStartEndingAt(
          lexemes, static_cast<std::size_t>(typeEnd - lexemes.data()));
      if (typeStart && isDeclarationScope(lexemes, *typeStart)) {
        return true;
      }
    }

    if (lexemes[name].kind != Kind::Word ||
        !declaredTypes.contains(lexemes[name].text) ||
        !isDeclarationScope(lexemes, name)) {
      return false;
    }
    const Lexeme *beforeName = previousSyntaxLexeme(lexemes, name);
    return isDeclarationBoundary(beforeName);
  }

  static bool isFunctionGenericParameterClause(
      const std::vector<Lexeme> &lexemes, std::size_t genericStart,
      const std::unordered_set<std::string> &declaredTypes) {
    const Lexeme *name = previousSyntaxLexeme(lexemes, genericStart);
    const std::optional<std::size_t> genericEnd =
        matchingGenericClose(lexemes, genericStart, true);
    if (name == nullptr || name->kind != Kind::Word || !genericEnd) {
      return false;
    }
    const Lexeme *parameters = nextSyntaxLexeme(lexemes, *genericEnd);
    return parameters != nullptr && parameters->kind == Kind::LeftParen &&
           isFunctionDeclarationName(
               lexemes, static_cast<std::size_t>(name - lexemes.data()),
               declaredTypes);
  }

  static bool isFunctionParameterClause(
      const std::vector<Lexeme> &lexemes, std::size_t leftParen,
      const std::unordered_set<std::string> &declaredTypes) {
    const Lexeme *before = previousSyntaxLexeme(lexemes, leftParen);
    if (before == nullptr) {
      return false;
    }
    if (before->kind == Kind::Word) {
      return isFunctionDeclarationName(
          lexemes, static_cast<std::size_t>(before - lexemes.data()),
          declaredTypes);
    }
    if (before->kind != Kind::Greater && before->kind != Kind::ShiftRight) {
      return false;
    }

    const std::size_t close = static_cast<std::size_t>(before - lexemes.data());
    for (std::size_t cursor = close; cursor > 0;) {
      --cursor;
      if (isDeclarationBoundary(&lexemes[cursor])) {
        break;
      }
      if (lexemes[cursor].kind != Kind::Less ||
          matchingGenericClose(lexemes, cursor, true) != close) {
        continue;
      }
      const Lexeme *name = previousSyntaxLexeme(lexemes, cursor);
      if (name != nullptr && name->kind == Kind::Word &&
          isFunctionDeclarationName(
              lexemes, static_cast<std::size_t>(name - lexemes.data()),
              declaredTypes)) {
        return true;
      }
    }
    return false;
  }

  static std::optional<std::size_t>
  enclosingLeftParen(const std::vector<Lexeme> &lexemes, std::size_t index) {
    std::size_t depth = 0;
    while (index > 0) {
      --index;
      if (lexemes[index].kind == Kind::RightParen) {
        ++depth;
      } else if (lexemes[index].kind == Kind::LeftParen) {
        if (depth == 0) {
          return index;
        }
        --depth;
      } else if (depth == 0 && (lexemes[index].kind == Kind::LeftBrace ||
                                lexemes[index].kind == Kind::RightBrace ||
                                lexemes[index].kind == Kind::Semicolon)) {
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

  static bool isUndeclaredParameterDeclarator(
      const std::vector<Lexeme> &lexemes, std::size_t marker,
      const std::unordered_set<std::string> &declaredTypes) {
    const std::optional<std::size_t> parameters =
        enclosingLeftParen(lexemes, marker);
    if (!parameters) {
      return false;
    }
    // Signature types have unnamed parameters and may use types imported from
    // another source unit. A terminal pointer/borrow marker in such a slot
    // cannot be a complete binary expression; no nominal-name list is needed.
    const Lexeme *next = nextSyntaxLexeme(lexemes, marker);
    const Lexeme *result = previousSyntaxLexeme(lexemes, *parameters);
    const bool unnamedSignatureParameter =
        next != nullptr && result != nullptr &&
        (next->kind == Kind::Comma || next->kind == Kind::RightParen) &&
        ((result->kind == Kind::Word &&
          !isNonTypeDeclarationWord(result->text)) ||
         result->kind == Kind::Greater || result->kind == Kind::ShiftRight ||
         result->kind == Kind::RightBracket ||
         (result->kind == Kind::Operator &&
          (result->text == "*" || result->text == "&")));
    if (!unnamedSignatureParameter &&
        !isFunctionParameterClause(lexemes, *parameters, declaredTypes)) {
      return false;
    }

    bool hasDefault = false;
    std::size_t nested = 0;
    for (std::size_t cursor = *parameters + 1; cursor < marker; ++cursor) {
      const Lexeme &candidate = lexemes[cursor];
      if (candidate.kind == Kind::LeftParen ||
          candidate.kind == Kind::LeftBracket) {
        ++nested;
      } else if ((candidate.kind == Kind::RightParen ||
                  candidate.kind == Kind::RightBracket) &&
                 nested > 0) {
        --nested;
      } else if (nested == 0 && candidate.kind == Kind::Comma) {
        hasDefault = false;
      } else if (nested == 0 && candidate.kind == Kind::Operator &&
                 candidate.text == "=") {
        hasDefault = true;
      }
    }
    return !hasDefault;
  }

  static bool
  isInitializedUndeclaredDeclarator(const std::vector<Lexeme> &lexemes,
                                    std::size_t marker,
                                    const std::unordered_set<std::string> &) {
    const Lexeme *typeEnd = previousSyntaxLexeme(lexemes, marker);
    const Lexeme *name = nextSyntaxLexeme(lexemes, marker);
    if (typeEnd == nullptr || name == nullptr || name->kind != Kind::Word) {
      return false;
    }
    const std::optional<std::size_t> typeStart = declarationTypeStartEndingAt(
        lexemes, static_cast<std::size_t>(typeEnd - lexemes.data()));
    if (!typeStart) {
      return false;
    }

    const Lexeme *afterName = nextSyntaxLexeme(
        lexemes, static_cast<std::size_t>(name - lexemes.data()));
    if (afterName == nullptr) {
      return false;
    }
    return (afterName->kind == Kind::Operator && afterName->text == "=") ||
           afterName->kind == Kind::LeftBrace ||
           (afterName->kind == Kind::Semicolon &&
            isDeclarationScope(lexemes, *typeStart));
  }

  static bool
  isUndeclaredFunctionReturnDeclarator(const std::vector<Lexeme> &lexemes,
                                       std::size_t typeEnd,
                                       std::size_t marker) {
    const std::optional<std::size_t> typeStart =
        declarationTypeStartEndingAt(lexemes, typeEnd);
    if (!typeStart || !isDeclarationScope(lexemes, *typeStart)) {
      return false;
    }

    const Lexeme *name = nextSyntaxLexeme(lexemes, marker);
    if (name == nullptr || name->kind != Kind::Word) {
      return false;
    }
    const Lexeme *parameters = nextSyntaxLexeme(
        lexemes, static_cast<std::size_t>(name - lexemes.data()));
    if (parameters != nullptr && parameters->kind == Kind::Less) {
      const std::optional<std::size_t> genericEnd = matchingGenericClose(
          lexemes, static_cast<std::size_t>(parameters - lexemes.data()), true);
      if (!genericEnd) {
        return false;
      }
      parameters = nextSyntaxLexeme(lexemes, *genericEnd);
    }
    return parameters != nullptr && parameters->kind == Kind::LeftParen;
  }

  static std::vector<Lexeme> scan(std::string_view source) {
    std::vector<Lexeme> result;
    std::size_t current =
        source_text::hasUtf8Bom(source) ? source_text::utf8Bom.size() : 0;

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
        add(Kind::LineComment, source.substr(start, current - start));
        continue;
      }
      if (character == '/' && current < source.size() &&
          source[current] == '*') {
        ++current;
        while (current + 1 < source.size() &&
               !(source[current] == '*' && source[current + 1] == '/')) {
          ++current;
        }
        if (current + 1 < source.size()) {
          current += 2;
        } else {
          current = source.size();
        }
        add(Kind::BlockComment, source.substr(start, current - start));
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

  static bool sameLexemes(const std::vector<Lexeme> &actual,
                          const std::vector<const Lexeme *> &expected) {
    if (actual.size() != expected.size()) {
      return false;
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
      if (actual[index].kind != expected[index]->kind ||
          actual[index].text != expected[index]->text) {
        return false;
      }
    }
    return true;
  }

  static bool retokenizes(const std::vector<const Lexeme *> &window) {
    std::string joined;
    for (const Lexeme *lexeme : window) {
      joined += lexeme->text;
    }
    return !sameLexemes(scan(joined), window);
  }

  static bool needsLexicalSeparator(const std::vector<Lexeme> &lexemes,
                                    std::size_t index) {
    const Lexeme &current = lexemes[index];
    if (current.kind == Kind::Newline || current.kind == Kind::Directive ||
        isComment(current.kind)) {
      return false;
    }

    std::vector<const Lexeme *> preceding;
    for (std::size_t cursor = index; cursor > 0 && preceding.size() < 2;) {
      --cursor;
      const Lexeme &candidate = lexemes[cursor];
      if (candidate.kind == Kind::Newline) {
        continue;
      }
      if (candidate.kind == Kind::Directive) {
        // A directive is normally a hard boundary, but an incomplete marker
        // such as a lone '#' merges with a following word into a single
        // directive token when re-scanned (e.g. '#' 'case' -> '#case'). Keep
        // them separated when gluing them would change the token stream, so
        // formatting stays idempotent on recovered input.
        if (preceding.empty() && retokenizes({&candidate, &current})) {
          return true;
        }
        break;
      }
      if (isComment(candidate.kind)) {
        break;
      }
      preceding.push_back(&candidate);
    }
    if (preceding.empty()) {
      return false;
    }

    const std::vector<const Lexeme *> adjacent{preceding.front(), &current};
    if (retokenizes(adjacent)) {
      return true;
    }
    if (preceding.size() < 2) {
      return false;
    }

    const std::vector<const Lexeme *> priorPair{preceding[1], preceding[0]};
    if (retokenizes(priorPair)) {
      return false;
    }
    return retokenizes({preceding[1], preceding[0], &current});
  }

  // Comment placement follows source lines: only a comment on the same line
  // trails the code before it.
  static bool startsSourceLine(const std::vector<Lexeme> &lexemes,
                               std::size_t index) {
    return index == 0 || lexemes[index - 1].kind == Kind::Newline;
  }

  static bool endsSourceLine(const std::vector<Lexeme> &lexemes,
                             std::size_t index) {
    return index + 1 >= lexemes.size() ||
           lexemes[index + 1].kind == Kind::Newline;
  }

  static bool commentFollowsOnLine(const std::vector<Lexeme> &lexemes,
                                   std::size_t index) {
    return index + 1 < lexemes.size() && isComment(lexemes[index + 1].kind);
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
          !isComment(lexemes[index].kind)) {
        return &lexemes[index];
      }
    }
    return nullptr;
  }

  static const Lexeme *nextSyntaxLexeme(const std::vector<Lexeme> &lexemes,
                                        std::size_t index) {
    for (++index; index < lexemes.size(); ++index) {
      if (lexemes[index].kind != Kind::Newline &&
          !isComment(lexemes[index].kind)) {
        return &lexemes[index];
      }
    }
    return nullptr;
  }

  static std::optional<std::size_t>
  matchingGenericClose(const std::vector<Lexeme> &lexemes, std::size_t left,
                       bool allowDefaultAssignment = false) {
    std::size_t depth = 0;
    std::size_t parenthesisDepth = 0;
    std::size_t bracketDepth = 0;
    for (std::size_t index = left; index < lexemes.size(); ++index) {
      const Lexeme &candidate = lexemes[index];
      if (candidate.kind == Kind::Semicolon ||
          candidate.kind == Kind::LeftBrace ||
          candidate.kind == Kind::RightBrace) {
        return std::nullopt;
      }
      if (candidate.kind == Kind::LeftParen) {
        ++parenthesisDepth;
        continue;
      }
      if (candidate.kind == Kind::RightParen) {
        if (parenthesisDepth == 0) {
          return std::nullopt;
        }
        --parenthesisDepth;
        continue;
      }
      if (candidate.kind == Kind::LeftBracket) {
        ++bracketDepth;
        continue;
      }
      if (candidate.kind == Kind::RightBracket) {
        if (bracketDepth == 0) {
          return std::nullopt;
        }
        --bracketDepth;
        continue;
      }
      if (candidate.kind == Kind::Operator) {
        const Lexeme *next = nextSyntaxLexeme(lexemes, index);
        const bool referenceSuffix =
            (candidate.text == "&" || candidate.text == "&&") &&
            next != nullptr &&
            (next->kind == Kind::Comma || next->kind == Kind::Greater ||
             next->kind == Kind::ShiftRight ||
             (parenthesisDepth > 0 && next->kind == Kind::RightParen));
        if (!referenceSuffix &&
            (candidate.text == "&" || candidate.text == "&&" ||
             candidate.text == "||" || candidate.text == "==" ||
             candidate.text == "!=" || candidate.text == "<=" ||
             candidate.text == ">=" ||
             (bracketDepth == 0 && isAssignmentOperator(candidate.text) &&
              !(allowDefaultAssignment && candidate.text == "=")))) {
          return std::nullopt;
        }
      }
      if (candidate.kind == Kind::Less) {
        ++depth;
      } else if (candidate.kind == Kind::Greater) {
        if (--depth == 0) {
          return index;
        }
      } else if (candidate.kind == Kind::ShiftRight) {
        for (std::size_t close = 0; close < 2; ++close) {
          if (--depth == 0) {
            return index;
          }
        }
      }
    }
    return std::nullopt;
  }

  static bool
  isConceptGenericCloseAssignment(const std::vector<Lexeme> &lexemes,
                                  std::size_t index) {
    return lexemes[index].kind == Kind::Operator &&
           lexemes[index].text == ">=" && isConceptGenericAngle(lexemes, index);
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

  // True when the `requires` keyword introduces a trailing requirement on a
  // declaration. The formatter tokenizes words independently from the
  // compiler lexer, so this still needs structural recognition. The grammar
  // places a requires-clause directly after the parameter clause, optionally
  // separated by the receiver qualifier `mut` or `&&`, and it is always
  // followed by a concept application.
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
    } else if (previous->kind == Kind::Operator && previous->text == "&&") {
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
    if (isFunctionGenericParameterClause(lexemes, index, declaredTypes)) {
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
        next->kind == Kind::RightParen || next->kind == Kind::Greater ||
        next->kind == Kind::Scope || next->kind == Kind::Semicolon) {
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
        !isDeclarationModifier(*beforeType)) {
      return false;
    }

    const std::size_t nextIndex =
        static_cast<std::size_t>(next - lexemes.data());
    const Lexeme *afterName = nextSyntaxLexeme(lexemes, nextIndex);
    if (afterName != nullptr && afterName->kind == Kind::Scope) {
      // Out-of-class return types precede Owner::method rather than a single
      // name. Only admit that shape in declaration scope: a body expression
      // such as left < middle > limits::value() still uses comparisons.
      if (!isDeclarationScope(lexemes, typeStart)) {
        return false;
      }
      do {
        const Lexeme *component = nextSyntaxLexeme(
            lexemes, static_cast<std::size_t>(afterName - lexemes.data()));
        if (component == nullptr || component->kind != Kind::Word) {
          return false;
        }
        afterName = nextSyntaxLexeme(
            lexemes, static_cast<std::size_t>(component - lexemes.data()));
      } while (afterName != nullptr && afterName->kind == Kind::Scope);
      return afterName != nullptr && afterName->kind == Kind::LeftParen;
    }
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
      if (candidate.kind == Kind::Newline || isComment(candidate.kind)) {
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

  // The outer '[' of a `[[...]]` declaration or statement attribute list,
  // such as after `export` or `else`. A subscript whose operand begins with a
  // lambda capture closes its inner '[' before '(' rather than ']'.
  static bool isAttributeListStart(const std::vector<Lexeme> &lexemes,
                                   std::size_t index) {
    const Lexeme *previous = previousSignificant(lexemes, index);
    const Lexeme *inner = nextSignificant(lexemes, index);
    if (inner == nullptr || inner->kind != Kind::LeftBracket ||
        (previous != nullptr && previous->kind == Kind::LeftBracket)) {
      return false;
    }
    std::size_t depth = 0;
    for (std::size_t current = static_cast<std::size_t>(inner - lexemes.data());
         current < lexemes.size(); ++current) {
      if (lexemes[current].kind == Kind::LeftBracket) {
        ++depth;
      } else if (lexemes[current].kind == Kind::RightBracket && --depth == 0) {
        const Lexeme *next = nextSignificant(lexemes, current);
        return next != nullptr && next->kind == Kind::RightBracket;
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

  static bool isNativeFunctionTypeArrow(const std::vector<Lexeme> &lexemes,
                                        std::size_t index) {
    const Lexeme *previous = previousSignificant(lexemes, index);
    if (previous == nullptr || previous->kind != Kind::RightParen) {
      return false;
    }

    bool afterAliasAssignment = false;
    for (std::size_t current = index; current-- > 0;) {
      const Lexeme &candidate = lexemes[current];
      if (candidate.kind == Kind::Semicolon ||
          candidate.kind == Kind::LeftBrace ||
          candidate.kind == Kind::RightBrace ||
          candidate.kind == Kind::Directive) {
        return false;
      }
      if (candidate.kind == Kind::Operator && candidate.text == "=") {
        afterAliasAssignment = true;
        continue;
      }
      if (afterAliasAssignment && candidate.kind == Kind::Word &&
          candidate.text == "using") {
        return true;
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

Formatter::Formatter(FormatOptions options) : options(std::move(options)) {
  if (this->options.indentWidth == 0) {
    this->options.indentWidth = 2;
  }
}

std::string Formatter::format(std::string_view source) const {
  return FormatterImpl(options).format(source);
}

} // namespace lang
