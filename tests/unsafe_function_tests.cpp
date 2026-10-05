#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"

#include "cpp_backend_test_support.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult analyze(std::string source, bool concurrent = false) {
  const auto stdlib =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  lang::FrontendOptions options;
  if (concurrent) {
    options.target.executionProfile = lang::ExecutionProfile::Concurrent;
    options.target.runtimeCapabilities.threads = true;
  }
  return lang::Frontend(options).analyze(
      "unsafe-function.gti", std::move(source), {stdlib / "prelude.gti"}, {},
      {stdlib});
}

bool hasCode(const lang::FrontendResult &result, std::string_view code) {
  return std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(),
      [&](const auto &diagnostic) { return diagnostic.code == code; });
}

void expectRejected(std::string source, std::string_view code,
                    std::string_view message, bool concurrent = false) {
  const auto result = analyze(std::move(source), concurrent);
  if (result.canGenerateCode() || !hasCode(result, code)) {
    for (const auto &diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  expect(!result.canGenerateCode() && hasCode(result, code), message);
}

void testCallsAndTooling() {
  const std::string source = R"(
namespace raw {
unsafe int32_t read(const int32_t* pointer) {
  unsafe { return *pointer; }
}
unsafe static T identity<T>(T value) { return value; }
unsafe int32_t select(int32_t value) { return value; }
bool select(bool value) { return value; }
}
int main() {
  if (!raw::select(true)) { return 1; }
  int32_t value = 42;
  unsafe {
    return raw::identity(raw::select(raw::read(&value))) == 42 ? 0 : 2;
  }
}
)";
  const auto frontend = analyze(source);
  for (const auto &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(),
         "namespace, static, generic and overloaded unsafe calls");
  if (!frontend.canGenerateCode()) {
    return;
  }
  std::size_t unsafeFunctions = 0;
  std::size_t unsafeCalls = 0;
  for (const auto &function : frontend.mir.functionInstances()) {
    if (function.requiresUnsafe) {
      ++unsafeFunctions;
      const auto *semantic =
          frontend.semantics.findFunction(function.declaration);
      const auto *hir = frontend.hir.findFunctionInstance(function.id);
      expect(semantic && semantic->requiresUnsafe && hir && hir->requiresUnsafe,
             "the declaration contract survives semantics, HIR and MIR");
    }
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.unsafeOperation ==
                lang::UnsafeOperationKind::DeclaredUnsafeCall) {
          ++unsafeCalls;
        }
      }
    }
  }
  expect(
      unsafeFunctions == 3 && unsafeCalls == 3,
      "only exact unsafe overloads and concrete instances carry the contract");
  const auto dump = lang::MirPrinter().print(frontend.mir);
  expect(dump.starts_with("mir-v88 ") &&
             dump.find("requires-unsafe=1") != std::string::npos,
         "the deterministic MIR snapshot includes the unsafe contract");
  const auto lowered =
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir);
  expect(lang::verifyLoweredProgram(lowered).empty(),
         "the detached lowered program verifies");
  auto forgedLowered = lowered;
  bool changedDeclaration = false;
  for (auto &declaration : const_cast<std::vector<lang::LoweredDeclaration> &>(
           forgedLowered.declarations())) {
    auto *function =
        std::get_if<lang::LoweredFunctionDeclaration>(&declaration.payload);
    if (function && function->requiresUnsafe) {
      function->requiresUnsafe = false;
      changedDeclaration = true;
      break;
    }
  }
  expect(
      changedDeclaration && !lang::verifyLoweredProgram(forgedLowered).empty(),
      "detached lowered verification rejects an erased declaration contract");
  const auto cpp = gti_test::emitCppText(frontend);
  for (const auto &function : frontend.mir.functionInstances()) {
    if (!function.requiresUnsafe) {
      continue;
    }
    const auto identity = " function-instance " + std::to_string(function.id);
    expect(cpp.find("// GTI verified-MIR body: scalar-cfg-v1" + identity) !=
                   std::string::npos ||
               cpp.find("// GTI verified-MIR body: scalar-cfg-failure-v1" +
                        identity) != std::string::npos,
           "every unsafe target is emitted by its exact production MIR body");
  }
  const auto hover =
      lang::LanguageQueries().hover(frontend, frontend.sourceGraph.entryUnit(),
                                    source.find("raw::read(&") + 6);
  expect(hover && hover->signature.starts_with("unsafe "),
         "shared frontend hover exposes the caller contract");
  const auto formatted = lang::Formatter().format(source);
  expect(formatted.find("unsafe") != std::string::npos &&
             lang::Formatter().format(formatted) == formatted &&
             analyze(formatted).canGenerateCode(),
         "formatting preserves the keyword and is idempotent");

  auto forgedHir = frontend.hir;
  for (auto &function : const_cast<std::vector<lang::HirFunctionInstance> &>(
           forgedHir.functionInstances())) {
    if (function.requiresUnsafe) {
      function.requiresUnsafe = false;
      break;
    }
  }
  expect(!lang::verifyHirProgramPlans(frontend.semantics, forgedHir).valid(),
         "HIR rejects a removed declaration contract");

  auto forgedMir = frontend.mir;
  bool changed = false;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           forgedMir.functionInstances())) {
    for (auto &block : function.body.blocks) {
      for (auto &instruction : block.instructions) {
        if (!changed && instruction.kind == lang::MirInstructionKind::Call &&
            instruction.unsafeOperation ==
                lang::UnsafeOperationKind::DeclaredUnsafeCall) {
          instruction.unsafeOperation = lang::UnsafeOperationKind::None;
          changed = true;
        }
      }
    }
  }
  expect(changed && !lang::verifyMirProgram(forgedMir).valid(),
         "MIR rejects an unsafe call with an erased marker");

  forgedMir = frontend.mir;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           forgedMir.functionInstances())) {
    if (function.requiresUnsafe) {
      function.requiresUnsafe = false;
      break;
    }
  }
  expect(!lang::verifyMirProgram(forgedMir).valid(),
         "MIR rejects an unsafe call whose target lost its contract");
  const auto drift = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, forgedMir, forgedMir,
      {}, lang::TargetInfo::host());
  expect(!drift.valid(),
         "lowered construction rejects unsafe declaration drift");
}

void testDiagnosticsAndBoundaries() {
  const std::string source = "unsafe int32_t raw() { return 1; }\n"
                             "int main() { return raw(); }\n";
  const auto result = analyze(source);
  const auto found = std::find_if(
      result.diagnostics.begin(), result.diagnostics.end(),
      [](const auto &diagnostic) { return diagnostic.code == "GTI-S2055"; });
  expect(!result.canGenerateCode() && found != result.diagnostics.end(),
         "safe calls are rejected before backend entry");
  if (found != result.diagnostics.end()) {
    const auto &diagnostic = *found;
    expect(diagnostic.primary.source ==
                   std::filesystem::absolute("unsafe-function.gti").string() &&
               diagnostic.primary.start == source.rfind("raw()") + 4 &&
               diagnostic.primary.end == diagnostic.primary.start + 1 &&
               diagnostic.message.find("unsafe function") !=
                   std::string::npos &&
               !diagnostic.hints.empty() && diagnostic.fixes.empty() &&
               diagnostic.related.size() == 1 &&
               diagnostic.related[0].span.start == 0 &&
               diagnostic.related[0].span.end == 6,
           "unsafe diagnostic has exact call and keyword spans, context and "
           "no unsafe fix-it");
  }
  expectRejected("unsafe int32_t raw(const int32_t* p) { return *p; }",
                 "GTI-S2055", "the declaration does not make its body unsafe");
  expectRejected("unsafe int32_t raw() { return 1; }\n"
                 "int main() { unsafe { auto f = []() -> int32_t { return "
                 "raw(); }; return f(); } }",
                 "GTI-S2055", "lambdas do not inherit outer unsafe permission");
  expectRejected("unsafe T raw<T>(T value) { return value; }\n"
                 "int main() { return raw(1); }",
                 "GTI-S2055",
                 "generic specialization retains its unsafe requirement");
  expectRejected("unsafe int main() { return 0; }", "GTI-S2055",
                 "hosted main cannot impose a caller contract");
  expectRejected(
      "using Callback = () -> int32_t;\n"
      "unsafe int32_t raw() { return 1; }\n"
      "int main() { unsafe { Callback f = raw; } return 0; }",
      "GTI-S2076",
      "even an unsafe block cannot erase the contract in a callback");
  expectRejected("#include <std/functional>\n"
                 "unsafe void raw() {}\n"
                 "int main() { unsafe { std::invoke_once(raw); } return 0; }",
                 "GTI-S2090",
                 "a confined callable cannot erase the unsafe contract");
  expectRejected("#include <std/thread>\n"
                 "unsafe void raw() {}\n"
                 "int main() { unsafe { auto task = std::thread::start(raw); } "
                 "return 0; }",
                 "GTI-S2090", "a managed task cannot erase the unsafe contract",
                 true);
  for (const std::string declaration :
       {"unsafe int32_t value = 0;", "unsafe class Value {};",
        "extern \"C\" { unsafe void raw(); }",
        "class Value { public: unsafe void raw() {} };",
        "[[unsafe, unsafe]] void raw() {}", "unsafe unsafe void raw() {}"}) {
    expectRejected(declaration + "\nint main() { return 0; }", "GTI-P0001",
                   "invalid unsafe modifier placement is a parser diagnostic");
  }
}

void testKeywordMigrationAndUnusedBlocks() {
  const std::string obsolete =
      "[[unsafe]] int32_t raw() { return 1; }\nint main() { return 0; }\n";
  const auto old = analyze(obsolete);
  const auto migration = std::find_if(
      old.diagnostics.begin(), old.diagnostics.end(),
      [](const auto &diagnostic) {
        return diagnostic.code == "GTI-P0001" &&
               diagnostic.message.find("replaced") != std::string::npos;
      });
  expect(!old.canGenerateCode() && migration != old.diagnostics.end(),
         "the old attribute spelling is a syntax error, not a permanent alias");
  if (migration != old.diagnostics.end()) {
    expect(migration->primary.start == 0 && migration->primary.end == 10 &&
               migration->fixes.size() == 1 &&
               migration->fixes.front().replacement == "unsafe",
           "contiguous legacy attributes have an exact spelling-only fix");
    const auto &fix = migration->fixes.front();
    std::string corrected = obsolete;
    corrected.replace(fix.span.start, fix.span.end - fix.span.start,
                      fix.replacement);
    expect(analyze(corrected).canGenerateCode(),
           "the spelling migration preserves the callable contract");
  }
  const auto commented = analyze(
      "[[/*retain this*/unsafe]] void raw() {}\nint main() { return 0; }");
  expect(!commented.canGenerateCode() &&
             std::all_of(commented.diagnostics.begin(),
                         commented.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.fixes.empty();
                         }),
         "migration does not delete comments inside a legacy attribute");

  for (const std::string adjacent :
       {"[[unsafe]]int32_t raw() { return 1; }\nint main() { return 0; }",
        "[[unsafe]]static int32_t raw() { return 1; }\nint main() { return 0; "
        "}"}) {
    const auto recovered = analyze(adjacent);
    const auto diagnostic =
        std::find_if(recovered.diagnostics.begin(), recovered.diagnostics.end(),
                     [](const auto &entry) { return !entry.fixes.empty(); });
    expect(!recovered.canGenerateCode() &&
               diagnostic != recovered.diagnostics.end(),
           "unspaced legacy attributes still offer a migration edit");
    if (diagnostic != recovered.diagnostics.end()) {
      const auto &fix = diagnostic->fixes.front();
      std::string corrected = adjacent;
      corrected.replace(fix.span.start, fix.span.end - fix.span.start,
                        fix.replacement);
      expect(fix.replacement == "unsafe " &&
                 analyze(corrected).canGenerateCode(),
             "migration inserts a separator before an adjacent type/modifier");
    }
  }

  const auto modifiers =
      analyze("[[noreturn]] unsafe static void stop() { while (true) {} }\n"
              "unsafe static constexpr int32_t number() { return 7; }\n"
              "int main() { unsafe { return number(); } }");
  expect(modifiers.canGenerateCode(), "contract attributes precede canonical "
                                      "unsafe/static/constexpr modifiers");
  expectRejected("unsafe void bad;\nint main() { return 0; }", "GTI-P0001",
                 "the modifier does not admit unsafe variables");
  expectRejected("unsafe static unsafe void bad() {}", "GTI-P0001",
                 "misordered or repeated modifiers are rejected");

  const std::string unnecessary =
      "int main() { unsafe { int32_t value = 7; return value; } }";
  const auto unused = analyze(unnecessary);
  const auto warning = std::find_if(
      unused.diagnostics.begin(), unused.diagnostics.end(),
      [](const auto &diagnostic) { return diagnostic.code == "GTI-S2100"; });
  expect(unused.canGenerateCode() && warning != unused.diagnostics.end(),
         "an unused unsafe block warns without changing validity");
  if (warning != unused.diagnostics.end()) {
    expect(
        warning->severity == lang::DiagnosticSeverity::Warning &&
            warning->group == "unused-unsafe" &&
            warning->primary.start == unnecessary.find("unsafe") &&
            warning->primary.end == warning->primary.start + 6 &&
            warning->fixes.size() == 1 &&
            warning->fixes.front().replacement.empty() &&
            lang::WarningPolicy({"unused-unsafe"}).suppresses(*warning),
        "the warning has a keyword span, shared policy and safe removal fix");
    const auto &fix = warning->fixes.front();
    std::string corrected = unnecessary;
    corrected.erase(fix.span.start, fix.span.end - fix.span.start);
    const auto fixed = analyze(corrected);
    expect(fixed.canGenerateCode() && !hasCode(fixed, "GTI-S2100"),
           "removing only the keyword preserves braces, scope and cleanup");
  }
  const auto nested = analyze("int main() { int32_t value = 7; unsafe { unsafe "
                              "{ return *(&value); } } }");
  expect(nested.canGenerateCode() &&
             std::count_if(nested.diagnostics.begin(), nested.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2100";
                           }) == 1,
         "a nested block discharges its own obligations, not its parent's");
  const auto addressOnly = analyze(
      "int main() { int32_t value = 7; unsafe { const int32_t* p = &value; "
      "return p == nullptr ? 1 : 0; } }");
  expect(addressOnly.canGenerateCode() && hasCode(addressOnly, "GTI-S2100"),
         "address formation alone does not consume unsafe permission");
  const auto lambda = analyze(
      "int main() { unsafe { auto read = []() -> int32_t { int32_t value = 7; "
      "unsafe { return *(&value); } }; return read(); } }");
  expect(lambda.canGenerateCode() && hasCode(lambda, "GTI-S2100"),
         "a lambda's own unsafe block cannot use its enclosing permission");
  const auto generic =
      analyze("T relay<T>(T value) { unsafe { return value; } }\n"
              "int main() { return relay(0); }");
  expect(generic.canGenerateCode() && !hasCode(generic, "GTI-S2100"),
         "symbolic generic bodies do not produce premature removal advice");
}

void testCallbackMutation() {
  const auto frontend =
      analyze("using Callback = () -> int32_t;\n"
              "int32_t safe() { return 1; }\n"
              "int main() { Callback f = safe; return f == nullptr ? 1 : 0; }");
  expect(frontend.canGenerateCode() &&
             frontend.mir.nativeCallbackAdapters().size() == 1,
         "safe callback control remains valid");
  if (!frontend.canGenerateCode() ||
      frontend.mir.nativeCallbackAdapters().empty()) {
    return;
  }
  auto forged = frontend.mir;
  const auto target = forged.nativeCallbackAdapters().front().target;
  for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
           forged.functionInstances())) {
    if (function.id == target) {
      function.requiresUnsafe = true;
    }
  }
  expect(!lang::verifyMirProgram(forged).valid(),
         "MIR rejects a forged unsafe native callback target");
}

void testAdditionalCallForms() {
  const auto frontend = analyze(R"(
unsafe constexpr int32_t constant() { return 7; }
unsafe void action(int32_t value = 1) {}
unsafe const int32_t* relay(const int32_t* value) { return value; }
int main() {
  unsafe {
    action();
    const int32_t* value = relay(nullptr);
    return value == nullptr && constant() == 7 ? 0 : 1;
  }
}
)");
  for (const auto &diagnostic : frontend.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(frontend.canGenerateCode(),
         "constexpr, default arguments, void and pointer return calls");
  if (frontend.canGenerateCode()) {
    expect(!gti_test::emitCppText(frontend).empty(),
           "additional unsafe forms reach the MIR backend");
  }
  expectRejected("unsafe constexpr int32_t constant() { return 7; }\n"
                 "int main() { return constant(); }",
                 "GTI-S2055",
                 "constexpr evaluation does not waive the unsafe contract");
  const auto recovered = analyze("unsafe int32_t invalid = 0;\n"
                                 "int after_error() { return 0; }");
  expect(hasCode(recovered, "GTI-P0001") &&
             std::any_of(
                 recovered.program.declarations().begin(),
                 recovered.program.declarations().end(),
                 [](const auto &declaration) {
                   const auto *function =
                       lang::stmtAs<lang::FunctionDecl>(declaration.get());
                   return function && function->name().lexeme == "after_error";
                 }),
         "invalid modifier recovery retains the later declaration");
}
} // namespace

int main() {
  testCallsAndTooling();
  testDiagnosticsAndBoundaries();
  testKeywordMigrationAndUnusedBlocks();
  testCallbackMutation();
  testAdditionalCallForms();
  return failures == 0 ? 0 : 1;
}
