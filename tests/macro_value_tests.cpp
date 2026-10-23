#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/mir_lowering.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void check(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
lang::FrontendResult analyze(std::string source) {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze("macro-values.gti", std::move(source),
                                  {library / "prelude.gti"}, {}, {library});
}
void diagnostic(std::string source, std::string_view fragment) {
  const auto result = analyze(std::move(source));
  check(!result.canGenerateCode(),
        "invalid result macro must stop before emission");
  const bool found = std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(),
      [&](const auto &value) {
        return value.message.find(fragment) != std::string::npos;
      });
  check(found, fragment);
  if (!found)
    for (const auto &value : result.diagnostics)
      std::cerr << value.code << ": " << value.message << '\n';
  check(
      std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                   [](const auto &value) { return value.code == "GTI-B0001"; }),
      "unsupported result shapes need a source diagnostic, not an ICE");
}
void positiveAndMutation() {
  const std::string source = R"gti(
#macro int choose(bool enabled, int first, int second) {
  if (enabled) { return first; }
  return second;
}
#macro int once(int expression) { int saved = expression; return saved + saved; }
#macro int nested(int expression) { return choose(true, once(expression), 0); }
#macro stop() { return; }
int main() {
  mut int effects = 0;
  int answer = choose(false, ++effects, nested(5));
  stop();
  return answer - 10 + effects;
}
)gti";
  auto result = analyze(source);
  if (!result.canGenerateCode())
    for (const auto &value : result.diagnostics)
      std::cerr << value.code << ": " << value.message << '\n';
  check(result.canGenerateCode(),
        "scoped scalar and void result regions must lower");
  if (!result.canGenerateCode())
    return;
  check(lang::verifyHirProgramPlans(result.semantics, result.hir).valid(),
        "result-region HIR must verify independently");
  check(lang::verifyMirProgram(result.mir).valid(),
        "result-region MIR must verify independently");
  const auto inspected = lang::LanguageQueries().macroExpansion(
      result, result.sourceGraph.entryUnit(), source.find("choose(false") + 2);
  check(inspected.has_value(),
        "value calls must support read-only expansion inspection");
  bool mutated = false;
  for (const auto &function : result.hir.functionInstances()) {
    for (const auto &statement : function.body.statements) {
      if (statement.resultRegion == 0)
        continue;
      auto malformed = result.hir;
      auto *changed = const_cast<lang::HirFunctionInstance *>(
          malformed.findFunctionInstance(function.id));
      const auto found = std::find_if(
          changed->body.statements.begin(), changed->body.statements.end(),
          [&](const auto &candidate) { return candidate.id == statement.id; });
      found->resultRegion = 0;
      check(!lang::verifyHirProgramPlans(result.semantics, malformed).valid(),
            "erasing a macro-local return target must not create a caller "
            "return");
      check(
          !lang::MirLowerer().lower(malformed, result.failureMetadata).valid(),
          "MIR lowering must reject an erased local return target");
      found->resultRegion = statement.resultRegion + 100000;
      check(!lang::verifyHirProgramPlans(result.semantics, malformed).valid(),
            "forged cross-region returns must be rejected");
      mutated = true;
      break;
    }
    if (mutated)
      break;
  }
  check(mutated, "fixture must retain a macro-local return identity");
}
void owningResults() {
  const auto result = analyze(R"gti(
#include <std/string>
#macro std::string select(bool enabled, std::string first, std::string second) {
  if (enabled) { return first; }
  return second;
}
#macro std::string local(std::string expression) {
  std::string saved = expression;
  return saved;
}
int main() {
  std::string untouched{"untouched"};
  std::string selected = select(false, std::move(untouched), local(std::string("yes")));
  return int32_t(untouched.size() + selected.size());
}
)gti");
  if (!result.canGenerateCode())
    for (const auto &value : result.diagnostics)
      std::cerr << value.code << ": " << value.message << '\n';
  check(result.canGenerateCode(),
        "move-only results and skipped moves must preserve caller ownership");
}
} // namespace
int main() {
  positiveAndMutation();
  owningResults();
  diagnostic("#macro int bad(bool flag) { if (flag) { return 1; } }\n"
             "int main() { return bad(false); }",
             "every normally completing path");
  diagnostic("#macro int bad() { return; }\nint main() { return bad(); }",
             "requires a value");
  diagnostic("#macro int bad() { return true; }\nint main() { return bad(); }",
             "macro result");
  diagnostic("#macro stop() { return 1; }\nint main() { stop(); return 0; }",
             "void macro cannot return");
  diagnostic("#macro int& bad() { int saved = 1; return saved; }\n"
             "int main() { int& value = bad(); return value; }",
             "cannot export a reference");
  diagnostic("#include <std/string>\n#macro std::string identity(std::string "
             "value) { return value; }\n"
             "int main() { std::string value{\"owned\"}; std::string moved = "
             "identity(value); return 0; }",
             "macro result");
  diagnostic(R"gti(
#include <std/string>
#macro std::string inner(std::string argument) { return argument; }
#macro std::string outer() { std::string saved{"owned"}; return inner(saved); }
int main() { std::string value = outer(); return 0; }
)gti",
             "macro result");
  diagnostic(R"gti(
class Pinned {
public:
  Pinned() {}
  Pinned(Pinned& other) = delete;
  Pinned(Pinned&& other) = delete;
};
#macro Pinned pinned() { return Pinned(); }
int main() { Pinned value = pinned(); return 0; }
)gti",
             "movable or copyable");
  return failures == 0 ? 0 : 1;
}
