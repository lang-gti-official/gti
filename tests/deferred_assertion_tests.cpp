#include "cpp_backend_test_support.h"
#include "gti/ast_clone.h"
#include "gti/mir_verifier.h"

#include <filesystem>
#include <iostream>
#include <string_view>
#include <unordered_set>

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

lang::FrontendResult
analyze(std::string_view source, lang::ConfigurationFlags flags = {},
        lang::FrontendPhase stopAfter = lang::FrontendPhase::Mir) {
  lang::FrontendOptions options;
  options.configurationFlags = std::move(flags);
  options.stopAfter = stopAfter;
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
int impossible() { std::assert(false); }
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

void expansionSourceOwnership() {
  lang::HirProgram retained;
  std::unordered_set<lang::HirFunctionInstanceId> expectedConstructors;
  {
    auto result = analyze(R"(
struct Cleanup { ~Cleanup() {} };
bool inspect(Cleanup& value) { return true; }
void observe(bool value) {}
[[deferred]] void force(bool value) { observe(value); }
[[deferred]] void relay(bool value) { force(value); }
int ordinary() { relay(inspect(Cleanup())); return 0; }
int selected<uint64_t Mode>() {
  if constexpr (Mode == 0) { return 0; }
  else { relay(inspect(Cleanup())); return 1; }
}
int main() { return ordinary() + selected<0>() + selected<1>() - 1; }
)",
                          {}, lang::FrontendPhase::Hir);
    if (!result.semanticValid || !result.hirValid)
      for (const auto &diagnostic : result.diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    check(result.semanticValid && result.hirValid,
          "generic deferred constructor temporaries produce checked HIR");
    if (!result.semanticValid || !result.hirValid)
      return;
    for (const auto &function : result.hir.functionInstances())
      if (function.source && (function.source->name().lexeme == "ordinary" ||
                              (function.source->name().lexeme == "selected" &&
                               !function.valueArguments.empty() &&
                               function.valueArguments.front().value == 1)))
        expectedConstructors.insert(function.id);
    retained = result.hir;
  }
  // Both the temporary HirLowerer and the FrontendResult are gone. Only the
  // copied HIR's exact shared roots may keep instance-local source alive.
  check(retained.sourceStatementOwners().size() >= 4,
        "nested ordinary and concrete deferred roots are retained by HIR");
  std::unordered_set<const lang::Expr *> ownedExpressions;
  for (const auto &root : retained.sourceStatementOwners()) {
    lang::AstCloneOptions inspection;
    inspection.expression = [&](const lang::Expr &expression) -> lang::ExprPtr {
      ownedExpressions.insert(&expression);
      return nullptr;
    };
    const auto inspected = lang::cloneStmt(*root, 0, inspection);
    check(inspected.statement != nullptr,
          "retained expansion descendants remain readable after lowering");
  }
  std::unordered_set<lang::HirFunctionInstanceId> constructors;
  for (const auto &function : retained.functionInstances())
    for (const auto &value : function.body.values) {
      if (!ownedExpressions.contains(value.source))
        continue;
      const auto *call = lang::exprAs<lang::Call>(value.source);
      const auto *callee =
          call == nullptr ? nullptr
                          : lang::exprAs<lang::Variable>(call->callee().get());
      if (callee && callee->name().lexeme == "Cleanup") {
        check(call->arguments().empty(),
              "a retained constructor source has its exact argument list");
        constructors.insert(function.id);
      }
    }
  check(expectedConstructors.size() == 2 &&
            constructors == expectedConstructors,
        "ordinary and concrete temporary call sources outlive their analyses");
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
  expansionSourceOwnership();
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
