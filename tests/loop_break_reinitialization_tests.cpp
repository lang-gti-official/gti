#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
constexpr std::string_view owner = R"(
class Own {
  int value;
public:
  Own(int value) : value(value) {}
  Own(Own& other) = delete;
  Own(Own&& other) = default;
  int get() { return this.value; }
};
void consume(Own value) {}
)";

int failures = 0;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
} // namespace

int main() {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  for (const std::string_view loop :
       {"while (true) { consume(std::move(value)); break; }",
        "while ((true)) { consume(std::move(value)); break; }",
        "for (;;) { consume(std::move(value)); break; }",
        "for (mut int i = 0; true; ++i) { if (i == 1) { "
        "consume(std::move(value)); break; } }",
        "do { consume(std::move(value)); break; } while (true);",
        "while (false) { consume(std::move(value)); break; }"}) {
    auto frontend = lang::Frontend().analyze(
        "loop-break-reinitialize.gti",
        std::string(owner) + "int main() { mut Own value = Own(1); " +
            std::string(loop) + " value = Own(2); return value.get() - 2; }",
        {library / "prelude.gti"}, {}, {library});
    expect(frontend.canGenerateCode() &&
               lang::verifyMirProgram(frontend.mir).valid(),
           "literal loop exit availability must agree with semantic state");
    if (!frontend.canGenerateCode()) {
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
      continue;
    }
    if (loop != "while (true) { consume(std::move(value)); break; }")
      continue;

    auto changedCondition = frontend.mir;
    bool changed = false;
    for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
             changedCondition.functionInstances())) {
      for (auto &block : function.body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.operation == lang::MirOperation::Literal &&
              instruction.info.type == lang::SemanticType::Bool &&
              instruction.literal &&
              std::get_if<bool>(&*instruction.literal) != nullptr) {
            instruction.literal = lang::Literal{false};
            changed = true;
          }
    }
    expect(changed && !lang::verifyMirProgram(changedCondition).valid(),
           "a changed executable branch must invalidate moved-state proof");

    auto changedEvent = frontend.mir;
    changed = false;
    for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
             changedEvent.functionInstances())) {
      for (auto &block : function.body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.ownership &&
              instruction.ownership->kind ==
                  lang::OwnershipEventKind::Reinitialize &&
              instruction.ownership->before == lang::OwnershipStateSet::Moved) {
            instruction.ownership->before = lang::OwnershipStateSet::Available;
            changed = true;
          }
    }
    expect(changed && !lang::verifyMirProgram(changedEvent).valid(),
           "post-break assignment must retain its exact ownership transition");
  }
  {
    auto frontend = lang::Frontend().analyze(
        "loop-break-dynamic.gti", std::string(owner) + R"(
int exercise(bool condition) {
  mut Own value = Own(1);
  while (condition) { consume(std::move(value)); break; }
  value = Own(2);
  return value.get() - 2;
}
int main() { return exercise(true) + exercise(false); }
)",
        {library / "prelude.gti"}, {}, {library});
    expect(
        frontend.canGenerateCode() &&
            lang::verifyMirProgram(frontend.mir).valid(),
        "unknown loop exits must join both ownership states before checking");
    if (!frontend.canGenerateCode())
      for (const auto &diagnostic : frontend.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  for (const std::string_view body :
       {"while (true) { consume(std::move(value)); break; } "
        "return value.get();",
        "while (true) { consume(std::move(value)); } return 0;",
        "while (true) { consume(std::move(value)); continue; } return 0;"}) {
    auto frontend = lang::Frontend().analyze(
        "loop-break-invalid.gti",
        std::string(owner) + "int main() { mut Own value = Own(1); " +
            std::string(body) + " }",
        {library / "prelude.gti"}, {}, {library});
    expect(!frontend.canGenerateCode(),
           "movement still forbids later reads and unrestored loop backedges");
  }
  return failures == 0 ? 0 : 1;
}
