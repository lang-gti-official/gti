#include "gti/cpp_backend.h"
#include "gti/cpp_emitter.h"
#include "gti/frontend.h"
#include "gti/mir_printer.h"
#include "gti/optimizer.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
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

std::size_t count(std::string_view text, std::string_view needle) {
  std::size_t result = 0;
  for (std::size_t position = text.find(needle); position != std::string::npos;
       position = text.find(needle, position + needle.size())) {
    ++result;
  }
  return result;
}

std::string_view functionDefinition(std::string_view generated,
                                    std::string_view sourceName) {
  const std::string needle = std::string{"_"} + std::string{sourceName} + "(";
  std::size_t definition = std::string_view::npos;
  for (std::size_t name = generated.find(needle);
       name != std::string_view::npos;
       name = generated.find(needle, name + needle.size())) {
    const std::size_t lineEnd = generated.find('\n', name);
    const std::size_t brace = generated.find(" {\n", name);
    if (brace != std::string_view::npos &&
        (lineEnd == std::string_view::npos || brace < lineEnd)) {
      definition = brace;
      break;
    }
  }
  const std::size_t end = definition == std::string_view::npos
                              ? definition
                              : generated.find("\n  }", definition);
  return definition == std::string_view::npos || end == std::string_view::npos
             ? std::string_view{}
             : generated.substr(definition, end + 4 - definition);
}

lang::FrontendResult analyze(std::string name, std::string source) {
  lang::FrontendResult result =
      lang::Frontend().analyze(std::move(name), std::move(source));
  if (!result.canGenerateCode()) {
    for (const lang::Diagnostic &diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  return result;
}

std::string source() {
  return R"(
int32_t mir_identity(int32_t value) {
  return (value);
}

int32_t mir_constant() {
  return (42);
}

int8_t mir_i8_identity(int8_t value) {
  return value;
}

int32_t mir_second(int8_t first, int32_t second, uint64_t third) {
  return second;
}

int64_t mir_i64_max() {
  return 9223372036854775807;
}

uint64_t mir_u64_max() {
  return 18446744073709551615;
}

void mir_noop() {}

int32_t compatibility_identity(int32_t value) {
  int32_t copy = value;
  return copy;
}

int32_t compatibility_checked(int32_t value) {
  return value + 1;
}

int32_t checked_leaf(int32_t value) {
  return value + 1;
}

int32_t checked_caller(int32_t value) {
  return checked_leaf(value) + 2;
}

bool compatibility_bool_identity(bool value) {
  return value;
}

char compatibility_char_identity(char value) {
  return value;
}

int main() {
  if (mir_identity(42) != compatibility_identity(42)) {
    return 1;
  }
  if (checked_caller(39) != 42) {
    return 9;
  }
  if (mir_constant() != compatibility_checked(41)) {
    return 2;
  }
  if (mir_i8_identity(int8_t(-128)) != int8_t(-128)) {
    return 3;
  }
  if (mir_second(int8_t(-1), 42, uint64_t(99)) != 42) {
    return 4;
  }
  if (mir_i64_max() != int64_t(9223372036854775807)) {
    return 5;
  }
  if (mir_u64_max() != uint64_t(18446744073709551615)) {
    return 6;
  }
  if (!compatibility_bool_identity(true)) {
    return 7;
  }
  if (compatibility_char_identity('x') != 'x') {
    return 8;
  }
  mir_noop();
  return 0;
}
)";
}

lang::OptimizedProgram optimize(const lang::FrontendResult &frontend,
                                lang::OptimizationLevel level,
                                const lang::OptimizationResult &compatibility) {
  return lang::OptimizationPipeline().run({.hir = frontend.hir,
                                           .mir = frontend.mir,
                                           .level = level,
                                           .compatibility = &compatibility});
}

lang::BackendArtifact emit(const lang::FrontendResult &frontend,
                           const lang::MirProgram &mir,
                           const lang::OptimizationResult &compatibility) {
  return lang::CppBackend().generate({.program = frontend.program,
                                      .semantics = frontend.semantics,
                                      .hir = frontend.hir,
                                      .mir = mir,
                                      .sourceMir = &frontend.mir,
                                      .optimizations = compatibility});
}

void testSelectedFamilyAndCompatibilityFallback() {
  const lang::FrontendResult frontend =
      analyze("mir-backend-first-family.gti", source());
  expect(frontend.canGenerateCode(),
         "the MIR backend family fixture should pass the frontend");
  if (!frontend.canGenerateCode()) {
    return;
  }

  const lang::OptimizationPipeline pipeline;
  const lang::OptimizationResult o0Compatibility =
      pipeline.run(frontend.hir, lang::OptimizationLevel::O0);
  const lang::OptimizedProgram o0 =
      optimize(frontend, lang::OptimizationLevel::O0, o0Compatibility);
  expect(o0.valid() && lang::verifyMirProgram(o0.mir).valid(),
         "the O0 production MIR input should be fully verified");
  if (!o0.valid()) {
    return;
  }

  const lang::BackendArtifact o0Artifact =
      emit(frontend, o0.mir, o0Compatibility);
  constexpr std::string_view marker = "// GTI verified-MIR body: scalar-cfg-v1";
  expect(count(o0Artifact.contents, marker) == 10,
         "exactly the ten scalar bodies of this fixture should use general "
         "MIR body emission");
  expect(o0Artifact.contents.find("__gti_mir_arg_0") != std::string::npos &&
             o0Artifact.contents.find("__gti_mir_v_") != std::string::npos,
         "the selected definitions should name MIR parameters and SSA values");
  // The checked sibling migrated to the per-body defined-failure boundary
  // (ADR 017): its transformed body emits from verified MIR and the
  // same-signature wrapper routes failure into the structured contract.
  expect(count(o0Artifact.contents,
               "// GTI verified-MIR body: scalar-cfg-failure-v1") == 3 &&
             functionDefinition(o0Artifact.contents, "compatibility_checked")
                     .find("gti_rt_failure_terminate_v1") != std::string::npos,
         "the checked arithmetic sibling should emit its transformed "
         "failure-form body plus the defined-failure boundary wrapper");
  expect(o0Artifact.contents.find("checked_leaf__gti_mir_failure(") !=
                 std::string::npos &&
             o0Artifact.contents.find("__gti_mir_call_success_") !=
                 std::string::npos,
         "checked_caller should reach checked_leaf through the transformed "
         "convention");
  expect(functionDefinition(o0Artifact.contents, "compatibility_identity")
                 .find("// GTI verified-MIR body: scalar-cfg-v1") !=
             std::string_view::npos,
         "the completed leaf gate should coexist with the next verified-MIR "
         "local-storage family");
  expect(
      functionDefinition(o0Artifact.contents, "mir_i8_identity")
                  .find("std::int8_t") != std::string_view::npos &&
          functionDefinition(o0Artifact.contents, "mir_i64_max")
                  .find("static_cast<std::int64_t>(9223372036854775807)") !=
              std::string_view::npos &&
          functionDefinition(o0Artifact.contents, "mir_u64_max")
                  .find("18446744073709551615ULL") != std::string_view::npos &&
          functionDefinition(o0Artifact.contents, "mir_noop").find("return;") !=
              std::string_view::npos,
      "the bounded family should preserve a signed narrow parameter, "
      "signed and unsigned integer extrema, and a void leaf");
  const std::string_view mirSecond =
      functionDefinition(o0Artifact.contents, "mir_second");
  expect(o0Artifact.contents.find(
             "mir_second(std::int8_t __gti_mir_arg_0, std::int32_t "
             "__gti_mir_arg_1, std::uint64_t __gti_mir_arg_2)") !=
                 std::string::npos &&
             mirSecond.find("= __gti_mir_arg_1;") != std::string_view::npos,
         "MIR parameter bindings should retain source order and load the "
         "selected second parameter");
  expect(
      functionDefinition(o0Artifact.contents, "compatibility_bool_identity")
                  .find(marker) != std::string_view::npos &&
          functionDefinition(o0Artifact.contents, "compatibility_char_identity")
                  .find(marker) != std::string_view::npos,
      "bool and character identity bodies are admitted per body by the "
      "general emitter");

  const std::string direct =
      lang::CppEmitter(frontend.semantics, frontend.hir).emit(frontend.program);
  expect(direct.find(marker) == std::string::npos &&
             direct.find("return (value);") != std::string::npos,
         "direct CppEmitter construction should remain an explicit "
         "compatibility-only API");

  const lang::OptimizationResult o1Compatibility =
      pipeline.run(frontend.hir, lang::OptimizationLevel::O1);
  const lang::OptimizedProgram o1 =
      optimize(frontend, lang::OptimizationLevel::O1, o1Compatibility);
  expect(o1.valid() && lang::verifyMirProgram(o1.mir).valid(),
         "the O1 transformed production MIR input should be fully verified");
  if (o1.valid()) {
    const lang::BackendArtifact o1Artifact =
        emit(frontend, o1.mir, o1Compatibility);
    const lang::BackendArtifact o1WithO0Compatibility =
        emit(frontend, o1.mir, o0Compatibility);
    expect(count(o1Artifact.contents, marker) == 10,
           "the same bodies should remain selected after MIR optimization");
    const std::string_view o0Constant =
        functionDefinition(o0Artifact.contents, "mir_constant");
    const std::string_view o1Constant =
        functionDefinition(o1Artifact.contents, "mir_constant");
    expect(
        o0Constant.find("__gti_mir_v_1 = __gti_mir_v_2;") !=
                std::string_view::npos &&
            o1Constant.find("__gti_mir_v_1 = static_cast<std::int32_t>(42);") !=
                std::string_view::npos &&
            o1Constant.find("__gti_mir_v_1 = __gti_mir_v_2;") ==
                std::string_view::npos,
        "the marked constant body should execute the verified O1 "
        "Identity-to-Literal MIR rewrite");
    expect(count(o1WithO0Compatibility.contents, marker) == 10 &&
               functionDefinition(o1WithO0Compatibility.contents,
                                  "mir_constant") == o1Constant,
           "verified optimized MIR should control the selected body even "
           "when the compatibility optimizer supplies its O0 result");

    lang::MirProgram laundered = o1.mir;
    auto &launderedFunctions =
        const_cast<std::vector<lang::MirFunctionInstance> &>(
            laundered.functionInstances());
    lang::MirInstruction *rewritten = nullptr;
    for (lang::MirFunctionInstance &candidate : launderedFunctions) {
      for (lang::MirInstruction &instruction :
           candidate.body.blocks.front().instructions) {
        if (instruction.literalProvenance.kind ==
            lang::MirLiteralProvenanceKind::IdentityFold) {
          rewritten = &instruction;
          break;
        }
      }
      if (rewritten != nullptr) {
        break;
      }
    }
    expect(rewritten != nullptr,
           "O1 should expose an identity-fold provenance record");
    if (rewritten != nullptr) {
      rewritten->literalProvenance = {
          .kind = lang::MirLiteralProvenanceKind::Source};
      expect(lang::verifyMirProgram(laundered).valid(),
             "source relabeling should remain structurally valid so HIR/MIR "
             "provenance coherence owns rejection");
      bool rejected = false;
      try {
        (void)emit(frontend, laundered, o0Compatibility);
      } catch (const std::logic_error &) {
        rejected = true;
      }
      expect(rejected,
             "an optimized grouping literal relabeled as source provenance "
             "must not execute or fall back");
    }
  }
}

void testUnverifiedMirIsRejected() {
  const lang::FrontendResult frontend = analyze("mir-backend-invalid.gti", R"(
int32_t identity(int32_t value) { return value; }
int main() { return identity(0); }
)");
  if (!frontend.canGenerateCode()) {
    expect(false, "the invalid-MIR fixture should first be valid source");
    return;
  }
  lang::MirProgram invalid = frontend.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      invalid.functionInstances());
  expect(!functions.empty() && functions.front().body.blocks.size() == 1,
         "the invalid-MIR fixture should contain the scalar leaf");
  if (functions.empty() || functions.front().body.blocks.empty() ||
      !functions.front().body.blocks.front().terminator.value) {
    return;
  }
  functions.front().body.blocks.front().terminator.value->value = 9999;
  expect(!lang::verifyMirProgram(invalid).valid(),
         "the adversarial return operand should fail MIR verification");

  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  bool rejected = false;
  try {
    (void)emit(frontend, invalid, compatibility);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected,
         "CppBackend should reject an unverified MIR program before body "
         "selection");
}

void testStaleMirSnapshotIsRejected() {
  constexpr std::string_view identicalSource = R"(
int32_t stable() { return (7); }
int main() { return stable() - 7; }
)";
  const lang::FrontendResult expected =
      analyze("same-snapshot-source.gti", std::string{identicalSource});
  const lang::FrontendResult stale =
      analyze("same-snapshot-source.gti", std::string{identicalSource});
  if (!expected.canGenerateCode() || !stale.canGenerateCode()) {
    expect(false, "both mixed-snapshot fixtures should pass the frontend");
    return;
  }
  expect(lang::verifyMirProgram(stale.mir).valid(),
         "the stale MIR should be independently valid so the regression "
         "tests snapshot coherence rather than structural verification");
  expect(expected.hir.module().placeDomain.snapshot !=
             stale.hir.module().placeDomain.snapshot,
         "byte-identical source analyzed twice should still carry distinct "
         "frontend snapshot identities");
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(expected.hir,
                                       lang::OptimizationLevel::O0);
  bool rejected = false;
  try {
    (void)emit(expected, stale.mir, compatibility);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected,
         "a valid same-shaped MIR program from another frontend snapshot "
         "must not control emission");

  lang::MirProgram staleWithMatchingModule = stale.mir;
  lang::MirBody &staleModule =
      const_cast<lang::MirBody &>(staleWithMatchingModule.module());
  staleModule.placeDomain = expected.hir.module().placeDomain;
  // The generated hosted-startup body pins its place domain to the module
  // snapshot, so a structurally coherent forgery has to move both.
  if (const lang::MirBody *hosted = staleWithMatchingModule.hostedStartup()) {
    const_cast<lang::MirBody *>(hosted)->placeDomain.snapshot =
        staleModule.placeDomain.snapshot;
  }
  expect(lang::verifyMirProgram(staleWithMatchingModule).valid(),
         "the defense-in-depth fixture should remain valid after only its "
         "empty module domain is forged");
  rejected = false;
  try {
    (void)emit(expected, staleWithMatchingModule, compatibility);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected,
         "the selected function body should independently reject a stale "
         "place-domain snapshot even if the module gate is forged");

  const lang::FrontendResult entryOnly =
      analyze("same-entry-only.gti", "int main() { return 0; }");
  const lang::FrontendResult staleEntryOnly =
      analyze("same-entry-only.gti", "int main() { return 0; }");
  if (!entryOnly.canGenerateCode() || !staleEntryOnly.canGenerateCode()) {
    expect(false, "the entry-only mixed-snapshot fixtures should be valid");
    return;
  }
  const lang::OptimizationResult entryCompatibility =
      lang::OptimizationPipeline().run(entryOnly.hir,
                                       lang::OptimizationLevel::O0);
  rejected = false;
  try {
    (void)emit(entryOnly, staleEntryOnly.mir, entryCompatibility);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected,
         "CppBackend should reject a mixed snapshot at entry even when no "
         "body can enter the first MIR family");
}

void testForgedFunctionHeaderIsRejected() {
  const lang::FrontendResult frontend = analyze("forged-header.gti", R"(
int32_t stable(int32_t value) { return value; }
int32_t unused_stable(int32_t value) { return value; }
int32_t unused_failing(int32_t value) { return value + 1; }
int main() { return stable(0); }
)");
  if (!frontend.canGenerateCode()) {
    expect(false, "the forged-header fixture should pass the frontend");
    return;
  }
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const auto headerTarget =
      std::find_if(frontend.hir.functionInstances().begin(),
                   frontend.hir.functionInstances().end(),
                   [](const lang::HirFunctionInstance &function) {
                     return function.source != nullptr &&
                            function.source->name().lexeme == "unused_stable";
                   });
  const auto expectHeaderRejected = [&](auto mutate,
                                        std::string_view description,
                                        bool structurallyValid = true) {
    lang::MirProgram forged = frontend.mir;
    auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
        forged.functionInstances());
    auto selected = std::find_if(
        functions.begin(), functions.end(),
        [&](const lang::MirFunctionInstance &function) {
          return headerTarget != frontend.hir.functionInstances().end() &&
                 function.id == headerTarget->id;
        });
    expect(selected != functions.end(),
           "the forged-header fixture should contain an eligible leaf");
    if (selected == functions.end()) {
      return;
    }
    mutate(*selected);
    expect(lang::verifyMirProgram(forged).valid() == structurallyValid,
           structurallyValid
               ? "the forged function header should remain structurally "
                 "valid so backend snapshot coherence owns rejection"
               : "forged non-source provenance must reject a retained source "
                 "body during MIR verification");
    bool rejected = false;
    try {
      (void)emit(frontend, forged, compatibility);
    } catch (const std::logic_error &) {
      rejected = true;
    }
    expect(rejected, description);
  };
  expectHeaderRejected(
      [](lang::MirFunctionInstance &function) {
        function.constexprFunction = !function.constexprFunction;
      },
      "a forged constexpr header must fail before body selection");
  expectHeaderRejected(
      [](lang::MirFunctionInstance &function) {
        function.receiverMutability = lang::ReceiverMutability::Mutable;
      },
      "a forged receiver-mutability header must fail before body selection");
  expectHeaderRejected(
      [](lang::MirFunctionInstance &function) {
        function.overloadedOperator = lang::OverloadedOperator::Equal;
      },
      "a forged overloaded-operator header must fail before body selection");
  expectHeaderRejected(
      [](lang::MirFunctionInstance &function) {
        function.definitionKind =
            lang::MirFunctionInstance::DefinitionKind::Declaration;
        function.mayRaiseDefinedFailure = true;
      },
      "a forged source-definition header must fail before body selection",
      false);

  lang::MirProgram forgedFailureSummary = frontend.mir;
  const lang::HirFunctionInstance *unused = nullptr;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source != nullptr &&
        function.source->name().lexeme == "unused_stable") {
      unused = &function;
      break;
    }
  }
  auto &summaryFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      forgedFailureSummary.functionInstances());
  lang::MirFunctionInstance *summary =
      unused == nullptr || unused->id == 0 ||
              unused->id > summaryFunctions.size()
          ? nullptr
          : &summaryFunctions[unused->id - 1];
  expect(summary != nullptr && !summary->mayRaiseDefinedFailure,
         "the failure-summary mutation should locate an unused no-fail leaf");
  if (summary != nullptr) {
    summary->mayRaiseDefinedFailure = true;
    expect(lang::verifyMirProgram(forgedFailureSummary).valid(),
           "a conservative true failure summary should remain valid MIR");
    bool rejected = false;
    try {
      (void)emit(frontend, forgedFailureSummary, compatibility);
    } catch (const std::logic_error &) {
      rejected = true;
    }
    expect(rejected,
           "a conservative true summary is valid generic MIR but must not "
           "drift from the canonical source MIR used for emission");
  }

  lang::MirProgram forgedNoFailure = frontend.mir;
  const lang::HirFunctionInstance *failing = nullptr;
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source != nullptr &&
        function.source->name().lexeme == "unused_failing") {
      failing = &function;
      break;
    }
  }
  auto &failingFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      forgedNoFailure.functionInstances());
  lang::MirFunctionInstance *falseClaim =
      failing == nullptr || failing->id == 0 ||
              failing->id > failingFunctions.size()
          ? nullptr
          : &failingFunctions[failing->id - 1];
  expect(falseClaim != nullptr && falseClaim->mayRaiseDefinedFailure,
         "the failure-summary mutation should locate an unused failing body");
  if (falseClaim != nullptr) {
    falseClaim->mayRaiseDefinedFailure = false;
    expect(!lang::verifyMirProgram(forgedNoFailure).valid(),
           "generic MIR verification must reject an unproved false "
           "defined-failure summary even when no caller observes it");
  }
}

void testLeafCannotDriftIntoCfgFamily() {
  const lang::FrontendResult frontend =
      analyze("leaf-family-drift.gti", source());
  if (!frontend.canGenerateCode()) {
    expect(false, "the cross-family fixture should pass the frontend");
    return;
  }
  const auto hirIdentity =
      std::find_if(frontend.hir.functionInstances().begin(),
                   frontend.hir.functionInstances().end(),
                   [](const lang::HirFunctionInstance &instance) {
                     return instance.source != nullptr &&
                            instance.source->name().lexeme == "mir_identity";
                   });
  lang::MirProgram drifted = frontend.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      drifted.functionInstances());
  lang::MirFunctionInstance *identity =
      hirIdentity == frontend.hir.functionInstances().end() ||
              hirIdentity->id == 0 || hirIdentity->id > functions.size()
          ? nullptr
          : &functions[hirIdentity->id - 1];
  expect(identity != nullptr && identity->body.blocks.size() == 1,
         "the cross-family mutation should locate the one-block scalar leaf");
  if (identity == nullptr || identity->body.blocks.size() != 1) {
    return;
  }
  lang::MirTerminator returnTerminator =
      identity->body.blocks.front().terminator;
  identity->body.blocks.front().terminator = {
      .kind = lang::MirTerminatorKind::Goto, .target = 2};
  identity->body.blocks.push_back(
      {.id = 2, .terminator = returnTerminator, .reachable = true});
  lang::rebuildMirReachability(identity->body);
  (void)lang::rebuildMirValueUses(identity->body);
  expect(lang::verifyMirProgram(drifted).valid(),
         "routing a leaf return through a second block should remain valid "
         "MIR and fit the broader CFG shape");
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  bool rejected = false;
  try {
    (void)emit(frontend, drifted, compatibility);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected,
         "a source-classified scalar leaf must reject family-shape drift "
         "instead of silently changing to scalar-cfg-v1 or compatibility");
}

void testForgedValidLiteralIsRejected() {
  const lang::FrontendResult frontend = analyze("forged-literal.gti", R"(
int32_t stable() { return (7); }
int main() { return stable() - 7; }
)");
  if (!frontend.canGenerateCode()) {
    expect(false, "the forged-literal fixture should pass the frontend");
    return;
  }
  lang::MirProgram forged = frontend.mir;
  auto &functions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      forged.functionInstances());
  lang::MirInstruction *literal = nullptr;
  if (!functions.empty()) {
    for (lang::MirInstruction &instruction :
         functions.front().body.blocks.front().instructions) {
      if (instruction.operation == lang::MirOperation::Literal) {
        literal = &instruction;
        break;
      }
    }
  }
  expect(literal != nullptr,
         "the forged-literal fixture should contain a MIR literal");
  if (literal == nullptr) {
    return;
  }
  literal->literal = lang::Literal{std::uint64_t{9}};
  expect(lang::verifyMirProgram(forged).valid(),
         "the adversarial literal should remain structurally valid so the "
         "backend provenance gate is exercised");
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  bool rejected = false;
  try {
    (void)emit(frontend, forged, compatibility);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected,
         "a structurally valid but unauthorized MIR literal must not execute "
         "or silently switch body authority");
}

void testMissingEligibleInstanceIsRejected() {
  const lang::FrontendResult frontend = analyze("missing-instance.gti", R"(
int main() { return 0; }
int32_t dormant(int32_t value) { return value; }
)");
  if (!frontend.canGenerateCode()) {
    expect(false, "the missing-instance fixture should pass the frontend");
    return;
  }
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);

  lang::MirProgram missing = frontend.mir;
  auto &missingFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      missing.functionInstances());
  expect(missingFunctions.size() == 2 &&
             missingFunctions.back().entryKind == lang::ProgramEntryKind::None,
         "the dormant eligible instance should follow the entry fixture");
  if (missingFunctions.size() != 2 ||
      missingFunctions.back().entryKind != lang::ProgramEntryKind::None) {
    return;
  }
  missingFunctions.pop_back();
  // That body counted toward the generated hosted-startup place domain, which
  // is pinned one past the maximum remaining body domain.
  if (const lang::MirBody *hosted = missing.hostedStartup()) {
    std::size_t maximumBodyDomain = 0;
    for (const lang::MirBodyAddress address :
         lang::enumerateMirBodyAddresses(missing)) {
      const lang::MirBody *candidate = lang::findMirBody(missing, address);
      if (candidate != nullptr && candidate != hosted) {
        maximumBodyDomain =
            std::max(maximumBodyDomain, candidate->placeDomain.body);
      }
    }
    const_cast<lang::MirBody *>(hosted)->placeDomain.body =
        maximumBodyDomain + 1;
  }
  expect(lang::verifyMirProgram(missing).valid(),
         "removing an unreferenced final instance should remain structurally "
         "valid so backend snapshot coherence owns the rejection");
  bool rejected = false;
  try {
    (void)emit(frontend, missing, compatibility);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected,
         "an eligible source body with no matching MIR instance must not "
         "silently use compatibility emission");

  lang::MirProgram duplicated = frontend.mir;
  auto &duplicatedFunctions =
      const_cast<std::vector<lang::MirFunctionInstance> &>(
          duplicated.functionInstances());
  duplicatedFunctions.back().declaration =
      duplicatedFunctions.front().declaration;
  expect(lang::verifyMirProgram(duplicated).valid(),
         "a duplicated nonzero declaration mapping should remain a valid MIR "
         "shape so the cross-snapshot identity gate is exercised");
  rejected = false;
  try {
    (void)emit(frontend, duplicated, compatibility);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected,
         "duplicate and missing eligible declaration mappings must fail "
         "closed before compatibility emission");
}

void testJointHirMirLiteralForgeryIsRejected() {
  lang::FrontendResult frontend = analyze("joint-literal-forgery.gti", R"(
int64_t huge() { return 9223372036854775807; }
int main() { return huge() == int64_t(9223372036854775807) ? 0 : 1; }
)");
  if (!frontend.canGenerateCode()) {
    expect(false, "the joint-literal fixture should pass the frontend");
    return;
  }
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact baseline =
      emit(frontend, frontend.mir, compatibility);
  expect(functionDefinition(baseline.contents, "huge")
                 .find("// GTI verified-MIR body: scalar-cfg-v1") !=
             std::string_view::npos,
         "the signed literal boundary fixture must enter the selected MIR "
         "family before mutation");

  auto &hirFunctions = const_cast<std::vector<lang::HirFunctionInstance> &>(
      frontend.hir.functionInstances());
  auto &mirFunctions = const_cast<std::vector<lang::MirFunctionInstance> &>(
      frontend.mir.functionInstances());
  lang::HirValue *hirLiteral = nullptr;
  lang::MirInstruction *mirLiteral = nullptr;
  for (lang::HirFunctionInstance &function : hirFunctions) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      continue;
    }
    for (lang::HirValue &value : function.body.values) {
      if (value.kind == lang::HirValueKind::Literal && value.literal) {
        hirLiteral = &value;
        break;
      }
    }
  }
  for (lang::MirFunctionInstance &function : mirFunctions) {
    if (function.entryKind != lang::ProgramEntryKind::None) {
      continue;
    }
    for (lang::MirInstruction &instruction :
         function.body.blocks.front().instructions) {
      if (instruction.operation == lang::MirOperation::Literal &&
          instruction.literal) {
        mirLiteral = &instruction;
        break;
      }
    }
  }
  expect(hirLiteral != nullptr && mirLiteral != nullptr,
         "the joint-literal fixture should expose matching HIR/MIR literals");
  if (hirLiteral == nullptr || mirLiteral == nullptr) {
    return;
  }
  const auto *sourceLiteral =
      dynamic_cast<const lang::LiteralExpr *>(hirLiteral->source);
  expect(sourceLiteral != nullptr,
         "the selected HIR literal should retain its exact AST source");
  if (sourceLiteral == nullptr) {
    return;
  }
  constexpr std::uint64_t forgedMagnitude =
      std::numeric_limits<std::uint64_t>::max();
  const_cast<lang::Literal &>(sourceLiteral->value()) =
      lang::Literal{forgedMagnitude};
  hirLiteral->literal = lang::Literal{forgedMagnitude};
  mirLiteral->literal = lang::Literal{forgedMagnitude};
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "the forged signed magnitude should remain generally MIR-valid so "
         "the selected-family representation gate is exercised");
  bool rejected = false;
  try {
    (void)emit(frontend, frontend.mir, compatibility);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected,
         "a jointly forged AST/HIR/MIR bare literal outside int64_t must be "
         "rejected rather than emitted through implementation-defined C++");
}

} // namespace

int main() {
  testSelectedFamilyAndCompatibilityFallback();
  testUnverifiedMirIsRejected();
  testStaleMirSnapshotIsRejected();
  testForgedFunctionHeaderIsRejected();
  testLeafCannotDriftIntoCfgFamily();
  testForgedValidLiteralIsRejected();
  testMissingEligibleInstanceIsRejected();
  testJointHirMirLiteralForgeryIsRejected();
  return failures == 0 ? 0 : 1;
}
