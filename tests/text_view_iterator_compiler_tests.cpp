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
template <typename Mutation>
void forgedRange(const lang::FrontendResult &frontend, Mutation mutation,
                 std::string_view label, bool owning = false) {
  auto mir = frontend.mir;
  bool changed = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           mir.functionInstances()))
    for (auto &range : function.body.rangeIterations)
      if (!changed && range.materializesElementValue &&
          (range.elementOwnerDrop != 0) == owning) {
        mutation(function.body, range);
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
         "backend refuses forged iteration materialization");
}
} // namespace

int main() {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto entry = root / "text-view-iterator-compiler-test.gti";
  const std::string source = R"(
import std.string_view;
using IndependentText = std::string_view;
char unrelated_yield() { return 'a'; }
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
  mut int count = 0;
  for (char& character : view) {
    char independent = unrelated_yield();
    if (character != '\0' && independent == 'a') { count++; }
  }
  return *cursor == 'a' && *middle == 'b' && *user == 'a' && count == 3 ? 0 : 1;
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
  forgedRange(
      frontend,
      [](auto &body, auto &range) {
        for (const auto &place : body.places)
          if (place.root == lang::MirPlaceRootKind::Binding &&
              place.type == range.elementType &&
              place.id != range.elementOwner) {
            range.elementOwner = place.id;
            return;
          }
      },
      "same-type source binding cannot replace the exact hidden value owner");
  forgedRange(
      frontend, [](auto &, auto &range) { range.elementOwnerStatement = 0; },
      "iteration storage must retain its synthetic declaration identity");
  forgedRange(
      frontend, [](auto &, auto &range) { range.elementAccessSource++; },
      "iteration storage cannot substitute another source expression");
  forgedRange(
      frontend, [](auto &, auto &range) { range.dereferenceFunction++; },
      "iteration value requires the actual selected dereference declaration");
  forgedRange(
      frontend,
      [](auto &body, auto &range) {
        body.loans[range.elementLoan - 1].escapes = true;
      },
      "an iteration-local value cannot publish its checked borrow");
  forgedRange(
      frontend,
      [](auto &body, auto &range) {
        for (auto &block : body.blocks)
          std::erase_if(block.instructions, [&](const auto &instruction) {
            return instruction.kind == lang::MirInstructionKind::EndBorrow &&
                   instruction.loan == range.elementLoan;
          });
      },
      "iteration loan retirement is required at every body exit");
  auto malformedHir = frontend.hir;
  bool malformedStaging = false;
  for (auto &function : const_cast<std::vector<lang::HirFunctionInstance> &>(
           malformedHir.functionInstances()))
    for (auto &statement : function.body.statements)
      if (!malformedStaging && statement.rangeIteration &&
          statement.rangeIteration->materializesElementValue) {
        statement.rangeIteration->elementOwner =
            statement.rangeIteration->element;
        malformedStaging = true;
      }
  expect(malformedStaging &&
             !lang::verifyHirProgramPlans(frontend.semantics, malformedHir)
                  .valid(),
         "HIR rejects a forged hidden iteration owner before MIR");
  const auto records = lang::Frontend().analyze(
      root / "tests/fixtures/text_view_iterator_completion.gti", std::nullopt,
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
  expect(
      records.canGenerateCode(),
      "unrelated scalar, record and virtual cursors compose with algorithms");
  if (records.canGenerateCode()) {
    forgedRange(
        records, [](auto &, auto &range) { range.elementOwnerDrop = 0; },
        "owned record materialization cannot erase its lexical drop", true);
    forgedRange(
        records,
        [](auto &, auto &range) {
          range.elementSourceDrop = range.elementOwnerDrop;
        },
        "owned record materialization cannot invent pending-result publication",
        true);
    forgedRange(
        records,
        [](auto &body, auto &range) {
          const auto *access = body.findValue(range.elementAccessValue);
          for (auto &block : body.blocks)
            for (auto &instruction : block.instructions)
              if (access && instruction.id == access->definition)
                instruction.successResultDestination.reset();
        },
        "direct record publication retains its exact success destination",
        true);
    forgedRange(
        records,
        [](auto &body, auto &range) {
          range.elementOwnerDrop = body.dropObligations.size() + 1;
        },
        "invalid owned iteration lifetime identity fails closed", true);
    forgedRange(
        records,
        [](auto &body, auto &range) {
          for (auto &block : body.blocks)
            for (auto &instruction : block.instructions)
              if (instruction.kind == lang::MirInstructionKind::Initialize &&
                  instruction.destination == range.elementOwner)
                instruction.lifecycle.clear();
        },
        "record initialization must transfer its exact existing cleanup "
        "obligation",
        true);
  } else {
    for (const auto &diagnostic : records.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
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
