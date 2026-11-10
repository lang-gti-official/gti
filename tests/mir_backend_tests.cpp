#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"

#include "llvm_backend_test_support.h"

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

// Lowers `mir` against the frontend's canonical source MIR and hands the
// verified lowered program to the shipped backend. Rejection of incoherent
// input is a std::logic_error from lowering or the backend; an unsupported
// backend feature is a different error and is never counted as rejection.
lang::BackendArtifact
emit(const lang::FrontendResult &frontend, const lang::MirProgram &mir,
     const lang::OptimizationResult &compatibility,
     const std::vector<lang::MirTransformRecord> &transformations = {}) {
  static_cast<void>(compatibility);
  return lang::LlvmBackend().generate(gti_test::lowerProgram(
      frontend, frontend.mir, mir, lang::TargetInfo::host(), transformations));
}

const lang::MirFunctionInstance *
sourceFunction(const lang::FrontendResult &frontend,
               const lang::MirProgram &mir, std::string_view name) {
  for (const lang::HirFunctionInstance &function :
       frontend.hir.functionInstances()) {
    if (function.source != nullptr && function.source->name().lexeme == name) {
      return mir.findFunctionInstance(function.id);
    }
  }
  return nullptr;
}

std::size_t identityFoldLiterals(const lang::MirFunctionInstance *function) {
  std::size_t result = 0;
  if (function == nullptr) {
    return result;
  }
  for (const lang::MirBlock &block : function->body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      result += !instruction.tombstone &&
                        instruction.operation == lang::MirOperation::Literal &&
                        instruction.literalProvenance.kind ==
                            lang::MirLiteralProvenanceKind::IdentityFold
                    ? 1U
                    : 0U;
    }
  }
  return result;
}

void testVerifiedMirRouteAndOptimizationCoherence() {
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
  expect(!o0Artifact.contents.empty(),
         "the shipped backend should accept the verified O0 scalar family");
  // The checked component uses only the explicit defined-failure ABI: the
  // checked bodies, and their caller, carry a defined-failure summary while
  // the plain scalar leaves do not.
  for (const std::string_view name :
       {"compatibility_checked", "checked_leaf", "checked_caller"}) {
    const lang::MirFunctionInstance *function =
        sourceFunction(frontend, o0.mir, name);
    expect(function != nullptr && function->mayRaiseDefinedFailure,
           std::string(name) + " should carry a defined-failure summary");
  }
  for (const std::string_view name :
       {"mir_identity", "mir_constant", "mir_i8_identity", "mir_second",
        "mir_i64_max", "mir_u64_max", "mir_noop", "compatibility_identity",
        "compatibility_bool_identity", "compatibility_char_identity"}) {
    const lang::MirFunctionInstance *function =
        sourceFunction(frontend, o0.mir, name);
    expect(function != nullptr && !function->mayRaiseDefinedFailure &&
               function->definitionKind ==
                   lang::MirFunctionInstance::DefinitionKind::Source,
           std::string(name) + " should be a no-fail verified source body");
  }

  const lang::OptimizationResult o1Compatibility =
      pipeline.run(frontend.hir, lang::OptimizationLevel::O1);
  const lang::OptimizedProgram o1 =
      optimize(frontend, lang::OptimizationLevel::O1, o1Compatibility);
  expect(o1.valid() && lang::verifyMirProgram(o1.mir).valid(),
         "the O1 transformed production MIR input should be fully verified");
  if (o1.valid()) {
    expect(!emit(frontend, o1.mir, o1Compatibility, o1.report.transformations)
                .contents.empty(),
           "the shipped backend should accept the verified O1 program");
    expect(identityFoldLiterals(
               sourceFunction(frontend, o0.mir, "mir_constant")) == 0 &&
               identityFoldLiterals(
                   sourceFunction(frontend, o1.mir, "mir_constant")) == 1,
           "the marked constant body should execute the verified O1 "
           "Identity-to-Literal MIR rewrite");

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
         "the backend should reject an unverified MIR program before body "
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
         "the backend should reject a mixed snapshot at entry even when no "
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

} // namespace

// An untyped non-negative literal widening into a larger scalar field
// through a constructor initializer list must lower to verifiable MIR:
// the stage lowering represents the value-preserving conversion
// explicitly, so the coverage verifier's operand-type demand holds.
// (Regression: the seven-line form ICEd with GTI-B0001 while every
// explicit spelling — uint64_t(0), int32 fields, in-class defaults —
// passed, so no corpus source could catch it.)
void testImplicitLiteralWideningConstructorStageVerifies() {
  lang::FrontendResult frontend = analyze("ctor-literal-widening.gti", R"(
class C {
  mut uint64_t a;
public:
  C() : a(0) {}
};
int main() { mut C c = C(); return 0; }
)");
  if (!frontend.canGenerateCode()) {
    expect(false, "the widening-constructor fixture should pass the frontend");
    return;
  }
  expect(lang::verifyMirProgram(frontend.mir).valid(),
         "an implicit literal widening in a constructor initializer must "
         "lower to verifiable MIR");
  const lang::OptimizationResult compatibility =
      lang::OptimizationPipeline().run(frontend.hir,
                                       lang::OptimizationLevel::O0);
  const lang::BackendArtifact artifact =
      emit(frontend, frontend.mir, compatibility);
  expect(!artifact.contents.empty(),
         "the widening constructor's program should reach code generation");
}

int main() {
  testVerifiedMirRouteAndOptimizationCoherence();
  testUnverifiedMirIsRejected();
  testStaleMirSnapshotIsRejected();
  testForgedFunctionHeaderIsRejected();
  testLeafCannotDriftIntoCfgFamily();
  testForgedValidLiteralIsRejected();
  testMissingEligibleInstanceIsRejected();
  testImplicitLiteralWideningConstructorStageVerifies();
  return failures == 0 ? 0 : 1;
}
