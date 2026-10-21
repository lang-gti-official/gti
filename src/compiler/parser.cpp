#include "gti/parser.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lang {

using ParseDiagnostic = Diagnostic;

class Parser::Impl {
private:
  class ParseError {};
  class ConditionalBoundary {};

  static constexpr std::size_t maximumNestingDepth = 64;

  class NestingGuard {
  public:
    explicit NestingGuard(std::size_t &depth) : depth(depth) { ++this->depth; }
    ~NestingGuard() { --depth; }

    NestingGuard(const NestingGuard &) = delete;
    NestingGuard &operator=(const NestingGuard &) = delete;

  private:
    std::size_t &depth;
  };

  enum class ItemContext {
    Declaration,
    ClassMember,
    MutableFieldGroup,
    Block,
  };

public:
  Impl(std::vector<Token> tokens, std::string_view source)
      : tokens(std::move(tokens)), source(source) {
    if (this->tokens.empty()) {
      this->tokens.emplace_back(TokenKind::END_OF_FILE, "", std::monostate{}, 0,
                                1);
    } else if (this->tokens.back().kind != TokenKind::END_OF_FILE) {
      const Token &last = this->tokens.back();
      this->tokens.emplace_back(TokenKind::END_OF_FILE, "", std::monostate{},
                                last.position + last.lexeme.size(), last.line,
                                last.source);
    }
  }

  Program parse() {
    reset();
    StmtList declarations;

    while (!isAtEnd()) {
      try {
        if (check(TokenKind::IMPORT) ||
            (check(TokenKind::EXPORT) && peekAt(1).kind == TokenKind::IMPORT)) {
          sourceUnitImport();
          continue;
        }
        declarations.emplace_back(declaration());
      } catch (const ParseError &) {
        synchronize(false, false, true);
      }
    }

    return Program(std::move(declarations));
  }

  // Kept as a focused entry point for expression tests and tooling.
  ExprPtr parseExpression() {
    reset();
    try {
      ExprPtr result = expression();
      match({TokenKind::SEMICOLON});
      if (!isAtEnd()) {
        throw error(peek(), "Expect end of expression.");
      }
      return result;
    } catch (const ParseError &) {
      return nullptr;
    }
  }

  [[nodiscard]] bool hadError() const { return !diagnostics.empty(); }

  [[nodiscard]] const std::vector<ParseDiagnostic> &errors() const {
    return diagnostics;
  }

private:
  // Resolution belongs to SourceLoader. Retain the directive through parsing
  // so only this source-unit boundary can consume it: an import embedded in a
  // declaration or expression must not disappear before its syntax is checked.
  void sourceUnitImport() {
    match({TokenKind::EXPORT});
    consume(TokenKind::IMPORT, "Expect 'import' after 'export'.");
    if (!match({TokenKind::STRING_LITERAL})) {
      consume(TokenKind::IDENTIFIER, "Expect a module name after 'import'.");
      if (match({TokenKind::DOT})) {
        consume(TokenKind::IDENTIFIER,
                "Expect a module name after the dependency alias.");
      }
    }
    consume(TokenKind::SEMICOLON, "Expect ';' after a module import.");
  }

  void reset() {
    current = 0;
    nestingDepth = 0;
    casePatternDepth = 0;
    conditionalDepth = 0;
    diagnostics.clear();
    currentClassName.reset();
    currentClassKind.reset();
    consumedCompletion = false;
    generatedNameCounter = 0;
    missingTokenError = false;
  }

  // Both statement funnels stamp the parser-recorded full extent onto the
  // returned node so tooling can consume enclosing declaration ranges
  // without re-deriving structure from punctuation.
  StmtPtr declaration() {
    const NestingGuard nesting = enterNesting(peek());
    const Token start = peek();
    StmtPtr result = declarationWithoutExtent();
    stampExtent(result, start);
    return result;
  }

  StmtPtr declarationWithoutExtent() {
    if (const std::optional<std::size_t> offset = misplacedExportOffset()) {
      // Recover as though 'export' had been written first, so tooling keeps
      // the exported declaration while the syntax error prevents generation.
      const Token keyword = peekAt(*offset);
      reportMisplacedExport(*offset);
      if (peekAt(*offset + 1).kind == TokenKind::EXPORT) {
        throw error(peekAt(*offset + 1),
                    "A declaration cannot repeat 'export'.");
      }
      tokens.erase(tokens.begin() +
                   static_cast<std::ptrdiff_t>(current + *offset));
      StmtPtr result = declaration();
      if (result->exportKeyword()) {
        throw error(keyword, "A declaration cannot repeat 'export'.");
      }
      result->setExportKeyword(keyword);
      return result;
    }
    if (match({TokenKind::EXPORT})) {
      const Token keyword = previous();
      StmtPtr result = declaration();
      if (result->exportKeyword()) {
        throw error(keyword, "A declaration cannot repeat 'export'.");
      }
      result->setExportKeyword(keyword);
      return result;
    }
    if (match({TokenKind::HASH_IF, TokenKind::HASH_IFDEF,
               TokenKind::HASH_IFNDEF})) {
      return conditionalCompilation(ItemContext::Declaration);
    }
    if (match({TokenKind::HASH_ERROR})) {
      return compileErrorDirective();
    }
    rejectStrayConditionalDirective();
    if (match({TokenKind::STATIC_ASSERT})) {
      return staticAssertDeclaration(previous());
    }
    if (match({TokenKind::AT})) {
      return attributedDeclaration();
    }
    if (match({TokenKind::EXTERN})) {
      return externCDeclaration(previous());
    }
    if (match({TokenKind::NAMESPACE})) {
      return namespaceDeclaration();
    }
    if (isConformanceStart()) {
      return conformanceDeclaration(advance());
    }
    if (match({TokenKind::CONCEPT})) {
      return conceptDeclaration(previous());
    }
    if (match({TokenKind::USING})) {
      return typeAliasDeclaration(previous());
    }
    if (match({TokenKind::ENUM})) {
      return enumDeclaration(previous());
    }
    if (check(TokenKind::LEFT_BRACKET) &&
        peekAt(1).kind == TokenKind::LEFT_BRACKET) {
      return bracketAttributedDeclaration(LanguageLinkage::Gti, true);
    }
    if (match({TokenKind::CLASS, TokenKind::STRUCT, TokenKind::INTERFACE,
               TokenKind::UNION})) {
      return classDeclaration({}, previous());
    }
    if (match({TokenKind::SEMICOLON})) {
      return std::make_shared<EmptyStmt>(previous());
    }
    if (isStructuredBindingPrefix()) {
      throw error(peek(),
                  "Structured bindings are local declarations and cannot "
                  "appear at namespace scope.");
    }
    if (check(TokenKind::UNSAFE) || isTypedDeclaration()) {
      return typedDeclaration(true);
    }

    throw error(peek(), "Expect a namespace, concept, enum, class, struct, "
                        "interface, union, function, or variable declaration.");
  }

  // 'export' publishes a whole declaration, so it precedes the attribute
  // lists, '@' bindings and specifiers that belong to the declared entity.
  // Returns the offset of an 'export' written after at least one of them.
  [[nodiscard]] std::optional<std::size_t> misplacedExportOffset() const {
    std::size_t offset = 0;
    // A prefix never contains a statement or body delimiter, so an unclosed
    // list stops at the declaration rather than scanning the rest of the unit.
    const auto skipBalanced = [&](TokenKind open, TokenKind close) {
      std::size_t depth = 0;
      do {
        const TokenKind kind = peekAt(offset).kind;
        if (kind == TokenKind::END_OF_FILE || kind == TokenKind::SEMICOLON ||
            kind == TokenKind::LEFT_BRACE || kind == TokenKind::RIGHT_BRACE) {
          return false;
        }
        depth += kind == open;
        depth -= kind == close;
        ++offset;
      } while (depth > 0);
      return true;
    };
    while (true) {
      const TokenKind kind = peekAt(offset).kind;
      if (kind == TokenKind::LEFT_BRACKET &&
          peekAt(offset + 1).kind == TokenKind::LEFT_BRACKET) {
        if (!skipBalanced(TokenKind::LEFT_BRACKET, TokenKind::RIGHT_BRACKET)) {
          return std::nullopt;
        }
      } else if (kind == TokenKind::AT &&
                 peekAt(offset + 1).kind == TokenKind::IDENTIFIER &&
                 peekAt(offset + 2).kind == TokenKind::LEFT_PAREN) {
        offset += 2;
        if (!skipBalanced(TokenKind::LEFT_PAREN, TokenKind::RIGHT_PAREN)) {
          return std::nullopt;
        }
      } else if (kind == TokenKind::UNSAFE || kind == TokenKind::VIRTUAL ||
                 kind == TokenKind::STATIC || kind == TokenKind::CONSTEXPR ||
                 kind == TokenKind::MUT) {
        ++offset;
      } else {
        break;
      }
    }
    if (offset == 0 || peekAt(offset).kind != TokenKind::EXPORT) {
      return std::nullopt;
    }
    return offset;
  }

  void reportMisplacedExport(std::size_t offset) {
    const Token &keyword = peekAt(offset);
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-P0001", DiagnosticPhase::Parsing, keyword,
        "'export' must begin the declaration, before its attributes and "
        "specifiers.");
    diagnostic.hints.emplace_back(
        "Write 'export' first, as in 'export [[track_caller]] void f()'.");
    // The prefix moves verbatim, keeping any comments inside it. Only the
    // whitespace before 'export' is dropped; a comment there withholds the
    // edit rather than discarding or reattaching it.
    const Token &first = peek();
    const Token &last = peekAt(offset - 1);
    const std::size_t prefixEnd = last.position + last.lexeme.size();
    const bool sameSource = std::all_of(
        tokens.begin() + static_cast<std::ptrdiff_t>(current),
        tokens.begin() + static_cast<std::ptrdiff_t>(current + offset + 1),
        [&](const Token &token) {
          return !token.generated && !token.completion &&
                 token.source == keyword.source;
        });
    if (sameSource && first.position <= prefixEnd &&
        prefixEnd <= keyword.position &&
        source.substr(std::min(first.position, source.size()),
                      first.lexeme.size()) == first.lexeme &&
        source.substr(std::min(keyword.position, source.size()),
                      keyword.lexeme.size()) == keyword.lexeme &&
        std::all_of(source.begin() + static_cast<std::ptrdiff_t>(prefixEnd),
                    source.begin() +
                        static_cast<std::ptrdiff_t>(keyword.position),
                    [](char character) {
                      return character == ' ' || character == '\t' ||
                             character == '\n' || character == '\r';
                    })) {
      SourceSpan spelling = tokenSpan(first);
      spelling.end = keyword.position + keyword.lexeme.size();
      diagnostic.fixes.push_back(
          {.span = spelling,
           .replacement =
               "export " + std::string(source.substr(
                               first.position, prefixEnd - first.position)),
           .message = "Move 'export' to the start of the declaration"});
    }
    diagnostics.emplace_back(std::move(diagnostic));
  }

  StmtPtr attributedDeclaration() {
    Token attribute =
        consume(TokenKind::IDENTIFIER, "Expect attribute name after '@'.");
    if (attribute.lexeme != "runtime" &&
        attribute.lexeme != "compiler_constraint" &&
        attribute.lexeme != "compiler_type") {
      throw error(attribute,
                  "Unknown declaration attribute '@" + attribute.lexeme + "'.");
    }
    consume(TokenKind::LEFT_PAREN,
            "Expect '(' after '@" + attribute.lexeme + "'.");
    Token binding =
        consume(TokenKind::STRING_LITERAL, "Expect attribute binding name.");
    consume(TokenKind::RIGHT_PAREN, "Expect ')' after attribute binding.");
    const auto *bindingName = std::get_if<std::string>(&binding.literal);
    const std::string name =
        bindingName == nullptr ? std::string{} : *bindingName;

    if (attribute.lexeme == "compiler_constraint") {
      Token keyword =
          consume(TokenKind::CONCEPT,
                  "Compiler constraint bindings must annotate a concept.");
      return conceptDeclaration(
          std::move(keyword),
          CompilerConstraintBinding{std::move(attribute), name});
    }
    if (attribute.lexeme == "compiler_type") {
      if (!match({TokenKind::CLASS, TokenKind::STRUCT})) {
        throw error(peek(),
                    "Compiler type bindings must annotate a class or struct.");
      }
      return classDeclaration({}, previous(),
                              CompilerTypeBinding{std::move(attribute), name});
    }
    if (!isTypedDeclaration()) {
      throw error(peek(), "Runtime binding must annotate a function.");
    }

    return typedDeclaration(
        true, RuntimeBinding{std::move(attribute), std::move(name)});
  }

  StmtPtr externCDeclaration(Token keyword) {
    Token language = consume(TokenKind::STRING_LITERAL,
                             "Expect string literal \"C\" after 'extern'.");
    const auto *languageName = std::get_if<std::string>(&language.literal);
    if (languageName == nullptr || *languageName != "C") {
      throw error(language,
                  "GTI currently supports only the extern \"C\" linkage.");
    }

    consume(TokenKind::LEFT_BRACE,
            "Expect '{' before extern \"C\" declarations.");
    StmtList declarations;
    while (!check(TokenKind::RIGHT_BRACE) && !isAtEnd()) {
      try {
        const Token declarationStart = peek();
        StmtPtr declaration;
        if (check(TokenKind::LEFT_BRACKET) &&
            peekAt(1).kind == TokenKind::LEFT_BRACKET) {
          declaration = bracketAttributedDeclaration(LanguageLinkage::C);
        } else if (!isTypedDeclaration()) {
          throw error(
              peek(),
              "An extern \"C\" block may contain only function or variable "
              "declarations.");
        } else {
          declaration = typedDeclaration(true, std::nullopt, false, false, true,
                                         LanguageLinkage::C);
        }
        stampExtent(declaration, declarationStart);
        const auto *function = stmtAs<FunctionDecl>(declaration.get());
        if (function == nullptr &&
            stmtAs<VariableDecl>(declaration.get()) == nullptr) {
          throw error(
              declarationStart,
              "An extern \"C\" block may contain only function or variable "
              "declarations.");
        }
        if (function != nullptr && (function->body() || function->isPure())) {
          throw error(function->name(),
                      "An extern \"C\" function must be a bodyless "
                      "declaration ending in ';'.");
        }
        declarations.emplace_back(std::move(declaration));
      } catch (const ParseError &) {
        synchronize(true, false, false);
      }
    }
    consume(TokenKind::RIGHT_BRACE,
            "Expect '}' after extern \"C\" declarations.");
    return std::make_shared<ExternCDecl>(
        std::move(keyword), std::move(language), std::move(declarations));
  }

  StmtPtr namespaceDeclaration() {
    Token name = consume(TokenKind::IDENTIFIER, "Expect namespace name.");
    if (match({TokenKind::EQUAL})) {
      NamePath target = parseNamePath();
      consume(TokenKind::SEMICOLON, "Expect ';' after namespace alias.");
      return std::make_shared<NamespaceAliasDecl>(name, std::move(target));
    }

    return namespaceBody(std::move(name));
  }

  StmtPtr namespaceBody(Token name) {
    if (match({TokenKind::SCOPE})) {
      Token childName =
          consume(TokenKind::IDENTIFIER, "Expect namespace name after '::'.");
      // Shorthand retains the ordinary nested namespace representation and
      // depth budget, including each component's exact declaration token.
      const NestingGuard nesting = enterNesting(childName);
      StmtPtr child = namespaceBody(childName);
      stampExtent(child, childName);
      StmtList declarations;
      declarations.emplace_back(std::move(child));
      return std::make_shared<NamespaceDecl>(std::move(name),
                                             std::move(declarations));
    }

    consume(TokenKind::LEFT_BRACE, "Expect '{' before namespace body.");
    StmtList declarations;
    while (!check(TokenKind::RIGHT_BRACE) && !isAtEnd()) {
      try {
        declarations.emplace_back(declaration());
      } catch (const ParseError &) {
        synchronize(true, false, true);
      }
    }
    consume(TokenKind::RIGHT_BRACE, "Expect '}' after namespace body.");
    return std::make_shared<NamespaceDecl>(name, std::move(declarations));
  }

  StmtPtr typeAliasDeclaration(Token keyword) {
    Token name =
        consume(TokenKind::IDENTIFIER, "Expect type alias name after 'using'.");
    consume(TokenKind::EQUAL, "Expect '=' after type alias name.");
    std::optional<Token> access;
    if (match({TokenKind::MUT}))
      access = previous();
    TypeRef target = parseType();
    target.referenceMutability = std::move(access);
    consume(TokenKind::SEMICOLON, "Expect ';' after type alias declaration.");
    return std::make_shared<TypeAliasDecl>(std::move(keyword), std::move(name),
                                           std::move(target));
  }

  StmtPtr conceptDeclaration(
      Token keyword,
      std::optional<CompilerConstraintBinding> compilerBinding = std::nullopt,
      std::optional<ConceptExplanation> explanation = std::nullopt) {
    Token name = consume(TokenKind::IDENTIFIER, "Expect concept name.");
    consume(TokenKind::LESS, "Expect '<' after concept name.");
    std::vector<Token> typeParameters;
    do {
      typeParameters.emplace_back(
          consume(TokenKind::IDENTIFIER,
                  "Expect a type parameter in concept declaration."));
    } while (match({TokenKind::COMMA}));
    consumeGenericClose("Expect '>' after concept type parameters.");

    ConstraintExpression requirements;
    std::optional<ProtocolConceptBody> protocolBody;
    if (compilerBinding) {
      consume(TokenKind::SEMICOLON,
              "Compiler-bound concepts are declarations and must end in ';'.");
    } else if (check(TokenKind::LEFT_BRACE)) {
      protocolBody = protocolConceptBody();
      consume(TokenKind::SEMICOLON, "Expect ';' after concept declaration.");
    } else {
      consume(TokenKind::EQUAL, "Expect '=' after concept parameter list.");
      requirements = constraintExpression();
      consume(TokenKind::SEMICOLON, "Expect ';' after concept declaration.");
    }

    return std::make_shared<ConceptDecl>(
        std::move(keyword), std::move(name), std::move(typeParameters),
        std::move(requirements), std::move(compilerBinding),
        std::move(protocolBody), std::move(explanation));
  }

  // `{ typename name; Result operation(Parameters); ... }`. Members are only
  // parsed here; semantics validates their names and signatures.
  ProtocolConceptBody protocolConceptBody() {
    ProtocolConceptBody body;
    body.leftBrace =
        consume(TokenKind::LEFT_BRACE, "Expect '{' before concept body.");
    while (!check(TokenKind::RIGHT_BRACE) && !isAtEnd()) {
      const Token start = peek();
      try {
        if (match({TokenKind::TYPENAME})) {
          Token keyword = previous();
          Token name =
              consume(TokenKind::IDENTIFIER,
                      "Expect an associated type name after 'typename'.");
          consume(TokenKind::SEMICOLON,
                  "Expect ';' after associated type declaration.");
          body.associatedTypes.push_back(
              {.keyword = std::move(keyword), .name = std::move(name)});
          continue;
        }
        StmtPtr operation =
            check(TokenKind::LEFT_BRACKET) &&
                    peekAt(1).kind == TokenKind::LEFT_BRACKET
                ? bracketAttributedDeclaration(LanguageLinkage::Gti)
                : typedDeclaration(true);
        const auto *function =
            dynamic_cast<const FunctionDecl *>(operation.get());
        if (function == nullptr || function->body() != nullptr ||
            function->isPure()) {
          (void)error(start, "Concept body members are 'typename' associated "
                             "types or operation declarations ending in "
                             "';'.");
          continue;
        }
        stampExtent(operation, start);
        body.operations.push_back(std::move(operation));
      } catch (const ParseError &) {
        synchronize(true, false, false, true);
      }
    }
    consume(TokenKind::RIGHT_BRACE, "Expect '}' after concept body.");
    return body;
  }

  [[nodiscard]] bool isConformanceStart() const {
    return check(TokenKind::FOR) && (peekAt(1).kind == TokenKind::LESS ||
                                     peekAt(1).kind == TokenKind::CONCEPT);
  }

  // `for <Parameters> concept Concept<Pattern> { using ...; functions };`
  //
  // The block's functions are ordinary generic functions over the block's
  // parameters. They are placed, with the ConformanceDecl that semantics
  // validates and registers, in a namespace whose generated `__gti_` name no
  // source can spell, so a block operation is reachable only through its
  // concept. The name derives from the concept application's tokens, so it
  // is stable across compilations and distinct for each block.
  StmtPtr conformanceDeclaration(Token forKeyword) {
    std::vector<GenericParameter> genericParameters;
    if (check(TokenKind::LESS)) {
      genericParameters = genericParameterList();
    }
    Token conceptKeyword =
        consume(TokenKind::CONCEPT,
                "Expect 'concept' after the conformance block's parameters.");
    const std::size_t targetStart = current;
    ConceptApplication target = conceptApplication();
    std::string targetSpelling;
    for (std::size_t index = targetStart; index < current; ++index) {
      targetSpelling += tokens[index].lexeme;
      targetSpelling.push_back(' ');
    }
    // Each operation requires the block's own application, which the block
    // proves; inside it, `Concept<Pattern>::name` is therefore available.
    // The generated keyword marks the restatement: semantics validates the
    // block's patterns once, for the block, and not again for each copy.
    RequiresClause blockRequirement;
    blockRequirement.keyword = forKeyword;
    blockRequirement.keyword.kind = TokenKind::REQUIRES;
    blockRequirement.keyword.lexeme = "requires";
    blockRequirement.keyword.generated = true;
    blockRequirement.expression.applications.push_back(target);
    blockRequirement.expression.nodes.push_back(
        {.kind = ConstraintExpressionKind::Application, .application = 0});
    blockRequirement.expression.root = 0;
    consume(TokenKind::LEFT_BRACE, "Expect '{' before conformance block body.");
    StmtList members;
    StmtList functions;
    while (!check(TokenKind::RIGHT_BRACE) && !isAtEnd()) {
      const Token start = peek();
      try {
        StmtPtr member;
        if (match({TokenKind::USING})) {
          member = typeAliasDeclaration(previous());
        } else {
          conformanceGenericParameters = genericParameters;
          conformanceRequirement = blockRequirement;
          member = check(TokenKind::LEFT_BRACKET) &&
                           peekAt(1).kind == TokenKind::LEFT_BRACKET
                       ? bracketAttributedDeclaration(LanguageLinkage::Gti)
                       : typedDeclaration(true);
          conformanceGenericParameters.reset();
          conformanceRequirement.reset();
          const auto *function =
              dynamic_cast<const FunctionDecl *>(member.get());
          if (function == nullptr || function->body() == nullptr) {
            (void)error(start, "Conformance block members are 'using' "
                               "associated types or operation definitions "
                               "with bodies.");
            continue;
          }
          functions.push_back(member);
        }
        stampExtent(member, start);
        members.push_back(std::move(member));
      } catch (const ParseError &) {
        conformanceGenericParameters.reset();
        conformanceRequirement.reset();
        synchronize(true, false, false, true);
      }
    }
    consume(TokenKind::RIGHT_BRACE, "Expect '}' after conformance block body.");
    consume(TokenKind::SEMICOLON, "Expect ';' after conformance block.");

    auto conformance = std::make_shared<ConformanceDecl>(
        forKeyword, std::move(genericParameters), std::move(conceptKeyword),
        std::move(target), std::move(members));
    stampExtent(conformance, forKeyword);
    StmtList declarations;
    declarations.push_back(std::move(conformance));
    for (StmtPtr &function : functions) {
      declarations.push_back(std::move(function));
    }
    Token carrier = std::move(forKeyword);
    carrier.kind = TokenKind::IDENTIFIER;
    carrier.lexeme = "__gti_conformance_" + stableNameDigest(targetSpelling);
    carrier.generated = true;
    return std::make_shared<NamespaceDecl>(std::move(carrier),
                                           std::move(declarations));
  }

  // FNV-1a over the spelling: stable across compilations and platforms.
  static std::string stableNameDigest(std::string_view spelling) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char character : spelling) {
      hash ^= static_cast<unsigned char>(character);
      hash *= 1099511628211ULL;
    }
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string result(16, '0');
    for (std::size_t index = 0; index < result.size(); ++index) {
      result[result.size() - 1 - index] = digits[hash & 0xfU];
      hash >>= 4U;
    }
    return result;
  }

  ConceptApplication conceptApplication() {
    NamePath name = parseNamePath();
    consume(TokenKind::LESS, "Expect '<' after concept name.");
    std::vector<TypeRef> arguments;
    do {
      arguments.emplace_back(parseType());
    } while (match({TokenKind::COMMA}));
    consumeGenericClose("Expect '>' after concept arguments.");
    return {std::move(name), std::move(arguments)};
  }

  ConstraintExpression constraintExpression() {
    ConstraintExpression expression;
    expression.root = constraintDisjunction(expression);
    return expression;
  }

  std::size_t constraintDisjunction(ConstraintExpression &expression) {
    std::size_t left = constraintConjunction(expression);
    while (match({TokenKind::OR})) {
      Token operation = previous();
      const std::size_t right = constraintConjunction(expression);
      expression.nodes.push_back({.kind = ConstraintExpressionKind::Disjunction,
                                  .left = left,
                                  .right = right,
                                  .operation = std::move(operation)});
      left = expression.nodes.size() - 1;
    }
    return left;
  }

  std::size_t constraintConjunction(ConstraintExpression &expression) {
    std::size_t left = constraintPrimary(expression);
    while (match({TokenKind::AND})) {
      Token operation = previous();
      const std::size_t right = constraintPrimary(expression);
      expression.nodes.push_back({.kind = ConstraintExpressionKind::Conjunction,
                                  .left = left,
                                  .right = right,
                                  .operation = std::move(operation)});
      left = expression.nodes.size() - 1;
    }
    return left;
  }

  std::size_t constraintPrimary(ConstraintExpression &expression) {
    if (match({TokenKind::REQUIRES})) {
      auto requirement = memberRequirement(previous());
      TypeRef receiver = requirement->receiver();
      receiver.reference.reset();
      // The synthetic constraint argument names the referent type. Keep
      // receiver access on the requirement, not on this non-reference type.
      receiver.referenceMutability.reset();
      const std::size_t application = expression.applications.size();
      std::vector<TypeRef> arguments{std::move(receiver),
                                     requirement->result()};
      arguments.insert(arguments.end(), requirement->parameters().begin(),
                       requirement->parameters().end());
      expression.applications.push_back(
          {.name = NamePath{{requirement->member()}},
           .arguments = std::move(arguments),
           .memberRequirement = std::move(requirement)});
      expression.nodes.push_back({.kind = ConstraintExpressionKind::Application,
                                  .application = application});
      return expression.nodes.size() - 1;
    }
    if (match({TokenKind::LEFT_PAREN})) {
      const NestingGuard nesting = enterNesting(previous());
      const std::size_t nested = constraintDisjunction(expression);
      consume(TokenKind::RIGHT_PAREN,
              "Expect ')' after parenthesized constraint expression.");
      return nested;
    }
    const std::size_t application = expression.applications.size();
    expression.applications.emplace_back(conceptApplication());
    expression.nodes.push_back({.kind = ConstraintExpressionKind::Application,
                                .application = application});
    return expression.nodes.size() - 1;
  }

  // A qualified name after `[[` is a declared attribute
  // (`[[json::name(...)]]`), not a compiler attribute; one group holds one or
  // more of them.
  [[nodiscard]] bool declaredAttributeAhead() const {
    return check(TokenKind::IDENTIFIER) && peekAt(1).kind == TokenKind::SCOPE;
  }

  void
  parseDeclaredAttributeGroup(const Token &opening,
                              std::vector<DeclarationAttribute> &declared) {
    while (true) {
      Token first = consume(TokenKind::IDENTIFIER,
                            "Expect an attribute name after '[['.");
      DeclarationAttribute attribute{.open = opening,
                                     .name = parseNamePath(std::move(first)),
                                     .paren = Token(previous())};
      if (match({TokenKind::LEFT_PAREN})) {
        attribute.paren = previous();
        const bool designated = check(TokenKind::DOT);
        if (!check(TokenKind::RIGHT_PAREN)) {
          while (true) {
            if (designated) {
              consume(TokenKind::DOT,
                      "Expect '.field = value' in a designated attribute "
                      "argument list.");
              attribute.designators.push_back(consume(
                  TokenKind::IDENTIFIER, "Expect a field name after '.'."));
              consume(TokenKind::EQUAL,
                      "Expect '=' after the designated field name.");
            }
            attribute.arguments.push_back(assignment());
            if (!match({TokenKind::COMMA})) {
              break;
            }
          }
        }
        consume(TokenKind::RIGHT_PAREN,
                "Expect ')' after the attribute arguments.");
      }
      declared.push_back(std::move(attribute));
      if (check(TokenKind::COMMA) && peekAt(1).kind == TokenKind::IDENTIFIER &&
          peekAt(2).kind == TokenKind::SCOPE) {
        advance();
        continue;
      }
      break;
    }
    consume(TokenKind::RIGHT_BRACKET,
            "Expect ']]' after declaration attributes.");
    consume(TokenKind::RIGHT_BRACKET,
            "Expect ']]' after declaration attributes.");
  }

  // Attaches declared attributes to the declaration they precede; only
  // fields and class-like declarations accept them in this slice.
  StmtPtr attachDeclaredAttributes(StmtPtr declaration,
                                   std::vector<DeclarationAttribute> declared) {
    if (declared.empty()) {
      return declaration;
    }
    if (auto *variable = dynamic_cast<VariableDecl *>(declaration.get());
        variable != nullptr && currentClassName.has_value()) {
      variable->setDeclarationAttributes(std::move(declared));
      return declaration;
    }
    if (auto *classDecl = dynamic_cast<ClassDecl *>(declaration.get())) {
      classDecl->setDeclarationAttributes(std::move(declared));
      return declaration;
    }
    throw error(declared.front().open,
                "Declared attributes apply to fields and to class, struct, "
                "interface, or union declarations.");
  }

  // Concepts, and so `[[unsatisfied(...)]]`, are declared only at namespace
  // scope.
  StmtPtr bracketAttributedDeclaration(LanguageLinkage linkage,
                                       bool namespaceScope = false) {
    std::optional<NativeCArrayAttribute> nativeCArray;
    std::optional<NativeLinkNameAttribute> nativeLinkName;
    std::vector<FunctionAttribute> functionAttributes;
    std::vector<DeclarationAttribute> declared;
    bool parsedNativeFunctionAttribute = false;
    while (true) {
      const Token opening = peek();
      consume(TokenKind::LEFT_BRACKET,
              "Expect '[[' before declaration attributes.");
      consume(TokenKind::LEFT_BRACKET,
              "Expect '[[' before declaration attributes.");
      if (declaredAttributeAhead()) {
        parseDeclaredAttributeGroup(opening, declared);
        if (check(TokenKind::LEFT_BRACKET) &&
            peekAt(1).kind == TokenKind::LEFT_BRACKET) {
          continue;
        }
        if (match({TokenKind::CLASS, TokenKind::STRUCT, TokenKind::INTERFACE,
                   TokenKind::UNION})) {
          return attachDeclaredAttributes(classDeclaration({}, previous()),
                                          std::move(declared));
        }
        if (!isTypedDeclaration()) {
          throw error(peek(),
                      "Declared attributes must annotate a field or a "
                      "class, struct, interface, or union declaration.");
        }
        return attachDeclaredAttributes(
            typedDeclaration(true, std::nullopt, currentClassName.has_value(),
                             currentClassName.has_value(), true, linkage,
                             std::move(nativeCArray), std::move(nativeLinkName),
                             std::nullopt, std::move(functionAttributes)),
            std::move(declared));
      }
      if (match({TokenKind::UNSAFE})) {
        Token keyword = previous();
        if (parsedNativeFunctionAttribute) {
          throw error(keyword,
                      "[[unsafe]] cannot be combined with a native function "
                      "attribute.");
        }
        consume(TokenKind::RIGHT_BRACKET,
                "Expect ']]' after unsafe attribute.");
        consume(TokenKind::RIGHT_BRACKET,
                "Expect ']]' after unsafe attribute.");
        if (linkage != LanguageLinkage::Gti || currentClassName ||
            !isTypedDeclaration()) {
          throw error(keyword,
                      "[[unsafe]] requires a namespace-scope GTI function.");
        }
        // Keep the recovered declaration for tooling, but the obsolete spelling
        // is a syntax error and can never pass the frontend generation gate.
        SourceSpan spelling = tokenSpan(opening);
        spelling.end = previous().position + previous().lexeme.size();
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-P0001", DiagnosticPhase::Parsing, spelling,
            "The '[[unsafe]]' attribute has been replaced by the 'unsafe' "
            "declaration keyword.");
        diagnostic.hints.emplace_back(
            "Write 'unsafe' before the function type.");
        // Do not discard comments embedded in a non-contiguous attribute.
        if (spelling.end - spelling.start == 10) {
          // Attribute delimiters separated the following type/modifier even
          // without written whitespace. The replacement keyword must do so too.
          const bool adjacent = peek().source == opening.source &&
                                peek().position == spelling.end;
          diagnostic.fixes.push_back(
              {.span = spelling,
               .replacement = adjacent ? "unsafe " : "unsafe",
               .message = "Replace obsolete '[[unsafe]]' with 'unsafe'"});
        }
        diagnostics.emplace_back(std::move(diagnostic));
        return typedDeclaration(true, std::nullopt, false, false, true, linkage,
                                std::nullopt, std::nullopt, std::move(keyword),
                                std::move(functionAttributes));
      }
      Token first = consume(TokenKind::IDENTIFIER,
                            "Expect an attribute name after '[['.");
      if (first.lexeme == "c_array" || first.lexeme == "link_name") {
        if (std::any_of(functionAttributes.begin(), functionAttributes.end(),
                        [](const FunctionAttribute &attribute) {
                          return attribute.name.lexeme != "discardable";
                        })) {
          throw error(first, "GTI function attributes cannot be combined with "
                             "native function attributes.");
        }
        parsedNativeFunctionAttribute = true;
        consume(TokenKind::LEFT_PAREN,
                "Expect '(' after native function attribute '" + first.lexeme +
                    "'.");
        if (first.lexeme == "c_array") {
          if (nativeCArray) {
            throw error(first, "Duplicate [[c_array(...)]] attribute.");
          }
          Token count = consume(
              TokenKind::IDENTIFIER,
              "Expect the count out-parameter name in '[[c_array(...)]]'.");
          nativeCArray =
              NativeCArrayAttribute{.attribute = std::move(first),
                                    .countParameter = std::move(count)};
          consume(TokenKind::RIGHT_PAREN,
                  "Expect ')' after the c_array count parameter.");
        } else {
          if (nativeLinkName) {
            throw error(first, "Duplicate [[link_name(...)]] attribute.");
          }
          Token symbol =
              consume(TokenKind::STRING_LITERAL,
                      "Expect a quoted native symbol in '[[link_name(...)]]'.");
          nativeLinkName = NativeLinkNameAttribute{
              .attribute = std::move(first), .symbol = std::move(symbol)};
          consume(TokenKind::RIGHT_PAREN,
                  "Expect ')' after the link_name symbol.");
        }
        consume(TokenKind::RIGHT_BRACKET,
                "Expect ']]' after the native function attribute.");
        consume(TokenKind::RIGHT_BRACKET,
                "Expect ']]' after the native function attribute.");
        if (check(TokenKind::LEFT_BRACKET) &&
            peekAt(1).kind == TokenKind::LEFT_BRACKET) {
          continue;
        }
        if (!isTypedDeclaration()) {
          throw error(peek(), "A native function attribute must annotate a "
                              "function declaration.");
        }
        return typedDeclaration(true, std::nullopt, false, false, true, linkage,
                                std::move(nativeCArray),
                                std::move(nativeLinkName), std::nullopt,
                                std::move(functionAttributes));
      }
      if (parsedNativeFunctionAttribute && first.lexeme != "discardable") {
        throw error(first,
                    "Only [[c_array(...)]], [[link_name(...)]] and "
                    "[[discardable]] may be combined on a native function.");
      }

      auto parseAttribute = [&](Token name) {
        FunctionAttribute attribute{.name = std::move(name),
                                    .argument = std::nullopt};
        if (attribute.name.lexeme == "borrowed_from") {
          consume(TokenKind::LEFT_PAREN, "Expect '(' after 'borrowed_from'.");
          if (!match({TokenKind::IDENTIFIER, TokenKind::THIS})) {
            throw error(peek(), "Expect an owner parameter name or 'this' in "
                                "'[[borrowed_from(...)]]'.");
          }
          attribute.argument = previous();
          consume(TokenKind::RIGHT_PAREN,
                  "Expect ')' after the borrowed_from owner.");
        } else if (attribute.name.lexeme == "unsatisfied") {
          consume(TokenKind::LEFT_PAREN, "Expect '(' after 'unsatisfied'.");
          attribute.argument =
              consume(TokenKind::STRING_LITERAL,
                      "Expect a quoted explanation in '[[unsatisfied(...)]]'.");
          consume(TokenKind::RIGHT_PAREN,
                  "Expect ')' after the unsatisfied explanation.");
        } else if ((attribute.name.lexeme == "unsafe_transfer" ||
                    attribute.name.lexeme == "unsafe_share") &&
                   match({TokenKind::LEFT_PAREN})) {
          attribute.argument = consume(TokenKind::IDENTIFIER,
                                       "Expect a generic type parameter in the "
                                       "conditional unsafe assertion.");
          consume(TokenKind::RIGHT_PAREN,
                  "Expect ')' after the capability type parameter.");
        } else if (check(TokenKind::LEFT_PAREN)) {
          if (attribute.name.lexeme == "test") {
            throw error(attribute.name, "[[test]] takes no arguments; annotate "
                                        "a zero-argument void function.");
          }
          throw error(attribute.name, "Unknown function attribute '[[" +
                                          attribute.name.lexeme + "(...)]]'.");
        }
        return attribute;
      };
      std::vector<FunctionAttribute> attributes;
      attributes.emplace_back(parseAttribute(std::move(first)));
      while (match({TokenKind::COMMA})) {
        attributes.emplace_back(parseAttribute(consume(
            TokenKind::IDENTIFIER, "Expect an attribute name after '[['.")));
      }
      consume(TokenKind::RIGHT_BRACKET,
              "Expect ']]' after declaration attributes.");
      consume(TokenKind::RIGHT_BRACKET,
              "Expect ']]' after declaration attributes.");
      const auto explanation =
          std::find_if(attributes.begin(), attributes.end(),
                       [](const FunctionAttribute &attribute) {
                         return attribute.name.lexeme == "unsatisfied";
                       });
      if (explanation != attributes.end() && !check(TokenKind::CONCEPT)) {
        throw error(explanation->name, "[[unsatisfied(...)]] must directly "
                                       "precede a concept declaration.");
      }
      if (check(TokenKind::CONCEPT)) {
        // A concept's only attribute states what satisfies it.
        if (explanation == attributes.end() || attributes.size() != 1 ||
            !functionAttributes.empty() || !declared.empty()) {
          const Token &extra =
              !declared.empty()             ? declared.front().name.last()
              : !functionAttributes.empty() ? functionAttributes.front().name
              : explanation == attributes.begin() ? attributes.back().name
                                                  : attributes.front().name;
          throw error(extra, "A concept declaration accepts only one "
                             "[[unsatisfied(\"...\")]] attribute.");
        }
        if (!namespaceScope) {
          throw error(peek(),
                      "Concept declarations are limited to namespace scope.");
        }
        ConceptExplanation stated{.attribute = explanation->name,
                                  .literal = *explanation->argument};
        if (stated.text().empty()) {
          throw error(stated.literal,
                      "An [[unsatisfied(...)]] explanation cannot be empty.");
        }
        Token keyword = advance();
        return conceptDeclaration(std::move(keyword), std::nullopt,
                                  std::move(stated));
      }
      // Attribute spelling is semantic policy. A typed declaration identifies
      // the function-attribute context even when every attribute is unknown.
      bool functionAttributeList =
          !functionAttributes.empty() || isTypedDeclaration();
      for (const FunctionAttribute &attribute : attributes) {
        if (parsedNativeFunctionAttribute &&
            attribute.name.lexeme != "discardable") {
          throw error(attribute.name,
                      "Only [[discardable]] may be combined with native "
                      "function attributes.");
        }
        functionAttributeList =
            functionAttributeList || attribute.name.lexeme == "noreturn" ||
            attribute.name.lexeme == "test" ||
            attribute.name.lexeme == "track_caller" ||
            attribute.name.lexeme == "thread_confined_callback" ||
            attribute.name.lexeme == "discardable" ||
            attribute.name.lexeme == "borrowed_from";
      }
      if (functionAttributeList) {
        for (FunctionAttribute &attribute : attributes) {
          functionAttributes.push_back(std::move(attribute));
        }
        if (check(TokenKind::LEFT_BRACKET) &&
            peekAt(1).kind == TokenKind::LEFT_BRACKET) {
          continue;
        }
        if (!isTypedDeclaration()) {
          throw error(peek(), "Function attributes require a function "
                              "declaration.");
        }
        return attachDeclaredAttributes(
            typedDeclaration(true, std::nullopt, currentClassName.has_value(),
                             currentClassName.has_value(), true, linkage,
                             std::move(nativeCArray), std::move(nativeLinkName),
                             std::nullopt, std::move(functionAttributes)),
            std::move(declared));
      }
      if (!match({TokenKind::CLASS, TokenKind::STRUCT, TokenKind::INTERFACE,
                  TokenKind::UNION})) {
        throw error(peek(), "These attributes apply only to class, "
                            "struct, interface, or union declarations.");
      }
      std::vector<ClassAttribute> classAttributes;
      for (FunctionAttribute &attribute : attributes) {
        classAttributes.push_back(
            {.name = std::move(attribute.name),
             .capabilityParameter = std::move(attribute.argument)});
      }
      return attachDeclaredAttributes(
          classDeclaration(std::move(classAttributes), previous()),
          std::move(declared));
    }
  }

  StmtPtr classDeclaration(
      std::vector<ClassAttribute> attributes, Token keyword,
      std::optional<CompilerTypeBinding> compilerTypeBinding = std::nullopt) {
    const NestingGuard nesting = enterNesting(keyword);
    const ClassKind kind =
        keyword.kind == TokenKind::STRUCT      ? ClassKind::Struct
        : keyword.kind == TokenKind::INTERFACE ? ClassKind::Interface
        : keyword.kind == TokenKind::UNION     ? ClassKind::Union
                                               : ClassKind::Class;
    if (currentClassName &&
        ((kind != ClassKind::Class && kind != ClassKind::Struct) ||
         (currentClassKind != ClassKind::Class &&
          currentClassKind != ClassKind::Struct))) {
      throw error(keyword,
                  "Only named classes and structs can be nested inside "
                  "classes and structs.");
    }
    Token name = consume(TokenKind::IDENTIFIER, "Expect type name.");
    std::optional<TypeRef> exactSpecialization;
    std::vector<GenericParameter> genericParameters;
    std::vector<TypeRef> genericArgumentPatterns;
    if (check(TokenKind::SCOPE)) {
      if (currentClassName) {
        throw error(name,
                    "Exact qualified class specializations are limited to "
                    "namespace scope.");
      }
      NamePath target = parseNamePath(name);
      if (!check(TokenKind::LESS)) {
        throw error(target.last(),
                    "A qualified class declaration must name an exact "
                    "specialization with generic arguments.");
      }
      std::vector<TypeRef> arguments = typeArgumentList();
      name = target.last();
      exactSpecialization.emplace(std::move(target), std::move(arguments));
    } else if (check(TokenKind::LESS)) {
      if (classSignaturePatternAhead()) {
        genericArgumentPatterns = typeArgumentList();
      } else {
        genericParameters = genericParameterList();
      }
    }
    std::vector<BaseSpecifier> bases;
    if (match({TokenKind::COLON})) {
      do {
        std::optional<Token> access;
        if (match({TokenKind::PUBLIC, TokenKind::PRIVATE,
                   TokenKind::PROTECTED})) {
          access = previous();
        }
        bases.push_back({.access = std::move(access), .type = parseBaseType()});
      } while (match({TokenKind::COMMA}));
    }
    if (match({TokenKind::SEMICOLON})) {
      return std::make_shared<ClassDecl>(
          std::move(attributes), std::move(keyword), kind, std::move(name),
          std::move(exactSpecialization), std::move(genericParameters),
          std::move(bases), StmtList{}, true,
          std::move(genericArgumentPatterns), std::move(compilerTypeBinding));
    }
    consume(TokenKind::LEFT_BRACE, "Expect '{' before type body.");

    StmtList members;
    const std::optional<Token> enclosingClassName = currentClassName;
    const std::optional<ClassKind> enclosingClassKind = currentClassKind;
    currentClassName = name;
    currentClassKind = kind;
    try {
      while (!check(TokenKind::RIGHT_BRACE) && !isAtEnd()) {
        try {
          members.emplace_back(item(ItemContext::ClassMember));
        } catch (const ParseError &) {
          synchronize(true, false, false, true);
        }
      }

      consume(TokenKind::RIGHT_BRACE, "Expect '}' after type body.");
      consume(TokenKind::SEMICOLON, "Expect ';' after type declaration.");
    } catch (...) {
      currentClassName = enclosingClassName;
      currentClassKind = enclosingClassKind;
      throw;
    }
    currentClassName = enclosingClassName;
    currentClassKind = enclosingClassKind;

    return std::make_shared<ClassDecl>(
        std::move(attributes), std::move(keyword), kind, name,
        std::move(exactSpecialization), std::move(genericParameters),
        std::move(bases), std::move(members), false,
        std::move(genericArgumentPatterns), std::move(compilerTypeBinding));
  }

  StmtPtr enumDeclaration(Token keyword) {
    std::optional<Token> classKeyword;
    if (match({TokenKind::CLASS})) {
      classKeyword = previous();
    }
    Token name = consume(TokenKind::IDENTIFIER, "Expect scoped enum name.");
    std::vector<GenericParameter> genericParameters;
    if (check(TokenKind::LESS)) {
      genericParameters = genericParameterList();
    }
    std::optional<TypeRef> underlyingType;
    if (match({TokenKind::COLON})) {
      underlyingType = parseType();
    }
    consume(TokenKind::LEFT_BRACE, "Expect '{' before scoped enum body.");

    std::vector<EnumeratorDecl> enumerators;
    if (!check(TokenKind::RIGHT_BRACE)) {
      do {
        Token enumerator =
            consume(TokenKind::IDENTIFIER, "Expect enumerator name.");
        std::vector<Parameter> payload;
        if (match({TokenKind::LEFT_PAREN})) {
          payload = parameterList();
        }
        ExprPtr initializer;
        if (match({TokenKind::EQUAL})) {
          initializer = assignment();
        }
        enumerators.push_back({std::move(enumerator), std::move(initializer),
                               std::move(payload)});
      } while (match({TokenKind::COMMA}) && !check(TokenKind::RIGHT_BRACE));
    }

    consume(TokenKind::RIGHT_BRACE, "Expect '}' after scoped enum body.");
    consume(TokenKind::SEMICOLON, "Expect ';' after scoped enum declaration.");
    return std::make_shared<EnumDecl>(
        std::move(keyword), std::move(classKeyword), std::move(name),
        std::move(underlyingType), std::move(enumerators),
        std::move(genericParameters));
  }

  StmtPtr typedDeclaration(
      bool allowFunction,
      std::optional<RuntimeBinding> runtimeBinding = std::nullopt,
      bool allowMutableReceiver = false, bool allowOperators = false,
      bool allowStatic = true, LanguageLinkage linkage = LanguageLinkage::Gti,
      std::optional<NativeCArrayAttribute> nativeCArray = std::nullopt,
      std::optional<NativeLinkNameAttribute> nativeLinkName = std::nullopt,
      std::optional<Token> unsafeKeyword = std::nullopt,
      std::vector<FunctionAttribute> functionAttributes = {}) {
    // Taken at entry, so declarations nested in the body never see them.
    std::optional<std::vector<GenericParameter>> blockParameters =
        std::exchange(conformanceGenericParameters, std::nullopt);
    std::optional<RequiresClause> blockRequirement =
        std::exchange(conformanceRequirement, std::nullopt);
    if (match({TokenKind::UNSAFE})) {
      if (unsafeKeyword) {
        throw error(previous(), "A function cannot repeat 'unsafe'.");
      }
      unsafeKeyword = previous();
    }
    if (unsafeKeyword &&
        (!allowFunction || runtimeBinding || linkage != LanguageLinkage::Gti)) {
      throw error(*unsafeKeyword,
                  "An unsafe declaration requires a GTI function or method, "
                  "not a native binding.");
    }
    if (unsafeKeyword && check(TokenKind::UNSAFE)) {
      throw error(peek(), "A function cannot repeat 'unsafe'.");
    }
    std::optional<Token> virtualKeyword;
    if (match({TokenKind::VIRTUAL})) {
      virtualKeyword = previous();
    }
    std::optional<Token> staticKeyword;
    if (match({TokenKind::STATIC})) {
      staticKeyword = previous();
      if (!allowStatic) {
        throw error(
            *staticKeyword,
            "Block-scope static declarations are not supported yet; declare "
            "static storage at namespace or class scope.");
      }
      if (runtimeBinding) {
        throw error(*staticKeyword,
                    "Runtime-bound functions cannot be declared static.");
      }
    }
    std::optional<Token> constexprKeyword;
    if (match({TokenKind::CONSTEXPR})) {
      constexprKeyword = previous();
    }
    std::optional<Token> mutKeyword;
    if (match({TokenKind::MUT})) {
      mutKeyword = previous();
    }
    const Mutability mutability =
        mutKeyword ? Mutability::Mutable : Mutability::Immutable;
    TypeRef type = parseType();
    std::optional<OperatorName> operatorName;
    Token name;
    if (match({TokenKind::OPERATOR})) {
      if (!allowOperators) {
        throw error(previous(),
                    "Operator overloads can only be declared as class or "
                    "struct members.");
      }
      operatorName = overloadedOperatorName(previous());
      name = syntheticOperatorName(*operatorName);
    } else {
      name = consume(TokenKind::IDENTIFIER, "Expect declaration name.");
    }
    std::optional<NamePath> definitionOwner;
    if (check(TokenKind::SCOPE)) {
      if (!allowFunction || allowOperators || operatorName) {
        throw error(
            peek(),
            "An out-of-class method definition must be at namespace scope.");
      }
      std::vector<Token> owner;
      while (match({TokenKind::SCOPE})) {
        owner.push_back(name);
        name = consume(TokenKind::IDENTIFIER,
                       "Expect a method or owner name after '::'.");
      }
      definitionOwner.emplace(std::move(owner));
    }
    std::vector<GenericParameter> genericParameters;
    if (!operatorName && check(TokenKind::LESS)) {
      genericParameters = genericParameterList();
    }
    if (blockParameters) {
      if (!genericParameters.empty()) {
        (void)error(name, "A conformance block operation takes the block's "
                          "generic parameters and cannot declare its own.");
      }
      genericParameters = std::move(*blockParameters);
    }

    if (match({TokenKind::LEFT_PAREN})) {
      if (!allowFunction) {
        throw error(previous(),
                    "Parenthesized declarations are not supported in block "
                    "scope; use 'Type name{arguments};' for direct "
                    "construction.");
      }
      if (mutability == Mutability::Mutable) {
        if (!type.reference) {
          throw error(name,
                      "A mutable function return must be a reference type.");
        }
      }
      if (operatorName && staticKeyword) {
        throw error(*staticKeyword,
                    "Operator overloads require an object receiver and cannot "
                    "be static.");
      }
      conformanceRequirement = std::move(blockRequirement);
      return functionDeclaration(
          std::move(type), name, std::move(genericParameters),
          std::move(runtimeBinding),
          allowMutableReceiver || definitionOwner.has_value(), mutability,
          std::move(operatorName), std::move(staticKeyword),
          std::move(virtualKeyword), linkage, std::move(constexprKeyword),
          std::move(nativeCArray), std::move(nativeLinkName),
          std::move(unsafeKeyword), std::move(functionAttributes),
          std::move(definitionOwner));
    }

    if (definitionOwner) {
      throw error(name, "Expect '(' after out-of-class method name.");
    }
    if (operatorName) {
      throw error(operatorName->symbol,
                  "Expect '(' after overloaded operator name.");
    }

    if (!genericParameters.empty()) {
      throw error(name, "Only functions can declare generic parameters here.");
    }

    if (runtimeBinding) {
      throw error(name, "Runtime binding must annotate a function.");
    }
    if (nativeCArray) {
      throw error(nativeCArray->attribute,
                  "[[c_array(...)]] must annotate a function declaration.");
    }
    if (nativeLinkName && linkage != LanguageLinkage::C) {
      throw error(nativeLinkName->attribute,
                  "[[link_name(...)]] must annotate a native function or "
                  "foreign variable declaration.");
    }
    if (unsafeKeyword) {
      throw error(*unsafeKeyword,
                  "'unsafe' must modify a function declaration.");
    }
    if (!functionAttributes.empty()) {
      throw error(functionAttributes.front().name,
                  "Function attributes must annotate a function declaration.");
    }
    if (virtualKeyword) {
      throw error(*virtualKeyword,
                  "Only member functions can be declared virtual.");
    }

    parseArrayDeclaratorSuffix(type);
    return variableDeclaration(
        mutability, std::move(type), name, std::move(staticKeyword),
        std::move(constexprKeyword), std::move(mutKeyword), linkage,
        std::move(nativeLinkName));
  }

  StmtPtr functionDeclaration(
      TypeRef returnType, Token name,
      std::vector<GenericParameter> genericParameters,
      std::optional<RuntimeBinding> runtimeBinding = std::nullopt,
      bool allowMutableReceiver = false,
      Mutability returnMutability = Mutability::Immutable,
      std::optional<OperatorName> operatorName = std::nullopt,
      std::optional<Token> staticKeyword = std::nullopt,
      std::optional<Token> virtualKeyword = std::nullopt,
      LanguageLinkage linkage = LanguageLinkage::Gti,
      std::optional<Token> constexprKeyword = std::nullopt,
      std::optional<NativeCArrayAttribute> nativeCArray = std::nullopt,
      std::optional<NativeLinkNameAttribute> nativeLinkName = std::nullopt,
      std::optional<Token> unsafeKeyword = std::nullopt,
      std::vector<FunctionAttribute> functionAttributes = {},
      std::optional<NamePath> definitionOwner = std::nullopt) {
    std::optional<RequiresClause> blockRequirement =
        std::exchange(conformanceRequirement, std::nullopt);
    std::vector<Parameter> parameters = parameterList();
    if (operatorName && operatorName->kind == OverloadedOperator::Dereference &&
        !parameters.empty()) {
      operatorName->kind = OverloadedOperator::Multiply;
      name.lexeme = std::string(operatorFunctionName(operatorName->kind));
    }

    ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly;
    if (match({TokenKind::MUT})) {
      if (staticKeyword) {
        throw error(previous(),
                    "Static methods do not have a receiver and cannot use a "
                    "trailing 'mut' qualifier.");
      }
      if (!allowMutableReceiver) {
        throw error(
            previous(),
            "Only class and struct methods can have a mutable receiver.");
      }
      receiverMutability = ReceiverMutability::Mutable;
      if (check(TokenKind::AND) && peek().lexeme == "&&") {
        throw error(peek(), "A receiver cannot combine trailing 'mut' and '&&' "
                            "qualifiers.");
      }
    } else if (check(TokenKind::AND) && peek().lexeme == "&&") {
      Token qualifier = advance();
      if (staticKeyword) {
        throw error(qualifier,
                    "Static methods do not have a receiver and cannot use a "
                    "trailing '&&' qualifier.");
      }
      if (!allowMutableReceiver) {
        throw error(
            qualifier,
            "Only class and struct methods can have a consuming receiver.");
      }
      receiverMutability = ReceiverMutability::Consuming;
      if (check(TokenKind::MUT)) {
        throw error(peek(), "A receiver cannot combine trailing '&&' and 'mut' "
                            "qualifiers.");
      }
    }

    std::optional<RequiresClause> requiresClause;
    if (match({TokenKind::REQUIRES})) {
      Token keyword = previous();
      requiresClause =
          RequiresClause{std::move(keyword), constraintExpression()};
      if (blockRequirement) {
        (void)error(requiresClause->keyword,
                    "A conformance block operation cannot declare a "
                    "requires clause; the block's concept application "
                    "applies to it.");
      }
    }
    if (blockRequirement) {
      requiresClause = std::move(blockRequirement);
    }

    std::optional<Token> overrideKeyword;
    if (match({TokenKind::OVERRIDE})) {
      overrideKeyword = previous();
    }

    if (unsafeKeyword &&
        (operatorName || virtualKeyword || overrideKeyword ||
         (currentClassKind == ClassKind::Interface && !staticKeyword) ||
         check(TokenKind::EQUAL))) {
      throw error(*unsafeKeyword,
                  "Unsafe methods must be non-polymorphic named methods; "
                  "operators and virtual/interface methods are not supported.");
    }

    std::optional<PureSpecifier> pureSpecifier;
    if (definitionOwner &&
        (check(TokenKind::SEMICOLON) || check(TokenKind::EQUAL))) {
      throw error(peek(), "An out-of-class method definition requires a body.");
    }
    if (match({TokenKind::EQUAL})) {
      Token equal = previous();
      Token zero = consume(TokenKind::INT_LITERAL,
                           "Expect integer literal 0 after '='.");
      const auto *value = std::get_if<std::uint64_t>(&zero.literal);
      if (value == nullptr || *value != 0) {
        throw error(zero, "A pure virtual declaration must use '= 0;'.");
      }
      consume(TokenKind::SEMICOLON,
              "Expect ';' after pure virtual declaration.");
      pureSpecifier =
          PureSpecifier{.equal = std::move(equal), .zero = std::move(zero)};
      return std::make_shared<FunctionDecl>(
          std::move(returnType), name, std::move(genericParameters),
          std::move(parameters), nullptr, std::move(runtimeBinding),
          receiverMutability, returnMutability, std::move(operatorName),
          std::move(staticKeyword), std::move(virtualKeyword),
          std::move(overrideKeyword), std::move(pureSpecifier), linkage,
          std::move(constexprKeyword), std::move(requiresClause),
          std::move(nativeCArray), std::move(nativeLinkName),
          std::move(unsafeKeyword), std::move(functionAttributes));
    }

    if (match({TokenKind::SEMICOLON})) {
      return std::make_shared<FunctionDecl>(
          std::move(returnType), name, std::move(genericParameters),
          std::move(parameters), nullptr, std::move(runtimeBinding),
          receiverMutability, returnMutability, std::move(operatorName),
          std::move(staticKeyword), std::move(virtualKeyword),
          std::move(overrideKeyword), std::nullopt, linkage,
          std::move(constexprKeyword), std::move(requiresClause),
          std::move(nativeCArray), std::move(nativeLinkName),
          std::move(unsafeKeyword), std::move(functionAttributes));
    }

    consume(TokenKind::LEFT_BRACE, "Expect '{' before function body.");
    auto body = std::make_unique<BlockStmt>(blockItems());
    return std::make_shared<FunctionDecl>(
        std::move(returnType), name, std::move(genericParameters),
        std::move(parameters), std::move(body), std::move(runtimeBinding),
        receiverMutability, returnMutability, std::move(operatorName),
        std::move(staticKeyword), std::move(virtualKeyword),
        std::move(overrideKeyword), std::nullopt, linkage,
        std::move(constexprKeyword), std::move(requiresClause),
        std::move(nativeCArray), std::move(nativeLinkName),
        std::move(unsafeKeyword), std::move(functionAttributes),
        std::move(definitionOwner));
  }

  // `operator Target()`. A bool target is the contextual boolean
  // conversion; any other target is an explicitly requested conversion.
  // `implicit` additionally admits either in declaration initialization.
  StmtPtr conversionOperatorDeclaration(
      Token keyword, std::optional<Token> virtualKeyword = std::nullopt,
      std::optional<Token> constexprKeyword = std::nullopt,
      std::optional<Token> implicitKeyword = std::nullopt) {
    StmtPtr declaration;
    if (check(TokenKind::BOOL) && peekAt(1).kind == TokenKind::LEFT_PAREN) {
      Token boolType = advance();
      OperatorName operatorName{OverloadedOperator::ContextualBool, keyword,
                                boolType};
      Token name = syntheticOperatorName(operatorName);
      consume(TokenKind::LEFT_PAREN, "Expect '(' after 'operator bool'.");
      declaration = functionDeclaration(
          TypeRef(boolType), std::move(name), {}, std::nullopt, true,
          Mutability::Immutable, std::move(operatorName), std::nullopt,
          std::move(virtualKeyword), LanguageLinkage::Gti,
          std::move(constexprKeyword));
    } else {
      // The parameter clause follows the target, so `Target()` is not a
      // function signature type here.
      const Token targetStart = peek();
      TypeRef target = parseType(false);
      OperatorName operatorName{OverloadedOperator::Conversion, keyword,
                                targetStart};
      Token name = syntheticOperatorName(operatorName);
      consume(TokenKind::LEFT_PAREN,
              "Expect '(' after the conversion operator's target type.");
      declaration = functionDeclaration(
          std::move(target), std::move(name), {}, std::nullopt, true,
          Mutability::Immutable, std::move(operatorName), std::nullopt,
          std::move(virtualKeyword), LanguageLinkage::Gti,
          std::move(constexprKeyword));
    }
    if (implicitKeyword) {
      static_cast<FunctionDecl &>(*declaration)
          .setImplicitKeyword(std::move(*implicitKeyword));
    }
    return declaration;
  }

  OperatorName overloadedOperatorName(Token keyword) {
    if (match({TokenKind::STAR})) {
      return {OverloadedOperator::Dereference, std::move(keyword), previous()};
    }
    if (match({TokenKind::ARROW})) {
      return {OverloadedOperator::Arrow, std::move(keyword), previous()};
    }
    if (match({TokenKind::PLUS_PLUS})) {
      return {OverloadedOperator::PreIncrement, std::move(keyword), previous()};
    }
    if (match({TokenKind::MINUS_MINUS})) {
      return {OverloadedOperator::PreDecrement, std::move(keyword), previous()};
    }
    if (match({TokenKind::EQUAL})) {
      return {OverloadedOperator::Assignment, std::move(keyword), previous()};
    }
    if (match({TokenKind::PLUS})) {
      return {OverloadedOperator::Plus, std::move(keyword), previous()};
    }
    if (match({TokenKind::MINUS})) {
      return {OverloadedOperator::Minus, std::move(keyword), previous()};
    }
    if (match({TokenKind::SLASH})) {
      return {OverloadedOperator::Divide, std::move(keyword), previous()};
    }
    if (match({TokenKind::PERCENT})) {
      return {OverloadedOperator::Remainder, std::move(keyword), previous()};
    }
    if (match({TokenKind::PLUS_EQUAL})) {
      return {OverloadedOperator::AddAssign, std::move(keyword), previous()};
    }
    if (match({TokenKind::MINUS_EQUAL})) {
      return {OverloadedOperator::SubtractAssign, std::move(keyword),
              previous()};
    }
    if (match({TokenKind::STAR_EQUAL})) {
      return {OverloadedOperator::MultiplyAssign, std::move(keyword),
              previous()};
    }
    if (match({TokenKind::SLASH_EQUAL})) {
      return {OverloadedOperator::DivideAssign, std::move(keyword), previous()};
    }
    if (match({TokenKind::PERCENT_EQUAL})) {
      return {OverloadedOperator::RemainderAssign, std::move(keyword),
              previous()};
    }
    if (match({TokenKind::TILDE})) {
      return {OverloadedOperator::BitwiseNot, std::move(keyword), previous()};
    }
    if (match({TokenKind::AMPERSAND})) {
      return {OverloadedOperator::BitwiseAnd, std::move(keyword), previous()};
    }
    if (match({TokenKind::PIPE})) {
      return {OverloadedOperator::BitwiseOr, std::move(keyword), previous()};
    }
    if (match({TokenKind::CARET})) {
      return {OverloadedOperator::BitwiseXor, std::move(keyword), previous()};
    }
    if (match({TokenKind::AMPERSAND_EQUAL})) {
      return {OverloadedOperator::BitwiseAndAssign, std::move(keyword),
              previous()};
    }
    if (match({TokenKind::PIPE_EQUAL})) {
      return {OverloadedOperator::BitwiseOrAssign, std::move(keyword),
              previous()};
    }
    if (match({TokenKind::CARET_EQUAL})) {
      return {OverloadedOperator::BitwiseXorAssign, std::move(keyword),
              previous()};
    }
    if (match({TokenKind::SHIFT_LEFT_EQUAL})) {
      return {OverloadedOperator::ShiftLeftAssign, std::move(keyword),
              previous()};
    }
    if (match({TokenKind::SHIFT_RIGHT_EQUAL})) {
      return {OverloadedOperator::ShiftRightAssign, std::move(keyword),
              previous()};
    }
    if (const std::optional<Token> shift = matchShiftOperator()) {
      return {shift->kind == TokenKind::SHIFT_LEFT
                  ? OverloadedOperator::ShiftLeft
                  : OverloadedOperator::ShiftRight,
              std::move(keyword), *shift};
    }
    if (match({TokenKind::EQUAL_EQUAL})) {
      return {OverloadedOperator::Equal, std::move(keyword), previous()};
    }
    if (match({TokenKind::BANG_EQUAL})) {
      return {OverloadedOperator::NotEqual, std::move(keyword), previous()};
    }
    if (match({TokenKind::LESS})) {
      return {OverloadedOperator::Less, std::move(keyword), previous()};
    }
    if (match({TokenKind::LESS_EQUAL})) {
      return {OverloadedOperator::LessEqual, std::move(keyword), previous()};
    }
    if (match({TokenKind::GREATER})) {
      return {OverloadedOperator::Greater, std::move(keyword), previous()};
    }
    if (match({TokenKind::GREATER_EQUAL})) {
      return {OverloadedOperator::GreaterEqual, std::move(keyword), previous()};
    }
    if (match({TokenKind::LEFT_BRACKET})) {
      Token bracket = previous();
      consume(TokenKind::RIGHT_BRACKET, "Expect ']' after 'operator['.");
      return {OverloadedOperator::Subscript, std::move(keyword),
              std::move(bracket)};
    }
    if (match({TokenKind::LEFT_PAREN})) {
      Token parenthesis = previous();
      consume(TokenKind::RIGHT_PAREN, "Expect ')' after 'operator('.");
      return {OverloadedOperator::Call, std::move(keyword),
              std::move(parenthesis)};
    }
    throw error(peek(),
                "This operator spelling is not supported for overloading in "
                "GTI.");
  }

  static Token syntheticOperatorName(const OperatorName &operatorName) {
    Token name = operatorName.keyword;
    name.kind = TokenKind::IDENTIFIER;
    name.lexeme = std::string(operatorFunctionName(operatorName.kind));
    return name;
  }

  // A signature, void argument or reference distinguishes a patterned primary
  // from the existing parameter declaration grammar. In particular, stop at
  // '=': a default such as <S = void()> is still an ordinary generic parameter.
  // This lookahead never resolves identifiers or edits/splits lexer tokens.
  bool classSignaturePatternAhead() const {
    std::size_t depth = 0;
    for (std::size_t offset = 1;; ++offset) {
      const TokenKind kind = peekAt(offset).kind;
      if (kind == TokenKind::END_OF_FILE || kind == TokenKind::LEFT_BRACE ||
          kind == TokenKind::RIGHT_BRACE || kind == TokenKind::SEMICOLON) {
        return false;
      }
      if (depth == 0) {
        // Signature, void, reference and raw-pointer argument patterns.
        if (kind == TokenKind::LEFT_PAREN || kind == TokenKind::VOID ||
            kind == TokenKind::AMPERSAND || kind == TokenKind::STAR ||
            kind == TokenKind::CONST) {
          return true;
        }
        if (kind == TokenKind::EQUAL || kind == TokenKind::GREATER ||
            kind == TokenKind::GREATER_EQUAL ||
            kind == TokenKind::SHIFT_RIGHT ||
            kind == TokenKind::SHIFT_RIGHT_EQUAL) {
          return false;
        }
      }
      if (kind == TokenKind::LESS) {
        ++depth;
      } else if (kind == TokenKind::GREATER ||
                 kind == TokenKind::GREATER_EQUAL) {
        if (depth == 0) {
          return false;
        }
        --depth;
      } else if (kind == TokenKind::SHIFT_RIGHT ||
                 kind == TokenKind::SHIFT_RIGHT_EQUAL) {
        if (depth < 2) {
          return false;
        }
        depth -= 2;
      }
    }
  }

  std::vector<GenericParameter> genericParameterList() {
    consume(TokenKind::LESS, "Expect '<' before generic parameters.");
    std::vector<GenericParameter> parameters;
    do {
      if (isValueGenericParameterStart()) {
        Token valueType = advance();
        Token name =
            consume(TokenKind::IDENTIFIER,
                    "Expect a value parameter name after its declared type.");
        GenericParameter parameter{.name = std::move(name),
                                   .valueType = std::move(valueType)};
        if (match({TokenKind::EQUAL})) {
          parameter.equal = previous();
          parameter.defaultValue = arrayExtentExpression();
        }
        parameters.emplace_back(std::move(parameter));
        continue;
      }
      Token first = consume(TokenKind::IDENTIFIER,
                            "Expect a generic type parameter name.");
      std::optional<NamePath> constraint;
      Token name;
      if (check(TokenKind::SCOPE) || check(TokenKind::IDENTIFIER)) {
        constraint = parseNamePath(std::move(first));
        name = consume(
            TokenKind::IDENTIFIER,
            "Expect a generic type parameter name after its constraint.");
      } else {
        name = std::move(first);
      }
      std::optional<Token> pack;
      if (match({TokenKind::ELLIPSIS})) {
        pack = previous();
      }
      GenericParameter parameter{.name = std::move(name),
                                 .pack = std::move(pack),
                                 .constraint = std::move(constraint)};
      if (match({TokenKind::EQUAL})) {
        parameter.equal = previous();
        parameter.defaultType = parseType();
      }
      parameters.emplace_back(std::move(parameter));
    } while (match({TokenKind::COMMA}));
    consumeGenericClose("Expect '>' after generic parameters.");
    return parameters;
  }

  StmtPtr constructorDeclaration(Token name,
                                 std::optional<Token> constexprKeyword = {},
                                 std::optional<Token> unsafeKeyword = {},
                                 std::optional<Token> implicitKeyword = {}) {
    std::vector<GenericParameter> genericParameters;
    if (check(TokenKind::LESS)) {
      genericParameters = genericParameterList();
    }
    consume(TokenKind::LEFT_PAREN, "Expect '(' after constructor name.");
    std::vector<Parameter> parameters = parameterList();
    if (match({TokenKind::MUT})) {
      throw error(previous(),
                  "Constructors do not have receiver mutability qualifiers.");
    }

    std::optional<RequiresClause> requiresClause;
    if (match({TokenKind::REQUIRES})) {
      Token keyword = previous();
      requiresClause =
          RequiresClause{std::move(keyword), constraintExpression()};
    }

    std::vector<ConstructorInitializer> initializers;
    if (match({TokenKind::COLON})) {
      do {
        TypeRef target = parseBaseType();
        consume(TokenKind::LEFT_PAREN,
                "Expect '(' after constructor initializer target.");
        ExprList arguments;
        if (!check(TokenKind::RIGHT_PAREN)) {
          do {
            arguments.emplace_back(check(TokenKind::LEFT_BRACE)
                                       ? arrayInitializer()
                                       : assignment());
          } while (match({TokenKind::COMMA}));
        }
        consume(TokenKind::RIGHT_PAREN,
                "Expect ')' after constructor initializer arguments.");
        initializers.push_back(ConstructorInitializer{
            .target = std::move(target), .arguments = std::move(arguments)});
      } while (match({TokenKind::COMMA}));
    }

    if (match({TokenKind::EQUAL})) {
      Token equal = previous();
      Token keyword;
      SpecialMemberSpecifierKind kind;
      if (match({TokenKind::DEFAULT})) {
        keyword = previous();
        kind = SpecialMemberSpecifierKind::Defaulted;
      } else if (match({TokenKind::DELETE})) {
        keyword = previous();
        kind = SpecialMemberSpecifierKind::Deleted;
      } else {
        throw error(peek(),
                    "Expect 'default' or 'delete' after '=' in a special "
                    "constructor declaration.");
      }
      consume(TokenKind::SEMICOLON,
              "Expect ';' after special constructor declaration.");
      if (unsafeKeyword) {
        throw error(*unsafeKeyword,
                    "Only constructors with a source body may be unsafe.");
      }
      if (implicitKeyword) {
        throw error(*implicitKeyword,
                    "Only constructors with a source body may be implicit.");
      }
      return std::make_shared<ConstructorDecl>(
          std::move(constexprKeyword), std::move(name),
          std::move(genericParameters), std::move(parameters),
          std::move(requiresClause), std::move(initializers),
          SpecialMemberSpecifier{.equal = std::move(equal),
                                 .keyword = std::move(keyword),
                                 .kind = kind},
          nullptr);
    }

    consume(TokenKind::LEFT_BRACE, "Expect '{' before constructor body.");
    auto body = std::make_unique<BlockStmt>(blockItems());
    return std::make_shared<ConstructorDecl>(
        std::move(constexprKeyword), std::move(name),
        std::move(genericParameters), std::move(parameters),
        std::move(requiresClause), std::move(initializers), std::nullopt,
        std::move(body), std::move(unsafeKeyword), std::move(implicitKeyword));
  }

  // The length of a constructor modifier run containing the contextual
  // `implicit`, when one starts here. `implicit` is a modifier only there.
  [[nodiscard]] std::optional<std::size_t>
  implicitConstructorModifiers() const {
    bool implicitSeen = false;
    bool unsafeSeen = false;
    bool constexprSeen = false;
    for (std::size_t offset = 0; offset < 3; ++offset) {
      const Token &token = peekAt(offset);
      bool *seen =
          token.kind == TokenKind::IDENTIFIER && token.lexeme == "implicit"
              ? &implicitSeen
          : token.kind == TokenKind::UNSAFE    ? &unsafeSeen
          : token.kind == TokenKind::CONSTEXPR ? &constexprSeen
                                               : nullptr;
      if (seen == nullptr) {
        return implicitSeen && isConstructorStart(offset)
                   ? std::optional<std::size_t>(offset)
                   : std::nullopt;
      }
      if (*seen) {
        return std::nullopt;
      }
      *seen = true;
    }
    return implicitSeen && isConstructorStart(3) ? std::optional<std::size_t>(3)
                                                 : std::nullopt;
  }

  // Misordered or conflicting modifiers are reported, and the constructor
  // they introduce is still parsed so that recovery consumes it.
  StmtPtr implicitConstructorDeclaration(std::size_t modifiers) {
    std::optional<Token> implicitKeyword;
    std::optional<Token> unsafeKeyword;
    std::optional<Token> constexprKeyword;
    for (std::size_t index = 0; index < modifiers; ++index) {
      Token modifier = advance();
      if (modifier.kind == TokenKind::CONSTEXPR) {
        constexprKeyword = std::move(modifier);
      } else if (modifier.kind == TokenKind::UNSAFE) {
        unsafeKeyword = std::move(modifier);
      } else {
        if (constexprKeyword) {
          (void)error(modifier, "Write 'implicit' before 'constexpr'.");
        }
        implicitKeyword = std::move(modifier);
      }
    }
    if (implicitKeyword && unsafeKeyword) {
      (void)error(*implicitKeyword,
                  "An implicit constructor cannot be unsafe; construct the "
                  "value explicitly in an unsafe context instead.");
      implicitKeyword.reset();
    }
    return constructorDeclaration(advance(), std::move(constexprKeyword),
                                  std::move(unsafeKeyword),
                                  std::move(implicitKeyword));
  }

  // The length of a conversion operator's modifier run containing the
  // contextual `implicit`: `implicit` and at most one of `virtual` or
  // `constexpr`, followed by `operator` and a type. Elsewhere `implicit` is an
  // identifier, including a type named `implicit` before an operator symbol.
  [[nodiscard]] std::optional<std::size_t>
  implicitConversionOperatorModifiers() const {
    bool implicitSeen = false;
    bool otherSeen = false;
    for (std::size_t offset = 0; offset < 3; ++offset) {
      const Token &token = peekAt(offset);
      if (token.kind == TokenKind::OPERATOR) {
        return implicitSeen && typeEnd(offset + 1)
                   ? std::optional<std::size_t>(offset)
                   : std::nullopt;
      }
      bool *seen =
          token.kind == TokenKind::IDENTIFIER && token.lexeme == "implicit"
              ? &implicitSeen
          : token.kind == TokenKind::VIRTUAL ||
                  token.kind == TokenKind::CONSTEXPR
              ? &otherSeen
              : nullptr;
      if (seen == nullptr || *seen) {
        return std::nullopt;
      }
      *seen = true;
    }
    return std::nullopt;
  }

  // A misordered modifier is reported, and the conversion operator it
  // introduces is still parsed so that recovery consumes it.
  StmtPtr implicitConversionOperatorDeclaration(std::size_t modifiers) {
    std::optional<Token> implicitKeyword;
    std::optional<Token> virtualKeyword;
    std::optional<Token> constexprKeyword;
    for (std::size_t index = 0; index < modifiers; ++index) {
      Token modifier = advance();
      if (modifier.kind == TokenKind::VIRTUAL) {
        virtualKeyword = std::move(modifier);
      } else if (modifier.kind == TokenKind::CONSTEXPR) {
        constexprKeyword = std::move(modifier);
      } else {
        if (virtualKeyword || constexprKeyword) {
          (void)error(
              modifier,
              "Write 'implicit' before '" +
                  (virtualKeyword ? virtualKeyword : constexprKeyword)->lexeme +
                  "'.");
        }
        implicitKeyword = std::move(modifier);
      }
    }
    return conversionOperatorDeclaration(advance(), std::move(virtualKeyword),
                                         std::move(constexprKeyword),
                                         std::move(implicitKeyword));
  }

  StmtPtr destructorDeclaration(Token tilde) {
    Token name = consume(TokenKind::IDENTIFIER, "Expect class name after '~'.");
    consume(TokenKind::LEFT_PAREN, "Expect '(' after destructor name.");
    consume(TokenKind::RIGHT_PAREN, "Destructors do not take parameters.");
    if (match({TokenKind::MUT})) {
      throw error(previous(),
                  "Destructors have an implicitly mutable receiver and do not "
                  "take a 'mut' qualifier.");
    }
    if (match({TokenKind::EQUAL})) {
      Token equal = previous();
      Token keyword;
      SpecialMemberSpecifierKind kind;
      if (match({TokenKind::DEFAULT})) {
        keyword = previous();
        kind = SpecialMemberSpecifierKind::Defaulted;
      } else if (match({TokenKind::DELETE})) {
        keyword = previous();
        kind = SpecialMemberSpecifierKind::Deleted;
      } else {
        throw error(peek(), "Expect 'default' or 'delete' after '=' in a "
                            "destructor declaration.");
      }
      consume(TokenKind::SEMICOLON,
              "Expect ';' after special destructor declaration.");
      return std::make_shared<DestructorDecl>(
          std::move(tilde), std::move(name),
          SpecialMemberSpecifier{.equal = std::move(equal),
                                 .keyword = std::move(keyword),
                                 .kind = kind},
          nullptr);
    }
    consume(TokenKind::LEFT_BRACE, "Expect '{' before destructor body.");
    auto body = std::make_unique<BlockStmt>(blockItems());
    return std::make_shared<DestructorDecl>(std::move(tilde), std::move(name),
                                            std::nullopt, std::move(body));
  }

  std::vector<Parameter> parameterList() {
    std::vector<Parameter> parameters;
    if (!check(TokenKind::RIGHT_PAREN)) {
      do {
        const Mutability mutability = match({TokenKind::MUT})
                                          ? Mutability::Mutable
                                          : Mutability::Immutable;
        TypeRef parameterType = parseType();
        std::optional<Token> pack;
        if (match({TokenKind::ELLIPSIS})) {
          pack = previous();
        }
        Token parameterName;
        if (check(TokenKind::IDENTIFIER)) {
          parameterName = advance();
        }
        parseArrayDeclaratorSuffix(parameterType);
        std::optional<ParameterDefault> defaultArgument;
        if (match({TokenKind::EQUAL})) {
          Token equal = previous();
          defaultArgument.emplace(std::move(equal), assignment());
        }
        parameters.emplace_back(std::move(parameterType),
                                std::move(parameterName), mutability,
                                std::move(pack), std::move(defaultArgument));
      } while (match({TokenKind::COMMA}));
    }
    consume(TokenKind::RIGHT_PAREN, "Expect ')' after parameters.");
    return parameters;
  }

  StmtPtr variableDeclaration(
      Mutability mutability, TypeRef type, Token name,
      std::optional<Token> staticKeyword = std::nullopt,
      std::optional<Token> constexprKeyword = std::nullopt,
      std::optional<Token> mutKeyword = std::nullopt,
      LanguageLinkage linkage = LanguageLinkage::Gti,
      std::optional<NativeLinkNameAttribute> nativeLinkName = std::nullopt) {
    const bool hadCompletion = consumedCompletion;
    ExprPtr initializer;
    if (linkage == LanguageLinkage::C &&
        (check(TokenKind::EQUAL) || check(TokenKind::LEFT_BRACE))) {
      throw error(peek(),
                  "A foreign variable declaration cannot have an initializer.");
    }
    if (match({TokenKind::EQUAL})) {
      initializer = initializerExpression();
    } else if (match({TokenKind::LEFT_BRACE})) {
      initializer = directInitializer(previous());
    }

    if (check(TokenKind::SEMICOLON)) {
      advance();
    } else if (hadCompletion == consumedCompletion) {
      consume(TokenKind::SEMICOLON, "Expect ';' after variable declaration.");
    }
    return std::make_shared<VariableDecl>(
        mutability, std::move(type), name, std::move(initializer),
        std::move(staticKeyword), false, std::move(constexprKeyword), false,
        false, std::move(mutKeyword), false, linkage,
        std::move(nativeLinkName));
  }

  StmtPtr mutableFieldGroupDeclaration(Token keyword) {
    Token leftBrace = consume(TokenKind::LEFT_BRACE,
                              "Expect '{' after 'mut' field-group keyword.");
    if (currentClassKind == ClassKind::Interface ||
        currentClassKind == ClassKind::Union) {
      diagnostics.push_back(
          makeDiagnostic("GTI-P0001", DiagnosticPhase::Parsing, keyword,
                         currentClassKind == ClassKind::Interface
                             ? "Interfaces cannot contain mutable field groups."
                             : "Unions cannot contain mutable field groups."));
    }

    StmtList members;
    while (!check(TokenKind::RIGHT_BRACE) && !isAtEnd()) {
      try {
        members.emplace_back(item(ItemContext::MutableFieldGroup));
      } catch (const ParseError &) {
        synchronize(true, false, false, false, false, false);
      }
    }
    Token rightBrace = consume(TokenKind::RIGHT_BRACE,
                               "Expect '}' after mutable field group.");
    if (members.empty()) {
      diagnostics.push_back(makeDiagnostic(
          "GTI-P0001", DiagnosticPhase::Parsing, keyword,
          "A mutable field group must contain at least one field or "
          "configuration item."));
    }
    return std::make_shared<MutableFieldGroupDecl>(
        std::move(keyword), std::move(leftBrace), std::move(members),
        std::move(rightBrace));
  }

  StmtPtr staticAssertDeclaration(Token keyword) {
    Token leftParen =
        consume(TokenKind::LEFT_PAREN, "Expect '(' after 'static_assert'.");
    if (check(TokenKind::RIGHT_PAREN) || check(TokenKind::COMMA)) {
      throw error(peek(), "Expect a condition in 'static_assert'.");
    }
    Token conditionStart = peek();
    ExprPtr condition = assignment();
    std::optional<Token> comma;
    std::optional<Token> message;
    if (match({TokenKind::COMMA})) {
      comma = previous();
      if (check(TokenKind::RIGHT_PAREN)) {
        throw error(peek(),
                    "A trailing comma is not allowed in 'static_assert'.");
      }
      message = consume(TokenKind::STRING_LITERAL,
                        "Expect a quoted string message after ','.");
      if (match({TokenKind::COMMA})) {
        throw error(previous(),
                    "'static_assert' accepts only a condition and one "
                    "optional message.");
      }
    }
    Token rightParen =
        consume(TokenKind::RIGHT_PAREN, "Expect ')' after 'static_assert'.");
    Token semicolon =
        consume(TokenKind::SEMICOLON, "Expect ';' after 'static_assert'.");
    return std::make_shared<StaticAssertDecl>(
        std::move(keyword), std::move(leftParen), std::move(conditionStart),
        std::move(condition), std::move(comma), std::move(message),
        std::move(rightParen), std::move(semicolon));
  }

  StmtPtr groupedFieldDeclaration() {
    if (match({TokenKind::MUT})) {
      throw error(previous(),
                  "Fields inside a 'mut { ... }' group are already mutable; "
                  "remove the redundant 'mut'.");
    }
    if (match({TokenKind::STATIC})) {
      throw error(previous(),
                  "A mutable field group can contain only direct instance "
                  "fields; move this static field outside the group.");
    }
    if (match({TokenKind::CONSTEXPR})) {
      throw error(previous(),
                  "A mutable field group cannot contain constexpr fields; "
                  "move this field outside the group.");
    }
    if (!isTypedDeclaration()) {
      throw error(peek(),
                  "A mutable field group can contain only direct instance "
                  "field declarations.");
    }

    TypeRef type = parseType();
    Token name = consume(TokenKind::IDENTIFIER,
                         "Expect field name in mutable field group.");
    if (check(TokenKind::LESS) || check(TokenKind::LEFT_PAREN)) {
      throw error(peek(),
                  "Methods and constructors cannot be declared inside a "
                  "mutable field group.");
    }
    parseArrayDeclaratorSuffix(type);
    return variableDeclaration(Mutability::Mutable, std::move(type),
                               std::move(name));
  }

  ExprPtr directInitializer(Token brace) {
    ExprList arguments;
    std::vector<Token> designators;
    const bool designated = check(TokenKind::DOT);
    if (!check(TokenKind::RIGHT_BRACE)) {
      while (true) {
        if (designated) {
          consume(TokenKind::DOT,
                  "Expect '.field = value' in designated construction.");
          designators.push_back(
              consume(TokenKind::IDENTIFIER, "Expect a field name after '.'."));
          consume(TokenKind::EQUAL, "Expect '=' after designated field name.");
          arguments.emplace_back(
              check(TokenKind::LEFT_BRACE) ? arrayInitializer() : assignment());
        } else {
          arguments.emplace_back(assignment());
        }
        if (!match({TokenKind::COMMA}) || check(TokenKind::RIGHT_BRACE)) {
          break;
        }
      }
    }
    Token closingBrace =
        consume(TokenKind::RIGHT_BRACE,
                "Expect '}' after direct constructor arguments.");
    return std::make_unique<DirectInitializer>(
        std::move(brace), std::move(arguments), std::move(closingBrace),
        std::move(designators));
  }

  TypeRef parseType(bool allowFunctionSignature = true) {
    const NestingGuard nesting = enterNesting(peek());
    std::optional<Token> pointeeConst;
    if (match({TokenKind::CONST})) {
      pointeeConst = previous();
    }
    TypeRef type = parseBaseType();
    while (match({TokenKind::STAR})) {
      checkNestingDepth(previous(), type.pointers.size());
      type.pointers.push_back(
          {.star = previous(), .pointeeConst = std::move(pointeeConst)});
      pointeeConst.reset();
      if (match({TokenKind::CONST})) {
        pointeeConst = previous();
      }
    }
    if (pointeeConst) {
      throw error(*pointeeConst,
                  "'const' is currently supported only as a raw-pointer "
                  "pointee qualifier, as in 'const T*' or 'T* const*'.");
    }
    parseArrayTypeSuffix(type);
    if (match({TokenKind::AMPERSAND})) {
      type.reference = previous();
    } else if (check(TokenKind::AND) && peek().lexeme == "&&") {
      type.reference = advance();
    }
    if (allowFunctionSignature && match({TokenKind::LEFT_PAREN})) {
      std::vector<TypeRef> parameters;
      if (!check(TokenKind::RIGHT_PAREN)) {
        do {
          std::optional<Token> access;
          if (match({TokenKind::MUT})) {
            access = previous();
          }
          parameters.emplace_back(parseType());
          parameters.back().referenceMutability = std::move(access);
          if (match({TokenKind::ELLIPSIS})) {
            parameters.back().packExpansion = previous();
          }
        } while (match({TokenKind::COMMA}));
      }
      consume(TokenKind::RIGHT_PAREN,
              "Expect ')' after function signature parameter types.");
      type = TypeRef::functionSignature(std::move(type), std::move(parameters));
      if (match({TokenKind::MUT})) {
        type.functionSignatureQualifier = previous();
      } else if (check(TokenKind::AND) && peek().lexeme == "&&") {
        type.functionSignatureQualifier = advance();
      }
    }
    return type;
  }

  TypeRef parseBaseType() {
    if (match({TokenKind::LEFT_PAREN})) {
      std::vector<TypeRef> parameters;
      if (!check(TokenKind::RIGHT_PAREN)) {
        do {
          parameters.emplace_back(parseType());
        } while (match({TokenKind::COMMA}));
      }
      consume(TokenKind::RIGHT_PAREN,
              "Expect ')' after native function parameter types.");
      Token arrow = consume(TokenKind::ARROW,
                            "Expect '->' before native function return type.");
      TypeRef returnType = parseType();
      return TypeRef::nativeFunction(std::move(arrow), std::move(parameters),
                                     std::move(returnType));
    }
    if (match({TokenKind::EXPECTED})) {
      Token expected = previous();
      consume(TokenKind::LESS, "Expect '<' after 'expected'.");
      std::vector<TypeRef> arguments;
      arguments.emplace_back(parseType());
      consume(TokenKind::COMMA,
              "Expect ',' between expected value and error types.");
      arguments.emplace_back(parseType());
      consumeGenericClose("Expect '>' after expected error type.");
      return TypeRef(std::move(expected), std::move(arguments));
    }
    if (match({TokenKind::INT, TokenKind::INT8, TokenKind::INT16,
               TokenKind::INT32, TokenKind::INT64, TokenKind::UINT,
               TokenKind::UINT8, TokenKind::UINT16, TokenKind::UINT32,
               TokenKind::UINT64, TokenKind::FLOAT, TokenKind::DOUBLE,
               TokenKind::BOOL, TokenKind::CHAR, TokenKind::NULLPTR_TYPE,
               TokenKind::VOID, TokenKind::AUTO})) {
      return TypeRef(previous());
    }
    if (match({TokenKind::IDENTIFIER})) {
      NamePath name = parseNamePath(previous());
      std::vector<TypeRef> arguments;
      if (match({TokenKind::LESS})) {
        if (!check(TokenKind::GREATER)) {
          do {
            arguments.emplace_back(parseGenericArgument());
          } while (match({TokenKind::COMMA}));
        }
        consumeGenericClose("Expect '>' after generic arguments.");
      }
      TypeRef type(std::move(name), std::move(arguments));
      std::size_t qualifierDepth = 0;
      while (match({TokenKind::SCOPE})) {
        Token member = consume(TokenKind::IDENTIFIER,
                               "Expect a type member name after '::'.");
        checkNestingDepth(member, ++qualifierDepth);
        std::vector<Token> segments = type.name.segments;
        segments.emplace_back(std::move(member));
        TypeRef selected(NamePath(std::move(segments)));
        selected.qualifiedOwner =
            std::make_shared<const TypeRef>(std::move(type));
        if (match({TokenKind::LESS})) {
          if (!check(TokenKind::GREATER)) {
            do {
              selected.arguments.emplace_back(parseGenericArgument());
            } while (match({TokenKind::COMMA}));
          }
          consumeGenericClose("Expect '>' after type member arguments.");
        }
        type = std::move(selected);
      }
      return type;
    }
    throw error(peek(), "Expect a type name.");
  }

  void parseArrayTypeSuffix(TypeRef &type) {
    while (match({TokenKind::LEFT_BRACKET})) {
      type.arrayExtents.emplace_back(arrayExtentExpression());
      consume(TokenKind::RIGHT_BRACKET, "Expect ']' after fixed array extent.");
    }
  }

  ArrayExtentExprPtr arrayExtentExpression() { return arrayExtentAdditive(); }

  ArrayExtentExprPtr arrayExtentAdditive() {
    ArrayExtentExprPtr expression = arrayExtentMultiplicative();
    while (match({TokenKind::PLUS, TokenKind::MINUS})) {
      Token oper = previous();
      expression = std::make_shared<ArrayExtentExpr>(
          std::move(expression), std::move(oper), arrayExtentMultiplicative());
    }
    return expression;
  }

  ArrayExtentExprPtr arrayExtentMultiplicative() {
    ArrayExtentExprPtr expression = arrayExtentPrimary();
    while (match({TokenKind::STAR, TokenKind::SLASH, TokenKind::PERCENT})) {
      Token oper = previous();
      expression = std::make_shared<ArrayExtentExpr>(
          std::move(expression), std::move(oper), arrayExtentPrimary());
    }
    return expression;
  }

  ArrayExtentExprPtr arrayExtentPrimary() {
    if (match({TokenKind::INT_LITERAL, TokenKind::IDENTIFIER})) {
      return std::make_shared<ArrayExtentExpr>(previous());
    }
    if (match({TokenKind::LEFT_PAREN})) {
      ArrayExtentExprPtr expression = arrayExtentExpression();
      consume(TokenKind::RIGHT_PAREN,
              "Expect ')' after fixed array extent expression.");
      return expression;
    }
    throw error(peek(),
                "Fixed array extent must be an integer constant expression "
                "or uint64_t value generic parameter.");
  }

  TypeRef parseGenericArgument() {
    const std::optional<std::size_t> expressionEnd =
        arrayExtentExpressionEnd(0);
    const bool expressionReachesDelimiter =
        expressionEnd && (peekAt(*expressionEnd).kind == TokenKind::COMMA ||
                          peekAt(*expressionEnd).kind == TokenKind::GREATER);
    const bool valueExpression =
        check(TokenKind::INT_LITERAL) ||
        (expressionReachesDelimiter &&
         (check(TokenKind::LEFT_PAREN) ||
          (isIdentifierUseToken(peek().kind) && *expressionEnd > 1)));
    if (valueExpression) {
      ArrayExtentExprPtr expression = arrayExtentExpression();
      TypeRef argument(expression->token);
      argument.genericArgumentSyntax = GenericArgumentSyntax::Value;
      argument.genericValueExpression = std::move(expression);
      return argument;
    }

    TypeRef argument = parseType();
    if (match({TokenKind::ELLIPSIS})) {
      argument.packExpansion = previous();
    }
    if (argument.name.segments.size() == 1 &&
        argument.name.last().kind == TokenKind::IDENTIFIER &&
        argument.arguments.empty() && argument.arrayExtents.empty() &&
        argument.pointers.empty() && !argument.reference) {
      argument.genericArgumentSyntax =
          GenericArgumentSyntax::UnresolvedIdentifier;
    }
    return argument;
  }

  void parseArrayDeclaratorSuffix(TypeRef &type) {
    if (!type.arrayExtents.empty() && check(TokenKind::LEFT_BRACKET)) {
      throw error(peek(), "Do not mix type and declarator array suffixes.");
    }
    parseArrayTypeSuffix(type);
  }

  ExprPtr initializerExpression() {
    return check(TokenKind::LEFT_BRACE) ? arrayInitializer() : expression();
  }

  ExprPtr arrayInitializer() {
    Token brace =
        consume(TokenKind::LEFT_BRACE, "Expect '{' before array initializer.");
    if (check(TokenKind::DOT)) {
      return directInitializer(std::move(brace));
    }
    ExprList elements;
    if (!check(TokenKind::RIGHT_BRACE)) {
      do {
        elements.emplace_back(check(TokenKind::LEFT_BRACE) ? arrayInitializer()
                                                           : assignment());
      } while (match({TokenKind::COMMA}) && !check(TokenKind::RIGHT_BRACE));
    }
    Token closingBrace =
        consume(TokenKind::RIGHT_BRACE, "Expect '}' after array initializer.");
    return std::make_unique<ArrayInitializer>(
        std::move(brace), std::move(elements), std::move(closingBrace));
  }

  NamePath parseNamePath() {
    return parseNamePath(
        consume(TokenKind::IDENTIFIER, "Expect qualified name."));
  }

  NamePath parseNamePath(Token first) {
    std::vector<Token> segments;
    segments.emplace_back(std::move(first));
    while (match({TokenKind::SCOPE})) {
      segments.emplace_back(
          consume(TokenKind::IDENTIFIER, "Expect name after '::'."));
    }
    return NamePath(std::move(segments));
  }

  StmtList blockItems() {
    StmtList statements;

    while (!check(TokenKind::RIGHT_BRACE) && !isAtEnd()) {
      try {
        statements.emplace_back(item(ItemContext::Block));
      } catch (const ParseError &) {
        synchronize(true, true, false);
      }
    }

    consume(TokenKind::RIGHT_BRACE, "Expect '}' after block.");
    return statements;
  }

  StmtPtr item(ItemContext context) {
    const Token start = peek();
    StmtPtr result = itemWithoutExtent(context);
    stampExtent(result, start);
    return result;
  }

  void stampExtent(const StmtPtr &statement, const Token &start) const {
    if (statement == nullptr) {
      return;
    }
    if (start.leadingDocumentationMarkdown) {
      statement->setDocumentationMarkdown(*start.leadingDocumentationMarkdown);
    }
    if (current == 0) {
      return;
    }
    const Token &last = previous();
    const std::size_t end = last.position + last.lexeme.size();
    if (end <= start.position) {
      return;
    }
    statement->setExtent(SourceSpan{.source = start.source,
                                    .start = start.position,
                                    .end = end,
                                    .line = start.line});
  }

  StmtPtr itemWithoutExtent(ItemContext context) {
    if (context == ItemContext::Declaration) {
      return declarationWithoutExtent();
    }
    if (match({TokenKind::HASH_IF, TokenKind::HASH_IFDEF,
               TokenKind::HASH_IFNDEF})) {
      return conditionalCompilation(context);
    }
    if (match({TokenKind::HASH_ERROR})) {
      return compileErrorDirective();
    }
    rejectStrayConditionalDirective();

    if (context != ItemContext::MutableFieldGroup &&
        match({TokenKind::STATIC_ASSERT})) {
      return staticAssertDeclaration(previous());
    }

    if (context == ItemContext::MutableFieldGroup) {
      if (match({TokenKind::SEMICOLON})) {
        return std::make_shared<EmptyStmt>(previous());
      }
      return groupedFieldDeclaration();
    }

    if (match({TokenKind::USING})) {
      if (context == ItemContext::ClassMember) {
        return typeAliasDeclaration(previous());
      }
      throw error(previous(),
                  "Type aliases are limited to namespace or class scope.");
    }
    if (match({TokenKind::CONCEPT})) {
      throw error(previous(),
                  "Concept declarations are limited to namespace scope.");
    }
    if (isConformanceStart()) {
      throw error(peek(), "Conformance blocks are limited to namespace scope.");
    }
    if (match({TokenKind::EXTERN})) {
      throw error(previous(),
                  "extern \"C\" linkage blocks are limited to namespace "
                  "scope.");
    }

    if (context == ItemContext::ClassMember) {
      if (check(TokenKind::LEFT_BRACKET) &&
          peekAt(1).kind == TokenKind::LEFT_BRACKET) {
        return bracketAttributedDeclaration(LanguageLinkage::Gti);
      }
      if (match({TokenKind::CLASS, TokenKind::STRUCT, TokenKind::INTERFACE,
                 TokenKind::UNION})) {
        return classDeclaration({}, previous());
      }
      if (check(TokenKind::MUT) && peekAt(1).kind == TokenKind::LEFT_BRACE) {
        Token keyword = advance();
        return mutableFieldGroupDeclaration(std::move(keyword));
      }
      if (match(
              {TokenKind::PUBLIC, TokenKind::PRIVATE, TokenKind::PROTECTED})) {
        Token keyword = previous();
        consume(TokenKind::COLON, "Expect ':' after access specifier.");
        return std::make_shared<AccessSpecifierDecl>(
            keyword, keyword.kind == TokenKind::PUBLIC ? AccessModifier::Public
                     : keyword.kind == TokenKind::PROTECTED
                         ? AccessModifier::Protected
                         : AccessModifier::Private);
      }
      if (match({TokenKind::SEMICOLON})) {
        return std::make_shared<EmptyStmt>(previous());
      }
      if (match({TokenKind::TILDE})) {
        return destructorDeclaration(previous());
      }
      if (match({TokenKind::OPERATOR})) {
        return conversionOperatorDeclaration(previous());
      }
      if (const std::optional<std::size_t> modifiers =
              implicitConversionOperatorModifiers()) {
        return implicitConversionOperatorDeclaration(*modifiers);
      }
      if (check(TokenKind::CONSTEXPR) &&
          peekAt(1).kind == TokenKind::OPERATOR) {
        Token constant = advance();
        return conversionOperatorDeclaration(advance(), std::nullopt,
                                             std::move(constant));
      }
      if (check(TokenKind::VIRTUAL) && peekAt(1).kind == TokenKind::OPERATOR) {
        Token virtualKeyword = advance();
        Token operatorKeyword = advance();
        return conversionOperatorDeclaration(std::move(operatorKeyword),
                                             std::move(virtualKeyword));
      }
      if (const std::optional<std::size_t> modifiers =
              implicitConstructorModifiers()) {
        return implicitConstructorDeclaration(*modifiers);
      }
      if (check(TokenKind::UNSAFE) &&
          (isConstructorStart(1) ||
           (peekAt(1).kind == TokenKind::CONSTEXPR && isConstructorStart(2)))) {
        Token unsafeKeyword = advance();
        std::optional<Token> constexprKeyword;
        if (match({TokenKind::CONSTEXPR}))
          constexprKeyword = previous();
        return constructorDeclaration(advance(), std::move(constexprKeyword),
                                      std::move(unsafeKeyword));
      }
      if (check(TokenKind::CONSTEXPR) && isConstructorStart(1)) {
        Token constexprKeyword = advance();
        return constructorDeclaration(advance(), std::move(constexprKeyword));
      }
      if (isConstructorStart()) {
        return constructorDeclaration(advance());
      }
      if (isStructuredBindingPrefix()) {
        throw error(peek(),
                    "Structured bindings cannot declare class or struct "
                    "fields; decompose a value inside a function body.");
      }
      if (isTypedDeclaration()) {
        return typedDeclaration(true, std::nullopt, true, true);
      }
      throw error(peek(), "Expect a class member declaration.");
    }

    if (check(TokenKind::CONSTEXPR) && peekAt(1).kind == TokenKind::FOR) {
      return constexprForStatement();
    }
    if (isStructuredBindingPrefix()) {
      if (check(TokenKind::MUT)) {
        throw error(peek(), "Structured bindings are immutable; remove 'mut'.");
      }
      if (peekAt(1).kind == TokenKind::AMPERSAND) {
        throw error(peekAt(1),
                    "Reference structured bindings are not supported; the "
                    "binding owns one initialized value.");
      }
      return structuredBindingDeclaration();
    }
    if (isTypedDeclaration()) {
      return typedDeclaration(false, std::nullopt, false, false, false);
    }
    return statement();
  }

  StmtPtr structuredBindingDeclaration() {
    Token autoKeyword =
        consume(TokenKind::AUTO, "Expect 'auto' before structured bindings.");
    Token leftBracket =
        consume(TokenKind::LEFT_BRACKET,
                "Expect '[' after 'auto' in structured binding declaration.");
    std::vector<VariableDecl> bindings;
    do {
      if (check(TokenKind::RIGHT_BRACKET)) {
        throw error(peek(),
                    bindings.empty()
                        ? "Structured bindings require at least one name."
                        : "Trailing commas are not allowed in structured "
                          "bindings.");
      }
      Token name = consume(TokenKind::IDENTIFIER,
                           "Expect a name in structured binding declaration.");
      Token inferredType = generatedToken(TokenKind::AUTO, "auto", autoKeyword);
      bindings.emplace_back(Mutability::Immutable,
                            TypeRef(std::move(inferredType)), std::move(name),
                            nullptr);
    } while (match({TokenKind::COMMA}));
    Token rightBracket = consume(TokenKind::RIGHT_BRACKET,
                                 "Expect ']' after structured binding names.");
    Token equal = consume(
        TokenKind::EQUAL,
        "Expect '=' after structured binding names; the source value is "
        "required.");
    ExprPtr initializer = initializerExpression();
    consume(TokenKind::SEMICOLON,
            "Expect ';' after structured binding declaration.");
    return std::make_shared<StructuredBindingDecl>(
        std::move(autoKeyword), std::move(leftBracket), std::move(bindings),
        std::move(rightBracket), std::move(equal), std::move(initializer));
  }

  StmtPtr conditionalCompilation(ItemContext context) {
    // A conditional is open for the whole of its branch parsing and its
    // terminating `#endif`, so recovery inside it stops at each boundary and
    // lets this function consume it. Nested conditionals stack.
    const NestingGuard conditional(conditionalDepth);
    Token directive = previous();
    std::vector<ConditionalBranch> branches;
    const auto recoverCondition = [&](const Token &branchDirective) {
      try {
        return compileCondition(branchDirective);
      } catch (const ParseError &) {
        // Keep this opener alive while recovering its failed condition. The
        // branch collector, not outer declaration recovery, owns #endif.
        try {
          synchronize(context != ItemContext::Declaration,
                      context == ItemContext::Block,
                      context == ItemContext::Declaration,
                      context == ItemContext::ClassMember);
        } catch (const ConditionalBoundary &) {
          missingTokenError = false;
        }
        CompileCondition invalid;
        invalid.token = branchDirective;
        invalid.resolved = false;
        return invalid;
      }
    };
    branches.push_back(
        {recoverCondition(directive), conditionalItems(context)});

    while (match({TokenKind::HASH_ELIF})) {
      Token branchDirective = previous();
      branches.push_back(
          {recoverCondition(branchDirective), conditionalItems(context)});
    }
    if (match({TokenKind::HASH_ELSE})) {
      branches.push_back({std::nullopt, conditionalItems(context)});
    }

    consume(TokenKind::HASH_ENDIF,
            "Expect '#endif' after compile-time conditional.");
    return std::make_shared<ConditionalStmt>(std::move(directive),
                                             std::move(branches));
  }

  StmtPtr compileErrorDirective() {
    Token directive = previous();
    Token message = consume(TokenKind::STRING_LITERAL,
                            "Expect a string message after '#error'.");
    return std::make_unique<CompileErrorDirective>(std::move(directive),
                                                   std::move(message));
  }

  StmtList conditionalItems(ItemContext context) {
    StmtList statements;
    try {
      while (!isAtEnd() && !check(TokenKind::RIGHT_BRACE) &&
             !isConditionalBoundary()) {
        try {
          statements.emplace_back(item(context));
        } catch (const ParseError &) {
          synchronize(context != ItemContext::Declaration,
                      context == ItemContext::Block,
                      context == ItemContext::Declaration,
                      context == ItemContext::ClassMember);
        }
      }
    } catch (const ConditionalBoundary &) {
      missingTokenError = false;
    }
    return statements;
  }

  CompileCondition compileCondition(const Token &directive) {
    CompileCondition condition;
    if (directive.kind == TokenKind::HASH_IFDEF ||
        directive.kind == TokenKind::HASH_IFNDEF) {
      Token flag = consumeConditionIdentifier(
          "Expect a configuration flag name after '" + directive.lexeme + "'.");
      condition = CompileCondition::defined(std::move(flag));
      if (directive.kind == TokenKind::HASH_IFNDEF) {
        condition = CompileCondition::unary(CompileCondition::Kind::Not,
                                            directive, std::move(condition));
      }
    } else {
      condition = compileOrCondition();
    }
    return condition;
  }

  CompileCondition compileOrCondition() {
    CompileCondition condition = compileAndCondition();
    while (check(TokenKind::OR) && peek().lexeme == "||") {
      Token oper = advance();
      condition = CompileCondition::binary(
          CompileCondition::Kind::Or, std::move(condition), std::move(oper),
          compileAndCondition());
    }
    return condition;
  }

  CompileCondition compileAndCondition() {
    CompileCondition condition = compileUnaryCondition();
    while (check(TokenKind::AND) && peek().lexeme == "&&") {
      Token oper = advance();
      condition = CompileCondition::binary(
          CompileCondition::Kind::And, std::move(condition), std::move(oper),
          compileUnaryCondition());
    }
    return condition;
  }

  CompileCondition compileUnaryCondition() {
    const NestingGuard nesting = enterNesting(peek());
    if (match({TokenKind::BANG})) {
      Token oper = previous();
      return CompileCondition::unary(CompileCondition::Kind::Not,
                                     std::move(oper), compileUnaryCondition());
    }
    return compilePrimaryCondition();
  }

  CompileCondition compilePrimaryCondition() {
    if (match({TokenKind::LEFT_PAREN})) {
      CompileCondition condition = compileOrCondition();
      consumeCondition(TokenKind::RIGHT_PAREN,
                       "Expect ')' after compile-time condition.");
      return condition;
    }

    if (check(TokenKind::IDENTIFIER) && peek().lexeme == "defined") {
      advance();
      consumeCondition(TokenKind::LEFT_PAREN, "Expect '(' after 'defined'.");
      Token flag = consumeConditionIdentifier(
          "Expect a configuration flag name inside 'defined(...)'.");
      consumeCondition(TokenKind::RIGHT_PAREN,
                       "Expect ')' after configuration flag name.");
      return CompileCondition::defined(std::move(flag));
    }

    Token target = consumeCondition(
        TokenKind::IDENTIFIER,
        "Expect a compile-time condition after the directive.");
    if (target.lexeme != "target") {
      throw conditionError(
          target, "Compile-time conditions must use 'defined(NAME)', a target "
                  "comparison, or a parenthesized condition.");
    }
    consumeCondition(TokenKind::DOT, "Expect '.' after 'target'.");
    Token property =
        consumeCondition(TokenKind::IDENTIFIER,
                         "Expect 'os', 'vendor', or 'arch' after 'target.'.");

    const std::optional<TargetProperty> targetProperty =
        parseTargetProperty(property.lexeme);
    if (!targetProperty) {
      throw conditionError(property, "Unknown target property '" +
                                         property.lexeme +
                                         "'. Expected os, vendor, or arch.");
    }

    Token oper = consumeComparisonOperator();
    Token value =
        consumeCondition(TokenKind::STRING_LITERAL,
                         "Expect a string target value after comparison.");
    const auto *text = std::get_if<std::string>(&value.literal);
    std::string expected = text == nullptr ? std::string{} : *text;
    return CompileCondition::targetComparison(
        std::move(property), *targetProperty, std::move(oper), std::move(value),
        std::move(expected));
  }

  Token consumeComparisonOperator() {
    if (match({TokenKind::EQUAL_EQUAL, TokenKind::BANG_EQUAL})) {
      return previous();
    }
    throw conditionError(peek(),
                         "Expect '==' or '!=' in compile-time condition.");
  }

  Token consumeCondition(TokenKind kind, std::string_view message) {
    if (check(kind)) {
      return advance();
    }
    throw conditionError(peek(), message);
  }

  Token consumeConditionIdentifier(std::string_view message) {
    if (check(TokenKind::IDENTIFIER)) {
      return advance();
    }
    throw conditionError(peek(), message);
  }

  void rejectStrayConditionalDirective() {
    if (match({TokenKind::HASH_ELIF, TokenKind::HASH_ELSE,
               TokenKind::HASH_ENDIF})) {
      throw error(previous(), "Unexpected '" + previous().lexeme +
                                  "' without a matching '#if'.");
    }
  }

  [[nodiscard]] bool isConditionalBoundary() const {
    return check(TokenKind::HASH_ELIF) || check(TokenKind::HASH_ELSE) ||
           check(TokenKind::HASH_ENDIF);
  }

  StmtPtr statement() {
    const NestingGuard nesting = enterNesting(peek());
    if (match({TokenKind::UNSAFE})) {
      Token keyword = previous();
      consume(TokenKind::LEFT_BRACE, "Expect '{' after 'unsafe'.");
      return std::make_unique<BlockStmt>(blockItems(), std::move(keyword));
    }
    if (match({TokenKind::LEFT_BRACE})) {
      return std::make_unique<BlockStmt>(blockItems());
    }
    if (check(TokenKind::CONSTEXPR) && peekAt(1).kind == TokenKind::FOR) {
      return constexprForStatement();
    }
    if (match({TokenKind::IF})) {
      return ifStatement();
    }
    if (match({TokenKind::DO})) {
      return doWhileStatement();
    }
    if (match({TokenKind::WHILE})) {
      return whileStatement();
    }
    if (match({TokenKind::FOR})) {
      return forStatement();
    }
    if (match({TokenKind::SWITCH})) {
      return switchStatement();
    }
    if (match({TokenKind::BREAK, TokenKind::CONTINUE})) {
      return loopControlStatement();
    }
    if (match({TokenKind::RETURN})) {
      return returnStatement();
    }
    if (match({TokenKind::SEMICOLON})) {
      return std::make_shared<EmptyStmt>(previous());
    }
    if (check(TokenKind::LEFT_BRACKET) &&
        peekAt(1).kind == TokenKind::LEFT_BRACKET) {
      return attributedExpressionStatement();
    }

    return expressionStatement();
  }

  StmtPtr attributedExpressionStatement() {
    consume(TokenKind::LEFT_BRACKET, "Expect '[' to begin an attribute.");
    consume(TokenKind::LEFT_BRACKET, "Expect '[[' to begin an attribute.");
    Token attribute =
        consume(TokenKind::IDENTIFIER, "Expect an attribute name.");
    if (attribute.lexeme != "discard") {
      throw error(attribute, "Unknown statement attribute '[[" +
                                 attribute.lexeme + "]]'.");
    }
    consume(TokenKind::RIGHT_BRACKET, "Expect ']]' after 'discard'.");
    consume(TokenKind::RIGHT_BRACKET, "Expect ']]' after 'discard'.");
    return expressionStatement(std::move(attribute));
  }

  StmtPtr ifStatement() {
    std::optional<Token> constexprKeyword;
    if (match({TokenKind::CONSTEXPR})) {
      constexprKeyword = previous();
    }
    consume(TokenKind::LEFT_PAREN, "Expect '(' after 'if'.");
    ExprPtr condition = expression();
    consume(TokenKind::RIGHT_PAREN, "Expect ')' after if condition.");
    StmtPtr thenBranch = statement();
    StmtPtr elseBranch;
    if (match({TokenKind::ELSE})) {
      elseBranch = statement();
    }
    return std::make_shared<IfStmt>(std::move(condition), std::move(thenBranch),
                                    std::move(elseBranch),
                                    std::move(constexprKeyword));
  }

  StmtPtr whileStatement() {
    consume(TokenKind::LEFT_PAREN, "Expect '(' after 'while'.");
    ExprPtr condition = expression();
    consume(TokenKind::RIGHT_PAREN, "Expect ')' after while condition.");
    return std::make_shared<WhileStmt>(std::move(condition), statement());
  }

  StmtPtr doWhileStatement() {
    StmtPtr body = statement();
    consume(TokenKind::WHILE, "Expect 'while' after do-while body.");
    consume(TokenKind::LEFT_PAREN, "Expect '(' after 'while'.");
    ExprPtr condition = expression();
    consume(TokenKind::RIGHT_PAREN, "Expect ')' after do-while condition.");
    consume(TokenKind::SEMICOLON, "Expect ';' after do-while statement.");
    return std::make_shared<DoWhileStmt>(std::move(body), std::move(condition));
  }

  // constexpr for ( [mut] type IDENTIFIER : sequence ) statement
  //
  // The element binding is synthesized here as `auto name = <element>()`
  // with a generated callee that only semantic analysis understands; it is
  // cloned per element together with the body.
  StmtPtr constexprForStatement() {
    Token constexprKeyword = advance();
    Token keyword = advance();
    consume(TokenKind::LEFT_PAREN, "Expect '(' after 'constexpr for'.");
    const Mutability mutability =
        match({TokenKind::MUT}) ? Mutability::Mutable : Mutability::Immutable;
    TypeRef bindingType = parseType();
    Token name = consume(TokenKind::IDENTIFIER,
                         "Expect an element binding name in 'constexpr for'.");
    Token colon = consume(TokenKind::COLON,
                          "Expect ':' after the 'constexpr for' binding.");
    ExprPtr sequence = expression();
    consume(TokenKind::RIGHT_PAREN,
            "Expect ')' after the 'constexpr for' sequence.");
    StmtPtr body = statement();
    auto initializer = std::make_unique<Call>(
        generatedVariable(generatedToken(TokenKind::IDENTIFIER,
                                         "__constexpr_for_element", name)),
        std::vector<TypeRef>{},
        generatedToken(TokenKind::RIGHT_PAREN, ")", name), ExprList{});
    const Call *initializerPointer = initializer.get();
    auto elementBinding = std::make_shared<VariableDecl>(
        mutability, bindingType, name, std::move(initializer), std::nullopt,
        false, std::nullopt, false, false,
        mutability == Mutability::Mutable
            ? std::optional<Token>{generatedToken(TokenKind::MUT, "mut", name)}
            : std::nullopt);
    return std::make_shared<ConstexprForStmt>(
        std::move(constexprKeyword), std::move(keyword), mutability,
        std::move(bindingType), std::move(name), std::move(colon),
        std::move(sequence), std::move(body), std::move(elementBinding),
        initializerPointer);
  }

  StmtPtr forStatement() {
    const Token keyword = previous();
    consume(TokenKind::LEFT_PAREN, "Expect '(' after 'for'.");

    if (isStructuredBindingPrefix()) {
      throw error(peek(), "Structured bindings are not supported in a for-loop "
                          "initializer; declare the binding in the surrounding "
                          "block.");
    }

    if (isTypedDeclaration() && !check(TokenKind::STATIC) &&
        !check(TokenKind::CONSTEXPR) && !check(TokenKind::VIRTUAL)) {
      const std::size_t declarationStart = current;
      const Mutability mutability =
          match({TokenKind::MUT}) ? Mutability::Mutable : Mutability::Immutable;
      TypeRef type = parseType();
      Token name =
          consume(TokenKind::IDENTIFIER, "Expect range element binding name.");
      parseArrayDeclaratorSuffix(type);
      if (match({TokenKind::COLON})) {
        Token colon = previous();
        ExprPtr range = expression();
        consume(TokenKind::RIGHT_PAREN, "Expect ')' after range expression.");
        StmtPtr body = statement();
        return makeRangeForStatement(keyword, mutability, std::move(type),
                                     std::move(name), std::move(colon),
                                     std::move(range), std::move(body));
      }
      current = declarationStart;
    }

    StmtPtr initializer;
    if (match({TokenKind::SEMICOLON})) {
      initializer = std::make_shared<EmptyStmt>(previous());
    } else if (isTypedDeclaration()) {
      initializer = typedDeclaration(false, std::nullopt, false, false, false);
    } else {
      initializer = expressionStatement();
    }

    ExprPtr condition;
    if (!check(TokenKind::SEMICOLON)) {
      condition = expression();
    }
    consume(TokenKind::SEMICOLON, "Expect ';' after loop condition.");

    ExprPtr increment;
    if (!check(TokenKind::RIGHT_PAREN)) {
      increment = expression();
    }
    consume(TokenKind::RIGHT_PAREN, "Expect ')' after for clauses.");

    return std::make_shared<ForStmt>(std::move(initializer),
                                     std::move(condition), std::move(increment),
                                     statement());
  }

  [[nodiscard]] Token generatedToken(TokenKind kind, std::string lexeme,
                                     const Token &anchor) const {
    return Token(kind, std::move(lexeme), std::monostate{}, anchor.position,
                 anchor.line, anchor.source, false, true);
  }

  [[nodiscard]] ExprPtr generatedVariable(const Token &name) const {
    return std::make_unique<Variable>(name);
  }

  [[nodiscard]] ExprPtr generatedMemberCall(const Token &receiver,
                                            std::string member,
                                            const Token &anchor) const {
    Token dot = generatedToken(TokenKind::DOT, ".", anchor);
    Token memberName =
        generatedToken(TokenKind::IDENTIFIER, std::move(member), anchor);
    Token paren = generatedToken(TokenKind::RIGHT_PAREN, ")", anchor);
    return std::make_unique<Call>(
        std::make_unique<Get>(generatedVariable(receiver), std::move(dot),
                              std::move(memberName)),
        std::vector<TypeRef>{}, std::move(paren), ExprList{});
  }

  StmtPtr makeRangeForStatement(Token keyword, Mutability bindingMutability,
                                TypeRef bindingType, Token bindingName,
                                Token colon, ExprPtr range, StmtPtr body) {
    const bool mutableElementLoan =
        bindingMutability == Mutability::Mutable && bindingType.reference;
    const std::string suffix = std::to_string(++generatedNameCounter);
    Token ownerName =
        generatedToken(TokenKind::IDENTIFIER, "__gti_owner_" + suffix, colon);
    Token rangeName =
        generatedToken(TokenKind::IDENTIFIER, "__gti_range_" + suffix, colon);
    Token iteratorName = generatedToken(TokenKind::IDENTIFIER,
                                        "__gti_iterator_" + suffix, colon);
    Token sentinelName = generatedToken(TokenKind::IDENTIFIER,
                                        "__gti_sentinel_" + suffix, colon);

    TypeRef ownerType(generatedToken(TokenKind::AUTO, "auto", colon));
    TypeRef rangeType(generatedToken(TokenKind::AUTO, "auto", colon));
    rangeType.reference = generatedToken(TokenKind::AMPERSAND, "&", colon);

    TypeRef iteratorType(generatedToken(TokenKind::AUTO, "auto", colon));
    TypeRef sentinelType(generatedToken(TokenKind::AUTO, "auto", colon));

    StmtList loopBody;
    ExprPtr elementAccess = std::make_unique<Unary>(
        generatedToken(TokenKind::STAR, "*", colon),
        generatedVariable(iteratorName), generatedVariable(rangeName));
    const VariableDecl *elementOwnerBindingPointer = nullptr;
    if (!mutableElementLoan && bindingType.reference) {
      Token elementOwnerName = generatedToken(
          TokenKind::IDENTIFIER, "__gti_element_owner_" + suffix, colon);
      auto elementOwner = std::make_shared<VariableDecl>(
          Mutability::Immutable,
          TypeRef(generatedToken(TokenKind::AUTO, "auto", colon)),
          elementOwnerName, std::move(elementAccess), std::nullopt, false,
          std::nullopt, false, false, std::nullopt, true);
      elementOwnerBindingPointer = elementOwner.get();
      loopBody.emplace_back(std::move(elementOwner));
      elementAccess = generatedVariable(elementOwnerName);
    }
    auto elementBinding = std::make_shared<VariableDecl>(
        bindingMutability, bindingType, bindingName, std::move(elementAccess),
        std::nullopt, true);
    const VariableDecl *elementBindingPointer = elementBinding.get();
    loopBody.emplace_back(std::move(elementBinding));
    loopBody.emplace_back(std::move(body));

    auto coreLoop = std::make_shared<ForStmt>(
        std::make_shared<EmptyStmt>(
            generatedToken(TokenKind::SEMICOLON, ";", colon)),
        std::make_unique<Binary>(
            generatedVariable(iteratorName),
            generatedToken(TokenKind::BANG_EQUAL, "!=", colon),
            generatedVariable(sentinelName)),
        std::make_unique<Unary>(
            generatedToken(TokenKind::PLUS_PLUS, "++", colon),
            generatedVariable(iteratorName)),
        std::make_unique<BlockStmt>(std::move(loopBody)));
    const ForStmt *coreLoopPointer = coreLoop.get();

    StmtList lowered;
    auto ownerBinding = std::make_shared<VariableDecl>(
        mutableElementLoan ? Mutability::Mutable : Mutability::Immutable,
        std::move(ownerType), ownerName, std::move(range), std::nullopt, false,
        std::nullopt, true);
    const VariableDecl *ownerBindingPointer = ownerBinding.get();
    lowered.emplace_back(std::move(ownerBinding));
    auto rangeBinding = std::make_shared<VariableDecl>(
        mutableElementLoan ? Mutability::Mutable : Mutability::Immutable,
        std::move(rangeType), rangeName, generatedVariable(ownerName),
        std::nullopt, true);
    const VariableDecl *rangeBindingPointer = rangeBinding.get();
    lowered.emplace_back(std::move(rangeBinding));
    auto iteratorBinding = std::make_shared<VariableDecl>(
        Mutability::Mutable, std::move(iteratorType), iteratorName,
        generatedMemberCall(rangeName,
                            mutableElementLoan ? "begin_mut" : "begin", colon),
        std::nullopt, false, std::nullopt, false, true);
    const VariableDecl *iteratorBindingPointer = iteratorBinding.get();
    lowered.emplace_back(std::move(iteratorBinding));
    auto sentinelBinding = std::make_shared<VariableDecl>(
        Mutability::Immutable, std::move(sentinelType), sentinelName,
        generatedMemberCall(rangeName, "end", colon));
    const VariableDecl *sentinelBindingPointer = sentinelBinding.get();
    lowered.emplace_back(std::move(sentinelBinding));
    lowered.emplace_back(std::move(coreLoop));

    return std::make_shared<RangeForStmt>(
        std::move(keyword), bindingMutability, std::move(bindingType),
        std::move(bindingName), std::move(colon),
        std::make_unique<BlockStmt>(std::move(lowered)), ownerBindingPointer,
        rangeBindingPointer, iteratorBindingPointer, sentinelBindingPointer,
        elementBindingPointer, coreLoopPointer, elementOwnerBindingPointer);
  }

  StmtPtr switchStatement() {
    Token keyword = previous();
    consume(TokenKind::LEFT_PAREN, "Expect '(' after 'switch'.");
    ExprPtr value = expression();
    consume(TokenKind::RIGHT_PAREN, "Expect ')' after switch expression.");
    consume(TokenKind::LEFT_BRACE, "Expect '{' before switch arms.");

    std::vector<SwitchArm> arms;
    while (!check(TokenKind::RIGHT_BRACE) && !isAtEnd()) {
      try {
        if (!check(TokenKind::CASE) && !check(TokenKind::DEFAULT)) {
          throw error(peek(),
                      "Expect a 'case' or 'default' label inside switch.");
        }

        std::vector<SwitchLabel> labels;
        do {
          Token label = advance();
          ExprPtr caseValue;
          if (label.kind == TokenKind::CASE) {
            NestingGuard patternGuard(casePatternDepth);
            caseValue = logicalOr();
          }
          Token colon =
              consume(TokenKind::COLON, "Expect ':' after switch label.");
          labels.push_back(
              {std::move(label), std::move(caseValue), std::move(colon)});
        } while (check(TokenKind::CASE) || check(TokenKind::DEFAULT));

        StmtList statements;
        while (!check(TokenKind::CASE) && !check(TokenKind::DEFAULT) &&
               !check(TokenKind::RIGHT_BRACE) && !isAtEnd()) {
          try {
            statements.emplace_back(item(ItemContext::Block));
          } catch (const ParseError &) {
            synchronize(true, true, false, false, true);
          }
        }
        arms.push_back({std::move(labels), std::move(statements)});
      } catch (const ParseError &) {
        synchronize(true, false, false, false, true, false);
      }
    }

    consume(TokenKind::RIGHT_BRACE, "Expect '}' after switch arms.");
    return std::make_shared<SwitchStmt>(std::move(keyword), std::move(value),
                                        std::move(arms));
  }

  StmtPtr returnStatement() {
    Token keyword = previous();
    ExprPtr value;
    if (!check(TokenKind::SEMICOLON)) {
      value = initializerExpression();
    }
    consume(TokenKind::SEMICOLON, "Expect ';' after return value.");
    return std::make_shared<ReturnStmt>(keyword, std::move(value));
  }

  StmtPtr loopControlStatement() {
    Token keyword = previous();
    const std::string message = "Expect ';' after '" + keyword.lexeme + "'.";
    consume(TokenKind::SEMICOLON, message);
    return std::make_shared<LoopControlStmt>(std::move(keyword));
  }

  StmtPtr
  expressionStatement(std::optional<Token> discardAttribute = std::nullopt) {
    const bool hadCompletion = consumedCompletion;
    Token expressionStart = peek();
    ExprPtr value = expression();
    if (check(TokenKind::SEMICOLON)) {
      advance();
    } else if (hadCompletion == consumedCompletion) {
      consume(TokenKind::SEMICOLON, "Expect ';' after expression.");
    }
    return std::make_shared<ExpressionStmt>(std::move(expressionStart),
                                            std::move(value),
                                            std::move(discardAttribute));
  }

  ExprPtr expression() { return comma(); }

  ExprPtr comma() {
    ExprPtr expr = assignment();
    while (match({TokenKind::COMMA})) {
      Token oper = previous();
      expr = std::make_unique<Binary>(std::move(expr), oper, assignment());
    }
    return expr;
  }

  ExprPtr assignment() {
    const NestingGuard nesting = enterNesting(peek());
    ExprPtr expr = conditional();

    if (match({TokenKind::EQUAL, TokenKind::PLUS_EQUAL, TokenKind::MINUS_EQUAL,
               TokenKind::STAR_EQUAL, TokenKind::SLASH_EQUAL,
               TokenKind::PERCENT_EQUAL, TokenKind::AMPERSAND_EQUAL,
               TokenKind::PIPE_EQUAL, TokenKind::CARET_EQUAL,
               TokenKind::SHIFT_LEFT_EQUAL, TokenKind::SHIFT_RIGHT_EQUAL})) {
      Token oper = previous();
      ExprPtr value =
          check(TokenKind::LEFT_BRACE) ? arrayInitializer() : assignment();

      const Expr *ungrouped = expr.get();
      while (const auto *group = exprAs<Grouping>(ungrouped)) {
        if (group->unsafeKeyword())
          break;
        ungrouped = group->expression().get();
      }
      const auto *unaryTarget = exprAs<Unary>(ungrouped);
      if (exprAs<Variable>(ungrouped) || exprAs<QualifiedName>(ungrouped) ||
          exprAs<Get>(ungrouped) || exprAs<Index>(ungrouped) ||
          (unaryTarget && unaryTarget->oper().kind == TokenKind::STAR)) {
        // Parentheses do not change a traditional target's initialization,
        // ownership, or reserved-projection semantics.
        while (auto *group = exprAs<Grouping>(expr.get()))
          expr = group->takeExpression();
      }

      if (auto *variable = exprAs<Variable>(expr.get())) {
        return std::make_unique<Assign>(variable->name(), oper,
                                        std::move(value));
      }
      if (auto *qualified = exprAs<QualifiedName>(expr.get())) {
        return std::make_unique<Assign>(qualified->takeName(), oper,
                                        std::move(value));
      }
      if (auto *get = exprAs<Get>(expr.get())) {
        return std::make_unique<Set>(get->takeObject(), get->access(),
                                     get->name(), oper, std::move(value));
      }
      if (auto *index = exprAs<Index>(expr.get())) {
        return std::make_unique<IndexSet>(index->takeObject(), index->bracket(),
                                          index->takeIndex(), oper,
                                          std::move(value));
      }
      if (auto *unary = exprAs<Unary>(expr.get());
          unary != nullptr && unary->oper().kind == TokenKind::STAR) {
        return std::make_unique<DereferenceSet>(
            unary->oper(), unary->takeRight(), oper, std::move(value));
      }
      if (exprAs<Call>(ungrouped)) {
        return std::make_unique<PlaceSet>(std::move(expr), oper,
                                          std::move(value));
      }
      throw error(oper, "Invalid assignment target.");
    }

    return expr;
  }

  ExprPtr conditional() {
    ExprPtr expr = logicalOr();
    if (!match({TokenKind::QUESTION})) {
      return expr;
    }

    Token question = previous();
    ExprPtr thenExpression = expression();
    Token colon =
        consume(TokenKind::COLON, "Expect ':' in conditional expression.");
    ExprPtr elseExpression = assignment();
    return std::make_unique<ConditionalExpr>(
        std::move(expr), std::move(question), std::move(thenExpression),
        std::move(colon), std::move(elseExpression));
  }

  ExprPtr logicalOr() {
    ExprPtr expr = logicalAnd();
    while (match({TokenKind::OR})) {
      Token oper = previous();
      expr = std::make_unique<Logical>(std::move(expr), oper, logicalAnd());
    }
    return expr;
  }

  ExprPtr logicalAnd() {
    ExprPtr expr = bitwiseOr();
    while (match({TokenKind::AND})) {
      Token oper = previous();
      expr = std::make_unique<Logical>(std::move(expr), oper, bitwiseOr());
    }
    return expr;
  }

  ExprPtr bitwiseOr() {
    ExprPtr expr = bitwiseXor();
    while (match({TokenKind::PIPE})) {
      Token oper = previous();
      expr = std::make_unique<Binary>(std::move(expr), oper, bitwiseXor());
    }
    return expr;
  }

  ExprPtr bitwiseXor() {
    ExprPtr expr = bitwiseAnd();
    while (match({TokenKind::CARET})) {
      Token oper = previous();
      expr = std::make_unique<Binary>(std::move(expr), oper, bitwiseAnd());
    }
    return expr;
  }

  ExprPtr bitwiseAnd() {
    ExprPtr expr = equality();
    while (match({TokenKind::AMPERSAND})) {
      Token oper = previous();
      expr = std::make_unique<Binary>(std::move(expr), oper, equality());
    }
    return expr;
  }

  ExprPtr equality() {
    ExprPtr expr = comparison();
    while (match({TokenKind::BANG_EQUAL, TokenKind::EQUAL_EQUAL})) {
      Token oper = previous();
      expr = std::make_unique<Binary>(std::move(expr), oper, comparison());
    }
    return expr;
  }

  ExprPtr comparison() {
    ExprPtr expr = shift();
    while (match({TokenKind::GREATER, TokenKind::GREATER_EQUAL, TokenKind::LESS,
                  TokenKind::LESS_EQUAL})) {
      Token oper = previous();
      expr = std::make_unique<Binary>(std::move(expr), oper, shift());
    }
    return expr;
  }

  ExprPtr shift() {
    ExprPtr expr = term();
    while (const std::optional<Token> oper = matchShiftOperator()) {
      expr = std::make_unique<Binary>(std::move(expr), *oper, term());
    }
    return expr;
  }

  ExprPtr term() {
    ExprPtr expr = factor();
    while (match({TokenKind::MINUS, TokenKind::PLUS})) {
      Token oper = previous();
      expr = std::make_unique<Binary>(std::move(expr), oper, factor());
    }
    return expr;
  }

  ExprPtr factor() {
    ExprPtr expr = unary();
    while (match({TokenKind::PERCENT, TokenKind::SLASH, TokenKind::STAR})) {
      Token oper = previous();
      expr = std::make_unique<Binary>(std::move(expr), oper, unary());
    }
    return expr;
  }

  ExprPtr unary() {
    if (match({TokenKind::BANG, TokenKind::MINUS, TokenKind::PLUS,
               TokenKind::PLUS_PLUS, TokenKind::MINUS_MINUS, TokenKind::STAR,
               TokenKind::AMPERSAND, TokenKind::TILDE, TokenKind::QUESTION})) {
      const NestingGuard nesting = enterNesting(peek());
      Token oper = previous();
      return std::make_unique<Unary>(oper, unary());
    }
    return postfix();
  }

  ExprPtr postfix() {
    ExprPtr expr = primary();

    while (true) {
      if (isGenericOwnerStart()) {
        std::vector<TypeRef> arguments = typeArgumentList();
        NamePath owner = [&]() {
          if (auto *qualified = exprAs<QualifiedName>(expr.get()))
            return NamePath(qualified->name().segments);
          if (auto *variable = exprAs<Variable>(expr.get()))
            return NamePath(variable->name());
          throw error(peek(),
                      "A qualified generic owner requires a type name.");
        }();
        consume(TokenKind::SCOPE, "Expect '::' after generic owner type.");
        Token member = consume(TokenKind::IDENTIFIER,
                               "Expect member name after generic owner.");
        NamePath path(owner.segments);
        path.segments.push_back(std::move(member));
        expr = std::make_unique<QualifiedName>(
            std::move(path), TypeRef(std::move(owner), std::move(arguments)));
      } else if (isExplicitGenericCallStart()) {
        std::vector<TypeRef> typeArguments = typeArgumentList();
        consume(TokenKind::LEFT_PAREN,
                "Expect '(' after explicit generic arguments.");
        expr = finishCall(std::move(expr), std::move(typeArguments));
      } else if (match({TokenKind::LEFT_PAREN})) {
        expr = finishCall(std::move(expr), {});
      } else if (match({TokenKind::DOT})) {
        Token access = previous();
        Token name =
            consume(TokenKind::IDENTIFIER, "Expect property name after '.'.");
        expr = std::make_unique<Get>(std::move(expr), std::move(access), name);
      } else if (match({TokenKind::ARROW})) {
        Token access = previous();
        Token name =
            consume(TokenKind::IDENTIFIER, "Expect property name after '->'.");
        expr = std::make_unique<Get>(std::move(expr), std::move(access), name);
      } else if (match({TokenKind::LEFT_BRACKET})) {
        Token bracket = previous();
        ExprPtr index = expression();
        consume(TokenKind::RIGHT_BRACKET, "Expect ']' after array index.");
        expr = std::make_unique<Index>(std::move(expr), std::move(bracket),
                                       std::move(index));
      } else if (match({TokenKind::PLUS_PLUS, TokenKind::MINUS_MINUS})) {
        expr = std::make_unique<Postfix>(std::move(expr), previous());
      } else {
        break;
      }
    }

    return expr;
  }

  ExprPtr finishCall(ExprPtr callee, std::vector<TypeRef> typeArguments) {
    Token leftParen = previous();
    std::vector<Token> argumentCommas;
    ExprList arguments;
    if (!check(TokenKind::RIGHT_PAREN)) {
      do {
        ExprPtr argument;
        if (casePatternDepth != 0 &&
            (check(TokenKind::AUTO) || check(TokenKind::MUT))) {
          std::optional<Token> mutability;
          if (match({TokenKind::MUT}))
            mutability = previous();
          Token automatic = consume(
              TokenKind::AUTO, "Expect 'auto' in a borrowed payload pattern.");
          Token reference =
              consume(TokenKind::AMPERSAND,
                      "Expect '&' after 'auto' in a payload pattern.");
          Token name = consume(TokenKind::IDENTIFIER,
                               "Expect a payload binding name after '&'.");
          argument = std::make_unique<Variable>(
              std::move(name), std::move(automatic), std::move(reference),
              std::move(mutability));
        } else {
          argument =
              check(TokenKind::LEFT_BRACE) ? arrayInitializer() : assignment();
        }
        if (match({TokenKind::ELLIPSIS})) {
          const auto *variable = exprAs<Variable>(argument.get());
          if (variable == nullptr) {
            throw error(previous(),
                        "Only a named function parameter pack can be "
                        "expanded.");
          }
          argument =
              std::make_unique<PackExpansion>(variable->name(), previous());
          arguments.emplace_back(std::move(argument));
          if (check(TokenKind::COMMA)) {
            throw error(peek(),
                        "A parameter pack expansion must be the final call "
                        "argument.");
          }
          break;
        }
        arguments.emplace_back(std::move(argument));
        if (check(TokenKind::COMMA)) {
          argumentCommas.push_back(peek());
        }
      } while (match({TokenKind::COMMA}));
    }

    Token paren =
        consume(TokenKind::RIGHT_PAREN, "Expect ')' after arguments.");
    auto call =
        std::make_unique<Call>(std::move(callee), std::move(typeArguments),
                               paren, std::move(arguments));
    call->setArgumentGeometry(std::move(leftParen), std::move(argumentCommas));
    return call;
  }

  std::vector<TypeRef> typeArgumentList() {
    consume(TokenKind::LESS, "Expect '<' before generic arguments.");
    std::vector<TypeRef> arguments;
    if (!check(TokenKind::GREATER)) {
      do {
        arguments.emplace_back(parseGenericArgument());
      } while (match({TokenKind::COMMA}));
    }
    consumeGenericClose("Expect '>' after generic arguments.");
    return arguments;
  }

  std::unique_ptr<MemberRequirement> memberRequirement(Token keyword) {
    const NestingGuard nesting = enterNesting(keyword);
    consume(TokenKind::LEFT_PAREN, "Expect '(' after member requirement.");
    std::optional<Token> receiverAccess;
    if (match({TokenKind::MUT}))
      receiverAccess = previous();
    TypeRef receiver = parseType();
    if (!receiver.reference ||
        receiver.reference->kind != TokenKind::AMPERSAND) {
      throw error(receiver.name.last(),
                  "Member requirement receiver must be a reference.");
    }
    receiver.referenceMutability = std::move(receiverAccess);
    Token binding =
        consume(TokenKind::IDENTIFIER, "Expect requirement receiver name.");
    std::vector<TypeRef> parameters;
    std::vector<Token> bindings;
    while (match({TokenKind::COMMA})) {
      std::optional<Token> access;
      if (match({TokenKind::MUT}))
        access = previous();
      TypeRef parameter = parseType();
      if (access && !parameter.reference)
        throw error(*access,
                    "Mutable requirement parameters must be references.");
      parameter.referenceMutability = access;
      Token name =
          consume(TokenKind::IDENTIFIER, "Expect requirement parameter name.");
      if (name.lexeme == binding.lexeme ||
          std::any_of(
              bindings.begin(), bindings.end(),
              [&](const Token &other) { return other.lexeme == name.lexeme; }))
        throw error(name, "Duplicate requirement parameter name.");
      parameters.push_back(std::move(parameter));
      bindings.push_back(std::move(name));
    }
    consume(TokenKind::RIGHT_PAREN, "Expect ')' after requirement receiver.");
    consume(TokenKind::LEFT_BRACE, "Expect '{' before member requirement.");
    std::optional<Token> unsafeKeyword;
    if (match({TokenKind::UNSAFE}))
      unsafeKeyword = previous();
    consume(TokenKind::LEFT_BRACE, "Expect '{' before required member call.");
    Token use =
        consume(TokenKind::IDENTIFIER, "Expect requirement receiver name.");
    if (use.lexeme != binding.lexeme) {
      throw error(use, "Member requirement must use its declared receiver.");
    }
    consume(TokenKind::DOT, "Expect '.' before required member name.");
    Token member =
        consume(TokenKind::IDENTIFIER, "Expect required member name.");
    consume(TokenKind::LEFT_PAREN, "Expect '(' after required member name.");
    std::vector<Token> arguments;
    if (!check(TokenKind::RIGHT_PAREN)) {
      do {
        Token argument = consume(TokenKind::IDENTIFIER,
                                 "Expect requirement parameter name.");
        if (arguments.size() >= bindings.size() ||
            argument.lexeme != bindings[arguments.size()].lexeme)
          throw error(argument, "Required call must use each declared "
                                "parameter once, in declaration order.");
        arguments.push_back(std::move(argument));
      } while (match({TokenKind::COMMA}));
    }
    if (arguments.size() != bindings.size())
      throw error(peek(), "Required call must use every declared parameter.");
    consume(TokenKind::RIGHT_PAREN,
            "Expect ')' after required member arguments.");
    consume(TokenKind::RIGHT_BRACE, "Expect '}' after required member call.");
    consume(TokenKind::ARROW, "Expect '->' before exact required result type.");
    TypeRef result = parseType();
    consume(TokenKind::SEMICOLON, "Expect ';' after required result type.");
    consume(TokenKind::RIGHT_BRACE, "Expect '}' after member requirement.");
    return std::make_unique<MemberRequirement>(
        std::move(keyword), std::move(receiver), std::move(binding),
        std::move(use), std::move(member), std::move(result),
        std::move(parameters), std::move(bindings), std::move(arguments),
        std::move(unsafeKeyword));
  }

  ExprPtr primary() {
    if (match({TokenKind::UNSAFE})) {
      Token keyword = previous();
      consume(TokenKind::LEFT_BRACE, "Expect '{' after 'unsafe'.");
      ExprPtr value = expression();
      consume(
          TokenKind::RIGHT_BRACE,
          "Expect '}' after the unsafe expression (without a trailing ';').");
      return std::make_unique<Grouping>(std::move(value), std::move(keyword));
    }
    if (match({TokenKind::REQUIRES})) {
      return memberRequirement(previous());
    }
    if (match({TokenKind::LEFT_BRACKET})) {
      return lambdaExpression(previous());
    }
    if (match({TokenKind::SIZEOF, TokenKind::ALIGNOF})) {
      Token keyword = previous();
      consume(TokenKind::LEFT_PAREN,
              "Expect '(' after '" + keyword.lexeme + "'.");
      TypeRef type = parseType();
      consume(TokenKind::RIGHT_PAREN, "Expect ')' after layout query type.");
      return std::make_unique<LayoutQuery>(std::move(keyword), std::move(type));
    }
    // `bool(value)` is explicit like the numeric spellings: a class value
    // converts through its own operator bool.
    if ((isNumericTypeToken(peek().kind) || peek().kind == TokenKind::CHAR ||
         peek().kind == TokenKind::BOOL) &&
        peekAt(1).kind == TokenKind::LEFT_PAREN) {
      TypeRef targetType = parseType(false);
      consume(TokenKind::LEFT_PAREN, "Expect '(' after conversion type.");
      ExprPtr value = assignment();
      Token paren =
          consume(TokenKind::RIGHT_PAREN, "Expect ')' after converted value.");
      return std::make_unique<Conversion>(std::move(targetType),
                                          std::move(paren), std::move(value));
    }
    if (match({TokenKind::FALSE})) {
      return std::make_unique<LiteralExpr>(previous(), false);
    }
    if (match({TokenKind::TRUE})) {
      return std::make_unique<LiteralExpr>(previous(), true);
    }
    if (match({TokenKind::NULLPTR})) {
      return std::make_unique<LiteralExpr>(previous(), nullptr);
    }
    if (match({TokenKind::INT_LITERAL, TokenKind::FLOAT_LITERAL,
               TokenKind::STRING_LITERAL, TokenKind::CHARACTER_LITERAL})) {
      return std::make_unique<LiteralExpr>(previous(), previous().literal);
    }
    if (match({TokenKind::IDENTIFIER})) {
      Token first = previous();
      if (first.lexeme == "defined") {
        diagnostics.push_back(makeDiagnostic(
            "GTI-P0004", DiagnosticPhase::Parsing, first,
            "'defined' is available only inside a compile-time condition."));
        throw ParseError{};
      }
      if (check(TokenKind::SCOPE)) {
        return std::make_unique<QualifiedName>(parseNamePath(std::move(first)));
      }
      return std::make_unique<Variable>(std::move(first));
    }
    if (match({TokenKind::THIS})) {
      return std::make_unique<This>(previous());
    }
    if (match({TokenKind::UNEXPECTED})) {
      Token keyword = previous();
      consume(TokenKind::LEFT_PAREN, "Expect '(' after 'unexpected'.");
      ExprPtr errorValue = assignment();
      consume(TokenKind::RIGHT_PAREN,
              "Expect ')' after unexpected error value.");
      return std::make_unique<Unexpected>(std::move(keyword),
                                          std::move(errorValue));
    }
    if (match({TokenKind::LEFT_PAREN})) {
      Token leftParen = previous();
      ExprPtr expr = assignment();
      if (match({TokenKind::COMMA})) {
        Token comma = previous();
        if (match({TokenKind::ELLIPSIS})) {
          Token ellipsis = previous();
          if (exprAs<Call>(expr.get()) == nullptr) {
            throw error(
                ellipsis,
                "A pack fold pattern must be a function or method call.");
          }
          Token rightParen =
              consume(TokenKind::RIGHT_PAREN,
                      "Expect ')' after the pack fold ellipsis.");
          return std::make_unique<PackFold>(
              std::move(leftParen), std::move(expr), std::move(comma),
              std::move(ellipsis), std::move(rightParen));
        }

        expr = std::make_unique<Binary>(std::move(expr), comma, assignment());
        while (match({TokenKind::COMMA})) {
          Token oper = previous();
          expr = std::make_unique<Binary>(std::move(expr), std::move(oper),
                                          assignment());
        }
      }
      consume(TokenKind::RIGHT_PAREN, "Expect ')' after expression.");
      return std::make_unique<Grouping>(std::move(expr));
    }

    throw error(peek(), "Expect expression.");
  }

  ExprPtr lambdaExpression(Token bracket) {
    std::vector<LambdaCapture> captures;
    if (!check(TokenKind::RIGHT_BRACKET)) {
      do {
        if (check(TokenKind::EQUAL)) {
          throw error(peek(),
                      "Lambda capture defaults are not supported; list each "
                      "captured local by name.");
        }
        std::optional<Token> mutKeyword;
        std::optional<Token> borrow;
        if (match({TokenKind::MUT}))
          mutKeyword = previous();
        if (match({TokenKind::AMPERSAND}))
          borrow = previous();
        else if (mutKeyword)
          throw error(peek(), "Expect '&' after 'mut' in a borrowing capture.");
        Token name =
            consume(TokenKind::IDENTIFIER,
                    "Expect a local binding name in lambda capture list.");
        std::optional<Token> equal;
        ExprPtr initializer;
        if (match({TokenKind::EQUAL})) {
          equal = previous();
          if (borrow)
            throw error(*equal,
                        "A borrowing capture names an existing binding; "
                        "bind the selected place to a reference first.");
          initializer = assignment();
        } else {
          Token source =
              generatedToken(TokenKind::IDENTIFIER, name.lexeme, name);
          initializer = generatedVariable(source);
        }
        captures.emplace_back(std::move(name), std::move(equal),
                              std::move(initializer), std::move(borrow),
                              std::move(mutKeyword));
      } while (match({TokenKind::COMMA}));
    }
    consume(TokenKind::RIGHT_BRACKET, "Expect ']' after lambda capture list.");
    consume(TokenKind::LEFT_PAREN, "Expect '(' after lambda capture list.");
    std::vector<Parameter> parameters = parameterList();
    Token arrow = consume(TokenKind::ARROW,
                          "Expect '->' and an explicit lambda return type.");
    TypeRef returnType = parseType();
    consume(TokenKind::LEFT_BRACE, "Expect '{' before lambda body.");
    return std::make_unique<Lambda>(std::move(bracket), std::move(captures),
                                    std::move(parameters), std::move(arrow),
                                    std::move(returnType), blockItems());
  }

  [[nodiscard]] static bool isIdentifierUseToken(TokenKind kind) {
    return kind == TokenKind::IDENTIFIER || kind == TokenKind::CPP_RESERVED ||
           kind == TokenKind::DELETE;
  }

  [[nodiscard]] bool isTypedDeclaration() const {
    std::size_t offset = check(TokenKind::UNSAFE) ? 1 : 0;
    if (peekAt(offset).kind == TokenKind::VIRTUAL) {
      ++offset;
    }
    if (peekAt(offset).kind == TokenKind::STATIC) {
      ++offset;
    }
    if (peekAt(offset).kind == TokenKind::CONSTEXPR) {
      ++offset;
    }
    if (peekAt(offset).kind == TokenKind::MUT) {
      ++offset;
    }
    const TokenKind first = peekAt(offset).kind;
    if (first != TokenKind::CONST && first != TokenKind::AUTO &&
        first != TokenKind::INT && first != TokenKind::INT8 &&
        first != TokenKind::INT16 && first != TokenKind::INT32 &&
        first != TokenKind::INT64 && first != TokenKind::UINT &&
        first != TokenKind::UINT8 && first != TokenKind::UINT16 &&
        first != TokenKind::UINT32 && first != TokenKind::UINT64 &&
        first != TokenKind::FLOAT && first != TokenKind::DOUBLE &&
        first != TokenKind::BOOL && first != TokenKind::CHAR &&
        first != TokenKind::EXPECTED && first != TokenKind::NULLPTR_TYPE &&
        first != TokenKind::VOID && first != TokenKind::IDENTIFIER) {
      return false;
    }
    const std::optional<std::size_t> end = typeEnd(offset);
    return end && (isIdentifierUseToken(peekAt(*end).kind) ||
                   peekAt(*end).kind == TokenKind::OPERATOR);
  }

  [[nodiscard]] bool isStructuredBindingPrefix() const {
    std::size_t offset = 0;
    if (peekAt(offset).kind == TokenKind::MUT) {
      ++offset;
    }
    if (peekAt(offset).kind != TokenKind::AUTO) {
      return false;
    }
    ++offset;
    if (peekAt(offset).kind == TokenKind::AMPERSAND) {
      ++offset;
    }
    if (peekAt(offset).kind != TokenKind::LEFT_BRACKET) {
      return false;
    }
    std::size_t depth = 0;
    for (; peekAt(offset).kind != TokenKind::END_OF_FILE; ++offset) {
      if (peekAt(offset).kind == TokenKind::LEFT_BRACKET) {
        ++depth;
      } else if (peekAt(offset).kind == TokenKind::RIGHT_BRACKET &&
                 --depth == 0) {
        return peekAt(offset + 1).kind != TokenKind::IDENTIFIER;
      }
    }
    return true;
  }

  [[nodiscard]] static bool isNumericTypeToken(TokenKind kind) {
    return kind == TokenKind::INT || kind == TokenKind::INT8 ||
           kind == TokenKind::INT16 || kind == TokenKind::INT32 ||
           kind == TokenKind::INT64 || kind == TokenKind::UINT ||
           kind == TokenKind::UINT8 || kind == TokenKind::UINT16 ||
           kind == TokenKind::UINT32 || kind == TokenKind::UINT64 ||
           kind == TokenKind::FLOAT || kind == TokenKind::DOUBLE;
  }

  [[nodiscard]] bool isValueGenericParameterStart() const {
    return (isNumericTypeToken(peek().kind) ||
            peek().kind == TokenKind::BOOL) &&
           isIdentifierUseToken(peekAt(1).kind);
  }

  [[nodiscard]] bool isExplicitGenericCallStart() const {
    if (!check(TokenKind::LESS)) {
      return false;
    }
    std::size_t next = 1;
    if (peekAt(next).kind == TokenKind::GREATER) {
      return peekAt(next + 1).kind == TokenKind::LEFT_PAREN;
    }
    const std::optional<std::size_t> firstArgumentEnd =
        genericArgumentEnd(next);
    if (!firstArgumentEnd) {
      return false;
    }
    next = *firstArgumentEnd;
    while (peekAt(next).kind == TokenKind::COMMA) {
      const std::optional<std::size_t> argumentEnd =
          genericArgumentEnd(next + 1);
      if (!argumentEnd) {
        return false;
      }
      next = *argumentEnd;
    }
    return peekAt(next).kind == TokenKind::GREATER &&
           peekAt(next + 1).kind == TokenKind::LEFT_PAREN;
  }

  [[nodiscard]] bool isGenericOwnerStart() const {
    if (!check(TokenKind::LESS))
      return false;
    std::size_t next = 1;
    if (peekAt(next).kind == TokenKind::GREATER)
      return peekAt(next + 1).kind == TokenKind::SCOPE;
    auto end = genericArgumentEnd(next);
    if (!end)
      return false;
    next = *end;
    while (peekAt(next).kind == TokenKind::COMMA) {
      end = genericArgumentEnd(next + 1);
      if (!end)
        return false;
      next = *end;
    }
    return peekAt(next).kind == TokenKind::GREATER &&
           peekAt(next + 1).kind == TokenKind::SCOPE;
  }

  [[nodiscard]]
  std::optional<std::size_t> genericArgumentEnd(std::size_t offset) const {
    const TokenKind first = peekAt(offset).kind;
    const std::optional<std::size_t> expressionEnd =
        arrayExtentExpressionEnd(offset);
    const bool expressionReachesDelimiter =
        expressionEnd && (peekAt(*expressionEnd).kind == TokenKind::COMMA ||
                          peekAt(*expressionEnd).kind == TokenKind::GREATER);
    if (first == TokenKind::INT_LITERAL ||
        (expressionReachesDelimiter &&
         (first == TokenKind::LEFT_PAREN ||
          (isIdentifierUseToken(first) && *expressionEnd > offset + 1)))) {
      return expressionEnd;
    }
    const auto end = typeEnd(offset);
    return end && peekAt(*end).kind == TokenKind::ELLIPSIS
               ? std::optional<std::size_t>(*end + 1)
               : end;
  }

  [[nodiscard]] std::optional<std::size_t>
  arrayExtentPrimaryEnd(std::size_t offset) const {
    if (peekAt(offset).kind == TokenKind::INT_LITERAL ||
        isIdentifierUseToken(peekAt(offset).kind)) {
      return offset + 1;
    }
    if (peekAt(offset).kind != TokenKind::LEFT_PAREN) {
      return std::nullopt;
    }
    const std::optional<std::size_t> expression =
        arrayExtentExpressionEnd(offset + 1);
    if (!expression || peekAt(*expression).kind != TokenKind::RIGHT_PAREN) {
      return std::nullopt;
    }
    return *expression + 1;
  }

  [[nodiscard]] std::optional<std::size_t>
  arrayExtentMultiplicativeEnd(std::size_t offset) const {
    std::optional<std::size_t> expression = arrayExtentPrimaryEnd(offset);
    if (!expression) {
      return std::nullopt;
    }
    while (peekAt(*expression).kind == TokenKind::STAR ||
           peekAt(*expression).kind == TokenKind::SLASH ||
           peekAt(*expression).kind == TokenKind::PERCENT) {
      expression = arrayExtentPrimaryEnd(*expression + 1);
      if (!expression) {
        return std::nullopt;
      }
    }
    return expression;
  }

  [[nodiscard]] std::optional<std::size_t>
  arrayExtentExpressionEnd(std::size_t offset) const {
    std::optional<std::size_t> expression =
        arrayExtentMultiplicativeEnd(offset);
    if (!expression) {
      return std::nullopt;
    }
    while (peekAt(*expression).kind == TokenKind::PLUS ||
           peekAt(*expression).kind == TokenKind::MINUS) {
      expression = arrayExtentMultiplicativeEnd(*expression + 1);
      if (!expression) {
        return std::nullopt;
      }
    }
    return expression;
  }

  [[nodiscard]] std::optional<std::size_t> typeEnd(std::size_t offset) const {
    if (peekAt(offset).kind == TokenKind::CONST) {
      const std::optional<std::size_t> qualified = typeEnd(offset + 1);
      if (!qualified) {
        return std::nullopt;
      }
      std::size_t declarator = *qualified;
      if (declarator > offset + 1 &&
          (peekAt(declarator - 1).kind == TokenKind::AMPERSAND ||
           peekAt(declarator - 1).kind == TokenKind::AND)) {
        --declarator;
      }
      while (declarator >= offset + 4 &&
             peekAt(declarator - 1).kind == TokenKind::RIGHT_BRACKET) {
        declarator -= 3;
      }
      return declarator > offset + 1 &&
                     peekAt(declarator - 1).kind == TokenKind::STAR
                 ? qualified
                 : std::nullopt;
    }
    const TokenKind first = peekAt(offset).kind;
    if (first == TokenKind::EXPECTED) {
      if (peekAt(offset + 1).kind != TokenKind::LESS) {
        return std::nullopt;
      }
      const std::optional<std::size_t> valueEnd = typeEnd(offset + 2);
      if (!valueEnd || peekAt(*valueEnd).kind != TokenKind::COMMA) {
        return std::nullopt;
      }
      const std::optional<std::size_t> errorEnd = typeEnd(*valueEnd + 1);
      if (!errorEnd || peekAt(*errorEnd).kind != TokenKind::GREATER) {
        return std::nullopt;
      }
      return signatureTypeEnd(*errorEnd + 1);
    }

    if (first == TokenKind::AUTO || first == TokenKind::INT ||
        first == TokenKind::INT8 || first == TokenKind::INT16 ||
        first == TokenKind::INT32 || first == TokenKind::INT64 ||
        first == TokenKind::UINT || first == TokenKind::UINT8 ||
        first == TokenKind::UINT16 || first == TokenKind::UINT32 ||
        first == TokenKind::UINT64 || first == TokenKind::FLOAT ||
        first == TokenKind::DOUBLE || first == TokenKind::BOOL ||
        first == TokenKind::CHAR || first == TokenKind::NULLPTR_TYPE ||
        first == TokenKind::VOID) {
      return signatureTypeEnd(offset + 1);
    }
    if (first != TokenKind::IDENTIFIER) {
      return std::nullopt;
    }

    std::size_t next = offset + 1;
    while (true) {
      if (peekAt(next).kind == TokenKind::LESS) {
        if (peekAt(next + 1).kind == TokenKind::GREATER) {
          next += 2;
        } else {
          do {
            const auto argumentEnd = genericArgumentEnd(next + 1);
            if (!argumentEnd) {
              return std::nullopt;
            }
            next = *argumentEnd;
          } while (peekAt(next).kind == TokenKind::COMMA);
          if (peekAt(next).kind != TokenKind::GREATER) {
            return std::nullopt;
          }
          ++next;
        }
      }
      if (peekAt(next).kind != TokenKind::SCOPE ||
          !isIdentifierUseToken(peekAt(next + 1).kind)) {
        return signatureTypeEnd(next);
      }
      next += 2;
    }
  }

  [[nodiscard]] std::optional<std::size_t>
  signatureTypeEnd(std::size_t offset) const {
    auto end = arrayTypeEnd(offset);
    if (!end || peekAt(*end).kind != TokenKind::LEFT_PAREN) {
      return end;
    }
    std::size_t next = *end + 1;
    if (peekAt(next).kind != TokenKind::RIGHT_PAREN) {
      while (true) {
        if (peekAt(next).kind == TokenKind::MUT) {
          ++next;
        }
        const auto parameter = typeEnd(next);
        if (!parameter) {
          return std::nullopt;
        }
        next = *parameter;
        if (peekAt(next).kind == TokenKind::ELLIPSIS) {
          ++next;
        }
        if (peekAt(next).kind != TokenKind::COMMA) {
          break;
        }
        ++next;
      }
    }
    if (peekAt(next).kind != TokenKind::RIGHT_PAREN) {
      return std::nullopt;
    }
    ++next;
    if (peekAt(next).kind == TokenKind::MUT ||
        (peekAt(next).kind == TokenKind::AND && peekAt(next).lexeme == "&&")) {
      ++next;
    }
    return next;
  }

  [[nodiscard]] std::optional<std::size_t>
  arrayTypeEnd(std::size_t offset) const {
    while (peekAt(offset).kind == TokenKind::STAR) {
      ++offset;
      if (peekAt(offset).kind == TokenKind::CONST) {
        ++offset;
      }
    }
    while (peekAt(offset).kind == TokenKind::LEFT_BRACKET) {
      if ((peekAt(offset + 1).kind != TokenKind::INT_LITERAL &&
           !isIdentifierUseToken(peekAt(offset + 1).kind)) ||
          peekAt(offset + 2).kind != TokenKind::RIGHT_BRACKET) {
        return std::nullopt;
      }
      offset += 3;
    }
    if (peekAt(offset).kind == TokenKind::AMPERSAND) {
      ++offset;
    }
    return offset;
  }

  [[nodiscard]] bool isConstructorStart(std::size_t start = 0) const {
    if (!currentClassName || peekAt(start).kind != TokenKind::IDENTIFIER ||
        peekAt(start).lexeme != currentClassName->lexeme) {
      return false;
    }
    if (peekAt(start + 1).kind == TokenKind::LEFT_PAREN) {
      return true;
    }
    if (peekAt(start + 1).kind != TokenKind::LESS) {
      return false;
    }
    std::size_t offset = start + 2;
    std::size_t depth = 1;
    while (peekAt(offset).kind != TokenKind::END_OF_FILE) {
      if (peekAt(offset).kind == TokenKind::LESS) {
        ++depth;
      } else if (peekAt(offset).kind == TokenKind::GREATER && --depth == 0) {
        return peekAt(offset + 1).kind == TokenKind::LEFT_PAREN;
      }
      ++offset;
    }
    return false;
  }

  [[nodiscard]] bool isDestructorStart() const {
    return check(TokenKind::TILDE) && peekAt(1).kind == TokenKind::IDENTIFIER &&
           peekAt(2).kind == TokenKind::LEFT_PAREN;
  }

  std::optional<Token> matchShiftOperator() {
    TokenKind combined;
    if (check(TokenKind::LESS) && peekAt(1).kind == TokenKind::LESS) {
      combined = TokenKind::SHIFT_LEFT;
    } else if (check(TokenKind::GREATER) &&
               peekAt(1).kind == TokenKind::GREATER) {
      combined = TokenKind::SHIFT_RIGHT;
    } else {
      return std::nullopt;
    }

    const Token &first = peek();
    const Token &second = peekAt(1);
    if (first.source != second.source || first.line != second.line ||
        first.position + first.lexeme.size() != second.position) {
      return std::nullopt;
    }

    Token oper = advance();
    oper.kind = combined;
    oper.lexeme += advance().lexeme;
    return oper;
  }

  bool match(std::initializer_list<TokenKind> kinds) {
    for (TokenKind kind : kinds) {
      if (check(kind)) {
        advance();
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool check(TokenKind kind) const {
    return !isAtEnd() && peek().kind == kind;
  }

  [[nodiscard]] bool isAtEnd() const {
    return peek().kind == TokenKind::END_OF_FILE;
  }

  [[nodiscard]] const Token &peek() const { return tokens.at(current); }

  [[nodiscard]] const Token &peekAt(std::size_t offset) const {
    const std::size_t index = current + offset;
    return tokens.at(index < tokens.size() ? index : tokens.size() - 1);
  }

  const Token &advance() {
    if (!isAtEnd()) {
      consumedCompletion = consumedCompletion || peek().completion;
      ++current;
    }
    return previous();
  }

  [[nodiscard]] const Token &previous() const { return tokens.at(current - 1); }

  Token consume(TokenKind kind, std::string_view message) {
    if (check(kind)) {
      return advance();
    }
    if (reservedIdentifierError(peek())) {
      throw ParseError{};
    }
    missingTokenError = true;
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-P0001", DiagnosticPhase::Parsing, peek(), std::string(message));
    if (const std::optional<std::string_view> spelling = fixedSpelling(kind)) {
      SourceSpan insertion = insertionPoint();
      diagnostic.fixes.push_back({std::move(insertion), std::string(*spelling),
                                  "Insert '" + std::string(*spelling) + "'."});
    }
    diagnostics.emplace_back(std::move(diagnostic));
    throw ParseError{};
  }

  // In a generic-close position, a greedily lexed `>=` is the two grammar
  // tokens `>` and `=`. `>>=` follows the same rule one close at a time, so a
  // nested generic can leave the final assignment token for its caller. The
  // split is deliberately contextual: ordinary comparison and shift-assignment
  // expressions retain their original lexer tokens.
  Token consumeGenericClose(std::string_view message) {
    if (check(TokenKind::GREATER)) {
      return advance();
    }
    if (check(TokenKind::GREATER_EQUAL) ||
        check(TokenKind::SHIFT_RIGHT_EQUAL)) {
      Token close = peek();
      consumedCompletion = consumedCompletion || close.completion;
      close.kind = TokenKind::GREATER;
      close.lexeme = ">";

      Token &remainder = tokens.at(current);
      remainder.kind = remainder.kind == TokenKind::GREATER_EQUAL
                           ? TokenKind::EQUAL
                           : TokenKind::GREATER_EQUAL;
      remainder.lexeme = remainder.kind == TokenKind::EQUAL ? "=" : ">=";
      ++remainder.position;
      remainder.literal = std::monostate{};
      remainder.leadingDocumentationMarkdown.reset();
      remainder.configurationFlagDefined.reset();
      remainder.configurationFlagReference = false;
      return close;
    }
    return consume(TokenKind::GREATER, message);
  }

  ParseError error(const Token &token, std::string_view message) {
    missingTokenError = false;
    if (reservedIdentifierError(token)) {
      return ParseError{};
    }
    diagnostics.push_back(makeDiagnostic("GTI-P0001", DiagnosticPhase::Parsing,
                                         token, std::string(message)));
    return ParseError{};
  }

  void checkNestingDepth(const Token &token, std::size_t additionalDepth = 0) {
    if (nestingDepth + additionalDepth >= maximumNestingDepth) {
      missingTokenError = false;
      Diagnostic diagnostic =
          makeDiagnostic("GTI-P0005", DiagnosticPhase::Parsing, token,
                         "Source nesting exceeds the parser limit of " +
                             std::to_string(maximumNestingDepth) + ".");
      diagnostic.hints.emplace_back(
          "Split the expression, type, or statement into shallower parts.");
      diagnostics.emplace_back(std::move(diagnostic));
      throw ParseError{};
    }
  }

  [[nodiscard]] NestingGuard enterNesting(const Token &token) {
    checkNestingDepth(token);
    return NestingGuard(nestingDepth);
  }

  ParseError conditionError(const Token &token, std::string_view message) {
    missingTokenError = false;
    diagnostics.push_back(makeDiagnostic("GTI-P0003", DiagnosticPhase::Parsing,
                                         token, std::string(message)));
    return ParseError{};
  }

  bool reservedIdentifierError(const Token &token) {
    std::string message;
    if (token.kind == TokenKind::CPP_RESERVED) {
      message = "'" + token.lexeme +
                "' is a reserved C++ keyword and cannot be used as a GTI "
                "identifier.";
    } else if (token.kind == TokenKind::DELETE) {
      message = "'delete' is reserved for '= delete' special-member policy "
                "and cannot be used as a GTI identifier.";
    } else {
      return false;
    }

    missingTokenError = false;
    diagnostics.push_back(makeDiagnostic("GTI-P0002", DiagnosticPhase::Parsing,
                                         token, std::move(message)));
    return true;
  }

  [[nodiscard]] SourceSpan insertionPoint() const {
    const Token &token = peek();
    if (current > 0 && previous().source != token.source) {
      const Token &last = previous();
      return {last.source, last.position + last.lexeme.size(),
              last.position + last.lexeme.size(), last.line};
    }
    return {token.source, token.position, token.position, token.line};
  }

  [[nodiscard]] static std::optional<std::string_view>
  fixedSpelling(TokenKind kind) {
    switch (kind) {
    case TokenKind::LEFT_PAREN:
      return "(";
    case TokenKind::RIGHT_PAREN:
      return ")";
    case TokenKind::LEFT_BRACE:
      return "{";
    case TokenKind::RIGHT_BRACE:
      return "}";
    case TokenKind::LEFT_BRACKET:
      return "[";
    case TokenKind::RIGHT_BRACKET:
      return "]";
    case TokenKind::COLON:
      return ":";
    case TokenKind::COMMA:
      return ",";
    case TokenKind::DOT:
      return ".";
    case TokenKind::EQUAL:
      return "=";
    case TokenKind::SEMICOLON:
      return ";";
    case TokenKind::LESS:
      return "<";
    case TokenKind::GREATER:
      return ">";
    case TokenKind::ARROW:
      return "->";
    default:
      return std::nullopt;
    }
  }

  void synchronize(bool stopAtRightBrace, bool allowStatements,
                   bool allowClasses, bool allowAccessSpecifiers = false,
                   bool stopAtSwitchLabel = false,
                   bool stopAtTypedDeclaration = true) {
    if (isAtEnd()) {
      return;
    }

    bool advanced = false;
    std::size_t parenthesisDepth = 0;
    std::size_t braceDepth = 0;
    while (!isAtEnd()) {
      const bool atRecoveryBoundary = parenthesisDepth == 0 && braceDepth == 0;
      if (atRecoveryBoundary) {
        // Unwind nested recovery to the nearest conditional branch collector.
        // It leaves the directive for conditionalCompilation to consume.
        if (conditionalDepth > 0 && isConditionalBoundary()) {
          throw ConditionalBoundary{};
        }
        if (stopAtRightBrace && check(TokenKind::RIGHT_BRACE)) {
          return;
        }
        if (stopAtSwitchLabel &&
            (check(TokenKind::CASE) || check(TokenKind::DEFAULT))) {
          return;
        }
        if (advanced && previous().kind == TokenKind::SEMICOLON) {
          return;
        }
        if (stopAtTypedDeclaration &&
            (isTypedDeclaration() || (isStructuredBindingPrefix() &&
                                      (advanced || missingTokenError)))) {
          return;
        }
        if (allowAccessSpecifiers &&
            (check(TokenKind::PUBLIC) || check(TokenKind::PRIVATE) ||
             check(TokenKind::PROTECTED))) {
          return;
        }
        if (allowAccessSpecifiers && isConstructorStart()) {
          return;
        }
        if (allowAccessSpecifiers && isDestructorStart()) {
          return;
        }
        if (allowAccessSpecifiers && check(TokenKind::OPERATOR)) {
          return;
        }
        if (allowAccessSpecifiers && check(TokenKind::TYPENAME)) {
          return;
        }

        if (allowClasses &&
            (check(TokenKind::HASH_IF) || check(TokenKind::HASH_IFDEF) ||
             check(TokenKind::HASH_IFNDEF) || check(TokenKind::HASH_ERROR) ||
             check(TokenKind::AT) || check(TokenKind::CLASS) ||
             check(TokenKind::CONCEPT) || check(TokenKind::ENUM) ||
             check(TokenKind::EXTERN) || check(TokenKind::STRUCT) ||
             check(TokenKind::INTERFACE) || check(TokenKind::UNION) ||
             check(TokenKind::NAMESPACE) || check(TokenKind::USING) ||
             isConformanceStart())) {
          return;
        }
        if (allowStatements &&
            (check(TokenKind::HASH_IF) || check(TokenKind::HASH_IFDEF) ||
             check(TokenKind::HASH_IFNDEF) || check(TokenKind::HASH_ERROR) ||
             check(TokenKind::LEFT_BRACKET) || check(TokenKind::BREAK) ||
             check(TokenKind::CONTINUE) || check(TokenKind::FOR) ||
             check(TokenKind::DO) || check(TokenKind::IF) ||
             check(TokenKind::RETURN) || check(TokenKind::SWITCH) ||
             check(TokenKind::UNSAFE) || check(TokenKind::WHILE))) {
          return;
        }
      }

      if (check(TokenKind::LEFT_PAREN)) {
        ++parenthesisDepth;
      } else if (check(TokenKind::RIGHT_PAREN) && parenthesisDepth > 0) {
        --parenthesisDepth;
      } else if (check(TokenKind::LEFT_BRACE)) {
        ++braceDepth;
      } else if (check(TokenKind::RIGHT_BRACE) && braceDepth > 0) {
        --braceDepth;
      }
      advance();
      advanced = true;
    }
  }

  std::vector<Token> tokens;
  std::string_view source;
  std::vector<ParseDiagnostic> diagnostics;
  std::size_t current = 0;
  std::size_t nestingDepth = 0;
  std::size_t casePatternDepth = 0;
  // Generic parameters of the conformance block whose member is being parsed,
  // and the block's concept application, which applies to each operation.
  std::optional<std::vector<GenericParameter>> conformanceGenericParameters;
  std::optional<RequiresClause> conformanceRequirement;
  // Number of compile-time conditionals whose body is currently being parsed.
  // A conditional-continuation directive (`#elif`/`#else`/`#endif`) is a
  // recovery boundary only while one is open; nested recovery propagates it to
  // the conditional parser. A stray directive must instead be advanced past.
  std::size_t conditionalDepth = 0;
  std::size_t generatedNameCounter = 0;
  std::optional<Token> currentClassName;
  std::optional<ClassKind> currentClassKind;
  bool consumedCompletion = false;
  bool missingTokenError = false;
};

Parser::Parser(std::vector<Token> tokens, std::string_view source)
    : impl(std::make_unique<Impl>(std::move(tokens), source)) {}

Parser::~Parser() = default;
Parser::Parser(Parser &&) noexcept = default;
Parser &Parser::operator=(Parser &&) noexcept = default;

Program Parser::parse() { return impl->parse(); }

ExprPtr Parser::parseExpression() { return impl->parseExpression(); }

bool Parser::hadError() const { return impl->hadError(); }

const std::vector<ParseDiagnostic> &Parser::errors() const {
  return impl->errors();
}

} // namespace lang
