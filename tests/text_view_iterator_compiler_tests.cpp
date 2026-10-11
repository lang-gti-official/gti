#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void expect(bool condition, std::string_view label) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << label << '\n';
  }
}
template <typename Mutation>
void forged(const lang::FrontendResult &frontend, Mutation mutation,
            std::string_view label) {
  auto mir = frontend.mir;
  bool changed = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           mir.functionInstances()))
    for (auto &block : function.body.blocks)
      for (auto &instruction : block.instructions)
        if (!changed && instruction.kind == lang::MirInstructionKind::Call &&
            instruction.intrinsic == lang::IntrinsicKind::None &&
            instruction.functionTarget &&
            mir.findFunctionInstance(*instruction.functionTarget)
                    ->overloadedOperator ==
                lang::OverloadedOperator::Dereference &&
            instruction.info.type == lang::SemanticType::Char) {
          mutation(function.body, instruction);
          changed = true;
        }
  expect(changed && !lang::verifyMirProgram(mir).valid(), label);
  bool rejected = false;
  try {
    (void)gti_test::emitCpp(frontend, frontend.mir, mir);
  } catch (const std::exception &) {
    rejected = true;
  }
  expect(changed && rejected,
         "backend refuses forged independent value-yield call");
}
} // namespace

int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto entry = root / "text-view-iterator-compiler-test.gti";
  const std::string source = R"(
#include <std/string_view>
using IndependentText = std::string_view;
class UserAdapter {
  IndependentText& text;
public:
  UserAdapter(IndependentText& text) : text(text) {}
  std::detail::text_view_iterator<0> cursor() { return std::begin(this.text); }
};
int main() {
  IndependentText view = "abc";
  auto cursor = view.begin();
  auto middle = view.iterator_at(std::size_t(1));
  UserAdapter adapter{view};
  auto user = adapter.cursor();
  return *cursor == 'a' && *middle == 'b' && *user == 'a' ? 0 : 1;
}
)";
  const auto frontend = lang::Frontend().analyze(
      entry, source, {root / "stdlib/prelude.gti"},
      {{std::filesystem::weakly_canonical(entry).string(), source}},
      {root / "stdlib"});
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "alias and unrelated adapter preserve verified view loans");
  const auto hover =
      lang::LanguageQueries().hover(frontend, frontend.sourceGraph.entryUnit(),
                                    source.find("view.begin") + 5);
  expect(hover && hover->signature.find("::begin()") != std::string::npos &&
             hover->signature.find("gti_internal") == std::string::npos,
         "public member hover hides injected implementation receiver");
  const auto help = lang::LanguageQueries().signatureHelp(
      frontend, frontend.sourceGraph.entryUnit(),
      source.find("view.iterator_at(") +
          std::string_view("view.iterator_at(").size());
  expect(help && help->parameterLabels.size() == 1 &&
             help->parameterLabels.front().find("position") !=
                 std::string::npos &&
             help->activeParameter == 0,
         "signature help counts only written arguments");
  const auto completion = lang::LanguageQueries().complete(
      {.entryPath = entry,
       .source = source,
       .byteOffset = source.find("view.begin") + 5,
       .preludePaths = {root / "stdlib/prelude.gti"},
       .standardLibraryRoots = {root / "stdlib"}});
  for (const std::string_view member :
       {"begin", "end", "rbegin", "rend", "iterator_at"})
    expect(
        std::any_of(completion.candidates.begin(), completion.candidates.end(),
                    [&](const auto &item) { return item.label == member; }),
        "compiler member completion exposes each source-defined cursor family");
  forged(
      frontend, [](auto &, auto &instruction) { instruction.loan = 1; },
      "independent code-unit yield cannot invent a checked result loan");
  forged(
      frontend,
      [](auto &, auto &instruction) {
        instruction.borrowOrigin = lang::BorrowOriginKind::Receiver;
      },
      "independent value yield cannot forge a receiver-dependent result");
  forged(
      frontend,
      [](auto &, auto &instruction) {
        instruction.info.type = lang::SemanticType::referenceTo(
            lang::SemanticType::Char, lang::AccessMode::ReadOnly);
      },
      "independent value yield cannot forge typed reference materialization");
  bool normalized = false;
  for (const auto &function : frontend.hir.functionInstances()) {
    for (const auto &value : function.body.values) {
      const auto *call = lang::exprAs<lang::Call>(value.source);
      const auto *selected =
          call ? frontend.semantics.findCall(*call) : nullptr;
      if (!selected || !selected->argumentReceiver)
        continue;
      normalized = true;
      expect(lang::validResolvedCallArguments(*call, selected) &&
                 value.callPlan && !value.callPlan->receiver &&
                 value.callPlan->arguments.size() ==
                     lang::resolvedCallArgumentCount(*call, selected),
             "source member has one ordinary ordered call plan, without "
             "duplicate receiver");
      if (value.callPlan) {
        for (std::size_t index = 0; index < value.callPlan->arguments.size();
             ++index) {
          const auto &input = value.callPlan->arguments[index];
          const auto *staged = function.body.findValue(input.value);
          const auto *written =
              lang::resolvedCallArgument(*call, selected, index);
          expect(input.parameterIndex == index && staged && written &&
                     staged->source == written->get(),
                 "ordered ordinary argument plan stages receiver once before "
                 "written arguments");
        }
      }
      auto stale = *selected;
      stale.argumentReceiver = &call->callee();
      expect(
          !lang::validResolvedCallArguments(*call, &stale) &&
              lang::resolvedCallArgumentCount(*call, &stale) == 0 &&
              lang::resolvedCallArgument(*call, &stale, 0) == nullptr,
          "foreign AST receiver identity fails normalized-call lookup closed");
    }
  }
  expect(normalized, "test contains a normalized builtin member call");
  return failures == 0 ? 0 : 1;
}
