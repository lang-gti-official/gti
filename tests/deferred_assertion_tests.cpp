#include "cpp_backend_test_support.h"
#include "gti/ast_clone.h"
#include "gti/lexer.h"
#include "gti/mir_editor.h"
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
  #macro conditional(bool enabled, Value value) {
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
  check(result.canGenerateCode(), "typed macros lower through HIR/MIR");
  if (!result.canGenerateCode())
    return;
  check(lang::verifyMirProgram(result.mir).valid(),
        "expanded control flow verifies independently");
  for (const auto &instance : result.hir.functionInstances())
    check(!instance.source || !instance.source->isMacro(),
          "typed macros have no eager runtime function instance");
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
#macro force(bool value) { observe(value); }
#macro relay(bool value) { force(value); }
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
          "generic macro constructor temporaries produce checked HIR");
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
  // The temporary HirLowerer and FrontendResult are gone. The copied HIR's
  // exact shared roots, not the semantic models, keep these sources alive.
  check(retained.sourceStatementOwners().size() >= 4,
        "nested ordinary and concrete macro roots are retained by HIR");
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

void typedMacroSyntax() {
  auto result = analyze(R"(
#include <std/assert>
namespace application {
  void accept(int number) { std::assert(number == 7); }
  void accept(bool condition) { std::assert(condition); }
  #macro emit(int number = 7) { accept(number); }
  #macro emit(bool condition) { accept(condition); }
  #macro when<T>(bool enabled, T value) {
    if (enabled) { emit(value); }
  }
  #macro numeric<T>(T value) requires std::numeric<T> {
    std::assert(value == T(7));
  }
  [[track_caller]] #macro ensure(bool condition) { std::assert(condition); }
}
int main() {
  int accept = 99;
  application::emit();
  application::emit(true);
  application::when<int>(true, 7);
  application::when(false, false);
  application::numeric(7);
  application::ensure(accept == 99);
  return 0;
}
)");
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  check(result.canGenerateCode(), "typed macro overloads, defaults, generics, "
                                  "attributes and qualified calls");
  if (result.canGenerateCode())
    check(lang::verifyMirProgram(result.mir).valid(),
          "typed macro syntax preserves independently verified execution");
  check(std::any_of(result.semantics.database().symbols().begin(),
                    result.semantics.database().symbols().end(),
                    [](const auto &symbol) {
                      return symbol.kind == lang::SymbolKind::Macro;
                    }),
        "semantic symbols retain macro declaration identity for tooling");

  for (std::string_view source :
       {"#macro broken(int value { }\nint main() { return 0; }",
        "#macro broken(\nint main() { return 0; }",
        "#macro broken(int value) { if (value > 0) { }\n"}) {
    auto malformed = analyze(source);
    check(!malformed.canGenerateCode() &&
              std::any_of(malformed.diagnostics.begin(),
                          malformed.diagnostics.end(),
                          [](const auto &error) {
                            return error.code.starts_with("GTI-P");
                          }),
          "incomplete macro syntax reports a parser error without an ICE");
    check(std::none_of(
              malformed.diagnostics.begin(), malformed.diagnostics.end(),
              [](const auto &error) { return error.code == "GTI-B0001"; }),
          "incomplete macro syntax never enters backend integrity diagnostics");
  }
}

void migrationAndOverloadDiagnostics() {
  constexpr std::string_view legacy = "[[deferred]] void old() {}";
  const auto migrated = analyze(legacy);
  const auto oldAttribute = std::find_if(
      migrated.diagnostics.begin(), migrated.diagnostics.end(),
      [](const auto &diagnostic) {
        return diagnostic.code == "GTI-S2099" &&
               diagnostic.message.find("removed") != std::string::npos;
      });
  check(oldAttribute != migrated.diagnostics.end(),
        "removed deferred attributes receive a migration diagnostic");
  if (oldAttribute != migrated.diagnostics.end()) {
    check(oldAttribute->primary.start == legacy.find("deferred") &&
              oldAttribute->primary.end == legacy.find("deferred") + 8,
          "legacy migration identifies the exact attribute name");
    check(std::any_of(oldAttribute->hints.begin(), oldAttribute->hints.end(),
                      [](const auto &hint) {
                        return hint.find("#macro") != std::string::npos &&
                               hint.find("without a return type") !=
                                   std::string::npos;
                      }),
          "legacy migration explains macro syntax and implicit void result");
    check(oldAttribute->fixes.empty(),
          "migration never rewrites a declaration through an unsafe fix-it");
  }

  for (std::string_view source :
       {"#macro same(int value) {}\nvoid same(bool value) {}",
        "void same(bool value) {}\n#macro same(int value) {}"}) {
    const auto mixed = analyze(source);
    const auto diagnostic =
        std::find_if(mixed.diagnostics.begin(), mixed.diagnostics.end(),
                     [](const auto &error) {
                       return error.code == "GTI-S2099" &&
                              error.message.find("evaluation rules differ") !=
                                  std::string::npos;
                     });
    check(diagnostic != mixed.diagnostics.end(),
          "mixed macro/function names fail in either declaration order");
    if (diagnostic == mixed.diagnostics.end())
      continue;
    check(diagnostic->primary.start == source.rfind("same") &&
              diagnostic->primary.end == source.rfind("same") + 4,
          "mixed-name error identifies the later declaration");
    check(std::any_of(diagnostic->related.begin(), diagnostic->related.end(),
                      [&](const auto &related) {
                        return related.span.start == source.find("same") &&
                               related.span.end == source.find("same") + 4;
                      }),
          "mixed-name error relates the original callable declaration");
  }
}

void localBindingsAndLoops() {
  auto result = analyze(R"(
#include <std/assert>
#include <std/string>
namespace library {
  struct Item { int code = 1; };
  #macro once(bool enabled, int expression) {
    if (enabled) {
      int saved = expression;
      auto& read = saved;
      std::assert(read == saved);
    }
  }
  #macro shadow(int value) {
    { int value = 4; std::assert(value == 4); }
    std::assert(value == 9);
  }
  #macro typed() { Item item{}; std::assert(item.code == 1); }
  #macro own(bool enabled, std::string expression) {
    if (enabled) {
      std::string local = expression;
      auto& borrowed = local;
      std::assert(!borrowed.empty());
    }
  }
  #macro loops(int count) {
    mut int total = 0;
    for (mut int index = 0; index < count; index++) {
      if (index == 1) { continue; }
      total += index;
      if (index == 3) { break; }
    }
    while (total > 1) { total--; }
    do { total++; } while (total < 2);
    int values[3] = {1, 2, 3};
    for (auto& value : values) { std::assert(value > 0); }
  }
  #macro assign(int destination, int value) { destination = value; }
  #macro defaults(int expression = 9) {
    int expression = expression;
    std::assert(expression == 9);
  }
  #macro decompose() {
    int values[2] = {1, 2};
    auto [first, second] = values;
    std::assert(first == 1 && second == 2);
  }
  #macro check_lambda(bool condition) { std::assert(condition); }
}
namespace application {
  struct Item { int code = 2; };
  int run() {
    int saved = 9;
    mut int evaluations = 0;
    library::once(false, evaluations++);
    library::once(true, saved);
    library::once(true, saved);
    library::shadow(saved);
    library::typed();
    library::loops(5);
    library::defaults();
    library::decompose();
    library::decompose();
    library::check_lambda([](int count) -> bool {
      { { { int count = 2; return count == 2; } } }
    }(1));
    library::check_lambda([]() -> bool {
      std::assert(true);
      { { { int own_local = 1; return own_local == 1; } } }
    }());
    mut int output = 0;
    library::assign(output, saved);
    std::string owner{"kept"};
    library::own(false, std::move(owner));
    std::assert(!owner.empty());
    library::own(true, std::move(owner));
    return 0;
  }
}
int main() { return application::run(); }
)");
  if (!result.canGenerateCode())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  check(result.canGenerateCode(),
        "macro locals, references, moves and scoped loops lower through MIR");
  if (result.canGenerateCode())
    check(lang::verifyMirProgram(result.mir).valid(),
          "macro-local ownership and control flow verify independently");
}

void feasibleLifecycleSources() {
  const auto result = analyze(R"(
#include <std/string>
struct Owned {
  std::string text;
  Owned() : text(std::string("owned")) {}
  bool empty() { return this.text.empty(); }
};
#macro own<T>(bool active, T expression) {
  if (active) { T local = expression; }
}
int main() {
  Owned owner{};
  bool dynamic = owner.empty();
  own(false, std::move(owner));
  own(true, std::move(owner));
  return dynamic ? 1 : 0;
}
)");
  check(result.canGenerateCode(),
        "literal feasibility refines a generic owning local's move source");
  if (!result.canGenerateCode())
    return;
  const auto entry = std::find_if(
      result.mir.functionInstances().begin(),
      result.mir.functionInstances().end(), [](const auto &function) {
        return function.entryKind != lang::ProgramEntryKind::None;
      });
  check(entry != result.mir.functionInstances().end(),
        "lifecycle mutation fixture has a program entry");
  if (entry == result.mir.functionInstances().end())
    return;
  const auto rejects = [&](const lang::MirBody &body, std::string_view reason) {
    const auto verification = lang::verifyMirBody(body, entry->id);
    const bool matched =
        std::any_of(verification.errors.begin(), verification.errors.end(),
                    [&](const auto &error) {
                      return error.message.find(reason) != std::string::npos;
                    });
    check(!verification.valid() && matched, reason);
    if (verification.valid() || !matched)
      for (const auto &error : verification.errors)
        std::cerr << error.message << '\n';
  };
  auto changedLiteral = entry->body;
  lang::MirValueId literal = 0;
  std::optional<lang::MirOperand> dynamic;
  for (auto &block : changedLiteral.blocks)
    for (auto &instruction : block.instructions) {
      if (!dynamic && instruction.kind == lang::MirInstructionKind::Call &&
          instruction.info.type == lang::SemanticType::Bool &&
          instruction.result)
        dynamic = lang::MirOperand{.kind = lang::MirOperandKind::Value,
                                   .value = *instruction.result,
                                   .type = lang::SemanticType::Bool};
      if (!literal && instruction.operation == lang::MirOperation::Literal &&
          instruction.literal &&
          std::holds_alternative<bool>(*instruction.literal) &&
          !std::get<bool>(*instruction.literal)) {
        literal = instruction.result.value_or(0);
        instruction.literal = true;
      }
    }
  check(literal != 0 && dynamic.has_value(),
        "fixture contains independent literal and dynamic condition producers");
  if (literal) {
    rejects(changedLiteral, "requires obligation");
    auto unknown = entry->body;
    bool changed = false;
    for (auto &block : unknown.blocks)
      if (dynamic && block.terminator.kind == lang::MirTerminatorKind::Branch &&
          block.terminator.value &&
          block.terminator.value->kind == lang::MirOperandKind::Value &&
          block.terminator.value->value == literal) {
        block.terminator.value = *dynamic;
        changed = true;
      }
    check(changed, "unknown-condition mutation changes the guarded move edge");
    if (changed) {
      check(lang::rebuildMirValueUses(unknown),
            "unknown-condition mutation repairs only its SSA use index");
      rejects(unknown, "requires obligation");
    }
  }
  auto wrongCleanup = entry->body;
  bool changed = false;
  for (auto &block : wrongCleanup.blocks) {
    for (auto &instruction : block.instructions) {
      if (changed)
        break;
      for (auto &event : instruction.lifecycle)
        if (event.kind == lang::MirLifecycleEventKind::Drop) {
          event.conditional = !event.conditional;
          changed = true;
          break;
        }
    }
  }
  check(changed, "cleanup mutation has a published drop");
  if (changed)
    rejects(wrongCleanup, "conditionality");
}

void parserMacroRecovery() {
  for (const bool nested : {false, true}) {
    const std::string source =
        std::string(nested ? "namespace recovered {\n" : "") +
        "?\n#macro retained(bool value) {}\n" + (nested ? "}\n" : "") +
        "int main() { return 0; }\n";
    lang::Lexer lexer;
    lang::Parser parser(lexer.scan(source, "macro-recovery.gti"), source);
    const auto parsed = parser.parse();
    check(parser.hadError(),
          "the preceding malformed declaration is diagnosed");
    const lang::StmtList *declarations = &parsed.declarations();
    if (nested && !declarations->empty()) {
      const auto *scope =
          lang::stmtAs<lang::NamespaceDecl>(declarations->front().get());
      check(scope != nullptr,
            "declaration recovery preserves the containing namespace");
      declarations = scope ? &scope->declarations() : nullptr;
    }
    const auto *macro =
        declarations && !declarations->empty()
            ? lang::stmtAs<lang::FunctionDecl>(declarations->front().get())
            : nullptr;
    check(macro && macro->isMacro() && macro->name().lexeme == "retained",
          "declaration recovery retains a following typed macro");
    if (macro && macro->isMacro()) {
      check(macro->macroKeyword()->lexeme == "#macro" &&
                !macro->macroKeyword()->generated &&
                macro->macroKeyword()->position == source.find("#macro"),
            "recovered macros retain the real keyword identity and span");
      check(macro->returnType().name.last().kind == lang::TokenKind::VOID &&
                macro->returnType().name.last().generated,
            "a recovered macro keeps its implicit return token generated");
    }
  }
  for (std::string_view source :
       {"int main() { #macro local() {} return 0; }",
        "class Owner { #macro member() {} };",
        "class Owner { [[track_caller]] #macro member() {} };"}) {
    lang::Lexer lexer;
    lang::Parser parser(lexer.scan(std::string(source)), source);
    (void)parser.parse();
    check(parser.hadError(),
          "macro declaration recovery does not admit block or class macros");
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
  expansionSourceOwnership();
  typedMacroSyntax();
  localBindingsAndLoops();
  feasibleLifecycleSources();
  migrationAndOverloadDiagnostics();
  parserMacroRecovery();
  negative("[[deferred]] void old(bool condition) {}", "GTI-S2099", "#macro");
  check(analyze("#macro stop() { return; } int main() { stop(); return 0; }")
            .canGenerateCode(),
        "void macro-local return resumes its caller");
  negative("#macro wrong() { break; }", "GTI-S2099", "within that macro");
  negative("#macro wrong() { continue; }", "GTI-S2099", "within that macro");
  negative("#macro wrong() { static int value = 1; }", "GTI-P0001",
           "Block-scope static");
  negative("[[test]] #macro wrong() {}", "GTI-S2099",
           "namespace-scope template");
  negative(R"(
namespace app {
  #macro same(int value) {}
  void same(bool value) {}
}
)",
           "GTI-S2099", "ordinary");
  negative(R"(
namespace app {
  void same(bool value) {}
  #macro same(int value) {}
}
)",
           "GTI-S2099", "ordinary");
  negative("#macro exact(int value) {}\nint main() { exact(true); return 0; }",
           "GTI-S2003", "parameter requires 'int32_t'");
  negative(R"(
#macro helper(bool value) {}
int main() { return (helper(true), 0); }
)",
           "GTI-S2124", "complete expression statement");
  negative(R"(
#include <std/assert>
int main() { std::debug_assert(1); return 0; }
)",
           "GTI-S2012", "exactly matches");
  negative(R"(
#macro helper(bool value) {}
int main() { auto callback = helper; return 0; }
)",
           "GTI-S2124", "not callable values");
  check(analyze(R"(
#macro execute<F>(F callback) { callback(); }
int main() { execute([]() -> void {}); return 0; }
)")
            .canGenerateCode(),
        "fresh callable expressions are valid macro arguments");
  negative(R"(
#macro helper() { helper(); }
int main() { helper(); return 0; }
)",
           "GTI-S2124", "resource limit");
  negative(R"(
#include <std/string>
void consume(std::string value) {}
#macro twice(std::string value) { consume(value); consume(value); }
int main() { std::string owner{"owned"}; twice(std::move(owner)); return 0; }
)",
           "GTI-S2018", "moved");
  negative(R"(
#include <std/string>
void consume(std::string value) {}
#macro twice(std::string expression) {
  std::string owned = expression;
  consume(std::move(owned));
  consume(std::move(owned));
}
int main() { twice(std::string("owned")); return 0; }
)",
           "GTI-S2018", "moved");
  negative(R"(
#include <std/string>
void consume(std::string value) {}
#macro invalid() {
  std::string owned{"owned"};
  std::string_view held = owned.view();
  consume(std::move(owned));
  [[discard]] held.size();
}
int main() { invalid(); return 0; }
)",
           "GTI-S2017", "borrow");
  negative(R"(
#include <std/string>
#macro escape(std::string_view destination) {
  std::string local{"owned"};
  destination = local.view();
}
int main() {
  mut std::string_view output = "safe";
  escape(output);
  return int(output.size());
}
)",
           "GTI-S2045", "cannot be assigned");
  negative(R"(
#include <std/string>
void consume(std::string value) {}
#macro loop(std::string expression) {
  for (mut int i = 0; i < 2; ++i) { consume(expression); }
}
int main() {
  std::string owner{"owned"};
  loop(std::move(owner));
  return 0;
}
)",
           "GTI-S2018", "loop backedge");
  negative("#macro assign(int destination) { destination = 7; }\n"
           "int main() { assign(4); return 0; }",
           "GTI-S2002", "mutable place");
  negative("#macro invalid() { break; }\n"
           "int main() { while (true) { invalid(); } return 0; }",
           "GTI-S2099", "within that macro");
  negative("#macro invalid() { continue; }\n"
           "#macro outer() { while (true) { invalid(); } }\n"
           "int main() { outer(); return 0; }",
           "GTI-S2099", "within that macro");
  negative(R"(
#include <std/string>
void consume(std::string value) {}
#macro choose(bool enabled, std::string value) {
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
#macro ignored(bool value) {}
int main() { ignored(inspect()); return 0; }
)",
           "GTI-S2055", "unsafe");
  return failures ? 1 : 0;
}
