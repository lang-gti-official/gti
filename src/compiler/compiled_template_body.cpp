#include "gti/compiled_template_body.h"
#include "gti/lexer.h"
#include "gti/parser.h"
#include <algorithm>
#include <climits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace lang {
namespace {
[[noreturn]] void invalid(std::string_view message) {
  throw std::invalid_argument("compiled template: " + std::string(message));
}
constexpr std::size_t byteLimit = 1024 * 1024;
constexpr unsigned itemLimit = 65536;
struct Output {
  std::string bytes;
  void number(std::size_t value) {
    if (value > UINT32_MAX || bytes.size() > byteLimit - 4)
      invalid("field exceeds limit");
    for (unsigned i = 0; i < 4; ++i)
      bytes.push_back(static_cast<char>((value >> (8 * i)) & 255));
  }
  void text(std::string_view s) {
    number(s.size());
    if (s.size() > byteLimit - bytes.size())
      invalid("payload exceeds limit");
    bytes.append(s);
  }
};
struct MacroSource {
  const std::string &declarations;
  std::optional<std::string> spelling;
  void restore(std::size_t start, std::string_view text) {
    if (!spelling)
      spelling = declarations;
    spelling->replace(start, text.size(), text);
  }
};
struct Input {
  std::string_view bytes;
  std::string path;
  std::size_t extent = byteLimit;
  unsigned nodes = 0;
  unsigned depth = 0;
  MacroSource *macroSource = nullptr;
  unsigned number() {
    if (bytes.size() < 4)
      invalid("truncated field");
    unsigned value = 0;
    for (unsigned i = 0; i < 4; ++i)
      value |= static_cast<unsigned>(static_cast<unsigned char>(bytes[i]))
               << (8 * i);
    bytes.remove_prefix(4);
    return value;
  }
  std::string_view text() {
    auto n = number();
    if (n > byteLimit || n > bytes.size())
      invalid("invalid field length");
    auto s = bytes.substr(0, n);
    bytes.remove_prefix(n);
    return s;
  }
  unsigned count() {
    auto n = number();
    if (n > itemLimit || n > bytes.size() / 4)
      invalid("invalid collection size");
    return n;
  }
  bool flag() {
    const auto n = number();
    if (n > 1)
      invalid("invalid optional field");
    return n != 0;
  }
  struct Level {
    Input &in;
    explicit Level(Input &in) : in(in) {
      if (++in.depth > 128 || ++in.nodes > itemLimit)
        invalid("body exceeds structural limits");
    }
    ~Level() { --in.depth; }
  };
};
template <class T> struct Optional : std::false_type {};
template <class T> struct Optional<std::optional<T>> : std::true_type {
  using Element = T;
};
template <class T> struct List : std::false_type {};
template <class T> struct List<std::vector<T>> : std::true_type {
  using Element = T;
};
void writeExpr(Output &, const Expr &);
void writeStmt(Output &, const Stmt &);
ExprPtr readExpr(Input &);
StmtPtr readStmt(Input &);
template <class T> void put(Output &, const T &);
template <class T> T get(Input &);
template <class... T> void fields(Output &out, const T &...values) {
  (put(out, values), ...);
}

void putType(Output &out, const TypeRef &t) {
  fields(out, t.name, t.arguments);
  out.number(bool(t.qualifiedOwner));
  if (t.qualifiedOwner)
    putType(out, *t.qualifiedOwner);
  fields(out, t.lifetimeArguments, t.referenceLifetime, t.lifetimeBinder,
         t.functionSignatureQualifier, t.packExpansion);
  out.number(t.nativeFunctionParameterCount      ? 1
             : t.functionSignatureParameterCount ? 2
                                                 : 0);
  if (t.nativeFunctionParameterCount || t.functionSignatureParameterCount)
    out.number(t.nativeFunctionParameterCount.value_or(
        t.functionSignatureParameterCount.value_or(0)));
  out.number(t.pointers.size());
  for (const auto &p : t.pointers)
    fields(out, p.star, p.pointeeConst);
  fields(out, t.arrayExtents, t.reference, t.referenceMutability);
  out.number(static_cast<unsigned>(t.genericArgumentSyntax));
  put(out, t.genericValueExpression);
}
TypeRef getType(Input &in) {
  Input::Level level(in);
  TypeRef t(get<NamePath>(in));
  t.arguments = get<std::vector<TypeRef>>(in);
  if (in.flag())
    t.qualifiedOwner = std::make_shared<TypeRef>(getType(in));
  t.lifetimeArguments = get<std::vector<TypeRef>>(in);
  t.referenceLifetime = get<std::optional<LifetimeArgument>>(in);
  t.lifetimeBinder = get<std::optional<LifetimeBinder>>(in);
  t.functionSignatureQualifier = get<std::optional<Token>>(in);
  t.packExpansion = get<std::optional<Token>>(in);
  const auto functionKind = in.number();
  if (functionKind) {
    const auto arity = in.number();
    if (functionKind > 2 || t.arguments.empty() ||
        arity != t.arguments.size() - 1)
      invalid("invalid function type arity");
    if (functionKind == 1)
      t.nativeFunctionParameterCount = arity;
    else
      t.functionSignatureParameterCount = arity;
  }
  for (unsigned n = in.count(); n; --n)
    t.pointers.push_back({get<Token>(in), get<std::optional<Token>>(in)});
  t.arrayExtents = get<std::vector<ArrayExtentExprPtr>>(in);
  t.reference = get<std::optional<Token>>(in);
  t.referenceMutability = get<std::optional<Token>>(in);
  const auto kind = in.number();
  if (kind > static_cast<unsigned>(GenericArgumentSyntax::UnresolvedIdentifier))
    invalid("invalid type argument kind");
  t.genericArgumentSyntax = static_cast<GenericArgumentSyntax>(kind);
  t.genericValueExpression = get<ArrayExtentExprPtr>(in);
  return t;
}
Token implicitToken(TokenKind kind, std::string spelling, const Token &anchor) {
  return {kind,        std::move(spelling), {},    anchor.position,
          anchor.line, anchor.source,       false, true};
}

template <class T> void put(Output &out, const T &value) {
  if constexpr (std::is_same_v<T, Token>) {
    if (value.generated || value.completion || value.expansion ||
        value.configurationFlagReference)
      invalid("unsupported generated token");
    out.number(static_cast<unsigned>(value.kind));
    out.number(value.position);
    out.number(value.line);
    out.text(value.lexeme);
  } else if constexpr (std::is_same_v<T, NamePath>)
    put(out, value.segments);
  else if constexpr (std::is_same_v<T, LifetimeArgument>)
    fields(out, value.open, value.name, value.close);
  else if constexpr (std::is_same_v<T, LifetimeParameterSyntax>)
    fields(out, value.keyword, value.name);
  else if constexpr (std::is_same_v<T, LifetimeBinder>)
    fields(out, value.keyword, value.open, value.parameters, value.close);
  else if constexpr (std::is_same_v<T, TypeRef>)
    putType(out, value);
  else if constexpr (std::is_same_v<T, ArrayExtentExprPtr>) {
    out.number(value != nullptr);
    if (value)
      fields(out, value->token, value->left, value->right);
  } else if constexpr (std::is_same_v<T, ExprPtr> ||
                       std::is_same_v<T, StmtPtr>) {
    if (!value)
      invalid("missing required body operand");
    if constexpr (std::is_same_v<T, ExprPtr>)
      writeExpr(out, *value);
    else
      writeStmt(out, *value);
  } else if constexpr (std::is_same_v<T, LambdaCapture>) {
    fields(out, value.name, value.equal, value.borrow, value.mutKeyword);
    if (value.equal)
      put(out, value.initializer);
  } else if constexpr (std::is_same_v<T, Parameter>) {
    fields(out, value.type, value.name, value.pack);
    out.number(value.mutability == Mutability::Mutable);
    out.number(bool(value.defaultArgument));
    if (value.defaultArgument)
      fields(out, value.defaultArgument->equal,
             value.defaultArgument->expression);
  } else if constexpr (Optional<T>::value) {
    out.number(value.has_value());
    if (value)
      put(out, *value);
  } else if constexpr (List<T>::value) {
    out.number(value.size());
    for (const auto &item : value)
      put(out, item);
  } else
    static_assert(sizeof(T) == 0);
}
template <class T> T get(Input &in) {
  if constexpr (std::is_same_v<T, Token>) {
    const auto k = in.number(), pos = in.number(), line = in.number();
    auto spelling = in.text();
    if (k > static_cast<unsigned>(TokenKind::END_OF_FILE) || !line ||
        line > INT_MAX || pos > in.extent || spelling.size() > in.extent - pos)
      invalid("invalid token");
    Token token{static_cast<TokenKind>(k),
                std::string(spelling),
                {},
                pos,
                static_cast<int>(line),
                in.path};
    if (token.kind == TokenKind::INT_LITERAL ||
        token.kind == TokenKind::FLOAT_LITERAL ||
        token.kind == TokenKind::STRING_LITERAL ||
        token.kind == TokenKind::CHARACTER_LITERAL) {
      Lexer lexer;
      auto tokens = lexer.scan(token.lexeme);
      if (lexer.hadError() || tokens.size() != 2 ||
          tokens.front().kind != token.kind ||
          tokens.front().lexeme != spelling)
        invalid("invalid literal");
      token.literal = std::move(tokens.front().literal);
      token.numericSuffix = tokens.front().numericSuffix;
    }
    return token;
  } else if constexpr (std::is_same_v<T, NamePath>) {
    auto parts = get<std::vector<Token>>(in);
    if (parts.empty())
      invalid("empty name");
    return NamePath(std::move(parts));
  } else if constexpr (std::is_same_v<T, LifetimeArgument>)
    return {get<Token>(in), get<Token>(in), get<Token>(in)};
  else if constexpr (std::is_same_v<T, LifetimeParameterSyntax>)
    return {get<Token>(in), get<Token>(in)};
  else if constexpr (std::is_same_v<T, LifetimeBinder>)
    return {get<Token>(in), get<Token>(in),
            get<std::vector<LifetimeParameterSyntax>>(in), get<Token>(in)};
  else if constexpr (std::is_same_v<T, TypeRef>)
    return getType(in);
  else if constexpr (std::is_same_v<T, ArrayExtentExprPtr>) {
    if (!in.flag())
      return {};
    Input::Level level(in);
    auto tok = get<Token>(in);
    auto l = get<T>(in);
    auto r = get<T>(in);
    if (bool(l) != bool(r))
      invalid("incomplete array extent");
    return std::make_shared<ArrayExtentExpr>(std::move(l), std::move(tok),
                                             std::move(r));
  } else if constexpr (std::is_same_v<T, ExprPtr>)
    return readExpr(in);
  else if constexpr (std::is_same_v<T, StmtPtr>)
    return readStmt(in);
  else if constexpr (std::is_same_v<T, LambdaCapture>) {
    auto name = get<Token>(in);
    auto equal = get<std::optional<Token>>(in);
    auto borrow = get<std::optional<Token>>(in);
    auto mut = get<std::optional<Token>>(in);
    if ((equal && (equal->kind != TokenKind::EQUAL || borrow)) ||
        (borrow && borrow->kind != TokenKind::AMPERSAND) ||
        (mut && (!borrow || mut->kind != TokenKind::MUT)) ||
        (name.kind != TokenKind::THIS && name.kind != TokenKind::IDENTIFIER) ||
        (name.kind == TokenKind::THIS && !borrow))
      invalid("invalid lambda capture");
    ExprPtr initializer;
    if (equal)
      initializer = get<ExprPtr>(in);
    else if (name.kind == TokenKind::THIS)
      initializer = std::make_unique<This>(name);
    else
      initializer = std::make_unique<Variable>(
          implicitToken(TokenKind::IDENTIFIER, name.lexeme, name));
    return {std::move(name), std::move(equal), std::move(initializer),
            std::move(borrow), std::move(mut)};
  } else if constexpr (std::is_same_v<T, Parameter>) {
    auto type = get<TypeRef>(in);
    auto name = get<Token>(in);
    auto pack = get<std::optional<Token>>(in);
    auto mutability = in.flag() ? Mutability::Mutable : Mutability::Immutable;
    std::optional<ParameterDefault> argument;
    if (in.flag()) {
      auto equal = get<Token>(in);
      if (equal.kind != TokenKind::EQUAL)
        invalid("invalid lambda parameter default");
      argument.emplace(std::move(equal), get<ExprPtr>(in));
    }
    return {std::move(type), std::move(name), mutability, std::move(pack),
            std::move(argument)};
  } else if constexpr (Optional<T>::value) {
    if (!in.flag())
      return {};
    return get<typename Optional<T>::Element>(in);
  } else if constexpr (List<T>::value) {
    T items;
    for (unsigned n = in.count(); n; --n)
      items.push_back(get<typename List<T>::Element>(in));
    return items;
  } else
    static_assert(sizeof(T) == 0);
}

// Constructor arguments are sequenced in the same order as the corresponding
// getters. This single schema is shared by the writer and reader.
template <class Node, auto... Getter> struct Shape {
  static void write(Output &out, const Node &node) {
    fields(out, (node.*Getter)()...);
  }
  static auto read(Input &in) {
    using Parts = std::tuple<std::remove_cvref_t<
        std::invoke_result_t<decltype(Getter), const Node &>>...>;
    Parts parts{get<std::remove_cvref_t<
        std::invoke_result_t<decltype(Getter), const Node &>>>(in)...};
    return std::apply(
        [](auto &&...fields) {
          return std::make_unique<Node>(std::move(fields)...);
        },
        std::move(parts));
  }
};
void writeOrigin(Output &out, const SourceSpan *s) {
  out.number(s != nullptr);
  if (s) {
    out.number(s->start);
    out.number(s->end);
    out.number(s->line);
  }
}
std::optional<SourceSpan> readOrigin(Input &in) {
  if (!in.flag())
    return {};
  auto start = in.number(), end = in.number(), line = in.number();
  if (start > end || end > in.extent || line == 0 || line > INT_MAX)
    invalid("invalid source span");
  return SourceSpan{in.path, start, end, static_cast<int>(line)};
}
// Only contiguous parser captures can be represented by a single lexical
// interval. Preserve trivia as well as tokens: it is observable to a macro.
SyntaxCapture lexicalCapture(std::string_view text, std::size_t start,
                             int firstLine, std::string_view path) {
  Lexer lexer;
  auto tokens = lexer.scan(std::string(text), std::string(path));
  if (lexer.hadError() || tokens.size() < 3 || tokens.size() > itemLimit)
    invalid("invalid captured macro syntax");
  tokens.pop_back();
  auto balanced = balancedSyntax(tokens, 0);
  if (!balanced.complete || balanced.end + 1 != tokens.size() ||
      tokens.front().kind != TokenKind::LEFT_PAREN ||
      tokens.front().position != 0 ||
      tokens.back().position + tokens.back().lexeme.size() != text.size())
    invalid("invalid macro argument delimiters");
  auto capture = captureSyntax(tokens, text);
  capture.start = start;
  for (std::size_t i = 0; i < capture.tokens.size(); ++i) {
    auto &token = capture.tokens[i];
    if (token.line - 1 > INT_MAX - firstLine)
      invalid("invalid macro line");
    token.position += start;
    token.line += firstLine - 1;
    capture.origins[i] = tokenSpan(token);
  }
  return capture;
}
void writeMacroCapture(Output &out, const Call &call) {
  put(out, call.macroBang());
  if (!call.macroBang()) {
    if (call.capturedArguments())
      invalid("unexpected call capture");
    return;
  }
  if (!call.capturedArguments() || !call.macroArgumentErrors().empty())
    invalid("unsupported macro capture with speculative argument diagnostics");
  const auto &capture = *call.capturedArguments();
  if (capture.tokens.empty())
    invalid("empty macro capture");
  const auto &start = capture.tokens.front();
  auto expected =
      lexicalCapture(capture.text, capture.start, start.line, start.source);
  if (expected.origins != capture.origins ||
      expected.textRanges != capture.textRanges ||
      expected.tokens.size() != capture.tokens.size())
    invalid("unsupported spliced macro capture");
  for (std::size_t i = 0; i < capture.tokens.size(); ++i) {
    const auto &token = capture.tokens[i];
    if (token.kind != expected.tokens[i].kind ||
        token.lexeme != expected.tokens[i].lexeme || token.generated ||
        token.expansion || token.configurationFlagReference || token.completion)
      invalid("unsupported transformed macro capture");
  }
  out.number(capture.start);
  out.number(start.line);
  out.text(capture.text);
}
void readMacroCapture(Input &in, Call &call) {
  auto bang = get<std::optional<Token>>(in);
  if (!bang)
    return;
  const auto start = in.number(), line = in.number();
  auto text = in.text();
  if (bang->kind != TokenKind::BANG || !line || line > INT_MAX ||
      start > in.extent || text.size() > in.extent - start ||
      !call.leftParen() || call.leftParen()->position != start)
    invalid("invalid macro argument location");
  auto capture = lexicalCapture(text, start, static_cast<int>(line), in.path);
  if (capture.tokens.back().position != call.paren().position)
    invalid("invalid macro closing parenthesis");
  if (in.macroSource)
    in.macroSource->restore(start, text);
  call.setCapturedArguments(
      std::make_shared<SyntaxCapture>(std::move(capture)));
  call.setMacroBang(std::move(*bang));
}
void writeExpr(Output &out, const Expr &e) {
  if (auto *u = exprAs<Unary>(&e); u && u->rangeObject())
    invalid("unsupported range operand");
  out.number(static_cast<unsigned>(e.exprKind()));
  writeOrigin(out, e.writtenOrigin().get());
  switch (e.exprKind()) {
#define GTI_TEMPLATE_EXPR(N, ...)                                              \
  case ExprKind::N:                                                            \
    return Shape<N, __VA_ARGS__>::write(out, static_cast<const N &>(e));
#include "compiled_template_nodes.def"
#undef GTI_TEMPLATE_EXPR
  case ExprKind::Variable: {
    const auto &var = static_cast<const Variable &>(e);
    put(out, var.name());
    out.number(var.isReferencePattern());
    if (var.isReferencePattern())
      fields(out, *var.patternAuto(), *var.patternReference(),
             var.patternMutability());
    return;
  }
  case ExprKind::ConceptTest: {
    const auto &test = static_cast<const ConceptTest &>(e);
    const auto &application = test.application();
    if (application.memberRequirement || application.predicate)
      invalid("unsupported compound concept-test expression");
    fields(out, application.name, application.arguments,
           application.lifetimeArguments, test.close());
    return;
  }
  case ExprKind::MemberRequirement: {
    const auto &requirement = static_cast<const MemberRequirement &>(e);
    out.number(requirement.isExpressionRequirement());
    put(out, requirement.keyword());
    if (requirement.isExpressionRequirement()) {
      out.number(bool(requirement.iteration()));
      if (requirement.iteration())
        put(out, requirement.iteration());
      put(out, requirement.expression());
      out.number(requirement.inferredResult());
      const auto *iteration =
          stmtAs<RangeForStmt>(requirement.iteration().get());
      if (!requirement.inferredResult() &&
          !(iteration &&
            iteration->bindingType().name.last().kind == TokenKind::AUTO))
        put(out, requirement.result());
      fields(out, requirement.parameters(), requirement.bindings(),
             requirement.unsafeKeyword(), requirement.borrowedFrom());
    } else {
      fields(out, requirement.receiver(), requirement.binding(),
             requirement.receiverUse(), requirement.member(),
             requirement.result(), requirement.parameters(),
             requirement.bindings(), requirement.arguments(),
             requirement.unsafeKeyword());
    }
    return;
  }
  case ExprKind::Call: {
    const auto &call = static_cast<const Call &>(e);
    fields(out, call.callee(), call.typeArguments(), call.paren(),
           call.arguments(), call.leftParen(), call.argumentCommas(),
           call.lifetimeArguments());
    writeMacroCapture(out, call);
    return;
  }
  case ExprKind::LiteralExpr:
    return put(out, static_cast<const LiteralExpr &>(e).token());
  default:
    invalid("unsupported expression in compiled body");
  }
}
ExprPtr readExpr(Input &in) {
  Input::Level level(in);
  const auto kind = in.number();
  auto span = readOrigin(in);
  ExprPtr expr;
  switch (kind) {
#define GTI_TEMPLATE_EXPR(N, ...)                                              \
  case static_cast<unsigned>(ExprKind::N):                                     \
    expr = Shape<N, __VA_ARGS__>::read(in);                                    \
    break;
#include "compiled_template_nodes.def"
#undef GTI_TEMPLATE_EXPR
  case static_cast<unsigned>(ExprKind::Variable): {
    auto name = get<Token>(in);
    if (!in.flag()) {
      expr = std::make_unique<Variable>(std::move(name));
      break;
    }
    auto pattern = get<Token>(in);
    auto ref = get<Token>(in);
    auto mut = get<std::optional<Token>>(in);
    if (pattern.kind != TokenKind::AUTO || ref.kind != TokenKind::AMPERSAND ||
        (mut && mut->kind != TokenKind::MUT))
      invalid("invalid reference binding pattern");
    expr = std::make_unique<Variable>(std::move(name), std::move(pattern),
                                      std::move(ref), std::move(mut));
    break;
  }
  case static_cast<unsigned>(ExprKind::ConceptTest): {
    ConceptApplication application{get<NamePath>(in),
                                   get<std::vector<TypeRef>>(in)};
    application.lifetimeArguments = get<std::vector<TypeRef>>(in);
    expr =
        std::make_unique<ConceptTest>(std::move(application), get<Token>(in));
    break;
  }
  case static_cast<unsigned>(ExprKind::MemberRequirement): {
    if (!in.flag()) {
      expr = Shape<MemberRequirement, &MemberRequirement::keyword,
                   &MemberRequirement::receiver, &MemberRequirement::binding,
                   &MemberRequirement::receiverUse, &MemberRequirement::member,
                   &MemberRequirement::result, &MemberRequirement::parameters,
                   &MemberRequirement::bindings, &MemberRequirement::arguments,
                   &MemberRequirement::unsafeKeyword>::read(in);
      break;
    }
    auto keyword = get<Token>(in);
    StmtPtr iteration = in.flag() ? get<StmtPtr>(in) : nullptr;
    auto *loop = stmtAs<RangeForStmt>(iteration.get());
    if (iteration && !loop)
      invalid("invalid iteration requirement");
    auto operand = get<ExprPtr>(in);
    bool inferred = in.flag();
    TypeRef result =
        inferred ? TypeRef(implicitToken(TokenKind::AUTO, "auto", keyword))
        : (loop && loop->bindingType().name.last().kind == TokenKind::AUTO)
            ? TypeRef(implicitToken(TokenKind::VOID, "void", loop->keyword()))
            : get<TypeRef>(in);
    auto parameters = get<std::vector<TypeRef>>(in);
    auto bindings = get<std::vector<Token>>(in);
    if (parameters.size() != bindings.size())
      invalid("invalid requirement binding count");
    auto unsafe = get<std::optional<Token>>(in);
    expr = std::make_unique<MemberRequirement>(
        std::move(keyword), std::move(operand), std::move(result),
        std::move(parameters), std::move(bindings), std::move(unsafe),
        get<std::optional<Token>>(in), std::move(iteration), inferred);
    break;
  }
  case static_cast<unsigned>(ExprKind::Call): {
    auto call = Shape<Call, &Call::callee, &Call::typeArguments, &Call::paren,
                      &Call::arguments>::read(in);
    auto open = get<std::optional<Token>>(in);
    auto commas = get<std::vector<Token>>(in);
    if ((!open && !commas.empty()) || commas.size() > call->arguments().size())
      invalid("invalid call geometry");
    if (open)
      call->setArgumentGeometry(std::move(*open), std::move(commas));
    call->setLifetimeArguments(get<std::vector<TypeRef>>(in));
    readMacroCapture(in, *call);
    expr = std::move(call);
    break;
  }
  case static_cast<unsigned>(ExprKind::LiteralExpr): {
    auto tok = get<Token>(in);
    Literal value = tok.literal;
    if (tok.kind == TokenKind::TRUE)
      value = true;
    else if (tok.kind == TokenKind::FALSE)
      value = false;
    else if (tok.kind == TokenKind::NULLPTR)
      value = nullptr;
    else if (std::holds_alternative<std::monostate>(value))
      invalid("expected literal");
    expr = std::make_unique<LiteralExpr>(std::move(tok), std::move(value));
    break;
  }
  default:
    invalid("unknown expression opcode");
  }
  if (auto *init = exprAs<DirectInitializer>(expr.get());
      init && !init->designators().empty() &&
      init->designators().size() != init->arguments().size())
    invalid("invalid designated initializer");
  if (span)
    expr->setWrittenOrigin(std::move(*span));
  return expr;
}
template <class Ptr> void putMaybe(Output &out, const Ptr &p) {
  out.number(bool(p));
  if (p)
    put(out, p);
}
template <class Ptr> Ptr getMaybe(Input &in) {
  return in.flag() ? get<Ptr>(in) : nullptr;
}
void writeCondition(Output &out, const CompileCondition &condition) {
  using Kind = CompileCondition::Kind;
  out.number(static_cast<unsigned>(condition.kind));
  switch (condition.kind) {
  case Kind::TargetComparison:
    fields(out, condition.token, condition.oper, condition.valueToken);
    return;
  case Kind::Defined:
    // The loader resolves a flag at its textual position, before body restore.
    invalid("unsupported configuration flag inside compiled body");
  case Kind::Not:
    put(out, condition.token);
    writeCondition(out, *condition.left);
    return;
  case Kind::And:
  case Kind::Or:
    put(out, condition.token);
    writeCondition(out, *condition.left);
    writeCondition(out, *condition.right);
    return;
  }
}
CompileCondition readCondition(Input &in) {
  Input::Level level(in);
  using Kind = CompileCondition::Kind;
  const auto kind = in.number();
  auto token = get<Token>(in);
  switch (kind) {
  case static_cast<unsigned>(Kind::TargetComparison): {
    auto prop = parseTargetProperty(token.lexeme);
    auto oper = get<Token>(in);
    auto value = get<Token>(in);
    const auto *literal = std::get_if<std::string>(&value.literal);
    if (!prop ||
        (oper.kind != TokenKind::EQUAL_EQUAL &&
         oper.kind != TokenKind::BANG_EQUAL) ||
        value.kind != TokenKind::STRING_LITERAL || !literal)
      invalid("invalid target condition");
    std::string expected = *literal;
    return CompileCondition::targetComparison(std::move(token), *prop,
                                              std::move(oper), std::move(value),
                                              std::move(expected));
  }
  case static_cast<unsigned>(Kind::Not):
    if (token.kind != TokenKind::BANG)
      invalid("invalid target negation");
    return CompileCondition::unary(Kind::Not, std::move(token),
                                   readCondition(in));
  case static_cast<unsigned>(Kind::And):
  case static_cast<unsigned>(Kind::Or): {
    const bool conjunction = kind == static_cast<unsigned>(Kind::And);
    if (token.kind != (conjunction ? TokenKind::AND : TokenKind::OR))
      invalid("invalid target condition operator");
    auto left = readCondition(in);
    return CompileCondition::binary(conjunction ? Kind::And : Kind::Or,
                                    std::move(left), std::move(token),
                                    readCondition(in));
  }
  default:
    invalid("invalid target condition kind");
  }
}
void writeStmt(Output &out, const Stmt &s) {
  out.number(static_cast<unsigned>(s.stmtKind()));
  writeOrigin(out, s.extent() ? &*s.extent() : nullptr);
  if (auto *b = stmtAs<BlockStmt>(&s)) {
    fields(out, b->statements(), b->unsafeKeyword());
    return;
  }
  if (auto *r = stmtAs<ReturnStmt>(&s)) {
    put(out, r->keyword());
    putMaybe(out, r->value());
    return;
  }
  if (auto *e = stmtAs<ExpressionStmt>(&s)) {
    fields(out, e->expressionStart(), e->expression(), e->discardAttribute());
    return;
  }
  if (auto *v = stmtAs<VariableDecl>(&s)) {
    if (v->isStatic() || v->isThreadLocal() ||
        v->linkage() != LanguageLinkage::Gti || v->isRangeBinding() ||
        v->isRangeOwner() || v->isRangeIterator() || v->isRangeElementOwner() ||
        !v->declarationAttributes().empty())
      invalid("unsupported local declaration");
    out.number(v->isMutable());
    fields(out, v->type(), v->name());
    putMaybe(out, v->initializer());
    fields(out, v->constexprKeyword(), v->mutKeyword());
    return;
  }
  if (auto *f = stmtAs<IfStmt>(&s)) {
    fields(out, f->condition(), f->thenBranch());
    putMaybe(out, f->elseBranch());
    put(out, f->constexprKeyword());
    putMaybe(out, f->initializer());
    return;
  }
  if (auto *f = stmtAs<ForStmt>(&s)) {
    putMaybe(out, f->initializer());
    putMaybe(out, f->condition());
    putMaybe(out, f->increment());
    put(out, f->body());
    return;
  }
  if (auto *conditional = stmtAs<ConditionalStmt>(&s)) {
    put(out, conditional->directive());
    out.number(conditional->branches().size());
    for (const auto &branch : conditional->branches()) {
      out.number(branch.condition.has_value());
      if (branch.condition)
        writeCondition(out, *branch.condition);
      put(out, branch.statements);
    }
    return;
  }
  if (auto *loop = stmtAs<RangeForStmt>(&s)) {
    if (!loop->sourceBody() || !loop->ownerBinding() ||
        !loop->ownerBinding()->initializer())
      invalid("range loop lacks written operands");
    if (!loop->syntaxOrdinal())
      invalid("range loop lacks a parser identity");
    out.number(loop->syntaxOrdinal());
    put(out, loop->keyword());
    out.number(loop->bindingMutability() == Mutability::Mutable);
    fields(out, loop->bindingType(), loop->bindingName(), loop->colon(),
           loop->ownerBinding()->initializer());
    writeStmt(out, *loop->sourceBody());
    putMaybe(out, loop->initializer());
    return;
  }
  if (auto *loop = stmtAs<ConstexprForStmt>(&s)) {
    fields(out, loop->constexprKeyword(), loop->forKeyword());
    out.number(loop->bindingMutability() == Mutability::Mutable);
    fields(out, loop->bindingType(), loop->bindingName(), loop->colon(),
           loop->sequence(), loop->body());
    return;
  }
  if (auto *loop = stmtAs<WhileStmt>(&s)) {
    fields(out, loop->condition(), loop->body());
    return;
  }
  if (auto *loop = stmtAs<DoWhileStmt>(&s)) {
    fields(out, loop->body(), loop->condition());
    return;
  }
  if (auto *control = stmtAs<LoopControlStmt>(&s))
    return put(out, control->keyword());
  if (auto *empty = stmtAs<EmptyStmt>(&s))
    return put(out, empty->semicolon());
  if (auto *choice = stmtAs<SwitchStmt>(&s)) {
    fields(out, choice->keyword(), choice->expression(),
           choice->closingBrace());
    putMaybe(out, choice->initializer());
    out.number(choice->arms().size());
    for (const auto &arm : choice->arms()) {
      out.number(arm.labels.size());
      for (const auto &label : arm.labels) {
        fields(out, label.keyword, label.colon);
        putMaybe(out, label.value);
      }
      put(out, arm.statements);
    }
    return;
  }
  if (auto *assertion = stmtAs<StaticAssertDecl>(&s)) {
    fields(out, assertion->keyword(), assertion->leftParen(),
           assertion->conditionStart(), assertion->condition(),
           assertion->comma(), assertion->messageToken(),
           assertion->rightParen(), assertion->semicolon());
    return;
  }
  if (stmtAs<StructuredBindingDecl>(&s))
    invalid("unsupported structured binding in compiled body");
  if (stmtAs<CompileErrorDirective>(&s))
    invalid("unsupported #error directive in compiled body");
  invalid("unsupported declaration inside compiled body");
}
StmtPtr readStmt(Input &in) {
  Input::Level level(in);
  const auto kind = in.number();
  auto span = readOrigin(in);
  StmtPtr stmt;
  switch (kind) {
  case static_cast<unsigned>(StmtKind::ConditionalStmt): {
    auto directive = get<Token>(in);
    std::vector<ConditionalBranch> branches;
    for (unsigned n = in.count(); n; --n) {
      std::optional<CompileCondition> condition;
      if (in.flag())
        condition = readCondition(in);
      else if (branches.empty() || n != 1)
        invalid("misplaced unconditional target branch");
      branches.push_back({std::move(condition), get<StmtList>(in)});
    }
    if (branches.empty())
      invalid("empty target conditional");
    stmt = std::make_shared<ConditionalStmt>(std::move(directive),
                                             std::move(branches));
    break;
  }
  case static_cast<unsigned>(StmtKind::RangeForStmt):
  case static_cast<unsigned>(StmtKind::ConstexprForStmt): {
    const bool compileTime =
        kind == static_cast<unsigned>(StmtKind::ConstexprForStmt);
    std::optional<Token> constexprKeyword;
    unsigned ordinal = 0;
    if (compileTime)
      constexprKeyword = get<Token>(in);
    else if (!(ordinal = in.number()))
      invalid("invalid range ordinal");
    auto keyword = get<Token>(in);
    const auto mutability =
        in.flag() ? Mutability::Mutable : Mutability::Immutable;
    auto type = get<TypeRef>(in);
    auto name = get<Token>(in);
    auto colon = get<Token>(in);
    if (keyword.kind != TokenKind::FOR || colon.kind != TokenKind::COLON ||
        name.kind != TokenKind::IDENTIFIER ||
        (constexprKeyword && constexprKeyword->kind != TokenKind::CONSTEXPR))
      invalid("invalid range syntax");
    auto range = get<ExprPtr>(in);
    auto body = get<StmtPtr>(in);
    if (compileTime)
      stmt = makeParsedConstexprFor(
          std::move(*constexprKeyword), std::move(keyword), mutability,
          std::move(type), std::move(name), std::move(colon), std::move(range),
          std::move(body));
    else
      stmt = makeParsedRangeFor(ordinal, std::move(keyword), mutability,
                                std::move(type), std::move(name),
                                std::move(colon), std::move(range),
                                std::move(body), getMaybe<StmtPtr>(in));
    break;
  }
  case static_cast<unsigned>(StmtKind::WhileStmt):
    stmt = Shape<WhileStmt, &WhileStmt::condition, &WhileStmt::body>::read(in);
    break;
  case static_cast<unsigned>(StmtKind::DoWhileStmt):
    stmt =
        Shape<DoWhileStmt, &DoWhileStmt::body, &DoWhileStmt::condition>::read(
            in);
    break;
  case static_cast<unsigned>(StmtKind::LoopControlStmt): {
    auto keyword = get<Token>(in);
    if (keyword.kind != TokenKind::BREAK && keyword.kind != TokenKind::CONTINUE)
      invalid("invalid loop control");
    stmt = std::make_shared<LoopControlStmt>(std::move(keyword));
    break;
  }
  case static_cast<unsigned>(StmtKind::EmptyStmt):
    stmt = Shape<EmptyStmt, &EmptyStmt::semicolon>::read(in);
    break;
  case static_cast<unsigned>(StmtKind::SwitchStmt): {
    auto keyword = get<Token>(in);
    auto subject = get<ExprPtr>(in);
    auto close = get<Token>(in);
    auto init = getMaybe<StmtPtr>(in);
    std::vector<SwitchArm> arms;
    for (unsigned n = in.count(); n; --n) {
      SwitchArm arm;
      for (unsigned m = in.count(); m; --m) {
        auto label = get<Token>(in);
        auto colon = get<Token>(in);
        auto value = getMaybe<ExprPtr>(in);
        const TokenKind separator =
            keyword.kind != TokenKind::IF   ? TokenKind::COLON
            : label.kind == TokenKind::CASE ? TokenKind::EQUAL
                                            : TokenKind::DEFAULT;
        if (colon.kind != separator ||
            (label.kind != TokenKind::CASE &&
             label.kind != TokenKind::DEFAULT) ||
            (label.kind == TokenKind::CASE) != bool(value))
          invalid("invalid switch label");
        arm.labels.push_back(
            {std::move(label), std::move(value), std::move(colon)});
      }
      if (arm.labels.empty())
        invalid("missing switch label");
      arm.statements = get<StmtList>(in);
      arms.push_back(std::move(arm));
    }
    stmt = std::make_shared<SwitchStmt>(std::move(keyword), std::move(subject),
                                        std::move(arms), std::move(close),
                                        std::move(init));
    break;
  }
  case static_cast<unsigned>(StmtKind::StaticAssertDecl):
    stmt =
        Shape<StaticAssertDecl, &StaticAssertDecl::keyword,
              &StaticAssertDecl::leftParen, &StaticAssertDecl::conditionStart,
              &StaticAssertDecl::condition, &StaticAssertDecl::comma,
              &StaticAssertDecl::messageToken, &StaticAssertDecl::rightParen,
              &StaticAssertDecl::semicolon>::read(in);
    break;
  case static_cast<unsigned>(StmtKind::BlockStmt):
    stmt = Shape<BlockStmt, &BlockStmt::statements,
                 &BlockStmt::unsafeKeyword>::read(in);
    break;
  case static_cast<unsigned>(StmtKind::ExpressionStmt):
    stmt = Shape<ExpressionStmt, &ExpressionStmt::expressionStart,
                 &ExpressionStmt::expression,
                 &ExpressionStmt::discardAttribute>::read(in);
    break;
  case static_cast<unsigned>(StmtKind::ReturnStmt): {
    auto kw = get<Token>(in);
    stmt = std::make_shared<ReturnStmt>(std::move(kw), getMaybe<ExprPtr>(in));
    break;
  }
  case static_cast<unsigned>(StmtKind::IfStmt): {
    auto c = get<ExprPtr>(in);
    auto then = get<StmtPtr>(in);
    auto other = getMaybe<StmtPtr>(in);
    auto kw = get<std::optional<Token>>(in);
    stmt = std::make_shared<IfStmt>(std::move(c), std::move(then),
                                    std::move(other), std::move(kw),
                                    getMaybe<StmtPtr>(in));
    break;
  }
  case static_cast<unsigned>(StmtKind::ForStmt): {
    auto init = getMaybe<StmtPtr>(in);
    auto c = getMaybe<ExprPtr>(in);
    auto inc = getMaybe<ExprPtr>(in);
    stmt = std::make_shared<ForStmt>(std::move(init), std::move(c),
                                     std::move(inc), get<StmtPtr>(in));
    break;
  }
  case static_cast<unsigned>(StmtKind::VariableDecl): {
    const bool mut = in.flag();
    auto t = get<TypeRef>(in);
    auto name = get<Token>(in);
    auto init = getMaybe<ExprPtr>(in);
    auto kw = get<std::optional<Token>>(in);
    stmt = std::make_shared<VariableDecl>(
        mut ? Mutability::Mutable : Mutability::Immutable, std::move(t),
        std::move(name), std::move(init), std::nullopt, false, std::move(kw),
        false, false, get<std::optional<Token>>(in));
    break;
  }
  default:
    invalid("unknown statement opcode");
  }
  if (span)
    stmt->setExtent(std::move(*span));
  return stmt;
}
// Walk expression operands in the same schema as the codec. Lambda bodies
// have their own recipe, so only their capture/default expressions belong in
// the enclosing declaration. Any remaining interface lambdas cause rejection.
struct InterfaceLambdas {
  std::vector<Lambda *> nodes;
  void scan(const ExprPtr &expression) {
    if (!expression)
      return;
    if (auto lambda = exprAs<Lambda>(expression.get()))
      nodes.push_back(lambda);
    switch (expression->exprKind()) {
#define GTI_TEMPLATE_EXPR(N, ...)                                              \
  case ExprKind::N:                                                            \
    return scanShape<__VA_ARGS__>(static_cast<const N &>(*expression));
#include "compiled_template_nodes.def"
#undef GTI_TEMPLATE_EXPR
    case ExprKind::Call:
      return scanShape<&Call::callee, &Call::arguments>(
          static_cast<const Call &>(*expression));
    case ExprKind::MemberRequirement:
      scan(static_cast<const MemberRequirement &>(*expression).expression());
      break;
    default:
      break;
    }
  }
  template <class T> void scan(const T &) {}
  template <class T> void scan(const std::vector<T> &values) {
    for (const auto &value : values)
      scan(value);
  }
  void scan(const Parameter &parameter) {
    if (parameter.defaultArgument)
      scan(parameter.defaultArgument->expression);
  }
  void scan(const LambdaCapture &capture) { scan(capture.initializer); }
  template <auto... Field, class Node> void scanShape(const Node &node) {
    (scan((node.*Field)()), ...);
  }
};

// The parser deliberately gives both a conformance and its carrier namespace
// the same operation. Callers deduplicate those shared body pointers.
template <class Visit>
void walkDeclarations(std::span<const StmtPtr> declarations, Visit &visit,
                      InterfaceLambdas *lambdas) {
  for (const auto &decl : declarations) {
    auto block = [&](const auto &body) {
      if (body)
        visit(*body, body->extent() ? &*body->extent() : nullptr,
              body->statements());
    };
    if (auto *ns = stmtAs<NamespaceDecl>(decl.get()))
      walkDeclarations(ns->declarations(), visit, lambdas);
    else if (auto *cls = stmtAs<ClassDecl>(decl.get()))
      walkDeclarations(cls->members(), visit, lambdas);
    else if (auto *ext = stmtAs<ExternCDecl>(decl.get()))
      walkDeclarations(ext->declarations(), visit, lambdas);
    else if (auto *conf = stmtAs<ConformanceDecl>(decl.get()))
      walkDeclarations(conf->members(), visit, lambdas);
    else if (auto *group = stmtAs<MutableFieldGroupDecl>(decl.get()))
      walkDeclarations(group->members(), visit, lambdas);
    else if (auto *cfg = stmtAs<ConditionalStmt>(decl.get())) {
      for (const auto &arm : cfg->branches())
        walkDeclarations(arm.statements, visit, lambdas);
    } else if (auto *fn = stmtAs<FunctionDecl>(decl.get())) {
      block(fn->body());
      if (lambdas)
        lambdas->scan(fn->parameters());
    } else if (auto *ctor = stmtAs<ConstructorDecl>(decl.get())) {
      block(ctor->body());
      if (lambdas) {
        lambdas->scan(ctor->parameters());
        for (const auto &initializer : ctor->initializers())
          lambdas->scan(initializer.arguments);
      }
    } else if (auto *dtor = stmtAs<DestructorDecl>(decl.get()))
      block(dtor->body());
    else if (auto *var = stmtAs<VariableDecl>(decl.get())) {
      if (lambdas)
        lambdas->scan(var->initializer());
    } else if (!stmtAs<TypeAliasDecl>(decl.get()) &&
               !stmtAs<AccessSpecifierDecl>(decl.get()) &&
               !stmtAs<EnumDecl>(decl.get()) &&
               !stmtAs<ConceptDecl>(decl.get()) &&
               !stmtAs<NamespaceAliasDecl>(decl.get()) &&
               !stmtAs<StaticAssertDecl>(decl.get()) &&
               !stmtAs<CompileErrorDirective>(decl.get()) &&
               !stmtAs<EmptyStmt>(decl.get()))
      invalid("unsupported declaration in module interface");
  }
}
template <class Visit>
void forEachBody(std::span<const StmtPtr> declarations, Visit &visit) {
  for (auto &root : declarations) {
    InterfaceLambdas lambdas;
    walkDeclarations(std::span(&root, 1), visit,
                     root->interfaceLambdaCount() ? &lambdas : nullptr);
    std::set<std::size_t> locations;
    for (auto *lambda : lambdas.nodes) {
      auto span = lambda->interfaceBodySpan();
      if (!span)
        invalid("lambda lacks an interface body extent");
      locations.insert(span->start);
      visit(*lambda, span.get(), lambda->body());
    }
    if (locations.size() != root->interfaceLambdaCount())
      invalid("unsupported lambda in declaration context");
  }
}
} // namespace
CompiledTemplateModule
compileTemplateModule(std::span<const StmtPtr> declarations,
                      std::string_view source, const LifetimeNames &names) {
  if (source.size() > byteLimit)
    invalid("module exceeds limit");
  if (names.completion)
    invalid("completion token in module lifetime metadata");
  CompiledTemplateModule module{std::string(source), {}, std::nullopt, names};
  // Conformance operations appear in both the conformance and its carrier
  // namespace. Those two declarations share exactly the same parsed body.
  std::set<const void *> visited;
  auto save = [&](const auto &body, const SourceSpan *extent,
                  const StmtList &statements) {
    if (!visited.insert(&body).second)
      return;
    if (!extent)
      invalid("body lacks a parsed extent");
    const auto &span = *extent;
    if (span.end > source.size() || span.end < span.start + 2 ||
        source[span.start] != '{' || source[span.end - 1] != '}')
      invalid("invalid method extent");
    Output out;
    put(out, statements);
    auto [entry, inserted] =
        module.bodies.try_emplace(span.start, std::move(out.bytes));
    if (!inserted && entry->second != out.bytes)
      invalid("inconsistent bodies at the same location");
    for (std::size_t i = span.start + 1; i < span.end - 1; ++i)
      if (module.declarations[i] != '\n')
        module.declarations[i] = ' ';
  };
  forEachBody(declarations, save);
  return module;
}
std::optional<std::string>
restoreTemplateBodies(std::span<const StmtPtr> declarations,
                      const CompiledTemplateModule &module,
                      std::string_view path) {
  std::set<const void *> visited;
  std::set<std::size_t> locations;
  MacroSource source{module.declarations, std::nullopt};
  auto load = [&](auto &body, const SourceSpan *extent,
                  const StmtList &original) {
    if (!visited.insert(&body).second)
      return;
    if (!extent || !original.empty())
      invalid("expected declaration-only method");
    locations.insert(extent->start);
    const auto found = module.bodies.find(extent->start);
    if (found == module.bodies.end())
      invalid("missing method recipe");
    Input in{found->second, std::string(path), module.declarations.size()};
    in.macroSource = &source;
    auto statements = get<StmtList>(in);
    if (!in.bytes.empty())
      invalid("trailing method data");
    body.restoreCompiledStatements(std::move(statements));
  };
  forEachBody(declarations, load);
  if (locations.size() != module.bodies.size())
    invalid("template declaration mismatch");
  return std::move(source.spelling);
}
std::string encodeTemplateModule(const CompiledTemplateModule &module) {
  Output out{std::string(compiledTemplateMagic)};
  out.text(module.declarations);
  out.number(module.bodies.size());
  for (const auto &[position, body] : module.bodies) {
    out.number(position);
    out.text(body);
  }
  put(out, module.lifetimeNames.binders);
  out.number(module.lifetimeNames.uses.size());
  for (const auto &use : module.lifetimeNames.uses)
    fields(out, use.name, use.binder);
  put(out, module.lifetimeNames.unknown);
  return std::move(out.bytes);
}
LifetimeNames
compiledTemplateLifetimeNames(const CompiledTemplateModule &module,
                              std::string_view path) {
  auto names = module.lifetimeNames;
  for (auto &binder : names.binders)
    binder.source = path;
  for (auto &use : names.uses) {
    use.name.source = path;
    use.binder.source = path;
  }
  for (auto &unknown : names.unknown)
    unknown.source = path;
  return names;
}
CompiledTemplateModule decodeTemplateModule(std::string_view bytes) {
  if (bytes.size() > byteLimit || !bytes.starts_with(compiledTemplateMagic))
    invalid("unsupported module recipe");
  Input in{bytes.substr(compiledTemplateMagic.size()), {}};
  CompiledTemplateModule module{std::string(in.text()), {}};
  for (unsigned n = in.count(); n; --n) {
    auto pos = in.number();
    auto body = in.text();
    if (pos >= module.declarations.size() || module.declarations[pos] != '{' ||
        !module.bodies.emplace(pos, body).second)
      invalid("invalid method location");
  }
  in.extent = module.declarations.size();
  module.lifetimeNames.binders = get<std::vector<Token>>(in);
  for (unsigned n = in.count(); n; --n)
    module.lifetimeNames.uses.push_back({get<Token>(in), get<Token>(in)});
  module.lifetimeNames.unknown = get<std::vector<Token>>(in);
  if (!in.bytes.empty())
    invalid("invalid method inventory");
  return module;
}
} // namespace lang
