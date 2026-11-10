#include "gti/ast_clone.h"
#include "gti/ast_printer.h"
#include "gti/lexer.h"
#include "gti/parser.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

const lang::FunctionDecl *findFunction(const lang::Program &program,
                                       std::string_view name) {
  for (const auto &declaration : program.declarations()) {
    const auto *function = lang::stmtAs<lang::FunctionDecl>(declaration.get());
    if (function && function->name().lexeme == name) {
      return function;
    }
  }
  return nullptr;
}

void expectLifetime(const lang::TypeRef &type, std::string_view name,
                    std::string_view source, std::size_t start) {
  expect(type.reference && type.referenceLifetime,
         "annotated references must retain their lifetime syntax");
  const auto &lifetime = *type.referenceLifetime;
  expect(lifetime.open.kind == lang::TokenKind::LESS &&
             lifetime.open.lexeme == "<" && lifetime.open.position == start &&
             lifetime.name.lexeme == name &&
             lifetime.name.position == start + 1 &&
             lifetime.close.kind == lang::TokenKind::GREATER &&
             lifetime.close.lexeme == ">" &&
             lifetime.close.position == start + name.size() + 1 &&
             lifetime.open.source == "lifetime-syntax.gti" &&
             lifetime.name.source == lifetime.open.source &&
             lifetime.close.source == lifetime.open.source &&
             source.substr(start, name.size() + 2) ==
                 "<" + std::string(name) + ">",
         "reference lifetime tokens must preserve exact source spans");
}

void testDeclarationSyntax() {
  const std::string source = R"(
int&<a> identity<lifetime a>(int&<a> value) { return value; }
mut int&<a> edit<lifetime a>(mut int&<a> value) { return value; }
struct Borrowed<T, lifetime a> { T&<a> value; };
using Alias<T, lifetime a> = Borrowed<T, a>;
concept Alive<T, lifetime a> = outlives<T, a>;
int&<shorter> shorten<lifetime longer, lifetime shorter>(int&<longer> value)
    requires outlives<longer, shorter> and outlives<int, static> {
  return value;
}
using Nested = Borrowed<Borrowed<int, a>, b>&<shell>;
using Signature = int&<a>(mut int&<a>);
using Inferred = Borrowed<int, _>;
using Static = Borrowed<int, static>;
)";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "lifetime-syntax.gti"));
  const auto program = parser.parse();
  expect(!lexer.hadError() && !parser.hadError() &&
             program.declarations().size() == 10,
         "lifetime binders and annotated reference/type forms must parse");

  const auto *identity = findFunction(program, "identity");
  expect(identity && identity->sourceGenericParameters().size() == 1 &&
             identity->genericParameters().empty(),
         "a function must retain its lifetime binder");
  const auto &parameter = identity->sourceGenericParameters().front();
  expect(parameter.lifetimeKeyword && !parameter.valueType &&
             !parameter.constraint && parameter.name.lexeme == "a" &&
             parameter.lifetimeKeyword->kind == lang::TokenKind::IDENTIFIER &&
             parameter.lifetimeKeyword->lexeme == "lifetime" &&
             parameter.lifetimeKeyword->position == source.find("lifetime a") &&
             parameter.name.position == source.find("lifetime a") + 9,
         "lifetime binders need contextual keyword and exact name tokens");
  expectLifetime(identity->returnType(), "a", source, source.find("<a>"));
  expectLifetime(identity->parameters().front().type, "a", source,
                 source.find("int&<a> value") + 4);

  const auto *edit = findFunction(program, "edit");
  expect(edit && edit->returnMutability() == lang::Mutability::Mutable &&
             edit->parameters().front().mutability == lang::Mutability::Mutable,
         "lifetime annotations must preserve mutable reference access");
  const auto *record =
      lang::stmtAs<lang::ClassDecl>(program.declarations()[2].get());
  expect(record && record->sourceGenericParameters().size() == 2 &&
             !record->sourceGenericParameters()[0].lifetimeKeyword &&
             record->sourceGenericParameters()[1].lifetimeKeyword &&
             record->genericParameters().size() == 1 &&
             record->lifetimeParameters().size() == 1,
         "ordinary and lifetime record parameters must remain distinct");
  const auto *field =
      lang::stmtAs<lang::VariableDecl>(record->members().front().get());
  expect(field && field->type().referenceLifetime &&
             field->type().referenceLifetime->name.lexeme == "a",
         "a stored reference must retain its declared lifetime");

  const auto *alias =
      lang::stmtAs<lang::TypeAliasDecl>(program.declarations()[3].get());
  expect(alias && alias->sourceGenericParameters()[1].lifetimeKeyword &&
             alias->target().arguments.size() == 1 &&
             alias->target().lifetimeArguments.size() == 1 &&
             alias->target().lifetimeArguments[0].genericArgumentSyntax ==
                 lang::GenericArgumentSyntax::Lifetime &&
             alias->target().lifetimeArguments[0].name.last().lexeme == "a",
         "an in-scope lifetime binder used as an argument is lifetime syntax, "
         "kept out of the type's argument list (RFC 0004 parametricity)");
  const auto *conceptDeclaration =
      lang::stmtAs<lang::ConceptDecl>(program.declarations()[4].get());
  expect(conceptDeclaration &&
             conceptDeclaration->sourceGenericParameters()[1].lifetimeKeyword &&
             conceptDeclaration->typeParameters().size() == 1 &&
             conceptDeclaration->typeParameters()[0].lexeme == "T",
         "concepts must retain lifetime binders");
  const auto *shorten = findFunction(program, "shorten");
  expect(
      shorten && shorten->requiresClause() &&
          shorten->requiresClause()->requirements().empty() &&
          shorten->requiresClause()->expression.lifetimeRequirements.size() ==
              2 &&
          shorten->requiresClause()
                  ->expression.lifetimeRequirements[0]
                  .name.last()
                  .lexeme == "outlives" &&
          shorten->requiresClause()
                  ->expression.lifetimeRequirements[1]
                  .arguments[1]
                  .genericArgumentSyntax ==
              lang::GenericArgumentSyntax::Lifetime,
      "outlives is a lifetime requirement, lifted out of type-level "
      "satisfaction with its argument kinds preserved");

  const auto *nested =
      lang::stmtAs<lang::TypeAliasDecl>(program.declarations()[6].get());
  expect(nested && nested->target().arguments.front().arguments.size() == 2 &&
             nested->target().arguments[1].name.last().lexeme == "b",
         "generic lifetime arguments must remain attached to their own type");
  expectLifetime(nested->target(), "shell", source, source.find("<shell>"));
  const auto *signature =
      lang::stmtAs<lang::TypeAliasDecl>(program.declarations()[7].get());
  expect(signature && signature->target().isFunctionSignature() &&
             signature->target().arguments[0].referenceMutability &&
             signature->target().arguments[0].referenceLifetime &&
             signature->target().arguments[1].referenceLifetime,
         "function-signature input and output regions must remain independent "
         "syntax");
  for (const std::size_t index : {8U, 9U}) {
    const auto *argumentAlias =
        lang::stmtAs<lang::TypeAliasDecl>(program.declarations()[index].get());
    expect(argumentAlias && argumentAlias->target().arguments.size() == 1 &&
               argumentAlias->target().lifetimeArguments.size() == 1 &&
               argumentAlias->target()
                       .lifetimeArguments[0]
                       .genericArgumentSyntax ==
                   lang::GenericArgumentSyntax::Lifetime,
           "placeholder and static arguments must be classified as lifetime "
           "syntax");
  }
}

void testContextualKeywordAndLookahead() {
  const std::string source = R"(
struct Context<lifetime> { lifetime value; };
struct Packed<lifetime...> {};
struct Constrained<lifetime::Bound T> {};
int lifetime(int value) { return value; }
int main() {
  int lifetime = 1;
  int&<_> first = lifetime;
  int&<static> second = global;
  const int*&<_> pointer = address;
  Borrowed<int, static> static_view = global;
  Borrowed<int, _> inferred_view = global;
  int result = identity<static>(second);
  return result;
}
)";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "lifetime-lookahead.gti"));
  const auto program = parser.parse();
  expect(!lexer.hadError() && !parser.hadError() &&
             program.declarations().size() == 5,
         "contextual lifetime syntax must preserve ordinary identifiers and "
         "declaration lookahead");
  const auto *context =
      lang::stmtAs<lang::ClassDecl>(program.declarations()[0].get());
  expect(context && !context->genericParameters()[0].lifetimeKeyword &&
             context->genericParameters()[0].name.lexeme == "lifetime",
         "a sole generic type parameter named lifetime must stay a type "
         "parameter");
  const auto *function = findFunction(program, "main");
  expect(function && function->body() &&
             function->body()->statements().size() == 8,
         "annotated local declarations and explicit calls must retain their "
         "statements");
  const auto *binding =
      lang::stmtAs<lang::VariableDecl>(function->body()->statements()[6].get());
  const auto *call =
      binding ? lang::exprAs<lang::Call>(binding->initializer().get())
              : nullptr;
  expect(
      call && call->typeArguments().empty() &&
          call->lifetimeArguments().size() == 1 &&
          call->lifetimeArguments()[0].genericArgumentSyntax ==
              lang::GenericArgumentSyntax::Lifetime,
      "explicit generic-call lookahead must admit static lifetime arguments");
}

void testOperatorLifetimeBinders() {
  const std::string source = R"(
struct Pick {
  int&<a> operator()<lifetime a>(int&<a> left, int& right) { return left; }
};
)";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "operator-lifetimes.gti"));
  const auto program = parser.parse();
  expect(!lexer.hadError() && !parser.hadError(),
         "an operator overload may bind lifetime parameters");
  const auto *owner =
      lang::stmtAs<lang::ClassDecl>(program.declarations()[0].get());
  const lang::FunctionDecl *call = nullptr;
  if (owner)
    for (const auto &member : owner->members())
      if (const auto *function = lang::stmtAs<lang::FunctionDecl>(member.get()))
        call = function;
  expect(call && call->lifetimeParameters().size() == 1 &&
             call->genericParameters().empty(),
         "an operator's lifetime binders are lifetime parameters only");

  lang::Lexer typedLexer;
  lang::Parser typed(typedLexer.scan(
      "struct Bad { int& operator()<lifetime a, T>(int& x) { return x; } };",
      "operator-type-binder.gti"));
  (void)typed.parse();
  expect(typed.hadError(), "an operator overload cannot bind a type");
}

void testRequirementResultRegions() {
  const std::string source = R"(
concept readable<C, T> = requires(C&<owner> owner) { { *owner } -> T&<owner>; };
)";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "requirement-regions.gti"));
  (void)parser.parse();
  expect(!lexer.hadError() && !parser.hadError(),
         "a requirement result may name a region its parameter binds");

  lang::Lexer unboundLexer;
  lang::Parser unbound(unboundLexer.scan(
      "concept bad<C, T> = requires(C& owner) { { *owner } -> T&<other>; };",
      "requirement-unbound-region.gti"));
  (void)unbound.parse();
  expect(unbound.hadError(),
         "a requirement result region must be bound by a parameter");
}

void testQuantifiedAndReceiverSyntax() {
  const std::string source = R"(
using Callback = for<lifetime a, lifetime b>
    mut int&<a>(mut int&<a>, int&<b>);
using Nested = CallbackBox<for<lifetime a> int&<a>(int&<a>)>;
using Mutable = Box<mut int&<a>, this>;
struct Example<lifetime a> {
  int value;
  int&<this> get() { return this.value; }
  Borrowed<int, this> retain<lifetime b>(int&<b> other)
      requires outlives<this, b>;
};
for<T> concept Alive<T> {};
int main() {
  CallbackBox<for<lifetime a> int&<a>(int&<a>)> bound = candidate;
  for<lifetime a> int&<a>(int&<a>) raw = candidate;
  for (int index = 0; index < 1; index = index + 1) {}
  auto result = use<for<lifetime a> int&<a>(int&<a>)>(candidate);
  auto size = sizeof(CallbackBox<for<lifetime a> int&<a>(int&<a>)>);
  return 0;
}
)";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "quantified-lifetime.gti"));
  const auto program = parser.parse();
  if (parser.hadError()) {
    for (const auto &error : parser.errors()) {
      std::cerr << error.message << " at " << error.primary.start << '\n';
    }
  }
  expect(!lexer.hadError() && !parser.hadError() &&
             program.declarations().size() == 6,
         "quantified function types and receiver lifetime syntax must parse");
  const auto *callback =
      lang::stmtAs<lang::TypeAliasDecl>(program.declarations()[0].get());
  expect(callback && callback->target().isFunctionSignature() &&
             callback->target().lifetimeBinder &&
             callback->target().lifetimeBinder->parameters.size() == 2 &&
             callback->target().arguments.back().referenceMutability &&
             !callback->target().arguments.back().lifetimeBinder,
         "higher-ranked binders must scope over the signature, including its "
         "mutable result");
  const auto &binder = *callback->target().lifetimeBinder;
  expect(binder.keyword.kind == lang::TokenKind::FOR &&
             binder.keyword.position == source.find("for<") &&
             binder.open.position == binder.keyword.position + 3 &&
             binder.parameters[0].keyword.position ==
                 source.find("lifetime a") &&
             binder.parameters[0].name.lexeme == "a" &&
             binder.parameters[1].name.lexeme == "b" &&
             binder.close.position == source.find('>'),
         "higher-ranked binder delimiters and declaration tokens must retain "
         "spans");
  const auto *mutableAlias =
      lang::stmtAs<lang::TypeAliasDecl>(program.declarations()[2].get());
  expect(
      mutableAlias && mutableAlias->target().arguments[0].referenceMutability &&
          mutableAlias->target().lifetimeArguments.size() == 1 &&
          mutableAlias->target().lifetimeArguments[0].genericArgumentSyntax ==
              lang::GenericArgumentSyntax::Lifetime &&
          mutableAlias->target().lifetimeArguments[0].name.last().kind ==
              lang::TokenKind::THIS,
      "generic arguments must preserve mutable references and receiver "
      "lifetimes");
  const auto *record =
      lang::stmtAs<lang::ClassDecl>(program.declarations()[3].get());
  expect(record && record->members().size() == 3,
         "receiver lifetime fixture must retain its members");
  const auto *get =
      lang::stmtAs<lang::FunctionDecl>(record->members()[1].get());
  const auto *retain =
      lang::stmtAs<lang::FunctionDecl>(record->members()[2].get());
  expect(get && get->returnType().referenceLifetime &&
             get->returnType().referenceLifetime->name.kind ==
                 lang::TokenKind::THIS &&
             retain && retain->requiresClause() &&
             retain->requiresClause()->requirements().empty() &&
             retain->requiresClause()->expression.lifetimeRequirements.size() ==
                 1 &&
             retain->requiresClause()
                     ->expression.lifetimeRequirements[0]
                     .arguments[0]
                     .genericArgumentSyntax ==
                 lang::GenericArgumentSyntax::Lifetime,
         "receiver regions must survive both result and requirement syntax");
  const auto *function = findFunction(program, "main");
  expect(function && function->body() &&
             function->body()->statements().size() == 6,
         "quantified type lookahead must coexist with ordinary for statements");
  const auto &declaration = *function->body()->statements()[0];
  const auto cloned = lang::cloneStmt(declaration, 91);
  const auto *variable =
      lang::stmtAs<lang::VariableDecl>(cloned.statement.get());
  expect(variable && variable->type().arguments[0].lifetimeBinder,
         "nested quantified types must clone");
  const auto &clonedBinder = *variable->type().arguments[0].lifetimeBinder;
  expect(clonedBinder.keyword.expansion == 91 &&
             clonedBinder.open.expansion == 91 &&
             clonedBinder.close.expansion == 91 &&
             clonedBinder.parameters[0].keyword.expansion == 91 &&
             clonedBinder.parameters[0].name.expansion == 91,
         "cloning must remap every quantified binder token");
  const auto *size =
      lang::stmtAs<lang::VariableDecl>(function->body()->statements()[4].get());
  lang::AstPrinter printer;
  expect(size && size->initializer() &&
             printer.print(*size->initializer())
                     .find("CallbackBox<for<lifetime a> int&<a>(int&<a>)>") !=
                 std::string::npos,
         "AST printing must retain quantified function-type syntax");
}

void testCloneAndPrinter() {
  const std::string source = R"(
int main() {
  Box<int&<a>, static>&<b> reference = value;
  auto size = sizeof(Box<int&<a>, static>&<b>);
}
)";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "lifetime-clone.gti"));
  const auto program = parser.parse();
  expect(!parser.hadError(), "lifetime cloning fixture must parse");
  const auto *function = findFunction(program, "main");
  expect(function && function->body(), "cloning fixture must retain its body");
  const auto &declaration = *function->body()->statements().front();
  lang::AstCloneOptions options;
  options.token = [](const lang::Token &token) {
    lang::Token copy = token;
    copy.source = "expanded-lifetime.gti";
    copy.position += 100;
    return copy;
  };
  const auto cloned = lang::cloneStmt(declaration, 37, options);
  const auto *variable =
      lang::stmtAs<lang::VariableDecl>(cloned.statement.get());
  const auto *original = lang::stmtAs<lang::VariableDecl>(&declaration);
  expect(variable && original, "body-level declarations must clone");
  for (const auto *type :
       {&variable->type(), &variable->type().arguments.front()}) {
    expect(type->referenceLifetime.has_value(),
           "cloned reference must preserve its region");
    const auto &region = *type->referenceLifetime;
    for (const auto *token : {&region.open, &region.name, &region.close}) {
      expect(token->source == "expanded-lifetime.gti" &&
                 token->position >= 100 && token->expansion == 37,
             "cloning must remap all lifetime tokens, including delimiters");
    }
  }
  expect(original->type().referenceLifetime->name.source ==
                 "lifetime-clone.gti" &&
             original->type().referenceLifetime->name.expansion == 0,
         "cloning must preserve the immutable original lifetime syntax");
  const auto *size =
      lang::stmtAs<lang::VariableDecl>(function->body()->statements()[1].get());
  lang::AstPrinter printer;
  expect(
      size && size->initializer() &&
          printer.print(*size->initializer()).find("Box<int&<a>,static>&<b>") !=
              std::string::npos,
      "AST printing must retain nested referent and reference lifetime "
      "spellings");
}

void testCompletionMarkers() {
  for (const bool generic : {false, true}) {
    for (const std::string &prefix : {std::string("sou"), std::string()}) {
      const std::string type =
          generic ? "Box<int, " + prefix + ">" : "int&<" + prefix + ">";
      const std::string source =
          "void check<lifetime sourceRegion>(int&<sourceRegion> value) { " +
          type + " local = value; }";
      const auto marker = source.find(type) + type.size() - 1;
      lang::Lexer lexer;
      lang::Parser parser(
          lexer.scanForCompletion(source, marker, "lifetime-completion.gti"));
      const auto program = parser.parse();
      const auto *function = findFunction(program, "check");
      expect(
          !lexer.hadError() && !parser.hadError() && function &&
              function->body() && function->body()->statements().size() == 1,
          "lifetime completion markers must preserve their local declaration");
      const auto *local = lang::stmtAs<lang::VariableDecl>(
          function->body()->statements().front().get());
      expect(local, "lifetime completion must retain its typed variable");
      const lang::Token &name = generic ? local->type().arguments[1].name.last()
                                        : local->type().referenceLifetime->name;
      expect(name.completion && name.lexeme == prefix &&
                 name.position == marker - prefix.size(),
             "lifetime completion names must retain marker and prefix spans");
    }
  }
}

void testMalformedLifetimeRecovery() {
  struct Fixture {
    const char *source;
    const char *token;
    const char *message;
  };
  for (const Fixture fixture : {
           Fixture{"using Bad = int&<>;", ">", "Expect a lifetime name"},
           Fixture{"using Bad = int&<int>;", "int>", "Expect a lifetime name"},
           Fixture{"using Bad = int&<a, b>;", ",",
                   "after the reference lifetime"},
           Fixture{"using Bad = int&<a::b>;",
                   "::", "after the reference lifetime"},
           Fixture{"using Bad = int&<a + 1>;", "+",
                   "after the reference lifetime"},
           Fixture{"using Bad = int&<a;", ";", "after the reference lifetime"},
           Fixture{"using Bad = int&&<a>;", "<", "'&&' move form"},
           Fixture{"struct Bad<lifetime _> {};", "_", "requires a name"},
           Fixture{"struct Bad<lifetime static> {};", "static",
                   "lifetime parameter name"},
           Fixture{"using Bad = for<T> int(int);", "T>",
                   "only 'lifetime name' parameters"},
           Fixture{"using Bad = for<lifetime a> int;", "for",
                   "requires a function signature type"},
           Fixture{"using Bad = for<> int(int);", ">",
                   "only 'lifetime name' parameters"},
           Fixture{"using Bad = for<lifetime _> int(int);", "_",
                   "requires a name"},
           Fixture{"using Bad = for<lifetime a = static> int(int);", "= static",
                   "after higher-ranked lifetime parameters"},
           Fixture{"using Bad = Box<mut int>;", "mut",
                   "must be an '&' reference"},
       }) {
    const std::string source =
        std::string(fixture.source) + "\nint recovered() { return 0; }\n";
    lang::Lexer lexer;
    lang::Parser parser(lexer.scan(source, "invalid-lifetime.gti"));
    const auto program = parser.parse();
    expect(!lexer.hadError() && parser.hadError() && !parser.errors().empty(),
           "malformed lifetime syntax must have an owning parser diagnostic");
    const auto &diagnostic = parser.errors().front();
    const std::string_view token = fixture.token;
    const std::size_t tokenLength = token == "int>" ? 3
                                    : token == "T>" || token == "= static"
                                        ? 1
                                        : token.size();
    expect(diagnostic.code == "GTI-P0001" &&
               diagnostic.phase == lang::DiagnosticPhase::Parsing &&
               diagnostic.primary.source == "invalid-lifetime.gti" &&
               diagnostic.primary.start == source.find(fixture.token) &&
               diagnostic.primary.end ==
                   diagnostic.primary.start + tokenLength &&
               diagnostic.message.find(fixture.message) != std::string::npos,
           "malformed lifetime diagnostics must identify the offending source "
           "token");
    expect(findFunction(program, "recovered") != nullptr,
           "malformed lifetimes must not discard the next valid declaration");
  }
}

void testProtocolOperationBinders() {
  const std::string source = R"(
concept P<T> { T&<a> borrow<lifetime a>(T&<a> value); };
for<T> concept P<Wrap<T>> {
  Wrap<T>&<a> borrow<lifetime a>(Wrap<T>&<a> value) { return value; }
  Wrap<T>&<b> shorten<lifetime a, lifetime b>(Wrap<T>&<a> value)
      requires outlives<a, b> { return value; }
};
)";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "lifetime-operation.gti"));
  const auto program = parser.parse();
  if (parser.hadError())
    for (const auto &error : parser.errors())
      std::cerr << error.message << '\n';
  expect(!lexer.hadError() && !parser.hadError(),
         "protocol operations may declare lifetime-only local binders");
  const auto findBlock =
      [](auto &&self,
         const lang::StmtList &declarations) -> const lang::ConformanceDecl * {
    for (const auto &declaration : declarations) {
      if (const auto *block =
              lang::stmtAs<lang::ConformanceDecl>(declaration.get()))
        return block;
      if (const auto *scope =
              lang::stmtAs<lang::NamespaceDecl>(declaration.get()))
        if (const auto *block = self(self, scope->declarations()))
          return block;
    }
    return nullptr;
  };
  const lang::ConformanceDecl *block =
      findBlock(findBlock, program.declarations());
  expect(block && block->members().size() == 2,
         "conformance lifetime operation remains a source member");
  if (!block || block->members().empty())
    return;
  const auto *operation =
      lang::stmtAs<lang::FunctionDecl>(block->members()[0].get());
  expect(
      operation && operation->sourceGenericParameters().size() == 2 &&
          operation->sourceGenericParameters()[0].name.lexeme == "T" &&
          !operation->sourceGenericParameters()[0].lifetimeKeyword &&
          operation->sourceGenericParameters()[1].lifetimeKeyword &&
          operation->sourceGenericParameters()[1].name.lexeme == "a" &&
          operation->genericParameters().size() == 1,
      "the operation keeps block parameters before its local lifetime binders");
  const auto *shorten =
      block->members().size() == 2
          ? lang::stmtAs<lang::FunctionDecl>(block->members()[1].get())
          : nullptr;
  expect(
      shorten && shorten->requiresClause() &&
          shorten->requiresClause()->requirements().size() == 1 &&
          shorten->requiresClause()->expression.lifetimeRequirements.size() ==
              1 &&
          !shorten->requiresClause()->keyword.generated,
      "a local lifetime requirement conjoins with the block's inherited "
      "application");
}

void testPatternedNominalLifetimes() {
  const std::string source = R"(
class Callback<R&<result>(Args...), lifetime environment> {
  int&<environment> value;
};
)";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "pattern-lifetimes.gti"));
  const auto program = parser.parse();
  expect(!lexer.hadError() && !parser.hadError() &&
             program.declarations().size() == 1,
         "patterned primaries accept trailing nominal lifetime parameters");
  const auto *owner =
      lang::stmtAs<lang::ClassDecl>(program.declarations().front().get());
  expect(
      owner && owner->genericArgumentPatterns().size() == 1 &&
          owner->sourceGenericParameters().size() == 1 &&
          owner->lifetimeParameters().size() == 1 &&
          owner->genericParameters().empty(),
      "public type patterns and nominal lifetime declarations remain distinct");
  const auto &parameter = owner->sourceGenericParameters().front();
  expect(parameter.lifetimeKeyword && parameter.name.lexeme == "environment" &&
             parameter.name.position == source.find("environment") &&
             parameter.lifetimeKeyword->position == source.find("lifetime") &&
             owner->genericArgumentPatterns()
                     .front()
                     .arguments.back()
                     .referenceLifetime->name.lexeme == "result",
         "projected and explicit nominal lifetime tokens retain their own "
         "source identity");
  for (const std::string invalid :
       {"class Bad<R(Args...), lifetime a, T> {};",
        "class Bad<R(Args...), lifetime _> {};",
        "class Bad<R(Args...), lifetime a = static> {};"}) {
    lang::Lexer invalidLexer;
    lang::Parser invalidParser(
        invalidLexer.scan(invalid, "invalid-pattern-lifetime.gti"));
    (void)invalidParser.parse();
    expect(invalidParser.hadError(),
           "pattern lifetime suffix rejects non-lifetime, anonymous and "
           "defaulted formals");
  }
}

void testConstructorInitializerPackArguments() {
  const std::string source = R"(
struct Derived<Args...> : public Base {
  int[2] values;
  Derived(Args... pack) : Base(7, pack...), values({1, 2}) {}
};
)";
  lang::Lexer lexer;
  lang::Parser parser(lexer.scan(source, "initializer-pack.gti"));
  const auto program = parser.parse();
  expect(
      !lexer.hadError() && !parser.hadError() &&
          program.declarations().size() == 1,
      "constructor initializers share ordinary call pack and brace arguments");
  const auto *owner =
      lang::stmtAs<lang::ClassDecl>(program.declarations().front().get());
  const auto *constructor =
      owner && owner->members().size() == 2
          ? lang::stmtAs<lang::ConstructorDecl>(owner->members()[1].get())
          : nullptr;
  expect(
      constructor && constructor->initializers().size() == 2 &&
          constructor->initializers()[0].arguments.size() == 2 &&
          constructor->initializers()[1].arguments.size() == 1,
      "initializer argument parsing preserves separate base and field lists");
  const auto *pack = lang::exprAs<lang::PackExpansion>(
      constructor->initializers()[0].arguments[1].get());
  expect(
      pack && pack->name().lexeme == "pack" &&
          pack->name().position == source.find("pack...") &&
          pack->ellipsis().position == source.find("pack...") + 4 &&
          lang::exprAs<lang::ArrayInitializer>(
              constructor->initializers()[1].arguments[0].get()),
      "initializer pack tokens and brace expression retain exact AST identity");
  for (const auto &[argument, message] :
       {std::pair{"pack..., 7", "must be the final call argument"},
        std::pair{"(pack)...", "Only a named function parameter pack"}}) {
    const std::string invalid =
        "struct Derived<Args...> { Derived(Args... pack) : Base(" +
        std::string(argument) + ") {} };";
    lang::Lexer invalidLexer;
    lang::Parser invalidParser(
        invalidLexer.scan(invalid, "invalid-initializer-pack.gti"));
    (void)invalidParser.parse();
    expect(invalidParser.hadError() &&
               std::any_of(
                   invalidParser.errors().begin(), invalidParser.errors().end(),
                   [&](const auto &error) {
                     return error.message.find(message) != std::string::npos;
                   }),
           "initializer packs enforce the same named-final-argument "
           "restrictions as calls");
  }
}

} // namespace

int main() {
  testOperatorLifetimeBinders();
  testRequirementResultRegions();
  testDeclarationSyntax();
  testContextualKeywordAndLookahead();
  testCloneAndPrinter();
  testQuantifiedAndReceiverSyntax();
  testCompletionMarkers();
  testMalformedLifetimeRecovery();
  testProtocolOperationBinders();
  testPatternedNominalLifetimes();
  testConstructorInitializerPackArguments();
}
