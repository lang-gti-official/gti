#include "gti/parser.h"

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

  enum class ItemContext {
    Declaration,
    ClassMember,
    Block,
  };

public:
  explicit Impl(std::vector<Token> tokens) : tokens(std::move(tokens)) {
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
  void reset() {
    current = 0;
    diagnostics.clear();
    currentClassName.reset();
    consumedCompletion = false;
    generatedNameCounter = 0;
    missingTokenError = false;
  }

  // Both statement funnels stamp the parser-recorded full extent onto the
  // returned node so tooling can consume enclosing declaration ranges
  // without re-deriving structure from punctuation.
  StmtPtr declaration() {
    const Token start = peek();
    StmtPtr result = declarationWithoutExtent();
    stampExtent(result, start);
    return result;
  }

  StmtPtr declarationWithoutExtent() {
    if (match({TokenKind::HASH_IF, TokenKind::HASH_IFDEF,
               TokenKind::HASH_IFNDEF})) {
      return conditionalCompilation(ItemContext::Declaration);
    }
    if (match({TokenKind::HASH_ERROR})) {
      return compileErrorDirective();
    }
    rejectStrayConditionalDirective();
    if (match({TokenKind::AT})) {
      return attributedDeclaration();
    }
    if (match({TokenKind::EXTERN})) {
      return externCDeclaration(previous());
    }
    if (match({TokenKind::NAMESPACE})) {
      return namespaceDeclaration();
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
      return bracketAttributedDeclaration(LanguageLinkage::Gti);
    }
    if (match({TokenKind::CLASS, TokenKind::STRUCT, TokenKind::INTERFACE,
               TokenKind::UNION})) {
      return classDeclaration({}, previous());
    }
    if (match({TokenKind::SEMICOLON})) {
      return std::make_unique<EmptyStmt>(previous());
    }
    if (isStructuredBindingPrefix()) {
      throw error(peek(),
                  "Structured bindings are local declarations and cannot "
                  "appear at namespace scope.");
    }
    if (isTypedDeclaration()) {
      return typedDeclaration(true);
    }

    throw error(
        peek(),
        "Expect a namespace, concept, enum class, class, struct, interface, "
        "union, function, or variable declaration.");
  }

  StmtPtr attributedDeclaration() {
    Token attribute =
        consume(TokenKind::IDENTIFIER, "Expect attribute name after '@'.");
    if (attribute.lexeme != "runtime" &&
        attribute.lexeme != "compiler_constraint") {
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
          throw error(peek(), "An extern \"C\" block may contain only function "
                              "declarations.");
        } else {
          declaration = typedDeclaration(true, std::nullopt, false, false, true,
                                         LanguageLinkage::C);
        }
        const auto *function =
            dynamic_cast<const FunctionDecl *>(declaration.get());
        if (function == nullptr) {
          throw error(declarationStart,
                      "An extern \"C\" block may contain only function "
                      "declarations.");
        }
        if (function->body() || function->isPure()) {
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
    return std::make_unique<ExternCDecl>(
        std::move(keyword), std::move(language), std::move(declarations));
  }

  StmtPtr namespaceDeclaration() {
    Token name = consume(TokenKind::IDENTIFIER, "Expect namespace name.");
    if (match({TokenKind::EQUAL})) {
      NamePath target = parseNamePath();
      consume(TokenKind::SEMICOLON, "Expect ';' after namespace alias.");
      return std::make_unique<NamespaceAliasDecl>(name, std::move(target));
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
    return std::make_unique<NamespaceDecl>(name, std::move(declarations));
  }

  StmtPtr typeAliasDeclaration(Token keyword) {
    Token name =
        consume(TokenKind::IDENTIFIER, "Expect type alias name after 'using'.");
    consume(TokenKind::EQUAL, "Expect '=' after type alias name.");
    TypeRef target = parseType();
    consume(TokenKind::SEMICOLON, "Expect ';' after type alias declaration.");
    return std::make_unique<TypeAliasDecl>(std::move(keyword), std::move(name),
                                           std::move(target));
  }

  StmtPtr conceptDeclaration(
      Token keyword,
      std::optional<CompilerConstraintBinding> compilerBinding = std::nullopt) {
    Token name = consume(TokenKind::IDENTIFIER, "Expect concept name.");
    consume(TokenKind::LESS, "Expect '<' after concept name.");
    std::vector<Token> typeParameters;
    do {
      typeParameters.emplace_back(
          consume(TokenKind::IDENTIFIER,
                  "Expect a type parameter in concept declaration."));
    } while (match({TokenKind::COMMA}));
    consume(TokenKind::GREATER, "Expect '>' after concept type parameters.");

    std::vector<ConceptApplication> requirements;
    if (compilerBinding) {
      consume(TokenKind::SEMICOLON,
              "Compiler-bound concepts are declarations and must end in ';'.");
    } else {
      consume(TokenKind::EQUAL, "Expect '=' after concept parameter list.");
      do {
        requirements.emplace_back(conceptApplication());
      } while (match({TokenKind::AND}));
      if (check(TokenKind::OR)) {
        throw error(peek(),
                    "Concept definitions currently support conjunction only; "
                    "'||' and 'or' are not supported.");
      }
      consume(TokenKind::SEMICOLON, "Expect ';' after concept declaration.");
    }

    return std::make_unique<ConceptDecl>(
        std::move(keyword), std::move(name), std::move(typeParameters),
        std::move(requirements), std::move(compilerBinding));
  }

  ConceptApplication conceptApplication() {
    NamePath name = parseNamePath();
    consume(TokenKind::LESS, "Expect '<' after concept name.");
    std::vector<Token> arguments;
    do {
      arguments.emplace_back(
          consume(TokenKind::IDENTIFIER,
                  "Expect a type argument in concept application."));
    } while (match({TokenKind::COMMA}));
    consume(TokenKind::GREATER, "Expect '>' after concept arguments.");
    return {std::move(name), std::move(arguments)};
  }

  StmtPtr bracketAttributedDeclaration(LanguageLinkage linkage) {
    consume(TokenKind::LEFT_BRACKET,
            "Expect '[[' before declaration attributes.");
    consume(TokenKind::LEFT_BRACKET,
            "Expect '[[' before declaration attributes.");
    Token first =
        consume(TokenKind::IDENTIFIER, "Expect an attribute name after '[['.");
    if (match({TokenKind::LEFT_PAREN})) {
      if (first.lexeme != "c_array") {
        throw error(first, "Unknown function attribute '[[" + first.lexeme +
                               "(...)]]'.");
      }
      Token count =
          consume(TokenKind::IDENTIFIER,
                  "Expect the count out-parameter name in '[[c_array(...)]]'.");
      consume(TokenKind::RIGHT_PAREN,
              "Expect ')' after the c_array count parameter.");
      consume(TokenKind::RIGHT_BRACKET,
              "Expect ']]' after the c_array attribute.");
      consume(TokenKind::RIGHT_BRACKET,
              "Expect ']]' after the c_array attribute.");
      if (!isTypedDeclaration()) {
        throw error(peek(), "[[c_array(...)]] must annotate a function "
                            "declaration.");
      }
      return typedDeclaration(
          true, std::nullopt, false, false, true, linkage,
          NativeCArrayAttribute{.attribute = std::move(first),
                                .countParameter = std::move(count)});
    }

    std::vector<Token> attributes;
    attributes.emplace_back(std::move(first));
    while (match({TokenKind::COMMA})) {
      attributes.emplace_back(consume(TokenKind::IDENTIFIER,
                                      "Expect an attribute name after '[['."));
    }
    consume(TokenKind::RIGHT_BRACKET,
            "Expect ']]' after class or interface attributes.");
    consume(TokenKind::RIGHT_BRACKET,
            "Expect ']]' after class or interface attributes.");
    if (!match({TokenKind::CLASS, TokenKind::STRUCT, TokenKind::INTERFACE,
                TokenKind::UNION})) {
      throw error(peek(), "These attributes apply only to class, "
                          "struct, interface, or union declarations.");
    }
    return classDeclaration(std::move(attributes), previous());
  }

  StmtPtr classDeclaration(std::vector<Token> attributes, Token keyword) {
    const ClassKind kind =
        keyword.kind == TokenKind::STRUCT      ? ClassKind::Struct
        : keyword.kind == TokenKind::INTERFACE ? ClassKind::Interface
        : keyword.kind == TokenKind::UNION     ? ClassKind::Union
                                               : ClassKind::Class;
    Token name = consume(TokenKind::IDENTIFIER, "Expect type name.");
    std::vector<GenericParameter> genericParameters;
    if (check(TokenKind::LESS)) {
      genericParameters = genericParameterList();
    }
    std::vector<BaseSpecifier> bases;
    if (match({TokenKind::COLON})) {
      do {
        std::optional<Token> access;
        if (match({TokenKind::PUBLIC, TokenKind::PRIVATE})) {
          access = previous();
        }
        bases.push_back({.access = std::move(access), .type = parseBaseType()});
      } while (match({TokenKind::COMMA}));
    }
    if (match({TokenKind::SEMICOLON})) {
      return std::make_unique<ClassDecl>(
          std::move(attributes), std::move(keyword), kind, std::move(name),
          std::move(genericParameters), std::move(bases), StmtList{}, true);
    }
    consume(TokenKind::LEFT_BRACE, "Expect '{' before type body.");

    StmtList members;
    const std::optional<Token> enclosingClassName = currentClassName;
    currentClassName = name;
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
    } catch (const ParseError &) {
      currentClassName = enclosingClassName;
      throw;
    }
    currentClassName = enclosingClassName;

    return std::make_unique<ClassDecl>(
        std::move(attributes), std::move(keyword), kind, name,
        std::move(genericParameters), std::move(bases), std::move(members),
        false);
  }

  StmtPtr enumDeclaration(Token keyword) {
    Token classKeyword =
        consume(TokenKind::CLASS,
                "Scoped enums use 'enum class'; unscoped enums are not "
                "supported.");
    Token name = consume(TokenKind::IDENTIFIER, "Expect scoped enum name.");
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
    return std::make_unique<EnumDecl>(
        std::move(keyword), std::move(classKeyword), std::move(name),
        std::move(underlyingType), std::move(enumerators));
  }

  StmtPtr typedDeclaration(
      bool allowFunction,
      std::optional<RuntimeBinding> runtimeBinding = std::nullopt,
      bool allowMutableReceiver = false, bool allowOperators = false,
      bool allowStatic = true, LanguageLinkage linkage = LanguageLinkage::Gti,
      std::optional<NativeCArrayAttribute> nativeCArray = std::nullopt) {
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
    const Mutability mutability =
        match({TokenKind::MUT}) ? Mutability::Mutable : Mutability::Immutable;
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
    std::vector<GenericParameter> genericParameters;
    if (!operatorName && check(TokenKind::LESS)) {
      genericParameters = genericParameterList();
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
      return functionDeclaration(
          std::move(type), name, std::move(genericParameters),
          std::move(runtimeBinding), allowMutableReceiver, mutability,
          std::move(operatorName), std::move(staticKeyword),
          std::move(virtualKeyword), linkage, std::move(constexprKeyword),
          std::move(nativeCArray));
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
    if (virtualKeyword) {
      throw error(*virtualKeyword,
                  "Only member functions can be declared virtual.");
    }

    parseArrayDeclaratorSuffix(type);
    return variableDeclaration(mutability, std::move(type), name,
                               std::move(staticKeyword),
                               std::move(constexprKeyword));
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
      std::optional<NativeCArrayAttribute> nativeCArray = std::nullopt) {
    std::vector<Parameter> parameters = parameterList();

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
      if (!operatorName || operatorName->kind != OverloadedOperator::Call) {
        throw error(qualifier, "Trailing '&&' is currently supported only on "
                               "operator() declarations.");
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
      std::vector<ConceptApplication> requirements;
      do {
        requirements.emplace_back(conceptApplication());
      } while (match({TokenKind::AND}));
      if (check(TokenKind::OR)) {
        throw error(peek(),
                    "Requires clauses currently support conjunction only; "
                    "'||' and 'or' are not supported.");
      }
      requiresClause =
          RequiresClause{std::move(keyword), std::move(requirements)};
    }

    std::optional<Token> overrideKeyword;
    if (match({TokenKind::OVERRIDE})) {
      overrideKeyword = previous();
    }

    std::optional<PureSpecifier> pureSpecifier;
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
      return std::make_unique<FunctionDecl>(
          std::move(returnType), name, std::move(genericParameters),
          std::move(parameters), nullptr, std::move(runtimeBinding),
          receiverMutability, returnMutability, std::move(operatorName),
          std::move(staticKeyword), std::move(virtualKeyword),
          std::move(overrideKeyword), std::move(pureSpecifier), linkage,
          std::move(constexprKeyword), std::move(requiresClause),
          std::move(nativeCArray));
    }

    if (match({TokenKind::SEMICOLON})) {
      return std::make_unique<FunctionDecl>(
          std::move(returnType), name, std::move(genericParameters),
          std::move(parameters), nullptr, std::move(runtimeBinding),
          receiverMutability, returnMutability, std::move(operatorName),
          std::move(staticKeyword), std::move(virtualKeyword),
          std::move(overrideKeyword), std::nullopt, linkage,
          std::move(constexprKeyword), std::move(requiresClause),
          std::move(nativeCArray));
    }

    consume(TokenKind::LEFT_BRACE, "Expect '{' before function body.");
    auto body = std::make_unique<BlockStmt>(blockItems());
    return std::make_unique<FunctionDecl>(
        std::move(returnType), name, std::move(genericParameters),
        std::move(parameters), std::move(body), std::move(runtimeBinding),
        receiverMutability, returnMutability, std::move(operatorName),
        std::move(staticKeyword), std::move(virtualKeyword),
        std::move(overrideKeyword), std::nullopt, linkage,
        std::move(constexprKeyword), std::move(requiresClause),
        std::move(nativeCArray));
  }

  StmtPtr conversionOperatorDeclaration(
      Token keyword, std::optional<Token> virtualKeyword = std::nullopt) {
    Token boolType =
        consume(TokenKind::BOOL,
                "Only contextual 'operator bool()' conversions are supported.");
    OperatorName operatorName{OverloadedOperator::ContextualBool, keyword,
                              boolType};
    Token name = syntheticOperatorName(operatorName);
    consume(TokenKind::LEFT_PAREN, "Expect '(' after 'operator bool'.");
    return functionDeclaration(TypeRef(boolType), std::move(name), {},
                               std::nullopt, true, Mutability::Immutable,
                               std::move(operatorName), std::nullopt,
                               std::move(virtualKeyword));
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
    if (match({TokenKind::EQUAL})) {
      return {OverloadedOperator::Assignment, std::move(keyword), previous()};
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
                "Supported overloads are operator*, operator->, prefix "
                "operator++, operator=, operator[], operator(), operator==, "
                "operator!=, operator<, operator<=, operator>, operator>=, "
                "and operator bool.");
  }

  static Token syntheticOperatorName(const OperatorName &operatorName) {
    Token name = operatorName.keyword;
    name.kind = TokenKind::IDENTIFIER;
    name.lexeme = std::string(operatorFunctionName(operatorName.kind));
    return name;
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
        parameters.push_back({std::move(name), std::nullopt,
                              std::move(valueType), std::nullopt});
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
      parameters.push_back({std::move(name), std::move(pack), std::nullopt,
                            std::move(constraint)});
    } while (match({TokenKind::COMMA}));
    consume(TokenKind::GREATER, "Expect '>' after generic parameters.");
    return parameters;
  }

  StmtPtr constructorDeclaration(Token name) {
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
      return std::make_unique<ConstructorDecl>(
          std::move(name), std::move(genericParameters), std::move(parameters),
          std::move(initializers),
          SpecialMemberSpecifier{.equal = std::move(equal),
                                 .keyword = std::move(keyword),
                                 .kind = kind},
          nullptr);
    }

    consume(TokenKind::LEFT_BRACE, "Expect '{' before constructor body.");
    auto body = std::make_unique<BlockStmt>(blockItems());
    return std::make_unique<ConstructorDecl>(
        std::move(name), std::move(genericParameters), std::move(parameters),
        std::move(initializers), std::nullopt, std::move(body));
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
    consume(TokenKind::LEFT_BRACE, "Expect '{' before destructor body.");
    auto body = std::make_unique<BlockStmt>(blockItems());
    return std::make_unique<DestructorDecl>(std::move(tilde), std::move(name),
                                            std::move(body));
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

  StmtPtr
  variableDeclaration(Mutability mutability, TypeRef type, Token name,
                      std::optional<Token> staticKeyword = std::nullopt,
                      std::optional<Token> constexprKeyword = std::nullopt) {
    ExprPtr initializer;
    if (match({TokenKind::EQUAL})) {
      initializer = initializerExpression();
    } else if (match({TokenKind::LEFT_BRACE})) {
      initializer = directInitializer(previous());
    }

    consume(TokenKind::SEMICOLON, "Expect ';' after variable declaration.");
    return std::make_unique<VariableDecl>(
        mutability, std::move(type), name, std::move(initializer),
        std::move(staticKeyword), false, std::move(constexprKeyword));
  }

  ExprPtr directInitializer(Token brace) {
    ExprList arguments;
    if (!check(TokenKind::RIGHT_BRACE)) {
      while (true) {
        arguments.emplace_back(assignment());
        if (!match({TokenKind::COMMA}) || check(TokenKind::RIGHT_BRACE)) {
          break;
        }
      }
    }
    Token closingBrace =
        consume(TokenKind::RIGHT_BRACE,
                "Expect '}' after direct constructor arguments.");
    return std::make_unique<DirectInitializer>(
        std::move(brace), std::move(arguments), std::move(closingBrace));
  }

  TypeRef parseType() {
    std::optional<Token> pointeeConst;
    if (match({TokenKind::CONST})) {
      pointeeConst = previous();
    }
    TypeRef type = parseBaseType();
    if (match({TokenKind::STAR})) {
      type.pointer = previous();
      if (match({TokenKind::STAR})) {
        type.outerPointer = previous();
      }
    }
    type.pointeeConst = std::move(pointeeConst);
    if (type.pointeeConst && !type.pointer) {
      throw error(*type.pointeeConst,
                  "'const' is currently supported only as a raw-pointer "
                  "pointee qualifier, as in 'const T*'.");
    }
    parseArrayTypeSuffix(type);
    if (match({TokenKind::AMPERSAND})) {
      type.reference = previous();
    } else if (check(TokenKind::AND) && peek().lexeme == "&&") {
      type.reference = advance();
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
      consume(TokenKind::GREATER, "Expect '>' after expected error type.");
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
        do {
          arguments.emplace_back(parseGenericArgument());
        } while (match({TokenKind::COMMA}));
        consume(TokenKind::GREATER, "Expect '>' after generic arguments.");
      }
      return TypeRef(std::move(name), std::move(arguments));
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
    if (match({TokenKind::INT_LITERAL})) {
      TypeRef argument(previous());
      argument.genericArgumentSyntax = GenericArgumentSyntax::Value;
      return argument;
    }

    TypeRef argument = parseType();
    if (argument.name.segments.size() == 1 &&
        argument.name.last().kind == TokenKind::IDENTIFIER &&
        argument.arguments.empty() && argument.arrayExtents.empty() &&
        !argument.pointer && !argument.reference) {
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
    if (statement == nullptr || current == 0) {
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

    if (match({TokenKind::USING})) {
      throw error(previous(),
                  "Type aliases are currently limited to namespace scope.");
    }
    if (match({TokenKind::CONCEPT})) {
      throw error(previous(),
                  "Concept declarations are limited to namespace scope.");
    }
    if (match({TokenKind::EXTERN})) {
      throw error(previous(),
                  "extern \"C\" linkage blocks are limited to namespace "
                  "scope.");
    }

    if (context == ItemContext::ClassMember) {
      if (match({TokenKind::PUBLIC, TokenKind::PRIVATE})) {
        Token keyword = previous();
        consume(TokenKind::COLON, "Expect ':' after access specifier.");
        return std::make_unique<AccessSpecifierDecl>(
            keyword, keyword.kind == TokenKind::PUBLIC
                         ? AccessModifier::Public
                         : AccessModifier::Private);
      }
      if (match({TokenKind::SEMICOLON})) {
        return std::make_unique<EmptyStmt>(previous());
      }
      if (match({TokenKind::TILDE})) {
        return destructorDeclaration(previous());
      }
      if (match({TokenKind::OPERATOR})) {
        return conversionOperatorDeclaration(previous());
      }
      if (check(TokenKind::VIRTUAL) && peekAt(1).kind == TokenKind::OPERATOR) {
        Token virtualKeyword = advance();
        Token operatorKeyword = advance();
        return conversionOperatorDeclaration(std::move(operatorKeyword),
                                             std::move(virtualKeyword));
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
    return std::make_unique<StructuredBindingDecl>(
        std::move(autoKeyword), std::move(leftBracket), std::move(bindings),
        std::move(rightBracket), std::move(equal), std::move(initializer));
  }

  StmtPtr conditionalCompilation(ItemContext context) {
    Token directive = previous();
    std::vector<ConditionalBranch> branches;
    branches.push_back(
        {compileCondition(directive), conditionalItems(context)});

    while (match({TokenKind::HASH_ELIF})) {
      Token branchDirective = previous();
      branches.push_back(
          {compileCondition(branchDirective), conditionalItems(context)});
    }
    if (match({TokenKind::HASH_ELSE})) {
      branches.push_back({std::nullopt, conditionalItems(context)});
    }

    consume(TokenKind::HASH_ENDIF,
            "Expect '#endif' after compile-time conditional.");
    return std::make_unique<ConditionalStmt>(std::move(directive),
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
    if (match({TokenKind::UNSAFE})) {
      Token keyword = previous();
      consume(TokenKind::LEFT_BRACE, "Expect '{' after 'unsafe'.");
      return std::make_unique<BlockStmt>(blockItems(), std::move(keyword));
    }
    if (match({TokenKind::LEFT_BRACE})) {
      return std::make_unique<BlockStmt>(blockItems());
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
      return std::make_unique<EmptyStmt>(previous());
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
    return std::make_unique<IfStmt>(std::move(condition), std::move(thenBranch),
                                    std::move(elseBranch),
                                    std::move(constexprKeyword));
  }

  StmtPtr whileStatement() {
    consume(TokenKind::LEFT_PAREN, "Expect '(' after 'while'.");
    ExprPtr condition = expression();
    consume(TokenKind::RIGHT_PAREN, "Expect ')' after while condition.");
    return std::make_unique<WhileStmt>(std::move(condition), statement());
  }

  StmtPtr doWhileStatement() {
    StmtPtr body = statement();
    consume(TokenKind::WHILE, "Expect 'while' after do-while body.");
    consume(TokenKind::LEFT_PAREN, "Expect '(' after 'while'.");
    ExprPtr condition = expression();
    consume(TokenKind::RIGHT_PAREN, "Expect ')' after do-while condition.");
    consume(TokenKind::SEMICOLON, "Expect ';' after do-while statement.");
    return std::make_unique<DoWhileStmt>(std::move(body), std::move(condition));
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
      initializer = std::make_unique<EmptyStmt>(previous());
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

    return std::make_unique<ForStmt>(std::move(initializer),
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
    const std::string suffix = std::to_string(++generatedNameCounter);
    Token rangeName =
        generatedToken(TokenKind::IDENTIFIER, "__gti_range_" + suffix, colon);
    Token iteratorName = generatedToken(TokenKind::IDENTIFIER,
                                        "__gti_iterator_" + suffix, colon);
    Token sentinelName = generatedToken(TokenKind::IDENTIFIER,
                                        "__gti_sentinel_" + suffix, colon);

    TypeRef rangeType(generatedToken(TokenKind::AUTO, "auto", colon));
    rangeType.reference = generatedToken(TokenKind::AMPERSAND, "&", colon);
    const Mutability rangeMutability =
        bindingMutability == Mutability::Mutable && bindingType.reference
            ? Mutability::Mutable
            : Mutability::Immutable;

    TypeRef iteratorType(generatedToken(TokenKind::AUTO, "auto", colon));
    TypeRef sentinelType(generatedToken(TokenKind::AUTO, "auto", colon));

    StmtList loopBody;
    loopBody.emplace_back(std::make_unique<VariableDecl>(
        bindingMutability, bindingType, bindingName,
        std::make_unique<Unary>(generatedToken(TokenKind::STAR, "*", colon),
                                generatedVariable(iteratorName)),
        std::nullopt, true));
    loopBody.emplace_back(std::move(body));

    StmtPtr coreLoop = std::make_unique<ForStmt>(
        std::make_unique<EmptyStmt>(
            generatedToken(TokenKind::SEMICOLON, ";", colon)),
        std::make_unique<Binary>(
            generatedVariable(iteratorName),
            generatedToken(TokenKind::BANG_EQUAL, "!=", colon),
            generatedVariable(sentinelName)),
        std::make_unique<Unary>(
            generatedToken(TokenKind::PLUS_PLUS, "++", colon),
            generatedVariable(iteratorName)),
        std::make_unique<BlockStmt>(std::move(loopBody)));

    StmtList lowered;
    lowered.emplace_back(std::make_unique<VariableDecl>(
        rangeMutability, std::move(rangeType), rangeName, std::move(range)));
    lowered.emplace_back(std::make_unique<VariableDecl>(
        Mutability::Mutable, std::move(iteratorType), iteratorName,
        generatedMemberCall(rangeName, "begin", colon)));
    lowered.emplace_back(std::make_unique<VariableDecl>(
        Mutability::Immutable, std::move(sentinelType), sentinelName,
        generatedMemberCall(rangeName, "end", colon)));
    lowered.emplace_back(std::move(coreLoop));

    return std::make_unique<RangeForStmt>(
        std::move(keyword), bindingMutability, std::move(bindingType),
        std::move(bindingName), std::move(colon),
        std::make_unique<BlockStmt>(std::move(lowered)));
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
    return std::make_unique<SwitchStmt>(std::move(keyword), std::move(value),
                                        std::move(arms));
  }

  StmtPtr returnStatement() {
    Token keyword = previous();
    ExprPtr value;
    if (!check(TokenKind::SEMICOLON)) {
      value = initializerExpression();
    }
    consume(TokenKind::SEMICOLON, "Expect ';' after return value.");
    return std::make_unique<ReturnStmt>(keyword, std::move(value));
  }

  StmtPtr loopControlStatement() {
    Token keyword = previous();
    const std::string message = "Expect ';' after '" + keyword.lexeme + "'.";
    consume(TokenKind::SEMICOLON, message);
    return std::make_unique<LoopControlStmt>(std::move(keyword));
  }

  StmtPtr
  expressionStatement(std::optional<Token> discardAttribute = std::nullopt) {
    const bool hadCompletion = consumedCompletion;
    ExprPtr value = expression();
    if (check(TokenKind::SEMICOLON)) {
      advance();
    } else if (hadCompletion == consumedCompletion) {
      consume(TokenKind::SEMICOLON, "Expect ';' after expression.");
    }
    return std::make_unique<ExpressionStmt>(std::move(value),
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
    ExprPtr expr = conditional();

    if (match({TokenKind::EQUAL, TokenKind::PLUS_EQUAL, TokenKind::MINUS_EQUAL,
               TokenKind::STAR_EQUAL, TokenKind::SLASH_EQUAL,
               TokenKind::PERCENT_EQUAL, TokenKind::AMPERSAND_EQUAL,
               TokenKind::PIPE_EQUAL, TokenKind::CARET_EQUAL,
               TokenKind::SHIFT_LEFT_EQUAL, TokenKind::SHIFT_RIGHT_EQUAL})) {
      Token oper = previous();
      ExprPtr value =
          check(TokenKind::LEFT_BRACE) ? arrayInitializer() : assignment();

      if (auto *variable = dynamic_cast<Variable *>(expr.get())) {
        return std::make_unique<Assign>(variable->name(), oper,
                                        std::move(value));
      }
      if (auto *qualified = dynamic_cast<QualifiedName *>(expr.get())) {
        return std::make_unique<Assign>(qualified->takeName(), oper,
                                        std::move(value));
      }
      if (auto *get = dynamic_cast<Get *>(expr.get())) {
        return std::make_unique<Set>(get->takeObject(), get->access(),
                                     get->name(), oper, std::move(value));
      }
      if (auto *index = dynamic_cast<Index *>(expr.get())) {
        return std::make_unique<IndexSet>(index->takeObject(), index->bracket(),
                                          index->takeIndex(), oper,
                                          std::move(value));
      }
      if (auto *unary = dynamic_cast<Unary *>(expr.get());
          unary != nullptr && unary->oper().kind == TokenKind::STAR) {
        return std::make_unique<DereferenceSet>(
            unary->oper(), unary->takeRight(), oper, std::move(value));
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
               TokenKind::AMPERSAND, TokenKind::TILDE})) {
      Token oper = previous();
      return std::make_unique<Unary>(oper, unary());
    }
    return postfix();
  }

  ExprPtr postfix() {
    ExprPtr expr = primary();

    while (true) {
      if (isExplicitGenericCallStart()) {
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
        ExprPtr argument =
            check(TokenKind::LEFT_BRACE) ? arrayInitializer() : assignment();
        if (match({TokenKind::ELLIPSIS})) {
          const auto *variable = dynamic_cast<const Variable *>(argument.get());
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
    do {
      arguments.emplace_back(parseGenericArgument());
    } while (match({TokenKind::COMMA}));
    consume(TokenKind::GREATER, "Expect '>' after generic arguments.");
    return arguments;
  }

  ExprPtr primary() {
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
    if (isNumericTypeToken(peek().kind) &&
        peekAt(1).kind == TokenKind::LEFT_PAREN) {
      TypeRef targetType = parseType();
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
          if (dynamic_cast<const Call *>(expr.get()) == nullptr) {
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
        if (check(TokenKind::AMPERSAND)) {
          throw error(peek(),
                      "Lambda reference captures are not supported; captures "
                      "are immutable value snapshots.");
        }
        Token name =
            consume(TokenKind::IDENTIFIER,
                    "Expect a local binding name in lambda capture list.");
        std::optional<Token> equal;
        ExprPtr initializer;
        if (match({TokenKind::EQUAL})) {
          equal = previous();
          initializer = assignment();
        } else {
          Token source =
              generatedToken(TokenKind::IDENTIFIER, name.lexeme, name);
          initializer = generatedVariable(source);
        }
        captures.emplace_back(std::move(name), std::move(equal),
                              std::move(initializer));
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
    std::size_t offset = check(TokenKind::VIRTUAL) ? 1 : 0;
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

  [[nodiscard]]
  std::optional<std::size_t> genericArgumentEnd(std::size_t offset) const {
    if (peekAt(offset).kind == TokenKind::INT_LITERAL) {
      return offset + 1;
    }
    return typeEnd(offset);
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
      return arrayTypeEnd(*errorEnd + 1);
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
      return arrayTypeEnd(offset + 1);
    }
    if (first != TokenKind::IDENTIFIER) {
      return std::nullopt;
    }

    std::size_t next = offset + 1;
    while (peekAt(next).kind == TokenKind::SCOPE &&
           isIdentifierUseToken(peekAt(next + 1).kind)) {
      next += 2;
    }
    if (peekAt(next).kind != TokenKind::LESS) {
      return arrayTypeEnd(next);
    }

    do {
      const std::optional<std::size_t> argumentEnd =
          genericArgumentEnd(next + 1);
      if (!argumentEnd) {
        return std::nullopt;
      }
      next = *argumentEnd;
    } while (peekAt(next).kind == TokenKind::COMMA);
    return peekAt(next).kind == TokenKind::GREATER ? arrayTypeEnd(next + 1)
                                                   : std::nullopt;
  }

  [[nodiscard]] std::optional<std::size_t>
  arrayTypeEnd(std::size_t offset) const {
    if (peekAt(offset).kind == TokenKind::STAR) {
      ++offset;
      if (peekAt(offset).kind == TokenKind::STAR) {
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

  [[nodiscard]] bool isConstructorStart() const {
    if (!currentClassName || !check(TokenKind::IDENTIFIER) ||
        peek().lexeme != currentClassName->lexeme) {
      return false;
    }
    if (peekAt(1).kind == TokenKind::LEFT_PAREN) {
      return true;
    }
    if (peekAt(1).kind != TokenKind::LESS) {
      return false;
    }
    std::size_t offset = 2;
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

  ParseError error(const Token &token, std::string_view message) {
    missingTokenError = false;
    if (reservedIdentifierError(token)) {
      return ParseError{};
    }
    diagnostics.push_back(makeDiagnostic("GTI-P0001", DiagnosticPhase::Parsing,
                                         token, std::string(message)));
    return ParseError{};
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
        if (isConditionalBoundary()) {
          return;
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
            (check(TokenKind::PUBLIC) || check(TokenKind::PRIVATE))) {
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

        if (allowClasses &&
            (check(TokenKind::HASH_IF) || check(TokenKind::HASH_IFDEF) ||
             check(TokenKind::HASH_IFNDEF) || check(TokenKind::HASH_ERROR) ||
             check(TokenKind::AT) || check(TokenKind::CLASS) ||
             check(TokenKind::CONCEPT) || check(TokenKind::ENUM) ||
             check(TokenKind::EXTERN) || check(TokenKind::STRUCT) ||
             check(TokenKind::INTERFACE) || check(TokenKind::UNION) ||
             check(TokenKind::NAMESPACE) || check(TokenKind::USING))) {
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
  std::vector<ParseDiagnostic> diagnostics;
  std::size_t current = 0;
  std::size_t generatedNameCounter = 0;
  std::optional<Token> currentClassName;
  bool consumedCompletion = false;
  bool missingTokenError = false;
};

Parser::Parser(std::vector<Token> tokens)
    : impl(std::make_unique<Impl>(std::move(tokens))) {}

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
