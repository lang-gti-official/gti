#include "cpp_backend_test_support.h"
#include "gti/mir_verifier.h"

#include <filesystem>
#include <iostream>
#include <string_view>

namespace {
int failures = 0;
void check(bool okay, std::string_view message) {
  if (!okay) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
const auto library =
    std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";

lang::FrontendResult analyze(std::string_view source,
                             lang::ConfigurationFlags flags = {}) {
  lang::FrontendOptions options;
  options.configurationFlags = std::move(flags);
  return lang::Frontend(options).analyze(
      "deferred-check.gti", std::string(source), {library / "prelude.gti"}, {},
      {library});
}

void positive() {
  auto result = analyze(R"(
#include <std/assert>
#include <std/string>
struct Value { int number = 7; };
namespace policy { constexpr bool enabled = false; }
namespace app {
  void sink(Value& value) { std::assert(value.number == 7); }
  [[deferred]] void conditional(bool enabled, Value value) {
    if (enabled) { sink(value); }
  }
}
int main() {
  Value value{};
  app::conditional(true, value);
  std::assert(true, "{}", std::string("unused"));
  std::string owner{"owned"};
  if (policy::enabled) { std::string consumed = std::move(owner); }
  std::assert(!owner.empty());
  std::string_view held = owner.view();
  std::assert(held == "owned");
  return 0;
}
)");
  if (!result.canGenerateCode())
    for (const auto &error : result.diagnostics)
      std::cerr << error.code << ": " << error.message << '\n';
  check(result.canGenerateCode(), "deferred helpers lower through HIR/MIR");
  if (!result.canGenerateCode())
    return;
  check(lang::verifyMirProgram(result.mir).valid(),
        "expanded control flow verifies independently");
  for (const auto &instance : result.hir.functionInstances())
    check(!instance.source ||
              !instance.source->hasFunctionAttribute("deferred"),
          "deferred helpers have no eager runtime function instance");
  bool mutatedCaller = false;
  for (const auto &function : result.mir.functionInstances()) {
    if (function.entryKind == lang::ProgramEntryKind::None)
      continue;
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (!instruction.tracksCaller)
          continue;
        auto malformed = result.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &call = *std::find_if(
            changed->body.blocks[block.id - 1].instructions.begin(),
            changed->body.blocks[block.id - 1].instructions.end(),
            [&](const auto &candidate) {
              return candidate.id == instruction.id;
            });
        call.callerLocation.reset();
        check(!lang::verifyMirProgram(malformed).valid(),
              "MIR rejects erased caller authority on an expanded assertion");
        mutatedCaller = true;
        break;
      }
      if (mutatedCaller)
        break;
    }
  }
  check(mutatedCaller,
        "the assertion's terminal edge carries caller authority");
}

void disabledTypeChecks() {
  for (bool disabled : {false, true}) {
    auto result = analyze(R"(
#include <std/assert>
struct Unknown { int number = 7; };
int main() { std::debug_assert(true, "{}", Unknown{}); return 0; }
)",
                          disabled ? lang::ConfigurationFlags{"NDEBUG"}
                                   : lang::ConfigurationFlags{});
    check(!result.canGenerateCode(),
          "disabled assertions still check formatter support");
    check(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                       [](const auto &diagnostic) {
                         return diagnostic.code == "GTI-B0001";
                       }),
          "unsupported formatting is a source diagnostic, not an ICE");
  }
}

void negative(std::string_view source, std::string_view code,
              std::string_view fragment) {
  auto result = analyze(source);
  const auto found = std::find_if(
      result.diagnostics.begin(), result.diagnostics.end(),
      [&](const lang::Diagnostic &diagnostic) {
        return diagnostic.code == code &&
               diagnostic.message.find(fragment) != std::string::npos;
      });
  check(!result.canGenerateCode() && found != result.diagnostics.end(),
        fragment);
  if (result.canGenerateCode() || found == result.diagnostics.end())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  if (found != result.diagnostics.end())
    check(found->primary.source.ends_with("deferred-check.gti") &&
              found->primary.end > found->primary.start,
          "source errors retain a nonempty written source span");
}
} // namespace

int main() {
  positive();
  disabledTypeChecks();
  negative("[[deferred]] int wrong(bool condition) { return 1; }", "GTI-S2099",
           "namespace-scope void");
  negative("[[deferred]] void wrong(bool condition) { return; }", "GTI-S2099",
           "declarations, returns and loops");
  negative("[[deferred, test]] void wrong() {}", "GTI-S2099",
           "namespace-scope void");
  negative(R"(
[[deferred]] void helper(bool value) {}
int main() { return (helper(true), 0); }
)",
           "GTI-S2124", "complete expression statement");
  negative(R"(
#include <std/assert>
int main() { std::debug_assert(1); return 0; }
)",
           "GTI-S2012", "exactly matches");
  negative(R"(
[[deferred]] void helper(bool value) {}
int main() { auto callback = helper; return 0; }
)",
           "GTI-S2124", "not callable values");
  negative(R"(
[[deferred]] void execute<F>(F callback) { callback(); }
int main() { execute([]() -> void {}); return 0; }
)",
           "GTI-S2124", "requires a named binding");
  negative(R"(
[[deferred]] void helper() { helper(); }
int main() { helper(); return 0; }
)",
           "GTI-S2124", "resource limit");
  negative(R"(
#include <std/string>
void consume(std::string value) {}
[[deferred]] void twice(std::string value) { consume(value); consume(value); }
int main() { std::string owner{"owned"}; twice(std::move(owner)); return 0; }
)",
           "GTI-S2018", "moved");
  negative(R"(
#include <std/string>
void consume(std::string value) {}
[[deferred]] void choose(bool enabled, std::string value) {
  if (enabled) { consume(value); }
}
int main() {
  std::string owner{"owned"};
  std::string_view borrowed = owner.view();
  choose(true, std::move(owner));
  return int(borrowed.size());
}
)",
           "GTI-S2017", "borrow");
  negative(R"(
unsafe bool inspect() { return true; }
[[deferred]] void ignored(bool value) {}
int main() { ignored(inspect()); return 0; }
)",
           "GTI-S2055", "unsafe");
  return failures ? 1 : 0;
}
