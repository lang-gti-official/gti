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
  expect(dump.starts_with("mir-v118 ") &&
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
        "class Value { public: unsafe virtual void raw() {} };",
        "class Value { public: unsafe void operator()() {} };",
        "class Value { public: unsafe Value() = default; };",
        "class Value { public: unsafe ~Value() {} };",
        "interface Value { unsafe void raw(); };",
        "class Value { public: unsafe void raw() override {} };",
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

void testUnsafeMembersAndRequirements() {
  for (const bool arguments : {false, true}) {
    const std::string query =
        arguments
            ? "requires(T& x, int input) { unsafe { x.read(input) } -> int; }"
            : "requires(T& x) { unsafe { x.read() } -> int; }";
    const auto invalid =
        analyze("struct Missing {};\nconcept readable<T> = " + query +
                ";\n"
                "void use<T>(T& x) requires readable<T> {}\n"
                "int main() { Missing value{}; use(value); return 0; }\n");
    const auto diagnostic =
        std::find_if(invalid.diagnostics.begin(), invalid.diagnostics.end(),
                     [](const auto &d) { return d.code == "GTI-S2029"; });
    expect(!invalid.canGenerateCode() &&
               diagnostic != invalid.diagnostics.end(),
           "missing unsafe-required member fails at concept viability");
    if (diagnostic != invalid.diagnostics.end()) {
      expect(
          std::any_of(diagnostic->hints.begin(), diagnostic->hints.end(),
                      [](const auto &hint) {
                        return hint.find("safe or unsafe") != std::string::npos;
                      }) &&
              std::none_of(diagnostic->hints.begin(), diagnostic->hints.end(),
                           [](const auto &hint) {
                             return hint.find("public, safe") !=
                                    std::string::npos;
                           }) &&
              diagnostic->fixes.empty(),
          "unsafe requirement advice does not demand an unsound safe wrapper");
      expect(std::any_of(diagnostic->related.begin(), diagnostic->related.end(),
                         [](const auto &note) {
                           return note.message.find("unsafe { read(") !=
                                  std::string::npos;
                         }),
             "required signature note preserves the unsafe effect");
    }
  }
  const std::string source = R"(
concept reads<T> = requires(T& x) { { x.read() } -> int; };
concept unsafe_reads<T> = requires(T& x) { unsafe { x.read() } -> int; };
concept either_reads<T> = reads<T> || unsafe_reads<T>;
concept both_reads<T> = reads<T> && unsafe_reads<T>;
concept changes<T> = requires(mut T& x, int value) {
  unsafe { x.change(value) } -> int;
};
struct Meter {
  mut int value = 3;
  unsafe int read() { return this.value; }
  unsafe int change(int value) mut { this.value = value; return this.value; }
  bool change(bool value) mut { return value; }
  unsafe static T relay<T>(T value) { return value; }
public:
  Meter() = default;
};
struct SafeMeter { int read() { return 7; }  public: SafeMeter() = default; };
struct Box<T> { unsafe T relay(T value) { return value; }  public: Box() = default; };
struct Ticket { Ticket() {} unsafe int consume() && { return 7; } };
enum class Failure { unavailable };
struct Layout { int count = 1;  public: Layout() = default; };
struct Region { void* address = nullptr;  public: Region() = default; };
concept provider<R, E> =
  requires(mut R& resource, Layout layout) {
    { resource.allocate(layout) } -> expected<Region, E>;
  } && requires(mut R& resource, Region region) {
    unsafe { resource.release(region) } -> void;
  };
struct Depot {
  expected<Region, Failure> allocate(Layout layout) mut {
    if (layout.count < 0) { return unexpected(Failure::unavailable); }
    Region region{};
    return region;
  }
  unsafe void release(Region region) mut {}
public:
  Depot() = default;
};
expected<Region, E> reserve<R, E>(mut R& resource, Layout layout)
  requires provider<R, E> { return resource.allocate(layout); }
unsafe void relinquish<R, E>(mut R& resource, Region region)
  requires provider<R, E> { unsafe { resource.release(region); } }
unsafe int read_generic<T>(T& x) requires unsafe_reads<T> {
  unsafe { return x.read(); }
}
unsafe int forward<T>(T& x) requires either_reads<T> {
  unsafe { return read_generic(x); }
}
int proved_safe<T>(T& x) requires both_reads<T> { return x.read(); }
unsafe int change_generic<T>(mut T& x, int value)
  requires requires(mut T& receiver, int input) {
    unsafe { receiver.change(input) } -> int;
  } {
  unsafe { return x.change(value); }
}
int main() {
  static_assert(!(requires(Meter& x) { { x.read() } -> int; }));
  static_assert(requires(Meter& x) { unsafe { x.read() } -> int; });
  static_assert(requires(SafeMeter& x) { unsafe { x.read() } -> int; });
  mut Meter meter{};
  SafeMeter safe{};
  Box<int> box{};
  mut Depot depot{};
  Layout layout{};
  auto region = reserve<Depot, Failure>(depot, layout);
  if (!region.has_value()) { return 4; }
  if (!meter.change(true) || proved_safe(safe) != 7) { return 1; }
  unsafe {
    relinquish<Depot, Failure>(depot, region.value());
    if (Ticket().consume() != 7) { return 5; }
    if (change_generic(meter, 9) != 9 || forward(meter) != 9) { return 2; }
    return Meter::relay(box.relay(forward(safe))) == 7 ? 0 : 3;
  }
}
)";
  const auto frontend = analyze(source);
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "ordinary, static, generic and structural unsafe methods compose");
  if (frontend.canGenerateCode()) {
    expect(!gti_test::emitCppText(frontend).empty(),
           "unsafe member calls use existing verified MIR production bodies");
    auto erased = frontend.mir;
    bool changed = false;
    for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
             erased.functionInstances())) {
      if (function.owner && function.requiresUnsafe) {
        function.requiresUnsafe = false;
        changed = true;
        break;
      }
    }
    expect(changed && !lang::verifyMirProgram(erased).valid(),
           "MIR rejects erasure of an unsafe member target's contract");
  }
  const auto formatted = lang::Formatter().format(source);
  expect(formatted == lang::Formatter().format(formatted) &&
             analyze(formatted).canGenerateCode(),
         "unsafe member requirements round-trip through formatting");
  const auto mutableRequirement = analyze(R"(
struct Counter {
  mut int value = 1;
  int advance(int amount) mut { this.value += amount; return this.value; }
public:
  Counter() = default;
};
int advance<T>(mut T& value)
  requires requires(mut T& receiver, int amount) {
    { receiver.advance(amount) } -> int;
  } {
  return value.advance(2);
}
int main() { mut Counter value{}; return advance(value) - 3; }
)");
  expect(mutableRequirement.canGenerateCode(),
         "a mutable requirement retains receiver access without qualifying "
         "its synthetic non-reference type argument");
  const auto hoverOffset = source.find("unsafe_reads<T> =") + 1;
  const auto hover = lang::LanguageQueries().hover(
      frontend, frontend.sourceGraph.entryUnit(), hoverOffset);
  expect(hover && hover->signature.find("unsafe {") != std::string::npos,
         "concept hover preserves its explicit unsafe member contract");

  const std::string declaration =
      "struct Item { unsafe int read() { return 1; } "
      "unsafe static int number() { return 2; }  public: Item() = default; };\n";
  expectRejected(declaration +
                     "int main() { Item item{}; return item.read(); }",
                 "GTI-S2055", "instance calls require lexical unsafe");
  expectRejected(declaration + "int main() { return Item::number(); }",
                 "GTI-S2055", "static calls require lexical unsafe");
  expectRejected(
      "struct Item { unsafe int read(const int* p) { return *p; }  public: Item() = default; };",
      "GTI-S2055", "unsafe methods do not authorize their bodies");
  expectRejected(declaration +
                     "int main() { unsafe { auto f = []() "
                     "-> int { return Item::number(); }; return f(); } }",
                 "GTI-S2055", "nested lambdas retain their own safety context");
  expectRejected(
      declaration + "concept safe<T> = requires(T& x) { { x.read() } -> int; };"
                    "int read<T>(T& x) requires safe<T> { return x.read(); }"
                    "int main() { Item item{}; return read(item); }",
      "GTI-S2029", "safe requirements reject unsafe implementations");
  const std::string unsafeContract =
      "concept readable<T> = requires(T& x) { unsafe { x.read() } -> int; };\n";
  expectRejected(
      unsafeContract +
          "int read<T>(T& x) requires readable<T> { return x.read(); }",
      "GTI-S2055", "uninstantiated generic bodies preserve obligations");
  expectRejected(
      unsafeContract +
          "concept safe<T> = requires(T& x) { { x.read() } -> int; };"
          "int safe_read<T>(T& x) requires safe<T> { return x.read(); }"
          "int read<T>(T& x) requires readable<T> { "
          "unsafe { return safe_read(x); } }",
      "GTI-S2029", "unsafe contracts cannot forward into safe ones");
  expectRejected(
      unsafeContract +
          "concept safe<T> = requires(T& x) { { x.read() } -> int; };"
          "int read<T>(T& x) requires safe<T> || readable<T> { "
          "return x.read(); }",
      "GTI-S2055", "constraint alternatives cannot erase unsafe effects");
  expectRejected(
      declaration +
          "int main() { unsafe { auto action = Item::number; } return 0; }",
      "GTI-S2090", "static method values cannot erase unsafe effects");
  expectRejected(declaration + "int main() { Item item{}; unsafe { auto action "
                               "= item.read; } return 0; }",
                 "GTI-S2000",
                 "bound method values cannot erase unsafe effects");
}
void testUnsafeConstructors() {
  const std::string declaration = R"(
struct ReadByte {
  mut int value = 0;
  unsafe ReadByte(const int* address) {
    unsafe { value = *address; }
  }
public:
  ReadByte() = default;
};
struct Generic<T> {
  T value;
  unsafe Generic(T source) : value(source) {}
};
struct LocalGeneric {
  unsafe LocalGeneric<T>(T source) {}
public:
  LocalGeneric() = default;
};
struct ConstantContract {
  int value;
  unsafe constexpr ConstantContract(int source) : value(source) {}
};
struct NativeFactory {
  unsafe static ReadByte create(const int* address);
public:
  NativeFactory() = default;
};
unsafe ReadByte NativeFactory::create(const int* pointer) {
  unsafe { return ReadByte(pointer); }
}
)";
  const std::string source = declaration + R"(
int main() {
  int value = 41;
  unsafe {
    ReadByte read{&value};
    auto other = Generic<int>(1);
    LocalGeneric local{1};
    ConstantContract constant{1};
    auto split = NativeFactory::create(&value);
    return read.value + other.value == 42 && split.value == 41 ? 0 : 1;
  }
}
)";
  const auto frontend = analyze(source);
  for (const auto &diagnostic : frontend.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(frontend.canGenerateCode(),
         "ordinary and generic unsafe constructors");
  if (frontend.canGenerateCode()) {
    expect(!gti_test::emitCppText(frontend).empty(),
           "unsafe constructors use the verified MIR emitter");
    auto erased = frontend.mir;
    bool changed = false;
    for (auto &function : const_cast<std::vector<lang::MirFunctionInstance> &>(
             erased.functionInstances()))
      for (auto &block : function.body.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.unsafeOperation ==
              lang::UnsafeOperationKind::DeclaredUnsafeConstruction) {
            instruction.unsafeOperation = lang::UnsafeOperationKind::None;
            changed = true;
          }
    expect(changed && !lang::verifyMirProgram(erased).valid(),
           "MIR rejects erasure of a constructor's unsafe contract");
    const auto hover = lang::LanguageQueries().hover(
        frontend, frontend.sourceGraph.entryUnit(),
        source.find("ReadByte(const") + 1);
    expect(hover && hover->signature.starts_with("unsafe "),
           "constructor hover preserves unsafe");
  }
  const auto formatted = lang::Formatter().format(source);
  expect(formatted == lang::Formatter().format(formatted) &&
             analyze(formatted).canGenerateCode(),
         "unsafe constructors format and reanalyze");
  expectRejected(declaration +
                     "int main() { int n = 1; ReadByte x{&n}; return 0; }",
                 "GTI-S2055", "direct initialization requires unsafe");
  expectRejected(declaration +
                     "int main() { auto x = Generic<int>(1); return 0; }",
                 "GTI-S2055", "generic constructor calls require unsafe");
  expectRejected(declaration +
                     "int main() { auto x = LocalGeneric(1); return 0; }",
                 "GTI-S2055", "constructor-local generics preserve unsafe");
  expectRejected("struct R { int n; unsafe R(const int* p) { n = *p; } };",
                 "GTI-S2055",
                 "unsafe constructors do not authorize their bodies");
  expectRejected(
      "struct B { unsafe B(int n) {}  public: B() = default; }; struct D : public B { D() : B(1) {} };",
      "GTI-S2055", "base initializer lists do not erase unsafe obligations");
  const auto zero = analyze(
      "struct Z { unsafe Z() {} }; int main() { unsafe { Z z{}; } return 0; }");
  expect(zero.canGenerateCode(), "explicit zero-argument unsafe construction");
  expectRejected("struct Z { unsafe Z() {} }; int main() { Z z{}; return 0; }",
                 "GTI-S2055", "zero-argument constructor requires unsafe");
  expectRejected(
      declaration + "int main() { int value = 1; "
                    "auto copy = NativeFactory::create(&value); return 0; }",
      "GTI-S2055", "split factory bodies retain their unsafe caller contract");
  expectRejected(
      "struct Z { unsafe Z() {} }; struct Owner { Z z; }; int main() { Owner "
      "x{}; return 0; }",
      "GTI-S2012",
      "implicit member initialization cannot hide unsafe construction");
  expectRejected(
      declaration + "int main() { unsafe { auto f = []() -> int { auto x = "
                    "Generic<int>(1); return x.value; }; return f(); } }",
      "GTI-S2055", "nested lambdas cannot inherit constructor permission");
  expectRejected(
      "#include <std/vector>\n" + declaration +
          "int main() { mut std::vector<Generic<int>> values{}; "
          "[[discard]] values.emplace_back(1); return 0; }",
      "GTI-S2055",
      "safe generic emplacement cannot conceal an unsafe constructor");
  expectRejected(
      "#include <std/memory>\n" + declaration +
          "int main() { mut std::uninitialized<Generic<int>> storage{}; "
          "unsafe { std::construct_at(storage.data(), 1); } return 0; }",
      "GTI-S2055",
      "raw lifetime permission does not imply a constructor contract");
  const auto completedValue =
      analyze("#include <std/memory>\n" + declaration +
              "int main() { mut std::uninitialized<Generic<int>> storage{}; "
              "unsafe { std::construct_at(storage.data(), Generic<int>(1)); "
              "std::destroy_at(storage.data()); } return 0; }");
  expect(completedValue.canGenerateCode(),
         "an explicitly constructed value can be moved into raw storage");
  if (completedValue.canGenerateCode())
    expect(!gti_test::emitCppText(completedValue).empty(),
           "completed unsafe-constructor values keep ordinary move semantics");
}
} // namespace

int main() {
  testCallsAndTooling();
  testDiagnosticsAndBoundaries();
  testKeywordMigrationAndUnusedBlocks();
  testCallbackMutation();
  testAdditionalCallForms();
  testUnsafeMembersAndRequirements();
  testUnsafeConstructors();
  return failures == 0 ? 0 : 1;
}
