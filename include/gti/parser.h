#pragma once

#include "gti/ast.h"
#include "gti/diagnostic.h"
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

using ParseDiagnostic = Diagnostic;

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
    consumedCompletion = false;
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
    if (match({TokenKind::USING})) {
      return typeAliasDeclaration(previous());
    }
    if (match({TokenKind::ENUM})) {
      return enumDeclaration(previous());
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
        "Expect a namespace, enum class, class, struct, function, or variable "
        "declaration.");
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

  StmtPtr typeAliasDeclaration(Token keyword) {
    Token name =
        consume(TokenKind::IDENTIFIER, "Expect type alias name after 'using'.");
    consume(TokenKind::EQUAL, "Expect '=' after type alias name.");
    TypeRef target = parseType();
    consume(TokenKind::SEMICOLON, "Expect ';' after type alias declaration.");
    return std::make_unique<TypeAliasDecl>(std::move(keyword), std::move(name),
                                           std::move(target));
  }

  StmtPtr classDeclaration(Token keyword) {
    const ClassKind kind = keyword.kind == TokenKind::STRUCT
                               ? ClassKind::Struct
                               : ClassKind::Class;
    Token name = consume(TokenKind::IDENTIFIER, "Expect class or struct name.");
    std::vector<GenericParameter> genericParameters;
    if (check(TokenKind::LESS)) {
      genericParameters = genericParameterList();
    }
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
                                       std::move(genericParameters),
                                       std::move(members));
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
        ExprPtr initializer;
        if (match({TokenKind::EQUAL})) {
          initializer = assignment();
        }
        enumerators.push_back(
            {std::move(enumerator), std::move(initializer)});
      } while (match({TokenKind::COMMA}) &&
               !check(TokenKind::RIGHT_BRACE));
    }

    consume(TokenKind::RIGHT_BRACE, "Expect '}' after scoped enum body.");
    consume(TokenKind::SEMICOLON, "Expect ';' after scoped enum declaration.");
    return std::make_unique<EnumDecl>(
        std::move(keyword), std::move(classKeyword), std::move(name),
        std::move(underlyingType), std::move(enumerators));
  }

  StmtPtr
  typedDeclaration(bool allowFunction,
                   std::optional<RuntimeBinding> runtimeBinding = std::nullopt,
                   bool allowMutableReceiver = false,
                   bool allowOperators = false, bool allowStatic = true) {
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
    const Mutability mutability = match({TokenKind::MUT})
                                      ? Mutability::Mutable
                                      : Mutability::Immutable;
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
        if (!allowMutableReceiver) {
          throw error(name,
                      "Mutable reference returns are currently limited to "
                      "class and struct methods.");
        }
        if (staticKeyword) {
          throw error(name,
                      "Static functions cannot return a mutable reference in "
                      "the current lifetime model.");
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
          std::move(operatorName), std::move(staticKeyword));
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

    parseArrayDeclaratorSuffix(type);
    return variableDeclaration(mutability, std::move(type), name,
                               std::move(staticKeyword));
  }

  StmtPtr functionDeclaration(
      TypeRef returnType, Token name,
      std::vector<GenericParameter> genericParameters,
      std::optional<RuntimeBinding> runtimeBinding = std::nullopt,
      bool allowMutableReceiver = false,
      Mutability returnMutability = Mutability::Immutable,
      std::optional<OperatorName> operatorName = std::nullopt,
      std::optional<Token> staticKeyword = std::nullopt) {
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
    }

    if (match({TokenKind::SEMICOLON})) {
      return std::make_unique<FunctionDecl>(
          std::move(returnType), name, std::move(genericParameters),
          std::move(parameters), nullptr, std::move(runtimeBinding),
          receiverMutability, returnMutability, std::move(operatorName),
          std::move(staticKeyword));
    }

    consume(TokenKind::LEFT_BRACE, "Expect '{' before function body.");
    auto body = std::make_unique<BlockStmt>(blockItems());
    return std::make_unique<FunctionDecl>(
        std::move(returnType), name, std::move(genericParameters),
        std::move(parameters), std::move(body), std::move(runtimeBinding),
        receiverMutability, returnMutability, std::move(operatorName),
        std::move(staticKeyword));
  }

  StmtPtr conversionOperatorDeclaration(Token keyword) {
    Token boolType =
        consume(TokenKind::BOOL,
                "Only contextual 'operator bool()' conversions are supported.");
    OperatorName operatorName{OverloadedOperator::ContextualBool, keyword,
                              boolType};
    Token name = syntheticOperatorName(operatorName);
    consume(TokenKind::LEFT_PAREN, "Expect '(' after 'operator bool'.");
    return functionDeclaration(TypeRef(boolType), std::move(name), {},
                               std::nullopt, true, Mutability::Immutable,
                               std::move(operatorName));
  }

  OperatorName overloadedOperatorName(Token keyword) {
    if (match({TokenKind::STAR})) {
      return {OverloadedOperator::Dereference, std::move(keyword), previous()};
    }
    if (match({TokenKind::ARROW})) {
      return {OverloadedOperator::Arrow, std::move(keyword), previous()};
    }
    if (match({TokenKind::EQUAL_EQUAL})) {
      return {OverloadedOperator::Equal, std::move(keyword), previous()};
    }
    if (match({TokenKind::BANG_EQUAL})) {
      return {OverloadedOperator::NotEqual, std::move(keyword), previous()};
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
                "Supported overloads are operator*, operator->, operator[], "
                "operator(), operator==, operator!=, and operator bool.");
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
        Token name = consume(
            TokenKind::IDENTIFIER,
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
        ExprPtr value =
            check(TokenKind::LEFT_BRACE) ? arrayInitializer() : assignment();
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

  StmtPtr destructorDeclaration(Token tilde) {
    Token name = consume(TokenKind::IDENTIFIER,
                         "Expect class or struct name after '~'.");
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
        parameters.emplace_back(std::move(parameterType),
                                std::move(parameterName), mutability,
                                std::move(pack));
      } while (match({TokenKind::COMMA}));
    }
    consume(TokenKind::RIGHT_PAREN, "Expect ')' after parameters.");
    return parameters;
  }

  StmtPtr
  variableDeclaration(Mutability mutability, TypeRef type, Token name,
                      std::optional<Token> staticKeyword = std::nullopt) {
    ExprPtr initializer;
    if (match({TokenKind::EQUAL})) {
      initializer = initializerExpression();
    } else if (match({TokenKind::LEFT_BRACE})) {
      initializer = directInitializer(previous());
    }

    consume(TokenKind::SEMICOLON, "Expect ';' after variable declaration.");
    return std::make_unique<VariableDecl>(mutability, std::move(type), name,
                                          std::move(initializer),
                                          std::move(staticKeyword));
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
    TypeRef type = parseBaseType();
    parseArrayTypeSuffix(type);
    if (match({TokenKind::AMPERSAND})) {
      type.reference = previous();
    }
    return type;
  }

  TypeRef parseBaseType() {
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
               TokenKind::CHAR, TokenKind::NULLPTR_TYPE, TokenKind::VOID,
               TokenKind::AUTO})) {
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
        !argument.reference) {
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

    if (match({TokenKind::USING})) {
      throw error(previous(),
                  "Type aliases are currently limited to namespace scope.");
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
      if (isConstructorStart()) {
        return constructorDeclaration(advance());
      }
      if (isTypedDeclaration()) {
        return typedDeclaration(true, std::nullopt, true, true);
      }
      throw error(peek(), "Expect a class member declaration.");
    }

    if (isTypedDeclaration()) {
      return typedDeclaration(false, std::nullopt, false, false, false);
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

    return std::make_unique<ForStmt>(
        std::move(initializer), std::move(condition), std::move(increment),
        statement());
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

  StmtPtr expressionStatement(
      std::optional<Token> discardAttribute = std::nullopt) {
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
    ExprPtr expr = logicalOr();

    if (match({TokenKind::EQUAL, TokenKind::PLUS_EQUAL,
               TokenKind::MINUS_EQUAL})) {
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
               TokenKind::TILDE})) {
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
    ExprList arguments;
    if (!check(TokenKind::RIGHT_PAREN)) {
      do {
        ExprPtr argument = assignment();
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
      } while (match({TokenKind::COMMA}));
    }

    Token paren =
        consume(TokenKind::RIGHT_PAREN, "Expect ')' after arguments.");
    return std::make_unique<Call>(std::move(callee), std::move(typeArguments),
                                  paren, std::move(arguments));
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
      if (check(TokenKind::SCOPE)) {
        return std::make_unique<QualifiedName>(
            parseNamePath(std::move(first)));
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
      ExprPtr expr = expression();
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
        if (check(TokenKind::EQUAL)) {
          throw error(peek(),
                      "Lambda init captures are not supported; capture an "
                      "existing local binding by name.");
        }
        captures.push_back({std::move(name)});
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

  [[nodiscard]] bool isTypedDeclaration() const {
    std::size_t offset = check(TokenKind::STATIC) ? 1 : 0;
    if (peekAt(offset).kind == TokenKind::MUT) {
      ++offset;
    }
    const TokenKind first = peekAt(offset).kind;
    if (first != TokenKind::AUTO && first != TokenKind::INT &&
        first != TokenKind::INT8 && first != TokenKind::INT16 &&
        first != TokenKind::INT32 && first != TokenKind::INT64 &&
        first != TokenKind::UINT && first != TokenKind::UINT8 &&
        first != TokenKind::UINT16 && first != TokenKind::UINT32 &&
        first != TokenKind::UINT64 && first != TokenKind::FLOAT &&
        first != TokenKind::BOOL && first != TokenKind::CHAR &&
        first != TokenKind::EXPECTED && first != TokenKind::NULLPTR_TYPE &&
        first != TokenKind::VOID && first != TokenKind::IDENTIFIER) {
      return false;
    }
    const std::optional<std::size_t> end = typeEnd(offset);
    return end && (peekAt(*end).kind == TokenKind::IDENTIFIER ||
                   peekAt(*end).kind == TokenKind::OPERATOR);
  }

  [[nodiscard]] static bool isNumericTypeToken(TokenKind kind) {
    return kind == TokenKind::INT || kind == TokenKind::INT8 ||
           kind == TokenKind::INT16 || kind == TokenKind::INT32 ||
           kind == TokenKind::INT64 || kind == TokenKind::UINT ||
           kind == TokenKind::UINT8 || kind == TokenKind::UINT16 ||
           kind == TokenKind::UINT32 || kind == TokenKind::UINT64 ||
           kind == TokenKind::FLOAT;
  }

  [[nodiscard]] bool isValueGenericParameterStart() const {
    return (isNumericTypeToken(peek().kind) ||
            peek().kind == TokenKind::BOOL) &&
           peekAt(1).kind == TokenKind::IDENTIFIER;
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
        first == TokenKind::BOOL || first == TokenKind::CHAR ||
        first == TokenKind::NULLPTR_TYPE || first == TokenKind::VOID) {
      return arrayTypeEnd(offset + 1);
    }
    if (first != TokenKind::IDENTIFIER) {
      return std::nullopt;
    }

    std::size_t next = offset + 1;
    while (peekAt(next).kind == TokenKind::SCOPE &&
           peekAt(next + 1).kind == TokenKind::IDENTIFIER) {
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
    while (peekAt(offset).kind == TokenKind::LEFT_BRACKET) {
      if ((peekAt(offset + 1).kind != TokenKind::INT_LITERAL &&
           peekAt(offset + 1).kind != TokenKind::IDENTIFIER) ||
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
    return currentClassName && check(TokenKind::IDENTIFIER) &&
           peek().lexeme == currentClassName->lexeme &&
           peekAt(1).kind == TokenKind::LEFT_PAREN;
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
    diagnostics.push_back(makeDiagnostic("GTI-P0001", DiagnosticPhase::Parsing,
                                         token, std::string(message)));
    return ParseError{};
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
        if (stopAtTypedDeclaration && isTypedDeclaration()) {
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
            (check(TokenKind::HASH_IF) || check(TokenKind::AT) ||
             check(TokenKind::CLASS) || check(TokenKind::ENUM) ||
             check(TokenKind::STRUCT) ||
             check(TokenKind::NAMESPACE) || check(TokenKind::USING))) {
          return;
        }
        if (allowStatements &&
            (check(TokenKind::HASH_IF) || check(TokenKind::LEFT_BRACKET) ||
             check(TokenKind::BREAK) || check(TokenKind::CONTINUE) ||
             check(TokenKind::FOR) || check(TokenKind::IF) ||
             check(TokenKind::RETURN) || check(TokenKind::SWITCH) ||
             check(TokenKind::WHILE))) {
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
  std::optional<Token> currentClassName;
  bool consumedCompletion = false;
};

} // namespace lang
