#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/session.h"
#include "gti/formatter.h"
#include "gti/hir.h"
#include "gti/language_queries.h"
#include "gti/mir_verifier.h"
#include "lowered_program_test_support.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <llvm/IR/Function.h>
#include <string_view>

namespace lang {
struct MirProgramTestAccess {
  static std::vector<MirFunctionInstance> &functions(MirProgram &program) {
    return program.functions;
  }
  static auto &constructors(MirProgram &program) {
    return program.constructors;
  }
  static auto &destructors(MirProgram &program) { return program.destructors; }
};
} // namespace lang

namespace {
using namespace lang;
int failures = 0;
void expect(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
void testPropagation() {
  const std::string source = R"(
[[inline]] int hinted(int value) { return value + 1; }
[[cold, noinline]] int uncommon(int value) { return value - 1; }
[[cold, inline]] int rare_hint(int value) { return value; }
[[noinline]] T identity<T>(T value) { return value; }
struct Reader {
  Reader() = default;
  [[inline]] int read();
};
int Reader::read() { return 4; }
interface Operation { int call(); };
class Implementation : public Operation {
public:
  Implementation() = default;
  [[cold]] int call() override { return 3; }
};
[[inline]] constexpr int compile_time() { return 5; }
static_assert(compile_time() == 5);
int main() {
  Reader reader{};
  Implementation implementation{};
  return hinted(uncommon(rare_hint(identity<int>(reader.read())))) - 4 +
         implementation.call() - 3 + compile_time() - 5;
}
)";
  const auto frontend = Frontend().analyze("codegen-attributes.gti", source);
  expect(frontend.canGenerateCode(),
         "source function and method annotations are accepted");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  const auto formatted = Formatter().format(source);
  const auto roundtrip = Frontend().analyze("formatted.gti", formatted);
  expect(roundtrip.canGenerateCode(),
         "formatter retains contextual inline annotation");
  expect(Formatter().format(formatted) == formatted,
         "annotated formatting is idempotent");
  std::vector<LoweredProgramIssue> issues;
  const auto verified = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  expect(verified.has_value(), "annotations survive sealed lowering");
  if (!verified)
    return;
  native_llvm::Session session(*verified, {});
  for (const auto &hir : frontend.hir.functionInstances()) {
    const auto *declaration = frontend.semantics.findFunction(hir.declaration);
    expect(declaration &&
               declaration->codegenAttributes == hir.codegenAttributes,
           "concrete HIR retains resolved declaration policy");
  }
  const auto compatibility =
      OptimizationPipeline().run(frontend.hir, OptimizationLevel::O3);
  const auto optimized =
      OptimizationPipeline().run({.hir = frontend.hir,
                                  .mir = frontend.mir,
                                  .level = OptimizationLevel::O3,
                                  .compatibility = &compatibility});
  expect(optimized.valid(), "optimization preserves code-generation contracts");
  if (optimized.valid()) {
    const auto lowered = gti_test::lowerProgram(
        frontend, optimized.sourceMir, optimized.mir, TargetInfo::host(),
        optimized.report.transformations);
    expect(verifyLoweredProgram(lowered).empty(),
           "optimized annotations match sealed declarations");
  }
  unsigned annotated = 0;
  for (const auto &instance : frontend.mir.functionInstances()) {
    const auto *declaration =
        frontend.semantics.findFunction(instance.declaration);
    expect(declaration != nullptr, "instance retains declaration identity");
    if (!declaration)
      continue;
    const auto policy = instance.codegenAttributes;
    expect(policy == declaration->codegenAttributes,
           "MIR preserves semantic annotation policy");
    auto *function = session.abi().declareFunction(instance.id);
    expect(function->hasFnAttribute(llvm::Attribute::Cold) == policy.cold,
           "LLVM cold matches the source hint");
    expect(function->hasFnAttribute(llvm::Attribute::InlineHint) ==
               (policy.inlining == FunctionInlining::Hint),
           "inline maps to inlinehint");
    expect(function->hasFnAttribute(llvm::Attribute::NoInline) ==
               (policy.inlining == FunctionInlining::Never),
           "noinline maps to noinline");
    expect(!function->hasFnAttribute(llvm::Attribute::AlwaysInline),
           "inline never means mandatory inlining");
    if (policy.inlining == FunctionInlining::Default && !policy.cold)
      continue;
    ++annotated;
    const auto signature =
        SignaturePrinter(frontend.semantics).function(*declaration);
    expect(policy.inlining == FunctionInlining::Default ||
               signature.find(policy.inlining == FunctionInlining::Never
                                  ? "[[noinline]]"
                                  : "[[inline]]") != std::string::npos,
           "editor signatures preserve resolved inlining intent");
    expect(!policy.cold || signature.find("[[cold]]") != std::string::npos,
           "editor signatures preserve cold intent");
    auto malformed = frontend.mir;
    auto &changed = MirProgramTestAccess::functions(
        malformed)[&instance - frontend.mir.functionInstances().data()];
    changed.codegenAttributes.inlining = static_cast<FunctionInlining>(99);
    expect(!verifyMirProgram(malformed).valid(),
           "MIR rejects invalid inlining policy");
    changed.codegenAttributes = policy;
    auto lost = frontend.mir;
    MirProgramTestAccess::functions(
        lost)[&instance - frontend.mir.functionInstances().data()]
        .codegenAttributes = {};
    const auto lostBuild = LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir, frontend.mir, lost,
        {}, TargetInfo::host());
    expect(!lostBuild.valid(),
           "lowering rejects an optimization that loses source policy");
    auto sealed = verified->program();
    auto *sealedDeclaration = const_cast<LoweredFunctionDeclaration *>(
        sealed.findFunctionDeclaration(instance.declaration));
    expect(sealedDeclaration != nullptr, "sealed declaration exists");
    if (sealedDeclaration) {
      sealedDeclaration->codegenAttributes = {};
      expect(!verifyLoweredProgram(sealed).empty(),
             "sealed verification rejects erased source policy");
    }
    changed.definitionKind = MirDefinitionKind::Declaration;
    expect(!verifyMirProgram(malformed).valid(),
           "MIR rejects attributes on non-source declarations");
  }
  expect(annotated == 7, "ordinary, generic, rare and separately defined "
                         "method instances retain annotations");
  const auto hover =
      LanguageQueries().hover(frontend, frontend.sourceGraph.entryUnit(),
                              source.find("hinted(uncommon"));
  expect(hover && hover->signature.find("[[inline]]") != std::string::npos,
         "LSP hover exposes inline advice at a call site");
}
void testTerminalInlinePrecedence() {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  const auto frontend =
      Frontend().analyze("terminal-inline.gti", R"(
import std.panic;
[[noreturn, inline]] void terminal_hint() { std::panic("terminal hint"); }
int main() { terminal_hint(); }
)",
                         {library / "prelude.gti"}, {}, {library});
  expect(frontend.canGenerateCode(),
         "terminal inline hint has valid source semantics");
  if (!frontend.canGenerateCode())
    return;
  std::vector<LoweredProgramIssue> issues;
  const auto verified = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  expect(verified.has_value(), "terminal inline hint has verified lowering");
  if (!verified)
    return;
  native_llvm::Session session(*verified, {});
  const auto admission = planLlvmAdmission(*verified, session.options());
  expect(admission.admitted(), "terminal inline hint is admitted");
  if (!admission.admitted())
    return;
  native_llvm::lowerScalarProgram(session, admission);
  unsigned found = 0;
  for (const auto &instance : frontend.mir.functionInstances()) {
    if (instance.codegenAttributes.inlining != FunctionInlining::Hint)
      continue;
    ++found;
    const auto *function = session.abi().declareFunction(instance.id);
    expect(
        function->hasFnAttribute(llvm::Attribute::InlineHint) &&
            !function->hasFnAttribute(llvm::Attribute::NoInline),
        "lowering respects explicit inline advice on a failure-only wrapper");
    expect(!function->hasFnAttribute(llvm::Attribute::NoReturn),
           "source noreturn preserves the GTI failure-status return ABI");
  }
  expect(found == 1, "terminal inline wrapper is represented");
  session.verify();
}
void testLifecyclePolicy() {
  const std::string source = R"(
mut int destroyed = 0;
struct Box<T> {
  T value;
  [[inline]] Box(T value) : value(value) {}
  [[cold, noinline]] ~Box() { destroyed += 1; }
};
struct Converted {
  int value;
  [[noinline]] implicit Converted(int value) : value(value) {}
};
struct Constant {
  int value;
  [[inline]] constexpr Constant(int value) : value(value) {}
};
struct Plain {
  [[cold]] Plain() {}
  [[inline]] ~Plain() {}
};
struct Unsafe {
  int value;
  [[noinline]] unsafe Unsafe(int value) : value(value) {}
};
static_assert(Constant(3).value == 3);
int main() {
  unsafe { Unsafe value{3}; if (value.value != 3) { return 1; } }
  { Box<int> box{7}; if (box.value != 7) { return 1; } }
  Converted converted = 2;
  Constant constant{3};
  Plain plain{};
  return destroyed + converted.value + constant.value - 6;
}
)";
  const auto frontend = Frontend().analyze("lifecycle-attributes.gti", source);
  expect(frontend.canGenerateCode(),
         "source lifecycle attributes are accepted");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    return;
  }
  const auto formatted = Formatter().format(source);
  expect(Frontend()
                 .analyze("lifecycle-formatted.gti", formatted)
                 .canGenerateCode() &&
             Formatter().format(formatted) == formatted,
         "lifecycle attribute formatting preserves valid, stable syntax");
  std::vector<LoweredProgramIssue> issues;
  const auto verified = verifiedLoweredProgram(
      gti_test::lowerProgram(frontend, frontend.mir, frontend.mir), issues);
  expect(verified.has_value(), "lifecycle policy seals successfully");
  if (!verified)
    return;
  const auto compatibility =
      OptimizationPipeline().run(frontend.hir, OptimizationLevel::O3);
  const auto optimized =
      OptimizationPipeline().run({.hir = frontend.hir,
                                  .mir = frontend.mir,
                                  .level = OptimizationLevel::O3,
                                  .compatibility = &compatibility});
  expect(optimized.valid(), "optimization preserves lifecycle policies");
  if (optimized.valid()) {
    expect(verifyLoweredProgram(
               gti_test::lowerProgram(frontend, optimized.sourceMir,
                                      optimized.mir, TargetInfo::host(),
                                      optimized.report.transformations))
               .empty(),
           "optimized lifecycle policies agree with sealed declarations");
  }
  native_llvm::Session session(*verified, {});
  const auto admission = planLlvmAdmission(*verified, session.options());
  expect(admission.admitted(), "annotated lifecycle bodies are admitted");
  if (!admission.admitted())
    return;
  native_llvm::lowerScalarProgram(session, admission);
  unsigned constructors = 0, destructors = 0;
  const auto check = [&](const auto &instance, MirBodyKind kind) {
    const auto policy = instance.codegenAttributes;
    auto *function = session.module().getFunction(
        "__gti_native_v1_body_" + std::to_string(static_cast<unsigned>(kind)) +
        "_" + std::to_string(instance.id));
    expect(function != nullptr, "source lifecycle has a native body");
    if (!function)
      return;
    expect(function->hasFnAttribute(llvm::Attribute::Cold) == policy.cold &&
               function->hasFnAttribute(llvm::Attribute::InlineHint) ==
                   (policy.inlining == FunctionInlining::Hint) &&
               function->hasFnAttribute(llvm::Attribute::NoInline) ==
                   (policy.inlining == FunctionInlining::Never) &&
               !function->hasFnAttribute(llvm::Attribute::AlwaysInline),
           "lifecycle LLVM attributes match verified policy without forced "
           "inlining");
  };
  for (const auto &instance : frontend.mir.constructorInstances()) {
    if (instance.codegenAttributes == FunctionCodegenAttributes{})
      continue;
    ++constructors;
    check(instance, MirBodyKind::Constructor);
    auto lost = frontend.mir;
    auto &changed = MirProgramTestAccess::constructors(lost)[instance.id - 1];
    changed.codegenAttributes = {};
    expect(!LoweredProgramBuilder()
                .build(frontend.program, frontend.semantics, frontend.hir,
                       frontend.mir, lost, {}, TargetInfo::host())
                .valid(),
           "constructor policy cannot disappear between HIR and MIR");
    changed.codegenAttributes.inlining = static_cast<FunctionInlining>(99);
    expect(!verifyMirProgram(lost).valid(),
           "invalid constructor policy is rejected");
    auto sealed = verified->program();
    const auto &hir = frontend.hir.constructorInstances()[instance.id - 1];
    auto *declaration = const_cast<LoweredConstructorDeclaration *>(
        sealed.findConstructorDeclaration(hir.declaration));
    expect(declaration != nullptr, "constructor has a sealed declaration");
    if (declaration) {
      declaration->codegenAttributes = {};
      expect(!verifyLoweredProgram(sealed).empty(),
             "sealed constructor policy cannot be erased");
    }
    const auto *info = frontend.semantics.findConstructor(*hir.source);
    const auto *owner =
        info ? frontend.semantics.findClassType(info->owner) : nullptr;
    expect(info && owner &&
               SignaturePrinter(frontend.semantics)
                   .constructor(*owner, *info)
                   .starts_with("[["),
           "constructor signatures include optimization policy");
  }
  for (const auto &instance : frontend.mir.destructorInstances()) {
    if (instance.codegenAttributes == FunctionCodegenAttributes{})
      continue;
    ++destructors;
    check(instance, MirBodyKind::Destructor);
    auto lost = frontend.mir;
    auto &changed = MirProgramTestAccess::destructors(lost)[instance.id - 1];
    changed.codegenAttributes = {};
    expect(!LoweredProgramBuilder()
                .build(frontend.program, frontend.semantics, frontend.hir,
                       frontend.mir, lost, {}, TargetInfo::host())
                .valid(),
           "destructor policy cannot disappear between HIR and MIR");
    changed.codegenAttributes.inlining = static_cast<FunctionInlining>(99);
    expect(!verifyMirProgram(lost).valid(),
           "invalid destructor policy is rejected");
    auto sealed = verified->program();
    bool erased = false;
    for (const auto &entry : sealed.declarations()) {
      if (const auto *declaration =
              std::get_if<LoweredDestructorDeclaration>(&entry.payload);
          declaration &&
          declaration->codegenAttributes == instance.codegenAttributes) {
        const_cast<LoweredDestructorDeclaration *>(declaration)
            ->codegenAttributes = {};
        erased = true;
        break;
      }
    }
    expect(erased && !verifyLoweredProgram(sealed).empty(),
           "sealed destructor policy cannot be erased");
    const auto &hir = frontend.hir.destructorInstances()[instance.id - 1];
    const auto *info = frontend.semantics.findDestructor(*hir.source);
    expect(info && SignaturePrinter(frontend.semantics)
                       .destructor(*info)
                       .starts_with("[["),
           "destructor signatures include optimization policy");
  }
  expect(constructors == 5 && destructors == 2,
         "generic and ordinary lifecycle policies survive lowering");
  session.verify();
}
void testRejections() {
  for (const auto source : {
           "[[inline, noinline]] int f() { return 0; } int main() { return "
           "f(); }",
           "[[noinline, inline]] int f() { return 0; } int main() { return "
           "f(); }",
           "[[cold, cold]] int f() { return 0; } int main() { return f(); }",
           "[[inline, inline]] int f() { return 0; } int main() { return f(); "
           "}",
           "[[noinline, noinline]] int f() { return 0; } int main() { return "
           "f(); }",
           "extern \"C\" { [[cold]] int foreign(); } int main() { return 0; }",
           "[[inline]] int missing(); int main() { return 0; }",
           "interface I { [[cold]] int read(); }; int main() { return 0; }",
           "struct S { [[inline, noinline]] S() {} }; int main() { return 0; }",
           "struct S { [[cold, cold]] ~S() {} }; int main() { return 0; }",
           "struct S { [[inline]] S() = default; }; int main() { return 0; }",
           "struct S { [[noinline]] S() = delete; }; int main() { return 0; }",
           "struct S { [[cold]] ~S() = default; }; int main() { return 0; }",
           "struct S { [[noreturn]] S() {} }; int main() { return 0; }",
       }) {
    const auto frontend = Frontend().analyze("invalid-codegen.gti", source);
    expect(!frontend.canGenerateCode() &&
               std::any_of(frontend.diagnostics.begin(),
                           frontend.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.code == "GTI-S2099";
                           }),
           "unsupported or contradictory annotations get a function-contract "
           "diagnostic");
  }
  for (const auto source : {
           "[[inline(always)]] int f() { return 0; }",
           "[[cold(1)]] int f() { return 0; }",
           "[[noinline()]] int f() { return 0; }",
           "[[inline]] struct S {};",
           "int main() { [[cold]] int value = 0; return value; }",
           "int inline() { return 0; }",
           "[[cold]] #macro ignored(int value) {} int main() { return 0; }",
           "struct S { [[inline(always)]] S() {} }; int main() { return 0; }",
           "struct S { [[cold(1)]] ~S() {} }; int main() { return 0; }",
       }) {
    const auto frontend = Frontend().analyze("invalid-placement.gti", source);
    expect(!frontend.canGenerateCode(),
           "arguments and unsupported annotation placements are rejected");
  }
}
} // namespace
int main() {
  testPropagation();
  testTerminalInlinePrecedence();
  testLifecyclePolicy();
  testRejections();
  return failures == 0 ? 0 : 1;
}
