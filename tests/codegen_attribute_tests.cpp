#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/session.h"
#include "cpp_backend_test_support.h"
#include "gti/formatter.h"
#include "gti/hir.h"
#include "gti/language_queries.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <iostream>
#include <llvm/IR/Function.h>
#include <string_view>

namespace lang {
struct MirProgramTestAccess {
  static std::vector<MirFunctionInstance> &functions(MirProgram &program) {
    return program.functions;
  }
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
int main() {
  Reader reader{};
  return hinted(uncommon(rare_hint(identity<int>(reader.read())))) - 4;
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
    expect(signature.find(policy.inlining == FunctionInlining::Never
                              ? "[[noinline]]"
                              : "[[inline]]") != std::string::npos,
           "editor signatures preserve resolved annotation intent");
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
  expect(annotated == 5, "ordinary, generic, rare and separately defined "
                         "method instances retain annotations");
  const auto hover =
      LanguageQueries().hover(frontend, frontend.sourceGraph.entryUnit(),
                              source.find("hinted(uncommon"));
  expect(hover && hover->signature.find("[[inline]]") != std::string::npos,
         "LSP hover exposes inline advice at a call site");
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
           "struct S { [[inline]] S() {} }; int main() { return 0; }",
           "struct S { [[cold]] ~S() {} }; int main() { return 0; }",
       }) {
    const auto frontend = Frontend().analyze("invalid-placement.gti", source);
    expect(!frontend.canGenerateCode(),
           "arguments and unsupported annotation placements are rejected");
  }
}
} // namespace
int main() {
  testPropagation();
  testRejections();
  return failures == 0 ? 0 : 1;
}
