#pragma once

#include "gti/ast.h"
#include "gti/token.h"

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lang {

struct ParseDiagnostic {
  Token token;
  std::string message;
};

class Parser {
private:
  class ParseError {};

  enum class ItemContext {
    Declaration,
    ClassMember,
    Block,
  };

public:
  explicit Parser(std::vector<Token> tokens) : tokens(std::move(tokens)) {
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
  }

  StmtPtr declaration() {
    if (match({TokenKind::HASH_IF})) {
      return conditionalCompilation(ItemContext::Declaration);
    }
    rejectStrayConditionalDirective();
    if (match({TokenKind::AT})) {
      return runtimeBoundDeclaration();
    }
    if (match({TokenKind::NAMESPACE})) {
      return namespaceDeclaration();
    }
    if (match({TokenKind::CLASS, TokenKind::STRUCT})) {
      return classDeclaration(previous());
    }
    if (match({TokenKind::SEMICOLON})) {
      return std::make_unique<EmptyStmt>(previous());
    }
    if (isTypedDeclaration()) {
      return typedDeclaration(true);
    }

    throw error(
        peek(),
        "Expect a namespace, class, struct, function, or variable declaration.");
  }

  StmtPtr runtimeBoundDeclaration() {
    Token attribute =
        consume(TokenKind::IDENTIFIER, "Expect attribute name after '@'.");
    if (attribute.lexeme != "runtime") {
      throw error(attribute, "Unknown declaration attribute '@" +
                                 attribute.lexeme + "'.");
    }
    consume(TokenKind::LEFT_PAREN, "Expect '(' after '@runtime'.");
    Token binding = consume(TokenKind::STRING_LITERAL,
                            "Expect runtime binding name.");
    consume(TokenKind::RIGHT_PAREN, "Expect ')' after runtime binding.");
    if (!isTypedDeclaration()) {
      throw error(peek(), "Runtime binding must annotate a function.");
    }

    const auto *bindingName = std::get_if<std::string>(&binding.literal);
    return typedDeclaration(
        true, RuntimeBinding{attribute, bindingName == nullptr
                                            ? std::string{}
                                            : *bindingName});
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

  StmtPtr classDeclaration(Token keyword) {
    const ClassKind kind = keyword.kind == TokenKind::STRUCT
                               ? ClassKind::Struct
                               : ClassKind::Class;
    Token name = consume(TokenKind::IDENTIFIER, "Expect class or struct name.");
    consume(TokenKind::LEFT_BRACE, "Expect '{' before class or struct body.");

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

      consume(TokenKind::RIGHT_BRACE, "Expect '}' after class or struct body.");
      consume(TokenKind::SEMICOLON,
              "Expect ';' after class or struct declaration.");
    } catch (const ParseError &) {
      currentClassName = enclosingClassName;
      throw;
    }
    currentClassName = enclosingClassName;

    return std::make_unique<ClassDecl>(std::move(keyword), kind, name,
                                       std::move(members));
  }

  StmtPtr
  typedDeclaration(bool allowFunction,
                   std::optional<RuntimeBinding> runtimeBinding = std::nullopt,
                   bool allowMutableReceiver = false) {
    const Mutability mutability = match({TokenKind::MUT})
                                      ? Mutability::Mutable
                                      : Mutability::Immutable;
    TypeRef type = parseType();
    Token name = consume(TokenKind::IDENTIFIER, "Expect declaration name.");

    if (match({TokenKind::LEFT_PAREN})) {
      if (mutability == Mutability::Mutable) {
        throw error(name, "'mut' applies to variables, not functions.");
      }
      if (!allowFunction) {
        throw error(previous(), "Function declarations are not allowed here.");
      }
      return functionDeclaration(std::move(type), name,
                                 std::move(runtimeBinding),
                                 allowMutableReceiver);
    }

    if (runtimeBinding) {
      throw error(name, "Runtime binding must annotate a function.");
    }

    return variableDeclaration(mutability, std::move(type), name);
  }

  StmtPtr functionDeclaration(
      TypeRef returnType, Token name,
      std::optional<RuntimeBinding> runtimeBinding = std::nullopt,
      bool allowMutableReceiver = false) {
    std::vector<Parameter> parameters = parameterList();

    ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly;
    if (match({TokenKind::MUT})) {
      if (!allowMutableReceiver) {
        throw error(
            previous(),
            "Only class and struct methods can have a mutable receiver.");
      }
      receiverMutability = ReceiverMutability::Mutable;
    }

    if (match({TokenKind::SEMICOLON})) {
      return std::make_unique<FunctionDecl>(
          std::move(returnType), name, std::move(parameters), nullptr,
          std::move(runtimeBinding), receiverMutability);
    }

    consume(TokenKind::LEFT_BRACE, "Expect '{' before function body.");
    auto body = std::make_unique<BlockStmt>(blockItems());
    return std::make_unique<FunctionDecl>(
        std::move(returnType), name, std::move(parameters), std::move(body),
        std::move(runtimeBinding), receiverMutability);
  }

  StmtPtr constructorDeclaration(Token name) {
    consume(TokenKind::LEFT_PAREN, "Expect '(' after constructor name.");
    std::vector<Parameter> parameters = parameterList();
    if (match({TokenKind::MUT})) {
      throw error(previous(),
                  "Constructors do not have receiver mutability qualifiers.");
    }

    std::vector<ConstructorInitializer> initializers;
    if (match({TokenKind::COLON})) {
      do {
        Token field =
            consume(TokenKind::IDENTIFIER,
                    "Expect a field name in constructor initializer.");
        consume(TokenKind::LEFT_PAREN,
                "Expect '(' after constructor initializer field.");
        ExprPtr value = assignment();
        consume(TokenKind::RIGHT_PAREN,
                "Expect ')' after constructor initializer value.");
        initializers.push_back(
            ConstructorInitializer{std::move(field), std::move(value)});
      } while (match({TokenKind::COMMA}));
    }

    consume(TokenKind::LEFT_BRACE, "Expect '{' before constructor body.");
    auto body = std::make_unique<BlockStmt>(blockItems());
    return std::make_unique<ConstructorDecl>(
        std::move(name), std::move(parameters), std::move(initializers),
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
        Token parameterName;
        if (check(TokenKind::IDENTIFIER)) {
          parameterName = advance();
        }
        parameters.emplace_back(std::move(parameterType),
                                std::move(parameterName), mutability);
      } while (match({TokenKind::COMMA}));
    }
    consume(TokenKind::RIGHT_PAREN, "Expect ')' after parameters.");
    return parameters;
  }

  StmtPtr variableDeclaration(Mutability mutability, TypeRef type, Token name) {
    ExprPtr initializer;
    if (match({TokenKind::EQUAL})) {
      initializer = expression();
    }

    consume(TokenKind::SEMICOLON, "Expect ';' after variable declaration.");
    return std::make_unique<VariableDecl>(
        mutability, std::move(type), name, std::move(initializer));
  }

  TypeRef parseType() {
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
               TokenKind::UINT64, TokenKind::FLOAT, TokenKind::BOOL,
               TokenKind::STRING_TYPE, TokenKind::VOID})) {
      return TypeRef(previous());
    }
    if (match({TokenKind::IDENTIFIER})) {
      return TypeRef(parseNamePath(previous()));
    }
    throw error(peek(), "Expect a type name.");
  }

  NamePath parseNamePath() {
    return parseNamePath(
        consume(TokenKind::IDENTIFIER, "Expect qualified name."));
  }

  NamePath parseNamePath(Token first) {
    std::vector<Token> segments;
    segments.emplace_back(std::move(first));
    while (match({TokenKind::SCOPE})) {
      segments.emplace_back(consume(TokenKind::IDENTIFIER,
                                    "Expect name after '::'."));
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
    if (context == ItemContext::Declaration) {
      return declaration();
    }
    if (match({TokenKind::HASH_IF})) {
      return conditionalCompilation(context);
    }
    rejectStrayConditionalDirective();

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
      if (isConstructorStart()) {
        return constructorDeclaration(advance());
      }
      if (isTypedDeclaration()) {
        return typedDeclaration(true, std::nullopt, true);
      }
      throw error(peek(), "Expect a class member declaration.");
    }

    if (isTypedDeclaration()) {
      return typedDeclaration(false);
    }
    return statement();
  }

  StmtPtr conditionalCompilation(ItemContext context) {
    Token directive = previous();
    std::vector<ConditionalBranch> branches;
    branches.push_back({compileCondition(), conditionalItems(context)});

    while (match({TokenKind::HASH_ELIF})) {
      branches.push_back({compileCondition(), conditionalItems(context)});
    }
    if (match({TokenKind::HASH_ELSE})) {
      branches.push_back({std::nullopt, conditionalItems(context)});
    }

    consume(TokenKind::HASH_ENDIF,
            "Expect '#endif' after compile-time conditional.");
    return std::make_unique<ConditionalStmt>(std::move(directive),
                                             std::move(branches));
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

  CompileCondition compileCondition() {
    Token target = consume(TokenKind::IDENTIFIER,
                           "Expect 'target' after compile-time directive.");
    if (target.lexeme != "target") {
      throw error(target, "Compile-time conditions must begin with 'target'.");
    }
    consume(TokenKind::DOT, "Expect '.' after 'target'.");
    Token property = consume(
        TokenKind::IDENTIFIER,
        "Expect 'os', 'vendor', or 'arch' after 'target.'.");

    TargetProperty targetProperty;
    if (property.lexeme == "os") {
      targetProperty = TargetProperty::Os;
    } else if (property.lexeme == "vendor") {
      targetProperty = TargetProperty::Vendor;
    } else if (property.lexeme == "arch") {
      targetProperty = TargetProperty::Arch;
    } else {
      throw error(property, "Unknown target property '" + property.lexeme +
                                "'. Expected os, vendor, or arch.");
    }

    Token oper = consumeComparisonOperator();
    Token value = consume(TokenKind::STRING_LITERAL,
                          "Expect a string target value after comparison.");
    const auto *text = std::get_if<std::string>(&value.literal);
    return CompileCondition{property, targetProperty, oper, value,
                            text == nullptr ? std::string{} : *text};
  }

  Token consumeComparisonOperator() {
    if (match({TokenKind::EQUAL_EQUAL, TokenKind::BANG_EQUAL})) {
      return previous();
    }
    throw error(peek(),
                "Expect '==' or '!=' in compile-time condition.");
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
    if (match({TokenKind::LEFT_BRACE})) {
      return std::make_unique<BlockStmt>(blockItems());
    }
    if (match({TokenKind::IF})) {
      return ifStatement();
    }
    if (match({TokenKind::WHILE})) {
      return whileStatement();
    }
    if (match({TokenKind::FOR})) {
      return forStatement();
    }
    if (match({TokenKind::RETURN})) {
      return returnStatement();
    }
    if (match({TokenKind::SEMICOLON})) {
      return std::make_unique<EmptyStmt>(previous());
    }
    if (check(TokenKind::LEFT_BRACKET)) {
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
    consume(TokenKind::LEFT_PAREN, "Expect '(' after 'if'.");
    ExprPtr condition = expression();
    consume(TokenKind::RIGHT_PAREN, "Expect ')' after if condition.");
    StmtPtr thenBranch = statement();
    StmtPtr elseBranch;
    if (match({TokenKind::ELSE})) {
      elseBranch = statement();
    }
    return std::make_unique<IfStmt>(std::move(condition),
                                    std::move(thenBranch),
                                    std::move(elseBranch));
  }

  StmtPtr whileStatement() {
    consume(TokenKind::LEFT_PAREN, "Expect '(' after 'while'.");
    ExprPtr condition = expression();
    consume(TokenKind::RIGHT_PAREN, "Expect ')' after while condition.");
    return std::make_unique<WhileStmt>(std::move(condition), statement());
  }

  StmtPtr forStatement() {
    consume(TokenKind::LEFT_PAREN, "Expect '(' after 'for'.");

    StmtPtr initializer;
    if (match({TokenKind::SEMICOLON})) {
      initializer = std::make_unique<EmptyStmt>(previous());
    } else if (isTypedDeclaration()) {
      initializer = typedDeclaration(false);
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

    return std::make_unique<ForStmt>(
        std::move(initializer), std::move(condition), std::move(increment),
        statement());
  }

  StmtPtr returnStatement() {
    Token keyword = previous();
    ExprPtr value;
    if (!check(TokenKind::SEMICOLON)) {
      value = expression();
    }
    consume(TokenKind::SEMICOLON, "Expect ';' after return value.");
    return std::make_unique<ReturnStmt>(keyword, std::move(value));
  }

  StmtPtr expressionStatement(
      std::optional<Token> discardAttribute = std::nullopt) {
    ExprPtr value = expression();
    consume(TokenKind::SEMICOLON, "Expect ';' after expression.");
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
    ExprPtr expr = logicalOr();

    if (match({TokenKind::EQUAL, TokenKind::PLUS_EQUAL,
               TokenKind::MINUS_EQUAL})) {
      Token oper = previous();
      ExprPtr value = assignment();

      if (auto *variable = dynamic_cast<Variable *>(expr.get())) {
        return std::make_unique<Assign>(variable->name(), oper,
                                        std::move(value));
      }
      if (auto *get = dynamic_cast<Get *>(expr.get())) {
        return std::make_unique<Set>(get->takeObject(), get->name(), oper,
                                     std::move(value));
      }
      throw error(oper, "Invalid assignment target.");
    }

    return expr;
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
    ExprPtr expr = equality();
    while (match({TokenKind::AND})) {
      Token oper = previous();
      expr = std::make_unique<Logical>(std::move(expr), oper, equality());
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
    ExprPtr expr = term();
    while (match({TokenKind::GREATER, TokenKind::GREATER_EQUAL, TokenKind::LESS,
                  TokenKind::LESS_EQUAL})) {
      Token oper = previous();
      expr = std::make_unique<Binary>(std::move(expr), oper, term());
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
    while (match({TokenKind::SLASH, TokenKind::STAR})) {
      Token oper = previous();
      expr = std::make_unique<Binary>(std::move(expr), oper, unary());
    }
    return expr;
  }

  ExprPtr unary() {
    if (match({TokenKind::BANG, TokenKind::MINUS, TokenKind::PLUS,
               TokenKind::PLUS_PLUS, TokenKind::MINUS_MINUS})) {
      Token oper = previous();
      return std::make_unique<Unary>(oper, unary());
    }
    return postfix();
  }

  ExprPtr postfix() {
    ExprPtr expr = primary();

    while (true) {
      if (match({TokenKind::LEFT_PAREN})) {
        expr = finishCall(std::move(expr));
      } else if (match({TokenKind::DOT})) {
        Token name =
            consume(TokenKind::IDENTIFIER, "Expect property name after '.'.");
        expr = std::make_unique<Get>(std::move(expr), name);
      } else if (match({TokenKind::PLUS_PLUS, TokenKind::MINUS_MINUS})) {
        expr = std::make_unique<Postfix>(std::move(expr), previous());
      } else {
        break;
      }
    }

    return expr;
  }

  ExprPtr finishCall(ExprPtr callee) {
    ExprList arguments;
    if (!check(TokenKind::RIGHT_PAREN)) {
      do {
        arguments.emplace_back(assignment());
      } while (match({TokenKind::COMMA}));
    }

    Token paren =
        consume(TokenKind::RIGHT_PAREN, "Expect ')' after arguments.");
    return std::make_unique<Call>(std::move(callee), paren,
                                  std::move(arguments));
  }

  ExprPtr primary() {
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
               TokenKind::STRING_LITERAL})) {
      return std::make_unique<LiteralExpr>(previous(), previous().literal);
    }
    if (match({TokenKind::IDENTIFIER})) {
      Token first = previous();
      if (check(TokenKind::SCOPE)) {
        return std::make_unique<QualifiedName>(
            parseNamePath(std::move(first)));
      }
      return std::make_unique<Variable>(std::move(first));
    }
    if (match({TokenKind::SELF})) {
      return std::make_unique<Self>(previous());
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
      ExprPtr expr = expression();
      consume(TokenKind::RIGHT_PAREN, "Expect ')' after expression.");
      return std::make_unique<Grouping>(std::move(expr));
    }

    throw error(peek(), "Expect expression.");
  }

  [[nodiscard]] bool isTypedDeclaration() const {
    const std::size_t offset = check(TokenKind::MUT) ? 1 : 0;
    const TokenKind first = peekAt(offset).kind;
    if (first == TokenKind::INT || first == TokenKind::INT8 ||
        first == TokenKind::INT16 || first == TokenKind::INT32 ||
        first == TokenKind::INT64 || first == TokenKind::UINT ||
        first == TokenKind::UINT8 || first == TokenKind::UINT16 ||
        first == TokenKind::UINT32 || first == TokenKind::UINT64 ||
        first == TokenKind::FLOAT || first == TokenKind::BOOL ||
        first == TokenKind::STRING_TYPE || first == TokenKind::EXPECTED ||
        first == TokenKind::VOID) {
      return true;
    }
    if (first != TokenKind::IDENTIFIER) {
      return false;
    }

    std::size_t next = offset + 1;
    while (peekAt(next).kind == TokenKind::SCOPE &&
           peekAt(next + 1).kind == TokenKind::IDENTIFIER) {
      next += 2;
    }
    return peekAt(next).kind == TokenKind::IDENTIFIER;
  }

  [[nodiscard]] bool isConstructorStart() const {
    return currentClassName && check(TokenKind::IDENTIFIER) &&
           peek().lexeme == currentClassName->lexeme &&
           peekAt(1).kind == TokenKind::LEFT_PAREN;
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
      ++current;
    }
    return previous();
  }

  [[nodiscard]] const Token &previous() const { return tokens.at(current - 1); }

  Token consume(TokenKind kind, std::string_view message) {
    if (check(kind)) {
      return advance();
    }
    throw error(peek(), message);
  }

  ParseError error(const Token &token, std::string_view message) {
    diagnostics.push_back({token, std::string(message)});
    return ParseError{};
  }

  void synchronize(bool stopAtRightBrace, bool allowStatements,
                   bool allowClasses, bool allowAccessSpecifiers = false) {
    if (isAtEnd()) {
      return;
    }

    if (current > 0 && previous().kind == TokenKind::SEMICOLON) {
      return;
    }

    while (!isAtEnd()) {
      if (isConditionalBoundary()) {
        return;
      }
      if (stopAtRightBrace && check(TokenKind::RIGHT_BRACE)) {
        return;
      }
      if (current > 0 && previous().kind == TokenKind::SEMICOLON) {
        return;
      }
      if (isTypedDeclaration()) {
        return;
      }
      if (allowAccessSpecifiers &&
          (check(TokenKind::PUBLIC) || check(TokenKind::PRIVATE))) {
        return;
      }
      if (allowAccessSpecifiers && isConstructorStart()) {
        return;
      }

      if (allowClasses &&
          (check(TokenKind::HASH_IF) || check(TokenKind::AT) ||
           check(TokenKind::CLASS) || check(TokenKind::STRUCT) ||
           check(TokenKind::NAMESPACE))) {
        return;
      }
      if (allowStatements &&
          (check(TokenKind::HASH_IF) || check(TokenKind::LEFT_BRACKET) ||
           check(TokenKind::FOR) ||
           check(TokenKind::IF) ||
           check(TokenKind::RETURN) || check(TokenKind::WHILE))) {
        return;
      }
      advance();
    }
  }

  std::vector<Token> tokens;
  std::vector<ParseDiagnostic> diagnostics;
  std::size_t current = 0;
  std::optional<Token> currentClassName;
};

} // namespace lang
