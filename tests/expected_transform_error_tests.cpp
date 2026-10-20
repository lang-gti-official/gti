#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <initializer_list>
#include <tuple>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

lang::FrontendResult analyze(const std::string &source) {
  const auto library =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "stdlib";
  return lang::Frontend().analyze("transform_error.gti", source,
                                  {library / "prelude.gti"}, {}, {library});
}

void reject(const std::string &source, std::string_view code,
            std::string_view anchor, std::string_view hint,
            bool related = false) {
  const auto result = analyze(source);
  expect(!result.semanticValid && !result.canGenerateCode(),
         "invalid transform must fail in semantics before code generation");
  const auto found = std::find_if(
      result.diagnostics.begin(), result.diagnostics.end(),
      [&](const auto &diagnostic) { return diagnostic.code == code; });
  if (found == result.diagnostics.end()) {
    std::cerr << "Missing " << code << " for:\n" << source;
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    expect(false, "expected owning diagnostic");
    return;
  }
  const auto offset = source.rfind(anchor);
  expect(offset != std::string::npos && found->primary.start == offset &&
             found->primary.end == offset + anchor.size(),
         "diagnostic identifies exactly the offending mapper/member token");
  expect(found->primary.source ==
             std::filesystem::weakly_canonical(
                 std::filesystem::absolute("transform_error.gti"))
                 .string(),
         "diagnostic retains exact source identity");
  expect(found->phase == lang::DiagnosticPhase::Semantics &&
             found->severity == lang::DiagnosticSeverity::Error,
         "transform diagnostic is a semantic error");
  expect(found->message.find(hint) != std::string::npos ||
             std::any_of(found->hints.begin(), found->hints.end(),
                         [&](const auto &text) {
                           return text.find(hint) != std::string::npos;
                         }),
         "diagnostic explains the applicable correction");
  expect(found->fixes.empty(), "ownership and mapper intent have no safe fix");
  expect(!related || !found->related.empty(),
         "rejected mapper lookup retains candidate declarations");
  expect(std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                      [](const auto &diagnostic) {
                        return diagnostic.code.starts_with("GTI-B");
                      }),
         "invalid source must not escape into backend diagnostics");
}

void testDiagnostics() {
  const std::string producer =
      "expected<int, int> read() { return unexpected(1); }\n";
  // transform_error is an ordinary consuming source member: the general
  // consuming-receiver diagnostic anchors the call's closing parenthesis.
  reject(producer + "int map(int e) { return e; }\n"
                    "int main() { auto result = read();\n"
                    "  [[discard]] result.transform_error(map); return 0; }\n",
         "GTI-S2022", ")", "std::move");
  // A function-item mapper is checked by the general function-item contract.
  for (const std::string declaration :
       {"int map(bool e) { return 1; }\n", "int map(int& e) { return e; }\n",
        "int map<T>(T e) { return 1; }\n",
        "unsafe int map(int e) { return e; }\n", "int map(int e);\n"}) {
    reject(producer + declaration +
               "int main() { /* 😀 */ [[discard]] read().transform_error(map); "
               "return 0; }\n",
           "GTI-S2090", "map", "non-generic namespace-scope GTI function",
           true);
  }
  // The mapped error must be movable: the member's own requirement rejects a
  // void or reference result at the call.
  for (const std::string declaration :
       {"void map(int e) {}\n",
        "int global = 1; int& map(int e) { return global; }\n"}) {
    reject(producer + declaration +
               "int main() { [[discard]] read().transform_error(map); "
               "return 0; }\n",
           "GTI-S2029", ")", "move construction");
  }
  reject(producer + "int map(int e) { return e; }\n"
                    "bool map(int e) { return true; }\n"
                    "int main() { [[discard]] read().transform_error(map); "
                    "return 0; }\n",
         "GTI-S2090", "map", "Use one non-generic namespace-scope GTI function",
         true);
  // The reference-success profile offers no transform_error member.
  reject("int map(int e) { return e; }\n"
         "expected<int&, int> read(int& value) { return value; }\n"
         "int main() { int value = 1; [[discard]] "
         "read(value).transform_error(map); return 0; }\n",
         "GTI-S2000", "transform_error", "Unknown member");
  const auto arity =
      analyze(producer + "int main() { [[discard]] "
                         "read().transform_error(); return 0; }\n");
  // The general generic-call arity error owns a missing mapper; the only
  // other error is the mapper's then-uninferable type parameter.
  expect(!arity.semanticValid &&
             std::count_if(arity.diagnostics.begin(), arity.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.severity ==
                                    lang::DiagnosticSeverity::Error;
                           }) <= 2 &&
             std::any_of(arity.diagnostics.begin(), arity.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2005";
                         }),
         "wrong mapper arity reports the call's arity rather than cascades");
  // A lambda mapper is invoked inside the member's instance; its errors are
  // anchored at the requesting member call and noted inside the library.
  for (const auto &[lambda, code, explanation] :
       std::initializer_list<
           std::tuple<std::string, std::string_view, std::string_view>>{
           {"[](bool error) -> int { return 1; }", "GTI-S2003",
            "requires 'bool'"},
           {"[](int& error) -> int { return error; }", "GTI-S2017",
            "References"},
           {"[]() -> int { return 1; }", "GTI-S2005", "expects 0 arguments"},
           {"[](int first, int second) -> int { return first; }", "GTI-S2005",
            "expects 2 arguments"}}) {
    reject(
        producer + "int main() { auto map = " + lambda +
            ";\n"
            "  /* 😀 */ [[discard]] read().transform_error(map); return 0; }\n",
        code, "transform_error", explanation, true);
  }
  reject(producer + "int main() { auto map = [](int error) -> void {};\n"
                    "  [[discard]] read().transform_error(map); return 0; }\n",
         "GTI-S2029", ")", "move construction");
  const auto movedCapture = analyze(producer + R"(
struct Owner { ~Owner() {} };
int main() {
  Owner owner{};
  auto result = read().transform_error(
      [captured = std::move(owner)](int error) -> int { return error; });
  auto invalid = [again = std::move(owner)]() -> int { return 1; };
  return result.error() + invalid();
}
)");
  expect(!movedCapture.canGenerateCode() &&
             std::any_of(movedCapture.diagnostics.begin(),
                         movedCapture.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.message.find(
                                      "already been moved") !=
                                  std::string::npos;
                         }),
         "mapper move captures retain ordinary source-availability checks");
}

void testPipeline() {
  const std::string source = R"(
enum class Error { failed };
namespace app {
  Error map(int error) { return Error::failed; }
  bool map(bool error) { return error; }
}
expected<int, int> read(bool fail) {
  if (fail) { return unexpected(1); }
  return 42;
}
expected<int, Error> transform(bool fail) {
  return read(fail).transform_error(app::map);
}
expected<int, Error> propagate(bool fail) {
  auto source = read(fail);
  return ?std::move(source).transform_error(app::map);
}
expected<void, int> perform(bool fail) {
  if (fail) { return unexpected(1); }
  return;
}
expected<void, Error> transform_void(bool fail) {
  ?perform(fail).transform_error(app::map);
  return;
}
expected<T, Error> generic_transform<T>(expected<T, int> source) {
  return std::move(source).transform_error(app::map);
}
int main() {
  auto result = generic_transform<int>(read(false));
  return result.value() - 42;
}
)";
  const auto result = analyze(source);
  for (const auto &diagnostic : result.diagnostics)
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  expect(result.canGenerateCode(),
         "owned scalar/void transforms and ? compose");
  if (!result.canGenerateCode())
    return;
  expect(lang::verifyMirProgram(result.mir).valid(),
         "transform pipeline produces verified MIR");
  const auto callee = [&](const lang::MirInstruction &instruction) {
    return instruction.kind == lang::MirInstructionKind::Call &&
                   instruction.functionTarget
               ? result.hir.findFunctionInstance(*instruction.functionTarget)
               : nullptr;
  };
  const auto named = [](const lang::HirFunctionInstance *function,
                        std::string_view name) {
    return function != nullptr && function->source != nullptr &&
           function->source->name().lexeme == name;
  };
  unsigned checked = 0;
  unsigned mutations = 0;
  for (const auto &caller : result.mir.functionInstances()) {
    const auto *hir = result.hir.findFunctionInstance(caller.id);
    if (!hir || !hir->source)
      continue;
    const auto &name = hir->source->name().lexeme;
    if (name != "transform" && name != "propagate" && name != "transform_void")
      continue;
    ++checked;
    // transform_error is an ordinary prelude member: its state branch, mapper
    // call and error wrapper are lowered in the selected instance's body.
    const lang::MirFunctionInstance *transform = nullptr;
    for (const auto &block : caller.body.blocks)
      for (const auto &instruction : block.instructions)
        if (named(callee(instruction), "transform_error"))
          transform =
              result.mir.findFunctionInstance(*instruction.functionTarget);
    expect(transform != nullptr, "each caller selects transform_error");
    if (transform == nullptr)
      continue;
    const auto &function = *transform;
    unsigned mapperCalls = 0;
    bool hasStateTest = false;
    bool hasErrorWrapper = false;
    for (const auto &block : function.body.blocks) {
      hasStateTest |= block.terminator.kind == lang::MirTerminatorKind::Switch;
      for (const auto &instruction : block.instructions) {
        hasErrorWrapper |= named(callee(instruction), "unexpected");
        const auto *target = callee(instruction);
        if (!named(target, "map"))
          continue;
        ++mapperCalls;
        expect(block.id != function.body.entry,
               "mapper call is conditional rather than in the entry block");
        expect(target->parameterTypes.size() == 1 &&
                   target->parameterTypes.front() == lang::SemanticType::Int32,
               "exact error parameter selects the namespace overload");
        auto malformed = result.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        for (auto &changedBlock : changed->body.blocks)
          for (auto &candidate : changedBlock.instructions)
            if (candidate.id == instruction.id)
              candidate.functionTarget = function.id;
        expect(!lang::verifyMirProgram(malformed).valid(),
               "MIR rejects retargeting the mapper to a different signature");
        ++mutations;
      }
    }
    expect(mapperCalls == 1 && hasStateTest && hasErrorWrapper,
           "each transform lowers one state branch and one error mapper call");
  }
  expect(checked == 3 && mutations == 3,
         "scalar, propagated, and void mapper pipelines checked independently");
}

void testLambdaMappers() {
  const auto result = analyze(R"(
enum class Error { bad = 7 };
expected<int, Error> read(bool fail) {
  if (fail) { return unexpected(Error::bad); }
  return 42;
}
expected<int, int> mapped(bool fail) {
  return read(fail).transform_error(([](Error error) -> int { return error == Error::bad ? 7 : 0; }));
}
int main() {
  int offset = 3;
  auto mapper = [offset](Error error) -> int { return (error == Error::bad ? 7 : 0) + offset; };
  auto first = read(true).transform_error(mapper);
  auto second = read(false).transform_error([owned = std::move(offset)](Error error) -> int {
    return (error == Error::bad ? 7 : 0) + owned;
  });
  return first.error() + second.value() - 52;
}
)");
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
  expect(result.canGenerateCode(),
         "literal and named capturing lambda mappers lower");
  if (!result.canGenerateCode())
    return;
  expect(lang::verifyMirProgram(result.mir).valid(),
         "lambda mapper MIR verifies");
  expect(lang::verifyHirProgramPlans(result.semantics, result.hir).valid(),
         "eager lambda inputs retain the selected source order");
  // A lambda mapper is an ordinary argument of the source member, staged
  // before the call through its exact MIR call input.
  unsigned eagerPlans = 0;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *target =
            instruction.kind == lang::MirInstructionKind::Call &&
                    instruction.functionTarget
                ? result.hir.findFunctionInstance(*instruction.functionTarget)
                : nullptr;
        const auto &mapperType = instruction.operands.empty()
                                     ? lang::SemanticType::Unknown
                                     : instruction.operands.front().type;
        if (!target || !target->source ||
            target->source->name().lexeme != "transform_error" ||
            instruction.operands.size() != 1 ||
            mapperType.kind != lang::SemanticType::Reference ||
            mapperType.arguments.size() != 1 ||
            mapperType.arguments.front().kind != lang::SemanticType::Lambda)
          continue;
        const lang::MirInstruction *stage = nullptr;
        for (const auto &candidateBlock : function.body.blocks)
          for (const auto &candidate : candidateBlock.instructions)
            if (candidate.kind == lang::MirInstructionKind::CallInput &&
                candidate.result &&
                *candidate.result == instruction.operands.front().value)
              stage = &candidate;
        if (!stage)
          continue;
        ++eagerPlans;
        for (unsigned mutation = 0; mutation < 2; ++mutation) {
          auto malformed = result.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          for (auto &changedBlock : changed->body.blocks)
            for (auto &candidate : changedBlock.instructions) {
              if (mutation == 0 && candidate.id == stage->id)
                candidate.callInputIndex = 1;
              if (mutation == 1 && candidate.id == instruction.id)
                candidate.operands.clear();
            }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "MIR rejects a misplaced or detached lambda mapper input");
        }
      }
    }
  }
  expect(eagerPlans == 3,
         "all three lambda mappings have verified eager plans");
  unsigned calls = 0;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (!instruction.lambdaTarget ||
            instruction.kind != lang::MirInstructionKind::Call)
          continue;
        ++calls;
        for (unsigned mutation = 0; mutation < 4; ++mutation) {
          auto malformed = result.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          for (auto &changedBlock : changed->body.blocks)
            for (auto &candidate : changedBlock.instructions)
              if (candidate.id == instruction.id) {
                if (mutation == 0)
                  candidate.lambdaTarget = 999999;
                else if (mutation == 1)
                  candidate.receiver.reset();
                else if (mutation == 2)
                  candidate.parameterTypes = {lang::SemanticType::Bool};
                else
                  candidate.definedFailure.propagation =
                      lang::FailurePropagationKind::DirectCall;
              }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "lambda mapping rejects forged target, environment and "
                 "signature or callable failure propagation");
        }
      }
    }
  }
  expect(calls == 3, "all three mappers use exact lambda calls");
  for (auto standard : {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    try {
      expect(!gti_test::emitCppText(result, standard).empty(),
             "lambda mapping emits through the production MIR backend");
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      expect(false, "lambda mapper production emission");
    }
  }
}

void testOwnedLambdaEnvironment() {
  const auto result = analyze(R"(
class Resource {
  int value;
public:
  Resource(int value) : value(value) {}
  ~Resource() { std::println(value); }
  int read() { return value; }
};
expected<int, int> read(bool fail) {
  if (fail) { return unexpected(7); }
  return 42;
}
int main() {
  auto resource = Resource(8);
  auto result = read(true).transform_error(
      [owned = std::move(resource)](int error) -> int {
        return owned.read() + error;
      });
  return result.error() - 15;
}
)");
  expect(result.canGenerateCode(), "owning mapper environment lowers");
  if (!result.canGenerateCode())
    return;
  unsigned closures = 0;
  unsigned stages = 0;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const bool closure =
            instruction.operation == lang::MirOperation::Closure;
        const bool stage =
            instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.info.type.kind == lang::SemanticType::Lambda;
        if (!closure && !stage)
          continue;
        closures += closure;
        stages += stage;
        for (unsigned mutation = 0; mutation < 2; ++mutation) {
          auto malformed = result.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          for (auto &changedBlock : changed->body.blocks)
            for (auto &candidate : changedBlock.instructions)
              if (candidate.id == instruction.id) {
                if (closure) {
                  if (mutation == 0)
                    candidate.lifecycle.clear();
                  else
                    candidate.operands.clear();
                } else {
                  if (mutation == 0)
                    candidate.callSite = 0;
                  else
                    candidate.callInputRole = lang::MirCallInputRole::Argument;
                }
              }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "owned closure rejects erased capture/lifecycle or forged "
                 "receiver stage");
          bool rejected = false;
          try {
            (void)gti_test::emitCpp(result, result.mir, malformed);
          } catch (const std::exception &) {
            rejected = true;
          }
          expect(rejected,
                 "invalid owned closure cannot reach C++ publication");
        }
      }
    }
  }
  expect(closures == 1 && stages == 1,
         "one eager closure and one prepared receiver");
  try {
    expect(!gti_test::emitCppText(result).empty(),
           "owned closure slot emits from MIR");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    expect(false, "owned closure slot production emission");
  }
}

void testMovedLambdaEnvironment() {
  const std::string source = R"(
struct Resource { ~Resource() {} };
expected<int, int> read() { return unexpected(7); }
int main() {
  Resource resource{};
  auto mapper = [owned = std::move(resource)](int error) -> int {
    return error;
  };
  auto result = read().transform_error(std::move(mapper));
  return result.error() - 7;
}
)";
  const auto result = analyze(source);
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "an explicitly moved owning mapper retains a verified environment");
  if (!result.canGenerateCode())
    return;
  unsigned movedReceivers = 0;
  for (const auto &function : result.mir.functionInstances()) {
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        if (instruction.kind == lang::MirInstructionKind::CallInput &&
            instruction.info.type.kind == lang::SemanticType::Lambda &&
            instruction.callInputRole == lang::MirCallInputRole::Receiver) {
          ++movedReceivers;
          // transform_error takes its mapper as `F&`: the invocation borrows
          // the moved environment, whose full-expression owner keeps cleanup.
          expect(instruction.callInputKind ==
                         lang::HirCallInputKind::ReadBorrow &&
                     instruction.lifecycle.empty() &&
                     !instruction.preparedParameterDrop,
                 "moved receiver staging aliases its full-expression owner "
                 "without transferring cleanup into the invocation");
        }
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.lambdaTarget) {
          expect(instruction.callableInvocation ==
                     lang::CallableInvocationCapability::Read,
                 "moving the environment does not select consuming invocation");
        }
      }
    }
  }
  expect(movedReceivers == 1, "the mapper has one explicit moved receiver");
  for (auto standard : {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    try {
      expect(!gti_test::emitCppText(result, standard).empty(),
             "moved owning mapper emits through the production MIR backend");
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      expect(false, "moved owning mapper production emission");
    }
  }
  std::string invalid = source;
  invalid.insert(invalid.find("  return result.error()"),
                 "  [[discard]] mapper(1);\n");
  const auto reused = analyze(invalid);
  expect(!reused.canGenerateCode() &&
             std::any_of(reused.diagnostics.begin(), reused.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.phase ==
                                      lang::DiagnosticPhase::Semantics &&
                                  diagnostic.message.find(
                                      "already been moved") !=
                                      std::string::npos;
                         }),
         "a moved mapper cannot be reused even though its invocation was "
         "read-only");
}

void testOwningPublication() {
  const auto result = analyze(R"(
struct Payload { ~Payload() { std::println(30); } };
struct Error { ~Error() { std::println(10); } };
struct Mapped { int code = 4; ~Mapped() { std::println(20); } };
Mapped map(Error error) { return Mapped(); }
int again(Mapped error) { return 1; }
expected<Payload, Error> read(bool fail) {
  if (fail) { return unexpected(Error()); }
  return Payload();
}
expected<Payload, Mapped> returned(bool fail) {
  return read(fail).transform_error(map);
}
expected<Payload, Mapped> propagated(bool fail) {
  return ?read(fail).transform_error(map);
}
expected<void, Error> perform() { return unexpected(Error()); }
int observe_void() {
  auto direct = perform().transform_error(map);
  auto source = perform();
  auto named = std::move(source).transform_error(map);
  return direct.error().code + named.error().code;
}
int consume(expected<Payload, Mapped> result) {
  return result.has_value() ? 0 : result.error().code;
}
int pass_onward(bool fail) { return consume(read(fail).transform_error(map)); }
int main() {
  auto result = read(true).transform_error(map).transform_error(again);
  return result.has_value() ? 0 : 1;
}
)");
  if (!result.canGenerateCode()) {
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.message << '\n';
  }
  expect(result.canGenerateCode() && lang::verifyMirProgram(result.mir).valid(),
         "owning transform returns, propagation, and chained cleanup verify");
  if (!result.canGenerateCode())
    return;
  for (auto standard : {lang::CppStandard::Cpp20, lang::CppStandard::Cpp23}) {
    try {
      const auto text = gti_test::emitCppText(result, standard);
      expect(!text.empty(), "owning and void transforms emit production C++");
    } catch (const std::exception &error) {
      std::cerr << "Owning production emission: " << error.what() << '\n';
      expect(false, "owning transforms retain complete native representation");
    }
  }
  // The result is an ordinary class value: each returning path retains its
  // owning result until publication, including the residual of `?`.
  const auto resultClass = [&](const lang::SemanticType &type) {
    if (type.kind != lang::SemanticType::Class)
      return false;
    const auto *info = result.semantics.findClassType(type.classId);
    return info && info->qualifiedName == "expected";
  };
  unsigned returnedResults = 0;
  unsigned propagatedResults = 0;
  for (const auto &function : result.mir.functionInstances()) {
    const auto *hir = result.hir.findFunctionInstance(function.id);
    if (!hir || !hir->source ||
        (hir->source->name().lexeme != "returned" &&
         hir->source->name().lexeme != "propagated"))
      continue;
    const bool propagated = hir->source->name().lexeme == "propagated";
    for (const auto &drop : function.body.dropObligations) {
      if (drop.kind != lang::MirDropObligationKind::ReturnResult ||
          !resultClass(drop.dropType.type) ||
          function.body.findPlace(drop.place) == nullptr)
        continue;
      ++(propagated ? propagatedResults : returnedResults);
      for (unsigned mutation = 0; mutation < 3; ++mutation) {
        auto malformed = result.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &body = changed->body;
        auto &returned = body.dropObligations[drop.id - 1];
        if (mutation == 0) {
          returned.place = 0;
        } else if (mutation == 1) {
          returned.dropType.type = lang::SemanticType::Int32;
        } else {
          for (auto &block : body.blocks)
            if (block.terminator.kind == lang::MirTerminatorKind::Return &&
                block.terminator.value)
              block.terminator.value->value = 0;
        }
        expect(!lang::verifyMirProgram(malformed).valid(),
               "return transport rejects wrong place, type, or identity");
        bool rejected = false;
        try {
          (void)gti_test::emitCpp(result, result.mir, malformed);
        } catch (const std::exception &) {
          rejected = true;
        }
        expect(rejected,
               "malformed return transport cannot reach C++ publication");
      }
    }
  }
  expect(returnedResults == 1 && propagatedResults == 2,
         "direct and propagated owning returns retain unpublished cleanup");
  // transform_error publishes either alternative by constructing the result
  // in its own selected instance; each construction keeps its exact target
  // and success handoff.
  unsigned publications = 0;
  const lang::MirFunctionInstance *member = nullptr;
  for (const auto &function : result.mir.functionInstances()) {
    const auto *hir = result.hir.findFunctionInstance(function.id);
    if (!hir || !hir->source || hir->source->name().lexeme != "returned")
      continue;
    for (const auto &block : function.body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Call &&
            instruction.functionTarget)
          if (const auto *target =
                  result.hir.findFunctionInstance(*instruction.functionTarget);
              target && target->source &&
              target->source->name().lexeme == "transform_error")
            member = result.mir.findFunctionInstance(*instruction.functionTarget);
  }
  expect(member != nullptr, "returned selects its transform_error instance");
  if (member != nullptr) {
    std::vector<lang::MirInstructionId> constructs;
    for (const auto &block : member->body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.kind == lang::MirInstructionKind::Construct &&
            instruction.constructorTarget &&
            resultClass(instruction.info.type))
          constructs.push_back(instruction.id);
    publications = constructs.size();
    for (std::size_t index = 0; index < constructs.size(); ++index)
      for (unsigned mutation = 0; mutation < 2; ++mutation) {
        auto malformed = result.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(member->id));
        std::optional<lang::HirConstructorInstanceId> other;
        for (auto &candidateBlock : changed->body.blocks)
          for (auto &candidate : candidateBlock.instructions)
            if (candidate.id == constructs[(index + 1) % constructs.size()])
              other = candidate.constructorTarget;
        for (auto &candidateBlock : changed->body.blocks)
          for (auto &candidate : candidateBlock.instructions)
            if (candidate.id == constructs[index]) {
              if (mutation == 0)
                candidate.lifecycle.clear();
              else
                candidate.constructorTarget = other;
            }
        expect(!lang::verifyMirProgram(malformed).valid(),
               "conditional publication rejects an erased handoff or "
               "substituted alternative constructor");
        bool rejected = false;
        try {
          (void)gti_test::emitCpp(result, result.mir, malformed);
        } catch (const std::exception &) {
          rejected = true;
        }
        expect(rejected, "malformed conditional publication cannot emit C++");
      }
  }
  expect(publications == 2,
         "both conditional alternative publications mutated");
}
} // namespace

int main() {
  testDiagnostics();
  testPipeline();
  testLambdaMappers();
  testOwnedLambdaEnvironment();
  testMovedLambdaEnvironment();
  testOwningPublication();
  return failures == 0 ? 0 : 1;
}
