#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/mir_editor.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/optimizer.h"

#include "cpp_backend_test_support.h"

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

lang::BackendArtifact
emit(const lang::FrontendResult &frontend, const lang::MirProgram &mir,
     const lang::OptimizationResult &compatibility,
     const std::vector<lang::MirTransformRecord> &transformations = {}) {
  static_cast<void>(compatibility);
  return gti_test::emitCpp(frontend, frontend.mir, mir, transformations);
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
  constexpr std::string_view marker = "// GTI verified-MIR body: scalar-cfg-v1";
  expect(count(o0Artifact.contents,
               "// GTI verified-MIR body: scalar-cfg-v1 function-instance") ==
             10,
         "exactly the ten scalar bodies of this fixture should use general "
         "MIR body emission");
  expect(count(o0Artifact.contents,
               "// GTI verified-MIR body: scalar-cfg-v1 module-instance") == 1,
         "the fixture's empty module body should emit from verified MIR");
  expect(o0Artifact.contents.find("__gti_mir_arg_0") != std::string::npos &&
             o0Artifact.contents.find("__gti_mir_v_") != std::string::npos,
         "the selected definitions should name MIR parameters and SSA values");
  // The checked component uses only the explicit defined-failure ABI. An
  // ordinary wrapper here would terminate before its caller's MIR cleanup.
  expect(
      count(o0Artifact.contents,
            "// GTI verified-MIR body: scalar-cfg-failure-v1") == 4 &&
          functionDefinition(o0Artifact.contents, "compatibility_checked")
              .empty() &&
          o0Artifact.contents.find("compatibility_checked__gti_mir_failure(") !=
              std::string::npos,
      "the checked arithmetic sibling should emit its transformed "
      "failure-form body without an ordinary boundary wrapper");
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

  const lang::OptimizationResult o1Compatibility =
      pipeline.run(frontend.hir, lang::OptimizationLevel::O1);
  const lang::OptimizedProgram o1 =
      optimize(frontend, lang::OptimizationLevel::O1, o1Compatibility);
  expect(o1.valid() && lang::verifyMirProgram(o1.mir).valid(),
         "the O1 transformed production MIR input should be fully verified");
  if (o1.valid()) {
    const lang::BackendArtifact o1Artifact =
        emit(frontend, o1.mir, o1Compatibility, o1.report.transformations);
    const lang::BackendArtifact o1WithO0Compatibility =
        emit(frontend, o1.mir, o0Compatibility, o1.report.transformations);
    expect(count(o1Artifact.contents, "// GTI verified-MIR body: scalar-cfg-v1 "
                                      "function-instance") == 10,
           "the same bodies should remain selected after MIR optimization");
    const std::string_view o0Constant =
        functionDefinition(o0Artifact.contents, "mir_constant");
    const std::string_view o1Constant =
        functionDefinition(o1Artifact.contents, "mir_constant");
    expect(
        o0Constant.find("__gti_mir_v_2 = __gti_mir_v_1;") !=
                std::string_view::npos &&
            o1Constant.find("__gti_mir_v_2 = static_cast<std::int32_t>(42);") !=
                std::string_view::npos &&
            o1Constant.find("__gti_mir_v_2 = __gti_mir_v_1;") ==
                std::string_view::npos,
        "the marked constant body should execute the verified O1 "
        "Identity-to-Literal MIR rewrite");
    expect(count(o1WithO0Compatibility.contents,
                 "// GTI verified-MIR body: scalar-cfg-v1 "
                 "function-instance") == 10 &&
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

void testEnumSwitchLoadIsNotAReturnCopy() {
  auto frontend = analyze("enum-switch-return-copy.gti", R"(
enum class State : uint8_t { cold, hot };
State select(State value) {
  switch (value) {
  case State::cold: return State::hot;
  default: return State::cold;
  }
}
State identity(State value) { return value; }
int main() { return identity(select(State::cold)) == State::hot ? 0 : 1; }
)");
  if (!frontend.canGenerateCode())
    return;
  const auto compatibility = lang::OptimizationPipeline().run(
      frontend.hir, lang::OptimizationLevel::O0);
  const auto artifact = emit(frontend, frontend.mir, compatibility);
  const auto selected = functionDefinition(artifact.contents, "select");
  const auto identity = functionDefinition(artifact.contents, "identity");
  expect(!selected.empty() &&
             selected.find("switch (") != std::string_view::npos &&
             selected.find(" = __gti_mir_p_1;") != std::string_view::npos &&
             selected.find("spells at its Return") == std::string_view::npos,
         "a switch subject load must execute even when its type matches the "
         "function result");
  expect(!identity.empty() &&
             identity.find("return __gti_mir_p_1;") != std::string_view::npos,
         "the actual unique Return still supports direct copy publication");
}

} // namespace

int main() {
  testVerifiedMirRouteAndOptimizationCoherence();
  testJointHirMirLiteralForgeryIsRejected();
  testEnumSwitchLoadIsNotAReturnCopy();
  return failures == 0 ? 0 : 1;
}
