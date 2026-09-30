#include "cpp_backend_test_support.h"
#include "gti/frontend.h"
#include "gti/mir_verifier.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
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
             std::filesystem::weakly_canonical("transform_error.gti").string(),
         "diagnostic retains exact source identity");
  expect(found->phase == lang::DiagnosticPhase::Semantics &&
             found->severity == lang::DiagnosticSeverity::Error,
         "transform diagnostic is a semantic error");
  expect(std::any_of(found->hints.begin(), found->hints.end(),
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
  reject(producer + "int map(int e) { return e; }\n"
                    "int main() { auto result = read();\n"
                    "  [[discard]] result.transform_error(map); return 0; }\n",
         "GTI-S2018", "transform_error", "std::move");
  for (const std::string declaration :
       {"int map(bool e) { return 1; }\n", "int map(int& e) { return e; }\n",
        "int map<T>(T e) { return 1; }\n",
        "[[unsafe]] int map(int e) { return e; }\n", "int map(int e);\n"}) {
    reject(producer + declaration +
               "int main() { /* 😀 */ [[discard]] read().transform_error(map); "
               "return 0; }\n",
           "GTI-S2097", "map", "non-generic namespace function", true);
  }
  for (const std::string declaration :
       {"void map(int e) {}\n",
        "int global = 1; int& map(int e) { return global; }\n"}) {
    reject(producer + declaration +
               "int main() { [[discard]] read().transform_error(map); "
               "return 0; }\n",
           "GTI-S2097", "map", "movable owned");
  }
  reject(producer + "int map(int e) { return e; }\n"
                    "bool map(int e) { return true; }\n"
                    "int main() { [[discard]] read().transform_error(map); "
                    "return 0; }\n",
         "GTI-S2097", "map", "uniquely named", true);
  reject("int map(int e) { return e; }\n"
         "expected<int&, int> read(int& value) { return value; }\n"
         "int main() { int value = 1; [[discard]] "
         "read(value).transform_error(map); return 0; }\n",
         "GTI-S2097", "transform_error", "Fallible borrows");
  const auto arity =
      analyze(producer + "int main() { [[discard]] "
                         "read().transform_error(); return 0; }\n");
  expect(!arity.semanticValid &&
             std::count_if(arity.diagnostics.begin(), arity.diagnostics.end(),
                           [](const auto &diagnostic) {
                             return diagnostic.severity ==
                                    lang::DiagnosticSeverity::Error;
                           }) == 1 &&
             std::any_of(arity.diagnostics.begin(), arity.diagnostics.end(),
                         [](const auto &diagnostic) {
                           return diagnostic.code == "GTI-S2097";
                         }),
         "wrong mapper arity has one owning error rather than cascades");
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
  unsigned checked = 0;
  unsigned mutations = 0;
  for (const auto &function : result.mir.functionInstances()) {
    const auto *hir = result.hir.findFunctionInstance(function.id);
    if (!hir || !hir->source)
      continue;
    const auto &name = hir->source->name().lexeme;
    if (name != "transform" && name != "propagate" && name != "transform_void")
      continue;
    ++checked;
    unsigned mapperCalls = 0;
    bool hasStateTest = false;
    bool hasErrorWrapper = false;
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        hasStateTest |=
            instruction.intrinsic == lang::IntrinsicKind::ExpectedHasValue ||
            instruction.operation == lang::MirOperation::ExpectedHasValue;
        hasErrorWrapper |=
            instruction.operation == lang::MirOperation::Unexpected;
        if (instruction.kind != lang::MirInstructionKind::Call ||
            !instruction.functionTarget)
          continue;
        const auto *target =
            result.hir.findFunctionInstance(*instruction.functionTarget);
        if (!target || !target->source ||
            target->source->name().lexeme != "map")
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
  unsigned temporaryReturns = 0;
  unsigned errorReturns = 0;
  for (const auto &function : result.mir.functionInstances()) {
    const auto *hir = result.hir.findFunctionInstance(function.id);
    if (!hir || !hir->source ||
        (hir->source->name().lexeme != "returned" &&
         hir->source->name().lexeme != "propagated"))
      continue;
    for (const auto &drop : function.body.dropObligations) {
      if (drop.kind != lang::MirDropObligationKind::ReturnResult)
        continue;
      const auto *place = function.body.findPlace(drop.place);
      if (!place)
        continue;
      if (drop.dropType.type.kind == lang::SemanticType::Unexpected)
        ++errorReturns;
      if (place->root != lang::MirPlaceRootKind::Temporary)
        continue;
      ++temporaryReturns;
      for (unsigned mutation = 0; mutation < 4; ++mutation) {
        auto malformed = result.mir;
        auto *changed = const_cast<lang::MirFunctionInstance *>(
            malformed.findFunctionInstance(function.id));
        auto &body = changed->body;
        auto &returned = body.dropObligations[drop.id - 1];
        if (mutation == 0) {
          for (auto &block : body.blocks)
            for (auto &instruction : block.instructions)
              if (instruction.kind == lang::MirInstructionKind::Move &&
                  instruction.hirValue == drop.value &&
                  instruction.operands.size() == 1 &&
                  instruction.operands.front().place == drop.place)
                instruction.operands.front().kind = lang::MirOperandKind::Copy;
        } else if (mutation == 1) {
          returned.place = 0;
        } else if (mutation == 2) {
          returned.dropType.type = lang::SemanticType::Int32;
        } else {
          for (auto &block : body.blocks)
            if (block.terminator.kind == lang::MirTerminatorKind::Return &&
                block.terminator.value)
              block.terminator.value->value = 0;
        }
        expect(!lang::verifyMirProgram(malformed).valid(),
               "return transport rejects wrong move, place, type, or identity");
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
  expect(temporaryReturns == 2 && errorReturns == 1,
         "direct and propagated owning returns retain unpublished cleanup");
  unsigned publications = 0;
  for (const auto &function : result.mir.functionInstances()) {
    const auto *hir = result.hir.findFunctionInstance(function.id);
    if (!hir || !hir->source || hir->source->name().lexeme != "returned")
      continue;
    for (const auto &block : function.body.blocks) {
      for (const auto &instruction : block.instructions) {
        const auto *place =
            instruction.destination
                ? function.body.findPlace(*instruction.destination)
                : nullptr;
        if (instruction.kind != lang::MirInstructionKind::Initialize ||
            !place || place->root != lang::MirPlaceRootKind::Temporary ||
            place->type.kind != lang::SemanticType::Expected)
          continue;
        ++publications;
        for (unsigned mutation = 0; mutation < 2; ++mutation) {
          auto malformed = result.mir;
          auto *changed = const_cast<lang::MirFunctionInstance *>(
              malformed.findFunctionInstance(function.id));
          if (mutation == 0) {
            changed->body.places[place->id - 1].type =
                lang::SemanticType::Int32;
          } else {
            for (auto &candidateBlock : changed->body.blocks)
              for (auto &candidate : candidateBlock.instructions)
                if (candidate.id == instruction.id)
                  candidate.lifecycle.clear();
          }
          expect(!lang::verifyMirProgram(malformed).valid(),
                 "conditional publication rejects wrong slot type or erased "
                 "handoff");
          bool rejected = false;
          try {
            (void)gti_test::emitCpp(result, result.mir, malformed);
          } catch (const std::exception &) {
            rejected = true;
          }
          expect(rejected, "malformed conditional publication cannot emit C++");
        }
      }
    }
  }
  expect(publications == 2,
         "both conditional alternative publications mutated");
}
} // namespace

int main() {
  testDiagnostics();
  testPipeline();
  testOwningPublication();
  return failures == 0 ? 0 : 1;
}
