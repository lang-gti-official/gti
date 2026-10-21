#include "gti/language_queries.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

lang::CompletionResult query(
    std::string source,
    std::shared_ptr<lang::FrontendCache> cache = nullptr,
    lang::FrontendWorkMetrics *metrics = nullptr) {
  const std::size_t offset = source.find('|');
  require(offset != std::string::npos, "missing cursor marker");
  source.erase(offset, 1);
  return lang::LanguageQueries().complete({.entryPath = "snippet.gti",
                                            .source = std::move(source),
                                            .byteOffset = offset,
                                            .workMetrics = metrics,
                                            .cache = std::move(cache)});
}

const lang::CompletionCandidate *snippet(const lang::CompletionResult &result,
                                          std::string_view label) {
  const auto found = std::find_if(
      result.candidates.begin(), result.candidates.end(),
      [&](const auto &candidate) {
        return candidate.kind == lang::CompletionCandidateKind::Snippet &&
               candidate.label == label;
      });
  return found == result.candidates.end() ? nullptr : &*found;
}

void run() {
  for (const std::string prefix : {"cl", "class", "class generic"}) {
    const auto result = query(prefix == "class generic" ? "cl|" : prefix + "|");
    require(snippet(result, "class") != nullptr, "class declaration missing");
    require(snippet(result, "class generic") != nullptr, "generic class missing");
    require(snippet(result, "if") == nullptr, "statement leaked to namespace");
  }
  const auto top = query("|");
  for (const auto label : {"class", "struct", "enum", "interface", "namespace",
                           "using", "fn"})
    require(snippet(top, label) != nullptr, "namespace template missing");
  require(snippet(top, "ctor") == nullptr, "constructor without owner");

  const auto member = query("class Widget { public: | }; ");
  for (const auto label : {"class", "struct", "ctor", "dtor", "method",
                           "mutmethod", "field", "mutfield", "using"})
    require(snippet(member, label) != nullptr, "member template missing");
  require(snippet(member, "namespace") == nullptr, "namespace inside class");
  require(snippet(member, "ctor")->snippet->starts_with("Widget("),
          "constructor did not use its owner");
  const auto nested = query("class Outer { struct Inner { dt| }; }; ");
  require(snippet(nested, "dtor") != nullptr &&
              snippet(nested, "dtor")->snippet->starts_with("~Inner("),
          "nested cleanup used the outer owner");
  require(snippet(query("interface I { ct| };"), "ctor") == nullptr,
          "interface constructor offered");

  const auto body = query("void f() { | }");
  for (const auto label : {"if", "ifelse", "for", "foreach", "while", "switch",
                           "unsafe"})
    require(snippet(body, label) != nullptr, "statement template missing");
  require(snippet(body, "class") == nullptr, "declaration leaked to statement");
  require(snippet(query("void f() { if| }"), "if") != nullptr,
          "complete keyword trigger missing");

  for (const std::string source : {
           "void f() { int32_t x = cl|; }", "void f() { f(cl|); }",
           "void f() { this.cl|; }", "namespace n {} void f() { n::cl|; }",
           "// cl|", "/* cl| */", "/* cl|", "\"cl|\"", "\"cl|",
           "void f() { char c = 'c|'; }", "#include <cl|>",
           "#if defined(NEVER)\ncl|\n#endif\n"}) {
    const auto result = query(source);
    require(std::none_of(result.candidates.begin(), result.candidates.end(),
                         [](const auto &candidate) {
                           return candidate.kind ==
                                  lang::CompletionCandidateKind::Snippet;
                         }),
            "snippet escaped its syntactic boundary");
  }

  const std::string source = "/* 😀 */ cl|utter";
  const auto middle = query(source);
  const auto *candidate = snippet(middle, "class");
  require(candidate != nullptr, "middle-of-word completion missing");
  require(candidate->replacementRange.start == source.find("cl|") &&
              candidate->replacementRange.end == source.size() - 1,
          "replacement failed to include suffix/UTF-8 prefix");

  auto cache = std::make_shared<lang::FrontendCache>();
  lang::FrontendWorkMetrics metrics;
  query("class Cached { ct| };", cache);
  const auto cached = query("class Cached { ct| };", cache, &metrics);
  require(snippet(cached, "ctor") != nullptr, "cache lost syntax context");
  require(!metrics.parseCacheHits.empty(), "parsed cache was not reused");
}

} // namespace

int main() {
  try {
    run();
    std::cout << "Code snippet contexts passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
