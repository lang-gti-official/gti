#include "language_snippets.h"

#include <string_view>

namespace lang {
namespace {

bool identifierCharacter(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '_';
}

} // namespace

std::vector<CompletionCandidate>
completionSnippets(const SyntaxCompletionContext &context,
                   std::string_view source) {
  std::vector<CompletionCandidate> result;
  SourceSpan range{.source = context.token.source,
                   .start = context.token.position,
                   .end = context.token.position + context.token.lexeme.size(),
                   .line = context.token.line};
  // Replace the entire current word, not just the prefix before the cursor.
  while (range.end < source.size() && identifierCharacter(source[range.end]))
    ++range.end;
  const auto add = [&](std::string label, std::string detail,
                       std::string snippet) {
    if (!std::string_view(label).starts_with(context.token.lexeme))
      return;
    result.push_back({.kind = CompletionCandidateKind::Snippet,
                      .label = std::move(label),
                      .detail = std::move(detail),
                      .insertion = {},
                      .snippet = std::move(snippet),
                      .sortText = {},
                      .replacementRange = range});
  };
  const bool declaration = context.kind == SyntaxCompletionKind::Declaration;
  const bool member = context.kind == SyntaxCompletionKind::ClassMember;
  const bool ordinaryOwner = member && (context.ownerKind == ClassKind::Class ||
                                        context.ownerKind == ClassKind::Struct);
  if (declaration || ordinaryOwner) {
    add("class", "Class with a linked name and default constructor",
        "class ${1:Name} {\npublic:\n\t$1() = default;\n\nprivate:\n\t$0\n};");
    add("class generic", "Generic class",
        "class ${1:Name}<${2:T}> {\npublic:\n\t$1() = "
        "default;\n\nprivate:\n\t$0\n};");
    add("struct", "Struct declaration", "struct ${1:Name} {\n\t$0\n};");
    add("using", "Type alias", "using ${1:Name} = ${2:int32_t};$0");
  }
  if (declaration) {
    add("namespace", "Namespace declaration", "namespace ${1:name} {\n\t$0\n}");
    add("enum", "Scoped enum declaration",
        "enum class ${1:Name} {\n\t${2:Value},\n\t$0\n};");
    add("interface", "Interface with a read-only method requirement",
        "interface ${1:Name} {\n\tvoid ${2:method_name}(${3});\n\t$0\n};");
    add("fn", "Free function", "${1:void} ${2:function_name}(${3}) {\n\t$0\n}");
    add("macro", "Typed statement macro",
        "#macro ${1:name}(${2:int value}) {\n\t$0\n}");
  }
  if (ordinaryOwner && context.owner) {
    add("ctor", "Constructor for " + context.owner->lexeme,
        context.owner->lexeme + "(${1}) {\n\t$0\n}");
    add("dtor", "Cleanup body for " + context.owner->lexeme,
        "~" + context.owner->lexeme + "() {\n\t$0\n}");
    add("method", "Read-only instance method",
        "${1:void} ${2:method_name}(${3}) {\n\t$0\n}");
    add("mutmethod", "Mutable instance method",
        "${1:void} ${2:method_name}(${3}) mut {\n\t$0\n}");
    add("field", "Immutable field with an initializer",
        "${1:int32_t} ${2:name} = ${3:0};$0");
    add("mutfield", "Mutable field with an initializer",
        "mut ${1:int32_t} ${2:name} = ${3:0};$0");
  }
  if (context.kind == SyntaxCompletionKind::Statement) {
    add("if", "Conditional statement", "if (${1:condition}) {\n\t$0\n}");
    add("ifelse", "Conditional statement with an else branch",
        "if (${1:condition}) {\n\t${2}\n} else {\n\t$0\n}");
    add("for", "Indexed loop with a mutable counter",
        "for (mut int32_t ${1:index} = 0; $1 < ${2:count}; ++$1) {\n\t$0\n}");
    add("foreach", "Read-only borrowed range loop",
        "for (auto& ${1:value} : ${2:values}) {\n\t$0\n}");
    add("while", "While loop", "while (${1:condition}) {\n\t$0\n}");
    add("switch", "Switch statement",
        "switch (${1:value}) {\ncase ${2:0}:\n\t$0\n\tbreak;\n}");
    add("unsafe", "Lexical unsafe block", "unsafe {\n\t$0\n}");
  }
  return result;
}

} // namespace lang
