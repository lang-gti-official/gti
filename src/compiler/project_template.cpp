#include "gti/project_template.h"
#include "gti/lexer.h"
#include "gti/module_name.h"
#include <algorithm>
#include <stdexcept>

namespace lang {
namespace {
constexpr std::string_view magic = "gti.project-template.1\n";
constexpr std::size_t limit = 1024 * 1024;
[[noreturn]] void reject(std::string_view reason) {
  throw std::invalid_argument("compiled project template: " +
                              std::string(reason));
}
void appendNumber(std::string &out, std::size_t n) {
  if (n > limit || out.size() > limit - 4)
    reject("metadata exceeds limit");
  for (unsigned i = 0; i != 4; ++i)
    out.push_back(static_cast<char>((n >> (8 * i)) & 255));
}
unsigned takeNumber(std::string_view &in) {
  if (in.size() < 4)
    reject("truncated metadata");
  unsigned value = 0;
  for (unsigned i = 0; i != 4; ++i)
    value |= static_cast<unsigned>(static_cast<unsigned char>(in[i]))
             << (8 * i);
  in.remove_prefix(4);
  return value;
}
void appendText(std::string &out, std::string_view text) {
  appendNumber(out, text.size());
  if (text.size() > limit - out.size())
    reject("metadata exceeds limit");
  out += text;
}
std::string_view takeText(std::string_view &in) {
  auto n = takeNumber(in);
  if (n > in.size())
    reject("truncated text");
  auto text = in.substr(0, n);
  in.remove_prefix(n);
  return text;
}
std::vector<Token> tokens(std::string_view text, std::string_view path = {}) {
  Lexer lexer;
  auto result = lexer.scan(std::string(text), std::string(path));
  if (lexer.hadError())
    reject("invalid lexical input");
  return result;
}
bool erased(const Token &token, std::string_view declarations) {
  return !token.lexeme.empty() && token.kind != TokenKind::END_OF_FILE &&
         declarations.substr(token.position, token.lexeme.size()) !=
             token.lexeme;
}
} // namespace

std::string projectTemplateModuleName(std::string_view declarations) {
  auto input = tokens(declarations);
  const auto header = parseModuleHeader(input);
  if (!header || !header->interface)
    reject("expected an exported module");
  for (std::size_t i = header->tokenEnd; i < input.size(); ++i) {
    const auto &t = input[i];
    if (t.kind >= TokenKind::HASH_IF && t.kind <= TokenKind::HASH_IFNDEF)
      reject("configuration or macro directives require their source closure");
    if (t.kind == TokenKind::MODULE)
      reject("unexpected second module declaration");
    if (t.kind == TokenKind::IMPORT) {
      // File and package imports require a separately bound dependency.
      // Reinterpreting a producer-relative path at the consumer is incorrect.
      if (i + 3 >= input.size() || input[i + 1].lexeme != "std" ||
          input[i + 2].kind != TokenKind::DOT ||
          input[i + 3].kind != TokenKind::IDENTIFIER)
        reject("import requires a packaged dependency");
      std::size_t j = i + 4;
      while (j + 1 < input.size() && input[j].kind == TokenKind::DOT &&
             input[j + 1].kind == TokenKind::IDENTIFIER)
        j += 2;
      if (j >= input.size() || input[j].kind != TokenKind::SEMICOLON)
        reject("invalid standard library import");
    }
  }
  return header->declaration.lexeme;
}

std::string encodeProjectTemplate(const CompiledTemplateModule &module,
                                  std::string_view source) {
  (void)projectTemplateModuleName(source);
  std::string out(magic);
  appendText(out, encodeTemplateModule(module));
  if (source.size() != module.declarations.size())
    reject("declaration and source extents differ");
  // A macro can observe comments and tabs between tokens in its arguments.
  // Restore the exact erased bytes before asking the ordinary parser to attest
  // the recipe. The declaration skeleton already contains spaces and newlines.
  for (std::size_t i = 0; i < source.size();) {
    if (source[i] == module.declarations[i]) {
      ++i;
      continue;
    }
    const auto begin = i;
    while (i < source.size() && source[i] != module.declarations[i])
      ++i;
    appendNumber(out, begin);
    appendText(out, source.substr(begin, i - begin));
  }
  return out;
}

CompiledTemplateModule decodeProjectTemplate(std::string_view bytes) {
  if (bytes.size() > limit || !bytes.starts_with(magic))
    reject("invalid project envelope");
  bytes.remove_prefix(magic.size());
  auto module = decodeTemplateModule(takeText(bytes));
  auto text = module.declarations;
  std::size_t end = 0;
  while (!bytes.empty()) {
    auto position = takeNumber(bytes);
    auto spelling = takeText(bytes);
    if (spelling.empty() || position < end || position > text.size() ||
        spelling.size() > text.size() - position)
      reject("invalid lexical extent");
    for (std::size_t i = 0; i < spelling.size(); ++i)
      if (text[position + i] != ' ' &&
          !(text[position + i] == '\n' && spelling[i] == '\n'))
        reject("body spelling overlaps declaration");
    text.replace(position, spelling.size(), spelling);
    end = position + spelling.size();
  }
  (void)projectTemplateModuleName(text);
  module.projectSyntax = std::move(text);
  return module;
}

std::vector<Token> projectTemplateTokens(const CompiledTemplateModule &module,
                                         std::span<const Token> interfaceTokens,
                                         std::string_view path) {
  // The trusted source loader already removed module/import directives. Insert
  // just the erased body tokens at their original offsets. Reproducing exactly
  // the saved recipe proves it can be constructed by the ordinary GTI parser
  // before any untrusted node reaches semantic analysis.
  const auto &source = module.projectSyntax.value();
  auto added = tokens(source, path);
  std::erase_if(added, [&](const Token &token) {
    return !erased(token, module.declarations);
  });
  std::vector<Token> merged;
  merged.reserve(interfaceTokens.size() + added.size());
  std::merge(
      interfaceTokens.begin(), interfaceTokens.end(), added.begin(),
      added.end(), std::back_inserter(merged),
      [](const Token &a, const Token &b) { return a.position < b.position; });
  return merged;
}

void verifyProjectTemplate(const CompiledTemplateModule &module,
                           std::span<const StmtPtr> declarations,
                           const LifetimeNames &lifetimes) {
  auto canonical = compileTemplateModule(
      declarations, module.projectSyntax.value(), lifetimes);
  if (encodeTemplateModule(canonical) != encodeTemplateModule(module))
    reject("body recipe does not match ordinary parsed syntax");
}
} // namespace lang
