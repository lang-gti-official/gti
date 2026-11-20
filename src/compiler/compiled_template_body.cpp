#include "gti/compiled_template_body.h"
#include "gti/lexer.h"
#include <algorithm>
#include <limits>
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
struct Input {
  std::string_view bytes;
  std::string path;
  std::size_t extent = byteLimit;
  unsigned nodes = 0;
  unsigned depth = 0;
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
  if (t.qualifiedOwner || t.referenceLifetime || !t.lifetimeArguments.empty() ||
      t.nativeFunctionParameterCount || t.functionSignatureParameterCount ||
      t.lifetimeBinder || t.functionSignatureQualifier || t.packExpansion)
    invalid("unsupported dependent type form");
  fields(out, t.name, t.arguments);
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
  } else if constexpr (std::is_same_v<T, TypeRef>)
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
  else if constexpr (Optional<T>::value) {
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
void writeExpr(Output &out, const Expr &e) {
  if (auto *c = exprAs<Call>(&e);
      c && (c->macroBang() || c->capturedArguments() ||
            !c->lifetimeArguments().empty()))
    invalid("unsupported macro or lifetime call");
  if (auto *v = exprAs<Variable>(&e); v && v->isReferencePattern())
    invalid("unsupported reference pattern");
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
  case ExprKind::LiteralExpr:
    return put(out, static_cast<const LiteralExpr &>(e).token());
  default:
    invalid("unsupported expression in generic method");
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
  invalid("unsupported statement in generic method");
}
StmtPtr readStmt(Input &in) {
  Input::Level level(in);
  const auto kind = in.number();
  auto span = readOrigin(in);
  StmtPtr stmt;
  switch (kind) {
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
template <class Visit>
void forEachMethod(std::span<const StmtPtr> declarations, Visit &visit,
                   bool &generic) {
  for (const auto &s : declarations) {
    if (auto *ns = stmtAs<NamespaceDecl>(s.get()))
      forEachMethod(ns->declarations(), visit, generic);
    else if (auto *c = stmtAs<ClassDecl>(s.get())) {
      generic |=
          c->exportKeyword().has_value() && !c->genericParameters().empty();
      forEachMethod(c->members(), visit, generic);
    } else if (auto *f = stmtAs<FunctionDecl>(s.get()); f && f->body())
      visit(*f->body());
    else if (auto *c = stmtAs<ConstructorDecl>(s.get()); c && c->body())
      visit(*c->body());
    else if (auto *d = stmtAs<DestructorDecl>(s.get()); d && d->body())
      visit(*d->body());
    else if (!stmtAs<TypeAliasDecl>(s.get()) &&
             !stmtAs<VariableDecl>(s.get()) &&
             !stmtAs<AccessSpecifierDecl>(s.get()))
      invalid("unsupported declaration in template module");
  }
}
} // namespace
CompiledTemplateModule
compileTemplateModule(std::span<const StmtPtr> declarations,
                      std::string_view source) {
  if (source.size() > byteLimit)
    invalid("module exceeds limit");
  CompiledTemplateModule module{std::string(source), {}};
  auto save = [&](const BlockStmt &body) {
    if (!body.extent())
      invalid("method body lacks a parsed extent");
    const auto &span = *body.extent();
    if (span.end > source.size() || span.end < span.start + 2 ||
        source[span.start] != '{' || source[span.end - 1] != '}')
      invalid("invalid method extent");
    Output out;
    put(out, body.statements());
    if (!module.bodies.emplace(span.start, std::move(out.bytes)).second)
      invalid("duplicate method");
    for (std::size_t i = span.start + 1; i < span.end - 1; ++i)
      if (module.declarations[i] != '\n')
        module.declarations[i] = ' ';
  };
  bool generic = false;
  forEachMethod(declarations, save, generic);
  if (!generic || module.bodies.empty())
    invalid("expected an exported class template with method bodies");
  return module;
}
void restoreTemplateBodies(std::span<const StmtPtr> declarations,
                           const CompiledTemplateModule &module,
                           std::string_view path) {
  std::size_t restored = 0;
  auto load = [&](BlockStmt &body) {
    if (!body.extent() || !body.statements().empty())
      invalid("expected declaration-only method");
    const auto found = module.bodies.find(body.extent()->start);
    if (found == module.bodies.end())
      invalid("missing method recipe");
    Input in{found->second, std::string(path), module.declarations.size()};
    auto statements = get<StmtList>(in);
    if (!in.bytes.empty())
      invalid("trailing method data");
    body.restoreCompiledStatements(std::move(statements));
    ++restored;
  };
  bool generic = false;
  forEachMethod(declarations, load, generic);
  if (!generic || restored != module.bodies.size())
    invalid("template declaration mismatch");
}
std::string encodeTemplateModule(const CompiledTemplateModule &module) {
  Output out{std::string(compiledTemplateMagic)};
  out.text(module.declarations);
  out.number(module.bodies.size());
  for (const auto &[position, body] : module.bodies) {
    out.number(position);
    out.text(body);
  }
  return std::move(out.bytes);
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
  if (!in.bytes.empty() || module.bodies.empty())
    invalid("invalid method inventory");
  return module;
}
} // namespace lang
