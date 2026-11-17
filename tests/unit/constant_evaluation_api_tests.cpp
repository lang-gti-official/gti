// Constant-evaluation library APIs with no source-level observable: the
// binary32 float kernel, direct MIR constant execution and staging
// (evaluateMirConstantFunction/Constructor, HirLowerer::lowerConstantFunction),
// staged intrinsic contracts, request-local evaluation reuse (on/off
// equivalence and work metrics) and native stack segments. (Moved unchanged
// from the former compiler_constexpr_tests.cpp; its GTI-source cases became
// tests/ui/constexpr files.)
#include "../llvm_backend_test_support.h"
#include "../lowered_program_test_support.h"
#include "../temporary_directory.h"
#include "gti/binary_float.h"
#include "gti/frontend.h"
#include "gti/lexer.h"
#include "gti/lowered_program_builder.h"
#include "gti/mir_constant_evaluator.h"
#include "gti/mir_lowering.h"
#include "gti/mir_printer.h"
#include "gti/mir_verifier.h"
#include "gti/semantic_analyzer.h"
#include "gti/source_loader.h"
#include "gti/support.h"
#include "unit_test.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

[[maybe_unused]] std::filesystem::path standardLibraryPrelude() {
  return std::filesystem::path(__FILE__)
             .parent_path()
             .parent_path()
             .parent_path() /
         "stdlib/prelude.gti";
}

[[maybe_unused]] std::filesystem::path standardLibraryRoot() {
  return standardLibraryPrelude().parent_path();
}

[[maybe_unused]] bool
hasDiagnostic(const std::vector<lang::Diagnostic> &diagnostics,
              const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

[[maybe_unused]] bool
hasDiagnosticCode(const std::vector<lang::Diagnostic> &diagnostics,
                  const std::string &code) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

[[maybe_unused]] const lang::Diagnostic *
findDiagnosticByCode(const std::vector<lang::Diagnostic> &diagnostics,
                     const std::string &code) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      return &diagnostic;
    }
  }
  return nullptr;
}

[[maybe_unused]] bool
hasDiagnosticAt(const std::vector<lang::Diagnostic> &diagnostics,
                std::string_view code, std::string_view source,
                std::size_t start, std::string_view reason) {
  const bool matched =
      std::any_of(diagnostics.begin(), diagnostics.end(),
                  [&](const lang::Diagnostic &diagnostic) {
                    const std::string_view actualSource =
                        diagnostic.primary.source;
                    const std::string actualFile =
                        std::filesystem::path(diagnostic.primary.source)
                            .filename()
                            .string();
                    return diagnostic.code == code &&
                           (actualSource == source || actualFile == source) &&
                           diagnostic.primary.start == start &&
                           diagnostic.message.find(reason) != std::string::npos;
                  });
  if (!matched) {
    std::cerr << "Expected diagnostic " << code << " at " << source << ':'
              << start << " containing '" << reason << "'.\n";
    for (const lang::Diagnostic &diagnostic : diagnostics) {
      std::cerr << "  found " << diagnostic.code << " at "
                << diagnostic.primary.source << ':' << diagnostic.primary.start
                << ": " << diagnostic.message << '\n';
    }
  }
  return matched;
}

[[maybe_unused]] bool
hasMirVerificationError(const lang::MirVerificationResult &result,
                        std::string_view reason) {
  return std::any_of(result.errors.begin(), result.errors.end(),
                     [&](const lang::MirVerificationError &error) {
                       return error.message.find(reason) != std::string::npos;
                     });
}

[[maybe_unused]] bool
hasRelatedDiagnostic(const std::vector<lang::Diagnostic> &diagnostics,
                     const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    for (const lang::RelatedDiagnostic &related : diagnostic.related) {
      if (related.message.find(text) != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

[[maybe_unused]] bool
hasDiagnosticHint(const std::vector<lang::Diagnostic> &diagnostics,
                  const std::string &text) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    for (const std::string &hint : diagnostic.hints) {
      if (hint.find(text) != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}

[[maybe_unused]] std::size_t
countDiagnosticCode(const lang::SemanticVisitor &semantic,
                    const std::string &code) {
  std::size_t count = 0;
  for (const lang::SemanticDiagnostic &diagnostic : semantic.errors()) {
    if (diagnostic.code == code) {
      ++count;
    }
  }
  return count;
}

[[maybe_unused]] std::size_t
countDiagnosticCode(const std::vector<lang::Diagnostic> &diagnostics,
                    const std::string &code) {
  std::size_t count = 0;
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.code == code) {
      ++count;
    }
  }
  return count;
}

[[maybe_unused]] std::size_t
countDiagnosticContaining(const std::vector<lang::Diagnostic> &diagnostics,
                          const std::string &text) {
  return static_cast<std::size_t>(
      std::count_if(diagnostics.begin(), diagnostics.end(),
                    [&](const lang::Diagnostic &item) {
                      return item.message.find(text) != std::string::npos;
                    }));
}

// Backend-neutral oracles: assertions about emitted programs are made on the
// verified MIR (and the shipped LLVM backend), never on generated C++ text.
[[maybe_unused]] const lang::MirFunctionInstance *
mirFunctionNamed(const lang::FrontendResult &frontend,
                 const lang::MirProgram &program,
                 std::string_view qualifiedName) {
  for (const lang::MirFunctionInstance &function :
       program.functionInstances()) {
    const lang::FunctionInfo *info =
        frontend.semantics.findFunction(function.declaration);
    if (info != nullptr && info->qualifiedName == qualifiedName) {
      return &function;
    }
  }
  return nullptr;
}

[[maybe_unused]] const lang::MirFunctionInstance *
mirFunctionNamed(const lang::FrontendResult &frontend,
                 std::string_view qualifiedName) {
  return mirFunctionNamed(frontend, frontend.mir, qualifiedName);
}

[[maybe_unused]] const lang::MirClassInstance *
mirClassNamed(const lang::FrontendResult &frontend,
              std::string_view qualifiedName) {
  for (const lang::MirClassInstance &instance : frontend.mir.classInstances()) {
    const lang::ClassTypeInfo *info =
        frontend.semantics.findClassType(instance.declaration);
    if (info != nullptr && info->qualifiedName == qualifiedName) {
      return &instance;
    }
  }
  return nullptr;
}

template <typename Predicate>
[[maybe_unused]] bool anyMirInstruction(const lang::MirBody &body,
                                        Predicate predicate) {
  for (const lang::MirBlock &block : body.blocks) {
    for (const lang::MirInstruction &instruction : block.instructions) {
      if (!instruction.tombstone && predicate(instruction)) {
        return true;
      }
    }
  }
  return false;
}

// Every executable body of `program`: functions, constructors, destructors,
// lambdas, class field initializers and the merged program module.
template <typename Visitor>
[[maybe_unused]] void forEachMirBody(const lang::MirProgram &program,
                                     Visitor visitor) {
  visitor(program.module());
  for (const auto &function : program.functionInstances())
    visitor(function.body);
  for (const auto &constructor : program.constructorInstances())
    visitor(constructor.body);
  for (const auto &destructor : program.destructorInstances())
    visitor(destructor.body);
  for (const auto &lambda : program.lambdaInstances())
    visitor(lambda.body);
  for (const auto &instance : program.classInstances()) {
    visitor(instance.fieldInitializers);
    visitor(instance.staticFieldInitializers);
  }
}

template <typename Predicate>
[[maybe_unused]] bool anyProgramInstruction(const lang::MirProgram &program,
                                            Predicate predicate) {
  bool found = false;
  forEachMirBody(program, [&](const lang::MirBody &body) {
    found = found || anyMirInstruction(body, predicate);
  });
  return found;
}

[[maybe_unused]] bool isMirIntegerLiteral(const lang::MirInstruction &item,
                                          std::uint64_t magnitude) {
  if (item.operation != lang::MirOperation::Literal || !item.literal)
    return false;
  const auto *value = std::get_if<std::uint64_t>(&*item.literal);
  return value != nullptr && *value == magnitude;
}

[[maybe_unused]] bool hasMirIntegerLiteral(const lang::MirBody &body,
                                           std::uint64_t magnitude) {
  return anyMirInstruction(body, [&](const lang::MirInstruction &item) {
    return isMirIntegerLiteral(item, magnitude);
  });
}

[[maybe_unused]] bool hasMirIntegerLiteral(const lang::MirProgram &program,
                                           std::uint64_t magnitude) {
  return anyProgramInstruction(program, [&](const lang::MirInstruction &item) {
    return isMirIntegerLiteral(item, magnitude);
  });
}

// Text reaching the program as a MIR string literal or a constant program
// storage initializer.
[[maybe_unused]] bool mirContainsText(const lang::MirProgram &program,
                                      std::string_view text) {
  const bool literal =
      anyProgramInstruction(program, [&](const lang::MirInstruction &item) {
        const auto *value =
            item.literal ? std::get_if<std::string>(&*item.literal) : nullptr;
        return value != nullptr && value->find(text) != std::string::npos;
      });
  const auto &steps = program.programInitializationPlan().steps;
  return literal ||
         std::any_of(steps.begin(), steps.end(), [&](const auto &step) {
           const auto *value =
               step.dataConstant ? std::get_if<std::string>(&*step.dataConstant)
                                 : nullptr;
           return value != nullptr && value->find(text) != std::string::npos;
         });
}

// The defined-failure outcomes an instruction's checked operation can raise.
[[maybe_unused]] std::vector<lang::DefinedFailureOutcome>
mirFailureOutcomes(const lang::MirProgram &program,
                   const lang::MirInstruction &instruction) {
  std::vector<lang::DefinedFailureOutcome> outcomes;
  for (const lang::FailureSiteId site : instruction.localFailureSites) {
    for (const lang::FailureSiteDescriptor &descriptor :
         program.failureMetadata().sites()) {
      if (descriptor.id == site) {
        outcomes.insert(outcomes.end(), descriptor.outcomes.begin(),
                        descriptor.outcomes.end());
      }
    }
  }
  return outcomes;
}

// A Compute of `operation` whose checked evaluation can raise exactly the
// given defined failure (and, when `type` is not Unknown, in that domain).
[[maybe_unused]] bool hasCheckedMirOperation(
    const lang::MirProgram &program, const lang::MirBody &body,
    lang::MirOperation operation, lang::DefinedFailureCode code,
    lang::DefinedFailureDetail detail,
    const lang::SemanticType &type = lang::SemanticType::Unknown) {
  return anyMirInstruction(body, [&](const lang::MirInstruction &item) {
    if (item.operation != operation ||
        (type != lang::SemanticType::Unknown && item.info.type != type))
      return false;
    const auto outcomes = mirFailureOutcomes(program, item);
    return std::any_of(outcomes.begin(), outcomes.end(), [&](auto outcome) {
      return outcome.code == code && outcome.detail == detail;
    });
  });
}

[[maybe_unused]] bool hasCheckedMirOperation(
    const lang::MirProgram &program, lang::MirOperation operation,
    lang::DefinedFailureCode code, lang::DefinedFailureDetail detail,
    const lang::SemanticType &type = lang::SemanticType::Unknown) {
  bool found = false;
  forEachMirBody(program, [&](const lang::MirBody &body) {
    found = found || hasCheckedMirOperation(program, body, operation, code,
                                            detail, type);
  });
  return found;
}

// A Compute of `operation` with no checked failure: it lowers directly.
[[maybe_unused]] bool hasUncheckedMirOperation(const lang::MirBody &body,
                                               lang::MirOperation operation) {
  return anyMirInstruction(body, [&](const lang::MirInstruction &item) {
    return item.kind == lang::MirInstructionKind::Compute &&
           item.operation == operation && item.localFailureSites.empty();
  });
}

[[maybe_unused]] bool hasUncheckedMirOperation(const lang::MirProgram &program,
                                               lang::MirOperation operation) {
  bool found = false;
  forEachMirBody(program, [&](const lang::MirBody &body) {
    found = found || hasUncheckedMirOperation(body, operation);
  });
  return found;
}

[[maybe_unused]] bool
callsMirFunction(const lang::MirBody &body,
                 const lang::MirFunctionInstance *callee) {
  return callee != nullptr &&
         anyMirInstruction(body, [&](const lang::MirInstruction &item) {
           return item.kind == lang::MirInstructionKind::Call &&
                  item.functionTarget == callee->id;
         });
}

[[maybe_unused]] const lang::MirProgramInitializationStep *
mirStorageStep(const lang::FrontendResult &frontend,
               const lang::MirProgram &program,
               std::string_view qualifiedName) {
  for (const auto &step : program.programInitializationPlan().steps) {
    const lang::SymbolRecord *symbol =
        frontend.semantics.database().findSymbol(step.symbol);
    if (symbol != nullptr && symbol->qualifiedName == qualifiedName) {
      return &step;
    }
  }
  return nullptr;
}

[[maybe_unused]] const lang::MirProgramInitializationStep *
mirStorageStep(const lang::FrontendResult &frontend,
               std::string_view qualifiedName) {
  return mirStorageStep(frontend, frontend.mir, qualifiedName);
}

// The exact constant integer a program-storage step publishes as data.
[[maybe_unused]] bool
storesMirInteger(const lang::MirProgramInitializationStep *step, bool negative,
                 std::uint64_t magnitude) {
  const auto *value =
      step == nullptr || !step->dataConstant
          ? nullptr
          : std::get_if<lang::ConstantInteger>(&*step->dataConstant);
  return value != nullptr && value->negative == negative &&
         value->magnitude == magnitude;
}

// True when the shipped native backend accepts the program; reports why not.
[[maybe_unused]] bool
llvmAccepts(const lang::FrontendResult &frontend,
            lang::OptimizationLevel level = lang::OptimizationLevel::O0,
            const lang::TargetInfo &target = lang::TargetInfo::host()) {
  std::string error;
  const bool emitted = gti_test::llvmEmits(frontend, level, target, &error);
  if (!emitted) {
    std::cerr << "LLVM emission failed: " << error << '\n';
  }
  return emitted;
}

// Exhaustion cases written against a 4096-step request budget keep that
// budget explicitly; the default is far larger (see testCompileLimits).
constexpr std::size_t smallStepBudget = 4096;

lang::FrontendOptions stepLimited(std::size_t steps = smallStepBudget) {
  lang::FrontendOptions options;
  options.limits.constexprSteps = steps;
  return options;
}

std::filesystem::path fixtureSourcePath(const std::filesystem::path &path) {
  // Derive the identity through public source loading, not a second path
  // normalizer or a platform-dependent weakly_canonical(relative) call.
  lang::SourceLoader loader;
  const auto graph = loader.load(path, std::string{});
  const auto *entry = graph.findUnit(graph.entryUnit());
  expect(
      !loader.hadError() && entry != nullptr && entry->path.is_absolute(),
      "diagnostic fixture paths retain the source loader's absolute identity");
  return entry ? entry->path : path;
}

// A string-view recursive-descent scan that is exactly `frames` constexpr
// calls deep: one per '(' plus the call that reads the terminator.
std::string recursiveDescentSource(std::size_t frames) {
  std::string text(frames - 1, '(');
  text += 'x';
  return "constexpr int32_t depth_of(std::string_view text, int32_t at) {\n"
         "  if (text[at] == '(') { return depth_of(text, at + 1) + 1; }\n"
         "  return 0;\n"
         "}\n"
         "static_assert(depth_of(\"" +
         text + "\", 0) == " + std::to_string(frames - 1) +
         ");\n"
         "int main() { return 0; }\n";
}

// `levels` nested staged evaluations: each concrete instance's compile-time
// branch needs the next instance's value before it can be analysed.
std::string stagedChainSource(std::size_t levels) {
  return "constexpr int32_t chain<uint64_t N>() {\n"
         "  if constexpr (N == 0) { return 0; }\n"
         "  else {\n"
         "    if constexpr (chain<N - 1>() >= 0) { return int32_t(N); }\n"
         "    else { return -1; }\n"
         "  }\n"
         "}\n"
         "static_assert(chain<" +
         std::to_string(levels - 1) + ">() == " + std::to_string(levels - 1) +
         ");\n"
         "int main() { return 0; }\n";
}

lang::FrontendResult analyzeWithLimits(const std::string &name,
                                       const std::string &source,
                                       lang::CompileLimits limits = {}) {
  lang::FrontendOptions options;
  options.limits = limits;
  return lang::Frontend(options).analyze(
      name, source, {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
}

std::string diagnosticText(const std::vector<lang::Diagnostic> &diagnostics) {
  std::string text;
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    text += diagnostic.code + " " + std::to_string(diagnostic.primary.start) +
            "-" + std::to_string(diagnostic.primary.end) + " " +
            diagnostic.message + "\n";
    for (const auto &related : diagnostic.related)
      text += "  " + std::to_string(related.span.start) + " " +
              related.message + "\n";
    for (const auto &hint : diagnostic.hints)
      text += "  hint " + hint + "\n";
  }
  return text;
}

std::vector<lang::Diagnostic>
errorsOf(const std::vector<lang::Diagnostic> &diagnostics) {
  std::vector<lang::Diagnostic> errors;
  for (const auto &diagnostic : diagnostics)
    if (diagnostic.severity == lang::DiagnosticSeverity::Error)
      errors.push_back(diagnostic);
  return errors;
}

bool sameDiagnostics(const std::vector<lang::Diagnostic> &left,
                     const std::vector<lang::Diagnostic> &right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](const auto &a, const auto &b) {
                      return a.code == b.code && a.message == b.message &&
                             a.primary.start == b.primary.start &&
                             a.primary.end == b.primary.end &&
                             a.hints == b.hints;
                    });
}

// A user-defined accumulator, so the limit and reuse rules are proven with
// ordinary source rather than a library type.
const std::string kCompileLimitsPrelude = R"(
struct Tally {
  uint64_t count;
  uint64_t sum;
  constexpr Tally(uint64_t c, uint64_t s) : count(c), sum(s) {}
};
constexpr Tally accumulate(uint64_t n, uint64_t seed) {
  mut uint64_t count = 0;
  mut uint64_t sum = seed;
  while (count < n) {
    count = count + 1;
    sum = sum + count;
  }
  return Tally(count, sum);
}
constexpr uint64_t spin(uint64_t n) { return accumulate(n, uint64_t(0)).count; }
)";

void testBinaryFloatApi() {
  lang::Lexer lexer;
  const std::vector<lang::Token> rounded =
      lexer.scan("0.1 1.000000059604644775390625 "
                 "1.0000000596046447753906250000000000000001 0.0");
  const auto floatBits = [&rounded](std::size_t index) {
    const auto *value =
        index < rounded.size()
            ? std::get_if<lang::BinaryFloat>(&rounded[index].literal)
            : nullptr;
    return value == nullptr ? std::optional<std::uint32_t>{}
                            : std::optional<std::uint32_t>{value->bits};
  };
  expect(!lexer.hadError() && floatBits(0) == 0x3dcccccdU &&
             floatBits(1) == 0x3f800000U && floatBits(2) == 0x3f800001U &&
             floatBits(3) == 0U,
         "floating literals should parse directly to binary32 with exact "
         "round-to-nearest, ties-to-even semantics");

  (void)lexer.scan("9999999999999999999999999999999999999999.0");
  expect(lexer.hadError() && lexer.errors().size() == 1 &&
             lexer.errors().front().code == "GTI-L0006" &&
             lexer.errors().front().message.find("finite binary32 range") !=
                 std::string::npos,
         "a decimal literal that overflows binary32 should receive a focused "
         "lexical diagnostic");

  const lang::BinaryFloat tenth = *lang::parseBinaryFloat("0.1").value;
  const lang::BinaryFloat fifth = *lang::parseBinaryFloat("0.2").value;
  const lang::BinaryFloat zero = *lang::parseBinaryFloat("0.0").value;
  const lang::BinaryFloat one = *lang::parseBinaryFloat("1.0").value;
  const lang::BinaryFloat sum =
      lang::evaluateBinaryFloat(lang::BinaryFloatOperation::Add, tenth, fifth);
  const lang::BinaryFloat negativeZero = lang::negateBinaryFloat(zero);
  const lang::BinaryFloat negativeInfinity = lang::evaluateBinaryFloat(
      lang::BinaryFloatOperation::Divide, one, negativeZero);
  const lang::BinaryFloat nan =
      lang::evaluateBinaryFloat(lang::BinaryFloatOperation::Divide, zero, zero);
  const lang::BinaryFloat overflow = lang::evaluateBinaryFloat(
      lang::BinaryFloatOperation::Multiply, {0x7f7fffffU}, {0x40000000U});
  const lang::BinaryFloat underflowTie = lang::evaluateBinaryFloat(
      lang::BinaryFloatOperation::Divide, {0x00000001U}, {0x40000000U});
  expect(sum.bits == 0x3e99999aU && negativeZero.bits == 0x80000000U &&
             negativeInfinity.bits == 0xff800000U &&
             overflow.bits == 0x7f800000U && underflowTie.bits == 0U &&
             lang::compareBinaryFloat(zero, negativeZero) ==
                 lang::BinaryFloatOrdering::Equal &&
             lang::compareBinaryFloat(nan, nan) ==
                 lang::BinaryFloatOrdering::Unordered,
         "APFloat-backed operations should preserve binary32 rounding, signed "
         "zero, infinities, and unordered NaN comparison");

  const lang::ConstantValue nanConstant{nan};
  const lang::ConstantEvaluation nanEqual = lang::evaluateConstantComparison(
      lang::TokenKind::EQUAL_EQUAL, nanConstant, nanConstant);
  const lang::ConstantEvaluation nanNotEqual = lang::evaluateConstantComparison(
      lang::TokenKind::BANG_EQUAL, nanConstant, nanConstant);
  const lang::ConstantEvaluation nanLess = lang::evaluateConstantComparison(
      lang::TokenKind::LESS, nanConstant, nanConstant);
  expect(nanEqual && !std::get<bool>(*nanEqual.value) && nanNotEqual &&
             std::get<bool>(*nanNotEqual.value) && nanLess &&
             !std::get<bool>(*nanLess.value),
         "constant NaN comparisons should follow IEEE unordered predicates");

  const std::optional<lang::BinaryFloat> roundedInteger =
      lang::integerToBinaryFloat({.magnitude = 16777217},
                                 {.width = 32, .signedValue = true});
  const lang::BinaryFloat threePointNine = *lang::parseBinaryFloat("3.9").value;
  const std::optional<lang::CheckedIntegerValue> truncated =
      lang::binaryFloatToInteger(threePointNine,
                                 {.width = 32, .signedValue = true});
  const std::optional<lang::CheckedIntegerValue> signed32Minimum =
      lang::binaryFloatToInteger({0xcf000000U},
                                 {.width = 32, .signedValue = true});
  const std::optional<lang::CheckedIntegerValue> unsigned32Value =
      lang::binaryFloatToInteger({0x4f000000U},
                                 {.width = 32, .signedValue = false});
  const std::optional<lang::CheckedIntegerValue> signed64Minimum =
      lang::binaryFloatToInteger({0xdf000000U},
                                 {.width = 64, .signedValue = true});
  expect(roundedInteger && roundedInteger->bits == 0x4b800000U && truncated &&
             !truncated->negative && truncated->magnitude == 3 &&
             signed32Minimum && signed32Minimum->negative &&
             signed32Minimum->magnitude == (std::uint64_t{1} << 31) &&
             unsigned32Value && !unsigned32Value->negative &&
             unsigned32Value->magnitude == (std::uint64_t{1} << 31) &&
             signed64Minimum && signed64Minimum->negative &&
             signed64Minimum->magnitude == (std::uint64_t{1} << 63) &&
             !lang::binaryFloatToInteger({0x4f000000U},
                                         {.width = 32, .signedValue = true}) &&
             !lang::binaryFloatToInteger({0x4f800000U},
                                         {.width = 32, .signedValue = false}) &&
             !lang::binaryFloatToInteger({0x5f000000U},
                                         {.width = 64, .signedValue = true}) &&
             !lang::binaryFloatToInteger({0x5f800000U},
                                         {.width = 64, .signedValue = false}) &&
             !lang::binaryFloatToInteger({0x7f800000U},
                                         {.width = 32, .signedValue = true}),
         "integer/float conversion should round through APFloat, preserve "
         "signed and unsigned boundaries, and reject out-of-range or "
         "non-finite conversions");
}

void testStagedIntrinsicContracts() {
  const std::string source = R"(
constexpr int staged_expected() {
  expected<int, int> value = 42;
  return value.value();
}
int main() { return staged_expected() - 42; }
)";
  const auto frontend = lang::Frontend().analyze(
      "staged-intrinsic-contract.gti", source, {standardLibraryPrelude()}, {},
      {standardLibraryRoot()});
  const auto foreign = lang::Frontend().analyze(
      "foreign-staged-intrinsic-contract.gti", source,
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode() && foreign.canGenerateCode(),
         "staged intrinsic fixture has valid ordinary runtime MIR");
  if (!frontend.canGenerateCode() || !foreign.canGenerateCode())
    return;
  lang::SemanticVisitor provider(lang::TargetInfo::host(),
                                 &frontend.sourceGraph, false, nullptr,
                                 &frontend.sources);
  expect(provider.check(frontend.program),
         "staged intrinsic provider checks completed source declarations");
  std::vector<lang::FunctionId> completed;
  std::vector<lang::ConstructorId> constructors;
  lang::FunctionId root = 0;
  for (const auto &function : frontend.hir.functionInstances()) {
    const auto *declaration =
        function.source ? provider.model().findFunction(*function.source)
                        : nullptr;
    if (!declaration || !function.source->isConstexpr())
      continue;
    completed.push_back(declaration->id);
    if (function.source->name().lexeme == "staged_expected")
      root = declaration->id;
  }
  for (const auto &constructor : frontend.hir.constructorInstances()) {
    const auto *declaration =
        constructor.source
            ? provider.model().findConstructor(*constructor.source)
            : nullptr;
    if (declaration && constructor.source->isConstexpr())
      constructors.push_back(declaration->id);
  }
  auto staged = lang::HirLowerer().lowerConstantFunction(
      provider.model(), provider, root, completed, {}, constructors);
  expect(staged.valid() &&
             staged.program.analysisSeal().activeStatements.empty() &&
             staged.program.analysisSeal().programSnapshot ==
                 provider.model().analysisSeal().programSnapshot,
         "staged declaration proofs retain identity without whole-program "
         "sealing");
  const auto *entry =
      frontend.sourceGraph.findUnit(frontend.sourceGraph.entryUnit());
  const auto metadata = lang::FailureMetadataBuilder().build(
      frontend.sourceGraph, frontend.sources, staged.program, entry->path);
  const auto mir = lang::MirLowerer().lower(staged.program, metadata.metadata);
  expect(metadata.valid() && mir.valid(),
         "tracked intrinsics in a completed constexpr closure verify");
  if (!metadata.valid() || !mir.valid())
    return;
  unsigned mutations = 0;
  for (const auto &function : mir.program.functionInstances()) {
    if (function.declaration == root) {
      const auto value =
          lang::evaluateMirConstantFunction(mir.program, function.id, {});
      expect(
          value &&
              std::get<lang::ConstantInteger>(*value.value).magnitude == 42,
          "staged checked result access executes under its declaration proof");
    }
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions) {
        if (!instruction.intrinsicCallContract)
          continue;
        const auto foreignContract = foreign.semantics.functionControlContract(
            instruction.intrinsicCallContract->declaration());
        expect(foreignContract &&
                   foreignContract->intrinsic() == instruction.intrinsic &&
                   foreignContract->snapshot() !=
                       instruction.intrinsicCallContract->snapshot(),
               "foreign analysis supplies an independent declaration witness");
        if (!foreignContract ||
            foreignContract->intrinsic() != instruction.intrinsic)
          continue;
        auto malformed = mir.program;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        for (auto &candidateBlock : changed->body.blocks)
          for (auto &candidate : candidateBlock.instructions)
            if (candidate.id == instruction.id)
              candidate.intrinsicCallContract = foreignContract;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "staged intrinsic contracts reject a foreign semantic snapshot");
        ++mutations;
      }
  }
  expect(mutations != 0,
         "staged fixture exercises intrinsic snapshot mutations");
  auto missingIdentity = staged.program;
  const_cast<lang::SemanticAnalysisSeal &>(missingIdentity.analysisSeal())
      .programSnapshot = 0;
  expect(!lang::MirLowerer().lower(missingIdentity, metadata.metadata).valid(),
         "staged lowering cannot erase declaration proof identity");
  expect(
      !lang::verifyHirProgramPlans(provider.model(), staged.program).valid() &&
          !lang::LoweredProgramBuilder()
               .build(frontend.program, provider.model(), staged.program,
                      mir.program, mir.program, {}, lang::TargetInfo::host())
               .valid(),
      "preserving staged identity never permits whole-program publication");
}

void testMirConstantEvaluation() {
  lang::FrontendWorkMetrics metrics;
  const auto frontend =
      lang::Frontend({.workMetrics = &metrics})
          .analyze("mir-constexpr.gti", R"(
constexpr int32_t sum(int32_t count) {
  mut int32_t total = 0;
  for (mut int32_t index = 0; index < count; index++) { total += index; }
  return total;
}
constexpr int32_t factorial(int32_t count) {
  if (count <= 1) { return 1; }
  return count * factorial(count - 1);
}
constexpr int32_t call_sum(int32_t count) { return sum(count); }
constexpr int table_sum(int seed) {
  mut int values[3] = {seed, seed + 1, seed + 2};
  values[1] += 4;
  mut int total = 0;
  for (mut uint64_t index = 0; index < 3; index++) { total += values[index]; }
  return total;
}
constexpr int array_defaults(int seed) {
  mut int values[3] = {};
  values[1] = seed;
  values[1]++;
  return values[0] + values[1] + values[2];
}
constexpr int array_bounds(int index) {
  int values[3] = {1, 2, 3};
  return values[index];
}
constexpr int array_write_bounds(int index) {
  mut int values[3] = {};
  values[index] = 4;
  return values[0];
}
constexpr int nested_bounds(int row) {
  int values[2][2] = {};
  return values[row][1 / (row - row)];
}
constexpr int large_array(int seed) {
  int values[10000] = {};
  return values[seed];
}
constexpr uint64_t checksum(std::string_view text) {
  mut uint64_t result = 0;
  for (mut uint64_t index = 0; index < text.size(); index++) {
    result += uint8_t(text[index]);
  }
  return result;
}
constexpr double half(double input) { return input / 2; }
constexpr bool folded(int32_t input) { return 2 == 2 || input > 0; }
constexpr T twice<std::numeric T>(T input) { return T(input + input); }
constexpr int32_t divide(int32_t input) { return 100 / input; }
constexpr int32_t endless(int32_t count) {
  while (count > 0) {}
  return count;
}
constexpr int32_t recursion(int32_t count) { return recursion(count); }
int32_t runtime(int32_t input) { return input; }
constexpr int32_t lazy(bool selected) {
  if (selected) { return 42; }
  return runtime(0);
}
constexpr int32_t sum_result = sum(5);
constexpr int32_t factorial_result = factorial(6);
constexpr uint64_t text_result = checksum("a\0é");
static_assert(sum_result == 10 && factorial_result == 720 && text_result == 461);
static_assert(table_sum(5) == 22 && array_defaults(4) == 5);
int main() { return twice<int32_t>(21) == 42 ? 0 : 1; }
)",
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(frontend.canGenerateCode(), "MIR constexpr fixture should lower");
  expect(metrics.constexprMirExecutions >= 5,
         "semantic constant calls actually use staged MIR before whole-program "
         "lowering");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    return;
  }
  const auto functionId = [&](std::string_view name) {
    for (const auto &function : frontend.hir.functionInstances()) {
      if (function.source != nullptr && function.source->name().lexeme == name)
        return function.id;
    }
    return lang::HirFunctionInstanceId{0};
  };
  const auto integer = [](std::uint64_t value) -> lang::ConstantValue {
    return lang::makeConstantInteger({.magnitude = value},
                                     {.width = 32, .signedValue = true});
  };
  const auto evaluate = [&](std::string_view name, lang::ConstantValue argument,
                            lang::MirConstantLimits limits = {}) {
    const std::array arguments{argument};
    auto result = lang::evaluateMirConstantFunction(
        frontend.mir, functionId(name), arguments, limits);
    return result;
  };
  for (const auto &[name, input, expected] :
       {std::tuple{"sum", 5, 10}, std::tuple{"factorial", 6, 720},
        std::tuple{"table_sum", 5, 22}, std::tuple{"array_defaults", 4, 5}}) {
    const auto result = evaluate(name, integer(input));
    if (!result)
      std::cerr << name << ": " << result.detail << " at " << result.block
                << ':' << result.instruction << '\n';
    expect(result && result.value == integer(expected),
           "canonical MIR execution matches frontend scalar constants");
  }
  for (const auto name : {"array_bounds", "array_write_bounds"}) {
    const auto bounds = evaluate(name, integer(3));
    expect(!bounds &&
               bounds.failure ==
                   lang::ConstantEvaluationFailure::InvalidOperands &&
               bounds.instruction != 0,
           "MIR array reads and writes reject out-of-range indices at the "
           "operation");
    const auto negative = evaluate(
        name, lang::makeConstantInteger({.negative = true, .magnitude = 1},
                                        {.width = 32, .signedValue = true}));
    expect(!negative && negative.failure ==
                            lang::ConstantEvaluationFailure::InvalidOperands,
           "MIR array reads and writes reject negative indices");
  }
  const auto earlyBounds = evaluate("nested_bounds", integer(2));
  const auto laterDivision = evaluate("nested_bounds", integer(0));
  expect(!earlyBounds &&
             earlyBounds.failure ==
                 lang::ConstantEvaluationFailure::InvalidOperands &&
             !laterDivision &&
             laterDivision.failure ==
                 lang::ConstantEvaluationFailure::DivisionByZero,
         "constant array prefixes fail before later index effects using the "
         "same canonical checked-place schedule as runtime");
  const auto *nestedBody =
      frontend.mir.findFunctionInstance(functionId("nested_bounds"));
  bool checkedAddress = false;
  if (nestedBody != nullptr) {
    for (const auto &block : nestedBody->body.blocks)
      for (const auto &instruction : block.instructions)
        checkedAddress |=
            instruction.id == earlyBounds.instruction &&
            instruction.operation == lang::MirOperation::AddressOf &&
            instruction.result &&
            nestedBody->body.usesOf(*instruction.result).empty();
  }
  expect(checkedAddress,
         "constant prefix failures identify the unused checked address stage "
         "without creating a constant raw pointer");
  const auto large =
      evaluate("large_array", integer(0), {.steps = smallStepBudget});
  expect(!large &&
             large.failure == lang::ConstantEvaluationFailure::ResourceLimit,
         "default array materialization is bounded before allocating storage");
  lang::FrontendWorkMetrics arrayMetrics;
  const auto arrayProgram =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics,
                      .workMetrics = &arrayMetrics})
          .analyze("early-array-constant.gti", R"(
constexpr int array_copy(int seed) {
  mut int matrix[2][3] = {};
  matrix[1][2] = seed;
  mut int copy[2][3] = matrix;
  copy[1][2]++;
  matrix[0] = {1, 2, 3};
  return matrix[1][2] + copy[1][2] + matrix[0][2];
}
constexpr int array_argument(int values[3]) {
  mut int copy[3] = values;
  copy[0]++;
  return copy[0] + values[0];
}
constexpr int table<uint64_t N>(int seed) {
  mut int values[N] = {};
  values[values.size() - 1] = seed;
  return values[values.size() - 1];
}
static_assert(array_copy(5) == 14);
static_assert(array_argument({2, 3, 4}) == 5);
static_assert(table<3>(7) == 7);
)");
  expect(arrayProgram.semanticValid &&
             arrayMetrics.constexprMirExecutions == 3 && !arrayProgram.hirValid,
         "nested array mutation, copy isolation, array arguments and generic "
         "extents execute early MIR");
  for (const bool write : {false, true}) {
    lang::FrontendWorkMetrics boundsMetrics;
    const std::string source =
        "constexpr int bounds(int index) { mut int values[3] = {}; " +
        std::string(write ? "values[index] = 7; return 0; }\n"
                          : "return values[index]; }\n") +
        "constexpr int bad = bounds(3);\n";
    const auto invalid = lang::Frontend({.workMetrics = &boundsMetrics})
                             .analyze("constant-array-bounds.gti", source);
    const auto *diagnostic =
        findDiagnosticByCode(invalid.diagnostics, "GTI-S2057");
    expect(!invalid.semanticValid &&
               boundsMetrics.constexprMirExecutions == 1 &&
               diagnostic != nullptr && diagnostic->primary.line == 1,
           "constant array bounds errors reject at the executing body, without "
           "AST fallback");
  }
  const auto text = evaluate("checksum", std::string("a\0é", 4));
  if (!text)
    std::cerr << "checksum: " << text.detail << " at " << text.block << ':'
              << text.instruction << '\n';
  expect(text && lang::constantUnsignedMagnitude(*text.value) == 461,
         "MIR text inspection shares byte semantics with the early kernel");
  const auto lazy = evaluate("lazy", true);
  const auto generic = evaluate("twice", integer(21));
  expect(generic && generic.value == integer(42),
         "MIR constant calls retain concrete generic substitution and "
         "conversions");
  const auto floating =
      lang::convertConstantFloat(integer(3), lang::BinaryFloatFormat::Binary64);
  const auto half = evaluate("half", *floating.value);
  expect(half && std::get<lang::BinaryFloat>(*half.value).bits ==
                     0x3ff8000000000000ULL,
         "MIR constant floating arithmetic uses the shared target binary64 "
         "kernel");
  const auto runtime = evaluate("lazy", false);
  expect(lazy && lazy.value == integer(42) && !runtime &&
             runtime.failure ==
                 lang::ConstantEvaluationFailure::RuntimeOnlyCall &&
             runtime.runtimeCallee != 0,
         "constant eligibility follows executed paths but never invokes "
         "runtime code");
  const auto zero = evaluate("divide", integer(0));
  expect(!zero &&
             zero.failure == lang::ConstantEvaluationFailure::DivisionByZero &&
             zero.function == functionId("divide") && zero.block != 0 &&
             zero.instruction != 0,
         "MIR constant arithmetic failure retains exact typed instruction "
         "provenance");
  const auto overflow = evaluate("factorial", integer(13));
  expect(!overflow && overflow.failure ==
                          lang::ConstantEvaluationFailure::IntegerOverflow,
         "MIR constant arithmetic preserves target integer overflow");
  const auto loop = evaluate("endless", integer(1), {.steps = 100});
  const auto recursion = evaluate("recursion", integer(1), {.callDepth = 4});
  expect(
      recursion.calls.size() == 4 && recursion.omittedCalls == 0 &&
          recursion.calls.front().body.owner == functionId("recursion") &&
          recursion.calls.front().instruction != 0,
      "failed MIR calls preserve executed caller identities without AST data");
  const auto deepRecursion =
      evaluate("recursion", integer(1), {.callDepth = 20});
  expect(deepRecursion.calls.size() == 8 &&
             deepRecursion.calls.capacity() == 8 &&
             deepRecursion.omittedCalls == 12,
         "failure traces bound storage and count omitted recursive calls");
  expect(lazy.calls.empty() && lazy.omittedCalls == 0,
         "successful constant execution retains no failure trace");
  expect(
      !loop && loop.failure == lang::ConstantEvaluationFailure::ResourceLimit &&
          !recursion &&
          recursion.failure == lang::ConstantEvaluationFailure::ResourceLimit,
      "all executed MIR blocks and nested calls share bounded resources");

  const std::string traceDefinitions =
      "constexpr int divide(int value) { return 100 / value; }\n"
      "constexpr int middle(int value) { return divide(value); }\n"
      "constexpr int outer(int value) { return middle(value); }\n";
  for (const std::string &request :
       {std::string("constexpr int result = outer(0);\n"),
        std::string("static_assert(outer(0) == 1);\n"),
        std::string("int select() { if constexpr (outer(0) == 1) { return 1; } "
                    "else { return 0; } }\n")}) {
    const auto failed = lang::Frontend().analyze("constant-trace.gti",
                                                 traceDefinitions + request);
    const auto *diagnostic =
        findDiagnosticByCode(failed.diagnostics, "GTI-S2057");
    expect(!failed.semanticValid && !failed.canGenerateCode() && diagnostic &&
               diagnostic->message.find("Division by zero") !=
                   std::string::npos,
           "all three constant contexts retain the owning arithmetic failure");
    if (diagnostic) {
      std::vector<std::size_t> lines;
      for (const auto &related : diagnostic->related)
        if (related.message == "Constant evaluation requested here.")
          lines.push_back(related.span.line);
      expect(lines == std::vector<std::size_t>{4, 3, 2} &&
                 diagnostic->primary.line == 1 && diagnostic->fixes.empty(),
             "constant failure notes link outer-to-inner calls, not "
             "speculative repairs");
    }
    std::string repaired = request;
    repaired.replace(repaired.find("outer(0)"), 8, "outer(100)");
    expect(lang::Frontend()
               .analyze("constant-trace.gti", traceDefinitions + repaired)
               .semanticValid,
           "repair clears the arithmetic failure and its call trace");
  }
  const auto constructorFailure = lang::Frontend().analyze(
      "constant-constructor-trace.gti",
      "struct Ratio {\n"
      "  int value;\n"
      "  constexpr Ratio(int divisor) : value(100 / divisor) {}\n"
      "};\n"
      "constexpr int make_ratio(int divisor) { Ratio ratio{divisor}; return "
      "ratio.value; }\n"
      "constexpr int result = make_ratio(0);\n");
  const auto *constructorDiagnostic =
      findDiagnosticByCode(constructorFailure.diagnostics, "GTI-S2057");
  expect(!constructorFailure.semanticValid && constructorDiagnostic &&
             constructorDiagnostic->primary.line == 3,
         "constructor failure preserves the executing initializer location");
  if (constructorDiagnostic) {
    std::vector<int> lines;
    for (const auto &related : constructorDiagnostic->related)
      if (related.message == "Constant evaluation requested here.")
        lines.push_back(related.span.line);
    expect(
        lines == std::vector<int>{6, 5},
        "constructor failures retain the requesting function and construction");
  }
  const auto badArgument = evaluate("sum", true);
  expect(!badArgument && badArgument.failure ==
                             lang::ConstantEvaluationFailure::InvalidOperands,
         "MIR evaluator requires exactly typed arguments");
  lang::MirProgram malformed = frontend.mir;
  const_cast<lang::MirFunctionInstance *>(
      malformed.findFunctionInstance(functionId("sum")))
      ->body.entry = 0;
  const std::array arguments{integer(5)};
  const auto rejected = lang::evaluateMirConstantFunction(
      malformed, functionId("sum"), arguments);
  expect(!rejected && rejected.detail.find("verified MIR") != std::string::npos,
         "ordinary MIR verification runs before const execution");
  const auto optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = lang::OptimizationLevel::O1});
  const auto transformed = lang::evaluateMirConstantFunction(
      optimized.mir, functionId("folded"), arguments);
  expect(optimized.valid() && !transformed &&
             transformed.detail.find("canonical unoptimized MIR") !=
                 std::string::npos,
         "const eligibility cannot depend on optimizer rewriting");

  lang::FrontendWorkMetrics earlyMetrics;
  const auto early =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics,
                      .workMetrics = &earlyMetrics})
          .analyze("early-constexpr.gti", R"(
constexpr uint64_t extent(uint64_t input) { return input + 1; }
constexpr int choose(int value) {
  switch (value) { case -4: return 11; default: return 22; }
}
constexpr int choose_char(char value) {
  switch (value) { case 'x': return 11; default: return 22; }
}
constexpr uint64_t capacity = extent(uint64_t(3));
int values[capacity] = {};
int selected() {
  if constexpr (extent(uint64_t(2)) == 3) { return 42; }
  else { return name_that_must_not_be_analyzed; }
}
static_assert(extent(uint64_t(41)) == 42);
static_assert(choose(-4) == 11 && choose(0) == 22);
static_assert(choose_char('x') == 11 && choose_char('y') == 22);
int later() { return selected(); }
)");
  expect(early.semanticValid && earlyMetrics.constexprMirExecutions >= 7 &&
             !early.hirValid,
         "staged execution forms types and selects branches before normal HIR "
         "exists");
  if (!early.semanticValid || earlyMetrics.constexprMirExecutions < 7) {
    std::cerr << "Early MIR executions: " << earlyMetrics.constexprMirExecutions
              << '\n';
    for (const auto &diagnostic : early.diagnostics)
      std::cerr << diagnostic.message << '\n';
  }

  // A completed-body closure never publishes unfinished unrelated functions,
  // and its deliberately absent whole-program seal must remain non-publishable.
  lang::SemanticVisitor provider(lang::TargetInfo::host(),
                                 &frontend.sourceGraph, false, nullptr,
                                 &frontend.sources);
  expect(provider.check(frontend.program),
         "closure provider checks the fixture");
  std::vector<lang::FunctionId> completed;
  lang::FunctionId sumDeclaration = 0;
  lang::FunctionId callerDeclaration = 0;
  for (const auto &function : frontend.hir.functionInstances()) {
    if (function.source == nullptr)
      continue;
    const auto *declaration = provider.model().findFunction(*function.source);
    if (declaration == nullptr)
      continue;
    if (function.source->name().lexeme == "sum")
      sumDeclaration = declaration->id;
    if (function.source->name().lexeme == "call_sum")
      callerDeclaration = declaration->id;
    if (function.source->isConstexpr())
      completed.push_back(declaration->id);
  }
  lang::HirLowerer stagedLowerer;
  auto staged = stagedLowerer.lowerConstantFunction(provider.model(), provider,
                                                    sumDeclaration, completed);
  expect(staged.valid() && staged.program.functionInstances().size() == 1 &&
             staged.program.analysisSeal().programSnapshot ==
                 provider.model().analysisSeal().programSnapshot &&
             staged.program.analysisSeal().activeStatements.empty() &&
             !lang::verifyHirProgramPlans(provider.model(), staged.program)
                  .valid(),
         "staged closure excludes unrelated bodies and cannot pass "
         "whole-program publication");
  const auto *entry =
      frontend.sourceGraph.findUnit(frontend.sourceGraph.entryUnit());
  const auto stagedMetadata = lang::FailureMetadataBuilder().build(
      frontend.sourceGraph, frontend.sources, staged.program, entry->path);
  const auto stagedMir =
      lang::MirLowerer().lower(staged.program, stagedMetadata.metadata);
  expect(stagedMetadata.valid() && stagedMir.valid() &&
             lang::verifyMirProgram(stagedMir.program).valid(),
         "staged bodies retain ordinary failure metadata and MIR verification");
  const auto publication = lang::LoweredProgramBuilder().build(
      frontend.program, provider.model(), staged.program, stagedMir.program,
      stagedMir.program, {}, lang::TargetInfo::host());
  expect(!publication.valid() && !publication.program.has_value(),
         "valid staged MIR cannot be sealed as a whole-program backend input");
  auto unavailable = stagedLowerer.lowerConstantFunction(
      provider.model(), provider, sumDeclaration, {});
  expect(!unavailable.valid(),
         "a registered but incomplete root cannot be staged");
  // Staging is lazy (#175): a callee absent from the completed inventory
  // keeps only a signature-only declaration body and is never executed.
  auto incompleteCallee = stagedLowerer.lowerConstantFunction(
      provider.model(), provider, callerDeclaration, {callerDeclaration});
  const auto signatureOnly =
      std::count_if(incompleteCallee.program.functionInstances().begin(),
                    incompleteCallee.program.functionInstances().end(),
                    [&](const lang::HirFunctionInstance &function) {
                      return function.declaration == sumDeclaration &&
                             function.source == nullptr &&
                             function.body.roots.empty();
                    });
  expect(incompleteCallee.valid() &&
             incompleteCallee.program.functionInstances().size() == 2 &&
             signatureOnly == 1,
         "a callee absent from the completed inventory stages as a "
         "signature-only declaration, never its body");
  auto completeClosure = stagedLowerer.lowerConstantFunction(
      provider.model(), provider, callerDeclaration,
      {callerDeclaration, sumDeclaration});
  expect(completeClosure.valid() &&
             completeClosure.program.functionInstances().size() == 2,
         "completed callee discovery closes the ordinary staged function "
         "worklist");

  for (const auto &instance : frontend.hir.functionInstances()) {
    if (instance.source == nullptr || instance.source->name().lexeme != "twice")
      continue;
    const auto *declaration = provider.model().findFunction(*instance.source);
    expect(declaration != nullptr && declaration->genericParameters.size() == 1,
           "generic stage fixture retains its semantic parameter identity");
    if (declaration == nullptr || declaration->genericParameters.size() != 1)
      continue;
    const auto id = declaration->id;
    const auto parameter = declaration->genericParameters.front().id;
    const lang::GenericSubstitution substitution{
        .types = {{parameter, lang::SemanticType::Int32}}};
    const auto genericStage = stagedLowerer.lowerConstantFunction(
        provider.model(), provider, id, completed, substitution);
    const auto genericMetadata = lang::FailureMetadataBuilder().build(
        frontend.sourceGraph, frontend.sources, genericStage.program,
        entry->path);
    const auto genericMir = lang::MirLowerer().lower(genericStage.program,
                                                     genericMetadata.metadata);
    const auto evaluated =
        lang::evaluateMirConstantFunction(genericMir.program, 1, arguments);
    expect(genericStage.valid() && genericMetadata.valid() &&
               genericMir.valid() && evaluated &&
               evaluated.value == integer(10),
           "generic HIR staging independently executes a concrete MIR body "
           "without frontend fallback");
    const auto missing = stagedLowerer.lowerConstantFunction(
        provider.model(), provider, id, completed);
    expect(!missing.valid(),
           "staged generic roots require exact concrete substitutions");
    break;
  }

  lang::FrontendWorkMetrics failureMetrics;
  const std::string failingSource =
      "constexpr int divide(int value) { return 100 / value; }\n"
      "constexpr int result = divide(0);\n";
  const auto failing =
      lang::Frontend({.workMetrics = &failureMetrics})
          .analyze("staged-constant-failure.gti", failingSource);
  const auto *failure = findDiagnosticByCode(failing.diagnostics, "GTI-S2057");
  expect(!failing.semanticValid && failureMetrics.constexprMirExecutions == 1 &&
             failure != nullptr &&
             failure->phase == lang::DiagnosticPhase::Semantics &&
             failure->primary.source ==
                 fixtureSourcePath("staged-constant-failure.gti").string() &&
             failure->primary.start == failingSource.find('/') &&
             failure->primary.end == failingSource.find('/') + 1 &&
             failure->fixes.empty(),
         "MIR arithmetic failure is authoritative and retains its exact source "
         "operator");

  const auto compatibility =
      lang::Frontend().analyze("constant-profile-compatibility.gti", R"(
enum class Mode { First, Second };
constexpr bool is_first(Mode mode) { return mode == Mode::First; }
constexpr int select(int value) {
  switch (value) { case 1: return 11; default: return 22; }
}
static_assert(is_first(Mode::First));
static_assert(!is_first(Mode::Second));
static_assert(select(1) == 11 && select(0) == 22);
)");
  expect(compatibility.semanticValid,
         "unsupported MIR profile types and control flow preserve the existing "
         "constexpr subset");

  lang::FrontendWorkMetrics genericMetrics;
  const auto stagedGeneric =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics,
                      .workMetrics = &genericMetrics})
          .analyze("early-generic-constant.gti", R"(
constexpr T twice<std::numeric T>(T value) { return T(value + value); }
constexpr T scale<std::numeric T, uint64_t N>(T value) {
  mut T result = T(0);
  for (mut uint64_t index = 0; index < N; index++) { result = T(result + value); }
  return twice<T>(result);
}
constexpr int indirect(int value) { return scale<int, 3>(value); }
constexpr int first = scale<int, 3>(7);
constexpr double second = scale<double, 2>(double(1.5));
constexpr int third = indirect(2);
constexpr int zero = scale<int, 0>(5);
static_assert(first == 42 && second == double(6.0) && third == 12 && zero == 0);
int enclosing(int value) {
  mut int local = value;
  constexpr int bound = scale<int, 2>(2);
  local += bound;
  if constexpr (twice<int>(3) == 6) { return local; }
  else { return should_not_exist; }
}
)",
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!stagedGeneric.semanticValid ||
      genericMetrics.constexprMirExecutions < 6) {
    std::cerr << "Generic MIR executions: "
              << genericMetrics.constexprMirExecutions << '\n';
    for (const auto &diagnostic : stagedGeneric.diagnostics)
      std::cerr << diagnostic.message << '\n';
  }
  expect(stagedGeneric.semanticValid &&
             genericMetrics.constexprMirExecutions >= 6,
         "concrete type/value generic closures execute MIR without corrupting "
         "enclosing scopes");

  lang::FrontendWorkMetrics loanMetrics;
  const auto loanIsolation =
      lang::Frontend({.workMetrics = &loanMetrics})
          .analyze("constexpr-loan-isolation.gti", R"(
constexpr T twice<std::numeric T>(T value) { return T(value + value); }
int invalid(int input) {
  mut int local = input;
  int& borrowed = local;
  constexpr int amount = twice<int>(1);
  local += amount;
  return borrowed;
}
)",
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  expect(!loanIsolation.semanticValid &&
             loanMetrics.constexprMirExecutions == 1 &&
             hasDiagnostic(loanIsolation.diagnostics, "borrow"),
         "staged concrete reanalysis cannot erase an enclosing live loan");

  const auto invalidInstance =
      lang::Frontend().analyze("constexpr-invalid-instance.gti", R"(
constexpr int invalid<uint64_t N>(int input) {
  static_assert(N == 0, "only zero is valid");
  return input;
}
constexpr int good = invalid<0>(7);
constexpr int bad = invalid<1>(7);
)");
  expect(!invalidInstance.semanticValid &&
             hasDiagnostic(invalidInstance.diagnostics, "only zero is valid"),
         "concrete semantic errors must not fall back to permissive AST "
         "execution");
}

void testMirConstantRecords() {
  lang::FrontendWorkMetrics observerMetrics;
  const auto observers =
      lang::Frontend({.stopAfter = lang::FrontendPhase::Semantics,
                      .workMetrics = &observerMetrics})
          .analyze("early-constexpr-methods.gti", R"(
struct Sized<T, uint64_t N> {
  T value;
  constexpr Sized(T initial) : value(initial) {}
  constexpr T get() { return this.value; }
  constexpr uint64_t size() { return N; }
  constexpr T recurse(int depth) {
    if (depth == 0) { return this.get(); }
    return this.recurse(depth - 1);
  }
};
struct Utility {
  static constexpr int twice(int value) { return value * 2; }
public:
  Utility() = default;
};
constexpr Sized<int, 3> first = Sized<int, 3>(9);
constexpr Sized<int, 5> second = Sized<int, 5>(17);
static_assert(first.get() == 9 && second.get() == 17);
static_assert(first.size() == 3 && second.size() == 5);
static_assert(Utility::twice(3) == 6);
constexpr uint64_t extent = first.size();
int values[extent] = {};
int selected() {
  if constexpr (first.recurse(2) == 9) { return 0; }
  else { return discarded_invalid_name; }
}
)");
  if (!observers.semanticValid ||
      observerMetrics.constexprMirExecutions +
              observerMetrics.constexprStagedReuses !=
          9) {
    std::cerr << "Direct method MIR executions: "
              << observerMetrics.constexprMirExecutions << '\n';
    for (const auto &diagnostic : observers.diagnostics)
      std::cerr << diagnostic.primary.line << ": " << diagnostic.message
                << '\n';
  }
  expect(observers.semanticValid && !observers.hirValid &&
             observerMetrics.constexprMirExecutions +
                     observerMetrics.constexprStagedReuses ==
                 9,
         "direct generic instance/static method roots use staged MIR for "
         "assertions, array extents and branch conditions");
  lang::FrontendWorkMetrics libraryMetrics;
  const auto library =
      lang::Frontend({.workMetrics = &libraryMetrics})
          .analyze("constexpr-library-array.gti", R"(
import std.array;
constexpr std::array<int, 3> make_values(int seed) {
  return std::array<int, 3>({seed, seed + 1, seed + 2});
}
constexpr std::array<int, 3> make_empty() { return std::array<int, 3>(); }
constexpr bool inspect_values(int seed) {
  std::array<int, 3> value = make_values(seed);
  return value.size() == 3 && !value.empty();
}
constexpr std::array<int, 3> values = make_values(4);
constexpr std::array<int, 3> empty = make_empty();
static_assert(inspect_values(9));
static_assert(values.size() == 3 && empty.size() == 3);
int main() { return make_values(4).size() == 3 ? 0 : 1; }
)",
                   {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  if (!library.canGenerateCode())
    for (const auto &diagnostic : library.diagnostics)
      std::cerr << diagnostic.message << '\n';
  expect(library.canGenerateCode() &&
             libraryMetrics.constexprMirExecutions >= 3,
         "ordinary std::array explicit and default constructors execute "
         "and read-only observers execute through staged MIR");
  if (library.canGenerateCode()) {
    auto malformed = library.mir;
    bool changedStage = false;
    auto &constructors =
        const_cast<std::vector<lang::MirConstructorInstance> &>(
            malformed.constructorInstances());
    for (auto &constructor : constructors) {
      for (auto &block : constructor.body.blocks) {
        for (auto &instruction : block.instructions) {
          if (!changedStage &&
              instruction.kind == lang::MirInstructionKind::Initialize &&
              instruction.info.type.kind == lang::SemanticType::Array &&
              instruction.constructorInitializer != 0) {
            instruction.constructorInitializer = 0;
            changedStage = true;
          }
        }
      }
    }
    expect(changedStage && !lang::verifyMirProgram(malformed).valid(),
           "MIR rejects a passive array field write detached from its exact "
           "constructor stage");
  }
  lang::FrontendWorkMetrics metrics;
  const auto frontend = lang::Frontend({.workMetrics = &metrics})
                            .analyze("constexpr-record.gti", R"(
struct Point {
  mut int x;
  mut int y;
  constexpr Point(int a, int b) : x(a), y(b) {}
  constexpr int squared() { return this.x * this.x + this.y * this.y; }
  constexpr int scaled(int factor) { return this.squared() * factor; }
};
struct Box<T> {
  mut T value;
  constexpr Box(T initial) : value(initial) {}
  constexpr T get() { return this.value; }
};
struct Mesh {
  mut Point point;
  mut int indices[3];
  constexpr Mesh(Point initial, int values[3]) : point(initial), indices(values) {}
  constexpr int inspect() { return this.point.scaled(this.indices[1]); }
};
constexpr Point translate(Point value, int delta) {
  mut Point result = value;
  result.x += delta;
  result.y++;
  return result;
}
constexpr Point create_point(int x, int y) { return Point(x, y); }
constexpr Box<T> create_box<T>(T value) { return Box<T>(value); }
constexpr Mesh create_mesh(int x) { return Mesh(Point(x, 4), {1, 2, 3}); }
constexpr int inspect_mesh(int x) { return create_mesh(x).inspect(); }
constexpr T inspect_box<T>(T value) { return create_box<T>(value).get(); }
constexpr int distance(Point value) { return value.x * value.x + value.y * value.y; }
constexpr Box<T> replace<T>(Box<T> value, T next) {
  mut Box<T> result = value;
  result.value = next;
  return result;
}
constexpr Mesh edit(Mesh input) {
  mut Mesh result = input;
  result.point.x++;
  result.indices[1] += 7;
  return result;
}
constexpr Point point = Point(3, 4);
constexpr Point moved = translate(point, 2);
constexpr Mesh mesh = Mesh(point, {1, 2, 3});
constexpr Mesh changed = edit(mesh);
constexpr Box<int> first = Box<int>(2);
constexpr Box<int> second = replace<int>(first, 7);
constexpr Point built = create_point(6, 8);
constexpr Box<int> built_box = create_box<int>(12);
constexpr Mesh built_mesh = create_mesh(3);
static_assert(built.x == 6 && built.y == 8);
static_assert(built_box.value == 12 && built_mesh.point.x == 3);
static_assert(inspect_mesh(3) == 50 && inspect_box<int>(17) == 17);
static_assert(point.x == 3 && point.y == 4);
static_assert(moved.x == 5 && moved.y == 5 && distance(moved) == 50);
static_assert(mesh.point.x == 3 && mesh.indices[1] == 2);
static_assert(changed.point.x == 4 && changed.indices[1] == 9);
static_assert(first.value == 2 && second.value == 7);
int main() { return distance(translate(Point(3, 4), 2)) == 50 ? 0 : 1; }
)");
  if (!frontend.canGenerateCode())
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.primary.line << ": " << diagnostic.message
                << '\n';
  expect(frontend.canGenerateCode() && metrics.constexprMirExecutions >= 9,
         "passive record fields, nested arrays, mutation, generic identity and "
         "return execute staged MIR");
  if (!frontend.canGenerateCode())
    return;
  lang::FrontendWorkMetrics failureMetrics;
  const std::string failingSource =
      "struct Fraction { int value; constexpr Fraction(int divisor) : value(10 "
      "/ divisor) {} };\n"
      "constexpr int build(int divisor) { Fraction item = Fraction(divisor); "
      "return item.value; }\n"
      "constexpr int invalid = build(0);\n";
  const auto failed =
      lang::Frontend({.workMetrics = &failureMetrics})
          .analyze("constant-constructor-failure.gti", failingSource);
  const auto *failure = findDiagnosticByCode(failed.diagnostics, "GTI-S2057");
  expect(!failed.semanticValid && failureMetrics.constexprMirExecutions == 1 &&
             failure != nullptr &&
             failure->primary.start == failingSource.find('/') &&
             failure->primary.end == failingSource.find('/') + 1,
         "MIR constructor arithmetic failures retain the original initializer "
         "operator");
  const auto point = std::find_if(
      frontend.mir.classInstances().begin(),
      frontend.mir.classInstances().end(),
      [](const lang::MirClassInstance &item) {
        return item.declaredFields.size() == 2 &&
               item.declaredFields[0].type == lang::SemanticType::Int32 &&
               item.declaredFields[1].type == lang::SemanticType::Int32;
      });
  expect(point != frontend.mir.classInstances().end(),
         "record execution retains exact MIR field identities");
  if (point == frontend.mir.classInstances().end())
    return;
  const auto integer = [](std::uint64_t value) -> lang::ConstantValue {
    return lang::makeConstantInteger({.magnitude = value},
                                     {.width = 32, .signedValue = true});
  };
  lang::ConstantAggregate aggregate{
      .kind = lang::ConstantAggregateKind::Object,
      .identity = point->declaration,
      .exactType = std::make_shared<const lang::SemanticType>(point->type),
      .fields = {{point->declaredFields[0].symbol, integer(3)},
                 {point->declaredFields[1].symbol, integer(4)}}};
  lang::HirFunctionInstanceId distance = 0;
  lang::HirConstructorInstanceId pointConstructor = 0;
  for (const auto &constructor : frontend.mir.constructorInstances())
    if (constructor.owner == point->id)
      pointConstructor = constructor.id;
  const std::array pointArguments{integer(3), integer(4)};
  const auto constructedPoint = lang::evaluateMirConstantConstructor(
      frontend.mir, pointConstructor, pointArguments);
  expect(constructedPoint &&
             constructedPoint.value ==
                 lang::ConstantValue{lang::makeConstantAggregate(aggregate)},
         "direct constructor roots publish exact fully initialized fields");
  lang::HirFunctionInstanceId inspectMesh = 0;
  lang::HirFunctionInstanceId observePoint = 0;
  for (const auto &function : frontend.hir.functionInstances()) {
    if (function.source && function.source->name().lexeme == "distance")
      distance = function.id;
    if (function.source && function.source->name().lexeme == "inspect_mesh")
      inspectMesh = function.id;
    if (function.source && function.source->name().lexeme == "squared")
      observePoint = function.id;
  }
  const std::array meshArguments{integer(3)};
  const auto observed = lang::evaluateMirConstantFunction(
      frontend.mir, inspectMesh, meshArguments);
  if (!observed || observed.value != integer(50))
    std::cerr << "Direct observer root=" << inspectMesh
              << " failure=" << static_cast<int>(observed.failure)
              << " body=" << static_cast<int>(observed.body.kind) << ':'
              << observed.body.owner << " instruction=" << observed.instruction
              << " detail=" << observed.detail << '\n';
  expect(observed && observed.value == integer(50),
         "direct MIR executes nested constructors, projected receivers and "
         "read-only method calls without compatibility fallback");
  expect(!lang::evaluateMirConstantFunction(frontend.mir, observePoint, {}),
         "an instance method cannot execute without its exact receiver");
  const auto directMethod = lang::evaluateMirConstantFunction(
      frontend.mir, observePoint, {}, {},
      lang::ConstantValue{lang::makeConstantAggregate(aggregate)});
  expect(directMethod && directMethod.value == integer(25),
         "direct method roots receive exact fully initialized constant state");
  expect(!lang::evaluateMirConstantFunction(frontend.mir, observePoint, {}, {},
                                            integer(3)),
         "a scalar cannot impersonate the method's structural receiver");
  auto nonConstantMethod = frontend.mir;
  auto &methodInstances = const_cast<std::vector<lang::MirFunctionInstance> &>(
      nonConstantMethod.functionInstances());
  for (auto &function : methodInstances)
    if (function.id == observePoint)
      function.constexprFunction = false;
  expect(!lang::evaluateMirConstantFunction(nonConstantMethod, inspectMesh,
                                            meshArguments),
         "a called read-only method requires explicit constexpr eligibility");
  auto nonConstantConstructor = frontend.mir;
  auto &constructorInstances =
      const_cast<std::vector<lang::MirConstructorInstance> &>(
          nonConstantConstructor.constructorInstances());
  for (auto &constructor : constructorInstances)
    constructor.constexprConstructor = false;
  expect(
      !lang::evaluateMirConstantFunction(nonConstantConstructor, inspectMesh,
                                         meshArguments),
      "native constructibility cannot replace semantic constexpr eligibility");
  lang::SemanticVisitor provider(lang::TargetInfo::host(),
                                 &frontend.sourceGraph, false, nullptr,
                                 &frontend.sources);
  expect(provider.check(frontend.program),
         "constructor closure provider checks the fixture");
  std::vector<lang::FunctionId> completedFunctions;
  std::vector<lang::ConstructorId> completedConstructors;
  lang::FunctionId createPoint = 0;
  for (const auto &function : frontend.hir.functionInstances()) {
    if (!function.source || !function.source->isConstexpr())
      continue;
    const auto *declaration = provider.model().findFunction(*function.source);
    if (!declaration)
      continue;
    completedFunctions.push_back(declaration->id);
    if (function.source->name().lexeme == "create_point")
      createPoint = declaration->id;
  }
  for (const auto &constructor : frontend.hir.constructorInstances()) {
    if (!constructor.source)
      continue;
    if (const auto *declaration =
            provider.model().findConstructor(*constructor.source))
      completedConstructors.push_back(declaration->id);
  }
  lang::HirLowerer constructorLowerer;
  const auto missingConstructor = constructorLowerer.lowerConstantFunction(
      provider.model(), provider, createPoint, completedFunctions);
  expect(!missingConstructor.valid(),
         "a registered constexpr constructor cannot be staged before its "
         "completed-definition inventory admits it");
  const auto completedConstructor = constructorLowerer.lowerConstantFunction(
      provider.model(), provider, createPoint, completedFunctions, {},
      completedConstructors);
  expect(completedConstructor.valid() &&
             completedConstructor.program.constructorInstances().size() == 1,
         "completed constructor staging reuses the ordinary exact worklist");
  const std::array arguments{
      lang::ConstantValue{lang::makeConstantAggregate(aggregate)}};
  const auto evaluated =
      lang::evaluateMirConstantFunction(frontend.mir, distance, arguments);
  expect(evaluated && evaluated.value == integer(25),
         "direct MIR record evaluation consumes ordinary verified field "
         "projections and passive drops");
  aggregate.fields.pop_back();
  const std::array incomplete{
      lang::ConstantValue{lang::makeConstantAggregate(aggregate)}};
  expect(!lang::evaluateMirConstantFunction(frontend.mir, distance, incomplete),
         "an incomplete constant record cannot supply an initialized field");
  aggregate.fields.push_back({point->declaredFields[1].symbol, true});
  const std::array mistyped{
      lang::ConstantValue{lang::makeConstantAggregate(aggregate)}};
  const auto invalid =
      lang::evaluateMirConstantFunction(frontend.mir, distance, mistyped);
  expect(!invalid && invalid.failure ==
                         lang::ConstantEvaluationFailure::InvalidOperands,
         "record identity alone does not authorize a mistyped constant field");
  aggregate.fields[1] = aggregate.fields[0];
  const std::array duplicated{
      lang::ConstantValue{lang::makeConstantAggregate(aggregate)}};
  expect(
      !lang::evaluateMirConstantFunction(frontend.mir, distance, duplicated),
      "duplicated field identities cannot stand in for complete record state");
}

void testConstantEvaluationReuse() {
  const auto analyze = [](const std::string &body, bool reuse,
                          std::size_t steps,
                          lang::FrontendWorkMetrics &metrics) {
    lang::FrontendOptions options = stepLimited(steps);
    options.reuseConstantEvaluations = reuse;
    options.workMetrics = &metrics;
    return lang::Frontend(std::move(options))
        .analyze("constant-reuse.gti",
                 kCompileLimitsPrelude + body + "\nint main() { return 0; }");
  };
  struct Observed {
    lang::FrontendResult result;
    lang::FrontendWorkMetrics metrics;
  };
  const auto both = [&](const std::string &body, std::size_t steps) {
    std::pair<Observed, Observed> observed;
    observed.first.result = analyze(body, true, steps, observed.first.metrics);
    observed.second.result =
        analyze(body, false, steps, observed.second.metrics);
    expect(observed.first.result.canGenerateCode() ==
                   observed.second.result.canGenerateCode() &&
               sameDiagnostics(observed.first.result.diagnostics,
                               observed.second.result.diagnostics) &&
               observed.second.metrics.constexprStagedReuses == 0 &&
               observed.second.metrics.constexprClosureReuses == 0,
           "constant-evaluation reuse never changes a program's diagnostics "
           "or validity: " +
               body);
    return observed;
  };
  const std::size_t defaultSteps = lang::CompileLimits{}.constexprSteps;

  // Identical immediate void calls (the CB-08 shape): one execution, the
  // rest replay its recorded result.
  std::string repeated =
      "consteval void require_total(uint64_t n, uint64_t expected) {\n"
      "  uint64_t checked = uint64_t(1) / (accumulate(n, uint64_t(0)).sum == "
      "expected "
      "? uint64_t(1) : uint64_t(0));\n}\nvoid validate() {\n";
  for (int index = 0; index < 20; ++index)
    repeated += "  require_total(uint64_t(100), uint64_t(5050));\n";
  repeated += "}";
  const auto identical = both(repeated, defaultSteps);
  expect(identical.first.result.canGenerateCode() &&
             identical.first.metrics.constexprStagedReuses >= 19 &&
             identical.first.metrics.constexprMirExecutions <
                 identical.second.metrics.constexprMirExecutions,
         "identical immediate calls execute once and replay the rest");

  // Distinct arguments still reuse the lowered call closure.
  std::string distinct = "consteval uint64_t total(uint64_t n) { return "
                         "accumulate(n, uint64_t(0)).sum; }\nuint64_t sum() {\n"
                         "  return uint64_t(0)";
  for (int index = 1; index <= 20; ++index)
    distinct += " + total(uint64_t(" + std::to_string(index) + "))";
  distinct += ";\n}";
  const auto closures = both(distinct, defaultSteps);
  expect(closures.first.result.canGenerateCode() &&
             closures.first.metrics.constexprClosureReuses >= 19 &&
             closures.first.metrics.constexprMirExecutions ==
                 closures.second.metrics.constexprMirExecutions,
         "calls with new arguments execute again without lowering the call "
         "closure again");

  // A lazily staged closure, whose runtime-only callee keeps a signature-only
  // body, is reused the same way, and an executed runtime-only call reports
  // identically whether its closure was reused or lowered again.
  std::string lazy = "int runtime_only(int v) { return v; }\n"
                     "consteval int pick(int v) {\n"
                     "  if (v > 100) { return runtime_only(v); }\n"
                     "  return v;\n}\nint sum() {\n  return 0";
  for (int index = 1; index <= 20; ++index)
    lazy += " + pick(" + std::to_string(index) + ")";
  const auto lazyClosures = both(lazy + ";\n}", defaultSteps);
  expect(lazyClosures.first.result.canGenerateCode() &&
             lazyClosures.first.metrics.constexprClosureReuses >= 19,
         "a lazily staged call closure is lowered once and reused");
  const auto lazyExecuted = both(lazy + " + pick(200);\n}", defaultSteps);
  expect(!lazyExecuted.first.result.canGenerateCode() &&
             lazyExecuted.first.metrics.constexprClosureReuses >= 20 &&
             std::any_of(lazyExecuted.first.result.diagnostics.begin(),
                         lazyExecuted.first.result.diagnostics.end(),
                         [](const lang::Diagnostic &diagnostic) {
                           const std::string named =
                               "'runtime_only' is not constexpr.";
                           return diagnostic.code == "GTI-S2057" &&
                                  (diagnostic.message.find(named) !=
                                       std::string::npos ||
                                   std::any_of(diagnostic.hints.begin(),
                                               diagnostic.hints.end(),
                                               [&](const std::string &hint) {
                                                 return hint.find(named) !=
                                                        std::string::npos;
                                               }));
                         }),
         "an executed runtime-only call in a reused lazy closure names its "
         "callee");

  // A request that exceeds its budget only through repeated calls fails the
  // same way whether the repeats replay or execute. Find the one-call cost
  // first, then allow two calls but not three.
  const std::string once = "static_assert(spin(uint64_t(300)) > uint64_t(0));";
  std::size_t low = 1, high = 100000;
  while (low < high) {
    const std::size_t middle = low + (high - low) / 2;
    lang::FrontendWorkMetrics unused;
    if (analyze(once, false, middle, unused).canGenerateCode())
      high = middle;
    else
      low = middle + 1;
  }
  const std::size_t oneCall = low;
  const std::size_t budget = oneCall * 2 + oneCall / 2;
  const auto twice = both("static_assert(spin(uint64_t(300)) + "
                          "spin(uint64_t(300)) > uint64_t(0));",
                          budget);
  const auto thrice = both("static_assert(spin(uint64_t(300)) + "
                           "spin(uint64_t(300)) + spin(uint64_t(300)) > "
                           "uint64_t(0));",
                           budget);
  expect(oneCall > 300 && twice.first.result.canGenerateCode() &&
             twice.first.metrics.constexprStagedReuses >= 1 &&
             !thrice.first.result.canGenerateCode() &&
             hasDiagnostic(thrice.first.result.diagnostics,
                           "execution-step limit"),
         "a replay charges the recorded steps, so exhaustion through "
         "repeated calls is identical with and without reuse");

  // Failures are never recorded as successes: each failing call reports.
  const auto failing = both(
      "consteval uint64_t checked(uint64_t n) { return uint64_t(1) / n; }\n"
      "uint64_t use() { return checked(uint64_t(0)) + "
      "checked(uint64_t(0)); }",
      defaultSteps);
  expect(errorsOf(failing.first.result.diagnostics).size() == 2,
         "a failed evaluation is executed and reported at every call");
}

// A segment switch is invisible: forcing one at every recursion boundary
// (constant-evaluation calls, staged analysis, macro expansion, HIR and MIR
// lowering of expanded bodies) yields byte-identical MIR and diagnostics.
void testNativeStackSegmentsAreInvisible() {
  std::string macros = "#macro int32_t m0(int32_t v) { return v + 1; }\n";
  for (int level = 1; level < 24; ++level)
    macros += "#macro int32_t m" + std::to_string(level) +
              "(int32_t v) { return m" + std::to_string(level - 1) +
              "!(v) + 1; }\n";
  const std::string valid = macros +
                            stagedChainSource(12).substr(
                                0, stagedChainSource(12).find("int main()")) +
                            "constexpr int32_t fold(int32_t n) {\n"
                            "  if (n == 0) { return 0; }\n"
                            "  return fold(n - 1) + n;\n"
                            "}\n"
                            "static_assert(fold(200) == 20100);\n"
                            "int main() { return m23!(0) - 24; }\n";
  const std::string invalid = recursiveDescentSource(300);
  lang::CompileLimits limits;
  limits.constexprDepth = 256;
  const auto run = [&](const std::string &name, const std::string &source) {
    const auto result = analyzeWithLimits(name, source, limits);
    return std::pair{lang::MirPrinter().print(result.mir),
                     diagnosticText(result.diagnostics) +
                         (result.canGenerateCode() ? "valid" : "invalid")};
  };
  const auto plain = run("segments.gti", valid);
  const auto plainInvalid = run("segments-invalid.gti", invalid);
  expect(plain.second == "valid" && !plain.first.empty(),
         "the segment-switch program is valid: " + plain.second);
  expect(plainInvalid.second.find("call depth exceeded") != std::string::npos,
         "the invalid program reaches the configured call-depth limit");

  const lang::NativeStackPolicy previous = lang::setNativeStackPolicyForTesting(
      {.headroom = std::size_t{1} << 40, .segment = std::size_t{4} << 20});
  const std::size_t segmentsBefore = lang::nativeStackSegmentsEntered();
  const auto segmented = run("segments.gti", valid);
  const auto segmentedInvalid = run("segments-invalid.gti", invalid);
  const std::size_t segments =
      lang::nativeStackSegmentsEntered() - segmentsBefore;
  lang::setNativeStackPolicyForTesting(previous);
  expect(segments > 200,
         "the forced policy switched segments at every boundary");
  expect(segmented.first == plain.first,
         "segment switches leave the produced MIR byte-identical");
  expect(segmented.second == plain.second &&
             segmentedInvalid.second == plainInvalid.second,
         "segment switches leave diagnostics byte-identical");

  // Errors raised on a segment reach the caller with their identity, through
  // nested segments.
  struct Raised {
    int value;
  };
  const auto forced = lang::setNativeStackPolicyForTesting(
      {.headroom = std::size_t{1} << 40, .segment = std::size_t{1} << 20});
  int caught = 0;
  try {
    lang::withSufficientStack([] {
      return lang::withSufficientStack([]() -> int { throw Raised{7}; });
    });
  } catch (const Raised &raised) {
    caught = raised.value;
  }
  lang::setNativeStackPolicyForTesting(forced);
  expect(caught == 7, "an exception thrown on a nested segment propagates");
}

const lang::ClassDecl *findTopLevelClass(const lang::Program &program,
                                         const std::string &name) {
  for (const lang::StmtPtr &declaration : program.declarations()) {
    const auto *classDecl =
        dynamic_cast<const lang::ClassDecl *>(declaration.get());
    if (classDecl != nullptr && classDecl->name().lexeme == name) {
      return classDecl;
    }
  }
  return nullptr;
}

void testBorrowedCleanupQueries() {
  // The prelude declares the result class, so the query program loads it.
  const lang::FrontendResult cleanupQuery = lang::Frontend().analyze(
      "cleanup-query.gti", R"(
class Cleanup {
public:
  Cleanup() {}
  ~Cleanup() {}
};
class Box<T> {
public:
  T nested;
};
class Expand<T> {
public:
  expected<Expand<Expand<T>>, int> nested = unexpected(0);
public:
  Expand() = default;
};
class Branch<T> {
public:
  T left;
  T right;
};
int main() { return 0; }
)",
      {standardLibraryPrelude()}, {}, {standardLibraryRoot()});
  const lang::Program &cleanupQueryProgram = cleanupQuery.program;
  lang::SemanticVisitor cleanupQuerySemantic(lang::TargetInfo::host(),
                                             &cleanupQuery.sourceGraph, false,
                                             nullptr, &cleanupQuery.sources);
  (void)cleanupQuerySemantic.check(cleanupQueryProgram);
  const lang::ClassDecl *cleanupClass =
      findTopLevelClass(cleanupQueryProgram, "Cleanup");
  const lang::ClassDecl *boxClass =
      findTopLevelClass(cleanupQueryProgram, "Box");
  const lang::ClassDecl *expandClass =
      findTopLevelClass(cleanupQueryProgram, "Expand");
  const lang::ClassDecl *branchClass =
      findTopLevelClass(cleanupQueryProgram, "Branch");
  const lang::ClassTypeInfo *cleanupInfo =
      cleanupClass == nullptr
          ? nullptr
          : cleanupQuerySemantic.model().findClassType(*cleanupClass);
  const lang::ClassTypeInfo *boxInfo =
      boxClass == nullptr
          ? nullptr
          : cleanupQuerySemantic.model().findClassType(*boxClass);
  const lang::ClassTypeInfo *expandInfo =
      expandClass == nullptr
          ? nullptr
          : cleanupQuerySemantic.model().findClassType(*expandClass);
  const lang::ClassTypeInfo *branchInfo =
      branchClass == nullptr
          ? nullptr
          : cleanupQuerySemantic.model().findClassType(*branchClass);
  lang::SemanticType nestedCleanup = lang::SemanticType::Unknown;
  lang::SemanticType expandingRecurrence = lang::SemanticType::Unknown;
  lang::SemanticType branchingCleanupFree = lang::SemanticType::Int32;
  if (cleanupInfo != nullptr && boxInfo != nullptr) {
    const lang::SemanticType cleanup =
        lang::SemanticType::classType(cleanupInfo->id);
    nestedCleanup = lang::SemanticType::classType(
        boxInfo->id, {lang::SemanticType::classType(boxInfo->id, {cleanup})});
  }
  if (expandInfo != nullptr) {
    expandingRecurrence = lang::SemanticType::classType(
        expandInfo->id, {lang::SemanticType::Int32});
  }
  if (branchInfo != nullptr) {
    for (std::size_t depth = 0; depth < 18; ++depth) {
      branchingCleanupFree = lang::SemanticType::classType(
          branchInfo->id, {std::move(branchingCleanupFree)});
    }
  }
  expect(
      cleanupQuery.syntaxValid && cleanupInfo != nullptr &&
          boxInfo != nullptr && expandInfo != nullptr &&
          branchInfo != nullptr &&
          cleanupQuerySemantic.requiresActiveCleanupFor(nestedCleanup) &&
          cleanupQuerySemantic.requiresActiveCleanupFor(expandingRecurrence) &&
          !cleanupQuerySemantic.requiresActiveCleanupFor(branchingCleanupFree),
      "active-cleanup queries should preserve decreasing concrete generic "
      "detection, terminate non-decreasing recurrence, and memoize repeated "
      "cleanup-free generic branches");

  expect(
      cleanupQuerySemantic.lifecycleBoundBorrowCarrierFor(
          expandingRecurrence) &&
          !cleanupQuerySemantic.lifecycleBoundBorrowCarrierFor(nestedCleanup) &&
          !cleanupQuerySemantic.lifecycleBoundBorrowCarrierFor(
              branchingCleanupFree),
      "borrowed-content lifecycle queries terminate expanding generic "
      "recurrence, preserve decreasing nests, and memoize repeated branches");
}

} // namespace

GTI_UNIT_TEST(constant_evaluation_api) {
  testBinaryFloatApi();
  testStagedIntrinsicContracts();
  testMirConstantEvaluation();
  testMirConstantRecords();
  testConstantEvaluationReuse();
  testNativeStackSegmentsAreInvisible();
  testBorrowedCleanupQueries();
  return failures == 0 ? 0 : 1;
}
