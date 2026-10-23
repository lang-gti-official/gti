#include "gti/language_queries.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

lang::FrontendResult analyze(std::string source) {
  lang::FrontendOptions options;
  options.stopAfter = lang::FrontendPhase::Semantics;
  return lang::Frontend(options).analyze("macro-inspection.gti",
                                         std::move(source));
}

void run() {
  const std::string source = R"(
void use(int value) {}
#macro inner(int value) {
  mut int saved = value;
  { int saved = 4; use(saved); }
  while (saved < 3) { ++saved; }
  use(saved);
}
#macro outer(int value) { inner(value); }
int main() {
  int saved = 7;
  outer(saved);
  outer(8);
  return 0;
}
)";
  auto snapshot = analyze(source);
  for (const auto &diagnostic : snapshot.diagnostics)
    if (diagnostic.severity == lang::DiagnosticSeverity::Error)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  require(snapshot.semanticValid, "macro inspection fixture must be valid");
  const auto unit = snapshot.sourceGraph.entryUnit();
  const lang::LanguageQueries queries;
  const auto offset = source.find("outer(saved)");
  const auto info = queries.macroExpansion(snapshot, unit, offset + 2);
  require(info.has_value(),
          "written invocation must have a retained expansion");
  require(!info->truncated && info->text.size() < 16384,
          "small expansion must be complete and bounded");
  require(info->text.find("not executable source") != std::string::npos &&
              info->text.find("Nested macro expansion") != std::string::npos &&
              info->text.find("while (") != std::string::npos,
          "inspection must explain nested checked control flow");
  require(info->text.find("saved__macro_local_1 = saved") !=
                  std::string::npos &&
              info->text.find("saved__macro_local_2") != std::string::npos,
          "hygienic locals must differ from same-spelled caller arguments");
  require(info->invocation.start == offset &&
              info->invocation.end ==
                  offset + std::string("outer(saved)").size(),
          "query must retain exact written call geometry");
  const auto again = queries.macroExpansion(snapshot, unit, offset + 2);
  require(again && again->text == info->text,
          "inspection must be deterministic on one immutable snapshot");
  require(!queries.macroExpansion(snapshot, unit, source.find("return 0")),
          "ordinary statements must not invent macro expansions");
  require(!queries.macroExpansion(snapshot, unit, source.size() + 1),
          "out-of-source queries must fail closed");

  // Expanded runtime symbols remain distinct, but references and rename act
  // on the exact written binder. Same-spelled nested/caller names stay
  // separate.
  const auto binder = source.find("saved = value");
  const auto definition = queries.definition(
      snapshot, unit, source.find("use(saved);", source.find("while")) + 4);
  require(definition && definition->target.start == binder,
          "expanded local use must navigate to its written binder");
  const auto renamed = queries.rename(snapshot, unit, binder, "temporary");
  require(renamed.edits.has_value(),
          "macro-local written binder must be renamable");
  const auto shadowed = source.find("saved = 4");
  const auto caller = source.find("saved = 7");
  require(std::all_of(renamed.edits->spans.begin(), renamed.edits->spans.end(),
                      [&](const auto &span) {
                        return span.start != shadowed && span.start != caller &&
                               span.start < source.find("#macro outer");
                      }),
          "macro-local rename must not capture shadowed or caller symbols");
  require(std::any_of(renamed.edits->spans.begin(), renamed.edits->spans.end(),
                      [&](const auto &span) { return span.start == binder; }),
          "macro-local rename must include its written declaration");

  std::string large = "void sink(int value) {}\n#macro large(int value) {\n";
  for (int index = 0; index < 1500; ++index)
    large += "sink(value + 123456789);\n";
  large += "}\nint main() { large(1); return 0; }\n";
  auto largeSnapshot = analyze(large);
  require(largeSnapshot.semanticValid,
          "large inspection fixture must be valid");
  const auto bounded = queries.macroExpansion(
      largeSnapshot, largeSnapshot.sourceGraph.entryUnit(),
      large.rfind("large(1)"));
  require(bounded && bounded->truncated && bounded->text.size() <= 16384 &&
              bounded->text.find("display truncated") != std::string::npos,
          "large expansions must terminate with an explicit bounded omission");
}
} // namespace

int main() {
  try {
    run();
    std::cout
        << "Macro inspection, hygiene navigation and bounded output passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
