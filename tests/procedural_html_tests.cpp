#include "gti/frontend.h"
#include "gti/frontend_cache.h"
#include "gti/language_queries.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
const auto repository =
    std::filesystem::path(__FILE__).parent_path().parent_path();
const auto entry = repository / "examples" / "html-query.gti";
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
void errors(const lang::FrontendResult &snapshot) {
  for (const auto &d : snapshot.diagnostics)
    if (d.severity == lang::DiagnosticSeverity::Error)
      std::cerr << d.code << " at " << d.primary.source << ':' << d.primary.line
                << " [" << d.primary.start << ',' << d.primary.end
                << "]: " << d.message << '\n';
}
lang::FrontendResult
analyze(std::string source, lang::FrontendOptions options = {},
        std::unordered_map<std::string, std::string> overrides = {}) {
  options.stopAfter = lang::FrontendPhase::Semantics;
  const auto stdlib = repository / "stdlib";
  return lang::Frontend(std::move(options))
      .analyze(
          entry, std::move(source), {stdlib / "prelude.gti"},
          std::move(overrides), {stdlib},
          {{.identity = "app",
            .name = "app",
            .packageRoot = repository / "examples",
            .sourceRoot = repository / "examples",
            .dependencies = {{"demo", "html-demo"}}},
           {.identity = "html-demo",
            .name = "html-demo",
            .packageRoot = repository / "examples/demo",
            .sourceRoot = repository / "examples/demo",
            .modules = {{"html", repository / "examples/demo/html.gti"}}}});
}
void run() {
  const std::string source = R"(
import demo.html;
struct User { std::string_view name; User(std::string_view value) : name(value) {} };
int main() {
  User user{"Ada & Lin"};
  int child = 9; int root = 8; int children = 7; int with_children = 6;
  auto page = demo::html!({<div class="profile"><h1>{user.name}</h1></div>});
  auto text = demo::render(page);
  return child + root + children + with_children - 30;
}
)";
  auto snapshot = analyze(source);
  errors(snapshot);
  require(snapshot.semanticValid,
          "imported HTML library analyzes with caller shadowing");
  lang::LanguageQueries queries;
  auto unit = snapshot.sourceGraph.entryUnit();
  auto embedded = source.find("user.name");
  auto hover = queries.hover(snapshot, unit, embedded + 1);
  require(hover.has_value(), "hover resolves an embedded caller expression");
  auto definition = queries.definition(snapshot, unit, embedded + 1);
  require(definition && definition->target.start == source.find("user{"),
          "embedded caller name navigates to the original declaration");
  auto renamed = queries.rename(snapshot, unit, embedded + 1, "person");
  require(
      renamed.edits &&
          std::any_of(renamed.edits->spans.begin(), renamed.edits->spans.end(),
                      [&](const auto &span) { return span.start == embedded; }),
      "rename includes written interpolation without renaming generated "
      "helpers");
  auto expansion =
      queries.macroExpansion(snapshot, unit, source.find("demo::html!") + 7);
  require(expansion && expansion->text.find("append") != std::string::npos &&
              expansion->text.find("user.name") != std::string::npos,
          "inspection presents checked HTML construction and caller splice");

  for (const auto &[markup, message] :
       std::vector<std::pair<std::string, std::string>>{
           {"<div><p></div>", "does not match"},
           {"<div>", "missing its closing tag"},
           {"<div class></div>", "attributes require"},
           {"<div>raw</div>", "quoted text"},
           {"<div/><p/>", "exactly one root"},
           {"<div class=4/>", "quoted text"}}) {
    auto rejected = analyze("import demo.html; int main(){ auto page=html!({" +
                            markup + "}); return 0; }");
    const auto found =
        std::find_if(rejected.diagnostics.begin(), rejected.diagnostics.end(),
                     [&](const auto &d) {
                       return d.message.find(message) != std::string::npos;
                     });
    if (found == rejected.diagnostics.end())
      errors(rejected);
    require(!rejected.semanticValid && found != rejected.diagnostics.end(),
            "invalid markup receives library-authored diagnostic");
    require(found->primary.source == entry.string() &&
                found->primary.end > found->primary.start &&
                !found->related.empty(),
            "markup failure preserves primary and related source spans");
  }
  auto unknown = analyze("import demo.html; int main(){ auto "
                         "page=html!({<div>{missing}</div>}); return 0; }");
  require(!unknown.semanticValid,
          "unknown interpolation is rejected by ordinary semantics");
  auto badType = analyze("import demo.html; int main(){ auto "
                         "page=html!({<div>{42}</div>}); return 0; }");
  require(!badType.semanticValid,
          "unsupported child types are not implicitly stringified");
  auto privateAccess = analyze(
      "import demo.html; class Vault { std::string_view secret=\"hidden\"; }; "
      "int main(){Vault vault; auto page=html!({<p>{vault.secret}</p>}); "
      "return 0;}");
  require(!privateAccess.semanticValid &&
              std::any_of(privateAccess.diagnostics.begin(),
                          privateAccess.diagnostics.end(),
                          [](const auto &d) {
                            return d.message.find("private") !=
                                   std::string::npos;
                          }),
          "caller interpolation cannot inherit library access privileges");
  auto escaping = analyze(
      "import demo.html; import std.string; int main(){ auto page=html!({<p>{"
      "[]() -> std::string_view {std::string local{\"gone\"}; return "
      "local.view();}()"
      "}</p>}); return 0;}");
  require(!escaping.semanticValid &&
              std::any_of(escaping.diagnostics.begin(),
                          escaping.diagnostics.end(),
                          [&](const auto &d) {
                            return d.code == "GTI-S2045" &&
                                   d.message.find("owner dependency") !=
                                       std::string::npos &&
                                   d.primary.source == entry.string() &&
                                   !d.related.empty();
                          }),
          "ordinary borrow checking rejects an escaping temporary at its "
          "mapped origin");
  auto duplicate =
      analyze("import demo.html; int main(){ auto child=html!({<b/>}); auto "
              "page=html!({<div>{std::move(child)}{std::move(child)}</div>}); "
              "return 0; }");
  require(!duplicate.semanticValid,
          "the ordinary move checker rejects repeated child consumption");
}
} // namespace
int main() {
  try {
    run();
    std::cout << "procedural HTML semantics and source queries passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
