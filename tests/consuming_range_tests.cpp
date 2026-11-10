#include "mir_test_fixture.h"
#include "lowered_program_test_support.h"
#include "gti/mir_printer.h"

namespace {
lang::FrontendResult check(std::string source) {
  const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
  return lang::Frontend().analyze(
      (root / "consuming-negative.gti").string(), std::move(source),
      {root / "stdlib/prelude.gti"}, {}, {root / "stdlib"});
}
void rejects(std::string source, std::string_view reason) {
  const auto result = check(std::move(source));
  expect(!result.canGenerateCode(), reason);
  expect(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto &d) { return d.code == "GTI-B0001"; }),
         "invalid source is rejected without an internal compiler error");
}
constexpr std::string_view counter = R"(
class Source {
  mut int count = 0;
public:
  Source() {}
  bool has_next() { return this.count < 3; }
  int take_next() mut { this.count++; return this.count; }
};
)";

template <typename Mutation>
void forgeRange(const lang::FrontendResult &frontend, Mutation mutate,
                std::string_view label) {
  auto mir = frontend.mir;
  bool changed = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           mir.functionInstances())) {
    for (auto &range : function.body.rangeIterations) {
      if (!changed &&
          range.strategy == lang::RangeIterationStrategy::Consuming) {
        mutate(function.body, range);
        (void)lang::rebuildMirValueUses(function.body);
        changed = true;
      }
    }
  }
  expect(changed && !lang::verifyMirProgram(mir).valid(), label);
}
template <typename Mutation>
void forgeTake(const lang::FrontendResult &frontend, Mutation mutate,
               std::string_view label) {
  auto mir = frontend.mir;
  bool changed = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           mir.functionInstances()))
    for (auto &block : function.body.blocks)
      for (auto &instruction : block.instructions)
        if (!changed &&
            instruction.intrinsic == lang::IntrinsicKind::ObjectTake &&
            instruction.info.traits.drop != lang::DropKind::Trivial) {
          mutate(instruction);
          changed = true;
        }
  expect(changed && !lang::verifyMirProgram(mir).valid(), label);
}
} // namespace

int main() {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "fixtures/consuming_range.gti";
  std::ifstream input(path);
  const std::string source((std::istreambuf_iterator<char>(input)), {});
  const auto frontend = analyzeWithStandardLibrary(path.string(), source);
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(), "consuming ranges have verified MIR");
  if (!frontend.canGenerateCode())
    return 1;
  expect(lang::verifyHirProgramPlans(frontend.semantics, frontend.hir).valid(),
         "consuming plans retain their exact HIR call/receiver identities");

  const auto scalar = check(std::string(counter) + R"(
int main() {
  mut int64_t sum = 0;
  for (mut int64_t value : Source()) { value += 1; sum += value; }
  return sum == 9 ? 0 : 1;
})");
  expect(
      scalar.canGenerateCode(),
      "owned scalar yields support ordinary binding conversion and mutation");
  if (!scalar.canGenerateCode())
    for (const auto &d : scalar.diagnostics)
      std::cerr << d.message << '\n';

  rejects(std::string(counter) +
              "int main() { Source s{}; for (auto v : s) {} return 0; }",
          "a named source needs explicit ownership transfer");
  rejects(std::string(counter) + "int main() { Source s{}; for (auto v : "
                                 "std::move(s)) {} return s.has_next(); }",
          "the consumed source cannot be reused");
  rejects(std::string(counter) +
              "int main() { for (auto& v : Source()) {} return 0; }",
          "consuming iteration rejects reference bindings");
  rejects(R"(class Source { public: Source() {} int has_next() { return 1; }
    int take_next() mut { return 2; } };
    int main() { for (auto v : Source()) {} return 0; })",
          "has_next must return bool exactly");
  rejects(
      R"(class Source { public: Source() {} bool has_next() mut { return true; }
    int take_next() mut { return 2; } };
    int main() { for (auto v : Source()) {} return 0; })",
      "has_next must be read-only");
  rejects(R"(class Source { public: Source() {} bool has_next() { return true; }
    int take_next() { return 2; } };
    int main() { for (auto v : Source()) {} return 0; })",
          "take_next must be mutable");
  rejects(R"(class Source { public: Source() {} bool has_next() { return true; }
    int take_next(int value = 2) mut { return value; } };
    int main() { for (auto v : Source()) {} return 0; })",
          "protocol calls cannot omit defaulted parameters");
  rejects(R"(class Source { int value = 2; public: Source() {}
    bool has_next() { return true; } int& take_next() mut { return this.value; } };
    int main() { for (auto v : Source()) {} return 0; })",
          "a producer cannot masquerade a borrow as an owned value");
  rejects(
      R"(class Source { int& value; public: Source(int& value) : value(value) {}
    bool has_next() { return true; } int take_next() mut { return this.value; } };
    int main() { int value = 2; for (auto v : Source(value)) {} return 0; })",
      "an owned cursor cannot hide a stored borrow");
  rejects(R"(class Source { public: Source() {} bool has_next() { return true; }
    std::string_view take_next() mut { return "text"; } };
    int main() { for (auto v : Source()) {} return 0; })",
          "legacy text handles do not prove independent ownership");
  rejects(
      R"(import std.memory; int main() { int* p = nullptr; auto value = std::take_at(p); return 0; })",
      "raw object extraction requires explicit unsafe permission");
  rejects(R"(import std.memory; int main() { const int* p = nullptr;
    unsafe { auto value = std::take_at(p); } return 0; })",
          "raw object extraction cannot consume read-only storage");
  rejects(R"(import std.vector; int main() { mut std::vector<int> values{};
    for (auto value : values.into_range()) {} return 0; })",
          "vector consuming receiver requires an explicit move");
  rejects(R"(import std.vector; int main() { mut std::vector<int> values{};
    for (auto value : std::move(values).into_range()) {} return values.empty(); })",
          "consuming a vector invalidates its original binding");
  rejects(R"(import std.vector; import std.string; int main() {
    mut std::vector<std::string> values{}; values.emplace_back("owned");
    auto value = std::move(values[std::size_t(0)]); return 0; })",
          "ordinary indexed vector extraction still cannot leave a hole");
  rejects(R"(import std.vector; int main() { mut std::vector<int> values{};
    values.emplace_back(1); auto& reference = values.front();
    for (auto value : std::move(values).into_range()) { std::println(reference); }
    return 0; })",
          "a retained element borrow prevents consuming the vector");

  for (bool condition : {false, true}) {
    auto hir = frontend.hir;
    bool changed = false;
    for (auto &function : const_cast<std::vector<lang::HirFunctionInstance> &>(
             hir.functionInstances()))
      for (auto &statement : function.body.statements)
        if (!changed && statement.rangeIteration &&
            statement.rangeIteration->strategy ==
                lang::RangeIterationStrategy::Consuming) {
          if (condition)
            ++statement.rangeIteration->compareFunction;
          else
            ++statement.rangeIteration->dereferenceFunction;
          changed = true;
        }
    expect(changed &&
               !lang::verifyHirProgramPlans(frontend.semantics, hir).valid(),
           "HIR rejects a forged selected consuming protocol declaration");
  }

  forgeRange(
      frontend, [](auto &, auto &r) { r.owner = r.iterator; },
      "MIR rejects a fabricated owner/cursor alias");
  forgeRange(
      frontend, [](auto &, auto &r) { r.element = r.iterator; },
      "MIR rejects a fabricated iteration-local binding");
  forgeRange(
      frontend, [](auto &, auto &r) { r.elementAccessValue = 0; },
      "MIR requires the exact element-producing call");
  forgeRange(
      frontend, [](auto &, auto &r) { ++r.dereferenceFunction; },
      "MIR binds the selected take declaration independently");
  forgeRange(
      frontend, [](auto &, auto &r) { r.ownsRangeTemporary = false; },
      "MIR rejects a borrowed consuming cursor");
  forgeRange(
      frontend,
      [](auto &body, auto &r) {
        auto *block =
            const_cast<lang::MirBlock *>(body.findBlock(r.conditionBlock));
        block->terminator.elseTarget = r.bodyBlock;
      },
      "MIR rejects a condition that cannot exit to cleanup");
  forgeTake(
      frontend,
      [](auto &i) { i.unsafeOperation = lang::UnsafeOperationKind::None; },
      "MIR rejects raw extraction without lifetime permission");
  forgeTake(
      frontend,
      [](auto &i) {
        i.parameterTypes[0].pointerAccess = lang::AccessMode::ReadOnly;
      },
      "MIR rejects raw extraction from read-only storage");
  forgeTake(
      frontend, [](auto &i) { i.lifecycle.clear(); },
      "MIR requires the owned result cleanup obligation");
  forgeTake(
      frontend, [](auto &i) { i.lifecycle.push_back(i.lifecycle.front()); },
      "MIR rejects duplicate ownership of the extracted value");
  forgeTake(
      frontend, [](auto &i) { i.result.reset(); },
      "MIR rejects extraction without an owned result");
  return failures ? 1 : 0;
}
